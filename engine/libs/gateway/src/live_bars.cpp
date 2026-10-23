#include <algorithm>
#include <chrono>

#include <spdlog/spdlog.h>

#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/gateway/chain_api.hpp>
#include <mda/gateway/live_bars.hpp>

namespace mda::gateway {

void LiveBars::update(const bars::Bar& bar) {
    const std::uint32_t id = bar.header.instrument_id.value();
    if (id == 0) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    Entry& entry = entries_[id];
    auto& recent = entry.recent;
    const auto at =
        std::ranges::find_if(recent, [&](const bars::Bar& b) { return b.start == bar.start; });
    if (at != recent.end()) {
        *at = bar;
    } else {
        recent.push_back(bar);
        std::ranges::sort(recent, {}, &bars::Bar::start);
        if (recent.size() > 2) {
            recent.erase(recent.begin());
        }
    }
    entry.version = ++version_;
}

std::vector<bars::Bar> LiveBars::changed(std::span<const std::uint32_t> ids,
                                         SeenVersions& seen) const {
    std::vector<bars::Bar> out;
    const std::scoped_lock lock(mutex_);
    for (const std::uint32_t id : ids) {
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            continue;
        }
        std::uint64_t& sent = seen[id];
        if (found->second.version > sent) {
            out.insert(out.end(), found->second.recent.begin(), found->second.recent.end());
            sent = found->second.version;
        }
    }
    return out;
}

AnalyticsFeed::AnalyticsFeed(LiveBars& bars, QuoteCache& quotes, ChainSummaries& chains,
                             std::string bus_name, FeedMetrics* metrics)
    : bars_(&bars)
    , quotes_(&quotes)
    , chains_(&chains)
    , bus_name_(std::move(bus_name))
    , metrics_(metrics)
    , thread_([this](const std::stop_token& stop) { run(stop); }) {}

AnalyticsFeed::~AnalyticsFeed() {
    thread_.request_stop();
}

void AnalyticsFeed::run(const std::stop_token& stop) {
    bool reported = false;
    while (!stop.stop_requested()) {
        auto reader = bus::Reader::attach(bus_name_, {.name = "mda-gateway-an"});
        if (!reader) {
            if (!reported) {
                spdlog::warn("analytics: bus '{}' not available ({}); waiting for mda-engine",
                             bus_name_, bus::to_string(reader.error()));
                reported = true;
            }
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }
        spdlog::info("analytics: reading bus '{}'", bus_name_);
        reported = false;
        bus::Backoff wait;
        auto next_heartbeat = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            const auto result =
                reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
                    if (auto bar = messages::decode<bars::Bar>(type, payload)) {
                        bars_->update(*bar);
                        if (metrics_ != nullptr) {
                            metrics_->on_bar(*bar);
                        }
                    } else if (auto analytics = messages::decode<options::OptionAnalyticsMessage>(
                                   type, payload)) {
                        quotes_->update_analytics(*analytics);
                        if (metrics_ != nullptr) {
                            metrics_->on_analytics();
                        }
                    } else if (auto summary =
                                   messages::decode<options::ChainSummaryMessage>(type, payload)) {
                        chains_->update(*summary);
                    }
                });
            if (result.status == bus::PollStatus::Empty) {
                wait.idle();
            } else {
                wait.reset();
            }
            if (const auto now = std::chrono::steady_clock::now(); now >= next_heartbeat) {
                next_heartbeat = now + std::chrono::seconds(1);
                reader->heartbeat();
            }
        }
    }
}

}  // namespace mda::gateway
