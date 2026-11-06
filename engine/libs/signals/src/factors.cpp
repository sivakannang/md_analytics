#include <mda/signals/factors.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace mda::signals {
namespace {

FactorValue present(FactorId id, double value) {
    return {.id = id, .status = FactorStatus::Present, .value = std::clamp(value, -1.0, 1.0)};
}

FactorValue absent(FactorId id) {
    return {.id = id, .status = FactorStatus::Absent, .value = 0};
}

// The nearest expiry's summary at `t` and `window_min` earlier, both on t's day.
std::optional<std::pair<SummaryPoint, SummaryPoint>> summary_change(const MarketState& state,
                                                                    std::int64_t t,
                                                                    std::int64_t window_min) {
    const auto expiry = state.nearest_expiry(t);
    if (!expiry) {
        return std::nullopt;
    }
    const auto* series = state.summaries(*expiry);
    const std::int64_t from = ist_day_start(t);
    const auto* now = series->at(t, from);
    const auto* then = series->at(t - window_min * 60, from);
    if (now == nullptr || then == nullptr) {
        return std::nullopt;
    }
    return std::pair{*now, *then};
}

}  // namespace

FactorValue futures_buildup(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::FuturesBuildup;
    const std::int64_t from = ist_day_start(t);
    const auto* now = state.future().at(t, from);
    const auto* then = state.future().at(t - params.buildup_window_min * 60, from);
    if (now == nullptr || then == nullptr || then->oi <= 0) {
        return absent(id);
    }
    const double price_change = now->close - then->close;
    const std::int64_t oi_change = now->oi - then->oi;
    const double threshold = params.buildup_oi_fraction * static_cast<double>(then->oi);
    if (static_cast<double>(std::llabs(oi_change)) < threshold || price_change == 0) {
        return present(id, 0);
    }
    if (oi_change > 0) {
        return present(id, price_change > 0 ? 1.0 : -1.0);  // long / short buildup
    }
    return present(id, price_change > 0 ? 0.5 : -0.5);  // short covering / long unwinding
}

FactorValue atm_oi_shift(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::AtmOiShift;
    const auto expiry = state.nearest_expiry(t);
    if (!expiry) {
        return absent(id);
    }
    const std::int64_t from = ist_day_start(t);
    const auto* summary = state.summaries(*expiry)->at(t, from);
    const auto strikes = state.strikes(*expiry);
    if (summary->atm_strike <= 0 || strikes.empty()) {
        return absent(id);
    }

    // ATM ± N strikes by position in the strike list, so uneven strike steps still work.
    std::size_t atm = 0;
    for (std::size_t i = 1; i < strikes.size(); ++i) {
        if (std::abs(strikes[i] - summary->atm_strike) < std::abs(strikes[atm] - summary->atm_strike)) {
            atm = i;
        }
    }
    const auto n = static_cast<std::size_t>(params.atm_strikes);
    const double low = strikes[atm >= n ? atm - n : 0];
    const double high = strikes[std::min(atm + n, strikes.size() - 1)];

    std::int64_t put_change = 0;
    std::int64_t call_change = 0;
    std::int64_t total = 0;
    const std::int64_t then_t = t - params.oi_window_min * 60;
    for (const auto& option : state.options()) {
        const auto& info = option.info;
        if (info.expiry_days != *expiry || info.strike < low || info.strike > high) {
            continue;
        }
        const auto* now = option.oi.at(t, from);
        const auto* then = option.oi.at(then_t, from);
        if (now == nullptr || then == nullptr) {
            continue;
        }
        (info.call ? call_change : put_change) += *now - *then;
        total += *now;
    }
    if (total <= 0) {
        return absent(id);
    }
    // Put writers adding (put OI up) and call writers leaving (call OI down) are bullish.
    const double shift = static_cast<double>(put_change - call_change) / static_cast<double>(total);
    return present(id, std::tanh(shift / params.oi_shift_scale));
}

FactorValue pcr_change(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::PcrChange;
    const auto change = summary_change(state, t, params.oi_window_min);
    if (!change || change->first.pcr <= 0 || change->second.pcr <= 0) {
        return absent(id);
    }
    return present(id, std::tanh((change->first.pcr - change->second.pcr) / params.pcr_scale));
}

FactorValue future_vs_vwap(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::FutureVsVwap;
    const auto day = state.future().between(ist_day_start(t), t);
    double value_sum = 0;
    std::int64_t volume_sum = 0;
    for (const auto& point : day) {
        value_sum += point.value.typical * static_cast<double>(point.value.volume);
        volume_sum += point.value.volume;
    }
    if (volume_sum <= 0) {
        return absent(id);
    }
    const double vwap = value_sum / static_cast<double>(volume_sum);
    const double close = day.back().value.close;
    return present(id, std::tanh((close - vwap) / vwap / params.vwap_scale));
}

FactorValue atm_iv_change(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::AtmIvChange;
    const auto change = summary_change(state, t, params.oi_window_min);
    if (!change || change->first.atm_iv <= 0 || change->second.atm_iv <= 0) {
        return absent(id);
    }
    // Falling IV: fear easing, which tends to go with a rising index.
    return present(id, -std::tanh((change->first.atm_iv - change->second.atm_iv) / params.iv_scale));
}

FactorValue max_pain_pull(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::MaxPainPull;
    const auto expiry = state.nearest_expiry(t);
    if (!expiry) {
        return absent(id);
    }
    if (*expiry - ist_day(t) > params.max_pain_days) {
        return {.id = id, .status = FactorStatus::NotApplicable, .value = 0};
    }
    const std::int64_t from = ist_day_start(t);
    const auto* summary = state.summaries(*expiry)->at(t, from);
    const auto* spot = state.spot().at(t, from);
    if (spot == nullptr || *spot <= 0 || summary->max_pain <= 0) {
        return absent(id);
    }
    return present(id, std::tanh((summary->max_pain - *spot) / *spot / params.max_pain_scale));
}

FactorValue sector_breadth(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::SectorBreadth;
    const std::int64_t from = ist_day_start(t);
    int counted = 0;
    int up = 0;
    for (const auto& sector : state.sectors()) {
        const auto* close = sector.close.at(t, from);
        if (close == nullptr || sector.prev_close <= 0) {
            continue;
        }
        ++counted;
        up += *close > sector.prev_close ? 1 : 0;
    }
    if (counted < params.min_sectors) {
        return absent(id);
    }
    return present(id, 2.0 * up / counted - 1.0);
}

FactorValue vix_change(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::VixChange;
    const std::int64_t from = ist_day_start(t);
    const auto* now = state.vix().at(t, from);
    const auto* then = state.vix().at(t - params.vix_window_min * 60, from);
    if (now == nullptr || then == nullptr || *then <= 0) {
        return absent(id);
    }
    // Rising VIX: hedging demand, which tends to go with a falling index.
    return present(id, -std::tanh((*now - *then) / *then / params.vix_scale));
}

FactorValue volume_delta(const MarketState& state, std::int64_t t, const FactorParams& params) {
    constexpr auto id = FactorId::VolumeDelta;
    const std::int64_t from = std::max(ist_day_start(t), t - (params.flow_window_min - 1) * 60);
    const auto minutes = state.flow().between(from, t);
    std::int64_t buy = 0;
    std::int64_t sell = 0;
    std::int64_t unclassified = 0;
    int traded = 0;  // minutes with volume: quiet minutes, pre-open and a thin future don't count
    for (const auto& minute : minutes) {
        buy += minute.value.buy;
        sell += minute.value.sell;
        unclassified += minute.value.unclassified;
        traded += minute.value.buy + minute.value.sell + minute.value.unclassified > 0 ? 1 : 0;
    }
    // Too little of the window traded (a restart, a gap, a thin future such as BANKEX's), or
    // mostly unclassified volume (no depth: feed.mode isn't full): no reading rather than a
    // biased one.
    if (traded < params.flow_min_minutes || buy + sell == 0 ||
        unclassified * 2 > buy + sell + unclassified) {
        return absent(id);
    }
    const double imbalance = static_cast<double>(buy - sell) / static_cast<double>(buy + sell);
    return present(id, std::tanh(imbalance / params.flow_scale));
}

FactorValue evaluate_factor(FactorId id, const MarketState& state, std::int64_t t,
                            const FactorParams& params) {
    switch (id) {
    case FactorId::FuturesBuildup:
        return futures_buildup(state, t, params);
    case FactorId::AtmOiShift:
        return atm_oi_shift(state, t, params);
    case FactorId::PcrChange:
        return pcr_change(state, t, params);
    case FactorId::FutureVsVwap:
        return future_vs_vwap(state, t, params);
    case FactorId::AtmIvChange:
        return atm_iv_change(state, t, params);
    case FactorId::MaxPainPull:
        return max_pain_pull(state, t, params);
    case FactorId::SectorBreadth:
        return sector_breadth(state, t, params);
    case FactorId::VixChange:
        return vix_change(state, t, params);
    case FactorId::VolumeDelta:
        return volume_delta(state, t, params);
    }
    return absent(id);
}

std::string_view name(FactorId id) noexcept {
    switch (id) {
    case FactorId::FuturesBuildup:
        return "futures_buildup";
    case FactorId::AtmOiShift:
        return "atm_oi_shift";
    case FactorId::PcrChange:
        return "pcr_change";
    case FactorId::FutureVsVwap:
        return "future_vs_vwap";
    case FactorId::AtmIvChange:
        return "atm_iv_change";
    case FactorId::MaxPainPull:
        return "max_pain_pull";
    case FactorId::SectorBreadth:
        return "sector_breadth";
    case FactorId::VixChange:
        return "vix_change";
    case FactorId::VolumeDelta:
        return "volume_delta";
    }
    return "unknown";
}

}  // namespace mda::signals
