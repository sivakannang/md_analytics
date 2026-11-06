#include <gtest/gtest.h>

#include <mda/oms/charges.hpp>

namespace mda::oms {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

const ChargeRates kRates = zerodha_2026_10();

ChargeBreakdown leg(Segment segment, Side side, double price, std::int64_t qty,
                    Exchange exchange = Exchange::Nse, Paise order_brokerage = 0) {
    return fill_charges(kRates, {.segment = segment,
                                 .exchange = exchange,
                                 .side = side,
                                 .quantity = qty,
                                 .price = rupees(price),
                                 .order_brokerage = order_brokerage});
}

// Worked by hand from the calculator's formulas (zerodha.com/brokerage-calculator,
// 2026-10-24); amounts in paise.
TEST(Charges, FuturesLegs) {
    const auto buy = leg(Segment::Future, Side::Buy, 22'517.35, 75);  // turnover ₹16,88,801.25
    EXPECT_EQ(buy.brokerage, 2'000);  // 0.03% = ₹506.64, capped at ₹20
    EXPECT_EQ(buy.stt, 0);
    EXPECT_EQ(buy.exchange, 2'922 + 169);  // 0.00173% + IPFT 0.0001%, rounded separately
    EXPECT_EQ(buy.sebi, 169);
    EXPECT_EQ(buy.stamp, 3'400);  // 0.002% = ₹33.78, to the rupee
    EXPECT_EQ(buy.gst, 947);      // 18% of ₹52.60
    EXPECT_EQ(buy.total(), 9'607);

    const auto sell = leg(Segment::Future, Side::Sell, 22'603.85, 75);  // ₹16,95,288.75
    EXPECT_EQ(sell.stt, 84'800);  // 0.05% = ₹847.64, to the rupee
    EXPECT_EQ(sell.exchange, 2'933 + 170);
    EXPECT_EQ(sell.sebi, 170);
    EXPECT_EQ(sell.stamp, 0);
    EXPECT_EQ(sell.gst, 949);
    EXPECT_EQ(sell.total(), 91'022);
}

TEST(Charges, OptionsLegs) {
    const auto buy = leg(Segment::Option, Side::Buy, 120.55, 75);  // premium ₹9,041.25
    EXPECT_EQ(buy.brokerage, 2'000);
    EXPECT_EQ(buy.exchange, 317 + 5);
    EXPECT_EQ(buy.sebi, 1);
    EXPECT_EQ(buy.stamp, 0);  // 0.003% = ₹0.27, to the rupee
    EXPECT_EQ(buy.gst, 418);
    EXPECT_EQ(buy.total(), 2'741);

    const auto sell = leg(Segment::Option, Side::Sell, 149.95, 75);  // ₹11,246.25
    EXPECT_EQ(sell.stt, 1'700);                                      // 0.15% = ₹16.87, to the rupee
    EXPECT_EQ(sell.exchange, 394 + 6);
    EXPECT_EQ(sell.gst, 432);
    EXPECT_EQ(sell.total(), 4'533);
}

// The calculator works on the round trip's combined turnover; per-leg rounding (as on a
// contract note) may differ by a few paise. Its totals: ₹1,006.25 and ₹72.73.
TEST(Charges, RoundTripsMatchTheCalculatorWithinTenPaise) {
    const Paise futures = leg(Segment::Future, Side::Buy, 22'517.35, 75).total() +
                          leg(Segment::Future, Side::Sell, 22'603.85, 75).total();
    EXPECT_NEAR(static_cast<double>(futures), 100'625.0, 10.0);
    const Paise options = leg(Segment::Option, Side::Buy, 120.55, 75).total() +
                          leg(Segment::Option, Side::Sell, 149.95, 75).total();
    EXPECT_NEAR(static_cast<double>(options), 7'273.0, 10.0);
    // And the calculator's defaults (buy 1000, sell 1100, qty 400): ₹294.33.
    const Paise defaults = leg(Segment::Future, Side::Buy, 1'000, 400).total() +
                           leg(Segment::Future, Side::Sell, 1'100, 400).total();
    EXPECT_NEAR(static_cast<double>(defaults), 29'433.0, 10.0);
}

TEST(Charges, BrokerageIsPerExecutedOrder) {
    // Futures: 0.03% of each fill until the order has paid ₹20.
    EXPECT_EQ(leg(Segment::Future, Side::Buy, 1'000, 50).brokerage, 1'500);  // ₹50,000 → ₹15
    EXPECT_EQ(leg(Segment::Future, Side::Buy, 1'000, 50, Exchange::Nse, 1'500).brokerage, 500);
    EXPECT_EQ(leg(Segment::Future, Side::Buy, 1'000, 50, Exchange::Nse, 2'000).brokerage, 0);
    // Options: ₹20 on the first fill only.
    EXPECT_EQ(leg(Segment::Option, Side::Buy, 100, 75, Exchange::Nse, 2'000).brokerage, 0);
}

TEST(Charges, BseRates) {
    EXPECT_EQ(leg(Segment::Future, Side::Buy, 22'517.35, 75, Exchange::Bse).exchange, 0);
    // Options on BSE: 0.0325% of ₹9,041.25 = ₹2.94, no IPFT.
    EXPECT_EQ(leg(Segment::Option, Side::Buy, 120.55, 75, Exchange::Bse).exchange, 294);
}

// MCX (ADR-0031, zerodha.com/charges 2026-10-27): CTT instead of STT, MCX's own rates.
TEST(Charges, McxRates) {
    // One crude oil lot (100 barrels) at ₹9,600: turnover ₹9,60,000.
    const auto buy = leg(Segment::Future, Side::Buy, 9'600, 100, Exchange::Mcx);
    EXPECT_EQ(buy.brokerage, 2'000);  // 0.03% = ₹288, capped at ₹20
    EXPECT_EQ(buy.exchange, 2'016);   // 0.0021%
    EXPECT_EQ(buy.sebi, 96);          // ₹10 / crore
    EXPECT_EQ(buy.stamp, 1'900);      // 0.002% = ₹19.20 → ₹19
    EXPECT_EQ(buy.stt, 0);
    EXPECT_EQ(buy.gst, 740);  // 18% of ₹41.12
    const auto sell = leg(Segment::Future, Side::Sell, 9'600, 100, Exchange::Mcx);
    EXPECT_EQ(sell.stt, 9'600);  // CTT 0.01% = ₹96
    EXPECT_EQ(sell.stamp, 0);
    // An option sold at ₹200 × 100: CTT 0.05% of ₹20,000 = ₹10; MCX 0.0418% = ₹8.36.
    const auto option = leg(Segment::Option, Side::Sell, 200, 100, Exchange::Mcx);
    EXPECT_EQ(option.brokerage, 2'000);
    EXPECT_EQ(option.stt, 1'000);
    EXPECT_EQ(option.exchange, 836);
}

TEST(Charges, ExerciseStt) {
    EXPECT_EQ(exercise_stt(kRates, 750'000), 1'100);  // 0.15% of ₹7,500 = ₹11.25 → ₹11
    EXPECT_EQ(exercise_stt(kRates, 0), 0);
}

}  // namespace
}  // namespace mda::oms
