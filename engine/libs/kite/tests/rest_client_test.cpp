// Offline tests for session loading and REST response handling. The network path
// itself is exercised by mda-kite-check against the real Kite API.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mda/kite/rest_client.hpp>
#include <mda/kite/session.hpp>

namespace mda::kite {
namespace {

using Code = KiteError::Code;

class SessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::unsetenv("MDA_KITE_API_KEY");
        ::unsetenv("MDA_KITE_ACCESS_TOKEN");
        path_ = std::filesystem::temp_directory_path() /
                ("mda_session_" + std::to_string(::getpid()) + ".json");
    }
    void TearDown() override {
        std::filesystem::remove(path_);
        ::unsetenv("MDA_KITE_API_KEY");
        ::unsetenv("MDA_KITE_ACCESS_TOKEN");
    }
    void write(const std::string& content, mode_t mode = 0600) const {
        std::ofstream(path_, std::ios::binary) << content;
        ::chmod(path_.c_str(), mode);
    }

    std::filesystem::path path_;
};

TEST_F(SessionTest, LoadsFromFile) {
    write(R"({"api_key": "key123", "access_token": "tok456"})");
    const auto loaded = load_session(path_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->session.api_key, "key123");
    EXPECT_EQ(loaded->session.access_token, "tok456");
    EXPECT_EQ(loaded->source, path_.string());
    EXPECT_FALSE(loaded->insecure_permissions);
}

TEST_F(SessionTest, FlagsFilesReadableByOthers) {
    write(R"({"api_key": "k", "access_token": "t"})", 0644);
    const auto loaded = load_session(path_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->insecure_permissions);
}

TEST_F(SessionTest, EnvironmentOverridesFile) {
    write(R"({"api_key": "file-key", "access_token": "file-token"})");
    ::setenv("MDA_KITE_API_KEY", "env-key", 1);
    ::setenv("MDA_KITE_ACCESS_TOKEN", "env-token", 1);
    const auto loaded = load_session(path_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->session.api_key, "env-key");
    EXPECT_EQ(loaded->source, "environment");
}

TEST_F(SessionTest, ReportsMissingOrBadFiles) {
    EXPECT_EQ(load_session(path_).error(), SessionError::NotFound);
    write("not json");
    EXPECT_EQ(load_session(path_).error(), SessionError::Malformed);
    write(R"({"api_key": "k"})");
    EXPECT_EQ(load_session(path_).error(), SessionError::MissingField);
    write(R"({"api_key": "k", "access_token": ""})");
    EXPECT_EQ(load_session(path_).error(), SessionError::MissingField);
}

TEST(RedactTest, ShowsOnlyAPrefix) {
    EXPECT_EQ(redact("abcdefghijkl"), "abcd…");
    EXPECT_EQ(redact("ab"), "ab…");
}

// --------------------------------------------------------------------------- responses

TEST(RestResponseTest, ClassifiesKiteErrors) {
    const auto expired = classify_error(
        403,
        R"({"status":"error","message":"Incorrect `api_key` or `access_token`.","error_type":"TokenException"})");
    EXPECT_EQ(expired.code, Code::AuthExpired);
    EXPECT_EQ(expired.http_status, 403);
    EXPECT_EQ(expired.message, "Incorrect `api_key` or `access_token`.");

    EXPECT_EQ(classify_error(400, R"({"error_type":"TokenException"})").code, Code::AuthExpired);
    EXPECT_EQ(classify_error(429, "{}").code, Code::RateLimited);
    EXPECT_EQ(classify_error(503, "<html>down</html>").code, Code::Server);
    EXPECT_EQ(classify_error(503, "<html>down</html>").message, "<html>down</html>");
    EXPECT_EQ(classify_error(404, R"({"message":"Route not found"})").code, Code::BadResponse);
}

TEST(RestResponseTest, ParsesProfile) {
    const auto profile = parse_profile(
        R"({"status":"success","data":{"user_id":"AB1234","user_name":"Test User","email":"x@y"}})");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->user_id, "AB1234");
    EXPECT_EQ(profile->user_name, "Test User");

    EXPECT_FALSE(parse_profile("garbage").has_value());
    EXPECT_FALSE(parse_profile(R"({"status":"success","data":{}})").has_value());
}

TEST(RestResponseTest, ParsesCircuitLimits) {
    const auto limits = parse_circuit_limits(R"({"status":"success","data":{
        "NSE:BAJFINANCE":{"instrument_token":81153,"last_price":926.45,
                          "lower_circuit_limit":833.4,"upper_circuit_limit":1018.6},
        "NFO:NIFTY26OCTFUT":{"instrument_token":12345,"lower_circuit_limit":20469,
                             "upper_circuit_limit":25017.6},
        "NSE:NIFTY 50":{"last_price":22711.0},
        "BAD":"x"}})");
    ASSERT_TRUE(limits.has_value()) << limits.error().message;
    ASSERT_EQ(limits->size(), 2u);  // the index has no token here: skipped
    const auto& baj = (*limits)[0].instrument_token == 81153 ? (*limits)[0] : (*limits)[1];
    EXPECT_EQ(baj.lower, core::Price::from_raw(8'334'000));
    EXPECT_EQ(baj.upper, core::Price::from_raw(10'186'000));
    EXPECT_EQ(baj.last_price, core::Price::from_raw(9'264'500));
    EXPECT_FALSE(parse_circuit_limits("not json").has_value());
}

TEST(RestResponseTest, ParsesCandlesWithAndWithoutOi) {
    const auto candles = parse_candles(R"({"status":"success","data":{"candles":[
        ["2026-10-21T09:15:00+0530", 23087, 23157, 23082.1, 23155.1, 35945, 3083275],
        ["2026-10-21T15:29:00+0530", 22738.45, 22811.35, 22738.45, 22811.35, 0]]}})");
    ASSERT_TRUE(candles.has_value()) << candles.error().message;
    ASSERT_EQ(candles->size(), 2u);
    EXPECT_EQ((*candles)[0].start, 1'792'554'300);  // 2026-10-21 09:15 IST = 03:45 UTC
    EXPECT_EQ((*candles)[0].low, core::Price::from_raw(230'821'000));
    EXPECT_EQ((*candles)[0].volume, 35'945);
    EXPECT_EQ((*candles)[0].oi, 3'083'275);
    EXPECT_EQ((*candles)[1].oi, 0);  // no OI column
    EXPECT_FALSE(parse_candles(R"({"data":{"candles":[["bad", 1, 2, 3, 4, 5]]}})").has_value());
    EXPECT_FALSE(parse_candles("{}").has_value());
}

TEST(QuoteKeyTest, UsesKiteSegments) {
    using instruments::Exchange;
    using instruments::Market;
    EXPECT_EQ(quote_key(Exchange::Nse, Market::Cash, "RELIANCE"), "NSE:RELIANCE");
    EXPECT_EQ(quote_key(Exchange::Bse, Market::Cash, "SENSEX"), "BSE:SENSEX");
    EXPECT_EQ(quote_key(Exchange::Nse, Market::Fo, "NIFTY26NOVFUT"), "NFO:NIFTY26NOVFUT");
    EXPECT_EQ(quote_key(Exchange::Bse, Market::Fo, "SENSEX26NOVFUT"), "BFO:SENSEX26NOVFUT");
}

TEST(TlsTest, BuildsContextFromCaBundleAndRejectsMissingFile) {
    const auto ca = default_ca_file();
    ASSERT_TRUE(ca.has_value()) << "no CA bundle on this machine";
    EXPECT_TRUE(make_tls_context(*ca).has_value());

    const auto missing = make_tls_context("/nonexistent/ca.pem");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, Code::Tls);
}

}  // namespace
}  // namespace mda::kite
