#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <mda/kite/ws_client.hpp>

namespace mda::kite {
namespace {

using Tokens = std::vector<std::uint32_t>;
using Messages = std::vector<std::string>;

TEST(SubscriptionMessagesTest, SubscribesThenSetsModeInBatches) {
    EXPECT_EQ(subscription_messages(Tokens{1, 2, 3}, "full", 2),
              (Messages{R"({"a":"subscribe","v":[1,2]})", R"({"a":"mode","v":["full",[1,2]]})",
                        R"({"a":"subscribe","v":[3]})", R"({"a":"mode","v":["full",[3]]})"}));
    EXPECT_TRUE(subscription_messages(Tokens{}, "full", 2).empty());
}

TEST(SubscriptionChangesTest, SendsOnlyTheDifference) {
    EXPECT_EQ(subscription_changes(Tokens{1, 2, 3}, Tokens{2, 3, 4, 5}, "quote", 500),
              (Messages{R"({"a":"unsubscribe","v":[1]})", R"({"a":"subscribe","v":[4,5]})",
                        R"({"a":"mode","v":["quote",[4,5]]})"}));
}

TEST(SubscriptionChangesTest, NothingChangedMeansNoMessages) {
    EXPECT_TRUE(subscription_changes(Tokens{7, 8}, Tokens{7, 8}, "full", 500).empty());
    EXPECT_TRUE(subscription_changes(Tokens{}, Tokens{}, "full", 500).empty());
}

TEST(SubscriptionChangesTest, BatchesLargeChanges) {
    const auto messages = subscription_changes(Tokens{1, 2, 3}, Tokens{}, "full", 2);
    EXPECT_EQ(messages,
              (Messages{R"({"a":"unsubscribe","v":[1,2]})", R"({"a":"unsubscribe","v":[3]})"}));
}

}  // namespace
}  // namespace mda::kite
