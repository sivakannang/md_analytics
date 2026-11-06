#include <gtest/gtest.h>

#include <mda/signals/signal_store.hpp>

#include "test_support.hpp"

namespace mda::signals {
namespace {

using namespace test;

SignalRecord record(std::string underlying, std::int64_t minute, int score) {
    Signal signal{.spot = kSpot, .minute = minute, .strategy = StrategyId::OiBiasV1,
                  .horizon_min = 30, .score = score,
                  .label = score >= 25 ? Label::Bullish : Label::Neutral, .factors = {}};
    signal.factors.push_back({.id = FactorId::PcrChange, .status = FactorStatus::Present,
                              .value = 0.7616, .points = 11.42});
    signal.factors.push_back({.id = FactorId::MaxPainPull, .status = FactorStatus::NotApplicable});
    return {.underlying = std::move(underlying), .signal = std::move(signal), .spot_raw = 250'000'000};
}

TEST(SignalStore, FactorsAsJson) {
    EXPECT_EQ(factors_json(record("NIFTY", 0, 0).signal),
              R"([{"factor":"pcr_change","status":"present","value":0.7616,"points":11.42},)"
              R"({"factor":"max_pain_pull","status":"not_applicable","value":0.0000,"points":0.00}])");
}

TEST(SignalStore, WritesAndReadsBack) {
    auto store = SignalStore::open_in_memory();
    ASSERT_TRUE(store.has_value());
    const std::vector<SignalRecord> records = {record("NIFTY", at_min(1), 30), record("NIFTY", at_min(0), 10),
                                               record("SENSEX", at_min(0), -5)};
    ASSERT_TRUE(store->write(records).has_value());

    EXPECT_EQ(*store->latest_minute("NIFTY"), at_min(1));
    EXPECT_FALSE(store->latest_minute("BANKEX")->has_value());

    const auto rows = store->range("NIFTY", at_min(0), at_min(2));
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_EQ((*rows)[0].minute, at_min(0));
    EXPECT_EQ((*rows)[1].strategy, "oi-bias-v1");
    EXPECT_EQ((*rows)[1].score, 30);
    EXPECT_EQ((*rows)[1].label, Label::Bullish);
    EXPECT_EQ((*rows)[1].horizon_min, 30);
    EXPECT_EQ((*rows)[1].spot_raw, 250'000'000);
    EXPECT_FALSE((*rows)[1].prob_bp.has_value());
    EXPECT_FALSE((*rows)[1].hit.has_value());
    EXPECT_NE((*rows)[1].factors_json.find("pcr_change"), std::string::npos);
}

TEST(SignalStore, SameMinuteReplaces) {
    auto store = SignalStore::open_in_memory();
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE(store->write(std::vector{record("NIFTY", at_min(0), 10)}).has_value());
    ASSERT_TRUE(store->write(std::vector{record("NIFTY", at_min(0), 40)}).has_value());
    const auto rows = store->range("NIFTY", 0, at_min(10));
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ((*rows)[0].score, 40);
}

// A day of signals with hand-worked outcomes (30-minute horizon, 5 bp flat band).
TEST(SignalStore, OutcomesAndTrackRecord) {
    auto store = SignalStore::open_in_memory();
    ASSERT_TRUE(store.has_value());
    const auto raw = [](double price) { return std::llround(price * core::Price::kScale); };
    struct Case {
        std::int64_t minute;
        Label label;
        double spot;
        std::optional<double> after;  // empty: still open
    };
    const Case cases[] = {
        {at_min(0), Label::Bullish, 100, 101},     // hit; nothing a horizon earlier
        {at_min(30), Label::Bearish, 101, 100},    // hit; momentum (100 -> 101, up) wrong
        {at_min(45), Label::Neutral, 100.5, 100.5},
        {at_min(60), Label::Bullish, 100, 99},     // miss; momentum (101 -> 100, down) right
        {at_min(90), Label::Bearish, 99, 99.03},   // +3 bp: flat
        {at_min(120), Label::Bullish, 99, std::nullopt},
    };
    std::vector<SignalRecord> records;
    std::vector<OutcomeUpdate> outcomes;
    for (const auto& c : cases) {
        auto r = record("NIFTY", c.minute, 0);
        r.signal.label = c.label;
        r.spot_raw = raw(c.spot);
        records.push_back(r);
        if (c.after) {
            outcomes.push_back({.strategy = "oi-bias-v1", .underlying = "NIFTY", .minute = c.minute,
                                .outcome = outcome(c.label, raw(c.spot), raw(*c.after), 5)});
        }
    }
    ASSERT_TRUE(store->write(records).has_value());
    ASSERT_EQ(store->pending("NIFTY")->size(), 6U);
    ASSERT_TRUE(store->set_outcomes(outcomes).has_value());

    const auto open = store->pending("NIFTY");
    ASSERT_EQ(open->size(), 1U);
    EXPECT_EQ((*open)[0].minute, at_min(120));

    const auto rows = store->range("NIFTY", 0, at_min(200));
    EXPECT_EQ((*rows)[1].return_bp, -99);
    EXPECT_EQ((*rows)[1].hit, true);
    EXPECT_EQ((*rows)[4].return_bp, 3);
    EXPECT_FALSE((*rows)[4].hit.has_value());

    const auto record_rows = store->record("NIFTY", 0, at_min(200));
    ASSERT_TRUE(record_rows.has_value());
    ASSERT_EQ(record_rows->size(), 1U);
    const TrackRecord& r = (*record_rows)[0];
    EXPECT_EQ(r.strategy, "oi-bias-v1");
    EXPECT_EQ(r.horizon_min, 30);
    EXPECT_EQ(r.settled, 5);
    EXPECT_EQ(r.bullish, 2);
    EXPECT_EQ(r.bearish, 2);
    EXPECT_EQ(r.neutral, 1);
    EXPECT_EQ(r.hits, 2);
    EXPECT_EQ(r.misses, 1);
    EXPECT_EQ(r.flats, 1);
    EXPECT_EQ(r.up_moves, 1);
    EXPECT_EQ(r.momentum_hits, 1);
    EXPECT_EQ(r.momentum_calls, 2);

    EXPECT_TRUE(store->record("SENSEX", 0, at_min(200))->empty());
}

TEST(SignalStore, ReEvaluatedMinuteLosesItsOutcome) {
    auto store = SignalStore::open_in_memory();
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE(store->write(std::vector{record("NIFTY", at_min(0), 30)}).has_value());
    const OutcomeUpdate update{.strategy = "oi-bias-v1", .underlying = "NIFTY", .minute = at_min(0),
                               .outcome = outcome(Label::Bullish, 250'000'000, 251'000'000, 5)};
    ASSERT_TRUE(store->set_outcomes(std::span(&update, 1)).has_value());
    EXPECT_TRUE(store->pending("NIFTY")->empty());
    ASSERT_TRUE(store->write(std::vector{record("NIFTY", at_min(0), 30)}).has_value());
    EXPECT_EQ(store->pending("NIFTY")->size(), 1U);
}

}  // namespace
}  // namespace mda::signals
