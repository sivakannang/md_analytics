// Live stream tests: QuoteCache and quote_json as units, then the WebSocket end to end
// (real server on a free port, a Beast client, quotes injected into the cache).

#include <chrono>
#include <future>
#include <set>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <mda/appdb/schema.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/auth/user_store.hpp>
#include <mda/gateway/last_quotes.hpp>
#include <mda/gateway/quote_cache.hpp>
#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/gateway/server.hpp>
#include <mda/gateway/stream_api.hpp>
#include <mda/gateway/user_api.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {
namespace {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace json = boost::json;
using asio::ip::tcp;
using messages::MarketSnapshot;

MarketSnapshot snapshot(std::uint32_t id, double ltp, std::uint32_t fields = messages::kFieldLtp) {
    MarketSnapshot s{};
    s.header = messages::make_header<MarketSnapshot>(core::InstrumentId(id), 1);
    s.fields = fields;
    s.ltp = core::Price::from_raw(static_cast<std::int64_t>(ltp * core::Price::kScale));
    return s;
}

MarketSnapshot full(std::uint32_t id, double ltp, double close) {
    MarketSnapshot s = snapshot(id, ltp,
                                messages::kFieldLtp | messages::kFieldOhlc |
                                    messages::kFieldVolume | messages::kFieldExchangeTime);
    s.open = core::Price::from_raw(static_cast<std::int64_t>(close * core::Price::kScale));
    s.high = s.ltp;
    s.low = s.open;
    s.close = s.open;
    s.volume = core::Quantity(12'345);
    s.exchange_ts = core::Timestamp::from_seconds(1'792'640'000);
    return s;
}

// --------------------------------------------------------------------------- cache

TEST(QuoteCacheTest, MergesFieldGroupsAndTracksWhatWasSent) {
    QuoteCache cache;
    cache.update(full(7, 101.5, 100.0));
    cache.update(snapshot(7, 102.0));  // LTP only: OHLC and volume must survive

    SeenVersions seen;
    const std::vector<std::uint32_t> ids{7, 8};
    auto changed = cache.changed(ids, seen);
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0].snapshot.ltp, core::Price::from_raw(1'020'000));
    EXPECT_EQ(changed[0].snapshot.close, core::Price::from_raw(1'000'000));
    EXPECT_EQ(changed[0].snapshot.volume, core::Quantity(12'345));

    EXPECT_TRUE(cache.changed(ids, seen).empty());  // nothing new since
    cache.update(snapshot(8, 50.0));
    changed = cache.changed(ids, seen);
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0].snapshot.header.instrument_id, core::InstrumentId(8));
}

TEST(QuoteCacheTest, StartsFromTheSavedLastQuotes) {
    auto store = quotestore::LastQuoteStore::open_in_memory().value();
    store.remember(full(7, 101.5, 100.0));
    messages::CircuitLimits limits{};
    limits.header = messages::make_header<messages::CircuitLimits>(core::InstrumentId(7), 1);
    limits.lower = core::Price::from_raw(900'000);
    limits.upper = core::Price::from_raw(1'100'000);
    store.remember(limits);
    messages::DepthSnapshot depth{};
    depth.header = messages::make_header<messages::DepthSnapshot>(core::InstrumentId(7), 1);
    store.remember(depth);
    ASSERT_EQ(store.flush().value(), 3u);

    QuoteCache cache;
    EXPECT_EQ(load_last_quotes(cache, store).value(), 3u);
    const auto quote = cache.get(7);
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->snapshot.ltp, core::Price::from_raw(1'015'000));
    EXPECT_EQ(quote->snapshot.close, core::Price::from_raw(1'000'000));  // for the day change
    EXPECT_EQ(quote->lower_circuit, core::Price::from_raw(900'000));

    cache.update(snapshot(7, 102.0));  // the first live tick replaces the saved price
    EXPECT_EQ(cache.get(7)->snapshot.ltp, core::Price::from_raw(1'020'000));
}

TEST(QuoteCacheTest, CircuitLimitsJoinTheQuoteAndRepeatsAreNotChanges) {
    QuoteCache cache;
    cache.update(full(7, 101.5, 100.0));
    SeenVersions seen;
    const std::vector<std::uint32_t> ids{7};
    ASSERT_EQ(cache.changed(ids, seen).size(), 1u);

    messages::CircuitLimits limits{};
    limits.header = messages::make_header<messages::CircuitLimits>(core::InstrumentId(7), 1);
    limits.lower = core::Price::from_raw(900'000);
    limits.upper = core::Price::from_raw(1'100'000);
    cache.update_limits(limits);
    const auto changed = cache.changed(ids, seen);
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0].upper_circuit, core::Price::from_raw(1'100'000));
    EXPECT_EQ(changed[0].snapshot.ltp, core::Price::from_raw(1'015'000));  // kept

    cache.update_limits(limits);  // the periodic re-publish
    EXPECT_TRUE(cache.changed(ids, seen).empty());

    const auto json = quote_json(changed[0]);
    EXPECT_DOUBLE_EQ(json.at("lower_circuit").as_double(), 90.0);
    EXPECT_DOUBLE_EQ(json.at("upper_circuit").as_double(), 110.0);
    EXPECT_EQ(quote_json(CachedQuote{.snapshot = full(8, 1, 1)}).if_contains("upper_circuit"),
              nullptr);
}

TEST(QuoteCacheTest, IgnoresSnapshotsWithoutAnInstrument) {
    QuoteCache cache;
    cache.update(snapshot(0, 1.0));
    EXPECT_EQ(cache.size(), 0u);
}

TEST(QuoteJsonTest, ComputesChangeAndOmitsMissingGroups) {
    const auto quote = quote_json(full(7, 101.5, 100.0));
    EXPECT_EQ(quote.at("id").as_int64(), 7);
    EXPECT_DOUBLE_EQ(quote.at("ltp").as_double(), 101.5);
    EXPECT_DOUBLE_EQ(quote.at("change").as_double(), 1.5);
    EXPECT_DOUBLE_EQ(quote.at("change_pct").as_double(), 1.5);
    EXPECT_EQ(quote.at("volume").as_int64(), 12'345);
    EXPECT_TRUE(quote.at("exchange_time").as_string().ends_with("Z"));
    EXPECT_EQ(quote.if_contains("oi"), nullptr);
    EXPECT_EQ(quote.at("buy_qty").as_int64(), 0);
    EXPECT_TRUE(quote.contains("avg_price"));

    const auto ltp_only = quote_json(snapshot(9, 22'811.35));
    EXPECT_DOUBLE_EQ(ltp_only.at("ltp").as_double(), 22'811.35);
    EXPECT_EQ(ltp_only.if_contains("change"), nullptr);
    EXPECT_EQ(ltp_only.if_contains("close"), nullptr);
}

// --------------------------------------------------------------------------- WebSocket

using Client = websocket::stream<tcp::socket>;

class StreamTest : public ::testing::Test {
protected:
    StreamTest()
        : db_(storage::Database::open_in_memory().value())
        , store_(db_)
        , auth_(store_, {.argon2 = {.memory_kib = 256, .iterations = 1, .lanes = 1}})
        , server_(io_.get_executor(), router_) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
        EXPECT_TRUE(auth_.signup({.username = "siva",
                                  .email = "siva@example.com",
                                  .phone = "9876543210",
                                  .password = "correct horse battery"}));
        token_ = auth_.signin("siva", "correct horse battery", "test").value().token;
        register_stream(server_, auth_, worker_.get_executor(), cache_,
                        {.publish_interval = std::chrono::milliseconds(10),
                         .max_instruments = 3,
                         .max_depth = 2,
                         .reauth_interval = std::chrono::seconds(1),
                         .bars = &live_bars_,
                         .chains = &chains_});
        EXPECT_FALSE(server_.listen({asio::ip::make_address("127.0.0.1"), 0}));
        port_ = server_.local_endpoint().port();
        io_thread_ = std::thread([this] { io_.run(); });
    }

    ~StreamTest() override {
        asio::post(io_, [this] {
            server_.stop();
            io_.stop();
        });
        io_thread_.join();
        worker_.join();
    }

    /// Connects; `error` receives the handshake failure and `status` the HTTP status.
    /// `forwarded_host`: as `tailscale serve` adds it (X-Forwarded-Host); this test connects
    /// from loopback, so the gateway believes it.
    std::unique_ptr<Client> connect(const std::string& token, const std::string& origin,
                                    beast::error_code& error, unsigned& status,
                                    const std::string& forwarded_host = {}) {
        auto client = std::make_unique<Client>(client_io_);
        client->next_layer().connect({asio::ip::make_address("127.0.0.1"), port_});
        timeval timeout{.tv_sec = 3, .tv_usec = 0};  // a missing message fails, not hangs
        setsockopt(client->next_layer().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof timeout);
        client->set_option(websocket::stream_base::decorator([=](websocket::request_type& request) {
            if (!token.empty()) {
                request.set(http::field::cookie, std::string(kSessionCookie) + "=" + token);
            }
            if (!origin.empty()) {
                request.set(http::field::origin, origin);
            }
            if (!forwarded_host.empty()) {
                request.set("X-Forwarded-Host", forwarded_host);
            }
        }));
        websocket::response_type response;
        client->handshake(response, "127.0.0.1:" + std::to_string(port_), std::string(kStreamPath),
                          error);
        status = response.result_int();
        return client;
    }

    std::unique_ptr<Client> connect_ok() {
        beast::error_code error;
        unsigned status = 0;
        auto client = connect(token_, "http://127.0.0.1:" + std::to_string(port_), error, status);
        EXPECT_FALSE(error) << error.message();
        return client;
    }

    /// Next message of `type` (skipping others); empty object on timeout or close.
    static json::object next(Client& client, std::string_view type) {
        for (int i = 0; i < 50; ++i) {
            beast::flat_buffer buffer;
            beast::error_code error;
            client.read(buffer, error);
            if (error) {
                return {};
            }
            json::value value = json::parse(beast::buffers_to_string(buffer.data()));
            if (value.as_object().at("type").as_string() == type) {
                return value.as_object();
            }
        }
        return {};
    }

    static std::set<std::int64_t> ids_in(const json::object& quotes) {
        std::set<std::int64_t> ids;
        for (const auto& quote : quotes.at("data").as_array()) {
            ids.insert(quote.as_object().at("id").as_int64());
        }
        return ids;
    }

    storage::Database db_;
    auth::UserStore store_;
    auth::AuthService auth_;
    QuoteCache cache_;
    LiveBars live_bars_;
    ChainSummaries chains_;
    asio::io_context io_;
    asio::thread_pool worker_{1};
    Router router_;
    HttpServer server_;
    std::uint16_t port_ = 0;
    std::thread io_thread_;
    asio::io_context client_io_;
    std::string token_;
};

TEST_F(StreamTest, RequiresASession) {
    beast::error_code error;
    unsigned status = 0;
    connect("", "", error, status);
    EXPECT_TRUE(error);
    EXPECT_EQ(status, 401u);
    connect("not-a-real-token", "", error, status);
    EXPECT_EQ(status, 401u);
}

TEST_F(StreamTest, RefusesCrossSiteOrigins) {
    beast::error_code error;
    unsigned status = 0;
    connect(token_, "https://evil.example", error, status);
    EXPECT_TRUE(error);
    EXPECT_EQ(status, 403u);
}

TEST_F(StreamTest, AcceptsTheProxysPublicHostAsOrigin) {
    // Through `tailscale serve` (ADR-0027): Host is ours, the browser's origin is the ts.net name.
    const std::string site = "mac.tail1234.ts.net";
    beast::error_code error;
    unsigned status = 0;
    connect(token_, "https://" + site, error, status, site);
    EXPECT_FALSE(error) << error.message();
    EXPECT_EQ(status, 101u);
    // Without the proxy's header the same origin is foreign, and a forged one doesn't help evil.
    connect(token_, "https://" + site, error, status);
    EXPECT_EQ(status, 403u);
    connect(token_, "https://evil.example", error, status, site);
    EXPECT_EQ(status, 403u);
}

TEST_F(StreamTest, SendsFeedStatusOnConnectAndOnChange) {
    auto client = connect_ok();
    EXPECT_EQ(next(*client, "status").at("data").as_object().at("feed").as_string(), "unavailable");
    cache_.set_feed(FeedHealth::Connected);
    EXPECT_EQ(next(*client, "status").at("data").as_object().at("feed").as_string(), "connected");
}

TEST_F(StreamTest, StreamsOnlySubscribedInstrumentsAndOnlyWhatChanged) {
    cache_.update(full(7, 101.5, 100.0));  // already known before subscribing
    auto client = connect_ok();
    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[7,8]})")));

    auto quotes = next(*client, "quotes");
    ASSERT_FALSE(quotes.empty());
    EXPECT_EQ(ids_in(quotes), (std::set<std::int64_t>{7}));  // current state at once
    EXPECT_DOUBLE_EQ(quotes.at("data").as_array()[0].as_object().at("change_pct").as_double(), 1.5);

    cache_.update(snapshot(9, 1.0));  // not subscribed
    cache_.update(snapshot(8, 50.0));
    EXPECT_EQ(ids_in(next(*client, "quotes")), (std::set<std::int64_t>{8}));

    client->write(asio::buffer(std::string(R"({"a":"unsubscribe","v":[8]})")));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cache_.update(snapshot(8, 51.0));
    cache_.update(snapshot(7, 102.0));
    EXPECT_EQ(ids_in(next(*client, "quotes")), (std::set<std::int64_t>{7}));
}

TEST_F(StreamTest, LimitsInstrumentsPerConnection) {
    auto client = connect_ok();
    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[1,2,3,4]})")));
    EXPECT_EQ(next(*client, "error").at("message").as_string(),
              "at most 3 instruments per connection");
}

TEST_F(StreamTest, ClosesOnMalformedMessages) {
    auto client = connect_ok();
    client->write(asio::buffer(std::string("not json")));
    EXPECT_TRUE(next(*client, "never").empty());
    EXPECT_EQ(client->reason().code, websocket::close_code::policy_error);
}

TEST_F(StreamTest, SigningOutElsewhereEndsTheStream) {
    auto client = connect_ok();
    std::promise<bool> done;
    asio::post(worker_, [&] { done.set_value(auth_.signout(token_).has_value()); });
    ASSERT_TRUE(done.get_future().get());
    EXPECT_TRUE(next(*client, "never").empty());  // closed within the 1 s re-check
    EXPECT_EQ(client->reason().code, 4001);
}

messages::DepthSnapshot book(std::uint32_t id) {
    messages::DepthSnapshot d{};
    d.header = messages::make_header<messages::DepthSnapshot>(core::InstrumentId(id), 1);
    d.bids[0] = {.price = core::Price::from_raw(1'000'000), .quantity = 25, .orders = 2};
    d.bids[1] = {.price = core::Price::from_raw(999'500), .quantity = 40, .orders = 3};
    d.asks[0] = {.price = core::Price::from_raw(1'000'500), .quantity = 10, .orders = 1};
    return d;
}

TEST(DepthJsonTest, ListsLevelsAndSkipsEmptyOnes) {
    const auto json = depth_json(book(7));
    EXPECT_EQ(json.at("id").as_int64(), 7);
    ASSERT_EQ(json.at("bids").as_array().size(), 2u);
    ASSERT_EQ(json.at("asks").as_array().size(), 1u);
    const auto& best = json.at("bids").as_array()[0].as_object();
    EXPECT_DOUBLE_EQ(best.at("price").as_double(), 100.0);
    EXPECT_EQ(best.at("qty").as_int64(), 25);
    EXPECT_EQ(best.at("orders").as_int64(), 2);
}

TEST_F(StreamTest, DepthIsASeparateSubscription) {
    cache_.update(full(7, 101.5, 100.0));
    cache_.update_depth(book(7));
    auto client = connect_ok();
    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[7]})")));
    ASSERT_FALSE(next(*client, "quotes").empty());

    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[7],"mode":"depth"})")));
    const auto depth = next(*client, "depth");
    ASSERT_FALSE(depth.empty());
    EXPECT_EQ(depth.at("data").as_array()[0].as_object().at("bids").as_array().size(), 2u);

    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[1,2,3],"mode":"depth"})")));
    EXPECT_EQ(next(*client, "error").at("message").as_string(),
              "at most 2 depth subscriptions per connection");
}

TEST_F(StreamTest, RejectsUnknownModes) {
    auto client = connect_ok();
    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[7],"mode":"turbo"})")));
    EXPECT_TRUE(next(*client, "never").empty());
    EXPECT_EQ(client->reason().code, websocket::close_code::policy_error);
}

bars::Bar live_bar(std::uint32_t id, std::int64_t start, double close, bool forming) {
    bars::Bar bar{};
    bar.header = messages::make_header<bars::Bar>(core::InstrumentId(id), 0);
    bar.header.flags = forming ? bars::kBarForming : 0;
    bar.start = start;
    bar.open = bar.high = bar.low = bar.close =
        core::Price::from_raw(static_cast<std::int64_t>(close * core::Price::kScale));
    return bar;
}

TEST(LiveBarsTest, KeepsTheTwoLatestMinutes) {
    LiveBars bars;
    bars.update(live_bar(7, 60, 1, false));
    bars.update(live_bar(7, 120, 2, true));
    bars.update(live_bar(7, 120, 3, false));  // the forming minute closes
    bars.update(live_bar(7, 180, 4, true));
    SeenVersions seen;
    const std::vector<std::uint32_t> ids{7};
    const auto changed = bars.changed(ids, seen);
    ASSERT_EQ(changed.size(), 2u);
    EXPECT_EQ(changed[0].start, 120);
    EXPECT_EQ(changed[0].close.to_double(), 3.0);
    EXPECT_EQ(changed[1].start, 180);
    EXPECT_TRUE(bars.changed(ids, seen).empty());
}

TEST_F(StreamTest, StreamsLiveBarsWhenSubscribed) {
    bars::Bar forming = live_bar(7, 600, 101.5, true);
    forming.oi_open = forming.oi_low = core::Quantity(5'000);
    forming.oi_high = forming.oi_close = core::Quantity(5'200);
    forming.oi_updates = 2;
    forming.oi_update_offset = 30;
    live_bars_.update(forming);
    auto client = connect_ok();
    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[7],"mode":"bars"})")));
    const auto bars = next(*client, "bars");
    ASSERT_FALSE(bars.empty());
    const auto& bar = bars.at("data").as_array()[0].as_object();
    EXPECT_EQ(bar.at("t").as_int64(), 600);
    EXPECT_DOUBLE_EQ(bar.at("c").as_double(), 101.5);
    EXPECT_TRUE(bar.at("forming").as_bool());
    EXPECT_EQ(bar.at("oi").as_int64(), 5'200);
    EXPECT_EQ(bar.at("oi_o").as_int64(), 5'000);
    EXPECT_EQ(bar.at("oi_h").as_int64(), 5'200);
    EXPECT_EQ(bar.at("oi_l").as_int64(), 5'000);
    EXPECT_EQ(bar.at("oi_n").as_int64(), 2);
    EXPECT_EQ(bar.at("oi_t").as_int64(), 630);

    live_bars_.update(live_bar(7, 600, 102.0, false));
    const auto closed = next(*client, "bars");
    ASSERT_FALSE(closed.empty());
    EXPECT_FALSE(closed.at("data").as_array()[0].as_object().at("forming").as_bool());
    EXPECT_TRUE(closed.at("data").as_array()[0].as_object().at("oi_t").is_null());
}

TEST(QuoteJsonTest, IncludesOptionAnalytics) {
    QuoteCache cache;
    auto s = full(7, 120.5, 100.0);
    s.fields |= messages::kFieldOpenInterest;
    s.oi = core::Quantity(5'000);
    cache.update(s);
    options::OptionResult result{.id = core::InstrumentId(7), .has_iv = true, .iv = 0.1452, .delta = 0.53214,
                                 .gamma = 0.0004, .theta = -15.75, .vega = 9.1, .has_prev_oi = true,
                                 .prev_oi = 4'000, .buildup = options::Buildup::LongBuildup};
    cache.update_analytics(options::to_message(result));
    SeenVersions seen;
    const std::vector<std::uint32_t> ids{7};
    const auto json = quote_json(cache.changed(ids, seen).at(0));
    EXPECT_DOUBLE_EQ(json.at("iv").as_double(), 14.52);  // percent
    EXPECT_DOUBLE_EQ(json.at("delta").as_double(), 0.5321);
    EXPECT_DOUBLE_EQ(json.at("theta").as_double(), -15.75);
    EXPECT_EQ(json.at("oi_change").as_int64(), 1'000);
    EXPECT_EQ(json.at("buildup").as_string(), "long_buildup");

    cache.update_analytics(options::to_message(result));  // the 15 s re-publish: not a change
    EXPECT_TRUE(cache.changed(ids, seen).empty());
}

TEST_F(StreamTest, StreamsChainSummaries) {
    options::ChainSummary summary{.underlying = "NIFTY", .spot = core::InstrumentId(3),
                                  .expiry = std::chrono::year_month_day{std::chrono::year{2026} / 10 / 27},
                                  .forward = 22'713.45, .atm_strike = 22'700, .pcr = 0.89, .max_pain = 22'800};
    chains_.update(options::to_message(summary));
    auto client = connect_ok();
    client->write(asio::buffer(std::string(R"({"a":"subscribe","v":[3],"mode":"chain"})")));
    const auto chain = next(*client, "chain");
    ASSERT_FALSE(chain.empty());
    const auto& data = chain.at("data").as_array()[0].as_object();
    EXPECT_EQ(data.at("expiry").as_string(), "2026-10-27");
    EXPECT_DOUBLE_EQ(data.at("max_pain").as_double(), 22'800);
    EXPECT_TRUE(data.at("atm_iv").is_null());  // unknown
}

}  // namespace
}  // namespace mda::gateway
