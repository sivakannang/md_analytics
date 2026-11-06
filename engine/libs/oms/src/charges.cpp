#include <algorithm>

#include <mda/oms/charges.hpp>

namespace mda::oms {

namespace {

constexpr std::int64_t kBillion = 1'000'000'000;

/// amount × rate, to the nearest paisa (half up: amounts are positive).
Paise to_paisa(Paise amount, Ppb rate) {
    return (amount * rate + kBillion / 2) / kBillion;
}

/// amount × rate, to the nearest whole rupee, in paise.
Paise to_rupee(Paise amount, Ppb rate) {
    // The calculator rounds to the paisa first, then to the rupee.
    const Paise paise = to_paisa(amount, rate);
    return (paise + 50) / 100 * 100;
}

}  // namespace

ChargeRates zerodha_2026_10() noexcept {
    return ChargeRates{
        .futures = {.brokerage = 300'000,    // 0.03%
                    .brokerage_cap = 2'000,  // ₹20
                    .brokerage_flat = 0,
                    .stt_sell = 500'000,     // 0.05%
                    .exchange_nse = 17'300,  // 0.00173%
                    .ipft_nse = 1'000,       // 0.0001%
                    .exchange_bse = 0,
                    .stamp_buy = 20'000},  // 0.002%
        .options = {.brokerage = 0,
                    .brokerage_cap = 0,
                    .brokerage_flat = 2'000,      // ₹20
                    .stt_sell = 1'500'000,        // 0.15% of premium
                    .exchange_nse = 350'300,      // 0.03503%
                    .ipft_nse = 5'000,            // 0.0005%
                    .exchange_bse = 325'000,      // 0.0325%
                    .stamp_buy = 30'000},         // 0.003%
        .mcx_futures = {.brokerage = 300'000,     // 0.03%
                        .brokerage_cap = 2'000,   // ₹20
                        .stt_sell = 100'000,      // CTT 0.01% (non-agri)
                        .exchange_mcx = 21'000,   // 0.0021%
                        .stamp_buy = 20'000},     // 0.002%
        .mcx_options = {.brokerage_flat = 2'000,  // ₹20
                        .stt_sell = 500'000,      // CTT 0.05% of premium
                        .exchange_mcx = 418'000,  // 0.0418%
                        .stamp_buy = 30'000},     // 0.003%
        .sebi = 1'000,                            // ₹10 / crore
        .gst = 180'000'000,                       // 18%
        .stt_exercise = 1'500'000,                // 0.15% of intrinsic value
    };
}

ChargeBreakdown fill_charges(const ChargeRates& rates, const FillCharge& fill) noexcept {
    const bool mcx = fill.exchange == Exchange::Mcx;
    const SegmentRates& r = fill.segment == Segment::Future
                                ? (mcx ? rates.mcx_futures : rates.futures)
                                : (mcx ? rates.mcx_options : rates.options);
    const Paise turnover = value_paise(fill.price, fill.quantity);
    ChargeBreakdown c;
    if (r.brokerage_flat > 0) {
        c.brokerage = fill.order_brokerage > 0 ? 0 : r.brokerage_flat;  // once per order
    } else {
        const Paise left = std::max<Paise>(0, r.brokerage_cap - fill.order_brokerage);
        c.brokerage = std::min(to_paisa(turnover, r.brokerage), left);
    }
    if (fill.side == Side::Sell) {
        c.stt = to_rupee(turnover, r.stt_sell);
    } else {
        c.stamp = to_rupee(turnover, r.stamp_buy);
    }
    c.exchange = fill.exchange == Exchange::Nse
                     ? to_paisa(turnover, r.exchange_nse) + to_paisa(turnover, r.ipft_nse)
                 : fill.exchange == Exchange::Bse ? to_paisa(turnover, r.exchange_bse)
                                                  : to_paisa(turnover, r.exchange_mcx);
    c.sebi = to_paisa(turnover, rates.sebi);
    c.gst = to_paisa(c.brokerage + c.exchange + c.sebi, rates.gst);
    return c;
}

Paise exercise_stt(const ChargeRates& rates, Paise intrinsic_value) noexcept {
    return intrinsic_value > 0 ? to_rupee(intrinsic_value, rates.stt_exercise) : 0;
}

}  // namespace mda::oms
