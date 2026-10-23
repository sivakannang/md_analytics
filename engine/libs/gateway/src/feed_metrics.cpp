#include <algorithm>
#include <bit>

#include <mda/gateway/feed_metrics.hpp>

namespace mda::gateway {

std::size_t LatencyHistogram::index_of(std::uint64_t value) noexcept {
    if (value < 16) {
        return static_cast<std::size_t>(value);
    }
    // 2^e ≤ value < 2^(e+1), split in four by the two bits after the leading one.
    const auto e = static_cast<std::size_t>(std::bit_width(value) - 1);
    const auto sub = static_cast<std::size_t>((value >> (e - 2)) & 3u);
    return std::min(kBuckets - 1, 16 + (e - 4) * 4 + sub);
}

std::uint64_t LatencyHistogram::upper_of(std::size_t index) noexcept {
    if (index < 16) {
        return index;
    }
    const std::size_t k = index - 16;
    const std::size_t e = k / 4 + 4;
    const std::uint64_t quarter = std::uint64_t{1} << (e - 2);
    return (std::uint64_t{1} << e) + (k % 4) * quarter + quarter - 1;
}

void LatencyHistogram::record(std::uint64_t value) noexcept {
    buckets_[index_of(value)].fetch_add(1, std::memory_order_relaxed);
    std::uint64_t seen = max_.load(std::memory_order_relaxed);
    while (value > seen && !max_.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

LatencyHistogram::Summary LatencyHistogram::take() noexcept {
    std::array<std::uint64_t, kBuckets> counts{};
    Summary out;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        counts[i] = buckets_[i].exchange(0, std::memory_order_relaxed);
        out.count += counts[i];
    }
    out.max = max_.exchange(0, std::memory_order_relaxed);
    if (out.count == 0) {
        return out;
    }
    const auto percentile = [&](std::uint64_t per_thousand) {
        const std::uint64_t rank = (out.count * per_thousand + 999) / 1000;  // ceil
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts[i];
            if (seen >= rank) {
                return std::min(upper_of(i), out.max);
            }
        }
        return out.max;
    };
    out.p50 = percentile(500);
    out.p99 = percentile(990);
    return out;
}

void FeedMetrics::on_snapshot(const messages::MarketSnapshot& snapshot,
                              std::int64_t now_ns) noexcept {
    snapshots_.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t received = snapshot.receive_ts.ns();
    last_tick_ns_.store(received, std::memory_order_relaxed);
    if (now_ns >= received) {
        latency_us_.record(static_cast<std::uint64_t>(now_ns - received) / 1000);
    }
    if ((snapshot.fields & messages::kFieldExchangeTime) != 0 && snapshot.exchange_ts.ns() > 0) {
        const std::int64_t lag_s = (received - snapshot.exchange_ts.ns()) / 1'000'000'000;
        exchange_s_.record(static_cast<std::uint64_t>(std::max<std::int64_t>(0, lag_s)));
    }
}

void FeedMetrics::on_feed_status(const messages::FeedStatus& status) noexcept {
    feed_state_.store(static_cast<std::uint8_t>(status.state), std::memory_order_relaxed);
    subscribed_.store(status.subscribed_instruments, std::memory_order_relaxed);
    reconnects_.store(status.reconnects, std::memory_order_relaxed);
}

void FeedMetrics::on_bar(const bars::Bar& bar) noexcept {
    if ((bar.header.flags & bars::kBarForming) != 0) {
        return;
    }
    bars_closed_.fetch_add(1, std::memory_order_relaxed);
    std::int64_t seen = last_bar_start_.load(std::memory_order_relaxed);
    while (bar.start > seen &&
           !last_bar_start_.compare_exchange_weak(seen, bar.start, std::memory_order_relaxed)) {
    }
}

FeedMetrics::Counters FeedMetrics::counters() const noexcept {
    return {.snapshots = snapshots_.load(std::memory_order_relaxed),
            .frames = frames_.load(std::memory_order_relaxed),
            .bars_closed = bars_closed_.load(std::memory_order_relaxed),
            .analytics = analytics_.load(std::memory_order_relaxed),
            .last_tick_ns = last_tick_ns_.load(std::memory_order_relaxed),
            .last_bar_start = last_bar_start_.load(std::memory_order_relaxed),
            .feed_state = feed_state_.load(std::memory_order_relaxed),
            .subscribed = subscribed_.load(std::memory_order_relaxed),
            .reconnects = reconnects_.load(std::memory_order_relaxed)};
}

}  // namespace mda::gateway
