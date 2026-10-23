#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <mda/appdb/schema.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/auth/password.hpp>
#include <mda/auth/session_token.hpp>
#include <mda/auth/user_store.hpp>

namespace mda::auth {
namespace {

using Code = AuthError::Code;
using namespace std::chrono_literals;

// Cheap parameters keep tests fast; production uses the OWASP defaults.
constexpr Argon2Params kFast{.memory_kib = 256, .iterations = 1, .lanes = 1};

// --------------------------------------------------------------------------- password hashing

TEST(PasswordTest, MatchesRfc9106Argon2idTestVector) {
    // RFC 9106 §5.3: password 32×0x01, salt 16×0x02, secret 8×0x03, ad 12×0x04,
    // t=3, m=32 KiB, p=4, tag 32 bytes.
    const std::vector<unsigned char> password(32, 0x01), salt(16, 0x02), secret(8, 0x03),
        ad(12, 0x04);
    const auto tag = argon2id(password, salt, secret, ad, 3, 32, 4, 32);
    ASSERT_TRUE(tag.has_value()) << tag.error();
    const std::vector<unsigned char> expected = {0x0d, 0x64, 0x0d, 0xf5, 0x8d, 0x78, 0x76, 0x6c,
                                                 0x08, 0xc0, 0x37, 0xa3, 0x4a, 0x8b, 0x53, 0xc9,
                                                 0xd0, 0x1e, 0xf0, 0x45, 0x2d, 0x75, 0xb6, 0x5e,
                                                 0xb5, 0x25, 0x20, 0xe9, 0x6b, 0x01, 0xe6, 0x59};
    EXPECT_EQ(*tag, expected);
}

TEST(PasswordTest, HashesInPhcFormatAndVerifies) {
    const auto hash = hash_password("correct horse battery", kFast);
    ASSERT_TRUE(hash.has_value()) << hash.error();
    EXPECT_TRUE(hash->starts_with("$argon2id$v=19$m=256,t=1,p=1$"));
    EXPECT_TRUE(verify_password("correct horse battery", *hash));
    EXPECT_FALSE(verify_password("correct horse batterY", *hash));
    EXPECT_FALSE(verify_password("", *hash));
}

TEST(PasswordTest, SamePasswordGetsDifferentSalts) {
    EXPECT_NE(hash_password("same password", kFast).value(),
              hash_password("same password", kFast).value());
}

TEST(PasswordTest, DefaultParametersAreOwaspMinimum) {
    const auto hash = hash_password("a real password");
    ASSERT_TRUE(hash.has_value());
    EXPECT_TRUE(hash->starts_with("$argon2id$v=19$m=19456,t=2,p=1$"));
    EXPECT_TRUE(verify_password("a real password", *hash));
}

TEST(PasswordTest, RejectsMalformedOrForeignHashes) {
    for (const char* bad :
         {"", "plain", "$argon2i$v=19$m=256,t=1,p=1$c2FsdHNhbHQ$aGFzaA",
          "$argon2id$v=16$m=256,t=1,p=1$c2FsdHNhbHRzYWx0$aGFzaGhhc2hoYXNoaGFzaA",
          "$argon2id$v=19$m=256,t=1$c2FsdHNhbHRzYWx0$aGFzaGhhc2hoYXNoaGFzaA",
          "$argon2id$v=19$m=999999999,t=1,p=1$c2FsdHNhbHRzYWx0$aGFzaGhhc2hoYXNoaGFzaA",
          "$argon2id$v=19$m=256,t=1,p=1$!!!$aGFzaA"}) {
        EXPECT_FALSE(verify_password("anything", bad)) << bad;
    }
}

TEST(PasswordTest, Base64RoundTripsWithoutPadding) {
    for (std::size_t n = 0; n < 10; ++n) {
        std::vector<unsigned char> data(n);
        for (std::size_t i = 0; i < n; ++i) {
            data[i] = static_cast<unsigned char>(i * 37 + 5);
        }
        const std::string text = base64_encode(data);
        EXPECT_EQ(text.find('='), std::string::npos);
        EXPECT_EQ(base64_decode(text), data);
    }
    EXPECT_EQ(base64_encode(std::vector<unsigned char>{'f', 'o', 'o', 'b'}), "Zm9vYg");
}

TEST(SessionTokenTest, TokensAreRandomCookieSafeAndHashed) {
    std::set<std::string> tokens;
    for (int i = 0; i < 100; ++i) {
        const std::string token = new_session_token();
        EXPECT_EQ(token.size(), 43u);
        EXPECT_EQ(token.find_first_of("+/="), std::string::npos);
        tokens.insert(token);
    }
    EXPECT_EQ(tokens.size(), 100u);
    const std::string token = *tokens.begin();
    EXPECT_EQ(hash_token(token), hash_token(token));
    EXPECT_NE(hash_token(token), hash_token(*tokens.rbegin()));
}

// --------------------------------------------------------------------------- service

/// Signs up with a unique email and a valid mobile number unless given.
std::expected<User, AuthError> signup(AuthService& auth, std::string_view username,
                                      std::string_view password, std::string email = {},
                                      std::string phone = "98765 43210") {
    static int counter = 0;
    if (email.empty()) {
        email = "user" + std::to_string(++counter) + "@example.com";
    }
    return auth.signup(
        {.username = username, .email = email, .phone = phone, .password = password});
}

class AuthServiceTest : public ::testing::Test {
protected:
    AuthServiceTest() : db_(storage::Database::open_in_memory().value()), store_(db_) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
    }

    AuthService make_service(AuthOptions options = {}) {
        options.argon2 = kFast;
        return AuthService(store_, options, [this] { return now_; });
    }

    storage::Database db_;
    UserStore store_;
    std::int64_t now_ = 1'761'100'000;
};

TEST_F(AuthServiceTest, SignupValidatesAndNormalizes) {
    AuthService auth = make_service();
    const auto user = signup(auth, "Siva_K", "correct horse battery");
    ASSERT_TRUE(user.has_value()) << user.error().message;
    EXPECT_EQ(user->username, "siva_k");  // stored lower-case
    EXPECT_EQ(user->created_at, now_);

    EXPECT_EQ(signup(auth, "ab", "correct horse battery").error().code, Code::Input);
    EXPECT_EQ(signup(auth, "bad name", "correct horse battery").error().code, Code::Input);
    EXPECT_EQ(signup(auth, "another", "short").error().code, Code::Input);
    EXPECT_EQ(signup(auth, "another", std::string(129, 'x')).error().code, Code::Input);
    EXPECT_TRUE(signup(auth, "unicode", "ಪಾಸ್‌ವರ್ಡ್ಪಾಸ್")
                    .has_value());  // counted in code points
}

TEST(ContactDetailsTest, NormalizesAndValidatesEmail) {
    EXPECT_EQ(AuthService::normalize_email("  Siva.K@Example.COM "), "siva.k@example.com");
    EXPECT_EQ(AuthService::normalize_email("a+tag@sub.domain.in"), "a+tag@sub.domain.in");
    for (const char* bad : {"", "plain", "@example.com", "a@", "a@b", "a@b.", "a@.com", "a@b..com",
                            "a b@example.com", "a@b@c.com"}) {
        EXPECT_EQ(AuthService::normalize_email(bad), "") << bad;
    }
}

TEST(ContactDetailsTest, NormalizesIndianMobileNumbersToE164) {
    for (const char* good : {"9876543210", "98765 43210", "+91 98765-43210", "+919876543210",
                             "919876543210", "09876543210", "(+91) 98765 43210"}) {
        EXPECT_EQ(AuthService::normalize_phone(good), "+919876543210") << good;
    }
    for (const char* bad : {"", "12345", "5876543210", "+1 202 555 0123", "98765432101",
                            "98765x43210", "+44 7911 123456"}) {
        EXPECT_EQ(AuthService::normalize_phone(bad), "") << bad;
    }
}

TEST_F(AuthServiceTest, SignupStoresNormalizedContactDetails) {
    AuthService auth = make_service();
    const auto user =
        signup(auth, "siva", "correct horse battery", " Siva@Example.com ", "+91 98765-43210");
    ASSERT_TRUE(user.has_value()) << user.error().message;
    EXPECT_EQ(user->email, "siva@example.com");
    EXPECT_EQ(user->phone, "+919876543210");
    EXPECT_FALSE(user->email_verified);

    const auto session = auth.signin("siva", "correct horse battery", "").value();
    const auto profile = auth.authenticate(session.token).value();
    EXPECT_EQ(profile.email, "siva@example.com");
    EXPECT_EQ(profile.phone, "+919876543210");
    EXPECT_FALSE(profile.phone_verified);
}

TEST_F(AuthServiceTest, SignupRejectsBadContactDetails) {
    AuthService auth = make_service();
    EXPECT_EQ(signup(auth, "siva", "correct horse battery", "not-an-email").error().code,
              Code::Input);
    EXPECT_EQ(signup(auth, "siva", "correct horse battery", {}, "12345").error().code, Code::Input);
}

TEST_F(AuthServiceTest, EmailsAreUniqueIgnoringCase) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery", "siva@example.com").has_value());
    const auto again = signup(auth, "other", "correct horse battery", "SIVA@example.com");
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, Code::Conflict);
    EXPECT_EQ(again.error().message, "email already registered");
    // Phone numbers may be shared (e.g. family members).
    EXPECT_TRUE(signup(auth, "other", "correct horse battery").has_value());
}

TEST_F(AuthServiceTest, UsernamesAreUniqueIgnoringCase) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    EXPECT_EQ(signup(auth, "SIVA", "another password").error().code, Code::Conflict);
}

TEST_F(AuthServiceTest, SignupCanBeDisabled) {
    AuthService auth = make_service({.allow_signup = false});
    EXPECT_EQ(signup(auth, "siva", "correct horse battery").error().code, Code::Permission);
}

TEST_F(AuthServiceTest, SigninIssuesASessionThatAuthenticates) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto signed_in = auth.signin("Siva", "correct horse battery", "test-agent");
    ASSERT_TRUE(signed_in.has_value()) << signed_in.error().message;
    EXPECT_EQ(signed_in->user.username, "siva");

    const auto user = auth.authenticate(signed_in->token);
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->id, signed_in->user.id);
    EXPECT_EQ(auth.authenticate("not-a-token").error().code, Code::Auth);
    EXPECT_EQ(auth.authenticate("").error().code, Code::Auth);
}

TEST_F(AuthServiceTest, WrongPasswordAndUnknownUserLookIdentical) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto wrong = auth.signin("siva", "wrong password!", "");
    const auto unknown = auth.signin("nobody", "wrong password!", "");
    ASSERT_FALSE(wrong.has_value());
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(wrong.error().code, Code::Auth);
    EXPECT_EQ(unknown.error().code, Code::Auth);
    EXPECT_EQ(wrong.error().message, unknown.error().message);
}

TEST_F(AuthServiceTest, RepeatedFailuresAreThrottledWithGrowingDelay) {
    AuthService auth = make_service({.free_failures = 3, .max_delay = 8s});
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(auth.signin("siva", "wrong password!", "").error().code, Code::Auth);
    }
    // Third failure started a 1 s block; even the right password is refused meanwhile.
    EXPECT_EQ(auth.signin("siva", "correct horse battery", "").error().code, Code::RateLimited);
    now_ += 1;
    EXPECT_EQ(auth.signin("siva", "wrong password!", "").error().code, Code::Auth);  // now 2 s
    now_ += 1;
    EXPECT_EQ(auth.signin("siva", "correct horse battery", "").error().code, Code::RateLimited);
    now_ += 1;
    EXPECT_TRUE(auth.signin("siva", "correct horse battery", "").has_value());  // resets
    EXPECT_EQ(auth.signin("siva", "wrong password!", "").error().code, Code::Auth);
}

TEST_F(AuthServiceTest, IdleSessionsExpireAndActiveOnesSlide) {
    AuthService auth = make_service({.session_idle_timeout = 100s});
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto session = auth.signin("siva", "correct horse battery", "").value();

    now_ += 90;
    EXPECT_TRUE(auth.authenticate(session.token).has_value());  // refreshes the idle timer
    now_ += 90;
    EXPECT_TRUE(auth.authenticate(session.token).has_value());  // 180 s total, but active
    now_ += 101;
    EXPECT_EQ(auth.authenticate(session.token).error().code, Code::Auth);
}

TEST_F(AuthServiceTest, SignoutEndsTheSession) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto session = auth.signin("siva", "correct horse battery", "").value();
    ASSERT_TRUE(auth.signout(session.token).has_value());
    EXPECT_EQ(auth.authenticate(session.token).error().code, Code::Auth);
}

TEST_F(AuthServiceTest, PasswordChangeRevokesOtherSessionsOnly) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto laptop = auth.signin("siva", "correct horse battery", "laptop").value();
    const auto phone = auth.signin("siva", "correct horse battery", "phone").value();

    EXPECT_EQ(auth.change_password(laptop.token, "wrong current", "a brand new pass").error().code,
              Code::Auth);
    EXPECT_EQ(auth.change_password(laptop.token, "correct horse battery", "short").error().code,
              Code::Input);
    ASSERT_TRUE(auth.change_password(laptop.token, "correct horse battery", "a brand new pass"));

    EXPECT_TRUE(auth.authenticate(laptop.token).has_value());            // kept
    EXPECT_EQ(auth.authenticate(phone.token).error().code, Code::Auth);  // revoked
    EXPECT_EQ(auth.signin("siva", "correct horse battery", "").error().code, Code::Auth);
    EXPECT_TRUE(auth.signin("siva", "a brand new pass", "").has_value());
}

TEST_F(AuthServiceTest, PurgeRemovesOnlyIdleSessions) {
    AuthService auth = make_service({.session_idle_timeout = 100s});
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto old_session = auth.signin("siva", "correct horse battery", "").value();
    now_ += 150;
    const auto fresh = auth.signin("siva", "correct horse battery", "").value();
    EXPECT_EQ(auth.purge_expired_sessions(), 1);
    EXPECT_TRUE(auth.authenticate(fresh.token).has_value());
}

TEST_F(AuthServiceTest, DatabaseNeverContainsPasswordsOrTokens) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto session = auth.signin("siva", "correct horse battery", "").value();

    auto users = db_.prepare("SELECT password_hash FROM users");
    ASSERT_EQ(users->step().value(), storage::StepResult::Row);
    EXPECT_EQ(users->column_text(0).find("correct horse"), std::string::npos);

    auto sessions = db_.prepare("SELECT token_hash FROM sessions");
    ASSERT_EQ(sessions->step().value(), storage::StepResult::Row);
    const auto stored = sessions->column_blob(0);
    const auto expected = hash_token(session.token);
    EXPECT_TRUE(std::equal(stored.begin(), stored.end(), expected.begin(), expected.end()));
}

TEST_F(AuthServiceTest, AccountEventsQueueNotifications) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery", "Siva@Example.com").has_value());
    const auto session = auth.signin("siva", "correct horse battery", "").value();
    now_ += 1;
    ASSERT_TRUE(auth.change_password(session.token, "correct horse battery", "a brand new pass"));
    now_ += 1;
    ASSERT_TRUE(auth.delete_account(session.token, "a brand new pass").has_value());

    auto rows = db_.prepare("SELECT type, email, payload, status FROM notifications ORDER BY id");
    std::vector<std::string> types;
    while (rows->step().value() == storage::StepResult::Row) {
        types.emplace_back(rows->column_text(0));
        EXPECT_EQ(rows->column_text(1), "siva@example.com");  // snapshot survives the deletion
        EXPECT_EQ(rows->column_text(2), R"({"username":"siva"})");
        EXPECT_EQ(rows->column_text(3), "pending");
    }
    EXPECT_EQ(types, (std::vector<std::string>{"account.signup", "account.password_changed",
                                               "account.deleted"}));
}

TEST_F(AuthServiceTest, DeleteAccountNeedsThePasswordAndRemovesEverything) {
    AuthService auth = make_service();
    ASSERT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
    const auto laptop = auth.signin("siva", "correct horse battery", "laptop").value();
    const auto phone = auth.signin("siva", "correct horse battery", "phone").value();

    EXPECT_EQ(auth.delete_account(laptop.token, "wrong password").error().code, Code::Auth);
    EXPECT_EQ(auth.delete_account("not-a-token", "correct horse battery").error().code, Code::Auth);
    ASSERT_TRUE(auth.authenticate(laptop.token).has_value());  // nothing happened yet

    const auto deleted = auth.delete_account(laptop.token, "correct horse battery");
    ASSERT_TRUE(deleted.has_value());
    EXPECT_EQ(deleted->username, "siva");
    EXPECT_EQ(auth.authenticate(laptop.token).error().code, Code::Auth);
    EXPECT_EQ(auth.authenticate(phone.token).error().code, Code::Auth);
    EXPECT_EQ(auth.signin("siva", "correct horse battery", "").error().code, Code::Auth);

    auto sessions = db_.prepare("SELECT COUNT(*) FROM sessions");
    ASSERT_EQ(sessions->step().value(), storage::StepResult::Row);
    EXPECT_EQ(sessions->column_int64(0), 0);
    // The username and email are free again.
    EXPECT_TRUE(signup(auth, "siva", "correct horse battery").has_value());
}

}  // namespace
}  // namespace mda::auth
