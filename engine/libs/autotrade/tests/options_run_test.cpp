#include <gtest/gtest.h>

#include <map>
#include <vector>

#include <mda/autotrade/options_run.hpp>

namespace mda::autotrade {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

constexpr std::int64_t kMidnightIst = 1'792'607'400;  // 2026-10-22 00:00 IST
constexpr std::int32_t kToday = 20'748;

constexpr Millis at(int hour, int minute, int second = 0) {
    return (kMidnightIst + hour * 3600 + minute * 60 + second) * 1000;
}
constexpr std::int64_t minute_of(int hour, int minute) {
    return kMidnightIst + hour * 3600 + minute * 60;
}

const Contract kCall{.id = core::InstrumentId(100), .lot_size = 65, .expiry_days = kToday + 5, .tick = rupees(0.05)};
const Contract kPut{.id = core::InstrumentId(101), .lot_size = 65, .expiry_days = kToday + 5, .tick = rupees(0.05)};

OptionsRunConfig config() {
    OptionsRunConfig c;
    c.id = "nifty-options";
    c.user = 1;
    c.signal = "oi-bias-v1";
    c.underlying = "NIFTY";
    return c;
}

SignalView signal(int hour, int minute, int score) {
    return {.minute = minute_of(hour, minute),
            .score = score,
            .label = score >= 25 ? signals::Label::Bullish : score <= -25 ? signals::Label::Bearish : signals::Label::Neutral};
}

broker::ExecutionReport report(std::uint64_t client, broker::ReportEvent event, oms::OrderStatus status,
                               std::int64_t order_id = 0) {
    broker::ExecutionReport r{};
    r.user = 1;
    r.client_order_id = client;
    r.order_id = order_id;
    r.event = event;
    r.status = status;
    return r;
}

broker::ExecutionReport fill(std::uint64_t client, oms::Side side, std::int64_t qty, double price,
                             std::int64_t order_id = 0, oms::OrderStatus status = oms::OrderStatus::Complete) {
    auto r = report(client, broker::ReportEvent::Fill, status, order_id);
    r.side = side;
    r.last_quantity = r.filled = r.quantity = qty;
    r.last_price = r.average_price = rupees(price);
    return r;
}

class OptionsRunTest : public ::testing::Test {
protected:
    OptionsRunTest() : run_(config()) { run_.set_options({.call = kCall, .put = kPut}); }

    /// The index at `spot`; every option quoted bid / ask around `premium`, all fresh at `now`.
    OptionsMarket market(double spot, Millis now) {
        return {.spot = rupees(spot), .spot_at = now, .feed_ok = true, .quote = [this, now](core::InstrumentId id) {
                    const auto it = premium_.find(id.value());
                    return it == premium_.end() ? std::optional<OptionQuote>{}
                                                : OptionQuote{.ltp = rupees(it->second),
                                                              .bid = rupees(it->second - 0.05),
                                                              .ask = rupees(it->second + 0.05),
                                                              .at = now};
                }};
    }

    /// Buys the call (filled at 120.10) on Bullish at 10:00 with the index at 24,000; the
    /// stop is acknowledged as order 7.
    void go_long() {
        premium_[100] = 120;
        run_.on_signal(signal(10, 0, 38));
        const auto enter = run_.evaluate(market(24'000, at(10, 1, 1)), at(10, 1, 1));
        ASSERT_EQ(enter.actions.size(), 1u);
        entry_client_ = enter.actions[0].client_order_id;
        (void)run_.on_report(report(entry_client_, broker::ReportEvent::Accepted, oms::OrderStatus::Open, 1), at(10, 1, 1));
        (void)run_.on_report(fill(entry_client_, oms::Side::Buy, 65, 120.10, 1), at(10, 1, 2));
        const auto stop = run_.evaluate(market(24'000, at(10, 1, 3)), at(10, 1, 3));
        ASSERT_EQ(stop.actions.size(), 1u);
        stop_ = stop.actions[0];
        stop_client_ = stop_.client_order_id;
        (void)run_.on_report(report(stop_client_, broker::ReportEvent::Accepted, oms::OrderStatus::TriggerPending, 7),
                             at(10, 1, 4));
        ASSERT_EQ(run_.state(), State::Long);
    }

    OptionsRun run_;
    std::map<std::uint32_t, double> premium_;
    std::uint64_t entry_client_ = 0;
    std::uint64_t stop_client_ = 0;
    Action stop_;
};

TEST_F(OptionsRunTest, BullishBuysTheCallAtTheAskPlusSlip) {
    premium_[100] = 120;
    run_.on_signal(signal(10, 0, 38));
    const auto out = run_.evaluate(market(24'000, at(10, 1, 1)), at(10, 1, 1));
    ASSERT_EQ(out.actions.size(), 1u);
    const auto& a = out.actions[0];
    EXPECT_EQ(a.instrument, kCall.id);
    EXPECT_EQ(a.side, oms::Side::Buy);
    EXPECT_EQ(a.type, oms::OrderType::Limit);
    EXPECT_EQ(a.quantity, 65);
    EXPECT_EQ(a.price, rupees(120.15));  // ask 120.05 + 2 ticks
    EXPECT_EQ(out.decisions[0].action, "buy call");
    EXPECT_EQ(run_.state(), State::Entering);
}

TEST_F(OptionsRunTest, BearishBuysThePut) {
    premium_[101] = 95;
    run_.on_signal(signal(10, 0, -40));
    const auto out = run_.evaluate(market(24'000, at(10, 1, 1)), at(10, 1, 1));
    ASSERT_EQ(out.actions.size(), 1u);
    EXPECT_EQ(out.actions[0].instrument, kPut.id);
    EXPECT_EQ(out.decisions[0].action, "buy put");
}

TEST_F(OptionsRunTest, FillPlacesAPremiumStop) {
    go_long();
    // 30% below 120.10 is 84.07: down to the tick 84.05, its limit 10 ticks below.
    const auto& s = stop_;
    EXPECT_EQ(s.side, oms::Side::Sell);
    EXPECT_EQ(s.type, oms::OrderType::StopLimit);
    EXPECT_EQ(s.trigger, rupees(84.05));
    EXPECT_EQ(s.price, rupees(83.55));
    EXPECT_EQ(s.quantity, 65);
}

TEST_F(OptionsRunTest, UnfilledEntryIsCancelledAfterTheTimeout) {
    premium_[100] = 120;
    run_.on_signal(signal(10, 0, 38));
    const auto enter = run_.evaluate(market(24'000, at(10, 1, 1)), at(10, 1, 1));
    const auto client = enter.actions[0].client_order_id;
    (void)run_.on_report(report(client, broker::ReportEvent::Accepted, oms::OrderStatus::Open, 3), at(10, 1, 1));
    EXPECT_TRUE(run_.evaluate(market(24'000, at(10, 1, 9)), at(10, 1, 9)).actions.empty());
    const auto out = run_.evaluate(market(24'000, at(10, 1, 11)), at(10, 1, 11));
    ASSERT_EQ(out.actions.size(), 1u);
    EXPECT_EQ(out.actions[0].kind, broker::Action::Cancel);
    EXPECT_EQ(out.actions[0].order_id, 3);
    EXPECT_TRUE(run_.evaluate(market(24'000, at(10, 1, 12)), at(10, 1, 12)).actions.empty());  // asked once

    const auto done = run_.on_report(report(client, broker::ReportEvent::Cancelled, oms::OrderStatus::Cancelled, 3), at(10, 1, 12));
    EXPECT_EQ(done.decisions.at(0).action, "entry not filled");
    EXPECT_EQ(run_.state(), State::Flat);
    EXPECT_FALSE(run_.held());
    EXPECT_EQ(run_.entries_today(), 0);  // not a trade
    // The same signal doesn't enter again.
    EXPECT_TRUE(run_.evaluate(market(24'000, at(10, 1, 20)), at(10, 1, 20)).actions.empty());
}

TEST_F(OptionsRunTest, IndexStopExitsAtTheBidMinusSlip) {
    go_long();
    premium_[100] = 100;
    EXPECT_TRUE(run_.evaluate(market(23'976, at(10, 5)), at(10, 5)).actions.empty());  // −24: not yet
    const auto out = run_.evaluate(market(23'975, at(10, 6)), at(10, 6));
    ASSERT_EQ(out.actions.size(), 2u);
    EXPECT_EQ(out.actions[0].kind, broker::Action::Cancel);
    EXPECT_EQ(out.actions[0].order_id, 7);
    const auto& sell = out.actions[1];
    EXPECT_EQ(sell.side, oms::Side::Sell);
    EXPECT_EQ(sell.type, oms::OrderType::Limit);
    EXPECT_EQ(sell.price, rupees(99.85));  // bid 99.95 − 2 ticks
    EXPECT_EQ(sell.quantity, 65);
    EXPECT_NE(out.decisions[0].reason.find("index stop"), std::string::npos);
    EXPECT_EQ(run_.state(), State::Exiting);

    (void)run_.on_report(report(stop_client_, broker::ReportEvent::Cancelled, oms::OrderStatus::Cancelled, 7), at(10, 6));
    const auto done = run_.on_report(fill(sell.client_order_id, oms::Side::Sell, 65, 99.85, 9), at(10, 6, 1));
    EXPECT_EQ(done.decisions.at(0).action, "flat");
    EXPECT_EQ(run_.state(), State::Flat);
    EXPECT_EQ(run_.realised_today(), -131'625);  // (99.85 − 120.10) × 65 in paise
}

TEST_F(OptionsRunTest, PutIsMeasuredTheOtherWay) {
    premium_[101] = 95;
    run_.on_signal(signal(10, 0, -40));
    const auto enter = run_.evaluate(market(24'000, at(10, 1, 1)), at(10, 1, 1));
    (void)run_.on_report(fill(enter.actions[0].client_order_id, oms::Side::Buy, 65, 95.10, 1), at(10, 1, 2));
    (void)run_.evaluate(market(24'000, at(10, 1, 3)), at(10, 1, 3));  // the stop
    run_.set_options({});                                              // the pair moving on doesn't matter
    EXPECT_TRUE(run_.evaluate(market(23'950, at(10, 5)), at(10, 5)).actions.empty());  // +50 for a put
    const auto out = run_.evaluate(market(24'025, at(10, 6)), at(10, 6));
    ASSERT_FALSE(out.actions.empty());
    EXPECT_EQ(out.actions.back().instrument, kPut.id);
    EXPECT_NE(out.decisions[0].reason.find("index stop"), std::string::npos);
}

TEST_F(OptionsRunTest, SignalChangeExits) {
    go_long();
    run_.on_signal(signal(10, 4, 12));  // Neutral, above exit_score 10: held
    EXPECT_TRUE(run_.evaluate(market(24'010, at(10, 5, 1)), at(10, 5, 1)).actions.empty());
    run_.on_signal(signal(10, 5, 5));
    const auto out = run_.evaluate(market(24'010, at(10, 6, 1)), at(10, 6, 1));
    ASSERT_FALSE(out.decisions.empty());
    EXPECT_NE(out.decisions[0].reason.find("signal changed"), std::string::npos);
}

TEST_F(OptionsRunTest, UnfilledExitIsRepricedWithTheWholeQuantity) {
    go_long();
    premium_[100] = 100;
    const auto out = run_.evaluate(market(23'970, at(10, 6)), at(10, 6));
    const auto exit_client = out.actions[1].client_order_id;
    (void)run_.on_report(report(exit_client, broker::ReportEvent::Accepted, oms::OrderStatus::Open, 9), at(10, 6));
    premium_[100] = 98;
    EXPECT_TRUE(run_.evaluate(market(23'965, at(10, 6, 2)), at(10, 6, 2)).actions.empty());
    const auto again = run_.evaluate(market(23'965, at(10, 6, 3)), at(10, 6, 3));
    ASSERT_EQ(again.actions.size(), 1u);
    EXPECT_EQ(again.actions[0].kind, broker::Action::Modify);
    EXPECT_EQ(again.actions[0].order_id, 9);
    EXPECT_EQ(again.actions[0].quantity, 65);
    EXPECT_EQ(again.actions[0].price, rupees(97.85));
}

TEST_F(OptionsRunTest, StopFillDuringExitCancelsTheExit) {
    go_long();
    premium_[100] = 80;
    const auto out = run_.evaluate(market(23'970, at(10, 6)), at(10, 6));
    const auto exit_client = out.actions[1].client_order_id;
    (void)run_.on_report(report(exit_client, broker::ReportEvent::Accepted, oms::OrderStatus::Open, 9), at(10, 6));
    // The stop triggered before our cancel landed.
    const auto stopped = run_.on_report(fill(stop_client_, oms::Side::Sell, 65, 84.00, 7), at(10, 6));
    ASSERT_EQ(stopped.actions.size(), 1u);
    EXPECT_EQ(stopped.actions[0].kind, broker::Action::Cancel);
    EXPECT_EQ(stopped.actions[0].order_id, 9);
    EXPECT_EQ(run_.position(), 0);

    const auto done = run_.on_report(report(exit_client, broker::ReportEvent::Cancelled, oms::OrderStatus::Cancelled, 9), at(10, 6, 1));
    EXPECT_EQ(done.decisions.at(0).action, "flat");
    EXPECT_EQ(run_.state(), State::Flat);
}

TEST_F(OptionsRunTest, ShortAfterARaceBuysBack) {
    go_long();
    premium_[100] = 80;
    const auto out = run_.evaluate(market(23'970, at(10, 6)), at(10, 6));
    const auto exit_client = out.actions[1].client_order_id;
    (void)run_.on_report(report(exit_client, broker::ReportEvent::Accepted, oms::OrderStatus::Open, 9), at(10, 6));
    (void)run_.on_report(fill(stop_client_, oms::Side::Sell, 65, 84.00, 7), at(10, 6));
    // Both filled: we're short 65.
    const auto both = run_.on_report(fill(exit_client, oms::Side::Sell, 65, 79.90, 9), at(10, 6));
    EXPECT_EQ(run_.position(), -65);
    EXPECT_EQ(both.decisions.at(0).action, "not flat");
    const auto back = run_.evaluate(market(23'970, at(10, 6, 1)), at(10, 6, 1));
    ASSERT_EQ(back.actions.size(), 1u);
    EXPECT_EQ(back.actions[0].side, oms::Side::Buy);
    EXPECT_EQ(back.actions[0].quantity, 65);
    EXPECT_EQ(back.actions[0].price, rupees(80.15));  // ask 80.05 + 2 ticks
}

TEST_F(OptionsRunTest, PremiumStopFillingMeansFlat) {
    go_long();
    const auto out = run_.on_report(fill(stop_client_, oms::Side::Sell, 65, 84.00, 7), at(10, 20));
    EXPECT_EQ(out.decisions.at(0).action, "stopped out");
    EXPECT_EQ(run_.state(), State::Flat);
    EXPECT_EQ(run_.last_exit(), at(10, 20));
}

TEST_F(OptionsRunTest, Guards) {
    premium_[100] = 120;
    run_.on_signal(signal(10, 0, 38));
    auto expiring = kCall;
    expiring.expiry_days = kToday;
    run_.set_options({.call = expiring});
    auto out = run_.evaluate(market(24'000, at(10, 1, 1)), at(10, 1, 1));
    EXPECT_TRUE(out.actions.empty());
    EXPECT_NE(out.decisions.at(0).reason.find("option expires today"), std::string::npos);

    run_.set_options({.put = kPut});
    out = run_.evaluate(market(24'000, at(10, 1, 2)), at(10, 1, 2));
    EXPECT_NE(out.decisions.at(0).reason.find("no option to buy"), std::string::npos);

    run_.set_options({.call = kCall});
    premium_.clear();
    out = run_.evaluate(market(24'000, at(10, 1, 3)), at(10, 1, 3));
    EXPECT_NE(out.decisions.at(0).reason.find("no fresh option quote"), std::string::npos);

    premium_[100] = 120;
    out = run_.evaluate(market(0, at(10, 1, 4)), at(10, 1, 4));
    EXPECT_NE(out.decisions.at(0).reason.find("no fresh index price"), std::string::npos);

    run_.set_kill_switch(true);
    out = run_.evaluate(market(24'000, at(10, 1, 5)), at(10, 1, 5));
    EXPECT_NE(out.decisions.at(0).reason.find("kill switch"), std::string::npos);
}

TEST_F(OptionsRunTest, RestoreWithoutEntrySpotMeasuresFromNow) {
    run_.restore(kCall, true, 65, rupees(120), at(10, 0), core::Price{}, 7, 42);
    EXPECT_EQ(run_.state(), State::Long);
    EXPECT_TRUE(run_.evaluate(market(24'100, at(10, 30)), at(10, 30)).actions.empty());
    EXPECT_EQ(run_.entry_spot(), rupees(24'100));
    const auto out = run_.evaluate(market(24'075, at(10, 31)), at(10, 31));
    ASSERT_EQ(out.actions.size(), 2u);
    EXPECT_EQ(out.actions[0].order_id, 7);  // the restored stop is cancelled
}

TEST(ChooseAtm, NearestExpiryAfterTodayAndNearestStrike) {
    const auto option = [](std::uint32_t id, std::int32_t expiry, double strike, bool call) {
        return ListedOption{.contract = {.id = core::InstrumentId(id), .lot_size = 65, .expiry_days = expiry, .tick = rupees(0.05)},
                            .strike = rupees(strike),
                            .call = call};
    };
    const std::vector<ListedOption> listed{
        option(1, kToday, 24'000, true),       option(2, kToday, 24'000, false),      // expire today
        option(3, kToday + 7, 23'950, true),   option(4, kToday + 7, 24'000, true),
        option(5, kToday + 7, 24'000, false),  option(6, kToday + 7, 24'050, false),
        option(7, kToday + 14, 24'000, true),  option(8, kToday + 14, 24'000, false),
    };
    auto pair = choose_atm(listed, rupees(24'020), kToday);
    ASSERT_TRUE(pair.call && pair.put);
    EXPECT_EQ(pair.call->id.value(), 4u);
    EXPECT_EQ(pair.put->id.value(), 5u);
    pair = choose_atm(listed, rupees(24'025), kToday);  // a tie: the lower strike
    EXPECT_EQ(pair.put->id.value(), 5u);
    pair = choose_atm(listed, rupees(24'040), kToday);
    EXPECT_EQ(pair.put->id.value(), 6u);
    EXPECT_FALSE(choose_atm({}, rupees(24'000), kToday).call);
}

TEST_F(OptionsRunTest, EndOfDayExits) {
    go_long();
    const auto out = run_.evaluate(market(24'000, at(15, 10)), at(15, 10));
    ASSERT_FALSE(out.decisions.empty());
    EXPECT_EQ(out.decisions[0].reason, "end of day");
}

}  // namespace
}  // namespace mda::autotrade
