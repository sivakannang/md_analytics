#include <mda/appdb/outbox.hpp>

namespace mda::appdb {

std::expected<void, storage::DbError> enqueue(storage::Database& db, const NewNotification& n,
                                              std::int64_t now) {
    auto statement = db.prepare(
        "INSERT INTO notifications (user_id, type, email, payload, dedup_key, status, "
        "next_attempt_at, created_at) VALUES (?1, ?2, ?3, ?4, ?5, 'pending', ?6, ?6) "
        "ON CONFLICT(dedup_key) DO NOTHING");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(2, n.type).bind(4, n.payload).bind(5, n.dedup_key).bind(6, now);
    if (n.user_id == 0) {  // a system notification: no user
        statement->bind_null(1);
    } else {
        statement->bind(1, n.user_id);
    }
    if (n.email.empty()) {
        statement->bind_null(3);
    } else {
        statement->bind(3, n.email);
    }
    return statement->run();
}

}  // namespace mda::appdb
