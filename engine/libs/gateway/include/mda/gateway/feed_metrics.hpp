#pragma once

// What the gateway's bus readers see, for the admin system monitor
// (docs/design/system-monitor.md §3, SM2): ticks, frames, bars and analytics counted, the
// feed's last status, and feed → gateway latency per snapshot. Updated on the bus reader
// threads with relaxed atomics only (no locks, no allocation); read by the collector.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <mda/bars/bar.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/market_snapshot.hpp>

namespace mda::gateway {

/// Counts of values in buckets of about ±12 % (four per power of two), so p50 / p99 need no
/// stored samples. `record` from any thread; `take` from one reader, which also resets.
class LatencyHistogram {
public:
    static constexpr std::size_t kBuckets = 16 + 4 * 44;

    struct Summary {
        std::uint64_t count = 0;
        std::uint64_t p50 = 0;  ///< bucket upper bounds, never above `max`
        std::uint64_t p99 = 0;
        std::uint64_t max = 0;
    };

    void record(std::uint64_t value) noexcept;
    [[nodiscard]] Summary take() noexcept;

    [[nodiscard]] static std::size_t index_of(std::uint64_t value) noexcept;
    [[nodiscard]] static std::uint64_t upper_of(std::size_t index) noexcept;

private:
    std::array<std::atomic<std::uint64_t>, kBuckets> buckets_{};
    std::atomic<std::uint64_t> max_{0};
};

class FeedMetrics {
public:
    struct Counters {
        std::uint64_t snapshots = 0;      ///< ticks (MarketSnapshot) since the gateway started
        std::uint64_t frames = 0;         ///< raw WebSocket frames
        std::uint64_t bars_closed = 0;    ///< closed minute bars on "an"
        std::uint64_t analytics = 0;      ///< option analytics messages
        std::int64_t last_tick_ns = 0;    ///< receive time of the newest snapshot
        std::int64_t last_bar_start = 0;  ///< newest closed bar's minute (Unix s)
        std::uint8_t feed_state = 0;      ///< messages::FeedState; 0 = no status seen
        std::uint32_t subscribed = 0;
        std::uint32_t reconnects = 0;  ///< since mda-feedd started
    };

    /// `now_ns`: the gateway's read time (system clock).
    void on_snapshot(const messages::MarketSnapshot& snapshot, std::int64_t now_ns) noexcept;
    void on_frame() noexcept { frames_.fetch_add(1, std::memory_order_relaxed); }
    void on_feed_status(const messages::FeedStatus& status) noexcept;
    void on_bar(const bars::Bar& bar) noexcept;
    void on_analytics() noexcept { analytics_.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] Counters counters() const noexcept;
    /// feed → gateway, µs, since the last call.
    [[nodiscard]] LatencyHistogram::Summary take_latency() noexcept { return latency_us_.take(); }
    /// exchange → feed, whole seconds (Kite's exchange time is per second), since the last call.
    [[nodiscard]] LatencyHistogram::Summary take_exchange_lag() noexcept {
        return exchange_s_.take();
    }

private:
    std::atomic<std::uint64_t> snapshots_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> bars_closed_{0};
    std::atomic<std::uint64_t> analytics_{0};
    std::atomic<std::int64_t> last_tick_ns_{0};
    std::atomic<std::int64_t> last_bar_start_{0};
    std::atomic<std::uint8_t> feed_state_{0};
    std::atomic<std::uint32_t> subscribed_{0};
    std::atomic<std::uint32_t> reconnects_{0};
    LatencyHistogram latency_us_;
    LatencyHistogram exchange_s_;
};

}  // namespace mda::gateway
