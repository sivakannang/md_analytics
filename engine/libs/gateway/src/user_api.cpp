#include <type_traits>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/gateway/user_api.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

using namespace detail;

}  // namespace

void register_user_routes(Router& router, auth::AuthService& auth,
                          asio::any_io_executor auth_executor, UserApiOptions options) {
    // POST /user/signup {username, email, phone, password}
    router.add(http::verb::post, "/api/v1/user/signup",
               [&auth, auth_executor](const Request& request,
                                      const PathParams&) -> asio::awaitable<Response> {
                   const auto body = json_body(request);
                   const auto username = body ? string_field(*body, "username") : std::nullopt;
                   const auto email = body ? string_field(*body, "email") : std::nullopt;
                   const auto phone = body ? string_field(*body, "phone") : std::nullopt;
                   const auto password = body ? string_field(*body, "password") : std::nullopt;
                   if (!username || !email || !phone || !password) {
                       co_return bad_input(
                           "expected JSON {\"username\", \"email\", \"phone\", \"password\"}");
                   }
                   const auto user = co_await run_on(auth_executor, [&] {
                       return auth.signup({.username = *username,
                                           .email = *email,
                                           .phone = *phone,
                                           .password = *password});
                   });
                   if (!user) {
                       spdlog::warn("auth: sign-up rejected: {}", user.error().message);
                       co_return from_error(user.error());
                   }
                   spdlog::info("auth: user {} '{}' signed up", user->id, user->username);
                   co_return success(user_json(*user), http::status::created);
               });

    // POST /session {username, password} -> Set-Cookie
    router.add(http::verb::post, "/api/v1/session",
               [&auth, auth_executor, options](const Request& request,
                                               const PathParams&) -> asio::awaitable<Response> {
                   const auto body = json_body(request);
                   const auto username = body ? string_field(*body, "username") : std::nullopt;
                   const auto password = body ? string_field(*body, "password") : std::nullopt;
                   if (!username || !password) {
                       co_return bad_input("expected JSON {\"username\", \"password\"}");
                   }
                   const std::string agent(request[http::field::user_agent]);
                   const std::string ip = client_ip(request);
                   const auto signed_in = co_await run_on(
                       auth_executor, [&] { return auth.signin(*username, *password, agent, ip); });
                   if (!signed_in) {
                       spdlog::warn("auth: sign-in failed for '{}': {}", username->substr(0, 32),
                                    signed_in.error().message);
                       co_return from_error(signed_in.error());
                   }
                   spdlog::info("auth: user {} '{}' signed in", signed_in->user.id,
                                signed_in->user.username);
                   Response response = success(boost::json::object{
                       {"user_id", signed_in->user.id}, {"username", signed_in->user.username}});
                   response.set(http::field::set_cookie,
                                session_cookie(signed_in->token, options.cookie_max_age_seconds,
                                               options.secure_cookie || via_https(request)));
                   co_return response;
               });

    // DELETE /session
    router.add(http::verb::delete_, "/api/v1/session",
               [&auth, auth_executor, options](const Request& request,
                                               const PathParams&) -> asio::awaitable<Response> {
                   const std::string token = token_of(request);
                   const std::string ip = client_ip(request);
                   const auto done =
                       co_await run_on(auth_executor, [&] { return auth.signout(token, ip); });
                   if (!done) {
                       co_return from_error(done.error());
                   }
                   spdlog::info("auth: user {} '{}' signed out", done->id, done->username);
                   Response response = success(true);
                   response.set(http::field::set_cookie,
                                session_cookie("", 0, options.secure_cookie || via_https(request)));
                   co_return response;
               });

    // GET /user/profile
    router.add(http::verb::get, "/api/v1/user/profile",
               [&auth, auth_executor](const Request& request,
                                      const PathParams&) -> asio::awaitable<Response> {
                   const std::string token = token_of(request);
                   const auto user =
                       co_await run_on(auth_executor, [&] { return auth.authenticate(token); });
                   if (!user) {
                       co_return from_error(user.error());
                   }
                   co_return success(user_json(*user));
               });

    // PUT /user/password {current_password, new_password}
    router.add(
        http::verb::put, "/api/v1/user/password",
        [&auth, auth_executor](const Request& request,
                               const PathParams&) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            const auto current = body ? string_field(*body, "current_password") : std::nullopt;
            const auto next = body ? string_field(*body, "new_password") : std::nullopt;
            if (!current || !next) {
                co_return bad_input("expected JSON {\"current_password\", \"new_password\"}");
            }
            const std::string token = token_of(request);
            const auto done = co_await run_on(
                auth_executor, [&] { return auth.change_password(token, *current, *next); });
            if (!done) {
                spdlog::warn("auth: password change rejected: {}", done.error().message);
                co_return from_error(done.error());
            }
            spdlog::info("auth: user {} '{}' changed password; other sessions signed out", done->id,
                         done->username);
            co_return success(true);
        });

    // DELETE /user {password}: delete the account (sessions and watchlists go with it).
    router.add(http::verb::delete_, "/api/v1/user",
               [&auth, auth_executor, options](const Request& request,
                                               const PathParams&) -> asio::awaitable<Response> {
                   const auto body = json_body(request);
                   const auto password = body ? string_field(*body, "password") : std::nullopt;
                   if (!password) {
                       co_return bad_input("expected JSON {\"password\"}");
                   }
                   const std::string token = token_of(request);
                   const auto deleted = co_await run_on(
                       auth_executor, [&] { return auth.delete_account(token, *password); });
                   if (!deleted) {
                       spdlog::warn("auth: account deletion rejected: {}", deleted.error().message);
                       co_return from_error(deleted.error());
                   }
                   spdlog::info("auth: user {} '{}' deleted their account", deleted->id,
                                deleted->username);
                   Response response = success(true);
                   response.set(http::field::set_cookie,
                                session_cookie("", 0, options.secure_cookie || via_https(request)));
                   co_return response;
               });
}

}  // namespace mda::gateway
