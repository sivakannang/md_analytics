#pragma once

// Daily import of Kite's instrument list (docs/design/instruments.md).
//
// Kite publishes a CSV of every instrument each morning (GET /instruments). Importing it:
//   - turns each supported row into a ContractKey and registers it (new contracts get a
//     new permanent InstrumentId; known contracts keep theirs),
//   - refreshes daily attributes (symbol, lot size, tick size),
//   - links derivatives to their underlying index/equity,
//   - builds today's Kite token -> InstrumentId map.

#include <cstddef>
#include <expected>
#include <string_view>

#include <mda/instruments/registry.hpp>
#include <mda/kite/token_map.hpp>

namespace mda::kite {

struct ImportStats {
    std::size_t rows = 0;                 ///< data rows in the file
    std::size_t imported = 0;             ///< rows registered and mapped
    std::size_t new_instruments = 0;      ///< contracts seen for the first time
    std::size_t skipped_unsupported = 0;  ///< segments we don't handle yet (CDS, MCX, …)
    std::size_t malformed = 0;            ///< rows with unparseable fields
    std::size_t unlinked = 0;             ///< derivatives whose underlying wasn't found
};

struct ImportResult {
    TokenMap tokens;
    ImportStats stats;
};

enum class ImportError {
    Empty,           ///< no header line
    MissingColumns,  ///< header lacks a required column
};

/// Imports one day's instrument CSV into `registry`. Clears yesterday's daily attributes
/// first. New instruments are registered in memory; persist with registry.save_new().
[[nodiscard]] std::expected<ImportResult, ImportError> import_instruments(
    std::string_view csv, instruments::InstrumentRegistry& registry);

}  // namespace mda::kite
