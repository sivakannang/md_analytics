#pragma once

// A net position in one instrument and product (docs/design/paper-trading.md §6). The
// average price is the weighted average of the fills that opened it, as Kite shows; a
// reducing fill realises (fill price − average) × quantity and leaves the average alone; a
// fill through zero closes the old position and opens the rest at the fill price. The daily
// mark moves the average to the close; the entry price stays at what the fills paid.

#include <cstdint>

#include <mda/core/price.hpp>
#include <mda/oms/types.hpp>

namespace mda::oms {

class Position {
public:
    Position() = default;
    /// As stored (paper.db): quantity, cost of the open quantity and realised P&L, and the
    /// open quantity's cost at its fill prices.
    Position(std::int64_t quantity, std::int64_t cost_raw, Paise realised,
             std::int64_t entry_cost_raw) noexcept
        : quantity_(quantity)
        , cost_raw_(cost_raw)
        , realised_(realised)
        , entry_cost_raw_(entry_cost_raw) {}
    /// Never marked: the entry cost is the cost.
    Position(std::int64_t quantity, std::int64_t cost_raw, Paise realised) noexcept
        : Position(quantity, cost_raw, realised, cost_raw) {}

    /// What a product conversion moves: signed like cost_raw.
    struct Taken {
        std::int64_t cost_raw = 0;
        std::int64_t entry_cost_raw = 0;
    };

    /// Applies a fill and returns the P&L it realised (paise; 0 for an opening fill).
    Paise apply(Side side, std::int64_t quantity, core::Price price) noexcept;

    /// Daily mark-to-market: realises the open quantity's P&L at `close` and makes `close`
    /// its average. Returns that P&L (paise).
    Paise mark(core::Price close) noexcept;

    /// Product conversion (MIS ↔ NRML): takes `quantity` (at most the open quantity) out at
    /// its share of the costs. Returns them; no P&L.
    Taken take(std::int64_t quantity) noexcept;
    /// Product conversion: adds `signed_quantity` with the costs `take` returned. Opening or
    /// adding keeps them exactly; against an opposite position it nets like a fill at that
    /// average. Returns the P&L realised (paise).
    Paise put(std::int64_t signed_quantity, Taken taken) noexcept;

    /// What `apply` would realise if the average were still the entry price: a closing fill's
    /// P&L from what the fills paid, before any daily mark. 0 for an opening fill.
    [[nodiscard]] Paise entry_pnl(Side side, std::int64_t quantity,
                                  core::Price price) const noexcept;

    /// Net quantity: positive long, negative short, 0 flat.
    [[nodiscard]] std::int64_t quantity() const noexcept { return quantity_; }
    /// Average opening price; zero when flat.
    [[nodiscard]] core::Price average_price() const noexcept;
    /// Weighted average of the fills behind the open quantity, untouched by the daily mark;
    /// zero when flat.
    [[nodiscard]] core::Price entry_price() const noexcept;
    /// P&L realised since the position was created (paise).
    [[nodiscard]] Paise realised() const noexcept { return realised_; }
    /// P&L of the open quantity at `last` (paise).
    [[nodiscard]] Paise unrealised(core::Price last) const noexcept;
    /// Notional of the open quantity at `price` (paise, positive).
    [[nodiscard]] Paise notional(core::Price price) const noexcept;
    /// Σ price.raw × quantity of the open quantity (signed), for storage.
    [[nodiscard]] std::int64_t cost_raw() const noexcept { return cost_raw_; }
    /// Σ fill price.raw × quantity of the open quantity (signed), for storage.
    [[nodiscard]] std::int64_t entry_cost_raw() const noexcept { return entry_cost_raw_; }

private:
    std::int64_t quantity_ = 0;
    std::int64_t cost_raw_ = 0;  ///< Σ price.raw × quantity of the open quantity (signed)
    Paise realised_ = 0;
    std::int64_t entry_cost_raw_ = 0;  ///< as cost_raw_, but the daily mark leaves it alone
};

}  // namespace mda::oms
