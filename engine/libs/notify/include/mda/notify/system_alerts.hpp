#pragma once

// System alerts to the admins (docs/design/notifications.md §5, unit N6): when to raise
// them. The producers enqueue what these return (appdb::enqueue, user 0); dedup keys make a
// repeat a no-op, so restarts and retries never send twice.
// - Feed down: not connected for `grace` while the market is open (Mon–Fri 09:00–15:45
//   IST); "back" once it reconnects after a down alert.
// - Kite session rejected: once per IST day, between 07:00 and 16:00 (the token lapses
//   around 06:00; nobody needs that at 06:00).
// - Login reminder: Mon–Fri from 08:45 until the close, when the session file wasn't
//   written today.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mda::notify {

struct Alert {
    std::string type;     ///< system.*
    std::string payload;  ///< JSON object (templates.cpp)
    std::string dedup_key;
};

/// "2026-10-23" and "09:41:07" in IST.
[[nodiscard]] std::string ist_date(std::int64_t unix_seconds);
[[nodiscard]] std::string ist_clock(std::int64_t unix_seconds);

enum class FeedCondition { Up, Down, AuthRejected };

class FeedAlerts {
public:
    struct Options {
        std::chrono::seconds grace{60};
        int market_open = 9 * 60;         ///< IST minute of day: feed alerts from here …
        int market_close = 15 * 60 + 45;  ///< … until here
        int session_from = 7 * 60;        ///< session alerts from here …
        int session_until = 16 * 60;      ///< … until here
    };

    FeedAlerts() = default;
    explicit FeedAlerts(Options options) : options_(options) {}

    /// The feed's condition now (call about once a second); alerts to raise.
    [[nodiscard]] std::vector<Alert> update(FeedCondition condition, std::string_view detail,
                                            std::int64_t now);

    /// The once-a-day "session rejected" alert, if its hours allow it now.
    [[nodiscard]] std::optional<Alert> session_rejected(std::string_view process,
                                                        std::int64_t now) const;

private:
    Options options_;
    std::optional<std::int64_t> down_since_;
    bool down_alerted_ = false;
    std::string session_alerted_on_;  ///< IST date of the last session alert
};

/// The morning reminder, if today's session file is missing (`written` = its last write).
[[nodiscard]] std::optional<Alert> login_reminder(std::int64_t now,
                                                  std::optional<std::int64_t> written);

}  // namespace mda::notify
