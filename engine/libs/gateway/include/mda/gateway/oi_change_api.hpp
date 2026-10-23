#pragma once

// GET /api/v1/chains/{underlying}/oi-change — call and put OI per strike at two times, for
// the OI-by-strike view (docs/design/oi-change-by-strike.md §3–4). The catalog (strikes,
// expiries, spot) is read on `db_executor`; OI and spot "as of a time" come from bars.db on
// `bars_executor`.

#include <vector>

#include <boost/asio/any_io_executor.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/gateway/chain_api.hpp>
#include <mda/gateway/http.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

void register_oi_change_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                               asio::any_io_executor db_executor, bars::BarStore& store,
                               asio::any_io_executor bars_executor, std::vector<ChainInfo> chains);

}  // namespace mda::gateway
