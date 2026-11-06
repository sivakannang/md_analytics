#include <gtest/gtest.h>

#include <mda/autotrade/decision_store.hpp>

namespace mda::autotrade {
namespace {

TEST(DecisionStore, SavesDecisionsAndTheDay) {
    auto store = DecisionStore::open_in_memory().value();
    const Decision decisions[] = {{.at = 100,
                                   .before = State::Flat,
                                   .action = "enter long",
                                   .reason = "Bullish +38",
                                   .client_order_id = 5,
                                   .price = core::Price::from_raw(226'000'000)},
                                  {.at = 200,
                                   .before = State::Entering,
                                   .action = "long",
                                   .reason = "entry filled",
                                   .client_order_id = 5}};
    ASSERT_TRUE(store
                    .save("nifty-follow", decisions,
                          {.day = 20'748,
                           .entries = 1,
                           .realised = 0,
                           .charges = 9'600,
                           .last_exit = 0,
                           .counter = 1,
                           .last_entry = 200})
                    .has_value());
    ASSERT_TRUE(
        store
            .save("nifty-follow", {},
                  {.day = 20'748, .entries = 1, .charges = 9'700, .counter = 2, .last_entry = 200})
            .has_value());  // the day row is replaced

    const auto day = store.day("nifty-follow", 20'748).value();
    ASSERT_TRUE(day.has_value());
    EXPECT_EQ(day->charges, 9'700);
    EXPECT_EQ(day->counter, 2u);
    EXPECT_EQ(day->last_entry, 200);
    EXPECT_FALSE(store.day("nifty-follow", 20'749).value().has_value());
    EXPECT_FALSE(store.day("other", 20'748).value().has_value());

    const auto back = store.decisions("nifty-follow", 0, 1'000).value();
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(back[0].action, "enter long");
    EXPECT_EQ(back[0].reason, "Bullish +38");
    EXPECT_EQ(back[0].price.raw(), 226'000'000);
    EXPECT_EQ(back[1].before, State::Entering);
    EXPECT_TRUE(store.decisions("nifty-follow", 150, 1'000).value().size() == 1u);
}

}  // namespace
}  // namespace mda::autotrade
