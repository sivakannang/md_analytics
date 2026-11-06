#include <gtest/gtest.h>

#include <mda/signals/market_signal.hpp>

#include "test_support.hpp"

namespace mda::signals {
namespace {

using namespace test;

Signal sample() {
    Signal signal{.spot = kSpot, .minute = at_min(30), .strategy = StrategyId::OiBiasV1,
                  .horizon_min = 30, .score = 37, .label = Label::Bullish, .factors = {}};
    const double points[kFactorCount] = {20, 0, 11.4, -14.8, 3.2, 0.3, 6, 0, 0};  // F8, F9 at weight 0
    for (std::size_t i = 0; i < kFactorCount; ++i) {
        signal.factors.push_back({.id = static_cast<FactorId>(i + 1), .status = FactorStatus::Present,
                                  .value = points[i] / 20, .points = points[i]});
    }
    return signal;
}

TEST(MarketSignal, KeepsTheFourLargestFactors) {
    const auto message = to_message(sample(), 7);
    EXPECT_EQ(message.header.instrument_id, kSpot);
    EXPECT_EQ(message.header.sequence, 7U);
    EXPECT_EQ(message.minute, at_min(30));
    EXPECT_EQ(message.score, 37);
    EXPECT_EQ(label_of(message), Label::Bullish);
    EXPECT_EQ(message.horizon_min, 30);
    EXPECT_EQ(message.strategy, static_cast<std::uint16_t>(StrategyId::OiBiasV1));
    EXPECT_EQ(message.top[0].factor, static_cast<std::uint16_t>(FactorId::FuturesBuildup));
    EXPECT_EQ(message.top[0].points, 20);
    EXPECT_EQ(message.top[1].factor, static_cast<std::uint16_t>(FactorId::FutureVsVwap));
    EXPECT_EQ(message.top[1].points, -15);
    EXPECT_EQ(message.top[2].factor, static_cast<std::uint16_t>(FactorId::PcrChange));
    EXPECT_EQ(message.top[3].factor, static_cast<std::uint16_t>(FactorId::SectorBreadth));
}

TEST(MarketSignal, UnusedEntriesAreZero) {
    auto signal = sample();
    signal.factors.resize(1);
    signal.label = Label::Bearish;
    const auto message = to_message(signal, 1);
    EXPECT_EQ(label_of(message), Label::Bearish);
    signal.label = Label::Neutral;
    EXPECT_EQ(label_of(to_message(signal, 2)), Label::Neutral);
    EXPECT_EQ(message.top[0].points, 20);
    EXPECT_EQ(message.top[1].factor, 0);
}

TEST(MarketSignal, RoundTripsThroughDecode) {
    const auto message = to_message(sample(), 3);
    const auto decoded = messages::decode<MarketSignal>(
        messages::to_wire(messages::MessageType::MarketSignal), messages::as_bytes(message));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->score, 37);
}

}  // namespace
}  // namespace mda::signals
