#pragma once

// data/signals.db (ADR-0028, docs/design/signals.md §6): every signal mda-signald gives,
// with all its factors, and later its outcome. One writer (mda-signald); the gateway reads.
// Prices are fixed-point raw values (core::Price::kScale), like bars.db.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <mda/signals/outcome.hpp>
#include <mda/signals/strategy.hpp>
#include <mda/storage/database.hpp>

namespace mda::signals {

/// A signal as given: which index, and the spot when it was given.
struct SignalRecord {
    std::string underlying;
    Signal signal;
    std::int64_t spot_raw = 0;  ///< spot close at the signal's minute; 0 when unknown
};

/// A stored row, as read back.
struct StoredSignal {
    std::string strategy;
    std::string underlying;
    std::int64_t minute = 0;
    int horizon_min = 0;
    int score = 0;
    Label label = Label::Neutral;
    std::optional<int> prob_bp;
    std::string factors_json;  ///< every factor: id, status, value, points
    std::int64_t spot_raw = 0;
    std::optional<std::int64_t> spot_after_raw;  ///< outcome (SG3); empty until known
    std::optional<int> return_bp;
    std::optional<bool> hit;
};

/// An outcome to store for one signal.
struct OutcomeUpdate {
    std::string strategy;
    std::string underlying;
    std::int64_t minute = 0;
    Outcome outcome;
};

/// One strategy's track record over settled signals (docs/design/signals.md §6). The
/// baselines are measured on the same minutes as the strategy's hits and misses.
struct TrackRecord {
    std::string strategy;
    int horizon_min = 0;
    int settled = 0;  ///< signals with an outcome
    int bullish = 0;
    int bearish = 0;
    int neutral = 0;
    int hits = 0;
    int misses = 0;
    int flats = 0;          ///< Bullish/Bearish calls whose move stayed within the flat band
    int up_moves = 0;       ///< "always up" baseline: of hits + misses, moves that were up
    int momentum_hits = 0;  ///< momentum baseline: the previous horizon's direction was right
    int momentum_calls = 0;  ///< … out of these (minutes with a signal one horizon earlier)
};

/// The factors as JSON: [{"factor":"pcr_change","status":"present","value":0.76,"points":11.42},…]
[[nodiscard]] std::string factors_json(const Signal& signal);

class SignalStore {
public:
    [[nodiscard]] static std::expected<SignalStore, storage::DbError> open(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<SignalStore, storage::DbError> open_in_memory();

    /// Writes the signals in one transaction; a signal for the same strategy, index and
    /// minute replaces the earlier one (a restart may evaluate a minute again).
    [[nodiscard]] std::expected<void, storage::DbError> write(std::span<const SignalRecord> records);

    /// One index's signals with minute in [from, to), oldest first, all strategies.
    [[nodiscard]] std::expected<std::vector<StoredSignal>, storage::DbError> range(
        std::string_view underlying, std::int64_t from, std::int64_t to);

    /// The newest signal minute of one index, if it has any.
    [[nodiscard]] std::expected<std::optional<std::int64_t>, storage::DbError> latest_minute(
        std::string_view underlying);

    /// Signals of one index still waiting for an outcome, oldest first.
    [[nodiscard]] std::expected<std::vector<StoredSignal>, storage::DbError> pending(
        std::string_view underlying);

    /// Stores outcomes in one transaction.
    [[nodiscard]] std::expected<void, storage::DbError> set_outcomes(
        std::span<const OutcomeUpdate> updates);

    /// Each strategy's record for one index over signals with minute in [from, to).
    [[nodiscard]] std::expected<std::vector<TrackRecord>, storage::DbError> record(
        std::string_view underlying, std::int64_t from, std::int64_t to);

private:
    explicit SignalStore(storage::Database db) : db_(std::move(db)) {}
    [[nodiscard]] static std::expected<SignalStore, storage::DbError> prepare(storage::Database db);

    storage::Database db_;
};

}  // namespace mda::signals
