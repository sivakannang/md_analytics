#pragma once

// GET /api/v1/admin/system — the admin system monitor (docs/design/system-monitor.md §4,
// SM2): the collector's latest sample and history, the notifier's queue from app.db, and
// the checks with the page's overall status. Admins only.

#include <boost/asio/any_io_executor.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/http.hpp>
#include <mda/gateway/system_monitor.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

void register_system_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                            asio::any_io_executor db_executor, const SystemMonitor& monitor);

}  // namespace mda::gateway
