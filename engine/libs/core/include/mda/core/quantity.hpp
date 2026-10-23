#pragma once

// Quantity in shares/units (ADR-0009): volume, open interest, depth sizes.
// Lots are a display concern: quantity / lot size of that trading day.

#include <compare>
#include <cstdint>

namespace mda::core {

class Quantity {
public:
    constexpr Quantity() noexcept = default;
    constexpr explicit Quantity(std::int64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

    friend constexpr auto operator<=>(Quantity, Quantity) noexcept = default;
    friend constexpr Quantity operator+(Quantity a, Quantity b) noexcept {
        return Quantity(a.value_ + b.value_);
    }
    friend constexpr Quantity operator-(Quantity a, Quantity b) noexcept {
        return Quantity(a.value_ - b.value_);
    }

private:
    std::int64_t value_ = 0;
};

}  // namespace mda::core
