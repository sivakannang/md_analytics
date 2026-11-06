#include <functional>
#include <initializer_list>
#include <utility>

#include <gtest/gtest.h>

#include <mda/broker/broker.hpp>

namespace mda::broker {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

constexpr std::int64_t kMidnightIst = 1'792'607'400;  // 2026-10-22 00:00 IST
constexpr std::int32_t kToday = 20'748;
constexpr Millis kT0 = (kMidnightIst + 10 * 3600) * 1000;  // 10:00 IST

constexpr core::InstrumentId kFut{1};
constexpr core::InstrumentId kCall{2};

std::optional<rms::InstrumentInfo> lookup(core::InstrumentId id) {
    if (id == kFut) {
        return rms::InstrumentInfo{
            .contract = {.id = kFut, .segment = oms::Segment::Future, .exchange = oms::Exchange::Nse, .lot_size = 75,
                         .strike = {}, .call = false},
            .tick = rupees(0.05), .freeze_quantity = 1800, .listed = true, .expiry_days = kToday + 5};
    }
    if (id == kCall) {
        return rms::InstrumentInfo{
            .contract = {.id = kCall, .segment = oms::Segment::Option, .exchange = oms::Exchange::Nse, .lot_size = 75,
                         .strike = rupees(22'500), .call = true},
            .tick = rupees(0.05), .freeze_quantity = 1800, .listed = true, .expiry_days = kToday + 5};
    }
    return std::nullopt;
}

messages::MarketSnapshot snapshot(core::InstrumentId id, double ltp, Millis at) {
    messages::MarketSnapshot s{};
    s.header.instrument_id = id;
    s.fields = messages::kFieldLtp;
    s.ltp = rupees(ltp);
    s.receive_ts = core::Timestamp::from_ns(at * 1'000'000);
    return s;
}

messages::DepthSnapshot depth(core::InstrumentId id, std::initializer_list<std::pair<double, std::uint32_t>> bids,
                              std::initializer_list<std::pair<double, std::uint32_t>> asks, Millis at) {
    messages::DepthSnapshot d{};
    d.header.instrument_id = id;
    d.receive_ts = core::Timestamp::from_ns(at * 1'000'000);
    std::size_t i = 0;
    for (const auto& [price, qty] : bids) {
        d.bids[i++] = {.price = rupees(price), .quantity = qty, .orders = 1};
    }
    i = 0;
    for (const auto& [price, qty] : asks) {
        d.asks[i++] = {.price = rupees(price), .quantity = qty, .orders = 1};
    }
    return d;
}

OrderRequest request(std::int64_t user, std::uint64_t client, core::InstrumentId id, oms::Side side,
                     oms::OrderType type, std::int64_t qty, double price = 0, double trigger = 0,
                     oms::Product product = oms::Product::Mis, Millis at = kT0) {
    OrderRequest r{};
    r.header = messages::make_header<OrderRequest>(id, client);
    r.at = at;
    r.user = user;
    r.client_order_id = client;
    r.price = rupees(price);
    r.trigger = rupees(trigger);
    r.quantity = qty;
    r.action = Action::New;
    r.side = side;
    r.type = type;
    r.product = product;
    r.source = oms::Source::Ui;
    return r;
}

OrderRequest control(std::int64_t user, Action action, std::int64_t order_id = 0, std::int64_t qty = 0,
                     double price = 0) {
    OrderRequest r{};
    r.header = messages::make_header<OrderRequest>({}, 0);
    r.user = user;
    r.client_order_id = 900 + static_cast<std::uint64_t>(action);
    r.action = action;
    r.order_id = order_id;
    r.quantity = qty;
    r.price = rupees(price);
    return r;
}

class BrokerTest : public ::testing::Test {
protected:
    BrokerTest() : store_(PaperStore::open_in_memory().value()), broker_({}, store_, lookup) {
        EXPECT_TRUE(broker_.load().has_value());
        market(kT0);
    }

    /// A normal market at `at`: future around 22,500, the call around 120.
    void market(Millis at) {
        broker_.on_snapshot(snapshot(kFut, 22'500, at));
        broker_.on_depth(depth(kFut, {{22'499.95, 300}}, {{22'500.05, 150}, {22'500.10, 300}}, at));
        broker_.on_snapshot(snapshot(kCall, 120, at));
        broker_.on_depth(depth(kCall, {{119.95, 600}}, {{120.05, 600}}, at));
    }

    Reports send(const OrderRequest& r, Millis now = kT0) {
        auto reports = broker_.on_request(r, now);
        EXPECT_TRUE(reports.has_value());
        return reports.value_or(Reports{});
    }

    Reports tick(Millis now) {
        auto reports = broker_.on_tick(now);
        EXPECT_TRUE(reports.has_value());
        return reports.value_or(Reports{});
    }

    PaperStore store_;
    Broker broker_;
};

TEST_F(BrokerTest, MarketOrderFillsAfterTheLatency) {
    const auto accepted = send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 75));
    ASSERT_EQ(accepted.size(), 1u);
    EXPECT_EQ(accepted[0].event, ReportEvent::Accepted);
    EXPECT_EQ(accepted[0].status, oms::OrderStatus::Open);
    EXPECT_GT(accepted[0].header.sequence, 0u);
    const std::int64_t id = accepted[0].order_id;

    EXPECT_TRUE(tick(kT0 + 50).empty());
    const auto filled = tick(kT0 + 100);
    ASSERT_EQ(filled.size(), 1u);
    EXPECT_EQ(filled[0].event, ReportEvent::Fill);
    EXPECT_EQ(filled[0].status, oms::OrderStatus::Complete);
    EXPECT_EQ(filled[0].last_quantity, 75);
    EXPECT_EQ(filled[0].last_price, rupees(22'500.05));
    EXPECT_GT(filled[0].charges, 0);
    EXPECT_EQ(broker_.live_order(id), nullptr);

    const auto* account = broker_.account_of_user(1);
    ASSERT_NE(account, nullptr);
    EXPECT_EQ(account->position(kFut, oms::Product::Mis)->quantity(), 75);
    EXPECT_EQ(account->cash(), 10'00'000 * 100 - filled[0].charges);

    const auto dump = store_.dump().value();
    EXPECT_NE(dump.find("[trades]\n" + std::to_string(id) + ",75,"), std::string::npos) << dump;
    EXPECT_EQ(store_.active_instruments().value(), std::vector<std::int64_t>{1});
}

// The auto trader's limits (₹40 L, 2 lots open) are on its own share: the user's own lots
// in the same future don't use them up.
TEST(BrokerAuto, UsersPositionsDontUseUpTheAutoLimits) {
    PaperStore store = PaperStore::open_in_memory().value();
    BrokerConfig config;
    config.opening_cash = 50'00'000 * 100;
    Broker broker(config, store, lookup);
    ASSERT_TRUE(broker.load(kT0).has_value());
    broker.on_snapshot(snapshot(kFut, 22'500, kT0));
    broker.on_depth(depth(kFut, {{22'499.95, 900}}, {{22'500.05, 900}}, kT0));
    const auto send = [&](OrderRequest r, oms::Source source, Millis at) {
        r.source = source;
        return broker.on_request(r, at).value().at(0);
    };
    // The user: 2 lots (₹33.75 L).
    EXPECT_EQ(send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 150), oms::Source::Ui, kT0).status,
              oms::OrderStatus::Open);
    ASSERT_EQ(broker.on_tick(kT0 + 100).value().size(), 1u);
    // The auto trader: 2 lots more, though the account already holds ₹33.75 L and 2 lots.
    EXPECT_EQ(send(request(1, 2, kFut, oms::Side::Buy, oms::OrderType::Market, 150), oms::Source::Auto, kT0 + 200)
                  .status,
              oms::OrderStatus::Open);
    ASSERT_EQ(broker.on_tick(kT0 + 300).value().size(), 1u);
    EXPECT_EQ(broker.account_of_user(1)->position(kFut, oms::Product::Mis)->quantity(), 300);
    // Its own 2 lots are its limit, and so after a restart.
    const auto third = send(request(1, 3, kFut, oms::Side::Buy, oms::OrderType::Market, 75), oms::Source::Auto, kT0 + 400);
    EXPECT_EQ(third.status, oms::OrderStatus::Rejected);
    Broker restarted(config, store, lookup);
    ASSERT_TRUE(restarted.load(kT0 + 500).has_value());
    restarted.on_snapshot(snapshot(kFut, 22'500, kT0 + 500));
    restarted.on_depth(depth(kFut, {{22'499.95, 900}}, {{22'500.05, 900}}, kT0 + 500));
    auto again = request(1, 4, kFut, oms::Side::Buy, oms::OrderType::Market, 75);
    again.source = oms::Source::Auto;
    EXPECT_EQ(restarted.on_request(again, kT0 + 600).value().at(0).status, oms::OrderStatus::Rejected);
}

TEST_F(BrokerTest, RejectionsAreReportedAndStored) {
    const auto r = send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 50));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].event, ReportEvent::Rejected);
    EXPECT_EQ(r[0].reject_code, static_cast<std::uint8_t>(rms::RejectCode::BadQuantity));
    EXPECT_EQ(text_of(r[0].reason), "quantity must be a multiple of the lot size (75)");
    EXPECT_EQ(broker_.live_orders(), 0u);
    const auto unknown = send(request(1, 2, core::InstrumentId(99), oms::Side::Buy, oms::OrderType::Market, 75));
    EXPECT_EQ(text_of(unknown[0].reason), "unknown instrument");
    // Stored as rejected (status 5).
    EXPECT_NE(store_.dump().value().find(",1,5,"), std::string::npos);
}

TEST_F(BrokerTest, RepeatedClientOrderIdGetsTheSameOrder) {
    const auto first = send(request(1, 7, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400));
    const auto again = send(request(1, 7, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400));
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].event, ReportEvent::Status);
    EXPECT_EQ(again[0].order_id, first[0].order_id);
    EXPECT_EQ(broker_.live_orders(), 1u);
    // After it is final the store answers.
    (void)send(control(1, Action::Cancel, first[0].order_id));
    const auto later = send(request(1, 7, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400));
    EXPECT_EQ(later[0].event, ReportEvent::Status);
    EXPECT_EQ(later[0].status, oms::OrderStatus::Cancelled);
}

TEST_F(BrokerTest, LimitRestsModifiesAndFillsWhenTradedThrough) {
    const auto placed = send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400));
    const std::int64_t id = placed[0].order_id;
    EXPECT_TRUE(tick(kT0 + 200).empty());  // first look: nothing at 22,400
    const auto modified = send(control(1, Action::Modify, id, 150, 22'450), kT0 + 300);
    ASSERT_EQ(modified[0].event, ReportEvent::Modified);
    EXPECT_EQ(modified[0].quantity, 150);
    EXPECT_TRUE(tick(kT0 + 400).empty());  // a fresh first look after the modify

    const Millis later = kT0 + 5'000;
    broker_.on_snapshot(snapshot(kFut, 22'445, later));
    broker_.on_depth(depth(kFut, {{22'444.95, 100}}, {{22'445.05, 100}}, later));
    const auto fill = tick(later + 10);
    ASSERT_EQ(fill.size(), 1u);
    EXPECT_EQ(fill[0].last_quantity, 150);
    EXPECT_EQ(fill[0].last_price, rupees(22'450));
}

TEST_F(BrokerTest, ModifyIsRiskChecked) {
    const auto placed = send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400));
    const auto r = send(control(1, Action::Modify, placed[0].order_id, 75 * 25, 22'400));  // above the freeze
    EXPECT_EQ(r[0].event, ReportEvent::Rejected);
    EXPECT_EQ(r[0].status, oms::OrderStatus::Open);  // the order itself is unchanged
    EXPECT_EQ(r[0].reject_code, static_cast<std::uint8_t>(rms::RejectCode::FreezeQuantity));
    EXPECT_EQ(broker_.live_order(placed[0].order_id)->spec().quantity, 75);
    EXPECT_EQ(send(control(2, Action::Cancel, placed[0].order_id))[0].event, ReportEvent::Rejected);  // not theirs
}

// Above the freeze quantity (1800) a person's order is sliced into legs, as Kite does, up
// to 10 of them.
TEST_F(BrokerTest, AnOrderAboveTheFreezeQuantityIsSliced) {
    const auto legs = send(request(1, 5, kCall, oms::Side::Buy, oms::OrderType::Limit, 75 * 30, 120));
    ASSERT_EQ(legs.size(), 2u);
    EXPECT_EQ(legs[0].event, ReportEvent::Accepted);
    EXPECT_EQ(legs[0].quantity, 1800);
    EXPECT_EQ(legs[0].client_order_id, 5u);
    EXPECT_EQ(legs[1].event, ReportEvent::Accepted);
    EXPECT_EQ(legs[1].quantity, 450);
    EXPECT_EQ(legs[1].client_order_id, 5u + (1ull << 52));
    EXPECT_EQ(broker_.live_order(legs[1].order_id)->spec().tag, "slice 2/2");
    EXPECT_EQ(broker_.live_orders(), 2u);

    const auto too_many = send(request(1, 6, kCall, oms::Side::Buy, oms::OrderType::Limit, 1800 * 10 + 75, 120));
    ASSERT_EQ(too_many.size(), 1u);
    EXPECT_EQ(too_many[0].event, ReportEvent::Rejected);
    EXPECT_EQ(too_many[0].reject_code, static_cast<std::uint8_t>(rms::RejectCode::FreezeQuantity));
    EXPECT_EQ(text_of(too_many[0].reason), "quantity above 10 legs of the freeze limit (10 × 1800)");
}

TEST_F(BrokerTest, KillSwitchCancelsAndBlocksOpening) {
    (void)send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400));
    const auto on = send(control(1, Action::KillSwitchOn));
    ASSERT_EQ(on.size(), 2u);
    EXPECT_EQ(on[0].event, ReportEvent::Cancelled);
    EXPECT_EQ(text_of(on[0].reason), "kill switch");
    EXPECT_EQ(on[1].event, ReportEvent::KillSwitchOn);
    EXPECT_TRUE(broker_.kill_switch(1));
    EXPECT_EQ(send(request(1, 2, kFut, oms::Side::Buy, oms::OrderType::Market, 75))[0].reject_code,
              static_cast<std::uint8_t>(rms::RejectCode::KillSwitch));
    (void)send(control(1, Action::KillSwitchOff));
    EXPECT_EQ(send(request(1, 3, kFut, oms::Side::Buy, oms::OrderType::Market, 75))[0].event, ReportEvent::Accepted);
}

TEST_F(BrokerTest, FeedDownCancelsAMarketOrder) {
    messages::FeedStatus down{};
    down.state = messages::FeedState::Disconnected;
    broker_.on_feed(down);
    (void)send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 75));
    const auto r = tick(kT0 + 200);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].event, ReportEvent::Cancelled);
    EXPECT_EQ(text_of(r[0].reason), "no fresh market data");
}

// A scripted session: steps of market data, requests and ticks.
using Step = std::function<void(Broker&)>;

std::vector<Step> session() {
    const auto md = [](core::InstrumentId id, double ltp, double bid, double ask, Millis at) -> Step {
        return [=](Broker& b) {
            b.on_snapshot(snapshot(id, ltp, at));
            b.on_depth(depth(id, {{bid, 300}}, {{ask, 300}}, at));
        };
    };
    const auto req = [](OrderRequest r, Millis at) -> Step {
        return [=](Broker& b) { ASSERT_TRUE(b.on_request(r, at).has_value()); };
    };
    const auto tick = [](Millis at) -> Step {
        return [=](Broker& b) { ASSERT_TRUE(b.on_tick(at).has_value()); };
    };
    using oms::OrderType;
    using oms::Side;
    return {
        md(kFut, 22'500, 22'499.95, 22'500.05, kT0),
        md(kCall, 120, 119.95, 120.05, kT0),
        req(request(1, 1, kFut, Side::Buy, OrderType::Market, 75), kT0 + 10),
        tick(kT0 + 200),
        req(request(1, 2, kFut, Side::Buy, OrderType::Limit, 75, 22'450, 0, oms::Product::Nrml), kT0 + 300),
        req(request(1, 3, kFut, Side::Sell, OrderType::StopMarket, 75, 0, 22'400), kT0 + 400),
        req(request(2, 1, kCall, Side::Buy, OrderType::Market, 150), kT0 + 500),
        tick(kT0 + 700),
        // ---- a restart may happen here ----
        md(kFut, 22'445, 22'444.95, 22'445.05, kT0 + 5'000),
        tick(kT0 + 5'100),
        md(kFut, 22'395, 22'394.95, 22'395.05, kT0 + 9'000),
        tick(kT0 + 9'100),
        tick(kT0 + 9'300),
        req(request(2, 2, kCall, Side::Sell, OrderType::Limit, 75, 121), kT0 + 9'400),
        md(kCall, 121.5, 121.45, 121.55, kT0 + 12'000),
        tick(kT0 + 12'100),
        tick(kT0 + 12'200),
    };
}
constexpr std::size_t kRestartAt = 8;

OrderRequest convert(std::int64_t user, std::uint64_t client, core::InstrumentId id, std::int64_t qty,
                     oms::Product to) {
    OrderRequest r{};
    r.header = messages::make_header<OrderRequest>(id, client);
    r.user = user;
    r.client_order_id = client;
    r.action = Action::Convert;
    r.quantity = qty;
    r.product = to;
    return r;
}

TEST_F(BrokerTest, AConvertedPositionMovesOnceAndEscapesTheSquareOff) {
    (void)send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 150));  // MIS
    (void)tick(kT0 + 200);
    const auto* account = broker_.account_of_user(1);
    ASSERT_EQ(account->position(kFut, oms::Product::Mis)->quantity(), 150);
    const oms::Paise cash = account->cash();

    auto r = send(convert(1, 50, kFut, 75, oms::Product::Nrml), kT0 + 300);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].event, ReportEvent::Converted) << text_of(r[0].reason);
    EXPECT_EQ(r[0].side, oms::Side::Buy);
    EXPECT_EQ(r[0].product, oms::Product::Nrml);
    r = send(convert(1, 50, kFut, 75, oms::Product::Nrml), kT0 + 800);  // the gateway's resend
    EXPECT_EQ(r.at(0).event, ReportEvent::Converted);
    EXPECT_EQ(account->position(kFut, oms::Product::Mis)->quantity(), 75);
    EXPECT_EQ(account->position(kFut, oms::Product::Nrml)->quantity(), 75);
    EXPECT_EQ(account->cash(), cash);  // no charges, no P&L

    r = send(convert(1, 51, kFut, 150, oms::Product::Nrml), kT0 + 900);
    EXPECT_EQ(r.at(0).event, ReportEvent::Rejected);
    EXPECT_EQ(text_of(r.at(0).reason), "the MIS position is 75 units");

    // At 15:25 only the MIS lot is squared off; the converted one carries.
    const Millis square_off = (kMidnightIst + 15 * 3600 + 25 * 60) * 1000;
    market(square_off);
    (void)tick(square_off);
    (void)tick(square_off + 200);
    EXPECT_EQ(account->position(kFut, oms::Product::Mis)->quantity(), 0);
    EXPECT_EQ(account->position(kFut, oms::Product::Nrml)->quantity(), 75);
    r = send(convert(1, 52, kFut, 75, oms::Product::Mis), square_off + 300);
    EXPECT_EQ(r.at(0).reject_code, static_cast<std::uint8_t>(rms::RejectCode::OutsideWindow));
}

TEST(BrokerSession, SameInputsSameState) {
    auto a = PaperStore::open_in_memory().value();
    auto b = PaperStore::open_in_memory().value();
    for (auto* store : {&a, &b}) {
        Broker broker({}, *store, lookup);
        ASSERT_TRUE(broker.load().has_value());
        for (const auto& step : session()) {
            step(broker);
        }
    }
    const auto dump = a.dump().value();
    EXPECT_EQ(dump, b.dump().value());
    // The session traded five times: the market buy at the ask, the limit when traded
    // through (at its price), the stop at the bid once triggered, the call bought at its ask,
    // and half of it sold by a limit at 121 whose first look (after its latency) found the
    // bid already at 121.45: marketable, so it takes the better price.
    const auto start = dump.find("[trades]\n") + 9;
    const auto trades = dump.substr(start, dump.find("[positions]") - start);
    EXPECT_EQ(std::count(trades.begin(), trades.end(), '\n'), 5) << trades;
    for (const char* price : {",225000500,", ",224500000,", ",223949500,", ",1200500,", ",1214500,"}) {
        EXPECT_NE(trades.find(price), std::string::npos) << price << "\n" << trades;
    }
}

TEST(BrokerSession, RestartResumesExactly) {
    auto whole = PaperStore::open_in_memory().value();
    {
        Broker broker({}, whole, lookup);
        ASSERT_TRUE(broker.load().has_value());
        for (const auto& step : session()) {
            step(broker);
        }
    }
    auto restarted = PaperStore::open_in_memory().value();
    const auto steps = session();
    {
        Broker before({}, restarted, lookup);
        ASSERT_TRUE(before.load().has_value());
        for (std::size_t i = 0; i < kRestartAt; ++i) {
            steps[i](before);
        }
        EXPECT_EQ(before.live_orders(), 2u);  // the resting limit and the stop
    }
    Broker after({}, restarted, lookup);
    ASSERT_TRUE(after.load().has_value());
    EXPECT_EQ(after.live_orders(), 2u);
    // Market data comes again from the bus after a restart.
    after.on_snapshot(snapshot(kCall, 120, kT0 + 4'000));
    after.on_depth(depth(kCall, {{119.95, 300}}, {{120.05, 300}}, kT0 + 4'000));
    for (std::size_t i = kRestartAt; i < steps.size(); ++i) {
        steps[i](after);
    }
    EXPECT_EQ(whole.dump().value(), restarted.dump().value());
}

// paper.db lost its accounts rows on 2026-10-27: the next order opened a second account over
// the old ledger and the cash showed both openings. Loading puts the row back instead.
TEST(BrokerSession, ALostAccountRowIsRebuiltNotReplaced) {
    auto store = PaperStore::open_in_memory().value();
    {
        Broker broker({}, store, lookup);
        ASSERT_TRUE(broker.load().has_value());
        broker.on_snapshot(snapshot(kFut, 22'500, kT0));
        broker.on_depth(depth(kFut, {{22'499.95, 300}}, {{22'500.05, 300}}, kT0));
        ASSERT_TRUE(broker.on_request(request(7, 1, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400), kT0));
    }
    ASSERT_TRUE(store.database().prepare("DELETE FROM accounts")->run().has_value());
    EXPECT_TRUE(store.accounts().value().empty());

    Broker restarted({}, store, lookup);
    ASSERT_TRUE(restarted.load().has_value());
    EXPECT_EQ(restarted.restored_accounts(), 1u);
    const auto* account = restarted.account_of_user(7);
    ASSERT_NE(account, nullptr);
    EXPECT_EQ(account->id(), 1);
    EXPECT_EQ(account->cash(), 10'00'000 * 100);  // one opening, not two
    EXPECT_EQ(store.accounts().value().at(0).user_id, 7);
}

// ---------------------------------------------------------------- close of day

constexpr Millis at_ist(int hour, int minute, int second = 0) {
    return (kMidnightIst + hour * 3600 + minute * 60 + second) * 1000;
}

constexpr core::InstrumentId kSpot{99};

/// Like lookup(), but the call expires today and both contracts settle on kSpot.
std::optional<rms::InstrumentInfo> expiring_lookup(core::InstrumentId id) {
    auto info = lookup(id);
    if (info) {
        info->underlying = kSpot;
        if (id == kCall) {
            info->expiry_days = kToday;
        }
    }
    return info;
}

class CloseOfDayTest : public ::testing::Test {
protected:
    CloseOfDayTest() : store_(PaperStore::open_in_memory().value()) { restart(); }

    void restart() {
        broker_.emplace(BrokerConfig{}, store_, expiring_lookup,
                        [this](core::InstrumentId underlying, std::int32_t day) -> std::optional<core::Price> {
                            if (underlying == kSpot && day == kToday && fsp_) {
                                return rupees(*fsp_);
                            }
                            return std::nullopt;
                        });
        ASSERT_TRUE(broker_->load().has_value());
    }
    void market(Millis at) {
        broker_->on_snapshot(snapshot(kFut, 22'500, at));
        broker_->on_depth(depth(kFut, {{22'499.95, 300}}, {{22'500.05, 300}}, at));
        broker_->on_snapshot(snapshot(kCall, 120, at));
        broker_->on_depth(depth(kCall, {{119.95, 600}}, {{120.05, 600}}, at));
    }
    Reports send(const OrderRequest& r, Millis at) {
        return broker_->on_request(r, at).value_or(Reports{});
    }
    Reports tick(Millis at) { return broker_->on_tick(at).value_or(Reports{}); }
    std::int64_t held(core::InstrumentId id, oms::Product product) {
        const auto* p = broker_->account_of_user(1)->position(id, product);
        return p == nullptr ? 0 : p->quantity();
    }

    PaperStore store_;
    std::optional<Broker> broker_;
    std::optional<double> fsp_;
};

TEST_F(CloseOfDayTest, MisIsSquaredOffOnceEvenAcrossARestart) {
    const Millis t0 = at_ist(10, 0);
    market(t0);
    (void)send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Mis), t0);
    (void)send(request(1, 2, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0);
    (void)tick(t0 + 200);
    // A resting MIS order (on the call: a third futures lot would pass the notional cap).
    const auto limit = send(request(1, 3, kCall, oms::Side::Buy, oms::OrderType::Limit, 75, 115, 0, oms::Product::Mis), t0 + 300);
    ASSERT_EQ(limit.at(0).event, ReportEvent::Accepted) << text_of(limit.at(0).reason);
    ASSERT_EQ(held(kFut, oms::Product::Mis), 75);

    EXPECT_TRUE(tick(at_ist(15, 24, 59)).empty());
    const Millis square_off = at_ist(15, 25);
    market(square_off);
    const auto reports = tick(square_off);
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].event, ReportEvent::Cancelled);
    EXPECT_EQ(text_of(reports[0].reason), "auto square-off");
    EXPECT_EQ(reports[1].event, ReportEvent::Accepted);
    EXPECT_EQ(reports[1].side, oms::Side::Sell);
    EXPECT_EQ(reports[1].source, oms::Source::System);

    restart();  // before the closing order fills
    market(square_off + 30'000);
    EXPECT_EQ(broker_->live_orders(), 1u);  // the closing order, restored
    (void)tick(square_off + 30'000);        // square-off runs again: no second order, this one fills
    EXPECT_EQ(held(kFut, oms::Product::Mis), 0);
    EXPECT_EQ(held(kFut, oms::Product::Nrml), 75);  // NRML carries over
    // One closing order in all: id 4, from the system, complete.
    const auto dump = store_.dump().value();
    EXPECT_NE(dump.find("\n4,1,"), std::string::npos) << dump;
    EXPECT_EQ(dump.find("\n5,1,"), std::string::npos) << dump;
}

TEST_F(CloseOfDayTest, DayOrdersExpireAtTheClose) {
    const Millis t0 = at_ist(10, 0);
    market(t0);
    (void)send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Limit, 75, 22'400, 0, oms::Product::Nrml), t0);
    (void)tick(t0 + 200);
    EXPECT_TRUE(tick(at_ist(15, 29, 59)).empty());
    const auto reports = tick(at_ist(15, 30));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, oms::OrderStatus::Cancelled);
    EXPECT_EQ(text_of(reports[0].reason), "expired at the close");
    EXPECT_EQ(broker_->live_orders(), 0u);
}

TEST_F(CloseOfDayTest, ExpiringOptionSettlesAtTheIndexClose) {
    const Millis t0 = at_ist(10, 0);
    market(t0);
    (void)send(request(1, 1, kCall, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0);
    (void)tick(t0 + 200);  // bought at 120.05
    const oms::Paise before = broker_->account_of_user(1)->cash();

    fsp_ = 22'600;  // the index closes at 22,600: the 22,500 call is worth 100
    EXPECT_TRUE(tick(at_ist(15, 30, 30)).empty());  // not before 15:31
    const auto reports = tick(at_ist(15, 31, 30));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].event, ReportEvent::Settled);
    EXPECT_EQ(reports[0].last_quantity, 75);
    EXPECT_EQ(reports[0].last_price, rupees(22'600));
    EXPECT_EQ(held(kCall, oms::Product::Nrml), 0);
    // (100 − 120.05) × 75 = −₹1,503.75, and exercise STT 0.15% of ₹7,500 = ₹11.25 → ₹11.
    EXPECT_EQ(broker_->account_of_user(1)->cash(), before - 150'375 - 1'100);
    EXPECT_EQ(broker_->account_of_user(1)->ledger().back().kind, oms::LedgerKind::Expiry);

    // Settled once: later checks find nothing.
    EXPECT_TRUE(tick(at_ist(15, 33)).empty());
}

TEST_F(CloseOfDayTest, SettlementWaitsForTheClosingPrice) {
    const Millis t0 = at_ist(10, 0);
    market(t0);
    (void)send(request(1, 1, kCall, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0);
    (void)tick(t0 + 200);
    EXPECT_TRUE(tick(at_ist(15, 31, 10)).empty());  // no price yet
    fsp_ = 22'400;                                   // out of the money: worthless
    EXPECT_TRUE(tick(at_ist(15, 31, 50)).empty());  // checked once a minute
    const auto reports = tick(at_ist(15, 32, 15));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(held(kCall, oms::Product::Nrml), 0);
}

TEST_F(CloseOfDayTest, DailySummary) {
    const Millis t0 = at_ist(10, 0);
    market(t0);
    (void)send(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0);
    (void)tick(t0 + 200);
    (void)send(request(1, 2, kFut, oms::Side::Sell, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0 + 300);
    (void)tick(t0 + 500);
    const auto summaries = broker_->daily_summaries(at_ist(15, 35));
    ASSERT_EQ(summaries.size(), 1u);
    EXPECT_EQ(summaries[0].user, 1);
    EXPECT_EQ(summaries[0].fills, 2);
    EXPECT_EQ(summaries[0].realised, -750);  // bought at the ask, sold at the bid: −₹0.10 × 75
    EXPECT_GT(summaries[0].charges, 0);
    EXPECT_EQ(summaries[0].open_positions, 0);
    EXPECT_TRUE(broker_->daily_summaries(at_ist(15, 35) + 86'400'000).empty());  // nothing the next day
}

}  // namespace
}  // namespace mda::broker

namespace mda::broker {
namespace {

// MCX (ADR-0031): crude oil in barrels against depth Kite gives in lots, traded in the
// evening, squared off and settled at MCX's close (23:30 on 2026-10-22).
constexpr core::InstrumentId kCrude{10};      // the November future
constexpr core::InstrumentId kCrudeCall{11};  // a 9,500 call on it, expiring today

std::optional<rms::InstrumentInfo> mcx_lookup(core::InstrumentId id) {
    if (id != kCrude && id != kCrudeCall) {
        return std::nullopt;
    }
    const bool call = id == kCrudeCall;
    return rms::InstrumentInfo{
        .contract = {.id = id, .segment = call ? oms::Segment::Option : oms::Segment::Future,
                     .exchange = oms::Exchange::Mcx, .lot_size = 100, .strike = call ? rupees(9'500) : core::Price{},
                     .call = call},
        .tick = rupees(call ? 0.1 : 1), .listed = true, .expiry_days = call ? kToday : kToday + 28,
        .underlying = call ? kCrude : core::InstrumentId{}, .futures_margin = 320'000'000,
        .market_open = 9 * 3600, .market_close = 23 * 3600 + 30 * 60};
}

TEST(BrokerMcx, BarrelsFillAgainstLotsAndTheDayClosesAtMcxsClose) {
    PaperStore store = PaperStore::open_in_memory().value();
    std::optional<double> fsp;
    Broker broker({}, store, mcx_lookup, [&](core::InstrumentId underlying, std::int32_t day) -> std::optional<core::Price> {
        return underlying == kCrude && day == kToday && fsp ? std::optional(rupees(*fsp)) : std::nullopt;
    });
    ASSERT_TRUE(broker.load().has_value());
    const auto market = [&](Millis at) {
        broker.on_snapshot(snapshot(kCrude, 9'600, at));
        broker.on_depth(depth(kCrude, {{9'599, 3}}, {{9'601, 1}, {9'602, 5}}, at));  // lots
        broker.on_snapshot(snapshot(kCrudeCall, 150, at));
        broker.on_depth(depth(kCrudeCall, {{149.9, 4}}, {{150.1, 4}}, at));
    };
    const auto held = [&](core::InstrumentId id, oms::Product product) {
        const auto* p = broker.account_of_user(1)->position(id, product);
        return p == nullptr ? 0 : p->quantity();
    };
    const Millis t0 = at_ist(21, 0);  // NSE has closed; MCX trades
    market(t0);
    auto r = broker.on_request(request(1, 1, kCrude, oms::Side::Buy, oms::OrderType::Market, 200, 0, 0,
                                       oms::Product::Mis, t0), t0).value();
    ASSERT_EQ(r.at(0).event, ReportEvent::Accepted) << text_of(r.at(0).reason);
    r = broker.on_request(request(1, 2, kCrudeCall, oms::Side::Buy, oms::OrderType::Market, 100, 0, 0,
                                  oms::Product::Nrml, t0), t0).value();
    ASSERT_EQ(r.at(0).event, ReportEvent::Accepted) << text_of(r.at(0).reason);
    (void)broker.on_tick(t0 + 200);
    ASSERT_EQ(held(kCrude, oms::Product::Mis), 200);
    // 1 lot shown at 9,601 is 100 barrels: 100 there and 100 at 9,602 (as lots, 1 + 5 barrels
    // would leave 194 to fill ticks worse).
    EXPECT_EQ(broker.account_of_user(1)->position(kCrude, oms::Product::Mis)->average_price(), rupees(9'601.5));

    // MIS: not at NSE's 15:25 (already past), but 5 minutes before MCX's close.
    EXPECT_TRUE(broker.on_tick(at_ist(23, 24, 59)).value().empty());
    market(at_ist(23, 25));
    r = broker.on_tick(at_ist(23, 25)).value();
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].source, oms::Source::System);
    EXPECT_EQ(r[0].side, oms::Side::Sell);
    (void)broker.on_tick(at_ist(23, 25) + 200);
    EXPECT_EQ(held(kCrude, oms::Product::Mis), 0);

    // The call settles in cash on its future's close, a minute after MCX's: (9,650 − 9,500) =
    // 150 against 150.10 paid, × 100 = −₹10, no exercise tax.
    const oms::Paise before = broker.account_of_user(1)->cash();
    fsp = 9'650;
    EXPECT_TRUE(broker.on_tick(at_ist(23, 30, 30)).value().empty());
    r = broker.on_tick(at_ist(23, 31, 30)).value();
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].event, ReportEvent::Settled);
    EXPECT_EQ(held(kCrudeCall, oms::Product::Nrml), 0);
    EXPECT_EQ(broker.account_of_user(1)->cash(), before - 1'000);
}

}  // namespace
}  // namespace mda::broker

namespace mda::broker {
namespace {

// Zerodha's daily MTM: an NRML future carried past the close settles against its own close at
// 15:41 (after F&O's last print) and carries on from it; options aren't marked.
TEST(BrokerMtm, ACarriedFutureIsMarkedToTheCloseOnce) {
    PaperStore store = PaperStore::open_in_memory().value();
    std::optional<double> close;
    const auto fsp = [&](core::InstrumentId id, std::int32_t day) -> std::optional<core::Price> {
        return id == kFut && day == kToday && close ? std::optional(rupees(*close)) : std::nullopt;
    };
    std::optional<Broker> broker;
    const auto start = [&] {
        broker.emplace(BrokerConfig{}, store, lookup, fsp);
        ASSERT_TRUE(broker->load().has_value());
    };
    start();
    const Millis t0 = at_ist(10, 0);
    broker->on_snapshot(snapshot(kFut, 22'500, t0));
    broker->on_depth(depth(kFut, {{22'499.95, 300}}, {{22'500.05, 300}}, t0));
    broker->on_snapshot(snapshot(kCall, 120, t0));
    broker->on_depth(depth(kCall, {{119.95, 600}}, {{120.05, 600}}, t0));
    (void)broker->on_request(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0);
    (void)broker->on_request(request(1, 2, kCall, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0);
    (void)broker->on_tick(t0 + 200);  // the future at 22,500.05, the call at 120.05
    const oms::Paise cash = broker->account_of_user(1)->cash();

    close = 22'560.05;
    (void)broker->on_tick(at_ist(15, 40, 30));  // not before 15:41
    EXPECT_EQ(broker->account_of_user(1)->cash(), cash);
    (void)broker->on_tick(at_ist(15, 41, 30));
    EXPECT_EQ(broker->account_of_user(1)->cash(), cash + 450'000);  // ₹60 × 75
    EXPECT_EQ(broker->account_of_user(1)->ledger().back().kind, oms::LedgerKind::Mtm);
    EXPECT_EQ(broker->account_of_user(1)->position(kFut, oms::Product::Nrml)->average_price(), rupees(22'560.05));
    EXPECT_EQ(broker->account_of_user(1)->position(kCall, oms::Product::Nrml)->average_price(), rupees(120.05));

    start();  // a restart the same evening marks nothing twice
    (void)broker->on_tick(at_ist(16, 0));
    EXPECT_EQ(broker->account_of_user(1)->cash(), cash + 450'000);
    EXPECT_EQ(broker->account_of_user(1)->position(kFut, oms::Product::Nrml)->average_price(), rupees(22'560.05));
}

}  // namespace
}  // namespace mda::broker

namespace mda::broker {
namespace {

// A close of day missed while the broker was down (the Mac was off): on the next start, MIS
// left open is closed at that day's close, once. The carried future waits for Kite's close.
TEST(BrokerCatchUp, AMissedCloseOfDayIsCaughtUpOnTheNextStart) {
    PaperStore store = PaperStore::open_in_memory().value();
    const Millis day1_close = at_ist(15, 40);  // the future's last minute ended
    const auto previous = [&](core::InstrumentId id, Millis before) -> std::optional<PreviousClose> {
        return id == kFut && before > day1_close ? std::optional(PreviousClose{.close = rupees(22'540), .ended = day1_close})
                                                 : std::nullopt;
    };
    std::optional<Broker> broker;
    const auto start = [&] {
        broker.emplace(BrokerConfig{}, store, lookup, FspLookup{}, previous);
        ASSERT_TRUE(broker->load().has_value());
    };
    start();
    const Millis t0 = at_ist(10, 0);
    broker->on_snapshot(snapshot(kFut, 22'500, t0));
    broker->on_depth(depth(kFut, {{22'499.95, 300}}, {{22'500.05, 300}}, t0));
    (void)broker->on_request(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Mis), t0);
    (void)broker->on_request(request(1, 2, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml), t0);
    (void)broker->on_tick(t0 + 200);  // both at 22,500.05; then the broker is down until tomorrow
    const oms::Paise cash = broker->account_of_user(1)->cash();

    start();
    const Millis next_morning = at_ist(9, 0) + 86'400'000;
    const auto reports = broker->on_tick(next_morning).value();
    const auto* account = broker->account_of_user(1);
    EXPECT_EQ(account->position(kFut, oms::Product::Mis)->quantity(), 0);
    EXPECT_EQ(account->position(kFut, oms::Product::Nrml)->quantity(), 75);
    EXPECT_EQ(account->position(kFut, oms::Product::Nrml)->average_price(), rupees(22'500.05));
    EXPECT_EQ(account->cash(), cash + 299'625);  // (22,540 − 22,500.05) × 75
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].event, ReportEvent::Settled);
    EXPECT_EQ(reports[0].product, oms::Product::Mis);

    start();  // again: nothing twice
    (void)broker->on_tick(next_morning + 120'000);
    EXPECT_EQ(broker->account_of_user(1)->cash(), cash + 299'625);
}

// Kite's previous close, on the live feed in today's session, settles a carried future.
messages::MarketSnapshot with_close(core::InstrumentId id, double ltp, double close, Millis at) {
    messages::MarketSnapshot s = snapshot(id, ltp, at);
    s.fields |= messages::kFieldOhlc | messages::kFieldExchangeTime;
    s.close = rupees(close);
    s.exchange_ts = core::Timestamp::from_ns(at * 1'000'000);
    return s;
}

class BrokerKiteClose : public ::testing::Test {
protected:
    void SetUp() override {
        start();
        broker_->on_snapshot(snapshot(kFut, 22'500, kBought));
        broker_->on_depth(depth(kFut, {{22'499.95, 300}}, {{22'500.05, 300}}, kBought));
        (void)broker_->on_request(request(1, 1, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml),
                                  kBought);
        (void)broker_->on_tick(kBought + 200);  // at 22,500.05
        cash_ = broker_->account_of_user(1)->cash();
    }
    void start() {
        broker_.emplace(BrokerConfig{}, store_, lookup);
        ASSERT_TRUE(broker_->load().has_value());
    }
    [[nodiscard]] const oms::Position& held() const {
        return *broker_->account_of_user(1)->position(kFut, oms::Product::Nrml);
    }

    static constexpr Millis kDay = 86'400'000;
    const Millis kBought = at_ist(10, 0);
    PaperStore store_ = PaperStore::open_in_memory().value();
    std::optional<Broker> broker_;
    oms::Paise cash_ = 0;
};

TEST_F(BrokerKiteClose, ACarriedFutureSettlesOnceAgainstKitesClose) {
    start();
    const Millis morning = at_ist(9, 20) + kDay;
    broker_->on_snapshot(with_close(kFut, 22'600, 22'540, morning));
    (void)broker_->on_tick(morning + 1'000);
    EXPECT_EQ(held().average_price(), rupees(22'540));
    EXPECT_EQ(held().entry_price(), rupees(22'500.05));
    EXPECT_EQ(broker_->account_of_user(1)->cash(), cash_ + 299'625);  // (22,540 − 22,500.05) × 75

    start();  // a restart: the same close finds nothing to do
    broker_->on_snapshot(with_close(kFut, 22'610, 22'540, morning + 60'000));
    (void)broker_->on_tick(morning + 61'000);
    EXPECT_EQ(broker_->account_of_user(1)->cash(), cash_ + 299'625);
    EXPECT_EQ(held().entry_price(), rupees(22'500.05));
}

TEST_F(BrokerKiteClose, AClosingFillKeepsItsPnlFromTheEntryPrice) {
    start();
    const Millis morning = at_ist(9, 20) + kDay;
    broker_->on_snapshot(with_close(kFut, 22'600, 22'540, morning));
    (void)broker_->on_tick(morning + 1'000);  // marked to 22,540
    broker_->on_depth(depth(kFut, {{22'599.95, 300}}, {{22'600.05, 300}}, morning + 1'000));
    (void)broker_->on_request(request(1, 2, kFut, oms::Side::Sell, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml),
                              morning + 1'000);
    (void)broker_->on_tick(morning + 1'200);  // sold at 22,599.95
    const auto trades = store_.trades(broker_->account_of_user(1)->id(), morning, morning + kDay).value();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].realised, 449'625);        // (22,599.95 − 22,540) × 75
    EXPECT_EQ(trades[0].entry_realised, 749'250);  // (22,599.95 − 22,500.05) × 75
}

TEST_F(BrokerKiteClose, ACloseFromBeforeTheOpenOrAfterTheCloseIsIgnored) {
    start();
    const Millis before_open = at_ist(9, 0) + kDay;
    broker_->on_snapshot(with_close(kFut, 22'600, 22'540, before_open));
    (void)broker_->on_tick(at_ist(9, 20) + kDay);  // in session, but seen before the open
    EXPECT_EQ(held().average_price(), rupees(22'500.05));

    const Millis evening = at_ist(16, 0) + kDay;
    broker_->on_snapshot(with_close(kFut, 22'600, 22'540, evening));
    (void)broker_->on_tick(evening + 120'000);  // after the close: the day's own mark's turn
    EXPECT_EQ(held().average_price(), rupees(22'500.05));
}

TEST_F(BrokerKiteClose, AFillTodayLeavesTheAverageForTonightsMark) {
    start();
    const Millis morning = at_ist(9, 20) + kDay;
    broker_->on_snapshot(snapshot(kFut, 22'600, morning));
    broker_->on_depth(depth(kFut, {{22'599.95, 300}}, {{22'600.05, 300}}, morning));
    (void)broker_->on_request(request(1, 2, kFut, oms::Side::Buy, oms::OrderType::Market, 75, 0, 0, oms::Product::Nrml),
                              morning);
    (void)broker_->on_tick(morning + 200);  // added at 22,600.05
    const oms::Paise cash = broker_->account_of_user(1)->cash();
    broker_->on_snapshot(with_close(kFut, 22'600, 22'540, morning + 1'000));
    (void)broker_->on_tick(morning + 61'000);
    EXPECT_EQ(held().average_price(), rupees(22'550.05));
    EXPECT_EQ(broker_->account_of_user(1)->cash(), cash);
}

}  // namespace
}  // namespace mda::broker
