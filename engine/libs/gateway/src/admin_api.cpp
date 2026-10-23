#include <algorithm>
#include <unordered_map>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/admin/admin.hpp>
#include <mda/gateway/admin_api.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using detail::as_admin;
using detail::from_error;
using detail::run_on;
using detail::token_of;

constexpr int kUsageDays = 30;

json::value time_or_null(std::int64_t unix_seconds) {
    return unix_seconds == 0 ? json::value(nullptr) : json::value(iso_utc(unix_seconds));
}

json::object usage_json(const admin::Usage& usage) {
    return {{"ws_bytes", usage.ws_bytes},
            {"ws_messages", usage.ws_messages},
            {"http_bytes", usage.http_bytes},
            {"http_requests", usage.http_requests},
            {"total_bytes", usage.ws_bytes + usage.http_bytes}};
}

struct Live {
    std::int64_t connections = 0;
    std::int64_t instruments = 0;
};

std::unordered_map<std::int64_t, Live> live_by_user(const UsageTracker& usage) {
    std::unordered_map<std::int64_t, Live> live;
    for (const auto& connection : usage.connections()) {
        Live& entry = live[connection.user_id];
        entry.connections += 1;
        entry.instruments += static_cast<std::int64_t>(connection.instruments);
    }
    return live;
}

json::object user_json(const admin::UserRow& row, const admin::Usage& today, const Live& live) {
    return {{"user_id", row.id},
            {"username", row.username},
            {"email", mask_email(row.email)},
            {"phone", mask_phone(row.phone)},
            {"role", row.role},
            {"created_at", iso_utc(row.created_at)},
            {"last_sign_in_at", time_or_null(row.last_sign_in_at)},
            {"last_sign_out_at", time_or_null(row.last_sign_out_at)},
            {"last_seen_at", time_or_null(row.last_seen_at)},
            {"active_sessions", row.active_sessions},
            {"online", live.connections},
            {"streaming_instruments", live.instruments},
            {"today", usage_json(today)}};
}

}  // namespace

std::string mask_email(std::string_view email) {
    const std::size_t at = email.find('@');
    if (at == std::string_view::npos) {
        return email.empty() ? "" : "***";
    }
    return std::string(email.substr(0, std::min<std::size_t>(2, at))) + "***" +
           std::string(email.substr(at));
}

std::string mask_phone(std::string_view phone) {
    if (phone.size() < 8) {
        return phone.empty() ? "" : "•••";
    }
    return std::string(phone.substr(0, 3)) + "•••••" + std::string(phone.substr(phone.size() - 5));
}

void register_admin_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                           asio::any_io_executor db_executor, const UsageTracker& usage,
                           std::chrono::seconds session_timeout) {
    // GET /admin/users
    router.add(http::verb::get, "/api/v1/admin/users",
               [&auth, &db, db_executor, &usage, session_timeout](
                   const Request& request, const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await as_admin(auth, db_executor, request, [&](const auth::User&) {
                       const std::int64_t now = auth::system_clock_seconds();
                       const auto rows = admin::list_users(db, admin::ist_day(now),
                                                           now - session_timeout.count());
                       if (!rows) {
                           spdlog::error("admin: listing users failed: {}", rows.error().message);
                           return failure(http::status::internal_server_error, "GeneralException",
                                          "internal error");
                       }
                       const auto live = live_by_user(usage);
                       json::array users;
                       for (const auto& row : *rows) {
                           admin::Usage today = row.today;
                           today += usage.unsaved_today(row.id);
                           const auto found = live.find(row.id);
                           users.push_back(
                               user_json(row, today, found == live.end() ? Live{} : found->second));
                       }
                       return success(std::move(users));
                   });
               });

    // GET /admin/users/{id}
    router.add(
        http::verb::get, "/api/v1/admin/users/{id}",
        [&auth, &db, db_executor, &usage, session_timeout](
            const Request& request, const PathParams& params) -> asio::awaitable<Response> {
            const auto id = param_int(params, "id");
            if (!id) {
                co_return detail::bad_input("numeric user id expected");
            }
            co_return co_await as_admin(auth, db_executor, request, [&](const auth::User&) {
                const std::int64_t now = auth::system_clock_seconds();
                const std::string today = admin::ist_day(now);
                const auto found =
                    admin::user_detail(db, *id, today, now - session_timeout.count(),
                                       admin::ist_day(now - (kUsageDays - 1) * 86'400));
                if (!found) {
                    spdlog::error("admin: user detail failed: {}", found.error().message);
                    return failure(http::status::internal_server_error, "GeneralException",
                                   "internal error");
                }
                if (!*found) {
                    return failure(http::status::not_found, "NotFoundException", "user not found");
                }
                const admin::UserDetail& detail = **found;
                const admin::Usage unsaved = usage.unsaved_today(*id);

                admin::Usage today_usage = detail.user.today;
                today_usage += unsaved;
                Live live;
                json::array connections;
                for (const auto& connection : usage.connections()) {
                    if (connection.user_id == *id) {
                        live.connections += 1;
                        live.instruments += static_cast<std::int64_t>(connection.instruments);
                        connections.push_back(json::object{
                            {"since", iso_utc(connection.since)},
                            {"instruments", static_cast<std::int64_t>(connection.instruments)}});
                    }
                }
                json::array sessions;
                for (const auto& s : detail.sessions) {
                    sessions.push_back(json::object{{"id", s.id},
                                                    {"created_at", iso_utc(s.created_at)},
                                                    {"last_seen_at", iso_utc(s.last_seen_at)},
                                                    {"user_agent", s.user_agent},
                                                    {"ip", s.ip}});
                }
                json::array events;
                for (const auto& e : detail.events) {
                    events.push_back(json::object{{"type", e.type},
                                                  {"at", iso_utc(e.at)},
                                                  {"ip", e.ip},
                                                  {"user_agent", e.user_agent}});
                }
                json::array days;
                bool has_today = false;
                for (const auto& d : detail.usage) {
                    admin::Usage day_usage = d.usage;
                    if (d.day == today) {
                        day_usage += unsaved;
                        has_today = true;
                    }
                    json::object day = usage_json(day_usage);
                    day["day"] = d.day;
                    days.push_back(std::move(day));
                }
                if (!has_today && unsaved != admin::Usage{}) {
                    json::object day = usage_json(unsaved);
                    day["day"] = today;
                    days.push_back(std::move(day));
                }
                return success(json::object{{"user", user_json(detail.user, today_usage, live)},
                                            {"sessions", std::move(sessions)},
                                            {"events", std::move(events)},
                                            {"usage", std::move(days)},
                                            {"connections", std::move(connections)}});
            });
        });

    // DELETE /admin/sessions/{session_id} — force sign-out
    router.add(http::verb::delete_, "/api/v1/admin/sessions/{session_id}",
               [&auth, &db, db_executor](const Request& request,
                                         const PathParams& params) -> asio::awaitable<Response> {
                   std::string session_id;
                   for (const auto& [name, value] : params) {
                       if (name == "session_id") {
                           session_id = value;
                       }
                   }
                   const bool hex =
                       session_id.size() == 64 && std::ranges::all_of(session_id, [](char c) {
                           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                  (c >= 'A' && c <= 'F');
                       });
                   if (!hex) {
                       co_return detail::bad_input("session id must be 64 hex characters");
                   }
                   co_return co_await as_admin(
                       auth, db_executor, request, [&](const auth::User& admin_user) {
                           const auto revoked = admin::revoke_session(
                               db, session_id, auth::system_clock_seconds(), admin_user.username);
                           if (!revoked) {
                               spdlog::error("admin: revoke failed: {}", revoked.error().message);
                               return failure(http::status::internal_server_error,
                                              "GeneralException", "internal error");
                           }
                           if (!*revoked) {
                               return failure(http::status::not_found, "NotFoundException",
                                              "session not found");
                           }
                           spdlog::info("admin: user {} '{}' signed out a session of user {}",
                                        admin_user.id, admin_user.username, **revoked);
                           return success(true);
                       });
               });
}

}  // namespace mda::gateway
