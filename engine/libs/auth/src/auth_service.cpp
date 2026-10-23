#include <algorithm>
#include <utility>

#include <mda/appdb/outbox.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/auth/session_token.hpp>

namespace mda::auth {
namespace {

using Code = AuthError::Code;

constexpr std::string_view kBadCredentials = "invalid username or password";
constexpr std::size_t kMaxTrackedUsernames = 10'000;

AuthError error(Code code, std::string message) {
    return AuthError{.code = code, .message = std::move(message)};
}

}  // namespace

std::int64_t system_clock_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

AuthService::AuthService(UserStore& store, AuthOptions options, Clock clock)
    : store_(&store), options_(options), clock_(std::move(clock)) {
    dummy_hash_ = hash_password("not-a-real-password", options_.argon2).value_or("");
}

std::string AuthService::normalize_username(std::string_view username) {
    if (username.size() < 3 || username.size() > 32) {
        return {};
    }
    std::string normalized;
    for (char c : username) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!allowed) {
            return {};
        }
        normalized += c;
    }
    return normalized;
}

bool AuthService::valid_password(std::string_view password) {
    // Count code points: every byte that is not a UTF-8 continuation byte (10xxxxxx).
    const auto code_points = std::ranges::count_if(
        password, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
    return code_points >= 8 && code_points <= 128;
}

AuthError AuthService::internal(std::string message) const {
    return error(Code::Internal, std::move(message));
}

std::string AuthService::normalize_email(std::string_view email) {
    while (!email.empty() && (email.front() == ' ' || email.front() == '\t')) {
        email.remove_prefix(1);
    }
    while (!email.empty() && (email.back() == ' ' || email.back() == '\t')) {
        email.remove_suffix(1);
    }
    // Deliberately simple: one '@', a non-empty local part, a dotted domain, no spaces.
    // Real validation is the verification email we'll send later.
    const std::size_t at = email.find('@');
    if (email.size() > 254 || at == std::string_view::npos || at == 0 || at > 64 ||
        email.find('@', at + 1) != std::string_view::npos) {
        return {};
    }
    const std::string_view domain = email.substr(at + 1);
    const std::size_t dot = domain.rfind('.');
    if (domain.empty() || dot == std::string_view::npos || dot == 0 || dot + 2 > domain.size() ||
        domain.find("..") != std::string_view::npos) {
        return {};
    }
    std::string normalized;
    for (char c : email) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= ' ' || byte == 0x7F) {
            return {};
        }
        normalized += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return normalized;
}

std::string AuthService::normalize_phone(std::string_view phone) {
    std::string digits;
    bool plus = false;
    for (const char c : phone) {
        if (c >= '0' && c <= '9') {
            digits += c;
        } else if (c == '+' && digits.empty() && !plus) {
            plus = true;
        } else if (c != ' ' && c != '-' && c != '(' && c != ')') {
            return {};
        }
    }
    // Accept 10 digits, 0 + 10 digits (trunk prefix), or 91 + 10 digits.
    if (digits.size() == 11 && digits.front() == '0' && !plus) {
        digits.erase(0, 1);
    } else if (digits.size() == 12 && digits.starts_with("91")) {
        digits.erase(0, 2);
    } else if (plus) {
        return {};  // a non-Indian country code
    }
    // Indian mobile numbers are 10 digits starting with 6-9.
    if (digits.size() != 10 || digits.front() < '6') {
        return {};
    }
    return "+91" + digits;
}

std::expected<User, AuthError> AuthService::signup(const SignupRequest& request) {
    if (!options_.allow_signup) {
        return std::unexpected(error(Code::Permission, "signup is disabled"));
    }
    const std::string name = normalize_username(request.username);
    if (name.empty()) {
        return std::unexpected(
            error(Code::Input, "username must be 3-32 characters: letters, digits, '_', '.', '-'"));
    }
    const std::string email = normalize_email(request.email);
    if (email.empty()) {
        return std::unexpected(error(Code::Input, "enter a valid email address"));
    }
    const std::string phone = normalize_phone(request.phone);
    if (phone.empty()) {
        return std::unexpected(error(Code::Input, "enter a valid Indian mobile number"));
    }
    if (!valid_password(request.password)) {
        return std::unexpected(error(Code::Input, "password must be 8-128 characters"));
    }
    const auto hash = hash_password(request.password, options_.argon2);
    if (!hash) {
        return std::unexpected(internal(hash.error()));
    }
    const std::int64_t now = clock_();
    // The account and its welcome notification are created together, or not at all.
    auto transaction = storage::Transaction::begin(store_->database());
    if (!transaction) {
        return std::unexpected(internal(transaction.error().message));
    }
    const auto id = store_->create_user(
        {.username = name, .email = email, .phone = phone, .password_hash = *hash}, now);
    if (!id) {
        switch (id.error().code) {
            case StoreError::Code::Conflict:
                return std::unexpected(error(Code::Conflict, "username already taken"));
            case StoreError::Code::EmailTaken:
                return std::unexpected(error(Code::Conflict, "email already registered"));
            case StoreError::Code::Database:
                break;
        }
        return std::unexpected(internal(id.error().message));
    }
    if (auto queued = notify(*id, "account.signup", email, name, now); !queued) {
        return std::unexpected(queued.error());
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(internal(committed.error().message));
    }
    return User{.id = *id, .username = name, .created_at = now, .email = email, .phone = phone};
}

std::expected<void, AuthError> AuthService::notify(std::int64_t user_id, std::string_view type,
                                                   std::string_view email,
                                                   std::string_view username, std::int64_t now) {
    // Usernames are [a-z0-9_.-] only, so they need no JSON escaping.
    const std::string payload = R"({"username":")" + std::string(username) + R"("})";
    const std::string dedup =
        std::string(type) + ":" + std::to_string(user_id) + ":" + std::to_string(now);
    auto queued = appdb::enqueue(
        store_->database(),
        {.user_id = user_id, .type = type, .email = email, .payload = payload, .dedup_key = dedup},
        now);
    if (!queued) {
        return std::unexpected(internal(queued.error().message));
    }
    return {};
}

std::expected<User, AuthError> AuthService::delete_account(std::string_view token,
                                                           std::string_view password) {
    const auto user = authenticate(token);
    if (!user) {
        return std::unexpected(user.error());
    }
    const auto record = store_->find_user(user->id);
    if (!record || !*record) {
        return std::unexpected(internal("user disappeared"));
    }
    if (!verify_password(password, (*record)->password_hash)) {
        return std::unexpected(error(Code::Auth, "password is incorrect"));
    }
    // The farewell notification keeps a copy of the email (the user row is gone after this);
    // the notifier purges it once delivered.
    auto transaction = storage::Transaction::begin(store_->database());
    if (!transaction) {
        return std::unexpected(internal(transaction.error().message));
    }
    const std::int64_t now = clock_();
    if (auto queued = notify(user->id, "account.deleted", user->email, user->username, now);
        !queued) {
        return std::unexpected(queued.error());
    }
    if (auto deleted = store_->delete_user(user->id); !deleted) {
        return std::unexpected(internal(deleted.error().message));
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(internal(committed.error().message));
    }
    log_event(user->id, user->username, "account_deleted");
    return user;
}

void AuthService::record_failure(const std::string& username) {
    if (failures_.size() >= kMaxTrackedUsernames) {
        failures_.clear();  // bounded memory even under a username-spraying attack
    }
    Failures& failures = failures_[username];
    ++failures.count;
    if (failures.count >= options_.free_failures) {
        const int doublings = std::min(failures.count - options_.free_failures, 30);
        const std::int64_t delay =
            std::min<std::int64_t>(std::int64_t{1} << doublings, options_.max_delay.count());
        failures.blocked_until = clock_() + delay;
    }
}

void AuthService::log_event(std::int64_t user_id, std::string_view username, std::string_view type,
                            std::string_view ip, std::string_view user_agent) {
    (void)store_->add_event({.user_id = user_id,
                             .username = username,
                             .type = type,
                             .at = clock_(),
                             .ip = ip,
                             .user_agent = user_agent});
}

std::expected<SignedIn, AuthError> AuthService::signin(std::string_view username,
                                                       std::string_view password,
                                                       std::string_view user_agent,
                                                       std::string_view ip) {
    const std::string name = normalize_username(username);
    const std::int64_t now = clock_();
    if (const auto it = failures_.find(name);
        it != failures_.end() && it->second.blocked_until > now) {
        return std::unexpected(
            error(Code::RateLimited, "too many failed attempts; retry in " +
                                         std::to_string(it->second.blocked_until - now) + " s"));
    }

    std::optional<UserRecord> user;
    if (!name.empty()) {
        auto found = store_->find_user_by_name(name);
        if (!found) {
            return std::unexpected(internal(found.error().message));
        }
        user = std::move(*found);
    }
    // Always run one Argon2 verification, so unknown and known users take equally long.
    const bool password_ok =
        verify_password(password, user ? std::string_view(user->password_hash) : dummy_hash_);
    if (!user || !password_ok) {
        if (!name.empty()) {
            record_failure(name);
            log_event(user ? user->id : 0, name, "sign_in_failed", ip, user_agent);
        }
        return std::unexpected(error(Code::Auth, std::string(kBadCredentials)));
    }
    failures_.erase(name);

    std::string token = new_session_token();
    if (token.empty()) {
        return std::unexpected(internal("no secure random source"));
    }
    if (auto created = store_->create_session(hash_token(token), user->id, now, user_agent, ip);
        !created) {
        return std::unexpected(internal(created.error().message));
    }
    log_event(user->id, user->username, "sign_in", ip, user_agent);
    if (token_observer_) {
        token_observer_(token, user->id);
    }
    return SignedIn{.user = {.id = user->id,
                             .username = user->username,
                             .created_at = user->created_at,
                             .role = user->role},
                    .token = std::move(token)};
}

std::expected<User, AuthError> AuthService::authenticate(std::string_view token) {
    if (token.empty()) {
        return std::unexpected(error(Code::Auth, "not signed in"));
    }
    const TokenHash key = hash_token(token);
    const auto session = store_->find_session(key);
    if (!session) {
        return std::unexpected(internal(session.error().message));
    }
    if (!*session) {
        return std::unexpected(error(Code::Auth, "not signed in"));
    }
    const std::int64_t now = clock_();
    if (now - (*session)->last_seen_at > options_.session_idle_timeout.count()) {
        (void)store_->delete_session(key);
        if (const auto owner = store_->find_user((*session)->user_id); owner && *owner) {
            log_event((*owner)->id, (*owner)->username, "session_expired");
        }
        return std::unexpected(error(Code::Auth, "session expired; sign in again"));
    }
    const auto user = store_->find_user((*session)->user_id);
    if (!user) {
        return std::unexpected(internal(user.error().message));
    }
    if (!*user) {
        return std::unexpected(error(Code::Auth, "not signed in"));
    }
    (void)store_->touch_session(key, now);
    const UserRecord& record = **user;
    if (token_observer_) {
        token_observer_(token, record.id);
    }
    return User{.id = record.id,
                .username = record.username,
                .created_at = record.created_at,
                .email = record.email,
                .phone = record.phone,
                .email_verified = record.email_verified_at != 0,
                .phone_verified = record.phone_verified_at != 0,
                .role = record.role};
}

std::expected<User, AuthError> AuthService::signout(std::string_view token, std::string_view ip) {
    auto user = authenticate(token);
    if (!user) {
        return std::unexpected(user.error());
    }
    if (auto deleted = store_->delete_session(hash_token(token)); !deleted) {
        return std::unexpected(internal(deleted.error().message));
    }
    log_event(user->id, user->username, "sign_out", ip);
    return user;
}

std::expected<User, AuthError> AuthService::change_password(std::string_view token,
                                                            std::string_view current,
                                                            std::string_view next) {
    const auto user = authenticate(token);
    if (!user) {
        return std::unexpected(user.error());
    }
    const auto record = store_->find_user(user->id);
    if (!record || !*record) {
        return std::unexpected(internal("user disappeared"));
    }
    if (!verify_password(current, (*record)->password_hash)) {
        return std::unexpected(error(Code::Auth, "current password is incorrect"));
    }
    if (!valid_password(next)) {
        return std::unexpected(error(Code::Input, "password must be 8-128 characters"));
    }
    const auto hash = hash_password(next, options_.argon2);
    if (!hash) {
        return std::unexpected(internal(hash.error()));
    }

    // Password change and revocation of other sessions succeed or fail together.
    auto transaction = storage::Transaction::begin(store_->database());
    if (!transaction) {
        return std::unexpected(internal(transaction.error().message));
    }
    const std::int64_t now = clock_();
    if (auto updated = store_->update_password(user->id, *hash, now); !updated) {
        return std::unexpected(internal(updated.error().message));
    }
    if (auto queued =
            notify(user->id, "account.password_changed", user->email, user->username, now);
        !queued) {
        return std::unexpected(queued.error());
    }
    if (auto revoked = store_->delete_other_sessions(user->id, hash_token(token)); !revoked) {
        return std::unexpected(internal(revoked.error().message));
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(internal(committed.error().message));
    }
    log_event(user->id, user->username, "password_changed");
    return user;
}

int AuthService::purge_expired_sessions() {
    const std::int64_t now = clock_();
    const auto deleted =
        store_->delete_sessions_seen_before(now - options_.session_idle_timeout.count(), now);
    return deleted.value_or(0);
}

}  // namespace mda::auth
