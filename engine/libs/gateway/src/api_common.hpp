#pragma once

// Helpers shared by the gateway's route files (internal to the gateway library).

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/auth/auth_service.hpp>
#include <mda/core/buildup.hpp>
#include <mda/gateway/http.hpp>
#include <mda/gateway/user_api.hpp>

namespace mda::gateway::detail {

using auth::AuthError;
using Code = AuthError::Code;

using auth::AuthError;
using Code = AuthError::Code;

/// Runs `work` on `executor` and resumes the caller (on its own executor) with the result.
template <typename Work>
asio::awaitable<std::invoke_result_t<Work>> run_on(asio::any_io_executor executor, Work work) {
    using Result = std::invoke_result_t<Work>;
    co_return co_await asio::co_spawn(
        executor,
        [work = std::move(work)]() mutable -> asio::awaitable<Result> { co_return work(); },
        asio::use_awaitable);
}

inline Response from_error(const AuthError& error) {
    switch (error.code) {
        case Code::Input:
            return failure(http::status::bad_request, "InputException", error.message);
        case Code::Auth:
            return failure(http::status::unauthorized, "AuthException", error.message);
        case Code::Permission:
            return failure(http::status::forbidden, "PermissionException", error.message);
        case Code::Conflict:
            return failure(http::status::conflict, "ConflictException", error.message);
        case Code::RateLimited:
            return failure(http::status::too_many_requests, "RateLimitException", error.message);
        case Code::Internal:
            break;
    }
    // Internal details stay in the server; the client gets a generic message.
    return failure(http::status::internal_server_error, "GeneralException", "internal error");
}

/// API name of a buildup state; empty for None.
inline std::string_view buildup_name(core::Buildup buildup) {
    switch (buildup) {
        case core::Buildup::LongBuildup:
            return "long_buildup";
        case core::Buildup::ShortBuildup:
            return "short_buildup";
        case core::Buildup::ShortCovering:
            return "short_covering";
        case core::Buildup::LongUnwinding:
            return "long_unwinding";
        case core::Buildup::Neutral:
            return "neutral";
        case core::Buildup::NoOiUpdate:
            return "no_oi_update";
        case core::Buildup::None:
            break;
    }
    return "";
}

inline Response bad_input(std::string_view message) {
    return failure(http::status::bad_request, "InputException", message);
}

inline boost::json::object user_json(const auth::User& user) {
    return {{"user_id", user.id},
            {"username", user.username},
            {"email", user.email},
            {"phone", user.phone},
            {"email_verified", user.email_verified},
            {"phone_verified", user.phone_verified},
            {"role", user.role},
            {"created_at", iso_utc(user.created_at)}};
}

inline std::string session_cookie(std::string_view token, std::int64_t max_age, bool secure) {
    return std::string(kSessionCookie) + "=" + std::string(token) +
           "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" + std::to_string(max_age) +
           (secure ? "; Secure" : "");
}

inline std::string token_of(const Request& request) {
    return cookie(request, kSessionCookie).value_or("");
}

/// Runs `work(user)` for the signed-in user on the DB thread; an error reply otherwise.
template <typename Work>
asio::awaitable<Response> as_user(auth::AuthService& auth, asio::any_io_executor db_executor,
                                  const Request& request, Work work) {
    const std::string token = token_of(request);
    co_return co_await run_on(db_executor, [&]() -> Response {
        const auto user = auth.authenticate(token);
        if (!user) {
            return from_error(user.error());
        }
        return work(*user);
    });
}

/// Authenticates the request on the DB thread and runs `work` only for an admin.
template <typename Work>
asio::awaitable<Response> as_admin(auth::AuthService& auth, asio::any_io_executor db_executor,
                                   const Request& request, Work work) {
    const std::string token = token_of(request);
    co_return co_await run_on(db_executor, [&]() -> Response {
        const auto user = auth.authenticate(token);
        if (!user) {
            return from_error(user.error());
        }
        if (!user->is_admin()) {
            spdlog::warn("admin: user {} '{}' denied {}", user->id, user->username,
                         std::string(request.target()));
            return failure(http::status::forbidden, "PermissionException", "admin only");
        }
        return work(*user);
    });
}

}  // namespace mda::gateway::detail
