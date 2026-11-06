#include <gtest/gtest.h>

#include <mda/broker/paper_store.hpp>

namespace mda::broker {
namespace {

TEST(PaperStore, RoundTripsAnOrderWithItsVenueState) {
    auto store = PaperStore::open_in_memory().value();
    OrderRow row;
    row.order.id = 5;
    row.order.spec = {.account = 1, .client_order_id = 42, .source = oms::Source::Auto,
                      .instrument = core::InstrumentId(7), .side = oms::Side::Sell,
                      .type = oms::OrderType::StopLimit, .product = oms::Product::Nrml, .quantity = 150,
                      .price = core::Price::from_raw(1'000'000), .trigger = core::Price::from_raw(1'010'000),
                      .tag = "sig"};
    row.order.status = oms::OrderStatus::Open;
    row.order.type = oms::OrderType::Limit;
    row.order.filled = 75;
    row.order.filled_value_raw = 75'000'000;
    row.order.reason = "";
    row.order.created_at = 10;
    row.order.updated_at = 20;
    row.user_id = 3;
    row.margin = 12'345;
    row.venue = {.eligible_at = 110, .resting = true};
    ASSERT_TRUE(store.upsert_order(row).has_value());

    const auto live = store.live_orders().value();
    ASSERT_EQ(live.size(), 1u);
    const auto& back = live[0];
    EXPECT_EQ(back.order.spec.client_order_id, 42u);
    EXPECT_EQ(back.order.spec.source, oms::Source::Auto);
    EXPECT_EQ(back.order.spec.type, oms::OrderType::StopLimit);
    EXPECT_EQ(back.order.type, oms::OrderType::Limit);
    EXPECT_EQ(back.order.spec.tag, "sig");
    EXPECT_EQ(back.order.filled_value_raw, 75'000'000);
    EXPECT_EQ(back.user_id, 3);
    EXPECT_EQ(back.margin, 12'345);
    EXPECT_EQ(back.venue.eligible_at, 110);
    EXPECT_TRUE(back.venue.resting);
    EXPECT_TRUE(store.order_by_client(1, 42).value().has_value());
    EXPECT_FALSE(store.order_by_client(1, 43).value().has_value());
    EXPECT_EQ(store.max_order_id().value(), 5);
    EXPECT_EQ(store.orders(1, 0, 11).value().size(), 1u);
    EXPECT_TRUE(store.orders(1, 11, 100).value().empty());
    EXPECT_TRUE(store.orders(2, 0, 100).value().empty());
}

TEST(PaperStore, PositionsLedgerAndActiveInstruments) {
    auto store = PaperStore::open_in_memory().value();
    ASSERT_TRUE(store.upsert_account({.id = 1, .user_id = 3, .created_at = 1, .opening_cash = 100,
                                      .active = true, .kill_switch = true}).has_value());
    const oms::PositionKey key{.instrument = core::InstrumentId(7), .product = oms::Product::Mis};
    ASSERT_TRUE(store.upsert_position(1, key, oms::Position(75, 750'000'000, 500, 720'000'000), 2).has_value());
    ASSERT_TRUE(store.insert_ledger(1, {.at = 1, .kind = oms::LedgerKind::Opening, .amount = 100, .order_id = 0,
                                        .instrument = {}}).has_value());
    const auto accounts = store.accounts().value();
    ASSERT_EQ(accounts.size(), 1u);
    EXPECT_TRUE(accounts[0].kill_switch);
    EXPECT_EQ(store.positions(1).value().at(key).cost_raw(), 750'000'000);
    EXPECT_EQ(store.positions(1).value().at(key).entry_cost_raw(), 720'000'000);
    EXPECT_EQ(store.ledger(1).value().at(0).amount, 100);
    EXPECT_EQ(store.active_instruments().value(), std::vector<std::int64_t>{7});
    EXPECT_EQ(store.account_for_user(3).value()->id, 1);
    EXPECT_FALSE(store.account_for_user(4).value().has_value());
    ASSERT_TRUE(store.insert_trade({.order_id = 5, .account_id = 1, .instrument = core::InstrumentId(7),
                                    .side = oms::Side::Buy, .product = oms::Product::Mis, .quantity = 75,
                                    .price = core::Price::from_raw(1'000'000), .charges = {.brokerage = 2'000},
                                    .realised = 0, .at = 50}).has_value());
    const auto trades = store.trades(1, 0, 100).value();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].charges.brokerage, 2'000);
    EXPECT_TRUE(store.trades(1, 51, 100).value().empty());
    EXPECT_TRUE(store.auto_totals(1, 0, 100).value().empty());  // order 5 isn't tagged auto
    // In one transaction: a failure rolls everything back.
    const auto failed = store.in_transaction([&]() -> std::expected<void, DbError> {
        (void)store.insert_event(5, "request", "x");
        return std::unexpected(DbError{});
    });
    EXPECT_FALSE(failed.has_value());
    EXPECT_EQ(store.dump().value().find("request"), std::string::npos);
}

}  // namespace
}  // namespace mda::broker
