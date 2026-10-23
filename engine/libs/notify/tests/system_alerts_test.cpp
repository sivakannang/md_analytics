#include <gtest/gtest.h>

#include <mda/notify/system_alerts.hpp>

namespace mda::notify {
namespace {

constexpr std::int64_t kThursday = 1'792'607'400;  // 2026-10-22 00:00 IST
constexpr std::int64_t kSaturday = kThursday + 2 * 86'400;

std::int64_t at(std::int64_t day, int h, int m, int s = 0) {
    return day + h * 3600 + m * 60 + s;
}

TEST(SystemAlertsTest, IstFormatting) {
    EXPECT_EQ(ist_date(at(kThursday, 9, 41, 7)), "2026-10-22");
    EXPECT_EQ(ist_clock(at(kThursday, 9, 41, 7)), "09:41:07");
}

TEST(SystemAlertsTest, FeedDownAfterTheGraceThenBackOnce) {
    FeedAlerts alerts;
    const std::int64_t t0 = at(kThursday, 10, 0);
    EXPECT_TRUE(alerts.update(FeedCondition::Up, "", t0).empty());
    EXPECT_TRUE(alerts.update(FeedCondition::Down, "no data for 5 s", t0 + 1).empty());
    EXPECT_TRUE(alerts.update(FeedCondition::Down, "no data for 5 s", t0 + 60).empty());  // 59 s down
    const auto down = alerts.update(FeedCondition::Down, "no data for 5 s", t0 + 61);
    ASSERT_EQ(down.size(), 1u);
    EXPECT_EQ(down[0].type, "system.feed_down");
    EXPECT_NE(down[0].payload.find(R"("since":"10:00:01")"), std::string::npos);
    EXPECT_TRUE(alerts.update(FeedCondition::Down, "", t0 + 300).empty());  // said once

    const auto back = alerts.update(FeedCondition::Up, "", t0 + 400);
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].type, "system.feed_restored");
    EXPECT_TRUE(alerts.update(FeedCondition::Up, "", t0 + 401).empty());
}

TEST(SystemAlertsTest, ShortDropsAndClosedMarketsStayQuiet) {
    FeedAlerts alerts;
    const std::int64_t t0 = at(kThursday, 11, 0);
    EXPECT_TRUE(alerts.update(FeedCondition::Down, "", t0).empty());
    EXPECT_TRUE(alerts.update(FeedCondition::Up, "", t0 + 30).empty());  // a 30 s blip: no down, no back

    FeedAlerts night;
    EXPECT_TRUE(night.update(FeedCondition::Down, "", at(kThursday, 22, 0)).empty());
    EXPECT_TRUE(night.update(FeedCondition::Down, "", at(kThursday, 23, 0)).empty());
    FeedAlerts weekend;
    EXPECT_TRUE(weekend.update(FeedCondition::Down, "", at(kSaturday, 10, 0)).empty());
    EXPECT_TRUE(weekend.update(FeedCondition::Down, "", at(kSaturday, 10, 5)).empty());
}

TEST(SystemAlertsTest, ARejectedSessionAlertsOncePerDayAfterSeven) {
    FeedAlerts alerts;
    EXPECT_TRUE(alerts.update(FeedCondition::AuthRejected, "", at(kThursday, 6, 5)).empty());  // too early
    const auto morning = alerts.update(FeedCondition::AuthRejected, "", at(kThursday, 7, 0));
    ASSERT_EQ(morning.size(), 1u);
    EXPECT_EQ(morning[0].type, "system.session_expired");
    EXPECT_EQ(morning[0].dedup_key, "system.session_expired:2026-10-22");
    EXPECT_TRUE(alerts.update(FeedCondition::AuthRejected, "", at(kThursday, 7, 1)).empty());  // once a day
    EXPECT_EQ(alerts.update(FeedCondition::AuthRejected, "", at(kThursday + 86'400, 7, 0)).size(), 1u);
}

TEST(SystemAlertsTest, LoginReminderOnWeekdayMorningsWithoutTodaysSession) {
    const std::int64_t yesterday = at(kThursday - 86'400, 8, 30);
    EXPECT_FALSE(login_reminder(at(kThursday, 8, 44), yesterday).has_value());
    const auto reminder = login_reminder(at(kThursday, 8, 45), yesterday);
    ASSERT_TRUE(reminder.has_value());
    EXPECT_EQ(reminder->dedup_key, "system.login_reminder:2026-10-22");
    EXPECT_TRUE(login_reminder(at(kThursday, 9, 0), std::nullopt).has_value());
    EXPECT_FALSE(login_reminder(at(kThursday, 9, 0), at(kThursday, 8, 50)).has_value());  // logged in
    EXPECT_FALSE(login_reminder(at(kSaturday, 9, 0), yesterday).has_value());
    EXPECT_FALSE(login_reminder(at(kThursday, 16, 0), yesterday).has_value());  // after the close
}

}  // namespace
}  // namespace mda::notify
