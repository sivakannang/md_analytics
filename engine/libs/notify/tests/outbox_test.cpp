#include <string>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>

#include <mda/appdb/outbox.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/notify/outbox.hpp>

namespace mda::notify {
namespace {

constexpr std::int64_t kNow = 1'792'690'000;

class FakeChannel : public Channel {
public:
    FakeChannel(std::string name, SendOutcome outcome) : name_(std::move(name)), outcome_(outcome) {}
    [[nodiscard]] std::string_view name() const override { return name_; }
    boost::asio::awaitable<SendResult> send(const Delivery& delivery) override {
        sent.push_back(delivery);
        co_return SendResult{.outcome = outcome_, .error = outcome_ == SendOutcome::Sent ? "" : "busy"};
    }
    std::vector<Delivery> sent;

private:
    std::string name_;
    SendOutcome outcome_;
};

class OutboxTest : public ::testing::Test {
protected:
    OutboxTest() : db_(storage::Database::open_in_memory().value()) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
    }

    std::int64_t add_user(const std::string& name, const std::string& role, bool email_verified,
                          const std::string& telegram = "") {
        auto insert = db_.prepare(
            "INSERT INTO users (username, password_hash, created_at, password_changed_at, email, "
            "email_verified_at, role) VALUES (?1, 'x', 0, 0, ?2, ?3, ?4)");
        insert->bind(1, std::string_view(name)).bind(2, std::string_view(name + "@example.com"))
            .bind(4, std::string_view(role));
        if (email_verified) {
            insert->bind(3, std::int64_t{1});
        } else {
            insert->bind_null(3);
        }
        EXPECT_TRUE(insert->run().has_value());
        const std::int64_t id = db_.last_insert_id();
        if (!telegram.empty()) {
            set_channel(id, "telegram", telegram, true);
        }
        return id;
    }

    void set_channel(std::int64_t user, const std::string& channel, const std::string& address, bool enabled) {
        auto insert = db_.prepare(
            "INSERT INTO user_channels (user_id, channel, address, enabled, linked_at) VALUES (?1, ?2, ?3, ?4, 0) "
            "ON CONFLICT (user_id, channel) DO UPDATE SET enabled = excluded.enabled");
        insert->bind(1, user).bind(2, std::string_view(channel)).bind(4, std::int64_t{enabled ? 1 : 0});
        if (address.empty()) {
            insert->bind_null(3);
        } else {
            insert->bind(3, std::string_view(address));
        }
        EXPECT_TRUE(insert->run().has_value());
    }

    void enqueue(std::int64_t user, std::string_view type, std::string_view email, std::int64_t at = kNow,
                 std::string_view payload = R"({"username":"siva"})") {
        const std::string dedup = std::string(type) + ":" + std::to_string(user) + ":" + std::to_string(at);
        EXPECT_TRUE(appdb::enqueue(db_,
                                   {.user_id = user, .type = type, .email = email, .payload = payload,
                                    .dedup_key = dedup},
                                   at)
                        .has_value());
    }

    std::string text(const std::string& sql) {
        auto query = db_.prepare(sql);
        if (query->step().value() != storage::StepResult::Row || query->column_is_null(0)) {
            return "NULL";
        }
        return query->column_text(0);
    }

    std::vector<Delivery> due(std::int64_t at) {
        const std::vector<std::string_view> all{"email", "telegram"};
        return outbox_.due(at, all).value();
    }

    storage::Database db_;
    Outbox outbox_{db_};
};

TEST_F(OutboxTest, AccountMessagesGoToTheEmailSnapshotOnly) {
    const auto user = add_user("asha", "user", false, "555");
    enqueue(user, "account.signup", "asha@example.com");
    EXPECT_EQ(outbox_.dispatch(kNow).value(), 1u);
    EXPECT_EQ(outbox_.dispatch(kNow).value(), 0u);  // already dispatched

    const auto deliveries = due(kNow);
    ASSERT_EQ(deliveries.size(), 1u);
    EXPECT_EQ(deliveries[0].channel, "email");
    EXPECT_EQ(deliveries[0].address, "asha@example.com");
    EXPECT_EQ(deliveries[0].message.subject, "Welcome to md_analytics");
    EXPECT_EQ(text("SELECT status FROM notifications"), "dispatched");
    EXPECT_EQ(text("SELECT email FROM notifications"), "NULL");  // only in the delivery now
}

TEST_F(OutboxTest, SystemAlertsGoToEveryAdminsVerifiedEnabledChannels) {
    const auto owner = add_user("owner", "admin", true, "111");
    add_user("other_admin", "admin", false);   // unverified email, no Telegram: nothing
    add_user("member", "user", true, "222");   // not an admin
    const auto quiet = add_user("quiet_admin", "admin", true, "333");
    set_channel(quiet, "email", "", false);    // email switched off
    set_channel(quiet, "telegram", "333", false);

    enqueue(0, "system.session_expired", "", kNow, R"({"process":"mda-feedd"})");
    EXPECT_EQ(outbox_.dispatch(kNow).value(), 2u);
    const auto deliveries = due(kNow);
    ASSERT_EQ(deliveries.size(), 2u);
    for (const auto& d : deliveries) {
        EXPECT_EQ(d.address, d.channel == "email" ? "owner@example.com" : "111");
        EXPECT_EQ(d.message.subject, "Kite session expired");
    }
    EXPECT_EQ(text("SELECT COUNT(DISTINCT user_id) FROM notification_deliveries"), std::to_string(1));
    (void)owner;
}

TEST_F(OutboxTest, StaleNotificationsExpireUnsent) {
    const auto user = add_user("asha", "user", false);
    enqueue(user, "account.signup", "asha@example.com", kNow - 25 * 3600);
    enqueue(user, "account.password_changed", "asha@example.com", kNow - 3600);
    EXPECT_EQ(outbox_.expire_stale(kNow).value(), 1u);
    EXPECT_EQ(outbox_.dispatch(kNow).value(), 1u);
    EXPECT_EQ(text("SELECT email FROM notifications WHERE status = 'expired'"), "NULL");
    EXPECT_EQ(due(kNow)[0].type, "account.password_changed");
}

TEST_F(OutboxTest, RetriesBackOffAndFailAfterTheLastAttempt) {
    EXPECT_EQ(outbox_.backoff(1), 30);
    EXPECT_EQ(outbox_.backoff(2), 60);
    EXPECT_EQ(outbox_.backoff(7), 1920);
    EXPECT_EQ(outbox_.backoff(9), 3600);

    const auto user = add_user("asha", "user", false);
    enqueue(user, "account.signup", "asha@example.com");
    ASSERT_TRUE(outbox_.dispatch(kNow).has_value());
    std::int64_t at = kNow;
    for (int attempt = 1; attempt < 8; ++attempt) {
        const auto deliveries = due(at);
        ASSERT_EQ(deliveries.size(), 1u) << attempt;
        ASSERT_TRUE(outbox_.record(deliveries[0], {.outcome = SendOutcome::Retry, .error = "timeout"}, at).has_value());
        EXPECT_TRUE(due(at + outbox_.backoff(attempt) - 1).empty());
        at += outbox_.backoff(attempt);
    }
    const auto last = due(at);
    ASSERT_EQ(last.size(), 1u);
    ASSERT_TRUE(outbox_.record(last[0], {.outcome = SendOutcome::Retry, .error = "timeout"}, at).has_value());
    EXPECT_EQ(text("SELECT status || ' ' || attempts || ' ' || last_error FROM notification_deliveries"),
              "failed 8 timeout");
    EXPECT_EQ(text("SELECT address FROM notification_deliveries"), "NULL");
}

TEST_F(OutboxTest, APermanentErrorFailsAtOnce) {
    const auto user = add_user("asha", "user", false);
    enqueue(user, "account.signup", "asha@example.com");
    ASSERT_TRUE(outbox_.dispatch(kNow).has_value());
    ASSERT_TRUE(outbox_.record(due(kNow)[0], {.outcome = SendOutcome::Fail, .error = "no such mailbox"}, kNow)
                    .has_value());
    EXPECT_EQ(text("SELECT status FROM notification_deliveries"), "failed");
    EXPECT_TRUE(due(kNow + 86'400).empty());
}

TEST_F(OutboxTest, DeliverDueSendsThroughConfiguredChannelsOnly) {
    add_user("owner", "admin", true, "111");
    enqueue(0, "system.login_reminder", "", kNow, "{}");
    FakeChannel email("email", SendOutcome::Sent);
    std::vector<Channel*> channels{&email};  // no Telegram in this notifier

    boost::asio::io_context io;
    auto result = boost::asio::co_spawn(io, deliver_due(outbox_, channels, kNow), boost::asio::use_future);
    io.run();
    const auto stats = result.get().value();
    EXPECT_EQ(stats.dispatched, 2u);
    EXPECT_EQ(stats.sent, 1u);
    EXPECT_EQ(stats.failed + stats.retried, 0u);
    ASSERT_EQ(email.sent.size(), 1u);
    EXPECT_EQ(email.sent[0].message.subject, "Kite login needed");
    EXPECT_EQ(text("SELECT status FROM notification_deliveries WHERE channel = 'email'"), "sent");
    EXPECT_EQ(text("SELECT status FROM notification_deliveries WHERE channel = 'telegram'"), "pending");
    EXPECT_EQ(text("SELECT address FROM notification_deliveries WHERE channel = 'email'"), "NULL");
}

}  // namespace
}  // namespace mda::notify
