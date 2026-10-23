#pragma once

// GET /api/v1/instruments/{id}/bars — chart history from bars.db (ADR-0021) with each
// bar's OI buildup state (docs/design/oi-buildup-chart.md §6). Queries run on
// `bars_executor` (bars.db's own thread) after the session and the instrument's lot size
// are read on `db_executor`.

#include <boost/asio/any_io_executor.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/gateway/http.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

void register_bars_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                          asio::any_io_executor db_executor, bars::BarStore& store,
                          asio::any_io_executor bars_executor);

}  // namespace mda::gateway
