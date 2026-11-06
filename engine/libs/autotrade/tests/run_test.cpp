#include <gtest/gtest.h>

#include <mda/autotrade/run.hpp>

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

const Contract kFuture{.id = core::InstrumentId(18486),
                       .lot_size = 65,
                       .expiry_days = kToday + 5,
                       .tick = rupees(0.10)};

RunConfig config() {
    RunConfig c;
    c.id = "nifty-follow";
    c.user = 1;
    c.signal = "oi-bias-v1";
    c.underlying = "NIFTY";
    return c;
}

Market market(double ltp, Millis now) {
    return {.ltp = rupees(ltp), .quote_at = now, .feed_ok = true};
}

SignalView signal(int hour, int minute, int score) {
    return {.minute = minute_of(hour, minute),
            .score = score,
            .label = score >= 25    ? signals::Label::Bullish
                     : score <= -25 ? signals::Label::Bearish
                                    : signals::Label::Neutral};
}

broker::ExecutionReport report(std::uint64_t client, broker::ReportEvent event,
                               oms::OrderStatus status, std::int64_t order_id = 0) {
    broker::ExecutionReport r{};
    r.user = 1;
    r.client_order_id = client;
    r.order_id = order_id;
    r.event = event;
    r.status = status;
    return r;
}

broker::ExecutionReport fill(std::uint64_t client, oms::Side side, std::int64_t qty, double price,
                             std::int64_t order_id = 0, oms::Paise charges = 0) {
    auto r = report(client, broker::ReportEvent::Fill, oms::OrderStatus::Complete, order_id);
    r.side = side;
    r.last_quantity = r.filled = r.quantity = qty;
    r.last_price = r.average_price = rupees(price);
    r.charges = charges;
    return r;
}

class RunTest : public ::testing::Test {
protected:
    RunTest() : run_(config()) { run_.set_contract(kFuture); }

    /// Enters long at `price` on a Bullish signal at 10:00, with the stop acknowledged as order 7.
    void go_long(double price = 22'600) {
        run_.on_signal(signal(10, 0, 38));
        const auto enter = run_.evaluate(market(price, at(10, 1, 1)), at(10, 1, 1));
        ASSERT_EQ(enter.actions.size(), 1u);
        (void)run_.on_report(
            fill(enter.actions[0].client_order_id, oms::Side::Buy, 65, price, 1, 9'600),
            at(10, 1, 2));
        const auto stop = run_.evaluate(market(price, at(10, 1, 3)), at(10, 1, 3));
        ASSERT_EQ(stop.actions.size(), 1u);
        stop_client_ = stop.actions[0].client_order_id;
        (void)run_.on_report(report(stop_client_, broker::ReportEvent::Accepted,
                                    oms::OrderStatus::TriggerPending, 7),
                             at(10, 1, 4));
        ASSERT_EQ(run_.state(), State::Long);
    }

    autotrade::Run run_;
    std::uint64_t stop_client_ = 0;
};

TEST_F(RunTest, EntersOnAFreshSignalAndPlacesAStop) {
    run_.on_signal(signal(10, 0, 38));
    const auto out = run_.evaluate(market(22'600, at(10, 1, 1)), at(10, 1, 1));
    ASSERT_EQ(out.actions.size(), 1u);
    const auto& a = out.actions[0];
    EXPECT_EQ(a.kind, broker::Action::New);
    EXPECT_EQ(a.side, oms::Side::Buy);
    EXPECT_EQ(a.type, oms::OrderType::Market);
    EXPECT_EQ(a.quantity, 65);
    EXPECT_EQ(a.instrument, kFuture.id);
    ASSERT_EQ(out.decisions.size(), 1u);
    EXPECT_EQ(out.decisions[0].action, "enter long");
    EXPECT_EQ(out.decisions[0].reason, "Bullish +38");
    EXPECT_EQ(run_.state(), State::Entering);
    EXPECT_TRUE(run_.evaluate(market(22'600, at(10, 1, 2)), at(10, 1, 2))
                    .actions.empty());  // waits for the fill

    (void)run_.on_report(fill(a.client_order_id, oms::Side::Buy, 65, 22'600.05), at(10, 1, 2));
    EXPECT_EQ(run_.state(), State::Long);
    const auto stop = run_.evaluate(market(22'600, at(10, 1, 3)), at(10, 1, 3));
    ASSERT_EQ(stop.actions.size(), 1u);
    EXPECT_EQ(stop.actions[0].type, oms::OrderType::StopMarket);
    EXPECT_EQ(stop.actions[0].side, oms::Side::Sell);
    EXPECT_EQ(stop.actions[0].trigger,
              rupees(22'560.0));  // 40 points below, on the 0.10 tick (away from the market)
    EXPECT_EQ(stop.decisions[0].action, "place stop");
}

TEST_F(RunTest, HoldsWhileTheScoreStaysAboveTheExitLevel) {
    go_long();
    run_.on_signal(signal(10, 5, 15));  // Neutral, but above exit_score 10
    EXPECT_TRUE(run_.evaluate(market(22'610, at(10, 6, 1)), at(10, 6, 1)).actions.empty());
    EXPECT_EQ(run_.state(), State::Long);
    run_.on_signal(signal(10, 6, 9));
    const auto out = run_.evaluate(market(22'610, at(10, 7, 1)), at(10, 7, 1));
    ASSERT_EQ(out.actions.size(), 2u);
    EXPECT_EQ(out.decisions[0].reason, "signal changed: Neutral +9");
}

TEST(SignalHolds, ExitLevelAndOppositeLabel) {
    const auto view = [](int score) {
        return SignalView{.score = score,
                          .label = score >= 25    ? signals::Label::Bullish
                                   : score <= -25 ? signals::Label::Bearish
                                                  : signals::Label::Neutral};
    };
    EXPECT_TRUE(signal_holds(view(30), true, 10));
    EXPECT_TRUE(signal_holds(view(10), true, 10));
    EXPECT_FALSE(signal_holds(view(9), true, 10));
    EXPECT_TRUE(signal_holds(view(-12), false, 10));
    EXPECT_FALSE(signal_holds(view(-5), false, 10));
    EXPECT_FALSE(signal_holds(view(-30), true, -50));  // an opposite label always closes
    EXPECT_FALSE(signal_holds(view(30), false, -50));
}

TEST_F(RunTest, ExitsWhenTheSignalTurns) {
    go_long();
    run_.on_signal(signal(10, 5, 5));
    const auto out = run_.evaluate(market(22'610, at(10, 6, 1)), at(10, 6, 1));
    ASSERT_EQ(out.actions.size(), 2u);
    EXPECT_EQ(out.actions[0].kind, broker::Action::Cancel);  // the stop first
    EXPECT_EQ(out.actions[0].order_id, 7);
    EXPECT_EQ(out.actions[1].side, oms::Side::Sell);
    EXPECT_EQ(out.actions[1].quantity, 65);
    EXPECT_EQ(out.decisions[0].reason, "signal changed: Neutral +5");
    (void)run_.on_report(
        fill(out.actions[1].client_order_id, oms::Side::Sell, 65, 22'610, 2, 9'700), at(10, 6, 2));
    EXPECT_EQ(run_.state(), State::Flat);
    EXPECT_EQ(run_.position(), 0);
    EXPECT_EQ(run_.pnl_today(), 65'000 - 9'600 - 9'700);  // ₹10 × 65 less both fills' charges
}

TEST_F(RunTest, CooldownAndOneEntryPerSignal) {
    go_long();
    run_.on_signal(signal(10, 5, 5));
    const auto exit = run_.evaluate(market(22'610, at(10, 6, 1)), at(10, 6, 1));
    (void)run_.on_report(fill(exit.actions[1].client_order_id, oms::Side::Sell, 65, 22'610),
                         at(10, 6, 2));

    run_.on_signal(signal(10, 8, 40));
    const auto blocked = run_.evaluate(market(22'610, at(10, 9, 1)), at(10, 9, 1));
    EXPECT_TRUE(blocked.actions.empty());
    ASSERT_EQ(blocked.decisions.size(), 1u);
    EXPECT_EQ(blocked.decisions[0].reason, "Bullish +40: cooldown");
    EXPECT_TRUE(run_.evaluate(market(22'610, at(10, 9, 2)), at(10, 9, 2))
                    .decisions.empty());  // recorded once

    run_.on_signal(signal(10, 15, 40));  // after the 10-minute cooldown
    const auto again = run_.evaluate(market(22'610, at(10, 16, 10)), at(10, 16, 10));
    ASSERT_EQ(again.actions.size(), 1u);
    EXPECT_EQ(run_.entries_today(), 2);
    // The same signal minute never opens a second position.
    (void)run_.on_report(report(again.actions[0].client_order_id, broker::ReportEvent::Cancelled,
                                oms::OrderStatus::Cancelled),
                         at(10, 16, 11));
    EXPECT_EQ(run_.state(), State::Flat);
    EXPECT_TRUE(run_.evaluate(market(22'610, at(10, 16, 20)), at(10, 16, 20)).actions.empty());
}

TEST_F(RunTest, Guards) {
    const auto reason = [&](autotrade::Run& run, const Market& m, Millis now) {
        const auto out = run.evaluate(m, now);
        EXPECT_TRUE(out.actions.empty());
        return out.decisions.empty() ? std::string() : out.decisions[0].reason;
    };
    run_.on_signal(signal(10, 0, 38));
    EXPECT_EQ(reason(run_, market(22'600, at(10, 4)), at(10, 4)), "Bullish +38: signal is stale");

    autotrade::Run early(config());
    early.set_contract(kFuture);
    early.on_signal(signal(9, 16, 38));
    EXPECT_EQ(reason(early, market(22'600, at(9, 17, 5)), at(9, 17, 5)),
              "Bullish +38: outside the entry window");

    autotrade::Run quiet(config());
    quiet.set_contract(kFuture);
    quiet.on_signal(signal(10, 0, -38));
    EXPECT_EQ(reason(quiet, {.ltp = rupees(22'600), .quote_at = at(10, 0, 50), .feed_ok = true},
                     at(10, 1, 5)),
              "Bearish -38: no fresh quote");

    autotrade::Run killed(config());
    killed.set_contract(kFuture);
    killed.set_kill_switch(true);
    killed.on_signal(signal(10, 0, 38));
    EXPECT_EQ(reason(killed, market(22'600, at(10, 1, 5)), at(10, 1, 5)),
              "Bullish +38: kill switch");

    autotrade::Run expiring(config());
    expiring.set_contract(
        {.id = kFuture.id, .lot_size = 65, .expiry_days = kToday, .tick = rupees(0.10)});
    expiring.on_signal(signal(10, 0, 38));
    EXPECT_EQ(reason(expiring, market(22'600, at(10, 1, 5)), at(10, 1, 5)),
              "Bullish +38: contract expires today");
}

TEST_F(RunTest, TradesPerDayAndDailyLossStop) {
    RunConfig c = config();
    c.max_trades_per_day = 1;
    autotrade::Run run(c);
    run.set_contract(kFuture);
    run.restore_day(kToday, 1, 0, 0, 0, 5);
    run.on_signal(signal(10, 0, 38));
    EXPECT_EQ(run.evaluate(market(22'600, at(10, 1, 5)), at(10, 1, 5)).decisions.at(0).reason,
              "Bullish +38: trades per day");

    autotrade::Run loser(config());
    loser.set_contract(kFuture);
    loser.restore_day(kToday, 1, -4'000 * 100, 1'000 * 100, 0, 5);  // −₹4,000 and ₹1,000 charges
    loser.on_signal(signal(10, 0, 38));
    EXPECT_EQ(loser.evaluate(market(22'600, at(10, 1, 5)), at(10, 1, 5)).decisions.at(0).reason,
              "Bullish +38: daily loss stop");
    // A new day starts fresh.
    loser.on_signal(signal(10, 0, 38));
    EXPECT_EQ(loser.evaluate(market(22'600, at(10, 1, 5) + 86'400'000), at(10, 1, 5) + 86'400'000)
                  .actions.size(),
              0u)
        << "yesterday's signal is stale today";
}

// A rejected entry (the broker's risk limits) isn't a trade: the day keeps its entries,
// and the next try waits the cooldown.
TEST_F(RunTest, ARejectedEntryIsNoTrade) {
    RunConfig c = config();
    c.max_trades_per_day = 1;
    autotrade::Run run(c);
    run.set_contract(kFuture);
    run.on_signal(signal(10, 0, 38));
    const auto enter = run.evaluate(market(22'600, at(10, 1, 1)), at(10, 1, 1));
    ASSERT_EQ(enter.actions.size(), 1u);
    const auto failed =
        run.on_report(report(enter.actions[0].client_order_id, broker::ReportEvent::Rejected,
                             oms::OrderStatus::Rejected),
                      at(10, 1, 1));
    EXPECT_EQ(failed.decisions.at(0).action, "entry failed");
    EXPECT_EQ(run.entries_today(), 0);
    run.on_signal(signal(10, 5, 38));
    EXPECT_EQ(run.evaluate(market(22'600, at(10, 6, 1)), at(10, 6, 1)).decisions.at(0).reason,
              "Bullish +38: cooldown");
    run.on_signal(signal(10, 11, 38));
    EXPECT_EQ(run.evaluate(market(22'600, at(10, 12, 1)), at(10, 12, 1)).actions.size(), 1u);
}

TEST_F(RunTest, TargetMaxHoldAndEndOfDay) {
    go_long();
    const auto target = run_.evaluate(market(22'680, at(10, 10)), at(10, 10));
    ASSERT_EQ(target.actions.size(), 2u);
    EXPECT_EQ(target.decisions[0].reason, "target: +80.00 points");

    autotrade::Run held(config());
    held.set_contract(kFuture);
    held.restore(65, rupees(22'600), at(10, 1), 7, 99);
    EXPECT_TRUE(held.evaluate(market(22'620, at(10, 45)), at(10, 45)).actions.empty());
    EXPECT_EQ(held.evaluate(market(22'620, at(10, 46)), at(10, 46)).decisions.at(0).reason,
              "max hold");

    autotrade::Run late(config());
    late.set_contract(kFuture);
    late.restore(-65, rupees(22'600), at(15, 5), 7, 99);
    EXPECT_EQ(late.evaluate(market(22'590, at(15, 10)), at(15, 10)).decisions.at(0).reason,
              "end of day");
    EXPECT_EQ(late.evaluate(market(22'590, at(15, 10)), at(15, 10)).actions.size(),
              0u);  // already exiting
}

TEST_F(RunTest, StoppedOut) {
    go_long();
    const auto out = run_.on_report(fill(stop_client_, oms::Side::Sell, 65, 22'560, 7), at(10, 20));
    EXPECT_EQ(out.decisions.at(0).action, "stopped out");
    EXPECT_EQ(run_.state(), State::Flat);
    EXPECT_EQ(run_.pnl_today(), -40 * 65 * 100 - 9'600);
}

TEST_F(RunTest, StopFillingDuringAnExitEndsFlatAnyway) {
    go_long();
    run_.on_signal(signal(10, 5, -30));
    const auto exit = run_.evaluate(market(22'565, at(10, 6, 1)), at(10, 6, 1));
    // The stop fills before our cancel arrives; then our market exit fills too: short 65.
    (void)run_.on_report(fill(stop_client_, oms::Side::Sell, 65, 22'560, 7), at(10, 6, 2));
    const auto turned = run_.on_report(
        fill(exit.actions[1].client_order_id, oms::Side::Sell, 65, 22'558), at(10, 6, 2));
    EXPECT_EQ(turned.decisions.at(0).action, "not flat");
    EXPECT_EQ(run_.position(), -65);
    EXPECT_TRUE(
        run_.evaluate(market(22'558, at(10, 6, 4)), at(10, 6, 4)).actions.empty());  // waits 5 s
    const auto fix = run_.evaluate(market(22'558, at(10, 6, 8)), at(10, 6, 8));
    ASSERT_EQ(fix.actions.size(), 1u);
    EXPECT_EQ(fix.actions[0].side, oms::Side::Buy);
    EXPECT_EQ(fix.actions[0].quantity, 65);
    (void)run_.on_report(fill(fix.actions[0].client_order_id, oms::Side::Buy, 65, 22'560),
                         at(10, 6, 9));
    EXPECT_EQ(run_.position(), 0);
    EXPECT_EQ(run_.state(), State::Flat);
}

TEST_F(RunTest, RejectedEntryAndLostStop) {
    run_.on_signal(signal(10, 0, 38));
    const auto enter = run_.evaluate(market(22'600, at(10, 1, 1)), at(10, 1, 1));
    auto rejected = report(enter.actions[0].client_order_id, broker::ReportEvent::Rejected,
                           oms::OrderStatus::Rejected);
    broker::set_text(rejected.reason, "the kill switch is on");
    const auto out = run_.on_report(rejected, at(10, 1, 2));
    EXPECT_EQ(run_.state(), State::Flat);
    EXPECT_EQ(out.decisions.at(0).reason, "the kill switch is on");

    autotrade::Run protectedless(config());
    protectedless.set_contract(kFuture);
    protectedless.restore(65, rupees(22'600), at(10, 1), 7, 55);
    auto expired = report(55, broker::ReportEvent::Cancelled, oms::OrderStatus::Cancelled, 7);
    broker::set_text(expired.reason, "expired at the close");
    const auto exit = protectedless.on_report(expired, at(10, 30));
    ASSERT_EQ(exit.actions.size(), 1u);  // sell at market: no position without a stop
    EXPECT_EQ(exit.actions[0].side, oms::Side::Sell);
    EXPECT_EQ(exit.decisions.at(0).reason, "stop lost: expired at the close");
}

TEST_F(RunTest, RestoredPositionGetsAStopAndAcknowledgedCancelFollows) {
    autotrade::Run restored(config());
    restored.set_contract(kFuture);
    restored.restore(-65, rupees(22'600), at(10, 1), 0, 0);
    const auto out = restored.evaluate(market(22'600, at(10, 2)), at(10, 2));
    ASSERT_EQ(out.actions.size(), 1u);
    EXPECT_EQ(out.actions[0].side, oms::Side::Buy);
    EXPECT_EQ(out.actions[0].trigger, rupees(22'640.0));
    // Exiting before the stop is acknowledged: its cancel goes out once it is.
    restored.on_signal(signal(10, 5, 30));
    const auto exit = restored.evaluate(market(22'600, at(10, 6, 1)), at(10, 6, 1));
    ASSERT_EQ(exit.actions.size(), 1u);  // only the market exit for now
    const auto ack =
        restored.on_report(report(out.actions[0].client_order_id, broker::ReportEvent::Accepted,
                                  oms::OrderStatus::TriggerPending, 12),
                           at(10, 6, 2));
    ASSERT_EQ(ack.actions.size(), 1u);
    EXPECT_EQ(ack.actions[0].kind, broker::Action::Cancel);
    EXPECT_EQ(ack.actions[0].order_id, 12);
}

TEST_F(RunTest, FailedExitsBackOffAndWaitForTheMarket) {
    autotrade::Run run(config());
    run.set_contract(kFuture);
    run.restore(-65, rupees(22'600), at(10, 1), 7, 99);
    run.on_signal(signal(10, 5, 5));
    auto exit = run.evaluate(market(22'600, at(10, 6)), at(10, 6));
    const auto reject = [&](std::uint64_t client, Millis now) {
        auto r = report(client, broker::ReportEvent::Cancelled, oms::OrderStatus::Cancelled);
        broker::set_text(r.reason, "no fresh market data");
        (void)run.on_report(r, now);
    };
    reject(exit.actions[1].client_order_id, at(10, 6, 1));
    // Retries after 5 s, then 10 s, then 20 s.
    EXPECT_TRUE(run.evaluate(market(22'600, at(10, 6, 5)), at(10, 6, 5)).actions.empty());
    auto retry = run.evaluate(market(22'600, at(10, 6, 6)), at(10, 6, 6));
    ASSERT_EQ(retry.actions.size(), 1u);
    reject(retry.actions[0].client_order_id, at(10, 6, 7));
    EXPECT_TRUE(run.evaluate(market(22'600, at(10, 6, 16)), at(10, 6, 16)).actions.empty());
    retry = run.evaluate(market(22'600, at(10, 6, 17)), at(10, 6, 17));
    ASSERT_EQ(retry.actions.size(), 1u);
    reject(retry.actions[0].client_order_id, at(10, 6, 18));
    EXPECT_TRUE(run.evaluate(market(22'600, at(10, 6, 37)), at(10, 6, 37)).actions.empty());
    EXPECT_EQ(run.evaluate(market(22'600, at(10, 6, 38)), at(10, 6, 38)).actions.size(), 1u);

    // After the close: no attempts at all until the next session.
    autotrade::Run late(config());
    late.set_contract(kFuture);
    late.restore(-65, rupees(22'600), at(15, 0), 0, 0);
    (void)late.evaluate(market(22'600, at(15, 5)), at(15, 5));               // places its stop
    const auto out = late.evaluate(market(22'600, at(15, 10)), at(15, 10));  // end of day: exit
    reject(out.actions.back().client_order_id, at(15, 10, 1));
    EXPECT_TRUE(late.evaluate(market(22'600, at(15, 31)), at(15, 31)).actions.empty());
    EXPECT_TRUE(late.evaluate(market(22'600, at(23, 0)), at(23, 0)).actions.empty());
}

TEST_F(RunTest, ReasonsNameTheFactorsBehindTheSignal) {
    SignalView bearish = signal(10, 0, -30);
    bearish.top = {{.factor = signals::FactorId::AtmOiShift, .points = -20},
                   {.factor = signals::FactorId::FutureVsVwap, .points = -15},
                   {.factor = signals::FactorId::SectorBreadth, .points = 5}};
    run_.on_signal(bearish);
    const auto enter = run_.evaluate(market(22'600, at(10, 1, 1)), at(10, 1, 1));
    EXPECT_EQ(enter.decisions.at(0).reason,
              "Bearish -30: OI shift -20, futures vs VWAP -15, sector breadth +5");
    (void)run_.on_report(fill(enter.actions[0].client_order_id, oms::Side::Sell, 65, 22'600),
                         at(10, 1, 2));
    (void)run_.evaluate(market(22'600, at(10, 1, 3)), at(10, 1, 3));  // its stop

    SignalView neutral = signal(10, 5, 4);
    neutral.top = {{.factor = signals::FactorId::PcrChange, .points = 8}};
    run_.on_signal(neutral);
    const auto exit = run_.evaluate(market(22'590, at(10, 6, 1)), at(10, 6, 1));
    EXPECT_EQ(exit.decisions.at(0).reason, "signal changed: Neutral +4: PCR change +8");
    EXPECT_EQ(factor_label(signals::FactorId::MaxPainPull), "max pain");
}

TEST(ChooseFuture, NearestNotExpiringToday) {
    const Contract futures[] = {{.id = core::InstrumentId(1), .expiry_days = kToday},
                                {.id = core::InstrumentId(2), .expiry_days = kToday + 35},
                                {.id = core::InstrumentId(3), .expiry_days = kToday + 7}};
    EXPECT_EQ(choose_future(futures, kToday)->id, core::InstrumentId(3));
    EXPECT_EQ(choose_future(futures, kToday - 1)->id, core::InstrumentId(1));
    EXPECT_FALSE(choose_future({}, kToday).has_value());
}

TEST(ClientOrderId, StableAndDistinct) {
    EXPECT_EQ(client_order_id("nifty-follow", kToday, 1),
              client_order_id("nifty-follow", kToday, 1));
    EXPECT_NE(client_order_id("nifty-follow", kToday, 1),
              client_order_id("nifty-follow", kToday, 2));
    EXPECT_NE(client_order_id("nifty-follow", kToday, 1),
              client_order_id("nifty-follow", kToday + 1, 1));
    EXPECT_NE(client_order_id("nifty-follow", kToday, 1),
              client_order_id("sensex-follow", kToday, 1));
    EXPECT_EQ(client_order_id("nifty-follow", kToday, 1) >> 63,
              0u);  // the broker's bit stays clear
}

}  // namespace
}  // namespace mda::autotrade
