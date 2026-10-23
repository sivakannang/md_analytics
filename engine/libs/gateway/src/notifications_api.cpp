#include <algorithm>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/appdb/outbox.hpp>
#include <mda/auth/session_token.hpp>
#include <mda/gateway/admin_api.hpp>
#include <mda/gateway/notifications_api.hpp>
#include <mda/notify/codes.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using detail::as_user;
using detail::bad_input;

constexpr std::chrono::seconds kLinkTtl{10 * 60};

Response internal(std::string_view what, std::string_view message) {
    spdlog::error("notifications: {}: {}", what, message);
    return failure(http::status::internal_server_error, "GeneralException", "internal error");
}

struct ChannelState {
    bool linked = false;  // Telegram: a chat id is stored
    bool enabled = true;  // no row = on
};

std::expected<ChannelState, storage::DbError> channel_state(storage::Database& db, std::int64_t user,
                                                             std::string_view channel) {
    auto query = db.prepare("SELECT address, enabled FROM user_channels WHERE user_id = ?1 AND channel = ?2");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bind(1, user).bind(2, channel);
    const auto row = query->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (*row == storage::StepResult::Done) {
        return ChannelState{};
    }
    return ChannelState{.linked = !query->column_is_null(0), .enabled = query->column_int64(1) != 0};
}

}  // namespace

void register_notification_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                                  asio::any_io_executor db_executor, std::string telegram_bot) {
    // GET /notifications — the user's channels.
    router.add(http::verb::get, "/api/v1/notifications",
               [&auth, &db, db_executor, telegram_bot](const Request& request,
                                                        const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await as_user(auth, db_executor, request, [&](const auth::User& user) {
                       const auto email = channel_state(db, user.id, "email");
                       const auto telegram = channel_state(db, user.id, "telegram");
                       if (!email || !telegram) {
                           return internal("channels", (!email ? email.error() : telegram.error()).message);
                       }
                       return success(json::object{
                           {"email", json::object{{"address", mask_email(user.email)},
                                                  {"verified", user.email_verified},
                                                  {"enabled", email->enabled}}},
                           {"telegram", json::object{{"available", !telegram_bot.empty()},
                                                     {"linked", telegram->linked},
                                                     {"enabled", telegram->linked && telegram->enabled}}},
                           {"phone_verified", user.phone_verified}});
                   });
               });

    // POST /notifications/email/code — email a 6-digit code to the account's address.
    router.add(http::verb::post, "/api/v1/notifications/email/code",
               [&auth, &db, db_executor](const Request& request, const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await as_user(auth, db_executor, request, [&](const auth::User& user) {
                       if (user.email.empty()) {
                           return bad_input("the account has no email address");
                       }
                       if (user.email_verified) {
                           return failure(http::status::conflict, "ConflictException", "email already verified");
                       }
                       const std::int64_t now = auth::system_clock_seconds();
                       const auto code = notify::create_email_code(db, user.id, now);
                       if (!code) {
                           return internal("email code", code.error().message);
                       }
                       if (!*code) {
                           return failure(http::status::too_many_requests, "RateLimitException",
                                          "a code was just sent; wait a minute before asking again");
                       }
                       // Usernames are [a-z0-9_.-] and the code is digits: no JSON escaping needed.
                       const std::string payload = R"({"username":")" + user.username + R"(","code":")" + **code + R"("})";
                       const std::string dedup = "account.email_code:" + std::to_string(user.id) + ":" + std::to_string(now);
                       if (auto queued = appdb::enqueue(db,
                                                        {.user_id = user.id, .type = "account.email_code",
                                                         .email = user.email, .payload = payload, .dedup_key = dedup},
                                                        now);
                           !queued) {
                           return internal("email code", queued.error().message);
                       }
                       spdlog::info("notifications: user {} asked for an email code", user.id);
                       return success(json::object{{"expires_in", notify::kEmailCodeTtl.count()}});
                   });
               });

    // POST /notifications/email/verify {code}
    router.add(http::verb::post, "/api/v1/notifications/email/verify",
               [&auth, &db, db_executor](const Request& request, const PathParams&) -> asio::awaitable<Response> {
                   const auto body = json_body(request);
                   const auto code = body ? string_field(*body, "code") : std::nullopt;
                   if (!code || code->size() != 6) {
                       co_return bad_input("expected JSON {\"code\"} with the 6-digit code");
                   }
                   co_return co_await as_user(auth, db_executor, request, [&](const auth::User& user) {
                       const std::int64_t now = auth::system_clock_seconds();
                       const auto ok = notify::consume_email_code(db, user.id, *code, now);
                       if (!ok) {
                           return internal("email verify", ok.error().message);
                       }
                       if (!*ok) {
                           return bad_input("wrong or expired code");
                       }
                       auto verify = db.prepare("UPDATE users SET email_verified_at = ?2 WHERE id = ?1");
                       if (!verify) {
                           return internal("email verify", verify.error().message);
                       }
                       verify->bind(1, user.id).bind(2, now);
                       if (auto done = verify->run(); !done) {
                           return internal("email verify", done.error().message);
                       }
                       spdlog::info("notifications: user {} verified their email", user.id);
                       return success(true);
                   });
               });

    // POST /notifications/telegram/link — a one-time t.me link that connects the bot.
    router.add(http::verb::post, "/api/v1/notifications/telegram/link",
               [&auth, &db, db_executor, telegram_bot](const Request& request,
                                                        const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await as_user(auth, db_executor, request, [&](const auth::User& user) {
                       if (telegram_bot.empty()) {
                           return failure(http::status::not_found, "NotFoundException", "Telegram is not set up");
                       }
                       const auto code = notify::create_link_code(db, user.id, kLinkTtl, auth::system_clock_seconds());
                       if (!code) {
                           return internal("telegram link", code.error().message);
                       }
                       return success(json::object{{"url", "https://t.me/" + telegram_bot + "?start=" + *code},
                                                   {"expires_in", kLinkTtl.count()}});
                   });
               });

    // DELETE /notifications/telegram — forget the linked chat.
    router.add(http::verb::delete_, "/api/v1/notifications/telegram",
               [&auth, &db, db_executor](const Request& request, const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await as_user(auth, db_executor, request, [&](const auth::User& user) {
                       auto unlink = db.prepare("DELETE FROM user_channels WHERE user_id = ?1 AND channel = 'telegram'");
                       if (!unlink) {
                           return internal("telegram unlink", unlink.error().message);
                       }
                       unlink->bind(1, user.id);
                       if (auto done = unlink->run(); !done) {
                           return internal("telegram unlink", done.error().message);
                       }
                       return success(true);
                   });
               });

    // PUT /notifications/{channel} {enabled} — switch a channel on or off.
    router.add(http::verb::put, "/api/v1/notifications/{channel}",
               [&auth, &db, db_executor](const Request& request, const PathParams& params) -> asio::awaitable<Response> {
                   const auto found =
                       std::ranges::find_if(params, [](const auto& p) { return p.first == "channel"; });
                   const std::string channel = found != params.end() ? found->second : std::string{};
                   const auto body = json_body(request);
                   const auto* enabled = body ? body->if_contains("enabled") : nullptr;
                   if ((channel != "email" && channel != "telegram") || enabled == nullptr || !enabled->is_bool()) {
                       co_return bad_input("channel email|telegram and JSON {\"enabled\": true|false} expected");
                   }
                   const bool on = enabled->as_bool();
                   co_return co_await as_user(auth, db_executor, request, [&](const auth::User& user) {
                       // Email has no address of its own here (users.email); Telegram must be linked first.
                       auto update = channel == "email"
                                         ? db.prepare("INSERT INTO user_channels (user_id, channel, enabled, linked_at) "
                                                      "VALUES (?1, 'email', ?2, ?3) ON CONFLICT (user_id, channel) "
                                                      "DO UPDATE SET enabled = excluded.enabled")
                                         : db.prepare("UPDATE user_channels SET enabled = ?2 WHERE user_id = ?1 AND "
                                                      "channel = 'telegram'");
                       if (!update) {
                           return internal("channel switch", update.error().message);
                       }
                       update->bind(1, user.id).bind(2, std::int64_t{on ? 1 : 0});
                       if (channel == "email") {
                           update->bind(3, auth::system_clock_seconds());
                       }
                       if (auto done = update->run(); !done) {
                           return internal("channel switch", done.error().message);
                       }
                       if (channel == "telegram" && db.changes() == 0) {
                           return failure(http::status::not_found, "NotFoundException", "Telegram is not connected");
                       }
                       return success(true);
                   });
               });
}

}  // namespace mda::gateway
