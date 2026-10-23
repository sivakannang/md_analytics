#include <array>

#include <mda/auth/user_store.hpp>

namespace mda::auth {
namespace {

using storage::DbError;
using storage::StepResult;

constexpr std::string_view kUserColumns =
    "id, username, password_hash, created_at, email, phone, email_verified_at, phone_verified_at, "
    "role";

StoreError to_store_error(const DbError& error) {
    if (!error.is_constraint()) {
        return {.code = StoreError::Code::Database, .message = error.message};
    }
    // SQLite names the column: "UNIQUE constraint failed: users.email".
    const bool email = error.message.find("users.email") != std::string::npos;
    return {.code = email ? StoreError::Code::EmailTaken : StoreError::Code::Conflict,
            .message = error.message};
}

std::span<const std::byte> bytes(const TokenHash& hash) {
    return {hash.data(), hash.size()};
}

/// Runs a prepared statement built by `bind` to completion.
template <typename Bind>
std::expected<void, StoreError> run(storage::Database& db, std::string_view sql, Bind bind) {
    auto statement = db.prepare(sql);
    if (!statement) {
        return std::unexpected(to_store_error(statement.error()));
    }
    bind(*statement);
    if (auto done = statement->run(); !done) {
        return std::unexpected(to_store_error(done.error()));
    }
    return {};
}

template <typename Bind>
std::expected<std::optional<UserRecord>, StoreError> query_user(storage::Database& db,
                                                                std::string_view sql, Bind bind) {
    auto statement = db.prepare(sql);
    if (!statement) {
        return std::unexpected(to_store_error(statement.error()));
    }
    bind(*statement);
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(to_store_error(row.error()));
    }
    if (*row == StepResult::Done) {
        return std::optional<UserRecord>{};
    }
    return UserRecord{.id = statement->column_int64(0),
                      .username = statement->column_text(1),
                      .password_hash = statement->column_text(2),
                      .created_at = statement->column_int64(3),
                      .email = statement->column_text(4),
                      .phone = statement->column_text(5),
                      .email_verified_at = statement->column_int64(6),
                      .phone_verified_at = statement->column_int64(7),
                      .role = statement->column_text(8)};
}

}  // namespace

std::expected<std::int64_t, StoreError> UserStore::create_user(const NewUser& user,
                                                               std::int64_t now) {
    auto inserted = run(*db_,
                        "INSERT INTO users (username, email, phone, password_hash, created_at, "
                        "password_changed_at) VALUES (?1, ?2, ?3, ?4, ?5, ?5)",
                        [&](storage::Statement& s) {
                            s.bind(1, user.username)
                                .bind(2, user.email)
                                .bind(3, user.phone)
                                .bind(4, user.password_hash)
                                .bind(5, now);
                        });
    if (!inserted) {
        return std::unexpected(inserted.error());
    }
    return db_->last_insert_id();
}

std::expected<std::optional<UserRecord>, StoreError> UserStore::find_user_by_name(
    std::string_view username) {
    return query_user(*db_,
                      "SELECT " + std::string(kUserColumns) + " FROM users WHERE username = ?1",
                      [&](storage::Statement& s) { s.bind(1, username); });
}

std::expected<std::optional<UserRecord>, StoreError> UserStore::find_user(std::int64_t id) {
    return query_user(*db_, "SELECT " + std::string(kUserColumns) + " FROM users WHERE id = ?1",
                      [&](storage::Statement& s) { s.bind(1, id); });
}

std::expected<void, StoreError> UserStore::update_password(std::int64_t user_id,
                                                           std::string_view password_hash,
                                                           std::int64_t now) {
    return run(
        *db_, "UPDATE users SET password_hash = ?1, password_changed_at = ?2 WHERE id = ?3",
        [&](storage::Statement& s) { s.bind(1, password_hash).bind(2, now).bind(3, user_id); });
}

std::expected<void, StoreError> UserStore::delete_user(std::int64_t user_id) {
    return run(*db_, "DELETE FROM users WHERE id = ?1",
               [&](storage::Statement& s) { s.bind(1, user_id); });
}

std::expected<void, StoreError> UserStore::create_session(const TokenHash& token_hash,
                                                          std::int64_t user_id, std::int64_t now,
                                                          std::string_view user_agent,
                                                          std::string_view ip) {
    return run(*db_,
               "INSERT INTO sessions (token_hash, user_id, created_at, last_seen_at, user_agent, "
               "ip) VALUES (?1, ?2, ?3, ?3, ?4, NULLIF(?5, ''))",
               [&](storage::Statement& s) {
                   s.bind(1, bytes(token_hash))
                       .bind(2, user_id)
                       .bind(3, now)
                       .bind(4, user_agent.substr(0, 256))
                       .bind(5, ip.substr(0, 64));
               });
}

std::expected<std::optional<SessionRecord>, StoreError> UserStore::find_session(
    const TokenHash& token_hash) {
    auto statement =
        db_->prepare("SELECT user_id, last_seen_at FROM sessions WHERE token_hash = ?1");
    if (!statement) {
        return std::unexpected(to_store_error(statement.error()));
    }
    statement->bind(1, bytes(token_hash));
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(to_store_error(row.error()));
    }
    if (*row == StepResult::Done) {
        return std::optional<SessionRecord>{};
    }
    return SessionRecord{.user_id = statement->column_int64(0),
                         .last_seen_at = statement->column_int64(1)};
}

std::expected<void, StoreError> UserStore::touch_session(const TokenHash& token_hash,
                                                         std::int64_t now) {
    return run(*db_, "UPDATE sessions SET last_seen_at = ?1 WHERE token_hash = ?2",
               [&](storage::Statement& s) { s.bind(1, now).bind(2, bytes(token_hash)); });
}

std::expected<void, StoreError> UserStore::delete_session(const TokenHash& token_hash) {
    return run(*db_, "DELETE FROM sessions WHERE token_hash = ?1",
               [&](storage::Statement& s) { s.bind(1, bytes(token_hash)); });
}

std::expected<void, StoreError> UserStore::delete_other_sessions(std::int64_t user_id,
                                                                 const TokenHash& keep) {
    return run(*db_, "DELETE FROM sessions WHERE user_id = ?1 AND token_hash <> ?2",
               [&](storage::Statement& s) { s.bind(1, user_id).bind(2, bytes(keep)); });
}

std::expected<int, StoreError> UserStore::delete_sessions_seen_before(std::int64_t cutoff,
                                                                      std::int64_t now) {
    auto logged = run(*db_,
                      "INSERT INTO auth_events (user_id, username, type, at, ip, user_agent) "
                      "SELECT s.user_id, u.username, 'session_expired', ?2, s.ip, s.user_agent "
                      "FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.last_seen_at < ?1",
                      [&](storage::Statement& s) { s.bind(1, cutoff).bind(2, now); });
    if (!logged) {
        return std::unexpected(logged.error());
    }
    auto deleted = run(*db_, "DELETE FROM sessions WHERE last_seen_at < ?1",
                       [&](storage::Statement& s) { s.bind(1, cutoff); });
    if (!deleted) {
        return std::unexpected(deleted.error());
    }
    return db_->changes();
}

std::expected<void, StoreError> UserStore::add_event(const AuthEvent& event) {
    return run(*db_,
               "INSERT INTO auth_events (user_id, username, type, at, ip, user_agent) VALUES "
               "(NULLIF(?1, 0), ?2, ?3, ?4, NULLIF(?5, ''), NULLIF(?6, ''))",
               [&](storage::Statement& s) {
                   s.bind(1, event.user_id)
                       .bind(2, event.username.substr(0, 64))
                       .bind(3, event.type)
                       .bind(4, event.at)
                       .bind(5, event.ip.substr(0, 64))
                       .bind(6, event.user_agent.substr(0, 256));
               });
}

}  // namespace mda::auth
