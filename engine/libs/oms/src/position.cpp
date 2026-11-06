#include <mda/oms/position.hpp>

#include <algorithm>
#include <cstdlib>

namespace mda::oms {

namespace {

/// a × b / c rounded half away from zero (c > 0).
std::int64_t mul_div_round(std::int64_t a, std::int64_t b, std::int64_t c) {
    const std::int64_t product = a * b;
    const std::int64_t half = c / 2;
    return product >= 0 ? (product + half) / c : -((-product + half) / c);
}

core::Price per_unit(std::int64_t cost_raw, std::int64_t quantity) {
    if (quantity == 0) {
        return {};
    }
    return core::Price::from_raw(mul_div_round(cost_raw, 1, quantity > 0 ? quantity : -quantity) *
                                 (quantity > 0 ? 1 : -1));
}

}  // namespace

Paise Position::apply(Side side, std::int64_t quantity, core::Price price) noexcept {
    const std::int64_t signed_qty = sign(side) * quantity;
    if (quantity_ == 0 || (quantity_ > 0) == (signed_qty > 0)) {
        quantity_ += signed_qty;  // opening or adding
        cost_raw_ += price.raw() * signed_qty;
        entry_cost_raw_ += price.raw() * signed_qty;
        return 0;
    }
    // Reducing: close up to the open quantity at the average, realise the difference.
    const std::int64_t closing = std::min(quantity, std::abs(quantity_));
    const std::int64_t closed_cost = mul_div_round(cost_raw_, closing, std::abs(quantity_));
    const std::int64_t closed_entry = mul_div_round(entry_cost_raw_, closing, std::abs(quantity_));
    const std::int64_t direction = quantity_ > 0 ? 1 : -1;
    const Paise realised = raw_to_paise(price.raw() * closing * direction - closed_cost);
    cost_raw_ -= closed_cost;
    entry_cost_raw_ -= closed_entry;
    quantity_ += signed_qty > 0 ? closing : -closing;
    if (quantity_ == 0) {
        cost_raw_ = 0;
        entry_cost_raw_ = 0;
    }
    realised_ += realised;
    if (const std::int64_t rest = quantity - closing; rest > 0) {
        quantity_ = sign(side) * rest;  // through zero: the rest opens at the fill price
        cost_raw_ = price.raw() * quantity_;
        entry_cost_raw_ = cost_raw_;
    }
    return realised;
}

Paise Position::entry_pnl(Side side, std::int64_t quantity, core::Price price) const noexcept {
    if (quantity_ == 0 || (quantity_ > 0) == (sign(side) > 0)) {
        return 0;
    }
    const std::int64_t closing = std::min(quantity, std::abs(quantity_));
    const std::int64_t closed_entry = mul_div_round(entry_cost_raw_, closing, std::abs(quantity_));
    return raw_to_paise(price.raw() * closing * (quantity_ > 0 ? 1 : -1) - closed_entry);
}

Paise Position::mark(core::Price close) noexcept {
    if (quantity_ == 0) {
        return 0;
    }
    const Paise pnl = unrealised(close);
    realised_ += pnl;
    cost_raw_ = close.raw() * quantity_;
    return pnl;
}

Position::Taken Position::take(std::int64_t quantity) noexcept {
    const std::int64_t open = std::abs(quantity_);
    const std::int64_t taken = std::min(quantity, open);
    if (taken <= 0) {
        return {};
    }
    const Taken out{.cost_raw = taken == open ? cost_raw_ : mul_div_round(cost_raw_, taken, open),
                    .entry_cost_raw = taken == open ? entry_cost_raw_ : mul_div_round(entry_cost_raw_, taken, open)};
    cost_raw_ -= out.cost_raw;
    entry_cost_raw_ -= out.entry_cost_raw;
    quantity_ -= quantity_ > 0 ? taken : -taken;
    if (quantity_ == 0) {
        cost_raw_ = 0;
        entry_cost_raw_ = 0;
    }
    return out;
}

Paise Position::put(std::int64_t signed_quantity, Taken taken) noexcept {
    if (signed_quantity == 0) {
        return 0;
    }
    if (quantity_ == 0 || (quantity_ > 0) == (signed_quantity > 0)) {
        quantity_ += signed_quantity;
        cost_raw_ += taken.cost_raw;
        entry_cost_raw_ += taken.entry_cost_raw;
        return 0;
    }
    const std::int64_t quantity = std::abs(signed_quantity);
    const std::int64_t rest = quantity - std::min(quantity, std::abs(quantity_));
    const core::Price average = core::Price::from_raw(mul_div_round(std::abs(taken.cost_raw), 1, quantity));
    const Paise realised = apply(signed_quantity > 0 ? Side::Buy : Side::Sell, quantity, average);
    if (rest > 0) {  // through zero: the rest keeps the entry price it came with
        entry_cost_raw_ = mul_div_round(taken.entry_cost_raw, rest, quantity);
    }
    return realised;
}

core::Price Position::average_price() const noexcept {
    return per_unit(cost_raw_, quantity_);
}

core::Price Position::entry_price() const noexcept {
    return per_unit(entry_cost_raw_, quantity_);
}

Paise Position::unrealised(core::Price last) const noexcept {
    return raw_to_paise(last.raw() * quantity_ - cost_raw_);
}

Paise Position::notional(core::Price price) const noexcept {
    return value_paise(price, std::abs(quantity_));
}

}  // namespace mda::oms
