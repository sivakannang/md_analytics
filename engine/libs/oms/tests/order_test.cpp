#include <gtest/gtest.h>

#include <mda/oms/order.hpp>

namespace mda::oms {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

OrderSpec spec(OrderType type, std::int64_t quantity = 150) {
    return {.account = 1,
            .client_order_id = 7,
            .source = Source::Ui,
            .instrument = core::InstrumentId(42),
            .side = Side::Buy,
            .type = type,
            .product = Product::Mis,
            .quantity = quantity,
            .price = rupees(100),
            .trigger = rupees(101),
            .tag = {}};
}

TEST(Order, MarketAndLimitStartOpen) {
    EXPECT_EQ(Order::accepted(1, spec(OrderType::Market), 10).status(), OrderStatus::Open);
    EXPECT_EQ(Order::accepted(2, spec(OrderType::Limit), 10).status(), OrderStatus::Open);
}

TEST(Order, StopOrdersWaitForTheirTrigger) {
    auto order = Order::accepted(1, spec(OrderType::StopLimit), 10);
    EXPECT_EQ(order.status(), OrderStatus::TriggerPending);
    EXPECT_EQ(order.fill(75, rupees(100), 11).error(), OrderError::NotOpen);
    ASSERT_TRUE(order.trigger(12).has_value());
    EXPECT_EQ(order.status(), OrderStatus::Open);
    EXPECT_EQ(order.type(), OrderType::Limit);
    EXPECT_EQ(order.trigger(13).error(), OrderError::NotTriggerable);

    auto stop_market = Order::accepted(2, spec(OrderType::StopMarket), 10);
    ASSERT_TRUE(stop_market.trigger(12).has_value());
    EXPECT_EQ(stop_market.type(), OrderType::Market);
    EXPECT_EQ(Order::accepted(3, spec(OrderType::Market), 10).trigger(11).error(),
              OrderError::NotTriggerable);
}

TEST(Order, PartialThenFullFill) {
    auto order = Order::accepted(1, spec(OrderType::Market), 10);
    ASSERT_TRUE(order.fill(75, rupees(100.05), 11).has_value());
    EXPECT_EQ(order.status(), OrderStatus::Open);
    EXPECT_EQ(order.remaining(), 75);
    EXPECT_EQ(order.fill(76, rupees(100.10), 12).error(), OrderError::Overfill);
    EXPECT_EQ(order.fill(0, rupees(100.10), 12).error(), OrderError::BadQuantity);
    ASSERT_TRUE(order.fill(75, rupees(100.10), 12).has_value());
    EXPECT_EQ(order.status(), OrderStatus::Complete);
    EXPECT_EQ(order.average_price(), rupees(100.075));
    EXPECT_EQ(order.updated_at(), 12);
    EXPECT_EQ(order.fill(1, rupees(100), 13).error(), OrderError::NotLive);
    EXPECT_EQ(order.cancel("late", 13).error(), OrderError::NotLive);
}

TEST(Order, AveragePriceBetweenTicksIsRounded) {
    auto order = Order::accepted(1, spec(OrderType::Market, 3), 10);
    ASSERT_TRUE(order.fill(1, rupees(10.05), 11).has_value());
    ASSERT_TRUE(order.fill(2, rupees(10.10), 11).has_value());
    EXPECT_EQ(order.average_price().raw(), 100'833);  // 10.0833…
}

TEST(Order, CancelKeepsTheReason) {
    auto order = Order::accepted(1, spec(OrderType::Limit), 10);
    ASSERT_TRUE(order.fill(75, rupees(100), 11).has_value());
    ASSERT_TRUE(order.cancel("kill switch", 12).has_value());
    EXPECT_EQ(order.status(), OrderStatus::Cancelled);
    EXPECT_EQ(order.reason(), "kill switch");
    EXPECT_EQ(order.filled(), 75);
}

TEST(Order, ModifyRespectsWhatFilled) {
    auto order = Order::accepted(1, spec(OrderType::Limit), 10);
    ASSERT_TRUE(order.fill(75, rupees(100), 11).has_value());
    EXPECT_EQ(order.modify(50, rupees(99), {}, 12).error(), OrderError::BadQuantity);
    ASSERT_TRUE(order.modify(225, rupees(99.5), {}, 12).has_value());
    EXPECT_EQ(order.spec().quantity, 225);
    EXPECT_EQ(order.spec().price, rupees(99.5));
    ASSERT_TRUE(order.modify(75, rupees(99.5), {}, 13).has_value());  // down to the filled qty
    EXPECT_EQ(order.status(), OrderStatus::Complete);
}

TEST(Order, RejectedStaysRejected) {
    auto order = Order::rejected(1, spec(OrderType::Market), "insufficient funds", 10);
    EXPECT_EQ(order.status(), OrderStatus::Rejected);
    EXPECT_EQ(order.reason(), "insufficient funds");
    EXPECT_EQ(order.fill(75, rupees(100), 11).error(), OrderError::NotLive);
    EXPECT_EQ(order.modify(75, rupees(100), {}, 11).error(), OrderError::NotLive);
}

TEST(Order, ChargesAccumulate) {
    auto order = Order::accepted(1, spec(OrderType::Market), 10);
    order.add_charges(1'500, 9'000);
    order.add_charges(500, 4'000);
    EXPECT_EQ(order.brokerage(), 2'000);
    EXPECT_EQ(order.charges(), 13'000);
    EXPECT_EQ(name(OrderStatus::TriggerPending), "TRIGGER_PENDING");
    EXPECT_EQ(name(OrderType::StopMarket), "SL-M");
}

}  // namespace
}  // namespace mda::oms
