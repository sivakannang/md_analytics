#include <chrono>
#include <cstdio>

#include <mda/admin/admin.hpp>

namespace mda::admin {
namespace {

using storage::DbError;
using storage::StepResult;

constexpr std::int64_t kIstOffset = 5 * 3600 + 30 * 60;

/// Steps `statement` to the end, calling `row` for each row.
template <typename Row>
std::expected<void, DbError> each(storage::Statement& statement, Row row) {
    while (true) {
        const auto step = statement.step();
        if (!step) {
            return std::unexpected(step.error());
        }
        if (*step == StepResult::Done) {
            return {};
        }
        row(statement);
    }
}

// The columns of a UserRow, in order; ?1 = today (YYYY-MM-DD), ?2 = active_since.
constexpr std::string_view kUserSelect = R"sql(
SELECT u.id, u.username, u.email, u.phone, u.role, u.created_at,
       (SELECT MAX(at) FROM auth_events e WHERE e.user_id = u.id AND e.type = 'sign_in'),
       (SELECT MAX(at) FROM auth_events e WHERE e.user_id = u.id AND e.type = 'sign_out'),
       (SELECT MAX(last_seen_at) FROM sessions s WHERE s.user_id = u.id),
       (SELECT COUNT(*) FROM sessions s WHERE s.user_id = u.id AND s.last_seen_at >= ?2),
       COALESCE(d.ws_bytes, 0), COALESCE(d.ws_messages, 0),
       COALESCE(d.http_bytes, 0), COALESCE(d.http_requests, 0)
FROM users u LEFT JOIN usage_daily d ON d.user_id = u.id AND d.day = ?1
)sql";

UserRow read_user(const storage::Statement& s) {
    const auto opt_int = [&](int column) {
        return s.column_is_null(column) ? 0 : s.column_int64(column);
    };
    return UserRow{.id = s.column_int64(0),
                   .username = s.column_text(1),
                   .email = s.column_text(2),
                   .phone = s.column_text(3),
                   .role = s.column_text(4),
                   .created_at = s.column_int64(5),
                   .last_sign_in_at = opt_int(6),
                   .last_sign_out_at = opt_int(7),
                   .last_seen_at = opt_int(8),
                   .active_sessions = s.column_int64(9),
                   .today = {.ws_bytes = s.column_int64(10),
                             .ws_messages = s.column_int64(11),
                             .http_bytes = s.column_int64(12),
                             .http_requests = s.column_int64(13)}};
}

}  // namespace

std::string ist_day(std::int64_t unix_seconds) {
    const std::chrono::sys_days day{std::chrono::floor<std::chrono::days>(
        std::chrono::sys_seconds{std::chrono::seconds{unix_seconds + kIstOffset}})};
    const std::chrono::year_month_day ymd{day};
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return text;
}

std::expected<void, DbError> add_usage(storage::Database& db, std::int64_t user_id,
                                       std::string_view day, const Usage& usage) {
    auto statement = db.prepare(R"sql(
INSERT INTO usage_daily (user_id, day, ws_bytes, ws_messages, http_bytes, http_requests)
SELECT ?1, ?2, ?3, ?4, ?5, ?6 WHERE EXISTS (SELECT 1 FROM users WHERE id = ?1)
ON CONFLICT (user_id, day) DO UPDATE SET
    ws_bytes = ws_bytes + excluded.ws_bytes,
    ws_messages = ws_messages + excluded.ws_messages,
    http_bytes = http_bytes + excluded.http_bytes,
    http_requests = http_requests + excluded.http_requests
)sql");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, user_id)
        .bind(2, day)
        .bind(3, usage.ws_bytes)
        .bind(4, usage.ws_messages)
        .bind(5, usage.http_bytes)
        .bind(6, usage.http_requests)
        .run();
}

std::expected<std::vector<UserRow>, DbError> list_users(storage::Database& db,
                                                        std::string_view today,
                                                        std::int64_t active_since) {
    auto statement = db.prepare(std::string(kUserSelect) + " ORDER BY u.id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, today).bind(2, active_since);
    std::vector<UserRow> users;
    if (auto done =
            each(*statement, [&](const storage::Statement& s) { users.push_back(read_user(s)); });
        !done) {
        return std::unexpected(done.error());
    }
    return users;
}

std::expected<std::optional<UserDetail>, DbError> user_detail(
    storage::Database& db, std::int64_t user_id, std::string_view today, std::int64_t active_since,
    std::string_view since_day, int event_limit) {
    auto user = db.prepare(std::string(kUserSelect) + " WHERE u.id = ?3");
    if (!user) {
        return std::unexpected(user.error());
    }
    user->bind(1, today).bind(2, active_since).bind(3, user_id);
    const auto found = user->step();
    if (!found) {
        return std::unexpected(found.error());
    }
    if (*found == StepResult::Done) {
        return std::optional<UserDetail>{};
    }
    UserDetail detail{.user = read_user(*user)};

    auto sessions = db.prepare(
        "SELECT lower(hex(token_hash)), created_at, last_seen_at, COALESCE(user_agent, ''), "
        "COALESCE(ip, '') FROM sessions WHERE user_id = ?1 AND last_seen_at >= ?2 "
        "ORDER BY last_seen_at DESC");
    if (!sessions) {
        return std::unexpected(sessions.error());
    }
    sessions->bind(1, user_id).bind(2, active_since);
    if (auto done = each(*sessions,
                         [&](const storage::Statement& s) {
                             detail.sessions.push_back({.id = s.column_text(0),
                                                        .created_at = s.column_int64(1),
                                                        .last_seen_at = s.column_int64(2),
                                                        .user_agent = s.column_text(3),
                                                        .ip = s.column_text(4)});
                         });
        !done) {
        return std::unexpected(done.error());
    }

    auto events = db.prepare(
        "SELECT type, at, COALESCE(ip, ''), COALESCE(user_agent, '') FROM auth_events "
        "WHERE user_id = ?1 ORDER BY at DESC, id DESC LIMIT ?2");
    if (!events) {
        return std::unexpected(events.error());
    }
    events->bind(1, user_id).bind(2, static_cast<std::int64_t>(event_limit));
    if (auto done = each(*events,
                         [&](const storage::Statement& s) {
                             detail.events.push_back({.type = s.column_text(0),
                                                      .at = s.column_int64(1),
                                                      .ip = s.column_text(2),
                                                      .user_agent = s.column_text(3)});
                         });
        !done) {
        return std::unexpected(done.error());
    }

    auto usage = db.prepare(
        "SELECT day, ws_bytes, ws_messages, http_bytes, http_requests FROM usage_daily "
        "WHERE user_id = ?1 AND day >= ?2 ORDER BY day");
    if (!usage) {
        return std::unexpected(usage.error());
    }
    usage->bind(1, user_id).bind(2, since_day);
    if (auto done =
            each(*usage,
                 [&](const storage::Statement& s) {
                     detail.usage.push_back({.day = s.column_text(0),
                                             .usage = {.ws_bytes = s.column_int64(1),
                                                       .ws_messages = s.column_int64(2),
                                                       .http_bytes = s.column_int64(3),
                                                       .http_requests = s.column_int64(4)}});
                 });
        !done) {
        return std::unexpected(done.error());
    }
    return detail;
}

std::expected<std::optional<std::int64_t>, DbError> revoke_session(storage::Database& db,
                                                                   std::string_view session_id,
                                                                   std::int64_t now,
                                                                   std::string_view by) {
    auto transaction = storage::Transaction::begin(db);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    auto find = db.prepare(
        "SELECT s.user_id, u.username, COALESCE(s.ip, ''), COALESCE(s.user_agent, '') FROM "
        "sessions s "
        "JOIN users u ON u.id = s.user_id WHERE lower(hex(s.token_hash)) = lower(?1)");
    if (!find) {
        return std::unexpected(find.error());
    }
    find->bind(1, session_id);
    const auto row = find->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (*row == StepResult::Done) {
        return std::optional<std::int64_t>{};
    }
    const std::int64_t user_id = find->column_int64(0);
    const std::string username = find->column_text(1);
    const std::string ip = find->column_text(2);
    const std::string agent = "revoked by " + std::string(by);

    auto remove = db.prepare("DELETE FROM sessions WHERE lower(hex(token_hash)) = lower(?1)");
    auto log = db.prepare(
        "INSERT INTO auth_events (user_id, username, type, at, ip, user_agent) "
        "VALUES (?1, ?2, 'session_revoked', ?3, NULLIF(?4, ''), ?5)");
    if (!remove || !log) {
        return std::unexpected(!remove ? remove.error() : log.error());
    }
    if (auto done = remove->bind(1, session_id).run(); !done) {
        return std::unexpected(done.error());
    }
    if (auto done = log->bind(1, user_id)
                        .bind(2, std::string_view(username))
                        .bind(3, now)
                        .bind(4, std::string_view(ip))
                        .bind(5, std::string_view(agent))
                        .run();
        !done) {
        return std::unexpected(done.error());
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return user_id;
}

std::expected<bool, DbError> set_role(storage::Database& db, std::string_view username,
                                      std::string_view role) {
    auto statement = db.prepare("UPDATE users SET role = ?1 WHERE username = ?2");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    if (auto done = statement->bind(1, role).bind(2, username).run(); !done) {
        return std::unexpected(done.error());
    }
    return db.changes() > 0;
}

std::expected<int, DbError> purge_events(storage::Database& db, std::int64_t before) {
    auto statement = db.prepare("DELETE FROM auth_events WHERE at < ?1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    if (auto done = statement->bind(1, before).run(); !done) {
        return std::unexpected(done.error());
    }
    return db.changes();
}

}  // namespace mda::admin
