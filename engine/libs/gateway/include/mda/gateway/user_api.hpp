#pragma once

// /api/v1/user/* and /api/v1/session routes (docs/api/rest.md).
//
// Threading: HTTP runs on the I/O thread; every AuthService call (Argon2 hashing and all
// database access) runs on `auth_executor`, a single worker thread. The I/O thread never
// blocks, and the database is only ever touched from one thread.

#include <boost/asio/any_io_executor.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/http.hpp>

namespace mda::gateway {

inline constexpr std::string_view kSessionCookie = "mda_session";

struct UserApiOptions {
    std::int64_t cookie_max_age_seconds = 30 * 24 * 3600;
    bool secure_cookie =
        false;  ///< always Secure; otherwise only for requests that came over HTTPS (ADR-0027)
};

/// Registers the user/session routes on `router`. `auth` must outlive the router and is
/// only called on `auth_executor`.
void register_user_routes(Router& router, auth::AuthService& auth,
                          asio::any_io_executor auth_executor, UserApiOptions options = {});

}  // namespace mda::gateway
