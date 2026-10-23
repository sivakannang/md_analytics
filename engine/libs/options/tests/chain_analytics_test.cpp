#include <algorithm>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include <mda/options/black76.hpp>
#include <mda/options/chain_analytics.hpp>

namespace mda::options {
namespace {

using namespace std::chrono_literals;

constexpr std::int64_t kNow = 1'792'607'400 + 11 * 3600;  // 2026-10-22 11:00 IST
constexpr auto kExpiry = 2026y / 10 / 27;
constexpr double kForward = 22'660;
constexpr double kSigma = 0.15;

messages::MarketSnapshot snap(std::uint32_t id, double ltp, double prev_close, std::int64_t oi,
                              std::int64_t volume) {
    messages::MarketSnapshot s{};
    s.header = messages::make_header<messages::MarketSnapshot>(core::InstrumentId(id), 1);
    s.fields = messages::kFieldLtp | messages::kFieldOhlc | messages::kFieldVolume |
               messages::kFieldOpenInterest;
    s.ltp =
        core::Price::from_raw(static_cast<std::int64_t>(std::llround(ltp * core::Price::kScale)));
    s.close = core::Price::from_raw(
        static_cast<std::int64_t>(std::llround(prev_close * core::Price::kScale)));
    s.oi = core::Quantity(oi);
    s.volume = core::Quantity(volume);
    return s;
}

class ChainAnalyticsTest : public ::testing::Test {
protected:
    ChainAnalyticsTest() : analytics_(make()) {
        analytics_.on_snapshot(snap(1, 22'640, 22'700, 0, 0));  // spot (index): no volume, no OI
        const double years = years_to_expiry(kNow, kExpiry);
        std::uint32_t id = 100;
        for (const double strike : kStrikes) {
            for (const bool call : {true, false}) {
                const double price = black76_price(kForward, strike, years, kSigma, call);
                // Calls OI rises above ATM, puts below: writers' walls.
                const std::int64_t oi = call ? static_cast<std::int64_t>(strike - 22'000) * 100
                                             : static_cast<std::int64_t>(23'500 - strike) * 100;
                analytics_.on_snapshot(
                    snap(id, std::round(price * 20) / 20, price * 1.1, oi, 5'000));
                ++id;
            }
        }
    }

    static ChainAnalytics make() {
        ExpiryChain expiry{.expiry = kExpiry};
        std::uint32_t id = 100;
        for (const double strike : kStrikes) {
            for (const bool call : {true, false}) {
                expiry.options.push_back(
                    {.id = core::InstrumentId(id++),
                     .strike = core::Price::from_raw(static_cast<std::int64_t>(strike) *
                                                     core::Price::kScale),
                     .call = call});
            }
        }
        expiry.options.push_back({.id = core::InstrumentId(999),
                                  .strike = core::Price::from_raw(230'000'000),
                                  .call = true});  // no quote
        return ChainAnalytics(
            {{.underlying = "NIFTY", .spot = core::InstrumentId(1), .expiries = {expiry}}});
    }

    static constexpr double kStrikes[] = {22'400, 22'500, 22'600, 22'700, 22'800, 22'900};
    ChainAnalytics analytics_;
};

TEST_F(ChainAnalyticsTest, ImpliesTheForwardAndRecoversVolatility) {
    std::vector<OptionResult> options;
    std::vector<ChainSummary> summaries;
    analytics_.compute(kNow, options, summaries);

    ASSERT_EQ(summaries.size(), 1u);
    const ChainSummary& s = summaries[0];
    EXPECT_NEAR(s.forward, kForward, 0.1);  // parity, despite spot 22640 (the basis)
    EXPECT_EQ(s.atm_strike, 22'700);        // nearest the forward, not the spot
    EXPECT_NEAR(s.atm_iv, kSigma, 0.002);   // prices were rounded to the 0.05 tick
    EXPECT_NEAR(s.years, (5 * 86'400 + 4.5 * 3600) / (365.0 * 86'400), 1e-9);

    ASSERT_EQ(options.size(), 12u);  // the unquoted strike is skipped
    for (const auto& r : options) {
        ASSERT_TRUE(r.has_iv);
        EXPECT_NEAR(r.iv, kSigma, 0.01);
        EXPECT_GT(r.vega, 0);
        EXPECT_LT(r.theta, 0);
    }
    const auto call = std::ranges::find_if(
        options, [](const OptionResult& r) { return r.id == core::InstrumentId(106); });
    EXPECT_NEAR(call->delta, 0.4, 0.15);  // 22700 call, slightly out of the money
}

TEST_F(ChainAnalyticsTest, SummarisesOpenInterest) {
    std::vector<OptionResult> options;
    std::vector<ChainSummary> summaries;
    analytics_.compute(kNow, options, summaries);
    const ChainSummary& s = summaries[0];
    std::int64_t calls = 0, puts = 0;
    for (const double k : kStrikes) {
        calls += static_cast<std::int64_t>(k - 22'000) * 100;
        puts += static_cast<std::int64_t>(23'500 - k) * 100;
    }
    EXPECT_EQ(s.call_oi, calls);
    EXPECT_EQ(s.put_oi, puts);
    EXPECT_NEAR(s.pcr, static_cast<double>(puts) / static_cast<double>(calls), 1e-12);
    EXPECT_EQ(s.max_pain, max_pain({22'400, 22'500, 22'600, 22'700, 22'800, 22'900},
                                   {40'000, 50'000, 60'000, 70'000, 80'000, 90'000},
                                   {110'000, 100'000, 90'000, 80'000, 70'000, 60'000}));
}

TEST_F(ChainAnalyticsTest, ClassifiesBuildupAgainstYesterdaysOi) {
    analytics_.set_prev_oi(core::InstrumentId(100), 1);  // OI up, price down (ltp < close*1.1)
    analytics_.set_prev_oi(core::InstrumentId(101), 10'000'000);  // OI down, price down
    std::vector<OptionResult> options;
    std::vector<ChainSummary> summaries;
    analytics_.compute(kNow, options, summaries);
    const auto of = [&](std::uint32_t id) {
        return *std::ranges::find_if(
            options, [&](const OptionResult& r) { return r.id == core::InstrumentId(id); });
    };
    EXPECT_EQ(of(100).buildup, Buildup::ShortBuildup);
    EXPECT_EQ(of(101).buildup, Buildup::LongUnwinding);
    EXPECT_TRUE(of(100).has_prev_oi);
    EXPECT_EQ(of(102).buildup, Buildup::None);  // no previous OI
}

TEST(ChainAnalyticsUnits, ClassificationAndMaxPain) {
    EXPECT_EQ(classify(+1, +5), Buildup::LongBuildup);
    EXPECT_EQ(classify(-1, +5), Buildup::ShortBuildup);
    EXPECT_EQ(classify(+1, -5), Buildup::ShortCovering);
    EXPECT_EQ(classify(-1, -5), Buildup::LongUnwinding);
    EXPECT_EQ(classify(+1, 0), Buildup::None);
    // Heavy call OI at 100 and heavy put OI at 100: writers lose least at 100.
    EXPECT_EQ(max_pain({90, 100, 110}, {0, 1000, 0}, {0, 1000, 0}), 100);
}

TEST(ChainAnalyticsUnits, YearsToExpiryHasAFloor) {
    EXPECT_NEAR(years_to_expiry(kNow, kExpiry) * 365, 5.1875, 1e-9);
    const std::int64_t after_close = 1'792'607'400 + 5 * 86'400 + 16 * 3600;  // 27 Oct 16:00 IST
    EXPECT_NEAR(years_to_expiry(after_close, kExpiry), 3600.0 / (365.0 * 86'400), 1e-12);
}

TEST(ChainAnalyticsUnits, SummaryMessageRoundTrips) {
    const ChainSummary summary{.underlying = "BANKNIFTY",
                               .spot = core::InstrumentId(9),
                               .expiry = kExpiry,
                               .forward = 54'800.5,
                               .atm_strike = 54'800,
                               .atm_iv = 0.14,
                               .pcr = 0.8,
                               .max_pain = 55'000,
                               .years = 0.01,
                               .call_oi = 5,
                               .put_oi = 4};
    const ChainSummary back = from_message(to_message(summary));
    EXPECT_EQ(back.underlying, "BANKNIFTY");
    EXPECT_EQ(back.expiry, kExpiry);
    EXPECT_EQ(back.spot, core::InstrumentId(9));
    EXPECT_DOUBLE_EQ(back.forward, 54'800.5);
    EXPECT_EQ(back.put_oi, 4);
}

}  // namespace
}  // namespace mda::options
