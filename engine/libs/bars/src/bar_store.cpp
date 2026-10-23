#include <array>
#include <string_view>

#include <mda/bars/bar_store.hpp>

namespace mda::bars {
namespace {

using storage::DbError;
using storage::StepResult;

// Prices in fixed-point ticks (core::Price::raw), as on the bus. WITHOUT ROWID: the key is
// the natural order for "one instrument, a time range". Migration 2 adds
// Bar::oi_update_offset (65535 = unknown, as for older rows).
constexpr std::array<std::string_view, 2> kMigrations = {R"sql(
CREATE TABLE bars_1m (
    instrument_id INTEGER NOT NULL,
    start         INTEGER NOT NULL,
    o             INTEGER NOT NULL,
    h             INTEGER NOT NULL,
    l             INTEGER NOT NULL,
    c             INTEGER NOT NULL,
    volume        INTEGER NOT NULL,
    oi_open       INTEGER NOT NULL,
    oi_high       INTEGER NOT NULL,
    oi_low        INTEGER NOT NULL,
    oi_close      INTEGER NOT NULL,
    oi_updates    INTEGER NOT NULL,
    trades        INTEGER NOT NULL,
    source        INTEGER NOT NULL,
    PRIMARY KEY (instrument_id, start)
) WITHOUT ROWID;
)sql",
                                                          R"sql(
ALTER TABLE bars_1m ADD COLUMN oi_update_offset INTEGER NOT NULL DEFAULT 65535;
)sql"};

constexpr std::string_view kColumns =
    "instrument_id, start, o, h, l, c, volume, oi_open, oi_high, oi_low, oi_close, oi_updates, "
    "trades, source, oi_update_offset";

Bar read_bar(const storage::Statement& s) {
    Bar bar{};
    bar.header = messages::make_header<Bar>(
        core::InstrumentId(static_cast<std::uint32_t>(s.column_int64(0))), 0);
    bar.start = s.column_int64(1);
    bar.open = core::Price::from_raw(s.column_int64(2));
    bar.high = core::Price::from_raw(s.column_int64(3));
    bar.low = core::Price::from_raw(s.column_int64(4));
    bar.close = core::Price::from_raw(s.column_int64(5));
    bar.volume = core::Quantity(s.column_int64(6));
    bar.oi_open = core::Quantity(s.column_int64(7));
    bar.oi_high = core::Quantity(s.column_int64(8));
    bar.oi_low = core::Quantity(s.column_int64(9));
    bar.oi_close = core::Quantity(s.column_int64(10));
    bar.oi_updates = static_cast<std::uint32_t>(s.column_int64(11));
    bar.trades = static_cast<std::uint32_t>(s.column_int64(12));
    bar.source = static_cast<BarSource>(s.column_int64(13));
    bar.oi_update_offset = static_cast<std::uint16_t>(s.column_int64(14));
    return bar;
}

std::expected<std::vector<Bar>, DbError> collect(storage::Statement& statement) {
    std::vector<Bar> bars;
    while (true) {
        const auto row = statement.step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row == StepResult::Done) {
            return bars;
        }
        bars.push_back(read_bar(statement));
    }
}

}  // namespace

std::expected<BarStore, DbError> BarStore::prepare(storage::Database db) {
    if (auto migrated = db.migrate(kMigrations); !migrated) {
        return std::unexpected(migrated.error());
    }
    return BarStore(std::move(db));
}

std::expected<BarStore, DbError> BarStore::open(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
    auto db = storage::Database::open(path);
    if (!db) {
        return std::unexpected(db.error());
    }
    return prepare(std::move(*db));
}

std::expected<BarStore, DbError> BarStore::open_in_memory() {
    auto db = storage::Database::open_in_memory();
    if (!db) {
        return std::unexpected(db.error());
    }
    return prepare(std::move(*db));
}

std::expected<void, DbError> BarStore::write(std::span<const Bar> bars) {
    if (bars.empty()) {
        return {};
    }
    auto transaction = storage::Transaction::begin(db_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    // Live (1) always wins; backfill (2) only fills or replaces backfill.
    auto upsert = db_.prepare(
        "INSERT INTO bars_1m (" + std::string(kColumns) +
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15) "
        "ON CONFLICT (instrument_id, start) DO UPDATE SET o = excluded.o, h = excluded.h, "
        "l = excluded.l, c = excluded.c, volume = excluded.volume, oi_open = excluded.oi_open, "
        "oi_high = excluded.oi_high, oi_low = excluded.oi_low, oi_close = excluded.oi_close, "
        "oi_updates = excluded.oi_updates, trades = excluded.trades, source = excluded.source, "
        "oi_update_offset = excluded.oi_update_offset "
        "WHERE excluded.source = 1 OR bars_1m.source = 2");
    if (!upsert) {
        return std::unexpected(upsert.error());
    }
    for (const Bar& bar : bars) {
        upsert->reset();
        if (auto done = upsert->bind(1, static_cast<std::int64_t>(bar.header.instrument_id.value()))
                            .bind(2, bar.start)
                            .bind(3, bar.open.raw())
                            .bind(4, bar.high.raw())
                            .bind(5, bar.low.raw())
                            .bind(6, bar.close.raw())
                            .bind(7, bar.volume.value())
                            .bind(8, bar.oi_open.value())
                            .bind(9, bar.oi_high.value())
                            .bind(10, bar.oi_low.value())
                            .bind(11, bar.oi_close.value())
                            .bind(12, static_cast<std::int64_t>(bar.oi_updates))
                            .bind(13, static_cast<std::int64_t>(bar.trades))
                            .bind(14, static_cast<std::int64_t>(bar.source))
                            .bind(15, static_cast<std::int64_t>(bar.oi_update_offset))
                            .run();
            !done) {
            return std::unexpected(done.error());
        }
    }
    return transaction->commit();
}

std::expected<std::vector<Bar>, DbError> BarStore::range(std::uint32_t instrument_id,
                                                         std::int64_t from, std::int64_t to) {
    auto statement = db_.prepare(
        "SELECT " + std::string(kColumns) +
        " FROM bars_1m WHERE instrument_id = ?1 AND start >= ?2 AND start < ?3 ORDER BY start");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, static_cast<std::int64_t>(instrument_id)).bind(2, from).bind(3, to);
    return collect(*statement);
}

std::expected<std::vector<Bar>, DbError> BarStore::latest_per_instrument(std::int64_t from,
                                                                         std::int64_t to) {
    auto statement =
        db_.prepare("SELECT " + std::string(kColumns) +
                    " FROM bars_1m b WHERE start = (SELECT MAX(start) FROM bars_1m x "
                    "WHERE x.instrument_id = b.instrument_id AND x.start >= ?1 AND x.start < ?2)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, from).bind(2, to);
    return collect(*statement);
}

std::expected<std::optional<Bar>, DbError> BarStore::latest_before(std::uint32_t instrument_id,
                                                                  std::int64_t before) {
    auto statement = db_.prepare("SELECT " + std::string(kColumns) +
                                 " FROM bars_1m WHERE instrument_id = ?1 AND start < ?2 ORDER BY start DESC LIMIT 1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, static_cast<std::int64_t>(instrument_id)).bind(2, before);
    auto bars = collect(*statement);
    if (!bars) {
        return std::unexpected(bars.error());
    }
    if (bars->empty()) {
        return std::optional<Bar>{};
    }
    return std::optional<Bar>{bars->front()};
}

std::expected<std::unordered_map<std::uint32_t, Bar>, DbError> BarStore::last_ended(
    std::span<const std::uint32_t> instrument_ids, std::int64_t at, std::int64_t earliest, bool with_oi) {
    // Walks the primary key (instrument_id, start) backwards from `at`: one seek per instrument.
    auto statement = db_.prepare("SELECT " + std::string(kColumns) +
                                 " FROM bars_1m WHERE instrument_id = ?1 AND start <= ?2 AND start >= ?3 AND "
                                 "(?4 = 0 OR (oi_close > 0 AND (oi_updates > 0 OR source = ?5))) ORDER BY start DESC LIMIT 1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    std::unordered_map<std::uint32_t, Bar> out;
    for (const std::uint32_t id : instrument_ids) {
        statement->reset();
        statement->bind(1, static_cast<std::int64_t>(id))
            .bind(2, at - 60)
            .bind(3, earliest)
            .bind(4, std::int64_t{with_oi ? 1 : 0})
            .bind(5, static_cast<std::int64_t>(BarSource::Kite));
        auto bars = collect(*statement);
        if (!bars) {
            return std::unexpected(bars.error());
        }
        if (!bars->empty()) {
            out.emplace(id, bars->front());
        }
    }
    return out;
}

std::expected<std::unordered_map<std::uint32_t, std::int64_t>, DbError> BarStore::volumes(std::int64_t from,
                                                                                       std::int64_t to) {
    auto statement = db_.prepare(
        "SELECT instrument_id, SUM(volume) FROM bars_1m WHERE start >= ?1 AND start < ?2 GROUP BY instrument_id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, from).bind(2, to);
    std::unordered_map<std::uint32_t, std::int64_t> out;
    while (true) {
        const auto step = statement->step();
        if (!step) {
            return std::unexpected(step.error());
        }
        if (*step != storage::StepResult::Row) {
            return out;
        }
        out[static_cast<std::uint32_t>(statement->column_int64(0))] = statement->column_int64(1);
    }
}

}  // namespace mda::bars
