#include <algorithm>

#include <mda/bars/aggregate.hpp>

namespace mda::bars {
namespace {

constexpr std::int64_t kIstOffset = 5 * 3600 + 30 * 60;
constexpr std::int64_t kSessionStart = 9 * 3600 + 15 * 60;  // seconds after IST midnight

std::int64_t ist_midnight(std::int64_t unix_seconds) {
    const std::int64_t local = unix_seconds + kIstOffset;
    const std::int64_t day = local - ((local % 86'400) + 86'400) % 86'400;
    return day - kIstOffset;
}

// The bar's OI update offset re-based on `bucket` (a start at or before the bar's).
std::uint16_t rebased_oi_offset(const Bar& bar, std::int64_t bucket) {
    if (bar.oi_update_offset == kNoOiUpdateTime) {
        return kNoOiUpdateTime;
    }
    const std::int64_t offset = bar.start - bucket + bar.oi_update_offset;
    return offset < kNoOiUpdateTime ? static_cast<std::uint16_t>(offset) : kNoOiUpdateTime;
}

}  // namespace

std::optional<Interval> parse_interval(std::string_view text) {
    if (text == "1m")
        return Interval{.minutes = 1};
    if (text == "3m")
        return Interval{.minutes = 3};
    if (text == "5m")
        return Interval{.minutes = 5};
    if (text == "15m")
        return Interval{.minutes = 15};
    if (text == "30m")
        return Interval{.minutes = 30};
    if (text == "1h")
        return Interval{.minutes = 60};
    if (text == "1d")
        return Interval{.minutes = 1440, .daily = true};
    return std::nullopt;
}

std::int64_t bucket_start(std::int64_t unix_seconds, Interval interval) {
    const std::int64_t midnight = ist_midnight(unix_seconds);
    if (interval.daily) {
        return midnight;
    }
    const std::int64_t session = midnight + kSessionStart;
    const std::int64_t length = static_cast<std::int64_t>(interval.minutes) * 60;
    const std::int64_t offset = unix_seconds - session;
    const std::int64_t floored = offset >= 0 ? offset / length : -((-offset + length - 1) / length);
    return session + floored * length;
}

std::vector<Bar> aggregate(std::span<const Bar> minutes, Interval interval) {
    std::vector<Bar> out;
    if (interval == Interval{.minutes = 1}) {
        out.assign(minutes.begin(), minutes.end());
        return out;
    }
    for (const Bar& bar : minutes) {
        const std::int64_t start = bucket_start(bar.start, interval);
        if (out.empty() || out.back().start != start) {
            Bar merged = bar;
            merged.start = start;
            merged.oi_update_offset = rebased_oi_offset(bar, start);
            out.push_back(merged);
            continue;
        }
        Bar& m = out.back();
        m.high = std::max(m.high, bar.high);
        m.low = std::min(m.low, bar.low);
        m.close = bar.close;
        m.volume = core::Quantity(m.volume.value() + bar.volume.value());
        m.oi_updates += bar.oi_updates;
        if (bar.oi_update_offset != kNoOiUpdateTime) {
            m.oi_update_offset = rebased_oi_offset(bar, m.start);
        }
        m.trades += bar.trades;
        if (bar.source == BarSource::Live) {
            m.source = BarSource::Live;
        }
        if (bar.oi_close.value() > 0) {
            if (m.oi_close.value() == 0) {  // first OI in the bucket
                m.oi_open = bar.oi_open;
                m.oi_high = bar.oi_high;
                m.oi_low = bar.oi_low;
            } else {
                m.oi_high = core::Quantity(std::max(m.oi_high.value(), bar.oi_high.value()));
                m.oi_low = core::Quantity(std::min(m.oi_low.value(), bar.oi_low.value()));
            }
            m.oi_close = bar.oi_close;
        }
    }
    return out;
}

}  // namespace mda::bars
