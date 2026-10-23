// End-to-end HTTP tests: a real mda-gateway server on 127.0.0.1:<free port> inside the
// test process, driven by a plain HTTP client.

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <mda/admin/admin.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/auth/user_store.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/catalog/catalog.hpp>
#include <mda/gateway/admin_api.hpp>
#include <mda/gateway/bars_api.hpp>
#include <mda/gateway/chain_api.hpp>
#include <mda/gateway/notifications_api.hpp>
#include <mda/gateway/oi_change_api.hpp>
#include <mda/gateway/server.hpp>
#include <mda/gateway/system_api.hpp>
#include <mda/gateway/usage_tracker.hpp>
#include <mda/gateway/user_api.hpp>
#include <mda/gateway/watchlist_api.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {
namespace {

namespace beast = boost::beast;
using asio::ip::tcp;

struct Reply {
    unsigned status = 0;
    boost::json::object body;
    std::string set_cookie;
};

class GatewayTest : public ::testing::Test {
protected:
    GatewayTest()
        : db_(storage::Database::open_in_memory().value())
        , store_(db_)
        , auth_(store_, {.argon2 = {.memory_kib = 256, .iterations = 1, .lanes = 1}})
        , lists_(db_)
        , usage_([] { return std::int64_t{1'792'640'000}; })
        , server_(io_.get_executor(), router_) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
        seed_catalog();
        auth_.set_token_observer([this](std::string_view token, std::int64_t user) {
            usage_.remember_token(token, user);
        });
        server_.on_response([this](const Request& request, std::size_t bytes) {
            usage_.meter_http(request, bytes);
        });
        register_user_routes(router_, auth_, worker_.get_executor());
        register_watchlist_routes(router_, auth_, lists_, db_, worker_.get_executor());
        register_admin_routes(router_, auth_, db_, worker_.get_executor(), usage_,
                              std::chrono::hours(24));
        register_bars_routes(router_, auth_, db_, worker_.get_executor(), bar_store_,
                             bars_worker_.get_executor());
        register_chain_routes(
            router_, auth_, db_, worker_.get_executor(), quotes_, summaries_,
            {{.underlying = "NIFTY", .spot = "NSE|CASH|IDX|NIFTY 50", .expiries = 2}});
        register_oi_change_routes(
            router_, auth_, db_, worker_.get_executor(), bar_store_, bars_worker_.get_executor(),
            {{.underlying = "NIFTY", .spot = "NSE|CASH|IDX|NIFTY 50", .expiries = 2}});
        register_notification_routes(router_, auth_, db_, worker_.get_executor(), "md_test_bot");
        register_system_routes(router_, auth_, db_, worker_.get_executor(), monitor_);
        EXPECT_FALSE(server_.listen({asio::ip::make_address("127.0.0.1"), 0}));
        port_ = server_.local_endpoint().port();
        io_thread_ = std::thread([this] { io_.run(); });
    }

    ~GatewayTest() override {
        asio::post(io_, [this] {
            server_.stop();
            io_.stop();
        });
        io_thread_.join();
        bars_worker_.join();
        worker_.join();
    }

    Reply send(http::verb verb, const std::string& target, const std::string& body = {},
               const std::string& session = {},
               const std::string& content_type = "application/json",
               const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        asio::io_context client_io;
        tcp::socket socket(client_io);
        socket.connect({asio::ip::make_address("127.0.0.1"), port_});

        http::request<http::string_body> request{verb, target, 11};
        request.set(http::field::host, "127.0.0.1");
        request.set(http::field::user_agent, "gateway-test");
        if (!body.empty()) {
            request.set(http::field::content_type, content_type);
            request.body() = body;
        }
        if (!session.empty()) {
            request.set(http::field::cookie, std::string(kSessionCookie) + "=" + session);
        }
        for (const auto& [name, value] : headers) {
            request.set(name, value);
        }
        request.prepare_payload();
        http::write(socket, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);

        Reply reply{.status = response.result_int(),
                    .set_cookie = std::string(response[http::field::set_cookie])};
        boost::system::error_code error;
        const auto parsed = boost::json::parse(response.body(), error);
        if (!error && parsed.is_object()) {
            reply.body = parsed.as_object();
        }
        return reply;
    }

    /// Instruments 1 RELIANCE, 2 INFY, 3 NIFTY 50 (index).
    void seed_catalog() {
        instruments::InstrumentRegistry registry;
        const auto add = [&](instruments::Kind kind, const char* symbol, const char* name) {
            const auto id = registry.assign({.exchange = instruments::Exchange::Nse,
                                             .market = instruments::Market::Cash,
                                             .kind = kind,
                                             .underlying = symbol});
            auto* instrument = registry.get(id);
            instrument->trading_symbol = symbol;
            instrument->name = name;
            instrument->lot_size = core::Quantity(1);
            instrument->tick_size = core::Price::from_raw(500);
            instrument->listed_today = true;
        };
        add(instruments::Kind::Equity, "RELIANCE", "RELIANCE INDUSTRIES");
        add(instruments::Kind::Equity, "INFY", "INFOSYS");
        add(instruments::Kind::Index, "NIFTY 50", "NIFTY 50");
        // NIFTY options, 27 Oct, strikes 22400..23000: ids 4.. (call, put per strike)
        using namespace std::chrono_literals;
        for (std::int64_t strike = 22'400; strike <= 23'000; strike += 100) {
            for (const auto type : {instruments::OptionType::Call, instruments::OptionType::Put}) {
                const auto id =
                    registry.assign({.exchange = instruments::Exchange::Nse,
                                     .market = instruments::Market::Fo,
                                     .kind = instruments::Kind::Option,
                                     .underlying = "NIFTY",
                                     .expiry = 2026y / 10 / 27,
                                     .strike = core::Price::from_raw(strike * core::Price::kScale),
                                     .option_type = type});
                auto* instrument = registry.get(id);
                instrument->trading_symbol = "NIFTY" + std::to_string(strike) +
                                             (type == instruments::OptionType::Call ? "CE" : "PE");
                instrument->name = "NIFTY";
                instrument->lot_size = core::Quantity(65);
                instrument->listed_today = true;
            }
        }
        EXPECT_TRUE(catalog::sync(db_, registry, 0).has_value());
    }

    static const boost::json::object& data(const Reply& reply) {
        return reply.body.at("data").as_object();
    }

    static std::string session_from(const std::string& set_cookie) {
        const std::string prefix = std::string(kSessionCookie) + "=";
        return set_cookie.substr(prefix.size(), set_cookie.find(';') - prefix.size());
    }

    static std::string error_type(const Reply& reply) {
        const auto* type = reply.body.if_contains("error_type");
        return type != nullptr ? std::string(type->as_string()) : "";
    }

    Reply signup(const std::string& user, const std::string& password,
                 const std::string& email = {}) {
        static int counter = 0;
        const std::string address =
            email.empty() ? "user" + std::to_string(++counter) + "@example.com" : email;
        return send(http::verb::post, "/api/v1/user/signup",
                    R"({"username":")" + user + R"(","email":")" + address +
                        R"(","phone":"98765 43210","password":")" + password + R"("})");
    }

    std::string signin(const std::string& user, const std::string& password) {
        const Reply reply =
            send(http::verb::post, "/api/v1/session",
                 R"({"username":")" + user + R"(","password":")" + password + R"("})");
        EXPECT_EQ(reply.status, 200u);
        return session_from(reply.set_cookie);
    }

    storage::Database db_;
    auth::UserStore store_;
    auth::AuthService auth_;
    watchlists::WatchlistService lists_;
    UsageTracker usage_;
    bars::BarStore bar_store_ = bars::BarStore::open_in_memory().value();
    QuoteCache quotes_;
    ChainSummaries summaries_;
    FeedMetrics feed_metrics_;
    // No pid files, buses or journal here: the monitor reports them missing.
    SystemMonitor monitor_{feed_metrics_,
                           {.run_dir = "/nonexistent/run",
                            .data_dir = std::filesystem::temp_directory_path(),
                            .journal_dir = "/nonexistent/journal",
                            .buses = {"gateway-test-no-bus"},
                            .start_thread = false}};
    asio::thread_pool bars_worker_{1};
    asio::io_context io_;
    asio::thread_pool worker_{1};
    Router router_;
    HttpServer server_;
    std::uint16_t port_ = 0;
    std::thread io_thread_;
};

TEST_F(GatewayTest, SignupSigninProfilePasswordSignout) {
    const Reply created = signup("siva", "correct horse battery");
    EXPECT_EQ(created.status, 201u);
    EXPECT_EQ(created.body.at("status").as_string(), "success");
    EXPECT_EQ(created.body.at("data").as_object().at("username").as_string(), "siva");

    const Reply signed_in = send(http::verb::post, "/api/v1/session",
                                 R"({"username":"siva","password":"correct horse battery"})");
    ASSERT_EQ(signed_in.status, 200u);
    EXPECT_NE(signed_in.set_cookie.find("HttpOnly"), std::string::npos);
    EXPECT_NE(signed_in.set_cookie.find("SameSite=Strict"), std::string::npos);
    EXPECT_EQ(signed_in.body.at("data").as_object().if_contains("token"), nullptr);  // cookie only
    const std::string session = session_from(signed_in.set_cookie);
    ASSERT_EQ(session.size(), 43u);

    const Reply profile = send(http::verb::get, "/api/v1/user/profile", {}, session);
    ASSERT_EQ(profile.status, 200u);
    EXPECT_EQ(profile.body.at("data").as_object().at("username").as_string(), "siva");
    EXPECT_EQ(profile.body.at("data").as_object().at("phone").as_string(), "+919876543210");
    EXPECT_FALSE(profile.body.at("data").as_object().at("email_verified").as_bool());
    EXPECT_TRUE(profile.body.at("data").as_object().at("created_at").as_string().ends_with("Z"));

    const Reply changed =
        send(http::verb::put, "/api/v1/user/password",
             R"({"current_password":"correct horse battery","new_password":"a brand new pass"})",
             session);
    EXPECT_EQ(changed.status, 200u);

    const Reply signed_out = send(http::verb::delete_, "/api/v1/session", {}, session);
    EXPECT_EQ(signed_out.status, 200u);
    EXPECT_NE(signed_out.set_cookie.find("Max-Age=0"), std::string::npos);
    EXPECT_EQ(send(http::verb::get, "/api/v1/user/profile", {}, session).status, 401u);
}

TEST_F(GatewayTest, PasswordChangeSignsOutOtherDevices) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string laptop = signin("siva", "correct horse battery");
    const std::string phone = signin("siva", "correct horse battery");
    ASSERT_EQ(
        send(http::verb::put, "/api/v1/user/password",
             R"({"current_password":"correct horse battery","new_password":"a brand new pass"})",
             laptop)
            .status,
        200u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/user/profile", {}, laptop).status, 200u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/user/profile", {}, phone).status, 401u);
}

TEST_F(GatewayTest, ErrorsUseTheKiteStyleEnvelope) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);

    const Reply duplicate = signup("SIVA", "another password");
    EXPECT_EQ(duplicate.status, 409u);
    EXPECT_EQ(duplicate.body.at("status").as_string(), "error");
    EXPECT_EQ(error_type(duplicate), "ConflictException");

    const Reply taken_email = signup("someone", "correct horse battery", "dup@example.com");
    EXPECT_EQ(taken_email.status, 201u);
    const Reply dup_email = signup("someone2", "correct horse battery", "DUP@example.com");
    EXPECT_EQ(dup_email.status, 409u);
    EXPECT_EQ(dup_email.body.at("message").as_string(), "email already registered");

    const Reply weak = signup("newuser", "short");
    EXPECT_EQ(weak.status, 400u);
    EXPECT_EQ(error_type(weak), "InputException");

    const Reply wrong = send(http::verb::post, "/api/v1/session",
                             R"({"username":"siva","password":"wrong password"})");
    EXPECT_EQ(wrong.status, 401u);
    EXPECT_EQ(error_type(wrong), "AuthException");
    EXPECT_TRUE(wrong.set_cookie.empty());

    const Reply anonymous = send(http::verb::get, "/api/v1/user/profile");
    EXPECT_EQ(anonymous.status, 401u);
    EXPECT_EQ(error_type(anonymous), "AuthException");
}

TEST_F(GatewayTest, RejectsMalformedRequests) {
    EXPECT_EQ(send(http::verb::post, "/api/v1/user/signup", "not json").status, 400u);
    EXPECT_EQ(send(http::verb::post, "/api/v1/user/signup", R"({"username":"x"})").status, 400u);
    // A form post (what a cross-site page can send without CORS) is refused.
    EXPECT_EQ(send(http::verb::post, "/api/v1/user/signup", "username=siva&password=correct+horse",
                   {}, "application/x-www-form-urlencoded")
                  .status,
              400u);

    const Reply missing = send(http::verb::get, "/api/v1/nope");
    EXPECT_EQ(missing.status, 404u);
    EXPECT_EQ(error_type(missing), "NotFoundException");
    EXPECT_EQ(send(http::verb::get, "/api/v1/user/signup").status, 405u);

    const Reply huge = send(http::verb::post, "/api/v1/user/signup",
                            R"({"username":")" + std::string(70'000, 'a') + R"("})");
    EXPECT_EQ(huge.status, 413u);
}

TEST_F(GatewayTest, SearchesInstruments) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");

    EXPECT_EQ(send(http::verb::get, "/api/v1/instruments/search?q=infy").status, 401u);
    const Reply found =
        send(http::verb::get, "/api/v1/instruments/search?q=reliance%20ind", {}, session);
    ASSERT_EQ(found.status, 200u);
    const auto& results = found.body.at("data").as_array();
    ASSERT_EQ(results.size(), 1u);
    const auto& reliance = results[0].as_object();
    EXPECT_EQ(reliance.at("instrument_id").as_int64(), 1);
    EXPECT_EQ(reliance.at("trading_symbol").as_string(), "RELIANCE");
    EXPECT_EQ(reliance.at("name").as_string(), "RELIANCE INDUSTRIES");
    EXPECT_DOUBLE_EQ(reliance.at("tick_size").as_double(), 0.05);

    const Reply one = send(http::verb::get, "/api/v1/instruments/3", {}, session);
    ASSERT_EQ(one.status, 200u);
    EXPECT_EQ(data(one).at("kind").as_string(), "IDX");
    EXPECT_EQ(send(http::verb::get, "/api/v1/instruments/99", {}, session).status, 404u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/instruments/abc", {}, session).status, 400u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/instruments/search?q=", {}, session)
                  .body.at("data")
                  .as_array()
                  .size(),
              0u);
}

TEST_F(GatewayTest, ManagesWatchlists) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");

    const Reply initial = send(http::verb::get, "/api/v1/watchlists", {}, session);
    ASSERT_EQ(initial.status, 200u);
    ASSERT_EQ(initial.body.at("data").as_array().size(), 1u);  // the default list
    const auto default_id = initial.body.at("data").as_array()[0].as_object().at("id").as_int64();

    const Reply created =
        send(http::verb::post, "/api/v1/watchlists", R"({"name":"Banks"})", session);
    ASSERT_EQ(created.status, 201u);
    const auto id = std::to_string(data(created).at("id").as_int64());
    const std::string items = "/api/v1/watchlists/" + id + "/items";

    ASSERT_EQ(send(http::verb::post, items, R"({"instrument_id":2})", session).status, 201u);
    const Reply added = send(http::verb::post, items, R"({"instrument_id":1})", session);
    ASSERT_EQ(added.status, 201u);
    const auto& list = data(added).at("items").as_array();
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].as_object().at("trading_symbol").as_string(), "INFY");
    EXPECT_EQ(send(http::verb::post, items, R"({"instrument_id":1})", session).status, 409u);
    EXPECT_EQ(send(http::verb::post, items, R"({"instrument_id":42})", session).status, 400u);

    const Reply renamed =
        send(http::verb::put, "/api/v1/watchlists/" + id, R"({"name":"PSU"})", session);
    ASSERT_EQ(renamed.status, 200u);
    EXPECT_EQ(data(renamed).at("name").as_string(), "PSU");

    EXPECT_EQ(send(http::verb::delete_, items + "/2", {}, session).status, 200u);
    EXPECT_EQ(send(http::verb::delete_, items + "/2", {}, session).status, 404u);
    EXPECT_EQ(send(http::verb::delete_, "/api/v1/watchlists/" + id, {}, session).status, 200u);

    const Reply after = send(http::verb::get, "/api/v1/watchlists", {}, session);
    ASSERT_EQ(after.body.at("data").as_array().size(), 1u);
    EXPECT_EQ(after.body.at("data").as_array()[0].as_object().at("id").as_int64(), default_id);
}

TEST_F(GatewayTest, WatchlistsArePrivate) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    ASSERT_EQ(signup("friend", "correct horse battery").status, 201u);
    const std::string siva = signin("siva", "correct horse battery");
    const std::string other = signin("friend", "correct horse battery");

    const Reply created = send(http::verb::post, "/api/v1/watchlists", R"({"name":"Mine"})", siva);
    const auto id = std::to_string(data(created).at("id").as_int64());
    EXPECT_EQ(send(http::verb::get, "/api/v1/watchlists").status, 401u);
    EXPECT_EQ(send(http::verb::post, "/api/v1/watchlists/" + id + "/items",
                   R"({"instrument_id":1})", other)
                  .status,
              404u);
    EXPECT_EQ(send(http::verb::delete_, "/api/v1/watchlists/" + id, {}, other).status, 404u);
    EXPECT_EQ(send(http::verb::put, "/api/v1/watchlists/" + id, R"({"name":"x"})", other).status,
              404u);
}

TEST_F(GatewayTest, DeletesTheAccount) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    ASSERT_EQ(send(http::verb::get, "/api/v1/watchlists", {}, session).status, 200u);

    EXPECT_EQ(send(http::verb::delete_, "/api/v1/user", R"({"password":"wrong password"})", session)
                  .status,
              401u);
    EXPECT_EQ(send(http::verb::delete_, "/api/v1/user", "{}", session).status, 400u);
    const Reply deleted = send(http::verb::delete_, "/api/v1/user",
                               R"({"password":"correct horse battery"})", session);
    EXPECT_EQ(deleted.status, 200u);
    EXPECT_NE(deleted.set_cookie.find("Max-Age=0"), std::string::npos);
    EXPECT_EQ(send(http::verb::get, "/api/v1/user/profile", {}, session).status, 401u);
    EXPECT_EQ(send(http::verb::post, "/api/v1/session",
                   R"({"username":"siva","password":"correct horse battery"})")
                  .status,
              401u);
}

TEST_F(GatewayTest, GroupsAndOrderRoutes) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    const auto list_id = std::to_string(send(http::verb::get, "/api/v1/watchlists", {}, session)
                                            .body.at("data")
                                            .as_array()[0]
                                            .as_object()
                                            .at("id")
                                            .as_int64());
    const std::string base = "/api/v1/watchlists/" + list_id;

    const Reply grouped =
        send(http::verb::post, base + "/groups", R"({"name":"Indices"})", session);
    ASSERT_EQ(grouped.status, 201u);
    const auto group_id = data(grouped).at("groups").as_array()[0].as_object().at("id").as_int64();

    const Reply added =
        send(http::verb::post, base + "/items",
             R"({"instrument_id":3,"group_id":)" + std::to_string(group_id) + "}", session);
    ASSERT_EQ(added.status, 201u);
    EXPECT_EQ(data(added).at("items").as_array()[0].as_object().at("group_id").as_int64(),
              group_id);
    ASSERT_EQ(send(http::verb::post, base + "/items", R"({"instrument_id":1})", session).status,
              201u);

    // Ungroup NIFTY 50 and put it after RELIANCE.
    const Reply ordered =
        send(http::verb::put, base + "/order",
             R"({"groups":[)" + std::to_string(group_id) +
                 R"(],"items":[{"instrument_id":1,"group_id":null},{"instrument_id":3}]})",
             session);
    ASSERT_EQ(ordered.status, 200u);
    const auto& items = data(ordered).at("items").as_array();
    EXPECT_EQ(items[0].as_object().at("trading_symbol").as_string(), "RELIANCE");
    EXPECT_TRUE(items[1].as_object().at("group_id").is_null());
    EXPECT_EQ(send(http::verb::put, base + "/order", R"({"groups":[],"items":[]})", session).status,
              409u);  // stale arrangement
    EXPECT_EQ(send(http::verb::put, base + "/order", R"({"items":"x"})", session).status, 400u);

    EXPECT_EQ(send(http::verb::put, base + "/groups/" + std::to_string(group_id),
                   R"({"name":"Idx"})", session)
                  .status,
              200u);
    const Reply removed =
        send(http::verb::delete_, base + "/groups/" + std::to_string(group_id), {}, session);
    ASSERT_EQ(removed.status, 200u);
    EXPECT_TRUE(data(removed).at("groups").as_array().empty());
    EXPECT_EQ(data(removed).at("items").as_array().size(), 2u);
}

TEST(MaskTest, HidesMostOfTheContactDetails) {
    EXPECT_EQ(mask_email("siva@example.com"), "si***@example.com");
    EXPECT_EQ(mask_email("a@b.in"), "a***@b.in");
    EXPECT_EQ(mask_phone("+919876543210"), "+91•••••43210");
    EXPECT_EQ(mask_email(""), "");
}

TEST(UsageTrackerTest, AttributesKnownTokensAndTakesCounts) {
    UsageTracker usage([] { return std::int64_t{1'792'640'000}; });
    Request request{http::verb::get, "/api/v1/watchlists", 11};
    request.set(http::field::cookie, std::string(kSessionCookie) + "=tok");
    usage.meter_http(request, 500);  // unknown token: not counted
    usage.remember_token("tok", 7);
    usage.meter_http(request, 300);
    usage.meter_ws(7, 120);
    usage.meter_ws(7, 80);
    EXPECT_EQ(
        usage.unsaved_today(7),
        (admin::Usage{.ws_bytes = 200, .ws_messages = 2, .http_bytes = 300, .http_requests = 1}));

    const auto connection = usage.open_connection(7);
    usage.set_instruments(connection, 12);
    ASSERT_EQ(usage.connections().size(), 1u);
    EXPECT_EQ(usage.connections()[0].instruments, 12u);
    usage.close_connection(connection);
    EXPECT_TRUE(usage.connections().empty());

    const auto taken = usage.take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].day, "2026-10-22");
    EXPECT_EQ(usage.unsaved_today(7), admin::Usage{});
}

TEST_F(GatewayTest, AdminRoutesNeedTheAdminRole) {
    ASSERT_EQ(signup("siva", "correct horse battery", "siva@example.com").status, 201u);
    ASSERT_EQ(signup("friend", "correct horse battery").status, 201u);
    const std::string friend_session = signin("friend", "correct horse battery");
    EXPECT_EQ(send(http::verb::get, "/api/v1/admin/users").status, 401u);
    const Reply denied = send(http::verb::get, "/api/v1/admin/users", {}, friend_session);
    EXPECT_EQ(denied.status, 403u);
    EXPECT_EQ(error_type(denied), "PermissionException");

    ASSERT_TRUE(admin::set_role(db_, "siva", "admin").value());
    const std::string admin_session = signin("siva", "correct horse battery");
    ASSERT_EQ(send(http::verb::get, "/api/v1/watchlists", {}, friend_session).status,
              200u);  // metered

    const Reply users = send(http::verb::get, "/api/v1/admin/users", {}, admin_session);
    ASSERT_EQ(users.status, 200u);
    const auto& list = users.body.at("data").as_array();
    ASSERT_EQ(list.size(), 2u);
    const auto& siva = list[0].as_object();
    EXPECT_EQ(siva.at("role").as_string(), "admin");
    EXPECT_EQ(siva.at("email").as_string(), "si***@example.com");  // masked
    EXPECT_FALSE(siva.at("last_sign_in_at").is_null());
    const auto& friend_row = list[1].as_object();
    EXPECT_EQ(friend_row.at("active_sessions").as_int64(), 1);
    EXPECT_GT(friend_row.at("today").as_object().at("http_bytes").as_int64(), 0);
    EXPECT_GE(friend_row.at("today").as_object().at("http_requests").as_int64(), 1);
}

TEST_F(GatewayTest, AdminSeesDetailAndCanForceSignOut) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    ASSERT_EQ(signup("friend", "correct horse battery").status, 201u);
    ASSERT_TRUE(admin::set_role(db_, "siva", "admin").value());
    const std::string admin_session = signin("siva", "correct horse battery");
    // From a loopback peer, X-Forwarded-For names the client; a client-sent internal
    // header is ignored.
    const Reply signed_in =
        send(http::verb::post, "/api/v1/session",
             R"({"username":"friend","password":"correct horse battery"})", {}, "application/json",
             {{"X-Forwarded-For", "203.0.113.9, 10.0.0.1"},
              {std::string(kClientIpHeader), "6.6.6.6"},
              {"User-Agent", "FriendPhone"}});
    ASSERT_EQ(signed_in.status, 200u);
    const std::string friend_session = session_from(signed_in.set_cookie);
    ASSERT_EQ(
        send(http::verb::post, "/api/v1/session", R"({"username":"friend","password":"nope nope"})")
            .status,
        401u);

    const Reply detail = send(http::verb::get, "/api/v1/admin/users/2", {}, admin_session);
    ASSERT_EQ(detail.status, 200u);
    const auto& data = detail.body.at("data").as_object();
    const auto& sessions = data.at("sessions").as_array();
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].as_object().at("ip").as_string(), "203.0.113.9");
    EXPECT_EQ(sessions[0].as_object().at("user_agent").as_string(), "FriendPhone");
    const auto& events = data.at("events").as_array();
    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events[0].as_object().at("type").as_string(), "sign_in_failed");
    EXPECT_EQ(events[1].as_object().at("type").as_string(), "sign_in");

    const std::string id(sessions[0].as_object().at("id").as_string());
    EXPECT_EQ(send(http::verb::delete_, "/api/v1/admin/sessions/" + id, {}, admin_session).status,
              200u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/user/profile", {}, friend_session).status, 401u);
    EXPECT_EQ(send(http::verb::delete_, "/api/v1/admin/sessions/" + id, {}, admin_session).status,
              404u);
    EXPECT_EQ(send(http::verb::delete_, "/api/v1/admin/sessions/xyz", {}, admin_session).status,
              400u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/admin/users/99", {}, admin_session).status, 404u);
}

TEST_F(GatewayTest, ServesAggregatedBarHistory) {
    constexpr std::int64_t k0915 = 1'792'607'400 + 9 * 3600 + 15 * 60;  // 2026-10-22 09:15 IST
    std::vector<bars::Bar> minutes;
    for (int m = 0; m < 7; ++m) {
        bars::Bar bar{};
        bar.header = messages::make_header<bars::Bar>(core::InstrumentId(1), 0);
        bar.start = k0915 + m * 60;
        bar.open = bar.close = core::Price::from_raw((100 + m) * core::Price::kScale);
        bar.high = core::Price::from_raw((101 + m) * core::Price::kScale);
        bar.low = core::Price::from_raw((99 + m) * core::Price::kScale);
        bar.volume = core::Quantity(10);
        bar.oi_open = bar.oi_high = bar.oi_low = bar.oi_close = core::Quantity(1000 + m);
        minutes.push_back(bar);
    }
    ASSERT_TRUE(bar_store_.write(minutes).has_value());
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    const std::string range =
        "&from=" + std::to_string(k0915) + "&to=" + std::to_string(k0915 + 3600);

    EXPECT_EQ(send(http::verb::get, "/api/v1/instruments/1/bars?interval=5m" + range).status, 401u);
    const Reply reply =
        send(http::verb::get, "/api/v1/instruments/1/bars?interval=5m" + range, {}, session);
    ASSERT_EQ(reply.status, 200u);
    const auto& rows = data(reply).at("bars").as_array();
    ASSERT_EQ(rows.size(), 2u);  // 09:15-09:19 and 09:20-09:21
    const auto& first = rows[0].as_array();
    EXPECT_EQ(first[0].as_int64(), k0915);
    EXPECT_DOUBLE_EQ(first[1].as_double(), 100.0);  // open
    EXPECT_DOUBLE_EQ(first[2].as_double(), 105.0);  // high of 09:19
    EXPECT_DOUBLE_EQ(first[4].as_double(), 104.0);  // close
    EXPECT_EQ(first[5].as_int64(), 50);             // volume
    EXPECT_EQ(first[6].as_int64(), 1004);           // OI at the close

    EXPECT_EQ(send(http::verb::get, "/api/v1/instruments/1/bars?interval=7m", {}, session).status,
              400u);
    EXPECT_EQ(
        data(send(http::verb::get, "/api/v1/instruments/2/bars?interval=1m" + range, {}, session))
            .at("bars")
            .as_array()
            .size(),
        0u);
}

TEST_F(GatewayTest, ClassifiesBuildupFromTheLastOiUpdateIncludingBeforeTheRange) {
    constexpr std::int64_t k0915 = 1'792'607'400 + 9 * 3600 + 15 * 60;  // 2026-10-22 09:15 IST
    constexpr std::uint32_t kOption = 4;                                // NIFTY 22400 CE, lot 65
    const auto minute = [](std::int64_t start, std::int64_t close, std::int64_t oi,
                           std::uint32_t updates, bars::BarSource source = bars::BarSource::Live) {
        bars::Bar bar{};
        bar.header = messages::make_header<bars::Bar>(core::InstrumentId(kOption), 0);
        bar.start = start;
        bar.open = bar.high = bar.low = bar.close =
            core::Price::from_raw(close * core::Price::kScale);
        bar.oi_open = bar.oi_high = bar.oi_low = bar.oi_close = core::Quantity(oi);
        bar.oi_updates = updates;
        bar.source = source;
        return bar;
    };
    std::vector<bars::Bar> minutes{
        minute(k0915 - (17 * 3600 + 46 * 60), 100, 100'000,
               1),  // 15:29 the day before: the reference
        minute(k0915, 110, 101'000, 1),
        minute(k0915 + 60, 108, 101'000, 0),
        minute(k0915 + 120, 105, 101'050, 1),  // +50 < 0.1% of 101,000: neutral
        minute(k0915 + 180, 100, 100'500, 0, bars::BarSource::Kite),  // backfill: OI changed
    };
    minutes[3].oi_update_offset = 30;
    ASSERT_TRUE(bar_store_.write(minutes).has_value());
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    const std::string range =
        "&from=" + std::to_string(k0915) + "&to=" + std::to_string(k0915 + 3600);
    const auto rows_for = [&](const std::string& interval) {
        const Reply reply =
            send(http::verb::get, "/api/v1/instruments/4/bars?interval=" + interval + range, {},
                 session);
        EXPECT_EQ(reply.status, 200u);
        return data(reply).at("bars").as_array();
    };

    const auto one = rows_for("1m");
    ASSERT_EQ(one.size(), 4u);  // the reference minute is read but not returned
    std::vector<std::string> states;
    for (const auto& row : one) {
        const auto& state = row.as_array().at(12);
        states.push_back(state.is_null() ? "" : std::string(state.as_string()));
    }
    EXPECT_EQ(states, (std::vector<std::string>{"long_buildup", "no_oi_update", "neutral",
                                                "long_unwinding"}));
    const auto& third = one[2].as_array();
    EXPECT_EQ(third[7].as_int64(), 101'050);  // oi_open
    EXPECT_EQ(third[10].as_int64(), 1);       // oi_updates
    EXPECT_EQ(third[11].as_int64(), k0915 + 150);
    EXPECT_TRUE(one[1].as_array()[11].is_null());
    EXPECT_EQ(one[0].as_array()[13].as_int64(), 1'000);  // ΔOI from the day before's 100,000
    EXPECT_TRUE(one[1].as_array()[13].is_null());        // no OI update
    EXPECT_EQ(third[13].as_int64(), 50);                 // neutral: below the threshold

    const auto five = rows_for("5m");
    ASSERT_EQ(five.size(), 1u);
    EXPECT_EQ(five[0].as_array().at(12).as_string(), "long_buildup");  // 100 -> 100, OI +500
}

TEST_F(GatewayTest, VerifiesTheEmailWithAMailedCode) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    EXPECT_EQ(send(http::verb::get, "/api/v1/notifications").status, 401u);

    auto channels = data(send(http::verb::get, "/api/v1/notifications", {}, session));
    EXPECT_FALSE(channels.at("email").as_object().at("verified").as_bool());
    EXPECT_TRUE(channels.at("email").as_object().at("enabled").as_bool());
    EXPECT_NE(channels.at("email").as_object().at("address").as_string().find("***@"),
              boost::json::string::npos);
    EXPECT_TRUE(channels.at("telegram").as_object().at("available").as_bool());
    EXPECT_FALSE(channels.at("telegram").as_object().at("linked").as_bool());

    ASSERT_EQ(send(http::verb::post, "/api/v1/notifications/email/code", "{}", session).status,
              200u);
    EXPECT_EQ(send(http::verb::post, "/api/v1/notifications/email/code", "{}", session).status,
              429u);
    // The code travels in the outbox row the notifier will mail.
    auto row = db_.prepare("SELECT payload FROM notifications WHERE type = 'account.email_code'");
    ASSERT_EQ(row->step().value(), storage::StepResult::Row);
    const auto code =
        std::string(boost::json::parse(row->column_text(0)).as_object().at("code").as_string());

    const std::string wrong = code == "000000" ? "111111" : "000000";
    EXPECT_EQ(send(http::verb::post, "/api/v1/notifications/email/verify",
                   R"({"code":")" + wrong + R"("})", session)
                  .status,
              400u);
    EXPECT_EQ(
        send(http::verb::post, "/api/v1/notifications/email/verify", R"({"code":"12"})", session)
            .status,
        400u);
    EXPECT_EQ(send(http::verb::post, "/api/v1/notifications/email/verify",
                   R"({"code":")" + code + R"("})", session)
                  .status,
              200u);
    channels = data(send(http::verb::get, "/api/v1/notifications", {}, session));
    EXPECT_TRUE(channels.at("email").as_object().at("verified").as_bool());
    EXPECT_EQ(send(http::verb::post, "/api/v1/notifications/email/code", "{}", session).status,
              409u);
}

TEST_F(GatewayTest, LinksTelegramAndSwitchesChannels) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    const Reply link = send(http::verb::post, "/api/v1/notifications/telegram/link", "{}", session);
    ASSERT_EQ(link.status, 200u);
    EXPECT_TRUE(std::string(data(link).at("url").as_string())
                    .starts_with("https://t.me/md_test_bot?start="));

    EXPECT_EQ(
        send(http::verb::put, "/api/v1/notifications/telegram", R"({"enabled":false})", session)
            .status,
        404u);
    // What the bot does on "/start <code>" (notify::TelegramLinker), done directly here.
    auto linked = db_.prepare(
        "INSERT INTO user_channels (user_id, channel, address, enabled, linked_at) "
        "SELECT id, 'telegram', '555', 1, 0 FROM users WHERE username = 'siva'");
    ASSERT_TRUE(linked->run().has_value());
    EXPECT_EQ(
        send(http::verb::put, "/api/v1/notifications/telegram", R"({"enabled":false})", session)
            .status,
        200u);
    EXPECT_EQ(send(http::verb::put, "/api/v1/notifications/email", R"({"enabled":false})", session)
                  .status,
              200u);
    auto channels = data(send(http::verb::get, "/api/v1/notifications", {}, session));
    EXPECT_TRUE(channels.at("telegram").as_object().at("linked").as_bool());
    EXPECT_FALSE(channels.at("telegram").as_object().at("enabled").as_bool());
    EXPECT_FALSE(channels.at("email").as_object().at("enabled").as_bool());

    EXPECT_EQ(send(http::verb::delete_, "/api/v1/notifications/telegram", {}, session).status,
              200u);
    channels = data(send(http::verb::get, "/api/v1/notifications", {}, session));
    EXPECT_FALSE(channels.at("telegram").as_object().at("linked").as_bool());
    EXPECT_EQ(
        send(http::verb::put, "/api/v1/notifications/sms", R"({"enabled":true})", session).status,
        400u);
    EXPECT_EQ(send(http::verb::put, "/api/v1/notifications/email", R"({"enabled":"yes"})", session)
                  .status,
              400u);
}

TEST_F(GatewayTest, ServesChainsAndStrikeRowsAroundAtm) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");

    const Reply chains = send(http::verb::get, "/api/v1/chains", {}, session);
    ASSERT_EQ(chains.status, 200u);
    const auto& nifty = chains.body.at("data").as_array()[0].as_object();
    EXPECT_EQ(nifty.at("underlying").as_string(), "NIFTY");
    EXPECT_EQ(nifty.at("spot_id").as_int64(), 3);
    EXPECT_EQ(nifty.at("expiries").as_array()[0].as_string(), "2026-10-27");

    // No summary yet: ATM from the spot's last price (22,640 -> 22,600).
    messages::MarketSnapshot spot{};
    spot.header = messages::make_header<messages::MarketSnapshot>(core::InstrumentId(3), 1);
    spot.fields = messages::kFieldLtp;
    spot.ltp = core::Price::from_raw(22'640 * core::Price::kScale);
    quotes_.update(spot);
    Reply chain = send(http::verb::get, "/api/v1/chains/NIFTY/2026-10-27?strikes=2", {}, session);
    ASSERT_EQ(chain.status, 200u);
    EXPECT_DOUBLE_EQ(data(chain).at("atm_strike").as_double(), 22'600);
    EXPECT_TRUE(data(chain).at("summary").is_null());
    EXPECT_EQ(data(chain).at("lot_size").as_int64(), 65);
    const auto& rows = data(chain).at("rows").as_array();
    ASSERT_EQ(rows.size(), 5u);  // ATM ± 2
    EXPECT_DOUBLE_EQ(rows[0].as_object().at("strike").as_double(), 22'400);
    EXPECT_EQ(rows[0].as_object().at("call").as_int64(), 4);
    EXPECT_EQ(rows[0].as_object().at("put").as_int64(), 5);

    // With the engine's summary, ATM follows its forward-based ATM.
    options::ChainSummary summary{
        .underlying = "NIFTY",
        .spot = core::InstrumentId(3),
        .expiry = std::chrono::year_month_day{std::chrono::year{2026} / 10 / 27},
        .forward = 22'713.45,
        .atm_strike = 22'700,
        .atm_iv = 0.1397,
        .pcr = 0.89,
        .max_pain = 22'800,
        .years = 5.0 / 365,
        .call_oi = 10,
        .put_oi = 9};
    summaries_.update(options::to_message(summary));
    chain = send(http::verb::get, "/api/v1/chains/NIFTY/2026-10-27?strikes=1", {}, session);
    EXPECT_DOUBLE_EQ(data(chain).at("atm_strike").as_double(), 22'700);
    const auto& s = data(chain).at("summary").as_object();
    EXPECT_DOUBLE_EQ(s.at("atm_iv").as_double(), 13.97);
    EXPECT_DOUBLE_EQ(s.at("forward").as_double(), 22'713.45);
    EXPECT_DOUBLE_EQ(s.at("days_to_expiry").as_double(), 5.0);

    EXPECT_EQ(send(http::verb::get, "/api/v1/chains/BANKNIFTY/2026-10-27", {}, session).status,
              404u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/chains/NIFTY/2026-12-31", {}, session).status, 404u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/chains").status, 401u);
}

TEST_F(GatewayTest, ServesOiChangeByStrike) {
    // 2026-10-22, NIFTY 50 = 3; options 27 Oct: 22400 CE 4 / PE 5, 22500 6 / 7, 22600 8 / 9,
    // 22700 10 / 11, 22800 12 / 13, 22900 14 / 15, 23000 16 / 17.
    constexpr std::int64_t k0915 = 1'792'607'400 + 9 * 3600 + 15 * 60;
    constexpr std::int64_t kYesterday1529 = k0915 - 86'400 + 6 * 3600 + 14 * 60;
    const auto bar = [](std::uint32_t id, std::int64_t start, double close, std::int64_t oi,
                        std::uint16_t oi_offset = bars::kNoOiUpdateTime) {
        bars::Bar b{};
        b.header = messages::make_header<bars::Bar>(core::InstrumentId(id), 0);
        b.start = start;
        b.open = b.high = b.low = b.close =
            core::Price::from_raw(static_cast<std::int64_t>(close * core::Price::kScale));
        b.oi_open = b.oi_high = b.oi_low = b.oi_close = core::Quantity(oi);
        b.oi_updates = oi > 0 ? 1 : 0;
        b.oi_update_offset = oi_offset;
        return b;
    };
    ASSERT_TRUE(
        bar_store_
            .write(std::vector<bars::Bar>{
                bar(3, kYesterday1529, 22'600, 0), bar(3, k0915, 22'640, 0),
                bar(3, k0915 + 45 * 60, 22'710, 0), bar(9, kYesterday1529, 10, 1'000),
                bar(9, k0915 + 15 * 60, 12, 1'500, 42),                              // 22600 PE
                bar(10, k0915, 50, 2'000), bar(10, k0915 + 44 * 60, 55, 2'600, 30),  // 22700 CE
                bar(12, kYesterday1529, 30, 3'000),
                bar(12, k0915 + 30 * 60, 25, 2'500, 7)})  // 22800 CE
            .has_value());
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    const std::string base = "/api/v1/chains/NIFTY/oi-change";

    // Defaults: nearest expiry, from the open to the end of the spot's last minute (10:01).
    const Reply reply = send(http::verb::get, base + "?strikes=2", {}, session);
    ASSERT_EQ(reply.status, 200u);
    const auto& d = data(reply);
    EXPECT_EQ(d.at("expiries").as_array()[0].as_string(), "2026-10-27");
    EXPECT_EQ(d.at("from").as_int64(), k0915);
    EXPECT_EQ(d.at("to").as_int64(), k0915 + 46 * 60);
    EXPECT_EQ(d.at("session_open").as_int64(), k0915);
    EXPECT_EQ(d.at("latest").as_int64(), k0915 + 46 * 60);
    EXPECT_DOUBLE_EQ(d.at("spot_from").as_double(), 22'600);  // yesterday's close
    EXPECT_DOUBLE_EQ(d.at("spot_to").as_double(), 22'710);
    EXPECT_DOUBLE_EQ(d.at("atm_strike").as_double(), 22'700);
    EXPECT_EQ(d.at("oi_as_of").as_int64(), k0915 + 44 * 60 + 30);
    const auto& rows = d.at("rows").as_array();
    ASSERT_EQ(rows.size(), 5u);  // 22500 .. 22900
    const auto row = [&](double strike) -> const boost::json::object& {
        for (const auto& r : rows) {
            if (r.as_object().at("strike").as_double() == strike) {
                return r.as_object();
            }
        }
        throw std::runtime_error("no row");
    };
    EXPECT_EQ(row(22'600).at("put_from").as_int64(), 1'000);
    EXPECT_EQ(row(22'600).at("put_to").as_int64(), 1'500);
    EXPECT_TRUE(row(22'600).at("call_from").is_null());
    // 22700 CE has no OI that ended by 09:15 (its first bar ends 09:16): left out of the totals.
    EXPECT_TRUE(row(22'700).at("call_from").is_null());
    EXPECT_EQ(row(22'700).at("call_to").as_int64(), 2'600);
    EXPECT_EQ(row(22'800).at("call_to").as_int64(), 2'500);
    const auto& totals = d.at("totals").as_object();
    EXPECT_EQ(totals.at("call_from").as_int64(), 3'000);
    EXPECT_EQ(totals.at("call_to").as_int64(), 2'500);
    EXPECT_EQ(totals.at("put_from").as_int64(), 1'000);
    EXPECT_EQ(totals.at("put_to").as_int64(), 1'500);
    EXPECT_DOUBLE_EQ(d.at("pcr").as_double(), 0.6);

    // An explicit window, snapped to minutes: 09:16 .. 09:31.
    const Reply window =
        send(http::verb::get,
             base + "?expiries=2026-10-27&strikes=1&from=" + std::to_string(k0915 + 75) +
                 "&to=" + std::to_string(k0915 + 16 * 60 + 5),
             {}, session);
    ASSERT_EQ(window.status, 200u);
    EXPECT_EQ(data(window).at("from").as_int64(), k0915 + 60);
    EXPECT_EQ(data(window).at("rows").as_array().size(), 3u);
    for (const auto& r : data(window).at("rows").as_array()) {
        if (r.as_object().at("strike").as_double() == 22'700) {
            EXPECT_EQ(r.as_object().at("call_from").as_int64(), 2'000);
            EXPECT_EQ(r.as_object().at("call_to").as_int64(), 2'000);
        }
    }

    // The last 30 minutes before the latest minute (10:01): from 09:31.
    const Reply last = send(http::verb::get, base + "?minutes=30", {}, session);
    ASSERT_EQ(last.status, 200u);
    EXPECT_EQ(data(last).at("from").as_int64(), k0915 + 16 * 60);
    EXPECT_EQ(data(last).at("to").as_int64(), k0915 + 46 * 60);
    // Longer than the session so far: from the open.
    EXPECT_EQ(data(send(http::verb::get, base + "?minutes=600", {}, session)).at("from").as_int64(),
              k0915);

    EXPECT_EQ(send(http::verb::get, base + "?minutes=0", {}, session).status, 400u);
    EXPECT_EQ(send(http::verb::get, base + "?minutes=5&from=" + std::to_string(k0915), {}, session)
                  .status,
              400u);
    EXPECT_EQ(send(http::verb::get, base + "?expiries=2026-12-31", {}, session).status, 400u);
    EXPECT_EQ(send(http::verb::get, base + "?strikes=30", {}, session).status, 400u);
    EXPECT_EQ(send(http::verb::get, base + "?from=x", {}, session).status, 400u);
    EXPECT_EQ(send(http::verb::get,
                   base + "?from=" + std::to_string(k0915 + 600) + "&to=" + std::to_string(k0915),
                   {}, session)
                  .status,
              400u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/chains/BANKNIFTY/oi-change", {}, session).status,
              404u);
    EXPECT_EQ(send(http::verb::get, base).status, 401u);
    // The strike-rows route still answers for a real expiry.
    EXPECT_EQ(send(http::verb::get, "/api/v1/chains/NIFTY/2026-10-27", {}, session).status, 200u);
}

TEST_F(GatewayTest, ServesTheSessionsOiTimeline) {
    // The same session as above: minute index m = value from 09:15 + m minutes.
    constexpr std::int64_t k0915 = 1'792'607'400 + 9 * 3600 + 15 * 60;
    constexpr std::int64_t kYesterday1529 = k0915 - 86'400 + 6 * 3600 + 14 * 60;
    const auto bar = [](std::uint32_t id, std::int64_t start, double close, std::int64_t oi,
                        std::uint16_t oi_offset = bars::kNoOiUpdateTime) {
        bars::Bar b{};
        b.header = messages::make_header<bars::Bar>(core::InstrumentId(id), 0);
        b.start = start;
        b.open = b.high = b.low = b.close =
            core::Price::from_raw(static_cast<std::int64_t>(close * core::Price::kScale));
        b.oi_open = b.oi_high = b.oi_low = b.oi_close = core::Quantity(oi);
        b.oi_updates = oi > 0 ? 1 : 0;
        b.oi_update_offset = oi_offset;
        return b;
    };
    bars::Bar carried = bar(10, k0915 + 20 * 60, 51, 2'000);  // OI carried, no update: not a point
    carried.oi_updates = 0;
    ASSERT_TRUE(bar_store_
                    .write(std::vector<bars::Bar>{
                        bar(3, kYesterday1529, 22'600, 0), bar(3, k0915, 22'640, 0),
                        bar(3, k0915 + 45 * 60, 22'710, 0), bar(9, kYesterday1529, 10, 1'000),
                        bar(9, k0915 + 15 * 60, 12, 1'500, 42), bar(10, k0915, 50, 2'000), carried,
                        bar(10, k0915 + 44 * 60, 55, 2'600, 30), bar(12, kYesterday1529, 30, 3'000),
                        bar(12, k0915 + 30 * 60, 25, 2'500, 7)})
                    .has_value());
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");

    const Reply reply =
        send(http::verb::get, "/api/v1/chains/NIFTY/oi-timeline?strikes=1", {}, session);
    ASSERT_EQ(reply.status, 200u);
    const auto& d = data(reply);
    EXPECT_EQ(d.at("session_open").as_int64(), k0915);
    EXPECT_EQ(d.at("latest").as_int64(), k0915 + 46 * 60);
    EXPECT_EQ(d.at("oi_as_of").as_int64(), k0915 + 44 * 60 + 30);
    EXPECT_DOUBLE_EQ(d.at("atm_strike").as_double(), 22'700);
    const auto points = [](const boost::json::value& series) {
        std::vector<std::pair<std::int64_t, double>> out;
        for (const auto& p : series.as_array()) {
            out.emplace_back(p.as_array()[0].as_int64(), p.as_array()[1].to_number<double>());
        }
        return out;
    };
    using P = std::vector<std::pair<std::int64_t, double>>;
    EXPECT_EQ(points(d.at("spot")), (P{{0, 22'600}, {1, 22'640}, {46, 22'710}}));
    const auto& rows = d.at("rows").as_array();
    ASSERT_EQ(rows.size(), 3u);  // 22600 .. 22800
    const auto& r22600 = rows[0].as_object();
    const auto& r22700 = rows[1].as_object();
    const auto& r22800 = rows[2].as_object();
    EXPECT_EQ(points(r22600.at("put")), (P{{0, 1'000}, {16, 1'500}}));
    EXPECT_TRUE(r22600.at("call").as_array().empty());
    EXPECT_EQ(points(r22700.at("call")), (P{{1, 2'000}, {45, 2'600}}));  // no value before 09:16
    EXPECT_EQ(points(r22800.at("call")), (P{{0, 3'000}, {31, 2'500}}));

    EXPECT_EQ(
        send(http::verb::get, "/api/v1/chains/NIFTY/oi-timeline?strikes=0", {}, session).status,
        400u);
    EXPECT_EQ(
        send(http::verb::get, "/api/v1/chains/NIFTY/oi-timeline?expiries=2026-12-31", {}, session)
            .status,
        400u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/chains/BANKNIFTY/oi-timeline", {}, session).status,
              404u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/chains/NIFTY/oi-timeline").status, 401u);
}

TEST_F(GatewayTest, ServesTheSystemMonitorToAdminsOnly) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    ASSERT_EQ(signup("friend", "correct horse battery").status, 201u);
    ASSERT_TRUE(admin::set_role(db_, "siva", "admin").value());
    const std::string admin_session = signin("siva", "correct horse battery");
    const std::string friend_session = signin("friend", "correct horse battery");

    EXPECT_EQ(send(http::verb::get, "/api/v1/admin/system").status, 401u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/admin/system", {}, friend_session).status, 403u);
    // Before the first sample there is nothing to show.
    EXPECT_EQ(send(http::verb::get, "/api/v1/admin/system", {}, admin_session).status, 503u);

    monitor_.sample();
    const Reply reply = send(http::verb::get, "/api/v1/admin/system", {}, admin_session);
    ASSERT_EQ(reply.status, 200u);
    const auto& d = data(reply);
    for (const char* key : {"time", "processes", "machine", "disk", "buses", "feed", "latency",
                            "engine", "recorder", "notifier", "history", "checks", "status"}) {
        EXPECT_TRUE(d.contains(key)) << key;
    }
    EXPECT_EQ(d.at("processes").as_array().size(), 6u);
    EXPECT_FALSE(d.at("buses").as_array()[0].as_object().at("available").as_bool());
    EXPECT_EQ(d.at("notifier").as_object().at("queued").as_int64(), 0);
    // No pid files: the gateway (expected at any time) is "not running", so the page is bad.
    EXPECT_EQ(d.at("status").as_string(), "bad");
    EXPECT_EQ(d.at("history").as_object().at("load1").as_array().size(), 1u);
}

TEST_F(GatewayTest, MarksTheSessionCookieSecureOverHttpsOnly) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string body = R"({"username":"siva","password":"correct horse battery"})";
    // Plain HTTP (local development): no Secure, or the browser would drop the cookie.
    const Reply plain = send(http::verb::post, "/api/v1/session", body);
    ASSERT_EQ(plain.status, 200u);
    EXPECT_EQ(plain.set_cookie.find("Secure"), std::string::npos) << plain.set_cookie;
    // Through the local HTTPS proxy (tailscale serve): Secure, on sign-in and on sign-out.
    const std::vector<std::pair<std::string, std::string>> https{{"X-Forwarded-Proto", "https"}};
    const Reply secure =
        send(http::verb::post, "/api/v1/session", body, {}, "application/json", https);
    ASSERT_EQ(secure.status, 200u);
    EXPECT_NE(secure.set_cookie.find("; Secure"), std::string::npos) << secure.set_cookie;
    const Reply out = send(http::verb::delete_, "/api/v1/session", {},
                           session_from(secure.set_cookie), "application/json", https);
    EXPECT_NE(out.set_cookie.find("; Secure"), std::string::npos) << out.set_cookie;
    // The internal header can't be sent by a client.
    const Reply forged = send(http::verb::post, "/api/v1/session", body, {}, "application/json",
                              {{std::string(kViaHttpsHeader), "1"}});
    EXPECT_EQ(forged.set_cookie.find("Secure"), std::string::npos) << forged.set_cookie;
}

}  // namespace
}  // namespace mda::gateway
