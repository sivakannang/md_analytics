#include <algorithm>
#include <iterator>
#include <random>
#include <variant>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/ssl.h>

#include <mda/kite/login.hpp>
#include <mda/kite/ws_client.hpp>

namespace mda::kite {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using asio::ip::tcp;
using namespace asio::experimental::awaitable_operators;
constexpr auto kUseTuple = asio::as_tuple(asio::use_awaitable);

asio::awaitable<void> sleep_for(std::chrono::milliseconds duration) {
    asio::steady_timer timer(co_await asio::this_coro::executor, duration);
    co_await timer.async_wait(kUseTuple);
}

}  // namespace

std::string_view to_string(WsState state) noexcept {
    switch (state) {
        case WsState::Connecting:
            return "connecting";
        case WsState::Connected:
            return "connected";
        case WsState::Disconnected:
            return "disconnected";
        case WsState::AuthExpired:
            return "auth expired";
    }
    return "?";
}

std::vector<std::string> subscription_messages(std::span<const std::uint32_t> tokens,
                                               std::string_view mode, std::size_t batch) {
    std::vector<std::string> messages;
    for (std::size_t start = 0; start < tokens.size(); start += batch) {
        std::string list;
        for (std::size_t i = start; i < std::min(tokens.size(), start + batch); ++i) {
            list += (i == start ? "" : ",") + std::to_string(tokens[i]);
        }
        messages.push_back(R"({"a":"subscribe","v":[)" + list + "]}");
        messages.push_back(R"({"a":"mode","v":[")" + std::string(mode) + R"(",[)" + list + "]]}");
    }
    return messages;
}

std::vector<std::string> subscription_changes(std::span<const std::uint32_t> from,
                                              std::span<const std::uint32_t> to,
                                              std::string_view mode, std::size_t batch) {
    std::vector<std::uint32_t> removed;
    std::vector<std::uint32_t> added;
    std::ranges::set_difference(from, to, std::back_inserter(removed));
    std::ranges::set_difference(to, from, std::back_inserter(added));

    std::vector<std::string> messages;
    for (std::size_t start = 0; start < removed.size(); start += batch) {
        std::string list;
        for (std::size_t i = start; i < std::min(removed.size(), start + batch); ++i) {
            list += (i == start ? "" : ",") + std::to_string(removed[i]);
        }
        messages.push_back(R"({"a":"unsubscribe","v":[)" + list + "]}");
    }
    for (std::string& message : subscription_messages(added, mode, batch)) {
        messages.push_back(std::move(message));
    }
    return messages;
}

KiteWebSocket::KiteWebSocket(asio::any_io_executor executor, asio::ssl::context& tls,
                             SessionProvider sessions, WsClientConfig config, WsHandlers handlers)
    : executor_(std::move(executor))
    , tls_(&tls)
    , sessions_(std::move(sessions))
    , config_(std::move(config))
    , handlers_(std::move(handlers)) {}

void KiteWebSocket::set_tokens(std::vector<std::uint32_t> tokens) {
    std::ranges::sort(tokens);
    tokens.erase(std::ranges::unique(tokens).begin(), tokens.end());
    tokens_ = std::move(tokens);
    if (wake_ != nullptr) {  // connected: send just the difference
        for (std::string& message :
             subscription_changes(live_, tokens_, config_.mode, config_.subscribe_batch)) {
            outbox_.push_back(std::move(message));
        }
        live_ = tokens_;
        wake_->cancel();
    }
}

asio::awaitable<void> KiteWebSocket::run() {
    std::mt19937 rng(std::random_device{}());
    auto delay = config_.reconnect_initial;
    std::string rejected_token;  // the token Kite refused; wait for a different one

    while (!stopping_) {
        const auto session = sessions_();
        if (!session || session->access_token == rejected_token) {
            co_await sleep_for(std::chrono::seconds(5));  // waiting for mda-login
            continue;
        }
        connected_long_enough_ = false;
        const Outcome outcome = co_await session_once(*session);
        if (outcome == Outcome::Stopped || stopping_) {
            break;
        }
        if (outcome == Outcome::AuthFailed) {
            rejected_token = session->access_token;
            handlers_.on_state(WsState::AuthExpired,
                               "access token rejected; run mda-login (retrying when the session "
                               "file changes)");
            continue;
        }
        rejected_token.clear();
        ++reconnects_;
        if (connected_long_enough_) {
            delay = config_.reconnect_initial;  // it was healthy: start back-off afresh
        }
        // ±20% jitter so many clients don't reconnect in lock-step.
        std::uniform_real_distribution<double> jitter(0.8, 1.2);
        const auto wait = std::chrono::milliseconds(
            static_cast<std::int64_t>(static_cast<double>(delay.count()) * jitter(rng)));
        handlers_.on_state(WsState::Disconnected,
                           "reconnecting in " + std::to_string(wait.count()) + " ms");
        co_await sleep_for(wait);
        delay = std::min(delay * 2, config_.reconnect_max);
    }
}

asio::awaitable<KiteWebSocket::Outcome> KiteWebSocket::session_once(const Session& session) {
    handlers_.on_state(WsState::Connecting, config_.host);

    tcp::resolver resolver(executor_);
    auto [resolve_error, endpoints] =
        co_await resolver.async_resolve(config_.host, "443", kUseTuple);
    if (resolve_error) {
        handlers_.on_state(WsState::Disconnected, "resolve: " + resolve_error.message());
        co_return Outcome::Retry;
    }

    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(executor_, *tls_);
    if (SSL_ctrl(ws.next_layer().native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME,
                 TLSEXT_NAMETYPE_host_name, const_cast<char*>(config_.host.c_str())) != 1) {
        handlers_.on_state(WsState::Disconnected, "cannot set SNI");
        co_return Outcome::Retry;
    }
    ws.next_layer().set_verify_callback(asio::ssl::host_name_verification(config_.host));

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
    auto [connect_error, endpoint] =
        co_await beast::get_lowest_layer(ws).async_connect(endpoints, kUseTuple);
    (void)endpoint;
    if (connect_error) {
        handlers_.on_state(WsState::Disconnected, "connect: " + connect_error.message());
        co_return Outcome::Retry;
    }
    auto [tls_error] =
        co_await ws.next_layer().async_handshake(asio::ssl::stream_base::client, kUseTuple);
    if (tls_error) {
        handlers_.on_state(WsState::Disconnected, "TLS: " + tls_error.message());
        co_return Outcome::Retry;
    }

    // From here the WebSocket layer owns timeouts: silence longer than stale_after (no
    // ticks and no heartbeats) fails the read, which we treat as a dead feed.
    beast::get_lowest_layer(ws).expires_never();
    websocket::stream_base::timeout timeouts{};
    timeouts.handshake_timeout = std::chrono::seconds(10);
    timeouts.idle_timeout = config_.stale_after;
    timeouts.keep_alive_pings = false;
    ws.set_option(timeouts);
    ws.set_option(websocket::stream_base::decorator([](websocket::request_type& request) {
        request.set(http::field::user_agent, "md_analytics/0.1");
        request.set("X-Kite-Version", "3");
    }));

    const std::string target = "/?api_key=" + url_encode(session.api_key) +
                               "&access_token=" + url_encode(session.access_token);
    websocket::response_type response;
    auto [handshake_error] = co_await ws.async_handshake(response, config_.host, target, kUseTuple);
    if (handshake_error) {
        if (response.result() == http::status::forbidden ||
            response.result() == http::status::unauthorized) {
            co_return Outcome::AuthFailed;
        }
        handlers_.on_state(WsState::Disconnected, "handshake: " + handshake_error.message());
        co_return Outcome::Retry;
    }

    ws.text(true);
    for (const std::string& message :
         subscription_messages(tokens_, config_.mode, config_.subscribe_batch)) {
        auto [write_error, written] = co_await ws.async_write(asio::buffer(message), kUseTuple);
        (void)written;
        if (write_error) {
            handlers_.on_state(WsState::Disconnected, "subscribe: " + write_error.message());
            co_return Outcome::Retry;
        }
    }
    live_ = tokens_;
    outbox_.clear();  // changes made while disconnected are already in tokens_
    handlers_.on_state(WsState::Connected, "subscribed " + std::to_string(tokens_.size()) +
                                               " instruments in " + config_.mode + " mode");

    // Reader and writer run side by side; whichever ends first (a read error, a failed
    // write) cancels the other. Beast allows one read and one write in flight.
    asio::steady_timer wake(executor_, asio::steady_timer::time_point::max());
    wake_ = &wake;
    struct Unset {  // wake_ must never outlive `wake`, even if a coroutine throws
        asio::steady_timer*& pointer;
        ~Unset() { pointer = nullptr; }
    } unset{wake_};
    const auto connected_at = std::chrono::steady_clock::now();

    auto reader = [&]() -> asio::awaitable<Outcome> {
        beast::flat_buffer buffer;
        while (!stopping_) {
            auto [read_error, bytes] = co_await ws.async_read(buffer, kUseTuple);
            (void)bytes;
            if (read_error) {
                connected_long_enough_ =
                    std::chrono::steady_clock::now() - connected_at > std::chrono::seconds(60);
                const bool stale = read_error == beast::error::timeout;
                handlers_.on_state(WsState::Disconnected,
                                   stale ? "no data for " +
                                               std::to_string(config_.stale_after.count()) +
                                               " s (stale feed)"
                                         : "read: " + read_error.message());
                co_return Outcome::Retry;
            }
            const core::Timestamp received = core::Timestamp::now();
            const auto data = buffer.cdata();
            if (ws.got_binary()) {
                handlers_.on_binary({static_cast<const std::byte*>(data.data()), data.size()},
                                    received);
            } else {
                handlers_.on_text({static_cast<const char*>(data.data()), data.size()});
            }
            buffer.consume(buffer.size());
        }
        co_return Outcome::Stopped;
    };

    auto writer = [&]() -> asio::awaitable<Outcome> {
        while (true) {
            while (!outbox_.empty()) {
                const std::string message = std::move(outbox_.front());
                outbox_.pop_front();
                auto [write_error, written] =
                    co_await ws.async_write(asio::buffer(message), kUseTuple);
                (void)written;
                if (write_error) {
                    handlers_.on_state(WsState::Disconnected,
                                       "subscribe: " + write_error.message());
                    co_return Outcome::Retry;
                }
            }
            auto [ignored] = co_await wake.async_wait(kUseTuple);  // set_tokens() cancels it
            (void)ignored;
            if ((co_await asio::this_coro::cancellation_state).cancelled() !=
                asio::cancellation_type::none) {
                co_return Outcome::Stopped;  // the reader ended the session
            }
        }
    };

    const auto finished = co_await (reader() || writer());
    co_return std::visit([](Outcome outcome) { return outcome; }, finished);
}

}  // namespace mda::kite
