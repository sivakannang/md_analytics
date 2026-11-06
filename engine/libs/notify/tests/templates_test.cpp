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
    const auto m = render("system.feed_down",
                          R"({"state":"stale","since":"10:02:11","detail":"no ticks for 5 s."})");
    EXPECT_EQ(m.subject, "Market feed stale");
    EXPECT_NE(m.text.find("10:02:11"), std::string::npos);
}

TEST(TemplatesTest, UnknownTypesAndBadPayloadsStillRender) {
    EXPECT_EQ(render("signal.new", R"({"x":1})").subject, "signal.new");
    EXPECT_TRUE(render("account.signup", "not json").text.starts_with("Hello,"));
}

}  // namespace
TEST(Templates, PaperDailySummary) {
    const auto m =
        render("paper.daily_summary",
               R"({"username":"asha","date":"2026-10-23","fills":4,"realised":"₹-994.50",)"
               R"("charges":"₹172.19","net":"₹-1166.69","open_positions":0,"cash":"₹998833.31"})");
    EXPECT_EQ(m.subject, "Paper P&L 2026-10-23: net ₹-1166.69");
    EXPECT_NE(m.text.find("Hi asha"), std::string::npos);
    EXPECT_NE(m.text.find("4 fills, realised ₹-994.50, charges ₹172.19"), std::string::npos);
    EXPECT_NE(m.text.find("no real orders"), std::string::npos);
    EXPECT_EQ(m.text.find("Auto trader"), std::string::npos);  // no auto section without auto fills
    const auto with_auto = render(
        "paper.daily_summary",
        R"({"username":"asha","date":"2026-10-23","auto":"nifty-foll: 6 fills, net ₹690.41"})");
    EXPECT_NE(with_auto.text.find("Auto trader: nifty-foll: 6 fills, net ₹690.41."),
              std::string::npos);
}

}  // namespace mda::notify
