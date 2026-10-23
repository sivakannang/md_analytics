#pragma once

// /api/v1/admin/* routes (ADR-0019, docs/api/rest.md "Admin"). Admin role only (403
// otherwise). Database work runs on `db_executor`, like the other routes.

#include <chrono>

#include <boost/asio/any_io_executor.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/http.hpp>
#include <mda/gateway/usage_tracker.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

/// `session_timeout`: sessions seen within it count as active (the auth idle timeout).
void register_admin_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                           asio::any_io_executor db_executor, const UsageTracker& usage,
                           std::chrono::seconds session_timeout);

/// "siva@example.com" -> "si***@example.com"
[[nodiscard]] std::string mask_email(std::string_view email);
/// "+919876543210" -> "+91•••••43210"
[[nodiscard]] std::string mask_phone(std::string_view phone);

}  // namespace mda::gateway
