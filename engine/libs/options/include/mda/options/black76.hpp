#pragma once

// Black-76 on a forward price, with r = 0 (the forward already carries the cost of carry;
// ADR-0022). Prices and the forward are in rupees, T in years, sigma as a fraction (0.15).

#include <optional>

namespace mda::options {

struct Greeks {
    double delta = 0;  ///< per 1 point of the forward
    double gamma = 0;  ///< per 1 point
    double theta = 0;  ///< per calendar day
    double vega = 0;   ///< per 1 volatility point (1%)
};

[[nodiscard]] double black76_price(double forward, double strike, double years, double sigma,
                                   bool call);

[[nodiscard]] Greeks black76_greeks(double forward, double strike, double years, double sigma,
                                    bool call);

/// The sigma that reproduces `price`; nothing when the price is outside the no-arbitrage
/// range (below intrinsic value, or above the forward / strike) or inputs are invalid.
/// Newton–Raphson from 20%, bisection fallback; accurate to 1e-6 in price.
[[nodiscard]] std::optional<double> implied_vol(double price, double forward, double strike,
                                                double years, bool call);

}  // namespace mda::options
