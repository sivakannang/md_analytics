// End-to-end HTTP tests: a real mda-gateway server on 127.0.0.1:<free port> inside the
// test process, driven by a plain HTTP client.

#include <atomic>
#include <chrono>
#include <filesystem>
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
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <unistd.h>

#include <mda/admin/admin.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/auth/user_store.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/broker/broker.hpp>
#include <mda/bus/bus.hpp>
#include <mda/catalog/catalog.hpp>
#include <mda/gateway/admin_api.hpp>
#include <mda/gateway/bars_api.hpp>
#include <mda/gateway/chain_api.hpp>
#include <mda/gateway/notifications_api.hpp>
#include <mda/gateway/oi_change_api.hpp>
#include <mda/gateway/paper_api.hpp>
#include <mda/gateway/server.hpp>
#include <mda/gateway/signals_api.hpp>
#include <mda/gateway/system_api.hpp>
#include <mda/gateway/usage_tracker.hpp>
#include <mda/gateway/user_api.hpp>
#include <mda/gateway/watchlist_api.hpp>
#include <mda/signals/market_state.hpp>
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
        register_signal_routes(router_, auth_, db_, worker_.get_executor(), signal_store_,
                               bars_worker_.get_executor(),
                               {{.underlying = "NIFTY", .spot = "NSE|CASH|IDX|NIFTY 50"}},
                               {{.id = "oi-bias-v1", .horizon_min = 30}});
        register_paper_routes(router_, auth_, db_, worker_.get_executor(), paper_store_,
                              bars_worker_.get_executor(), *order_sender_, order_updates_, quotes_,
                              {.opening_cash = 10'00'000 * 100,
                               .futures_margin = 120'000'000,
                               .freeze_quantity = {},
                               .resend_after = std::chrono::milliseconds(100),
                               .give_up_after = std::chrono::milliseconds(600)});
        register_auto_routes(router_, auth_, worker_.get_executor(), paper_store_, auto_decisions_,
                             bars_worker_.get_executor(),
                             {{.id = "test-run",
                               .user = 1,
                               .strategy = "follow-signal",
                               .signal = "oi-bias-v1",
                               .underlying = "NIFTY",
                               .enabled = false,
                               .settings = {{"lots", 1}}}});
        EXPECT_FALSE(server_.listen({asio::ip::make_address("127.0.0.1"), 0}));
        port_ = server_.local_endpoint().port();
        io_thread_ = std::thread([this] { io_.run(); });
    }

    ~GatewayTest() override {
        paper_broker_.request_stop();
        if (paper_broker_.joinable()) {
            paper_broker_.join();
        }
        asio::post(io_, [this] {
            server_.stop();
            io_.stop();
        });
        io_thread_.join();
        bars_worker_.join();
        worker_.join();
        (void)bus::remove_bus(ord_bus_);
        std::error_code ignored;
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(paper_path_.string() + suffix, ignored);
        }
    }

    /// A paper broker for the test: reads the gateway's order bus, fills against a call
    /// (instrument 4, NIFTY 22400 CE) quoted 119.95 / 120.05, and hands its reports to the
    /// gateway directly. Session time is 2026-10-22 10:00 IST plus the time since start.
    void start_paper_broker() {
        paper_broker_ = std::jthread([this](const std::stop_token& stop) {
            auto store = broker::PaperStore::open(paper_path_).value();
            const auto lookup = [](core::InstrumentId id) -> std::optional<rms::InstrumentInfo> {
                if (id.value() != 4) {
                    return std::nullopt;
                }
                return rms::InstrumentInfo{
                    .contract = {.id = id,
                                 .segment = oms::Segment::Option,
                                 .exchange = oms::Exchange::Nse,
                                 .lot_size = 65,
                                 .strike = core::Price::from_raw(22'400 * core::Price::kScale),
                                 .call = true},
                    .tick = core::Price::from_raw(500),
                    .freeze_quantity = 1800,
                    .listed = true,
                    .expiry_days = 20'753};  // 2026-10-27
            };
            broker::Broker paper({}, store, lookup);
            EXPECT_TRUE(paper.load().has_value());
            constexpr oms::Millis kSession = 1'792'643'400'000;  // 2026-10-22 10:00 IST
            const auto started = std::chrono::steady_clock::now();
            const auto now = [&] {
                return kSession + std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
            };
            messages::MarketSnapshot quote{};
            quote.header.instrument_id = core::InstrumentId(4);
            quote.fields = messages::kFieldLtp;
            quote.ltp = core::Price::from_raw(1'200'000);
            messages::DepthSnapshot depth{};
            depth.header.instrument_id = core::InstrumentId(4);
            depth.bids[0] = {
                .price = core::Price::from_raw(1'199'500), .quantity = 1000, .orders = 1};
            depth.asks[0] = {
                .price = core::Price::from_raw(1'200'500), .quantity = 1000, .orders = 1};
            auto reader = bus::Reader::attach(ord_bus_, {.name = "test-broker"});
            ASSERT_TRUE(reader.has_value());
            const auto publish = [&](const auto& reports) {
                for (const auto& r : reports.value_or(broker::Reports{})) {
                    order_updates_.add(r);
                }
            };
            while (!stop.stop_requested()) {
                // A fresh quote every loop keeps the book from going stale.
                quote.receive_ts = depth.receive_ts = core::Timestamp::from_ns(now() * 1'000'000);
                paper.on_snapshot(quote);
                paper.on_depth(depth);
                (void)reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
                    if (auto request = messages::decode<broker::OrderRequest>(type, payload)) {
                        publish(paper.on_request(*request, now()));
                    }
                });
                publish(paper.on_tick(now()));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
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
    signals::SignalStore signal_store_ = signals::SignalStore::open_in_memory().value();
    inline static std::atomic<int> fixtures_{0};
    const int fixture_ = ++fixtures_;
    std::filesystem::path paper_path_ =
        std::filesystem::temp_directory_path() /
        ("gw_paper_" + std::to_string(::getpid()) + "_" + std::to_string(fixture_) + ".db");
    broker::PaperStore paper_store_ = broker::PaperStore::open(paper_path_).value();
    std::string ord_bus_ =
        "gwt" + std::to_string(::getpid() % 100'000) + "-" + std::to_string(fixture_);
    std::unique_ptr<OrderSender> order_sender_ = OrderSender::open(ord_bus_, 1 << 20);
    OrderUpdates order_updates_;
    autotrade::DecisionStore auto_decisions_ = autotrade::DecisionStore::open_in_memory().value();
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
    std::jthread paper_broker_;  // last: uses the members above
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

    // Too large: refused on the announced length. Only the headers go out, because the server
    // closes unread data, and writing the body into that races with a connection reset.
    asio::io_context client_io;
    tcp::socket socket(client_io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port_});
    asio::write(socket, asio::buffer(std::string_view("POST /api/v1/user/signup HTTP/1.1\r\n"
                                                      "Host: 127.0.0.1\r\n"
                                                      "Content-Type: application/json\r\n"
                                                      "Content-Length: 70000\r\n\r\n")));
    beast::flat_buffer buffer;
    http::response<http::string_body> huge;
    http::read(socket, buffer, huge);
    EXPECT_EQ(huge.result_int(), 413u);
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
    EXPECT_EQ(d.at("processes").as_array().size(), 9u);
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

// `count` signals a minute apart from 09:15 IST of the day starting at `day_start`,
// alternating bullish and bearish, all at spot 22,500.
std::vector<signals::SignalRecord> signal_day(std::int64_t day_start, int count) {
    std::vector<signals::SignalRecord> records;
    for (int i = 0; i < count; ++i) {
        signals::Signal signal{
            .spot = core::InstrumentId(3),
            .minute = day_start + 33'300 + i * 60,
            .strategy = signals::StrategyId::OiBiasV1,
            .horizon_min = 30,
            .score = i % 2 == 0 ? 40 : -40,
            .label = i % 2 == 0 ? signals::Label::Bullish : signals::Label::Bearish,
            .factors = {}};
        signal.factors.push_back({.id = signals::FactorId::PcrChange,
                                  .status = signals::FactorStatus::Present,
                                  .value = 0.5,
                                  .points = 7.5});
        records.push_back({.underlying = "NIFTY",
                           .signal = std::move(signal),
                           .spot_raw = 22'500 * core::Price::kScale});
    }
    return records;
}

TEST_F(GatewayTest, ListsSignalIndicesAndStrategies) {
    EXPECT_EQ(send(http::verb::get, "/api/v1/signals").status, 401u);
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    const Reply reply = send(http::verb::get, "/api/v1/signals", {}, session);
    ASSERT_EQ(reply.status, 200u);
    const auto& index = data(reply).at("indices").as_array().at(0).as_object();
    EXPECT_EQ(index.at("underlying").as_string(), "NIFTY");
    EXPECT_EQ(index.at("spot_id").as_int64(), 3);
    EXPECT_TRUE(index.at("latest").as_array().empty());
    const auto& strategy = data(reply).at("strategies").as_array().at(0).as_object();
    EXPECT_EQ(strategy.at("horizon_min").as_int64(), 30);
    EXPECT_EQ(strategy.at("bullish").as_int64(), 25);
    EXPECT_EQ(strategy.at("bearish").as_int64(), -25);
    EXPECT_EQ(data(reply).at("factors").as_array().size(), signals::kFactorCount);

    // With signals: the newest minute's, one per strategy.
    ASSERT_TRUE(signal_store_.write(signal_day(1'792'607'400, 3)).has_value());
    const Reply with = send(http::verb::get, "/api/v1/signals", {}, session);
    const auto& latest =
        data(with).at("indices").as_array().at(0).as_object().at("latest").as_array();
    ASSERT_EQ(latest.size(), 1u);
    EXPECT_EQ(latest[0].as_object().at("minute").as_int64(), 1'792'607'400 + 33'300 + 120);
    EXPECT_EQ(latest[0].as_object().at("label").as_string(), "bullish");
}

TEST_F(GatewayTest, ServesADaysSignals) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    EXPECT_EQ(send(http::verb::get, "/api/v1/signals/NIFTY").status, 401u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/signals/BANKEX", {}, session).status, 404u);
    EXPECT_EQ(send(http::verb::get, "/api/v1/signals/NIFTY?date=22-10-2026", {}, session).status,
              400u);

    Reply empty = send(http::verb::get, "/api/v1/signals/NIFTY", {}, session);
    ASSERT_EQ(empty.status, 200u);
    EXPECT_TRUE(data(empty).at("date").is_null());
    EXPECT_TRUE(data(empty).at("signals").as_array().empty());

    constexpr std::int64_t kOct22 = 1'792'607'400;  // 2026-10-22 00:00 IST
    ASSERT_TRUE(signal_store_.write(signal_day(kOct22, 3)).has_value());
    ASSERT_TRUE(signal_store_.write(signal_day(kOct22 + 86'400, 2)).has_value());
    const signals::OutcomeUpdate update{
        .strategy = "oi-bias-v1",
        .underlying = "NIFTY",
        .minute = kOct22 + 86'400 + 33'300,
        .outcome = signals::outcome(signals::Label::Bullish, 22'500 * core::Price::kScale,
                                    22'545 * core::Price::kScale, 5)};
    ASSERT_TRUE(signal_store_.set_outcomes(std::span(&update, 1)).has_value());

    // No date: the latest day with signals.
    const Reply latest = send(http::verb::get, "/api/v1/signals/NIFTY", {}, session);
    ASSERT_EQ(latest.status, 200u);
    EXPECT_EQ(data(latest).at("date").as_string(), "2026-10-23");
    const auto& rows = data(latest).at("signals").as_array();
    ASSERT_EQ(rows.size(), 2u);
    const auto& first = rows[0].as_object();
    EXPECT_EQ(first.at("minute").as_int64(), kOct22 + 86'400 + 33'300);
    EXPECT_EQ(first.at("label").as_string(), "bullish");
    EXPECT_DOUBLE_EQ(first.at("spot").as_double(), 22'500);
    EXPECT_EQ(first.at("factors").as_array()[0].as_object().at("factor").as_string(), "pcr_change");
    const auto& outcome = first.at("outcome").as_object();
    EXPECT_EQ(outcome.at("return_bp").as_int64(), 20);
    EXPECT_EQ(outcome.at("hit").as_bool(), true);
    EXPECT_TRUE(rows[1].as_object().at("outcome").is_null());

    const Reply earlier =
        send(http::verb::get, "/api/v1/signals/NIFTY?date=2026-10-22", {}, session);
    ASSERT_EQ(earlier.status, 200u);
    EXPECT_EQ(data(earlier).at("signals").as_array().size(), 3u);
}

TEST_F(GatewayTest, ServesTheTrackRecord) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    EXPECT_EQ(send(http::verb::get, "/api/v1/signals/NIFTY/record?days=0", {}, session).status,
              400u);

    const std::int64_t today = signals::ist_day_start(auth::system_clock_seconds());
    const auto records = signal_day(today, 4);  // bullish, bearish, bullish, bearish
    ASSERT_TRUE(signal_store_.write(records).has_value());
    // The spot rose 20 bp after each: two hits (bullish), two misses (bearish).
    std::vector<signals::OutcomeUpdate> updates;
    for (const auto& r : records) {
        updates.push_back({.strategy = "oi-bias-v1",
                           .underlying = "NIFTY",
                           .minute = r.signal.minute,
                           .outcome = signals::outcome(r.signal.label, r.spot_raw,
                                                       22'545 * core::Price::kScale, 5)});
    }
    ASSERT_TRUE(signal_store_.set_outcomes(updates).has_value());

    const Reply reply = send(http::verb::get, "/api/v1/signals/NIFTY/record?days=5", {}, session);
    ASSERT_EQ(reply.status, 200u);
    EXPECT_EQ(data(reply).at("days").as_int64(), 5);
    const auto& record = data(reply).at("strategies").as_array().at(0).as_object();
    EXPECT_EQ(record.at("strategy").as_string(), "oi-bias-v1");
    EXPECT_EQ(record.at("settled").as_int64(), 4);
    EXPECT_EQ(record.at("hits").as_int64(), 2);
    EXPECT_EQ(record.at("misses").as_int64(), 2);
    EXPECT_DOUBLE_EQ(record.at("hit_rate").as_double(), 0.5);
    EXPECT_DOUBLE_EQ(record.at("always_up_rate").as_double(), 1.0);
    EXPECT_TRUE(record.at("momentum_rate").is_null());  // no signal a horizon earlier
}

// --------------------------------------------------------------- paper trading

TEST_F(GatewayTest, PaperOrderIsPlacedFilledAndListed) {
    EXPECT_EQ(send(http::verb::get, "/api/v1/paper/orders").status, 401u);
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    start_paper_broker();

    const Reply placed =
        send(http::verb::post, "/api/v1/paper/orders",
             R"({"instrument_id":4,"side":"BUY","quantity":65,"type":"MARKET","product":"NRML"})",
             session);
    ASSERT_EQ(placed.status, 200u) << boost::json::serialize(placed.body);
    const auto& order = data(placed).at("order").as_object();
    EXPECT_EQ(order.at("event").as_string(), "accepted");
    EXPECT_EQ(order.at("status").as_string(), "OPEN");
    const std::int64_t id = order.at("order_id").as_int64();

    std::this_thread::sleep_for(
        std::chrono::milliseconds(300));  // the 100 ms latency, then the fill
    const Reply orders = send(http::verb::get, "/api/v1/paper/orders?date=2026-10-22", {}, session);
    ASSERT_EQ(orders.status, 200u);
    const auto& list = orders.body.at("data").as_array();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].as_object().at("order_id").as_int64(), id);
    EXPECT_EQ(list[0].as_object().at("status").as_string(), "COMPLETE");
    EXPECT_DOUBLE_EQ(list[0].as_object().at("average_price").as_double(), 120.05);
    EXPECT_TRUE(send(http::verb::get, "/api/v1/paper/orders", {}, session)
                    .body.at("data")
                    .as_array()
                    .empty());

    const Reply trades = send(http::verb::get, "/api/v1/paper/trades?date=2026-10-22", {}, session);
    const auto& trade = trades.body.at("data").as_array().at(0).as_object();
    EXPECT_EQ(trade.at("quantity").as_int64(), 65);
    EXPECT_DOUBLE_EQ(trade.at("charges").as_object().at("brokerage").as_double(), 20.0);

    const Reply positions = send(http::verb::get, "/api/v1/paper/positions", {}, session);
    const auto& position = positions.body.at("data").as_array().at(0).as_object();
    EXPECT_EQ(position.at("quantity").as_int64(), 65);
    EXPECT_EQ(position.at("product").as_string(), "NRML");
    EXPECT_TRUE(position.at("last_price").is_null());  // no live quote in this test

    const Reply funds = send(http::verb::get, "/api/v1/paper/funds", {}, session);
    const auto& f = data(funds);
    const double charges = trade.at("charges").as_object().at("total").as_double();
    EXPECT_NEAR(f.at("cash").as_double(), 10'00'000 - charges, 0.001);
    EXPECT_NEAR(f.at("blocked").as_double(), 65 * 120.05,
                0.001);  // a bought option blocks its premium
    EXPECT_FALSE(f.at("kill_switch").as_bool());
}

TEST_F(GatewayTest, PaperPositionsShowOpenOnesAndTodaysWithTodaysRealised) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    start_paper_broker();
    // A round trip on the broker's day (2026-10-22): closed on an earlier day, so hidden.
    for (const char* side : {"BUY", "SELL"}) {
        ASSERT_EQ(send(http::verb::post, "/api/v1/paper/orders",
                       std::string(R"({"instrument_id":4,"quantity":65,"side":")") + side + R"("})",
                       session)
                      .status,
                  200u);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));  // filled before the next
    }
    const auto account = paper_store_.account_for_user(1).value();
    ASSERT_TRUE(account.has_value());
    const std::int64_t id = account->id;
    const std::int64_t now = auth::system_clock_seconds() * 1000;
    const auto at = [](std::int64_t instrument, oms::Product product) {
        return oms::PositionKey{
            .instrument = core::InstrumentId(static_cast<std::uint32_t>(instrument)),
            .product = product};
    };
    // 5: closed today; its row's realised (50.00) includes earlier days, today's trade made 12.00.
    ASSERT_TRUE(paper_store_.upsert_position(id, at(5, oms::Product::Mis),
                                             oms::Position(0, 0, 5'000), now));
    ASSERT_TRUE(paper_store_.insert_trade({.order_id = 1,
                                           .account_id = id,
                                           .instrument = core::InstrumentId(5),
                                           .side = oms::Side::Sell,
                                           .product = oms::Product::Mis,
                                           .quantity = 65,
                                           .price = core::Price::from_raw(1'000'000),
                                           .realised = 1'200,
                                           .at = now}));
    // 6: closed on an earlier day. 7: open since an earlier day. 8: settled at expiry today.
    ASSERT_TRUE(
        paper_store_.upsert_position(id, at(6, oms::Product::Nrml), oms::Position(0, 0, 300), now));
    ASSERT_TRUE(paper_store_.upsert_position(id, at(7, oms::Product::Nrml),
                                             oms::Position(65, 65 * 1'000'000, 0), now));
    ASSERT_TRUE(
        paper_store_.upsert_position(id, at(8, oms::Product::Nrml), oms::Position(0, 0, 999), now));
    ASSERT_TRUE(paper_store_.insert_ledger(id, {.at = now,
                                                .kind = oms::LedgerKind::Expiry,
                                                .amount = -500,
                                                .instrument = core::InstrumentId(8)}));

    const Reply reply = send(http::verb::get, "/api/v1/paper/positions", {}, session);
    ASSERT_EQ(reply.status, 200u);
    std::map<std::int64_t, double> realised;
    for (const auto& p : reply.body.at("data").as_array()) {
        realised[p.as_object().at("instrument_id").as_int64()] =
            p.as_object().at("realised").as_double();
    }
    EXPECT_EQ(realised.size(), 3u);
    EXPECT_DOUBLE_EQ(realised.at(5), 12.0);
    EXPECT_DOUBLE_EQ(realised.at(7), 0.0);
    EXPECT_DOUBLE_EQ(realised.at(8), -5.0);
    for (const auto& p : reply.body.at("data").as_array()) {
        if (p.as_object().at("instrument_id").as_int64() == 8) {
            EXPECT_DOUBLE_EQ(p.as_object().at("closed_pnl").as_double(), -5.0);  // an expiry counts
        }
    }
}

TEST_F(GatewayTest, AdminReadsAnotherUsersPaperButTradesOnlyOnTheirOwn) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string trader = signin("siva", "correct horse battery");
    ASSERT_EQ(signup("friend", "correct horse battery").status, 201u);
    start_paper_broker();
    const std::string order =
        R"({"instrument_id":4,"side":"BUY","quantity":65,"type":"MARKET","product":"NRML"})";
    ASSERT_EQ(send(http::verb::post, "/api/v1/paper/orders", order, trader).status, 200u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Not an admin: another user's paper is refused, not silently replaced by one's own.
    const std::string not_admin = signin("friend", "correct horse battery");
    const Reply denied = send(http::verb::get, "/api/v1/paper/positions?user=1", {}, not_admin);
    EXPECT_EQ(denied.status, 403u);
    EXPECT_EQ(error_type(denied), "PermissionException");

    ASSERT_TRUE(admin::set_role(db_, "friend", "admin").value());
    const std::string admin_session = signin("friend", "correct horse battery");
    std::int64_t siva = 0;
    const Reply users = send(http::verb::get, "/api/v1/admin/users", {}, admin_session);
    for (const auto& u : users.body.at("data").as_array()) {
        if (u.as_object().at("username").as_string() == "siva") {
            siva = u.as_object().at("user_id").as_int64();
        }
    }
    ASSERT_GT(siva, 0);
    const std::string of_siva = "user=" + std::to_string(siva);
    const auto list = [&](const std::string& path) {
        return send(http::verb::get, path, {}, admin_session).body.at("data").as_array();
    };
    EXPECT_EQ(list("/api/v1/paper/orders?date=2026-10-22&" + of_siva).size(), 1u);
    EXPECT_EQ(list("/api/v1/paper/trades?date=2026-10-22&" + of_siva).size(), 1u);
    EXPECT_EQ(
        list("/api/v1/paper/positions?" + of_siva).at(0).as_object().at("quantity").as_int64(), 65);
    EXPECT_LT(data(send(http::verb::get, "/api/v1/paper/funds?" + of_siva, {}, admin_session))
                  .at("cash")
                  .as_double(),
              10'00'000);
    EXPECT_TRUE(data(send(http::verb::get, "/api/v1/paper/auto?" + of_siva, {}, admin_session))
                    .at("runs")
                    .is_array());
    EXPECT_TRUE(list("/api/v1/paper/orders?date=2026-10-22").empty());  // the admin's own: none yet
    EXPECT_EQ(send(http::verb::get, "/api/v1/paper/orders?user=abc", {}, admin_session).status,
              400u);

    // Orders ignore `user`: this one goes on the admin's own account.
    ASSERT_EQ(
        send(http::verb::post, "/api/v1/paper/orders?" + of_siva, order, admin_session).status,
        200u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(list("/api/v1/paper/orders?date=2026-10-22").size(), 1u);
    EXPECT_EQ(list("/api/v1/paper/orders?date=2026-10-22&" + of_siva).size(), 1u);
}

TEST_F(GatewayTest, PaperRejectionsAndValidation) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    EXPECT_EQ(send(http::verb::post, "/api/v1/paper/orders",
                   R"({"instrument_id":4,"side":"HOLD","quantity":65})", session)
                  .status,
              400u);
    start_paper_broker();
    const Reply rejected = send(http::verb::post, "/api/v1/paper/orders",
                                R"({"instrument_id":4,"side":"BUY","quantity":50})", session);
    EXPECT_EQ(rejected.status, 400u);
    EXPECT_EQ(error_type(rejected), "OrderException");
    EXPECT_EQ(rejected.body.at("message").as_string(),
              "quantity must be a multiple of the lot size (65)");
}

TEST_F(GatewayTest, PaperRetryWithTheSameClientOrderIdIsOneOrder) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    start_paper_broker();
    const std::string body =
        R"({"instrument_id":4,"side":"BUY","quantity":65,"type":"LIMIT","price":115,"client_order_id":777})";
    const Reply first = send(http::verb::post, "/api/v1/paper/orders", body, session);
    const Reply again = send(http::verb::post, "/api/v1/paper/orders", body, session);
    ASSERT_EQ(first.status, 200u) << boost::json::serialize(first.body);
    ASSERT_EQ(again.status, 200u) << boost::json::serialize(again.body);
    EXPECT_EQ(data(again).at("order").as_object().at("order_id").as_int64(),
              data(first).at("order").as_object().at("order_id").as_int64());
    EXPECT_EQ(send(http::verb::get, "/api/v1/paper/orders?date=2026-10-22", {}, session)
                  .body.at("data")
                  .as_array()
                  .size(),
              1u);
}

TEST_F(GatewayTest, PaperModifyCancelAndKillSwitch) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    start_paper_broker();
    const Reply placed = send(
        http::verb::post, "/api/v1/paper/orders",
        R"({"instrument_id":4,"side":"BUY","quantity":65,"type":"LIMIT","price":115})", session);
    ASSERT_EQ(placed.status, 200u) << boost::json::serialize(placed.body);
    const std::int64_t id = data(placed).at("order").as_object().at("order_id").as_int64();
    const std::string path = "/api/v1/paper/orders/" + std::to_string(id);

    const Reply modified = send(http::verb::put, path, R"({"quantity":130,"price":116})", session);
    ASSERT_EQ(modified.status, 200u) << boost::json::serialize(modified.body);
    EXPECT_EQ(data(modified).at("order").as_object().at("event").as_string(), "modified");
    EXPECT_EQ(data(modified).at("order").as_object().at("quantity").as_int64(), 130);

    const Reply cancelled = send(http::verb::delete_, path, {}, session);
    EXPECT_EQ(data(cancelled).at("order").as_object().at("status").as_string(), "CANCELLED");
    EXPECT_EQ(send(http::verb::delete_, path, {}, session).status, 400u);  // no longer open

    // A position converted from MIS (the default) to NRML; more than it holds is refused.
    const Reply bought = send(http::verb::post, "/api/v1/paper/orders",
                              R"({"instrument_id":4,"side":"BUY","quantity":65})", session);
    ASSERT_EQ(bought.status, 200u) << boost::json::serialize(bought.body);
    Reply converted;
    for (int i = 0; i < 50; ++i) {  // until the fill has landed
        converted = send(http::verb::post, "/api/v1/paper/positions/convert",
                         R"({"instrument_id":4,"quantity":65,"to":"NRML"})", session);
        if (converted.status == 200u) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_EQ(converted.status, 200u) << boost::json::serialize(converted.body);
    EXPECT_EQ(data(converted).at("order").as_object().at("event").as_string(), "converted");
    EXPECT_EQ(data(converted).at("order").as_object().at("product").as_string(), "NRML");
    EXPECT_EQ(send(http::verb::post, "/api/v1/paper/positions/convert",
                   R"({"instrument_id":4,"quantity":65,"to":"NRML"})", session)
                  .status,
              400u);  // no MIS left
    EXPECT_EQ(
        send(http::verb::post, "/api/v1/paper/positions/convert", R"({"instrument_id":4})", session)
            .status,
        400u);

    const Reply on = send(http::verb::post, "/api/v1/paper/kill-switch", R"({"on":true})", session);
    EXPECT_TRUE(data(on).at("kill_switch").as_bool());
    const Reply blocked = send(http::verb::post, "/api/v1/paper/orders",
                               R"({"instrument_id":4,"side":"BUY","quantity":65})", session);
    EXPECT_EQ(blocked.status, 400u);
    EXPECT_TRUE(data(send(http::verb::get, "/api/v1/paper/funds", {}, session))
                    .at("kill_switch")
                    .as_bool());
}

TEST_F(GatewayTest, PaperOrderWithoutABrokerIsPending) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);
    const std::string session = signin("siva", "correct horse battery");
    const Reply pending =
        send(http::verb::post, "/api/v1/paper/orders",
             R"({"instrument_id":4,"side":"BUY","quantity":65,"client_order_id":42})", session);
    EXPECT_EQ(pending.status, 202u);
    EXPECT_EQ(data(pending).at("status").as_string(), "PENDING");
    EXPECT_EQ(data(pending).at("client_order_id").as_int64(), 42);
    // Before any order, funds show a fresh account.
    EXPECT_DOUBLE_EQ(
        data(send(http::verb::get, "/api/v1/paper/funds", {}, session)).at("cash").as_double(),
        10'00'000);
}

TEST_F(GatewayTest, AutoRunsShowTheirMetricsAndDecisions) {
    ASSERT_EQ(signup("siva", "correct horse battery").status, 201u);  // user 1: owns test-run
    const std::string session = signin("siva", "correct horse battery");
    start_paper_broker();
    // A round trip tagged as the run's (the auto trader's orders carry this tag).
    for (const char* side : {"BUY", "SELL"}) {
        const Reply r =
            send(http::verb::post, "/api/v1/paper/orders",
                 std::string(R"({"instrument_id":4,"quantity":65,"tag":"auto:test-run","side":")") +
                     side + R"("})",
                 session);
        ASSERT_EQ(r.status, 200u) << boost::json::serialize(r.body);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));  // filled before the next
    }
    const std::int64_t now = auth::system_clock_seconds() * 1000;
    const autotrade::Decision exit{.at = now,
                                   .before = autotrade::State::Long,
                                   .action = "exit",
                                   .reason = "target: +80.00 points"};
    ASSERT_TRUE(
        auto_decisions_
            .save("test-run", std::span(&exit, 1),
                  {.day = static_cast<std::int32_t>((now + 19'800'000) / 86'400'000), .entries = 1})
            .has_value());

    const Reply reply = send(http::verb::get, "/api/v1/paper/auto?days=30", {}, session);
    ASSERT_EQ(reply.status, 200u);
    const auto& run = data(reply).at("runs").as_array().at(0).as_object();
    EXPECT_EQ(run.at("id").as_string(), "test-run");
    EXPECT_FALSE(run.at("enabled").as_bool());
    EXPECT_EQ(run.at("position").as_int64(), 0);
    EXPECT_TRUE(run.at("held").as_array().empty());  // the round trip closed
    EXPECT_EQ(run.at("entries_today").as_int64(), 1);
    const auto& m = run.at("metrics").as_object();
    EXPECT_EQ(m.at("round_trips").as_int64(), 1);
    EXPECT_EQ(m.at("losses").as_int64(), 1);  // bought at the ask, sold at the bid, paid charges
    EXPECT_NEAR(m.at("gross").as_double(), -6.50, 0.001);  // −0.10 × 65
    EXPECT_GT(m.at("charges").as_double(), 0);
    EXPECT_EQ(m.at("exits").as_object().at("target").as_int64(), 1);
    EXPECT_EQ(run.at("decisions_today").as_array().at(0).as_object().at("reason").as_string(),
              "target: +80.00 points");

    // Another user sees no runs.
    ASSERT_EQ(signup("friend", "correct horse battery").status, 201u);
    const std::string other = signin("friend", "correct horse battery");
    EXPECT_TRUE(
        data(send(http::verb::get, "/api/v1/paper/auto", {}, other)).at("runs").as_array().empty());
    EXPECT_EQ(send(http::verb::get, "/api/v1/paper/auto?days=0", {}, session).status, 400u);
}

}  // namespace
}  // namespace mda::gateway
