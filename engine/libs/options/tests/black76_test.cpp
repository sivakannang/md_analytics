#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include <mda/options/black76.hpp>

namespace mda::options {
namespace {

TEST(Black76Test, PricesTheTextbookAtTheMoneyCase) {
    // F = K = 100, T = 1, sigma = 20%: C = P = 100 * (N(0.1) - N(-0.1)) = 7.9656
    EXPECT_NEAR(black76_price(100, 100, 1, 0.2, true), 7.965567, 1e-5);
    EXPECT_NEAR(black76_price(100, 100, 1, 0.2, false), 7.965567, 1e-5);
}

TEST(Black76Test, SatisfiesPutCallParity) {
    for (const double strike : {80.0, 95.0, 100.0, 110.0, 130.0}) {
        const double call = black76_price(102, strike, 0.25, 0.3, true);
        const double put = black76_price(102, strike, 0.25, 0.3, false);
        EXPECT_NEAR(call - put, 102 - strike, 1e-9);  // r = 0: C - P = F - K
    }
}

TEST(Black76Test, GreeksMatchFiniteDifferences) {
    const double f = 22'650, k = 22'700, t = 5.0 / 365, s = 0.15, h = 0.01;
    for (const bool call : {true, false}) {
        const Greeks g = black76_greeks(f, k, t, s, call);
        const double up = black76_price(f + h, k, t, s, call);
        const double down = black76_price(f - h, k, t, s, call);
        const double mid = black76_price(f, k, t, s, call);
        EXPECT_NEAR(g.delta, (up - down) / (2 * h), 1e-4);
        EXPECT_NEAR(g.gamma, (up - 2 * mid + down) / (h * h), 1e-4);
        EXPECT_NEAR(
            g.vega,
            (black76_price(f, k, t, s + 0.0001, call) - black76_price(f, k, t, s - 0.0001, call)) /
                0.0002 / 100,
            1e-4);
        // Theta is the instantaneous decay per calendar day: compare with a tiny time step.
        const double dt = 1e-5 / 365;
        EXPECT_NEAR(g.theta, (black76_price(f, k, t - dt, s, call) - mid) / (dt * 365),
                    std::abs(g.theta) * 1e-3);
    }
    EXPECT_NEAR(black76_greeks(f, k, t, s, true).delta - black76_greeks(f, k, t, s, false).delta,
                1.0, 1e-12);
}

TEST(ImpliedVolTest, RecoversTheVolatility) {
    for (const double sigma : {0.05, 0.12, 0.2, 0.45, 1.2}) {
        for (const double strike : {20'000.0, 22'500.0, 22'650.0, 23'500.0}) {
            for (const bool call : {true, false}) {
                const double price = black76_price(22'650, strike, 12.0 / 365, sigma, call);
                const double intrinsic =
                    call ? std::max(22'650 - strike, 0.0) : std::max(strike - 22'650, 0.0);
                if (price - intrinsic < 0.01) {
                    continue;  // (almost) no time value: not meaningfully invertible
                }
                const auto iv = implied_vol(price, 22'650, strike, 12.0 / 365, call);
                ASSERT_TRUE(iv.has_value()) << sigma << " " << strike << " " << call;
                EXPECT_NEAR(*iv, sigma, 1e-4);
            }
        }
    }
}

TEST(ImpliedVolTest, RejectsArbitragePrices) {
    EXPECT_FALSE(implied_vol(40, 22'650, 22'600, 0.01, true).has_value());  // below intrinsic 50
    EXPECT_FALSE(implied_vol(23'000, 22'650, 22'600, 0.01, true).has_value());  // above the forward
    EXPECT_FALSE(implied_vol(0, 22'650, 22'600, 0.01, true).has_value());
    EXPECT_FALSE(implied_vol(10, 22'650, 22'600, 0, true).has_value());
}

}  // namespace
}  // namespace mda::options
