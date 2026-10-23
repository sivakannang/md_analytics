#include <gtest/gtest.h>

#include <mda/notify/secrets.hpp>

namespace mda::notify {
namespace {

TEST(SecretsTest, ReadsBothChannelsWithDefaults) {
    const auto secrets = parse_notifier_secrets(R"({
        "telegram": {"bot_token": "123:abc"},
        "smtp": {"host": "smtp.gmail.com", "username": "me@gmail.com", "password": "app pass",
                 "from_address": "me@gmail.com"}})");
    ASSERT_TRUE(secrets.has_value()) << secrets.error();
    EXPECT_EQ(secrets->telegram->token, "123:abc");
    EXPECT_EQ(secrets->smtp->port, "465");
    EXPECT_EQ(secrets->smtp->from_name, "md_analytics");
}

TEST(SecretsTest, AMissingSectionTurnsTheChannelOff) {
    const auto secrets = parse_notifier_secrets(R"({"telegram": {"bot_token": "123:abc"}})");
    ASSERT_TRUE(secrets.has_value());
    EXPECT_FALSE(secrets->smtp.has_value());
}

TEST(SecretsTest, ErrorsNameTheFieldNotTheValue) {
    const auto missing = parse_notifier_secrets(R"({"smtp": {"host": "h", "username": "u", "from_address": "a@b.c"}})");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error(), "smtp.password is missing or not a string");
    const auto bad = parse_notifier_secrets(R"({"smtp": {"host": "h", "username": "u", "password": "topsecret",
                                                         "from_address": "nope"}})");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().find("topsecret"), std::string::npos);
    EXPECT_FALSE(parse_notifier_secrets("[]").has_value());
}

TEST(SecretsTest, TheExampleFileIsValid) {
    const auto secrets = load_notifier_secrets(MDA_NOTIFIER_EXAMPLE);
    ASSERT_TRUE(secrets.has_value()) << secrets.error();
    EXPECT_TRUE(secrets->telegram.has_value());
    EXPECT_TRUE(secrets->smtp.has_value());
}

}  // namespace
}  // namespace mda::notify
