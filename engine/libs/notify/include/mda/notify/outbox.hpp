#pragma once

// The notifier's side of the outbox (ADR-0016, docs/design/notifications.md §2, §4):
// expires stale notifications, fans pending ones out into one delivery per recipient and
// channel (routing), hands out due deliveries and records their outcome with back-off.
// Producers only call appdb::enqueue().
//
// Routing: account.* → the user's email (the snapshot in the row), verified or not, as it
// is how a new user is reached; system.* → every admin's enabled, verified channels;
// anything else → the user's enabled, verified channels.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include <mda/notify/channel.hpp>
#include <mda/storage/database.hpp>

namespace mda::notify {

struct OutboxOptions {
    std::chrono::seconds max_age{24 * 3600};  ///< older pending notifications expire unsent
    std::chrono::seconds first_retry{30};     ///< doubled per attempt …
    std::chrono::seconds max_retry{3600};     ///< … up to this
    int max_attempts = 8;                     ///< then the delivery fails
};

class Outbox {
public:
    explicit Outbox(storage::Database& db, OutboxOptions options = {})
        : db_(&db), options_(options) {}

    /// Marks pending notifications older than max_age `expired` (and drops their address).
    [[nodiscard]] std::expected<std::size_t, storage::DbError> expire_stale(std::int64_t now);

    /// Fans pending notifications out into deliveries; returns how many deliveries.
    [[nodiscard]] std::expected<std::size_t, storage::DbError> dispatch(std::int64_t now);

    /// Pending deliveries on `channels` due by `now`, oldest first, rendered.
    [[nodiscard]] std::expected<std::vector<Delivery>, storage::DbError> due(
        std::int64_t now, std::span<const std::string_view> channels, std::size_t limit = 50);

    /// Records an attempt: sent; retried later with back-off; or failed (after
    /// max_attempts, or a permanent error). A finished delivery's address is dropped.
    [[nodiscard]] std::expected<void, storage::DbError> record(const Delivery& delivery,
                                                               const SendResult& result,
                                                               std::int64_t now);

    /// Seconds to wait after the `attempts`-th failed attempt.
    [[nodiscard]] std::int64_t backoff(int attempts) const;
    [[nodiscard]] int max_attempts() const { return options_.max_attempts; }

private:
    storage::Database* db_;
    OutboxOptions options_;
};

struct RoundStats {
    std::size_t expired = 0;     ///< notifications too old to send
    std::size_t dispatched = 0;  ///< deliveries created
    std::size_t sent = 0;
    std::size_t retried = 0;
    std::size_t failed = 0;
};

/// One round: expire, dispatch, then send the due deliveries through their channels (in
/// order; a channel paces itself) and record the outcomes. Deliveries on channels not in
/// `channels` stay pending. `on_error` sees each unsuccessful attempt (no addresses in it).
[[nodiscard]] boost::asio::awaitable<std::expected<RoundStats, storage::DbError>> deliver_due(
    Outbox& outbox, std::span<Channel* const> channels, std::int64_t now,
    const std::function<void(const Delivery&, const SendResult&)>& on_error = {});

}  // namespace mda::notify
