#pragma once

// Queries behind the admin dashboard and mda-useradm (ADR-0019): users with their sign-in
// history, sessions and daily usage; force sign-out; roles; event retention.
// Plain functions over the app DB; callers decide which thread runs them.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mda/storage/database.hpp>

namespace mda::admin {

/// Traffic one user caused on one IST day (bytes as written by the gateway).
struct Usage {
    std::int64_t ws_bytes = 0;
    std::int64_t ws_messages = 0;
    std::int64_t http_bytes = 0;
    std::int64_t http_requests = 0;

    Usage& operator+=(const Usage& other) {
        ws_bytes += other.ws_bytes;
        ws_messages += other.ws_messages;
        http_bytes += other.http_bytes;
        http_requests += other.http_requests;
        return *this;
    }
    friend bool operator==(const Usage&, const Usage&) = default;
};

/// The Indian trading day (IST, UTC+5:30) of a Unix time, as YYYY-MM-DD.
[[nodiscard]] std::string ist_day(std::int64_t unix_seconds);

/// Adds `usage` to the user's row for `day` (creating it). A user deleted meanwhile is
/// skipped, not an error.
[[nodiscard]] std::expected<void, storage::DbError> add_usage(storage::Database& db,
                                                              std::int64_t user_id,
                                                              std::string_view day,
                                                              const Usage& usage);

struct UserRow {
    std::int64_t id = 0;
    std::string username;
    std::string email;
    std::string phone;
    std::string role;
    std::int64_t created_at = 0;
    std::int64_t last_sign_in_at = 0;   ///< 0 = never (since the log started)
    std::int64_t last_sign_out_at = 0;  ///< 0 = never
    std::int64_t last_seen_at = 0;      ///< latest activity of any session; 0 = none
    std::int64_t active_sessions = 0;   ///< sessions seen since `active_since`
    Usage today;                        ///< saved so far (the gateway adds unsaved counts)
};

[[nodiscard]] std::expected<std::vector<UserRow>, storage::DbError> list_users(
    storage::Database& db, std::string_view today, std::int64_t active_since);

struct SessionRow {
    std::string id;  ///< hex SHA-256 of the token (never the token)
    std::int64_t created_at = 0;
    std::int64_t last_seen_at = 0;
    std::string user_agent;
    std::string ip;
};

struct EventRow {
    std::string type;
    std::int64_t at = 0;
    std::string ip;
    std::string user_agent;
};

struct DayUsage {
    std::string day;
    Usage usage;
};

struct UserDetail {
    UserRow user;
    std::vector<SessionRow> sessions;  ///< active ones, most recent first
    std::vector<EventRow> events;      ///< newest first, at most `event_limit`
    std::vector<DayUsage> usage;       ///< days >= since_day, oldest first
};

[[nodiscard]] std::expected<std::optional<UserDetail>, storage::DbError> user_detail(
    storage::Database& db, std::int64_t user_id, std::string_view today, std::int64_t active_since,
    std::string_view since_day, int event_limit = 50);

/// Deletes the session with this id and logs session_revoked; the owner's id, or nothing
/// if there is no such session.
[[nodiscard]] std::expected<std::optional<std::int64_t>, storage::DbError> revoke_session(
    storage::Database& db, std::string_view session_id, std::int64_t now, std::string_view by);

/// Sets a user's role ("user" or "admin"); false if there is no such user.
[[nodiscard]] std::expected<bool, storage::DbError> set_role(storage::Database& db,
                                                             std::string_view username,
                                                             std::string_view role);

/// Deletes auth events older than `before`; returns how many.
[[nodiscard]] std::expected<int, storage::DbError> purge_events(storage::Database& db,
                                                                std::int64_t before);

}  // namespace mda::admin
