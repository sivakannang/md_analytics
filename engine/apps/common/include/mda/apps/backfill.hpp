#pragma once

// Bar backfill from Kite's historical candles, shared by mda-backfill (by hand, ADR-0021)
// and mda-feedd's automatic backfill of newly subscribed instruments (ADR-0025).

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <mda/bars/bar.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/core/instrument_id.hpp>
#include <mda/kite/rest_client.hpp>

namespace mda::apps {

/// Kite allows 60 days of minute candles per request.
inline constexpr int kMaxBackfillDaysPerRequest = 60;

/// "YYYY-MM-DD HH:MM:SS" in IST for a Unix time (Kite's from/to format).
[[nodiscard]] std::string ist_text(std::int64_t unix_seconds);

/// Kite candles -> bars (source Kite), dropping candles that start at or after `before`
/// (the minute still forming). OI updates: 1 when OI differs from the previous candle's
/// (OI buildup design §5; Kite has no update counts).
[[nodiscard]] std::vector<bars::Bar> to_bars(core::InstrumentId id, const std::vector<kite::Candle>& candles,
                                             std::int64_t before);

/// Mon–Fri 09:15–15:40 IST: past-day backfills wait until after it (CLAUDE.md).
[[nodiscard]] bool in_market_hours(std::int64_t now);

/// What an instrument already has in bars.db.
struct BarCoverage {
    bool past = false;   ///< any bar in the `days` before today
    bool today = false;  ///< any bar today
};

[[nodiscard]] std::expected<BarCoverage, storage::DbError> bar_coverage(bars::BarStore& store,
                                                                        std::uint32_t instrument_id,
                                                                        std::int64_t now, int days);

struct BackfillWindow {
    std::int64_t from = 0;  ///< Unix seconds
    std::int64_t to = 0;
};

/// ADR-0025: during market hours, today's minutes if there are none; otherwise the past
/// `days` up to now if there are no past bars; nothing when the instrument has them.
[[nodiscard]] std::optional<BackfillWindow> auto_backfill_window(std::int64_t now, BarCoverage coverage,
                                                                 int days);

}  // namespace mda::apps
