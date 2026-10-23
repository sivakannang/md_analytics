#pragma once

// Per-user traffic counters and live stream connections, for the admin dashboard
// (ADR-0019). Counting is in memory under one mutex (two additions per write); the gateway
// moves the totals into usage_daily every minute with take().
//
// REST traffic is attributed through a token -> user cache that AuthService fills each time
// it authenticates a token, so metering an HTTP response needs no database lookup.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mda/admin/admin.hpp>
#include <mda/gateway/http.hpp>

namespace mda::gateway {

class UsageTracker {
public:
    using Clock = std::function<std::int64_t()>;  ///< Unix seconds

    explicit UsageTracker(Clock clock);

    /// From AuthService's token observer: `token` belongs to `user_id`.
    void remember_token(std::string_view token, std::int64_t user_id);
    /// From the HTTP server: a response of `bytes` was written for `request`.
    void meter_http(const Request& request, std::size_t bytes);
    /// From the stream: one WebSocket message of `bytes` was sent to `user_id`.
    void meter_ws(std::int64_t user_id, std::size_t bytes);

    struct Connection {
        std::int64_t user_id = 0;
        std::int64_t since = 0;
        std::size_t instruments = 0;
    };
    /// A stream connection opened / its subscription count / closed.
    [[nodiscard]] std::uint64_t open_connection(std::int64_t user_id);
    void set_instruments(std::uint64_t connection, std::size_t instruments);
    void close_connection(std::uint64_t connection);
    [[nodiscard]] std::vector<Connection> connections() const;

    /// Counts not yet saved for `user_id` today.
    [[nodiscard]] admin::Usage unsaved_today(std::int64_t user_id) const;

    struct Pending {
        std::int64_t user_id = 0;
        std::string day;
        admin::Usage usage;
    };
    /// Moves out every unsaved count (to be added to usage_daily).
    [[nodiscard]] std::vector<Pending> take();

private:
    Clock clock_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::int64_t> token_users_;  // SHA-256(token) -> user
    std::map<std::pair<std::int64_t, std::string>, admin::Usage> pending_;  // (user, day)
    std::unordered_map<std::uint64_t, Connection> connections_;
    std::uint64_t next_connection_ = 1;
};

}  // namespace mda::gateway
