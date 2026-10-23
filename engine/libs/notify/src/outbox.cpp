#include <algorithm>
#include <optional>
#include <string>

#include <mda/notify/outbox.hpp>
#include <mda/notify/templates.hpp>

namespace mda::notify {
namespace {

using storage::DbError;
using storage::StepResult;

struct Recipient {
    std::int64_t user_id = 0;  // 0: none (an account row whose user is gone)
    std::string channel;
    std::string address;
};

/// A user's channels that may carry non-account messages: verified email, linked Telegram,
/// both unless switched off.
std::expected<std::vector<Recipient>, DbError> verified_channels(storage::Database& db, std::int64_t user_id) {
    auto query = db.prepare(
        "SELECT u.email, u.email_verified_at, e.enabled, t.address, t.enabled FROM users u "
        "LEFT JOIN user_channels e ON e.user_id = u.id AND e.channel = 'email' "
        "LEFT JOIN user_channels t ON t.user_id = u.id AND t.channel = 'telegram' WHERE u.id = ?1");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bind(1, user_id);
    std::vector<Recipient> out;
    const auto row = query->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (*row == StepResult::Done) {
        return out;
    }
    const bool email_on = query->column_is_null(2) || query->column_int64(2) != 0;
    if (!query->column_is_null(0) && !query->column_is_null(1) && email_on) {
        out.push_back({.user_id = user_id, .channel = "email", .address = query->column_text(0)});
    }
    if (!query->column_is_null(3) && !query->column_is_null(4) && query->column_int64(4) != 0) {
        out.push_back({.user_id = user_id, .channel = "telegram", .address = query->column_text(3)});
    }
    return out;
}

std::expected<std::vector<std::int64_t>, DbError> admins(storage::Database& db) {
    auto query = db.prepare("SELECT id FROM users WHERE role = 'admin' ORDER BY id");
    if (!query) {
        return std::unexpected(query.error());
    }
    std::vector<std::int64_t> ids;
    while (true) {
        const auto row = query->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row == StepResult::Done) {
            return ids;
        }
        ids.push_back(query->column_int64(0));
    }
}

struct Pending {
    std::int64_t id = 0;
    std::optional<std::int64_t> user_id;
    std::string type;
    std::optional<std::string> email;
};

std::expected<std::vector<Recipient>, DbError> route(storage::Database& db, const Pending& n) {
    if (n.type.starts_with("account.")) {
        std::vector<Recipient> out;
        if (n.email) {
            out.push_back({.user_id = n.user_id.value_or(0), .channel = "email", .address = *n.email});
        }
        return out;
    }
    std::vector<std::int64_t> users;
    if (n.type.starts_with("system.")) {
        auto ids = admins(db);
        if (!ids) {
            return std::unexpected(ids.error());
        }
        users = std::move(*ids);
    } else if (n.user_id) {
        users.push_back(*n.user_id);
    }
    std::vector<Recipient> out;
    for (const std::int64_t user : users) {
        auto channels = verified_channels(db, user);
        if (!channels) {
            return std::unexpected(channels.error());
        }
        out.insert(out.end(), channels->begin(), channels->end());
    }
    return out;
}

}  // namespace

std::int64_t Outbox::backoff(int attempts) const {
    std::int64_t wait = options_.first_retry.count();
    for (int i = 1; i < attempts && wait < options_.max_retry.count(); ++i) {
        wait *= 2;
    }
    return std::min(wait, options_.max_retry.count());
}

std::expected<std::size_t, DbError> Outbox::expire_stale(std::int64_t now) {
    auto update = db_->prepare(
        "UPDATE notifications SET status = 'expired', email = NULL WHERE status = 'pending' AND created_at < ?1");
    if (!update) {
        return std::unexpected(update.error());
    }
    update->bind(1, now - options_.max_age.count());
    if (auto done = update->run(); !done) {
        return std::unexpected(done.error());
    }
    return static_cast<std::size_t>(db_->changes());
}

std::expected<std::size_t, DbError> Outbox::dispatch(std::int64_t now) {
    std::vector<Pending> pending;
    {
        auto query = db_->prepare(
            "SELECT id, user_id, type, email FROM notifications WHERE status = 'pending' ORDER BY id LIMIT 200");
        if (!query) {
            return std::unexpected(query.error());
        }
        while (true) {
            const auto row = query->step();
            if (!row) {
                return std::unexpected(row.error());
            }
            if (*row == StepResult::Done) {
                break;
            }
            Pending n{.id = query->column_int64(0), .type = query->column_text(2)};
            if (!query->column_is_null(1)) {
                n.user_id = query->column_int64(1);
            }
            if (!query->column_is_null(3)) {
                n.email = query->column_text(3);
            }
            pending.push_back(std::move(n));
        }
    }
    std::size_t created = 0;
    for (const Pending& n : pending) {
        auto transaction = storage::Transaction::begin(*db_);
        if (!transaction) {
            return std::unexpected(transaction.error());
        }
        auto recipients = route(*db_, n);
        if (!recipients) {
            return std::unexpected(recipients.error());
        }
        for (const Recipient& r : *recipients) {
            auto insert = db_->prepare(
                "INSERT INTO notification_deliveries (notification_id, user_id, channel, address, status, "
                "next_attempt_at) VALUES (?1, ?2, ?3, ?4, 'pending', ?5) ON CONFLICT DO NOTHING");
            if (!insert) {
                return std::unexpected(insert.error());
            }
            insert->bind(1, n.id).bind(3, std::string_view(r.channel)).bind(4, std::string_view(r.address)).bind(5, now);
            if (r.user_id == 0) {
                insert->bind_null(2);
            } else {
                insert->bind(2, r.user_id);
            }
            if (auto done = insert->run(); !done) {
                return std::unexpected(done.error());
            }
            created += static_cast<std::size_t>(db_->changes());
        }
        // The address now lives in the deliveries only.
        auto mark = db_->prepare("UPDATE notifications SET status = 'dispatched', email = NULL WHERE id = ?1");
        if (!mark) {
            return std::unexpected(mark.error());
        }
        mark->bind(1, n.id);
        if (auto done = mark->run(); !done) {
            return std::unexpected(done.error());
        }
        if (auto committed = transaction->commit(); !committed) {
            return std::unexpected(committed.error());
        }
    }
    return created;
}

std::expected<std::vector<Delivery>, DbError> Outbox::due(std::int64_t now, std::span<const std::string_view> channels,
                                                          std::size_t limit) {
    std::vector<Delivery> out;
    if (channels.empty()) {
        return out;
    }
    std::string in;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        in += (i == 0 ? "?" : ", ?") + std::to_string(i + 3);
    }
    auto query = db_->prepare(
        "SELECT d.id, d.notification_id, d.channel, d.address, d.attempts, n.type, n.payload "
        "FROM notification_deliveries d JOIN notifications n ON n.id = d.notification_id "
        "WHERE d.status = 'pending' AND d.next_attempt_at <= ?1 AND d.channel IN (" +
        in + ") ORDER BY d.next_attempt_at, d.id LIMIT ?2");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bind(1, now).bind(2, static_cast<std::int64_t>(limit));
    for (std::size_t i = 0; i < channels.size(); ++i) {
        query->bind(static_cast<int>(i + 3), channels[i]);
    }
    while (true) {
        const auto row = query->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row == StepResult::Done) {
            return out;
        }
        Delivery d{.id = query->column_int64(0),
                   .notification_id = query->column_int64(1),
                   .channel = query->column_text(2),
                   .address = query->column_text(3),
                   .type = query->column_text(5),
                   .attempts = static_cast<int>(query->column_int64(4))};
        d.message = render(d.type, query->column_text(6));
        out.push_back(std::move(d));
    }
}

std::expected<void, DbError> Outbox::record(const Delivery& delivery, const SendResult& result, std::int64_t now) {
    const int attempts = delivery.attempts + 1;
    const std::string error = result.error.substr(0, 300);
    if (result.outcome == SendOutcome::Sent) {
        auto update = db_->prepare(
            "UPDATE notification_deliveries SET status = 'sent', attempts = ?2, sent_at = ?3, address = NULL, "
            "last_error = NULL WHERE id = ?1");
        if (!update) {
            return std::unexpected(update.error());
        }
        update->bind(1, delivery.id).bind(2, static_cast<std::int64_t>(attempts)).bind(3, now);
        return update->run();
    }
    if (result.outcome == SendOutcome::Fail || attempts >= options_.max_attempts) {
        auto update = db_->prepare(
            "UPDATE notification_deliveries SET status = 'failed', attempts = ?2, address = NULL, last_error = ?3 "
            "WHERE id = ?1");
        if (!update) {
            return std::unexpected(update.error());
        }
        update->bind(1, delivery.id).bind(2, static_cast<std::int64_t>(attempts)).bind(3, std::string_view(error));
        return update->run();
    }
    auto update = db_->prepare(
        "UPDATE notification_deliveries SET attempts = ?2, next_attempt_at = ?3, last_error = ?4 WHERE id = ?1");
    if (!update) {
        return std::unexpected(update.error());
    }
    update->bind(1, delivery.id)
        .bind(2, static_cast<std::int64_t>(attempts))
        .bind(3, now + backoff(attempts))
        .bind(4, std::string_view(error));
    return update->run();
}

boost::asio::awaitable<std::expected<RoundStats, DbError>> deliver_due(
    Outbox& outbox, std::span<Channel* const> channels, std::int64_t now,
    const std::function<void(const Delivery&, const SendResult&)>& on_error) {
    RoundStats stats;
    auto expired = outbox.expire_stale(now);
    if (!expired) {
        co_return std::unexpected(expired.error());
    }
    stats.expired = *expired;
    auto dispatched = outbox.dispatch(now);
    if (!dispatched) {
        co_return std::unexpected(dispatched.error());
    }
    stats.dispatched = *dispatched;
    std::vector<std::string_view> names;
    for (const Channel* channel : channels) {
        names.push_back(channel->name());
    }
    auto due = outbox.due(now, names);
    if (!due) {
        co_return std::unexpected(due.error());
    }
    for (const Delivery& delivery : *due) {
        const auto channel =
            std::ranges::find_if(channels, [&](const Channel* c) { return c->name() == delivery.channel; });
        const SendResult result = co_await (*channel)->send(delivery);
        if (auto recorded = outbox.record(delivery, result, now); !recorded) {
            co_return std::unexpected(recorded.error());
        }
        if (result.outcome == SendOutcome::Sent) {
            ++stats.sent;
            continue;
        }
        const bool last = result.outcome == SendOutcome::Fail || delivery.attempts + 1 >= outbox.max_attempts();
        ++(last ? stats.failed : stats.retried);
        if (on_error) {
            on_error(delivery, result);
        }
    }
    co_return stats;
}

}  // namespace mda::notify
