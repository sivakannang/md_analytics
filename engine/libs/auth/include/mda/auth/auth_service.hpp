#pragma once

// App authentication rules (ADR-0014, docs/design/user-management.md §4).
//
// Synchronous and CPU-heavy (Argon2id ≈ tens of ms): the gateway runs every call on a
// dedicated worker thread, which is also the only thread that touches the database.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <mda/auth/password.hpp>
#include <mda/auth/user_store.hpp>

namespace mda::auth {

struct AuthError {
    enum class Code {
        Input,        ///< invalid username/password format
        Auth,         ///< wrong credentials, missing or expired session
        Permission,   ///< e.g. signup disabled
        Conflict,     ///< username taken
        RateLimited,  ///< too many failed sign-ins
        Internal,     ///< database or crypto failure
    };
    Code code;
    std::string message;
};

struct User {
    std::int64_t id = 0;
    std::string username;
    std::int64_t created_at = 0;  ///< Unix seconds, UTC
    std::string email;
    std::string phone;  ///< E.164
    bool email_verified = false;
    bool phone_verified = false;
    std::string role = "user";  ///< user | admin (ADR-0019)

    [[nodiscard]] bool is_admin() const noexcept { return role == "admin"; }
};

struct SignupRequest {
    std::string_view username;
    std::string_view email;
    std::string_view phone;
    std::string_view password;
};

struct SignedIn {
    User user;
    std::string token;  ///< give to the client once (cookie); never stored in clear
};

struct AuthOptions {
    bool allow_signup = true;
    std::chrono::seconds session_idle_timeout = std::chrono::hours(24 * 30);
    Argon2Params argon2{};
    int free_failures = 5;  ///< sign-in failures before delays start
    std::chrono::seconds max_delay{60};
};

/// Unix seconds; injectable so tests can move time.
using Clock = std::function<std::int64_t()>;
[[nodiscard]] std::int64_t system_clock_seconds();

class AuthService {
public:
    AuthService(UserStore& store, AuthOptions options = {}, Clock clock = system_clock_seconds);

    [[nodiscard]] std::expected<User, AuthError> signup(const SignupRequest& request);
    /// `ip` is the client address (logged with the session and the auth event).
    [[nodiscard]] std::expected<SignedIn, AuthError> signin(std::string_view username,
                                                            std::string_view password,
                                                            std::string_view user_agent,
                                                            std::string_view ip = {});
    /// The user behind a session token; refreshes the session's idle timer.
    [[nodiscard]] std::expected<User, AuthError> authenticate(std::string_view token);
    /// Ends the session; returns whose it was.
    [[nodiscard]] std::expected<User, AuthError> signout(std::string_view token,
                                                         std::string_view ip = {});
    /// Verifies `current`, sets `next`, and signs out every other session of the user.
    /// Returns the user whose password changed.
    [[nodiscard]] std::expected<User, AuthError> change_password(std::string_view token,
                                                                 std::string_view current,
                                                                 std::string_view next);
    /// Verifies the password, then deletes the account with its sessions and watchlists.
    /// Queues an "account.deleted" notification. Returns who was deleted.
    [[nodiscard]] std::expected<User, AuthError> delete_account(std::string_view token,
                                                                std::string_view password);
    /// Removes sessions idle longer than the timeout (logging session_expired); returns how many.
    int purge_expired_sessions();

    /// Called (on the DB thread) whenever a token is authenticated or issued: lets the
    /// gateway attribute traffic to users without database lookups (ADR-0019).
    using TokenObserver = std::function<void(std::string_view token, std::int64_t user_id)>;
    void set_token_observer(TokenObserver observer) { token_observer_ = std::move(observer); }

    /// Lower-cases and validates a username ([a-z0-9_.-], 3–32); empty if invalid.
    [[nodiscard]] static std::string normalize_username(std::string_view username);
    /// Trims and lower-cases an email address and checks its shape; empty if invalid.
    [[nodiscard]] static std::string normalize_email(std::string_view email);
    /// An Indian mobile number in any common form ("98765 43210", "+91-98765-43210",
    /// "09876543210") as E.164 "+919876543210"; empty if invalid.
    [[nodiscard]] static std::string normalize_phone(std::string_view phone);
    /// 8–128 characters (Unicode code points).
    [[nodiscard]] static bool valid_password(std::string_view password);

private:
    struct Failures {
        int count = 0;
        std::int64_t blocked_until = 0;
    };

    [[nodiscard]] AuthError internal(std::string message) const;
    /// Queues an account notification in the current transaction.
    [[nodiscard]] std::expected<void, AuthError> notify(std::int64_t user_id, std::string_view type,
                                                        std::string_view email,
                                                        std::string_view username,
                                                        std::int64_t now);
    void record_failure(const std::string& username);
    /// Appends to the auth event log; a failure to log never fails the user's request.
    void log_event(std::int64_t user_id, std::string_view username, std::string_view type,
                   std::string_view ip = {}, std::string_view user_agent = {});

    UserStore* store_;
    AuthOptions options_;
    Clock clock_;
    std::string dummy_hash_;  // verified for unknown users, so timing doesn't reveal them
    std::unordered_map<std::string, Failures> failures_;
    TokenObserver token_observer_;
};

}  // namespace mda::auth
