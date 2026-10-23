#include <optional>

#include <spdlog/spdlog.h>

#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/gateway/bus_feed.hpp>
#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/message.hpp>

namespace mda::gateway {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kHousekeeping = std::chrono::milliseconds(200);

FeedHealth health_of(messages::FeedState state) {
    switch (state) {
        case messages::FeedState::Connected:
            return FeedHealth::Connected;
        case messages::FeedState::AuthExpired:
            return FeedHealth::LoginRequired;
        case messages::FeedState::Connecting:
        case messages::FeedState::Disconnected:
        case messages::FeedState::Stale:
            break;
    }
    return FeedHealth::Disconnected;
}

/// Sleeps up to `duration`, waking early when stop is requested.
void sleep_unless_stopped(const std::stop_token& stop, std::chrono::milliseconds duration) {
    const auto until = Clock::now() + duration;
    while (!stop.stop_requested() && Clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

BusFeed::BusFeed(QuoteCache& cache, BusFeedOptions options)
    : cache_(&cache)
    , options_(std::move(options))
    , thread_([this](const std::stop_token& stop) { run(stop); }) {}

BusFeed::~BusFeed() {
    thread_.request_stop();
}

void BusFeed::run(const std::stop_token& stop) {
    bool reported_missing = false;
    while (!stop.stop_requested()) {
        auto reader = bus::Reader::attach(options_.bus_name, {.name = "mda-gateway"});
        if (!reader) {
            cache_->set_feed(FeedHealth::Unavailable);
            if (!reported_missing) {
                spdlog::warn("market data: bus '{}' not available ({}); waiting for mda-feedd",
                             options_.bus_name, bus::to_string(reader.error()));
                reported_missing = true;
            }
            sleep_unless_stopped(stop, std::chrono::seconds(1));
            continue;
        }
        spdlog::info("market data: reading bus '{}'", options_.bus_name);
        reported_missing = false;

        // Until a FeedStatus says otherwise, a live writer means "connecting"; the first
        // snapshot proves "connected" (status events are only sent on change).
        std::optional<FeedHealth> health;
        const auto raw_frame = messages::to_wire(messages::MessageType::RawFrame);
        bus::Backoff wait;
        auto next_housekeeping = Clock::now();
        while (!stop.stop_requested()) {
            const auto result = reader->poll([&](bus::MsgType type,
                                                 std::span<const std::byte> payload) {
                if (auto snapshot = messages::decode<messages::MarketSnapshot>(type, payload)) {
                    cache_->update(*snapshot);
                    if (options_.metrics != nullptr) {
                        options_.metrics->on_snapshot(*snapshot, core::Timestamp::now().ns());
                    }
                    if (health != FeedHealth::Connected) {
                        health = FeedHealth::Connected;
                        cache_->set_feed(*health);
                    }
                } else if (auto limits = messages::decode<messages::CircuitLimits>(type, payload)) {
                    cache_->update_limits(*limits);
                } else if (auto depth = messages::decode<messages::DepthSnapshot>(type, payload)) {
                    cache_->update_depth(*depth);
                } else if (auto status = messages::decode<messages::FeedStatus>(type, payload)) {
                    health = health_of(status->state);
                    cache_->set_feed(*health);
                    if (options_.metrics != nullptr) {
                        options_.metrics->on_feed_status(*status);
                    }
                } else if (options_.metrics != nullptr && type == raw_frame) {
                    options_.metrics->on_frame();
                }
            });
            if (result.status == bus::PollStatus::Empty) {
                wait.idle();
            } else {
                wait.reset();
                if (result.status == bus::PollStatus::Lapped) {
                    spdlog::warn("market data: gateway fell behind the bus; {} bytes skipped",
                                 result.lost_bytes);
                }
            }

            if (const auto now = Clock::now(); now >= next_housekeeping) {
                next_housekeeping = now + kHousekeeping;
                reader->heartbeat();
                const auto writer = reader->writer_status();
                const auto age = std::chrono::nanoseconds(
                    core::Timestamp::now().ns() - static_cast<std::int64_t>(writer.heartbeat_ns));
                const bool alive = writer.pid != 0 && age < options_.writer_timeout;
                if (!alive && health != FeedHealth::Unavailable) {
                    spdlog::warn("market data: mda-feedd stopped publishing");
                    health = FeedHealth::Unavailable;
                    cache_->set_feed(*health);
                } else if (alive && (!health || health == FeedHealth::Unavailable)) {
                    spdlog::info("market data: mda-feedd is publishing (writer pid {})",
                                 writer.pid);
                    health = FeedHealth::Disconnected;
                    cache_->set_feed(*health);
                }
            }
        }
    }
}

}  // namespace mda::gateway
