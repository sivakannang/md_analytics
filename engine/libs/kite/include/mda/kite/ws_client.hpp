#pragma once

// Kite market data WebSocket client (ADR-0011, docs/design/kite-connectivity.md §4).
//
// One TLS WebSocket to wss://ws.kite.trade: subscribe tokens in the configured mode, read
// frames, detect silence (stale feed), reconnect with exponential back-off and jitter, and
// stop retrying on an auth failure until the session file holds a new token.
// Subscriptions can change while connected: only the difference is sent, no reconnect.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <mda/core/timestamp.hpp>
#include <mda/kite/session.hpp>

namespace mda::kite {

namespace asio = boost::asio;

struct WsClientConfig {
    std::string host = "ws.kite.trade";
    std::string mode = "full";  ///< ltp, quote or full
    std::chrono::seconds stale_after{5};
    std::chrono::milliseconds reconnect_initial{1000};
    std::chrono::milliseconds reconnect_max{30000};
    std::size_t subscribe_batch = 500;  ///< tokens per subscribe message
};

enum class WsState {
    Connecting,
    Connected,     ///< handshake done and tokens subscribed
    Disconnected,  ///< lost or failed; will retry
    AuthExpired,   ///< token rejected; waiting for a new session file
};

[[nodiscard]] std::string_view to_string(WsState state) noexcept;

struct WsHandlers {
    /// A binary market data frame, with the time it was read from the socket.
    std::function<void(std::span<const std::byte> frame, core::Timestamp receive_ts)> on_binary;
    /// A text frame (JSON: errors, messages, order updates).
    std::function<void(std::string_view text)> on_text;
    /// State changes, with a human-readable detail.
    std::function<void(WsState state, std::string_view detail)> on_state;
};

/// Supplies the current session; called before every connection attempt so a new
/// morning token is picked up without restarting.
using SessionProvider = std::function<std::optional<Session>()>;

class KiteWebSocket {
public:
    KiteWebSocket(asio::any_io_executor executor, asio::ssl::context& tls, SessionProvider sessions,
                  WsClientConfig config, WsHandlers handlers);

    /// The tokens to be subscribed to. Used on every (re)connection; on a live
    /// connection the difference is sent at once (unsubscribe dropped tokens, subscribe
    /// and set the mode of new ones). Call on the client's executor.
    void set_tokens(std::vector<std::uint32_t> tokens);
    [[nodiscard]] std::size_t token_count() const noexcept { return tokens_.size(); }

    /// Runs connect / read / reconnect until stop(). Co-spawn it on the executor.
    asio::awaitable<void> run();
    void stop() noexcept { stopping_ = true; }

    [[nodiscard]] std::uint64_t reconnects() const noexcept { return reconnects_; }

private:
    enum class Outcome { Retry, AuthFailed, Stopped };
    asio::awaitable<Outcome> session_once(const Session& session);

    asio::any_io_executor executor_;
    asio::ssl::context* tls_;
    SessionProvider sessions_;
    WsClientConfig config_;
    WsHandlers handlers_;
    std::vector<std::uint32_t> tokens_;   // wanted, sorted and unique
    std::vector<std::uint32_t> live_;     // what the current connection has been told
    std::deque<std::string> outbox_;      // subscription changes waiting to be written
    asio::steady_timer* wake_ = nullptr;  // set while connected: wakes the writer
    bool stopping_ = false;
    std::uint64_t reconnects_ = 0;
    bool connected_long_enough_ = false;
};

/// Kite subscribe/mode messages for `tokens`, split into batches.
[[nodiscard]] std::vector<std::string> subscription_messages(std::span<const std::uint32_t> tokens,
                                                             std::string_view mode,
                                                             std::size_t batch);

/// Messages that turn subscription `from` into `to` (both sorted): unsubscribe for
/// tokens only in `from`, then subscribe + mode for tokens only in `to`.
[[nodiscard]] std::vector<std::string> subscription_changes(std::span<const std::uint32_t> from,
                                                            std::span<const std::uint32_t> to,
                                                            std::string_view mode,
                                                            std::size_t batch);

}  // namespace mda::kite
