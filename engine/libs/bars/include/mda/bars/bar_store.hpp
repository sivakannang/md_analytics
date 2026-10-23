#pragma once

// data/bars.db: 1-minute bars (ADR-0021). mda-engine is the only live writer; backfill
// inserts only what is missing; the gateway reads it.
//
// A live bar replaces a backfilled one for the same minute; a backfilled bar never replaces
// a live one.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <mda/bars/bar.hpp>
#include <mda/storage/database.hpp>

namespace mda::bars {

class BarStore {
public:
    [[nodiscard]] static std::expected<BarStore, storage::DbError> open(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<BarStore, storage::DbError> open_in_memory();

    /// Writes bars in one transaction (upsert with the source rule above).
    [[nodiscard]] std::expected<void, storage::DbError> write(std::span<const Bar> bars);

    /// One instrument's bars with start in [from, to), oldest first.
    [[nodiscard]] std::expected<std::vector<Bar>, storage::DbError> range(
        std::uint32_t instrument_id, std::int64_t from, std::int64_t to);

    /// Each instrument's latest bar with start in [from, to) (to seed the builder after a restart).
    [[nodiscard]] std::expected<std::vector<Bar>, storage::DbError> latest_per_instrument(
        std::int64_t from, std::int64_t to);

    /// One instrument's last bar starting before `before`, if any.
    [[nodiscard]] std::expected<std::optional<Bar>, storage::DbError> latest_before(
        std::uint32_t instrument_id, std::int64_t before);

    /// For each instrument, its last bar that ended by `at` (start + 60 ≤ at) and started at
    /// or after `earliest`; with `with_oi`, only bars in which OI was published (a live OI
    /// update, or a backfilled bar with OI), so the OI's update time is known. "OI / price as of a
    /// time" for the OI-by-strike view (docs/design/oi-change-by-strike.md §3). Instruments with no
    /// such bar are absent.
    [[nodiscard]] std::expected<std::unordered_map<std::uint32_t, Bar>, storage::DbError>
    last_ended(std::span<const std::uint32_t> instrument_ids, std::int64_t at,
               std::int64_t earliest, bool with_oi);

    /// Total volume per instrument of the bars in [from, to).
    [[nodiscard]] std::expected<std::unordered_map<std::uint32_t, std::int64_t>, storage::DbError>
    volumes(std::int64_t from, std::int64_t to);

    [[nodiscard]] storage::Database& database() noexcept { return db_; }

private:
    explicit BarStore(storage::Database db) : db_(std::move(db)) {}
    [[nodiscard]] static std::expected<BarStore, storage::DbError> prepare(storage::Database db);

    storage::Database db_;
};

}  // namespace mda::bars
