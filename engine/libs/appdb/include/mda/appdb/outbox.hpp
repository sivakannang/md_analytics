#pragma once

// Notification outbox (ADR-0016): producers call enqueue() inside the same transaction as
// the change that caused the notification; mda-notifier delivers later. A repeated
// dedup_key is ignored, so retried producers never send twice.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <mda/storage/database.hpp>

namespace mda::appdb {

struct NewNotification {
    std::int64_t user_id = 0;  ///< 0 = a system notification (to the admins)
    std::string_view type;     ///< e.g. "account.signup"
    std::string_view email;    ///< recipient snapshot; may be empty
    std::string_view payload;  ///< JSON object
    std::string_view dedup_key;
};

/// Inserts a pending notification due now. Duplicate dedup keys are a no-op.
[[nodiscard]] std::expected<void, storage::DbError> enqueue(storage::Database& db,
                                                            const NewNotification& notification,
                                                            std::int64_t now);

}  // namespace mda::appdb
