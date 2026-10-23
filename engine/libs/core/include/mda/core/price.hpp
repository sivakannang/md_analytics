#pragma once

// Fixed-point price (ADR-0009): int64 in units of 1/10,000 rupee.
//
// A strong type: no implicit conversion to or from integers or Quantity, so a price
// can't be added to a quantity or passed where a raw count is expected.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mda::core {

class Price {
public:
    static constexpr std::int64_t kScale = 10'000;

    constexpr Price() noexcept = default;

    [[nodiscard]] static constexpr Price from_raw(std::int64_t raw) noexcept { return Price(raw); }
    /// Kite sends NSE/BSE prices as integer paise (1/100 rupee).
    [[nodiscard]] static constexpr Price from_paise(std::int64_t paise) noexcept {
        return Price(paise * (kScale / 100));
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
    /// For display and analytics only; never compare prices as doubles.
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(raw_) / static_cast<double>(kScale);
    }

    friend constexpr auto operator<=>(Price, Price) noexcept = default;
    friend constexpr Price operator+(Price a, Price b) noexcept { return Price(a.raw_ + b.raw_); }
    friend constexpr Price operator-(Price a, Price b) noexcept { return Price(a.raw_ - b.raw_); }
    friend constexpr Price operator-(Price a) noexcept { return Price(-a.raw_); }

private:
    constexpr explicit Price(std::int64_t raw) noexcept : raw_(raw) {}

    std::int64_t raw_ = 0;
};

/// Parses decimal text exactly ("25000", "24550.5", "0.05", "-7.25"); no floating point.
/// Fails on more than 4 significant decimals, junk characters, or overflow.
[[nodiscard]] constexpr std::optional<Price> parse_price(std::string_view text) noexcept {
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty() || text == ".") {
        return std::nullopt;
    }
    std::int64_t whole = 0;
    std::int64_t fraction = 0;
    int fraction_digits = 0;
    bool in_fraction = false;
    constexpr std::int64_t kMaxWhole = INT64_MAX / Price::kScale;
    for (const char c : text) {
        if (c == '.' && !in_fraction) {
            in_fraction = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const int digit = c - '0';
        if (!in_fraction) {
            if (whole > (kMaxWhole - digit) / 10) {
                return std::nullopt;
            }
            whole = whole * 10 + digit;
        } else if (fraction_digits < 4) {
            fraction = fraction * 10 + digit;
            ++fraction_digits;
        } else if (digit != 0) {
            return std::nullopt;  // finer than 1/10,000: not representable exactly
        }
    }
    for (; fraction_digits < 4; ++fraction_digits) {
        fraction *= 10;
    }
    const std::int64_t raw = whole * Price::kScale + fraction;
    return Price::from_raw(negative ? -raw : raw);
}

/// Exact decimal text with at least two decimals: 123.45, 0.0025, -7.50.
[[nodiscard]] inline std::string to_string(Price price) {
    const std::int64_t raw = price.raw();
    const std::uint64_t magnitude =
        raw < 0 ? 0 - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
    const auto scale = static_cast<std::uint64_t>(Price::kScale);
    std::string fraction = std::to_string(magnitude % scale);
    fraction.insert(0, 4 - fraction.size(), '0');
    while (fraction.size() > 2 && fraction.back() == '0') {
        fraction.pop_back();
    }
    return (raw < 0 ? "-" : "") + std::to_string(magnitude / scale) + "." + fraction;
}

}  // namespace mda::core
