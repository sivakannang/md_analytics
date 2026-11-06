#pragma once

// data/autotrader.db (ADR-0030, docs/design/auto-trader.md §7): every decision of every
// run, and each run's counters for the day, so its guards survive a restart. mda-autotrader
// is the only writer.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mda/autotrade/run.hpp>
#include <mda/storage/database.hpp>

namespace mda::autotrade {

using storage::DbError;

struct RunDay {
    std::int32_t day = 0;
    int entries = 0;
    Paise realised = 0;
    Paise charges = 0;
    Millis last_exit = 0;
    std::uint32_t counter = 0;
    Millis last_entry = 0;  ///< when the run last went long or short (max hold after a restart)
};

struct StoredDecision {
    std::string run;
    Decision decision;
};

class DecisionStore {
public:
    [[nodiscard]] static std::expected<DecisionStore, DbError> open(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<DecisionStore, DbError> open_in_memory();

    /// Saves a run's decisions and its day counters in one transaction.
    [[nodiscard]] std::expected<void, DbError> save(std::string_view run,
                                                    std::span<const Decision> decisions,
                                                    const RunDay& day);
    [[nodiscard]] std::expected<std::optional<RunDay>, DbError> day(std::string_view run,
                                                                    std::int32_t day);
    /// One run's decisions in [from, to) (Unix ms), oldest first.
    [[nodiscard]] std::expected<std::vector<Decision>, DbError> decisions(std::string_view run,
                                                                          Millis from, Millis to);

private:
    explicit DecisionStore(storage::Database db) : db_(std::move(db)) {}
    [[nodiscard]] static std::expected<DecisionStore, DbError> prepare(storage::Database db);

    storage::Database db_;
};

}  // namespace mda::autotrade
