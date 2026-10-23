#include <gtest/gtest.h>

#include <mda/appdb/schema.hpp>
#include <mda/notify/codes.hpp>

namespace mda::notify {
namespace {

constexpr std::int64_t kNow = 1'792'690'000;

class CodesTest : public ::testing::Test {
protected:
    CodesTest() : db_(storage::Database::open_in_memory().value()) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
        for (const char* name : {"asha", "ravi"}) {
            auto insert = db_.prepare(
                "INSERT INTO users (username, password_hash, created_at, password_changed_at) "
                "VALUES (?1, 'x', 0, 0)");
            insert->bind(1, std::string_view(name));
            EXPECT_TRUE(insert->run().has_value());
        }
    }
    storage::Database db_;
};

TEST_F(CodesTest, ALinkCodeWorksOnceBeforeItExpires) {
    const auto code = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    EXPECT_EQ(code.size(), 32u);
    EXPECT_FALSE(consume_link_code(db_, "0000", kNow).value().has_value());
    EXPECT_EQ(consume_link_code(db_, code, kNow + 60).value(), std::optional<std::int64_t>(1));
    EXPECT_FALSE(consume_link_code(db_, code, kNow + 61).value().has_value());  // used

    const auto late = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    EXPECT_FALSE(consume_link_code(db_, late, kNow + 600).value().has_value());  // expired
}

TEST_F(CodesTest, ANewLinkCodeReplacesTheOldOne) {
    const auto first = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    const auto second = create_link_code(db_, 1, std::chrono::minutes(10), kNow).value();
    EXPECT_FALSE(consume_link_code(db_, first, kNow).value().has_value());
    EXPECT_TRUE(consume_link_code(db_, second, kNow).value().has_value());
}

std::string wrong(const std::string& code) {
    return code == "000000" ? "111111" : "000000";
}

TEST_F(CodesTest, EmailCodesBelongToTheirUser) {
    const auto code = create_email_code(db_, 1, kNow).value().value();
    EXPECT_EQ(code.size(), 6u);
    EXPECT_FALSE(consume_email_code(db_, 2, code, kNow).value());  // someone else's
    EXPECT_FALSE(consume_email_code(db_, 1, wrong(code), kNow).value());
    EXPECT_TRUE(consume_email_code(db_, 1, code, kNow).value());
    EXPECT_FALSE(consume_email_code(db_, 1, code, kNow).value());  // used

    const auto again = create_email_code(db_, 1, kNow + 60).value().value();
    EXPECT_FALSE(consume_email_code(db_, 1, again, kNow + 60 + 15 * 60).value());  // expired
}

TEST_F(CodesTest, EmailCodesAreRationedAndDieAfterFiveWrongGuesses) {
    const auto code = create_email_code(db_, 1, kNow).value().value();
    EXPECT_FALSE(create_email_code(db_, 1, kNow + 59).value().has_value());  // too soon
    for (int i = 0; i < kEmailCodeGuesses; ++i) {
        EXPECT_FALSE(consume_email_code(db_, 1, wrong(code), kNow).value());
    }
    EXPECT_FALSE(consume_email_code(db_, 1, code, kNow).value());  // gone: ask for a new one
    EXPECT_TRUE(create_email_code(db_, 1, kNow + 60).value().has_value());
}

}  // namespace
}  // namespace mda::notify
