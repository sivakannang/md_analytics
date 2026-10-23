#pragma once

// Instrument catalog in the app DB (docs/api/rest.md, "Instruments").
//
// mda-feedd syncs it each morning from the registry after the broker import; the gateway
// searches it. Search is token based, like Zerodha's: every word of the query must appear
// ("nifty 25000 ce", "reliance fut", "hdfc"), ranked index/equity first, then futures,
// then options by nearest expiry.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mda/core/price.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/storage/database.hpp>

namespace mda::catalog {

struct InstrumentInfo {
    std::int64_t id = 0;
    std::string exchange;  ///< NSE | BSE
    std::string market;    ///< CASH | FO
    std::string kind;      ///< IDX | EQ | FUT | OPT
    std::string underlying;
    std::string expiry;  ///< ISO date, empty for cash
    std::optional<core::Price> strike;
    std::string option_type;  ///< CE | PE, empty otherwise
    std::string trading_symbol;
    std::string name;
    std::int64_t lot_size = 0;
    core::Price tick_size;
    std::int64_t underlying_id = 0;
    bool listed = false;
};

struct SyncStats {
    std::size_t listed = 0;  ///< instruments in today's list (upserted)
};

/// Replaces the catalog's daily view with the registry's listed instruments, in one
/// transaction. Instruments no longer listed stay (watchlists may reference them) with
/// listed = 0.
[[nodiscard]] std::expected<SyncStats, storage::DbError> sync(
    storage::Database& db, const instruments::InstrumentRegistry& registry, std::int64_t now);

/// Listed instruments matching every word of `query`, best first, at most `limit`.
[[nodiscard]] std::expected<std::vector<InstrumentInfo>, storage::DbError> search(
    storage::Database& db, std::string_view query, std::size_t limit);

[[nodiscard]] std::expected<std::optional<InstrumentInfo>, storage::DbError> get(
    storage::Database& db, std::int64_t id);

/// The lower-case words a query is matched against (exposed for tests).
[[nodiscard]] std::string search_text(const instruments::Instrument& instrument);

/// Columns selected for InstrumentInfo, in read_info() order.
inline constexpr std::string_view kInfoColumns =
    "i.id, i.exchange, i.market, i.kind, i.underlying, i.expiry, i.strike_raw, i.option_type, "
    "i.trading_symbol, i.name, i.lot_size, i.tick_size_raw, i.underlying_id, i.listed";

/// Reads InstrumentInfo from a row whose first columns are kInfoColumns, starting at `first`.
[[nodiscard]] InstrumentInfo read_info(const storage::Statement& row, int first = 0);

}  // namespace mda::catalog
