// Admin queries over a real schema, with events produced by the real AuthService.

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <mda/admin/admin.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/auth/user_store.hpp>

namespace mda::admin {
namespace {

using namespace std::chrono_literals;

class AdminTest : public ::testing::Test {
protected:
    AdminTest()
        : db_(storage::Database::open_in_memory().value())
        , store_(db_)
        , auth_(store_,
                {.session_idle_timeout = 100s,
                 .argon2 = {.memory_kib = 256, .iterations = 1, .lanes = 1}},
                [this] { return now_; }) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
        for (const char* name : {"asha", "bala"}) {
            EXPECT_TRUE(auth_.signup({.username = name,
                                      .email = std::string(name) + "@example.com",
                                      .phone = "9876543210",
                                      .password = "correct horse battery"}));
        }
    }

    std::int64_t now_ = 1'792'640'000;  // 2026-10-22 ~09:16 IST
    storage::Database db_;
    auth::UserStore store_;
    auth::AuthService auth_;
};

TEST(IstDayTest, UsesIndianTime) {
    EXPECT_EQ(ist_day(1'792'640'000), "2026-10-22");
    EXPECT_EQ(ist_day(1'792'607'400), "2026-10-22");  // 00:00 IST = 18:30 UTC the day before
    EXPECT_EQ(ist_day(1'792'607'399), "2026-10-21");
}

TEST_F(AdminTest, ListsUsersWithSignInHistoryAndUsage) {
    const auto session =
        auth_.signin("asha", "correct horse battery", "Firefox", "10.0.0.7").value();
    now_ += 10;
    EXPECT_FALSE(auth_.signin("bala", "wrong password", "curl", "10.0.0.9").has_value());
    now_ += 10;
    ASSERT_TRUE(auth_.signout(session.token, "10.0.0.7").has_value());
    ASSERT_TRUE(add_usage(db_, 1, "2026-10-22", {.ws_bytes = 1000, .ws_messages = 4}).has_value());
    ASSERT_TRUE(add_usage(db_, 1, "2026-10-22", {.ws_bytes = 500, .http_requests = 2}).has_value());
    ASSERT_TRUE(
        add_usage(db_, 99, "2026-10-22", {.ws_bytes = 1}).has_value());  // deleted user: skipped

    const auto users = list_users(db_, "2026-10-22", now_ - 100).value();
    ASSERT_EQ(users.size(), 2u);
    EXPECT_EQ(users[0].username, "asha");
    EXPECT_EQ(users[0].role, "user");
    EXPECT_EQ(users[0].last_sign_in_at, 1'792'640'000);
    EXPECT_EQ(users[0].last_sign_out_at, 1'792'640'020);
    EXPECT_EQ(users[0].active_sessions, 0);  // signed out
    EXPECT_EQ(users[0].today, (Usage{.ws_bytes = 1500, .ws_messages = 4, .http_requests = 2}));
    EXPECT_EQ(users[1].last_sign_in_at, 0);  // only a failed attempt

    const auto detail = user_detail(db_, 2, "2026-10-22", now_ - 100, "2026-09-22").value();
    ASSERT_TRUE(detail.has_value());
    ASSERT_EQ(detail->events.size(), 1u);
    EXPECT_EQ(detail->events[0].type, "sign_in_failed");
    EXPECT_EQ(detail->events[0].ip, "10.0.0.9");
}

TEST_F(AdminTest, DetailShowsSessionsEventsAndUsageDays) {
    ASSERT_TRUE(auth_.signin("asha", "correct horse battery", "Chrome", "10.0.0.7").has_value());
    now_ += 5;
    ASSERT_TRUE(auth_.signin("asha", "correct horse battery", "Safari", "10.0.0.8").has_value());
    ASSERT_TRUE(add_usage(db_, 1, "2026-10-21", {.ws_bytes = 10}).has_value());
    ASSERT_TRUE(add_usage(db_, 1, "2026-10-22", {.ws_bytes = 20}).has_value());

    const auto detail = user_detail(db_, 1, "2026-10-22", now_ - 100, "2026-10-01").value();
    ASSERT_TRUE(detail.has_value());
    ASSERT_EQ(detail->sessions.size(), 2u);
    EXPECT_EQ(detail->sessions[0].user_agent, "Safari");  // most recent first
    EXPECT_EQ(detail->sessions[0].ip, "10.0.0.8");
    EXPECT_EQ(detail->sessions[0].id.size(), 64u);  // hex SHA-256
    EXPECT_EQ(detail->events.size(), 2u);
    ASSERT_EQ(detail->usage.size(), 2u);
    EXPECT_EQ(detail->usage[0].day, "2026-10-21");
    EXPECT_EQ(detail->user.active_sessions, 2);
    EXPECT_FALSE(user_detail(db_, 42, "2026-10-22", 0, "2026-10-01").value().has_value());
}

TEST_F(AdminTest, RevokingASessionSignsItOutAndIsLogged) {
    const auto session =
        auth_.signin("asha", "correct horse battery", "Chrome", "10.0.0.7").value();
    const auto id = user_detail(db_, 1, "2026-10-22", 0, "2026-10-01").value()->sessions.at(0).id;

    EXPECT_EQ(revoke_session(db_, id, now_ + 1, "siva").value(), std::optional<std::int64_t>(1));
    EXPECT_FALSE(auth_.authenticate(session.token).has_value());
    EXPECT_EQ(revoke_session(db_, id, now_ + 2, "siva").value(), std::nullopt);  // already gone
    EXPECT_EQ(user_detail(db_, 1, "2026-10-22", 0, "2026-10-01").value()->events.at(0).type,
              "session_revoked");
}

TEST_F(AdminTest, ExpiryIsLoggedAndOldEventsArePurged) {
    ASSERT_TRUE(auth_.signin("asha", "correct horse battery", "Chrome", "10.0.0.7").has_value());
    now_ += 500;
    EXPECT_EQ(auth_.purge_expired_sessions(), 1);
    const auto events = user_detail(db_, 1, "2026-10-22", 0, "2026-10-01").value()->events;
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, "session_expired");
    EXPECT_EQ(events[0].ip, "10.0.0.7");

    EXPECT_EQ(purge_events(db_, now_ - 100).value(), 1);  // the sign-in, 500 s ago
}

TEST_F(AdminTest, RolesAreSetByUsername) {
    EXPECT_TRUE(set_role(db_, "bala", "admin").value());
    EXPECT_FALSE(set_role(db_, "nobody", "admin").value());
    const auto session = auth_.signin("bala", "correct horse battery", "").value();
    EXPECT_TRUE(auth_.authenticate(session.token).value().is_admin());
    EXPECT_EQ(list_users(db_, "2026-10-22", 0).value()[1].role, "admin");
}

}  // namespace
}  // namespace mda::admin
