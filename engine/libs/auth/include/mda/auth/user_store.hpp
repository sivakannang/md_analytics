#pragma once

// SQL for users and sessions (docs/design/user-management.md §2). No business rules
// here — just persistence; AuthService decides what is allowed.

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <mda/auth/session_token.hpp>
#include <mda/storage/database.hpp>

namespace mda::auth {

struct UserRecord {
    std::int64_t id = 0;
    std::string username;
    std::string password_hash;
    std::int64_t created_at = 0;  ///< Unix seconds, UTC
    std::string email;            ///< lower-case; empty for accounts created before migration 2
    std::string phone;            ///< E.164, e.g. +919876543210
    std::int64_t email_verified_at = 0;  ///< 0 = not verified
    std::int64_t phone_verified_at = 0;  ///< 0 = not verified
    std::string role = "user";           ///< user | admin (ADR-0019)
};

/// One row of the auth event log (ADR-0019).
struct AuthEvent {
    std::int64_t user_id = 0;  ///< 0 when unknown (failed sign-in for a missing user)
    std::string_view username;
    std::string_view type;  ///< sign_in, sign_in_failed, sign_out, password_changed, ...
    std::int64_t at = 0;
    std::string_view ip;
    std::string_view user_agent;
};

struct NewUser {
    std::string_view username;
    std::string_view email;
    std::string_view phone;
    std::string_view password_hash;
};

struct SessionRecord {
    std::int64_t user_id = 0;
    std::int64_t last_seen_at = 0;
};

struct StoreError {
    enum class Code {
        Conflict,    ///< unique constraint (username taken)
        EmailTaken,  ///< unique constraint on email
        Database,
    };
    Code code;
    std::string message;
};

class UserStore {
public:
    explicit UserStore(storage::Database& db) : db_(&db) {}

    [[nodiscard]] std::expected<std::int64_t, StoreError> create_user(const NewUser& user,
                                                                      std::int64_t now);
    [[nodiscard]] std::expected<std::optional<UserRecord>, StoreError> find_user_by_name(
        std::string_view username);
    [[nodiscard]] std::expected<std::optional<UserRecord>, StoreError> find_user(std::int64_t id);
    [[nodiscard]] std::expected<void, StoreError> update_password(std::int64_t user_id,
                                                                  std::string_view password_hash,
                                                                  std::int64_t now);

    /// Deletes the user; sessions and watchlists go with it (foreign-key cascade).
    [[nodiscard]] std::expected<void, StoreError> delete_user(std::int64_t user_id);

    [[nodiscard]] std::expected<void, StoreError> create_session(const TokenHash& token_hash,
                                                                 std::int64_t user_id,
                                                                 std::int64_t now,
                                                                 std::string_view user_agent,
                                                                 std::string_view ip = {});
    [[nodiscard]] std::expected<std::optional<SessionRecord>, StoreError> find_session(
        const TokenHash& token_hash);
    [[nodiscard]] std::expected<void, StoreError> touch_session(const TokenHash& token_hash,
                                                                std::int64_t now);
    [[nodiscard]] std::expected<void, StoreError> delete_session(const TokenHash& token_hash);
    /// Deletes every session of the user except `keep`.
    [[nodiscard]] std::expected<void, StoreError> delete_other_sessions(std::int64_t user_id,
                                                                        const TokenHash& keep);
    /// Deletes sessions last seen before `cutoff`, logging a session_expired event at `now`
    /// for each; returns how many.
    [[nodiscard]] std::expected<int, StoreError> delete_sessions_seen_before(std::int64_t cutoff,
                                                                             std::int64_t now);

    /// Appends to the auth event log.
    [[nodiscard]] std::expected<void, StoreError> add_event(const AuthEvent& event);

    [[nodiscard]] storage::Database& database() noexcept { return *db_; }

private:
    storage::Database* db_;
};

}  // namespace mda::auth
