#pragma once

// Today's instruments for the processes and tools that talk to Kite (mda-feedd,
// mda-backfill): the permanent registry (data/instruments/registry.csv) plus today's
// instrument list, cached per IST day and refreshed after Kite's 08:30 IST update.
// Logs what it did; on failure logs why and returns nothing.

#include <optional>
#include <string>

#include <mda/instruments/registry.hpp>
#include <mda/kite/rest_client.hpp>
#include <mda/kite/token_map.hpp>
#include <mda/runtime/config.hpp>

namespace mda::apps {

struct DailyInstruments {
    instruments::InstrumentRegistry registry;
    kite::TokenMap tokens;  ///< today's Kite tokens <-> InstrumentIds
};

[[nodiscard]] std::optional<DailyInstruments> load_today_instruments(
    const runtime::KiteConfig& config, kite::RestClient& rest);

/// Today's date in IST ("2026-10-22").
[[nodiscard]] std::string ist_date();

}  // namespace mda::apps
