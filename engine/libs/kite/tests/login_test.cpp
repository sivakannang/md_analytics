#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mda/kite/login.hpp>
#include <mda/kite/rest_client.hpp>
#include <mda/kite/session.hpp>

namespace mda::kite {
namespace {

TEST(LoginTest, ChecksumMatchesKitesFormula) {
    // Reference: python3 -c "import hashlib;print(hashlib.sha256(
    //   b'myapikey'+b'reqtoken123'+b'mysecret').hexdigest())"
    EXPECT_EQ(session_checksum("myapikey", "reqtoken123", "mysecret"),
              "82e9d50f2abaf7f9c82d95b32a0c2167ba8279e7dd394dcf4079d64f0465f87d");
}

TEST(LoginTest, ParsesLoopbackRedirectUrls) {
    const auto url = parse_redirect_url("http://127.0.0.1:8080/auth/zerodha/callback");
    ASSERT_TRUE(url.has_value());
    EXPECT_EQ(url->host, "127.0.0.1");
    EXPECT_EQ(url->port, 8080);
    EXPECT_EQ(url->path, "/auth/zerodha/callback");

    EXPECT_EQ(parse_redirect_url("http://localhost/cb")->port, 80);
    EXPECT_FALSE(parse_redirect_url("https://127.0.0.1:8080/cb").has_value());   // not http
    EXPECT_FALSE(parse_redirect_url("http://example.com:8080/cb").has_value());  // not loopback
    EXPECT_FALSE(parse_redirect_url("http://127.0.0.1:99999/cb").has_value());
    EXPECT_FALSE(parse_redirect_url("http://127.0.0.1:abc/cb").has_value());
}

TEST(LoginTest, LoginUrlCarriesStateInRedirectParams) {
    EXPECT_EQ(login_url("key1", "abc123"),
              "https://kite.zerodha.com/connect/login?v=3&api_key=key1&redirect_params=state%"
              "3Dabc123");
}

TEST(LoginTest, ParsesCallbackTarget) {
    const Callback cb = parse_callback(
        "/auth/zerodha/callback?type=login&action=login&status=success&request_token=Rt%2Bx9&state="
        "abc123");
    EXPECT_EQ(cb.path, "/auth/zerodha/callback");
    EXPECT_EQ(cb.status, "success");
    EXPECT_EQ(cb.request_token, "Rt+x9");
    EXPECT_EQ(cb.state, "abc123");

    const Callback bare = parse_callback("/favicon.ico");
    EXPECT_EQ(bare.path, "/favicon.ico");
    EXPECT_TRUE(bare.request_token.empty());
}

TEST(LoginTest, UrlEncodingRoundTrips) {
    EXPECT_EQ(url_encode("state=a b&c"), "state%3Da%20b%26c");
    EXPECT_EQ(url_decode("state%3Da%20b%26c"), "state=a b&c");
    EXPECT_EQ(url_decode("a+b%2"), "a b%2");  // '+' is a space; truncated escape kept as is
}

TEST(LoginTest, RandomStateIsUnpredictableHex) {
    const std::string a = random_state();
    const std::string b = random_state();
    EXPECT_EQ(a.size(), 32u);
    EXPECT_NE(a, b);
    EXPECT_EQ(a.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(LoginTest, ParsesSessionTokenResponse) {
    const auto token = parse_session_token(
        R"({"status":"success","data":{"user_id":"AB1234","access_token":"tok","refresh_token":""}})");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(token->access_token, "tok");
    EXPECT_EQ(token->user_id, "AB1234");
    EXPECT_FALSE(parse_session_token(R"({"status":"success","data":{}})").has_value());
}

// --------------------------------------------------------------------------- session file

class SessionFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / ("mda_login_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
        path_ = dir_ / "kite_session.json";
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
    std::filesystem::path path_;
};

TEST_F(SessionFileTest, SaveIsPrivateAndRoundTrips) {
    const Session session{
        .api_key = "key", .access_token = "tok", .api_secret = "sec", .user_id = "AB1234"};
    ASSERT_TRUE(save_session(path_, session).has_value());

    struct stat info{};
    ASSERT_EQ(::stat(path_.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600u);
    EXPECT_FALSE(std::filesystem::exists(path_.string() + ".tmp"));

    const auto loaded = load_login_credentials(path_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->session.api_secret, "sec");
    EXPECT_EQ(loaded->session.access_token, "tok");
    EXPECT_EQ(loaded->session.user_id, "AB1234");
    EXPECT_FALSE(loaded->insecure_permissions);
}

TEST_F(SessionFileTest, SaveReplacesAnExistingFile) {
    ASSERT_TRUE(save_session(path_, {.api_key = "k", .access_token = "old", .api_secret = "s"}));
    ASSERT_TRUE(save_session(path_, {.api_key = "k", .access_token = "new", .api_secret = "s"}));
    EXPECT_EQ(load_session(path_)->session.access_token, "new");
}

TEST_F(SessionFileTest, LoginNeedsApiSecretButNotAToken) {
    ASSERT_TRUE(save_session(path_, {.api_key = "k", .access_token = "", .api_secret = "s"}));
    EXPECT_TRUE(load_login_credentials(path_).has_value());
    EXPECT_EQ(load_session(path_).error(), SessionError::MissingField);  // no token yet

    ASSERT_TRUE(save_session(path_, {.api_key = "k", .access_token = "t"}));
    EXPECT_EQ(load_login_credentials(path_).error(), SessionError::MissingField);  // no secret
}

}  // namespace
}  // namespace mda::kite
