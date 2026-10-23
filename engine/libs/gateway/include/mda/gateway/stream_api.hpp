#pragma once

// GET /api/v1/stream — live quotes over a WebSocket (docs/api/websocket.md, ADR-0018).

#include <chrono>
#include <cstddef>
#include <string_view>

#include <boost/asio/any_io_executor.hpp>
#include <boost/json/object.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/chain_api.hpp>
#include <mda/gateway/live_bars.hpp>
#include <mda/gateway/quote_cache.hpp>
#include <mda/gateway/server.hpp>
#include <mda/gateway/usage_tracker.hpp>

namespace mda::gateway {

inline constexpr std::string_view kStreamPath = "/api/v1/stream";

struct StreamOptions {
    /// Push changed quotes this often; 0 = as fast as possible (checked every 10 ms).
    std::chrono::milliseconds publish_interval{250};
    std::size_t max_instruments = 200;  ///< per connection
    std::size_t max_depth = 10;         ///< depth subscriptions per connection
    /// How often the session cookie is re-checked (sign-out elsewhere ends the stream).
    std::chrono::seconds reauth_interval{60};
    /// Meters traffic and registers live connections (admin dashboard); optional.
    UsageTracker* usage = nullptr;
    /// Live 1-minute bars for charts ("bars" mode); optional.
    const LiveBars* bars = nullptr;
    /// Option-chain summaries ("chain" mode, by spot instrument); optional.
    const ChainSummaries* chains = nullptr;
    std::size_t max_bars = 10;  ///< bar subscriptions per connection
};

/// Serves the stream on `server`. Authentication runs on `db_executor` like the REST
/// routes; quotes come from `cache`.
void register_stream(HttpServer& server, auth::AuthService& auth, asio::any_io_executor db_executor,
                     const QuoteCache& cache, StreamOptions options = {});

/// One instrument's quote as sent to browsers. Field groups the snapshot doesn't carry
/// are omitted.
[[nodiscard]] boost::json::object quote_json(const messages::MarketSnapshot& snapshot);

/// The same plus "lower_circuit" / "upper_circuit" when the band is known.
[[nodiscard]] boost::json::object quote_json(const CachedQuote& quote);

/// One instrument's 5-level book: {"id", "bids": [{"price","qty","orders"}], "asks": [...]}.
/// Empty levels (price 0, no orders) are left out.
[[nodiscard]] boost::json::object depth_json(const messages::DepthSnapshot& depth);

}  // namespace mda::gateway
