#pragma once

// Paper trading vocabulary (ADR-0029, docs/design/paper-trading.md §4): sides, order types,
// products, statuses and money. Values are stored in paper.db and sent on the buses, so
// only append.

#include <cstdint>
#include <string_view>

#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>

namespace mda::oms {

/// Money in paise (1/100 rupee). Every amount the ledger holds is a whole number of paise.
using Paise = std::int64_t;

/// Times (`at`, `now`) throughout the OMS are Unix milliseconds: the fill model's latency is
/// in milliseconds.
using Millis = std::int64_t;

enum class Side : std::uint8_t { Buy = 1, Sell = 2 };

enum class OrderType : std::uint8_t {
    Market = 1,
    Limit = 2,
    StopLimit = 3,   ///< SL: a LIMIT order once the trigger is reached
    StopMarket = 4,  ///< SL-M: a MARKET order once the trigger is reached
};

enum class Product : std::uint8_t {
    Mis = 1,   ///< intraday: squared off near the close
    Nrml = 2,  ///< carried until closed or expiry
};

enum class Source : std::uint8_t {
    Ui = 1,
    Auto = 2,
    System = 3,  ///< the broker itself: MIS square-off
};

enum class OrderStatus : std::uint8_t {
    TriggerPending = 1,  ///< SL / SL-M waiting for its trigger
    Open = 2,
    Complete = 3,
    Cancelled = 4,
    Rejected = 5,
};

/// Futures and options pay different charges.
enum class Segment : std::uint8_t { Future = 1, Option = 2 };

enum class Exchange : std::uint8_t { Nse = 1, Bse = 2, Mcx = 3 };

[[nodiscard]] constexpr bool is_final(OrderStatus status) noexcept {
    return status == OrderStatus::Complete || status == OrderStatus::Cancelled ||
           status == OrderStatus::Rejected;
}

/// +1 for a buy, -1 for a sell.
[[nodiscard]] constexpr std::int64_t sign(Side side) noexcept {
    return side == Side::Buy ? 1 : -1;
}

/// price × quantity in paise, rounded half away from zero (exact for tick-sized prices).
[[nodiscard]] Paise value_paise(core::Price price, std::int64_t quantity) noexcept;

/// `raw` (core::Price units, 1/10000 rupee) in paise, rounded half away from zero.
[[nodiscard]] Paise raw_to_paise(std::int64_t raw) noexcept;

[[nodiscard]] std::string_view name(Side side) noexcept;
[[nodiscard]] std::string_view name(OrderType type) noexcept;
[[nodiscard]] std::string_view name(Product product) noexcept;
[[nodiscard]] std::string_view name(OrderStatus status) noexcept;

}  // namespace mda::oms
