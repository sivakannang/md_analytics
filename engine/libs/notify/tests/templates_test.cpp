#include <gtest/gtest.h>

#include <mda/notify/templates.hpp>

namespace mda::notify {
namespace {

TEST(TemplatesTest, AccountMessagesGreetTheUser) {
    const auto m = render("account.signup", R"({"username":"asha"})");
    EXPECT_EQ(m.subject, "Welcome to md_analytics");
    EXPECT_TRUE(m.text.starts_with("Hi asha,"));
    EXPECT_TRUE(m.text.ends_with("— md_analytics"));
}

TEST(TemplatesTest, VerificationCodeIsInTheText) {
    const auto m = render("account.email_code", R"({"username":"asha","code":"482913"})");
    EXPECT_NE(m.text.find("482913"), std::string::npos);
}

TEST(TemplatesTest, SystemAlertsUseTheirFields) {
    const auto m = render("system.feed_down", R"({"state":"stale","since":"10:02:11","detail":"no ticks for 5 s."})");
    EXPECT_EQ(m.subject, "Market feed stale");
    EXPECT_NE(m.text.find("10:02:11"), std::string::npos);
}

TEST(TemplatesTest, UnknownTypesAndBadPayloadsStillRender) {
    EXPECT_EQ(render("signal.new", R"({"x":1})").subject, "signal.new");
    EXPECT_TRUE(render("account.signup", "not json").text.starts_with("Hello,"));
}

}  // namespace
}  // namespace mda::notify
