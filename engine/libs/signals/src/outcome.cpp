#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <mda/signals/market_state.hpp>
#include <mda/signals/outcome.hpp>

namespace mda::signals {

Outcome outcome(Label label, std::int64_t spot_raw, std::int64_t spot_after_raw, int flat_bp) {
    Outcome result{
        .spot_after_raw = spot_after_raw, .return_bp = std::nullopt, .hit = std::nullopt};
    if (spot_raw <= 0 || spot_after_raw <= 0) {
        return result;
    }
    const double change =
        static_cast<double>(spot_after_raw - spot_raw) / static_cast<double>(spot_raw);
    const int bp = static_cast<int>(std::lround(change * 10'000));
    result.return_bp = bp;
    if (label != Label::Neutral && std::abs(bp) > flat_bp) {
        result.hit = (bp > 0) == (label == Label::Bullish);
    }
    return result;
}

bool day_closed(std::int64_t signal_minute, std::int64_t latest_minute) {
    if (ist_day(latest_minute) > ist_day(signal_minute)) {
        return true;
    }
    return latest_minute - ist_day_start(latest_minute) >= kCashLastMinute;
}

std::optional<std::int64_t> settle_price(std::span<const SpotClose> day, std::int64_t minute,
                                         int horizon_min, bool closed) {
    const std::int64_t due = minute + std::int64_t{horizon_min} * 60;
    const auto it = std::ranges::lower_bound(day, due, {}, &SpotClose::minute);
    if (it != day.end()) {
        return it->close_raw;
    }
    if (closed && !day.empty() && day.back().minute >= minute) {
        return day.back().close_raw;
    }
    return std::nullopt;
}

}  // namespace mda::signals
