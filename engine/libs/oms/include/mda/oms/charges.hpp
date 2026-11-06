#pragma once

// Indian F&O charges per fill (docs/design/paper-trading.md §7), the way Zerodha's
// brokerage calculator computes them (checked against it on 2026-10-24):
// - brokerage: futures 0.03% of the order's turnover capped at ₹20 per executed order;
//   options a flat ₹20 per executed order; to the paisa;
// - STT on the sell side and stamp duty on the buy side, to the whole rupee;
// - exchange charges (plus NSE's IPFT charge, rounded separately) and the SEBI fee, to the
//   paisa;
// - GST on brokerage + exchange charges + SEBI fee (as rounded), to the paisa.
// MCX (ADR-0031) has its own rates: CTT in place of STT, MCX's transaction charges, and no
// tax on exercise (options devolve into futures).
// Rates are parts per billion (1e-9 of turnover), so they are exact integers.

#include <cstdint>

#include <mda/oms/types.hpp>

namespace mda::oms {

using Ppb = std::int64_t;  ///< parts per billion: 0.03% = 300'000

struct SegmentRates {
    Ppb brokerage = 0;           ///< futures: 0.03%, capped at brokerage_cap per order
    Paise brokerage_cap = 0;     ///< futures: ₹20 per executed order
    Paise brokerage_flat = 0;    ///< options: ₹20 per executed order (0 = use the rate)
    Ppb stt_sell = 0;            ///< of the sell turnover
    Ppb exchange_nse = 0;
    Ppb ipft_nse = 0;            ///< NSE's investor protection fund charge, rounded on its own
    Ppb exchange_bse = 0;
    Ppb exchange_mcx = 0;
    Ppb stamp_buy = 0;           ///< of the buy turnover
};

struct ChargeRates {
    SegmentRates futures;
    SegmentRates options;
    SegmentRates mcx_futures;    ///< stt_sell is CTT
    SegmentRates mcx_options;
    Ppb sebi = 0;                ///< ₹10 per crore = 1'000 ppb
    Ppb gst = 0;                 ///< 18% = 180'000'000 ppb
    Ppb stt_exercise = 0;        ///< options exercised at expiry: of the intrinsic value
};

/// Zerodha's rates as published on 2026-10-24 (zerodha.com/charges, brokerage calculator).
[[nodiscard]] ChargeRates zerodha_2026_10() noexcept;

struct ChargeBreakdown {
    Paise brokerage = 0;
    Paise stt = 0;
    Paise exchange = 0;
    Paise sebi = 0;
    Paise stamp = 0;
    Paise gst = 0;

    [[nodiscard]] Paise total() const noexcept { return brokerage + stt + exchange + sebi + stamp + gst; }
};

struct FillCharge {
    Segment segment = Segment::Future;
    Exchange exchange = Exchange::Nse;
    Side side = Side::Buy;
    std::int64_t quantity = 0;
    core::Price price;
    /// Brokerage already charged to this order by earlier fills (the cap is per order).
    Paise order_brokerage = 0;
};

[[nodiscard]] ChargeBreakdown fill_charges(const ChargeRates& rates, const FillCharge& fill) noexcept;

/// STT on a bought option exercised at expiry: of its intrinsic value, to the whole rupee.
[[nodiscard]] Paise exercise_stt(const ChargeRates& rates, Paise intrinsic_value) noexcept;

}  // namespace mda::oms
