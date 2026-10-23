#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/gateway/stream_api.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace json = boost::json;
using namespace asio::experimental::awaitable_operators;
using detail::from_error;
using detail::run_on;
using detail::token_of;

constexpr auto kUseTuple = asio::as_tuple(asio::use_awaitable);
constexpr std::size_t kMaxInbound = 4096;
constexpr auto kFastestPush = std::chrono::milliseconds(10);
constexpr auto kIdleTimeout = std::chrono::seconds(30);  // pings are sent at half of this
constexpr auto kSessionEnded = static_cast<websocket::close_code>(4001);

using Socket = websocket::stream<beast::tcp_stream>;
using Clock = std::chrono::steady_clock;

struct Deps {
    auth::AuthService* auth;
    asio::any_io_executor db;
    const QuoteCache* cache;
    StreamOptions options;
};

/// Per-connection state, shared by the reader and the writer (same executor, no lock).
struct Session {
    std::vector<std::uint32_t> ids;  ///< subscribed, sorted and unique
    SeenVersions seen;
    std::vector<std::uint32_t> depth_ids;  ///< depth subscriptions, sorted and unique
    SeenVersions seen_depth;
    std::vector<std::uint32_t> bar_ids;  ///< live bar subscriptions, sorted and unique
    SeenVersions seen_bars;
    std::vector<std::uint32_t> chain_spots;  ///< chain-summary subscriptions (spot ids)
    SeenVersions seen_chains;
    std::vector<std::string> outbox;  ///< error messages for the writer to send
    asio::steady_timer* wake = nullptr;
    std::int64_t user_id = 0;
    UsageTracker* usage = nullptr;  // optional
    std::uint64_t connection = 0;   // its id in `usage`
};

double round2(double value) {
    return std::round(value * 100.0) / 100.0;
}

/// A browser always sends Origin; refuse one whose host differs from the host the browser
/// used (CSWSH). Behind `tailscale serve` that is X-Forwarded-Host (ADR-0027), stamped by the
/// server. Non-browser clients (tests, curl) send none.
bool same_origin(const Request& request) {
    const std::string_view origin = request[http::field::origin];
    if (origin.empty()) {
        return true;
    }
    const std::size_t scheme = origin.find("://");
    const std::string_view host =
        scheme == std::string_view::npos ? origin : origin.substr(scheme + 3);
    return host == public_host(request);
}

asio::awaitable<void> refuse(beast::tcp_stream& stream, const Request& request, Response response) {
    response.version(request.version());
    response.keep_alive(false);
    response.prepare_payload();
    stream.expires_after(std::chrono::seconds(5));
    co_await http::async_write(stream, response, kUseTuple);
    boost::system::error_code ignored;
    stream.socket().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

std::string error_message(std::string_view text) {
    return json::serialize(json::object{{"type", "error"}, {"message", text}});
}

/// Adds `ids` to the sorted set `subscribed`, stopping at `max` (then queues an error).
void subscribe_into(std::vector<std::uint32_t>& subscribed, std::span<const std::uint32_t> ids,
                    std::size_t max, std::string_view what, Session& session) {
    for (const std::uint32_t id : ids) {
        const auto at = std::ranges::lower_bound(subscribed, id);
        if (at != subscribed.end() && *at == id) {
            continue;
        }
        if (subscribed.size() >= max) {
            session.outbox.push_back(error_message("at most " + std::to_string(max) + " " +
                                                   std::string(what) + " per connection"));
            break;
        }
        subscribed.insert(at, id);
    }
}

void unsubscribe_from(std::vector<std::uint32_t>& subscribed, SeenVersions& seen,
                      std::span<const std::uint32_t> ids) {
    for (const std::uint32_t id : ids) {
        if (const auto at = std::ranges::lower_bound(subscribed, id);
            at != subscribed.end() && *at == id) {
            subscribed.erase(at);
        }
        seen.erase(id);
    }
}

/// Applies one client message; false if it is malformed (the stream is then closed).
/// {"a": "subscribe" | "unsubscribe", "v": [ids], "mode": "quote" (default) | "depth"}
bool apply(Session& session, std::string_view text, const StreamOptions& options) {
    boost::system::error_code ec;
    const json::value value = json::parse(text, ec);
    if (ec || !value.is_object()) {
        return false;
    }
    const auto* action = value.as_object().if_contains("a");
    const auto* list = value.as_object().if_contains("v");
    const auto* mode = value.as_object().if_contains("mode");
    if (action == nullptr || !action->is_string() || list == nullptr || !list->is_array()) {
        return false;
    }
    if (mode != nullptr &&
        (!mode->is_string() || (mode->as_string() != "quote" && mode->as_string() != "depth" &&
         mode->as_string() != "bars" && mode->as_string() != "chain"))) {
        return false;
    }
    std::vector<std::uint32_t> ids;
    for (const json::value& item : list->as_array()) {
        if (!item.is_int64() || item.as_int64() <= 0 ||
            item.as_int64() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        ids.push_back(static_cast<std::uint32_t>(item.as_int64()));
    }

    const bool depth = mode != nullptr && mode->as_string() == "depth";
    const bool bars = mode != nullptr && mode->as_string() == "bars";
    const bool chain = mode != nullptr && mode->as_string() == "chain";
    if (action->as_string() == "subscribe") {
        if (chain) {
            subscribe_into(session.chain_spots, ids, 3, "chain subscriptions", session);
        } else if (bars) {
            subscribe_into(session.bar_ids, ids, options.max_bars, "bar subscriptions", session);
        } else if (depth) {
            subscribe_into(session.depth_ids, ids, options.max_depth, "depth subscriptions",
                           session);
        } else {
            subscribe_into(session.ids, ids, options.max_instruments, "instruments", session);
        }
        return true;
    }
    if (action->as_string() == "unsubscribe") {
        if (chain) {
            unsubscribe_from(session.chain_spots, session.seen_chains, ids);
        } else if (bars) {
            unsubscribe_from(session.bar_ids, session.seen_bars, ids);
        } else if (depth) {
            unsubscribe_from(session.depth_ids, session.seen_depth, ids);
        } else {
            unsubscribe_from(session.ids, session.seen, ids);
        }
        return true;
    }
    return false;
}

/// Reads client messages until the connection ends. Never writes data frames (the
/// writer does); wakes the writer so new subscriptions get their quotes promptly.
asio::awaitable<void> read_loop(Socket& ws, Session& session, const StreamOptions& options) {
    beast::flat_buffer buffer;
    while (true) {
        auto [error, bytes] = co_await ws.async_read(buffer, kUseTuple);
        (void)bytes;
        if (error) {
            co_return;  // closed by the client, timed out, or too large
        }
        const std::string text = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        if (!ws.got_text() || !apply(session, text, options)) {
            co_await ws.async_close({websocket::close_code::policy_error, "malformed message"},
                                    kUseTuple);
            co_return;
        }
        if (session.usage != nullptr) {
            session.usage->set_instruments(session.connection, session.ids.size());
        }
        session.wake->cancel();
    }
}

/// The only writer: errors, feed status changes and changed quotes, every interval.
asio::awaitable<void> push_loop(Socket& ws, Session& session, const Deps& deps, std::string token) {
    const auto interval = std::max(deps.options.publish_interval, kFastestPush);
    auto reauth_at = Clock::now() + deps.options.reauth_interval;
    std::optional<FeedHealth> sent_health;
    std::string text;

    while (true) {
        std::vector<std::string> outgoing;
        outgoing.swap(session.outbox);

        if (const FeedHealth health = deps.cache->feed(); health != sent_health) {
            sent_health = health;
            outgoing.push_back(json::serialize(json::object{
                {"type", "status"}, {"data", json::object{{"feed", to_string(health)}}}}));
        }
        if (!session.ids.empty()) {
            const auto quotes = deps.cache->changed(session.ids, session.seen);
            if (!quotes.empty()) {
                json::array data;
                data.reserve(quotes.size());
                for (const auto& quote : quotes) {
                    data.emplace_back(quote_json(quote));
                }
                outgoing.push_back(
                    json::serialize(json::object{{"type", "quotes"}, {"data", std::move(data)}}));
            }
        }
        if (deps.options.chains != nullptr && !session.chain_spots.empty()) {
            const auto changed = deps.options.chains->changed(session.chain_spots, session.seen_chains);
            if (!changed.empty()) {
                json::array data;
                for (const auto& summary : changed) {
                    data.emplace_back(chain_summary_json(summary));
                }
                outgoing.push_back(json::serialize(json::object{{"type", "chain"}, {"data", std::move(data)}}));
            }
        }
        if (deps.options.bars != nullptr && !session.bar_ids.empty()) {
            const auto changed = deps.options.bars->changed(session.bar_ids, session.seen_bars);
            if (!changed.empty()) {
                json::array data;
                data.reserve(changed.size());
                for (const auto& bar : changed) {
                    // The OI candle forms live; its buildup state comes from the REST bars,
                    // which the chart re-reads as minutes close (oi-buildup-chart.md §6).
                    json::value oi_time = nullptr;
                    if (bar.oi_update_offset != bars::kNoOiUpdateTime) {
                        oi_time = bar.start + bar.oi_update_offset;
                    }
                    data.emplace_back(json::object{
                        {"id", static_cast<std::int64_t>(bar.header.instrument_id.value())},
                        {"t", bar.start},
                        {"o", bar.open.to_double()},
                        {"h", bar.high.to_double()},
                        {"l", bar.low.to_double()},
                        {"c", bar.close.to_double()},
                        {"v", bar.volume.value()},
                        {"oi", bar.oi_close.value()},
                        {"oi_o", bar.oi_open.value()},
                        {"oi_h", bar.oi_high.value()},
                        {"oi_l", bar.oi_low.value()},
                        {"oi_n", bar.oi_updates},
                        {"oi_t", std::move(oi_time)},
                        {"forming", (bar.header.flags & bars::kBarForming) != 0}});
                }
                outgoing.push_back(
                    json::serialize(json::object{{"type", "bars"}, {"data", std::move(data)}}));
            }
        }
        if (!session.depth_ids.empty()) {
            const auto books = deps.cache->changed_depth(session.depth_ids, session.seen_depth);
            if (!books.empty()) {
                json::array data;
                data.reserve(books.size());
                for (const auto& book : books) {
                    data.emplace_back(depth_json(book));
                }
                outgoing.push_back(
                    json::serialize(json::object{{"type", "depth"}, {"data", std::move(data)}}));
            }
        }
        for (std::string& message : outgoing) {
            text = std::move(message);
            auto [error, written] = co_await ws.async_write(asio::buffer(text), kUseTuple);
            (void)written;
            if (error) {
                co_return;
            }
            if (session.usage != nullptr) {
                session.usage->meter_ws(session.user_id, text.size());
            }
        }

        if (Clock::now() >= reauth_at) {
            reauth_at = Clock::now() + deps.options.reauth_interval;
            const auto user =
                co_await run_on(deps.db, [&] { return deps.auth->authenticate(token); });
            if (!user) {
                co_await ws.async_close({kSessionEnded, "session ended"}, kUseTuple);
                co_return;
            }
        }

        session.wake->expires_after(interval);
        auto [ignored] = co_await session.wake->async_wait(kUseTuple);
        (void)ignored;  // expiry, or cancel() from the reader: both mean "check now"
        if ((co_await asio::this_coro::cancellation_state).cancelled() !=
            asio::cancellation_type::none) {
            co_return;  // the reader finished; the connection is over
        }
    }
}

asio::awaitable<void> serve(beast::tcp_stream stream, Request request, const Deps& deps) {
    if (!same_origin(request)) {
        spdlog::warn("stream: refused cross-origin upgrade (Origin {})",
                     std::string(request[http::field::origin]));
        co_await refuse(stream, request,
                        failure(http::status::forbidden, "PermissionException",
                                "cross-origin WebSocket refused"));
        co_return;
    }
    const std::string token = token_of(request);
    const auto user = co_await run_on(deps.db, [&] { return deps.auth->authenticate(token); });
    if (!user) {
        co_await refuse(stream, request, from_error(user.error()));
        co_return;
    }

    Socket ws(std::move(stream));
    websocket::stream_base::timeout timeouts =
        websocket::stream_base::timeout::suggested(beast::role_type::server);
    timeouts.idle_timeout = kIdleTimeout;
    ws.set_option(timeouts);
    ws.read_message_max(kMaxInbound);
    ws.text(true);
    auto [accept_error] = co_await ws.async_accept(request, kUseTuple);
    if (accept_error) {
        co_return;
    }
    spdlog::info("stream: user {} '{}' connected", user->id, user->username);

    asio::steady_timer wake(co_await asio::this_coro::executor);
    Session session{.wake = &wake, .user_id = user->id, .usage = deps.options.usage};
    if (session.usage != nullptr) {
        session.connection = session.usage->open_connection(user->id);
    }
    try {
        co_await (read_loop(ws, session, deps.options) || push_loop(ws, session, deps, token));
    } catch (const std::exception& error) {
        spdlog::warn("stream: user {} connection error: {}", user->id, error.what());
    }
    if (session.usage != nullptr) {
        session.usage->close_connection(session.connection);
    }
    spdlog::info("stream: user {} '{}' disconnected", user->id, user->username);
}

}  // namespace

json::object quote_json(const messages::MarketSnapshot& s) {
    json::object quote{{"id", static_cast<std::int64_t>(s.header.instrument_id.value())}};
    const bool has_ltp = (s.fields & messages::kFieldLtp) != 0;
    if (has_ltp) {
        quote["ltp"] = s.ltp.to_double();
    }
    if ((s.fields & messages::kFieldOhlc) != 0) {
        quote["open"] = s.open.to_double();
        quote["high"] = s.high.to_double();
        quote["low"] = s.low.to_double();
        quote["close"] = s.close.to_double();
        if (has_ltp && s.close.raw() != 0) {
            const double change = s.ltp.to_double() - s.close.to_double();
            quote["change"] = round2(change);
            quote["change_pct"] = round2(change / s.close.to_double() * 100.0);
        }
    }
    if ((s.fields & messages::kFieldVolume) != 0) {
        quote["volume"] = s.volume.value();
        quote["avg_price"] = s.avg_price.to_double();
        quote["last_qty"] = s.last_qty.value();
        quote["buy_qty"] = s.total_buy_qty.value();  // whole book, like Kite's depth "Total"
        quote["sell_qty"] = s.total_sell_qty.value();
    }
    if ((s.fields & messages::kFieldOpenInterest) != 0) {
        quote["oi"] = s.oi.value();
        quote["oi_day_high"] = s.oi_day_high.value();
        quote["oi_day_low"] = s.oi_day_low.value();
    }
    if ((s.fields & messages::kFieldExchangeTime) != 0) {
        quote["exchange_time"] = iso_utc(s.exchange_ts.ns() / 1'000'000'000);
    }
    if ((s.fields & messages::kFieldLastTradeTime) != 0) {
        quote["last_trade_time"] = iso_utc(s.last_trade_ts.ns() / 1'000'000'000);
    }
    return quote;
}

json::object quote_json(const CachedQuote& cached) {
    json::object quote = quote_json(cached.snapshot);
    if (cached.upper_circuit.raw() != 0) {
        quote["lower_circuit"] = cached.lower_circuit.to_double();
        quote["upper_circuit"] = cached.upper_circuit.to_double();
    }
    if (cached.has_analytics) {
        const auto& a = cached.analytics;
        if ((a.flags & options::kHasIv) != 0) {
            quote["iv"] = round2(a.iv * 100.0);  // percent
            quote["delta"] = std::round(a.delta * 10'000.0) / 10'000.0;
            quote["gamma"] = std::round(a.gamma * 1e6) / 1e6;
            quote["theta"] = round2(a.theta);
            quote["vega"] = round2(a.vega);
        }
        if ((a.flags & options::kHasPrevOi) != 0) {
            quote["prev_oi"] = a.prev_oi;
            if ((cached.snapshot.fields & messages::kFieldOpenInterest) != 0) {
                quote["oi_change"] = cached.snapshot.oi.value() - a.prev_oi;
            }
        }
        if (const auto name = detail::buildup_name(a.buildup); !name.empty()) {
            quote["buildup"] = name;
        }
    }
    return quote;
}

json::object depth_json(const messages::DepthSnapshot& depth) {
    const auto side = [](const messages::DepthLevel(&levels)[messages::kDepthLevels]) {
        json::array out;
        for (const auto& level : levels) {
            if (level.price.raw() == 0 && level.orders == 0) {
                continue;  // empty level
            }
            out.push_back(json::object{{"price", level.price.to_double()},
                                       {"qty", static_cast<std::int64_t>(level.quantity)},
                                       {"orders", static_cast<std::int64_t>(level.orders)}});
        }
        return out;
    };
    return {{"id", static_cast<std::int64_t>(depth.header.instrument_id.value())},
            {"bids", side(depth.bids)},
            {"asks", side(depth.asks)}};
}

void register_stream(HttpServer& server, auth::AuthService& auth, asio::any_io_executor db_executor,
                     const QuoteCache& cache, StreamOptions options) {
    server.on_upgrade(std::string(kStreamPath),
                      [deps = Deps{&auth, std::move(db_executor), &cache, options}](
                          beast::tcp_stream stream, Request request) -> asio::awaitable<void> {
                          co_await serve(std::move(stream), std::move(request), deps);
                      });
}

}  // namespace mda::gateway
