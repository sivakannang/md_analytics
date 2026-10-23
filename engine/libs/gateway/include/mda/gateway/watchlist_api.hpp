#pragma once

// /api/v1/instruments/* and /api/v1/watchlists/* routes (docs/api/rest.md). All require a
// signed-in user. Like the user routes, every database call runs on `db_executor` (the
// single thread that owns the app DB connection).

#include <boost/asio/any_io_executor.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/http.hpp>
#include <mda/storage/database.hpp>
#include <mda/watchlists/watchlists.hpp>

namespace mda::gateway {

void register_watchlist_routes(Router& router, auth::AuthService& auth,
                               watchlists::WatchlistService& lists, storage::Database& db,
                               asio::any_io_executor db_executor);

}  // namespace mda::gateway
