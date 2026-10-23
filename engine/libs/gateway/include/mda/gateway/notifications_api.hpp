#pragma once

// Account → Notifications (docs/design/notifications.md §3, unit N5): the signed-in user's
// channels, email verification by code, the Telegram connect link, and per-channel
// switches. The gateway only writes the app DB (codes, outbox rows, user_channels);
// mda-notifier sends the mail and runs the bot. All on the DB thread (`db_executor`).

#include <string>

#include <boost/asio/any_io_executor.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/http.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

/// `telegram_bot`: the bot's username for the t.me link; empty = Telegram not offered.
void register_notification_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                                  asio::any_io_executor db_executor, std::string telegram_bot);

}  // namespace mda::gateway
