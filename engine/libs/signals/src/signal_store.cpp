#include <mda/signals/signal_store.hpp>

#include <array>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

namespace mda::signals {
namespace {

using storage::DbError;
using storage::StepResult;

constexpr std::array<std::string_view, 1> kMigrations = {R"sql(
CREATE TABLE signal (
    strategy     TEXT    NOT NULL,
    underlying   TEXT    NOT NULL,
    minute       INTEGER NOT NULL,   -- minute start, Unix seconds
    horizon_min  INTEGER NOT NULL,
    score        INTEGER NOT NULL,   -- -100 … +100
    label        INTEGER NOT NULL,   -- -1 bearish, 0 neutral, 1 bullish
    prob_bp      INTEGER,            -- calibrated P(up); NULL until calibrated
    factors      TEXT    NOT NULL,   -- JSON, every factor
    spot         INTEGER NOT NULL,   -- core::Price raw; 0 = unknown
    spot_after   INTEGER,            -- outcome: spot at minute + horizon
    return_bp    INTEGER,
    hit          INTEGER,            -- 1 / 0; NULL for neutral or flat
    PRIMARY KEY (strategy, underlying, minute)
) WITHOUT ROWID;
CREATE INDEX signal_by_underlying ON signal (underlying, minute);
)sql"};

constexpr std::string_view kColumns =
    "strategy, underlying, minute, horizon_min, score, label, prob_bp, factors, spot, "
    "spot_after, return_bp, hit";

std::string_view status_name(FactorStatus status) {
    switch (status) {
    case FactorStatus::Present:
        return "present";
    case FactorStatus::Absent:
        return "absent";
    case FactorStatus::NotApplicable:
        return "not_applicable";
    }
    return "absent";
}

std::optional<std::int64_t> optional_int(const storage::Statement& statement, int column) {
    if (statement.column_is_null(column)) {
        return std::nullopt;
    }
    return statement.column_int64(column);
}

std::expected<std::vector<StoredSignal>, DbError> collect(storage::Statement& statement) {
    std::vector<StoredSignal> rows;
    while (true) {
        const auto row = statement.step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row == StepResult::Done) {
            return rows;
        }
        const auto& s = statement;
        const auto prob = optional_int(s, 6);
        const auto return_bp = optional_int(s, 10);
        const auto hit = optional_int(s, 11);
        rows.push_back(StoredSignal{
            .strategy = s.column_text(0),
            .underlying = s.column_text(1),
            .minute = s.column_int64(2),
            .horizon_min = static_cast<int>(s.column_int64(3)),
            .score = static_cast<int>(s.column_int64(4)),
            .label = static_cast<Label>(s.column_int64(5)),
            .prob_bp = prob ? std::optional<int>(static_cast<int>(*prob)) : std::nullopt,
            .factors_json = s.column_text(7),
            .spot_raw = s.column_int64(8),
            .spot_after_raw = optional_int(s, 9),
            .return_bp = return_bp ? std::optional<int>(static_cast<int>(*return_bp)) : std::nullopt,
            .hit = hit ? std::optional<bool>(*hit != 0) : std::nullopt,
        });
    }
}

}  // namespace

std::string factors_json(const Signal& signal) {
    std::string json = "[";
    for (const auto& factor : signal.factors) {
        if (json.size() > 1) {
            json += ',';
        }
        json += std::format(R"({{"factor":"{}","status":"{}","value":{:.4f},"points":{:.2f}}})",
                            name(factor.id), status_name(factor.status), factor.value, factor.points);
    }
    json += ']';
    return json;
}

std::expected<SignalStore, DbError> SignalStore::prepare(storage::Database db) {
    if (auto migrated = db.migrate(kMigrations); !migrated) {
        return std::unexpected(migrated.error());
    }
    return SignalStore(std::move(db));
}

std::expected<SignalStore, DbError> SignalStore::open(const std::filesystem::path& path) {
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

std::expected<SignalStore, DbError> SignalStore::open_in_memory() {
    auto db = storage::Database::open_in_memory();
    if (!db) {
        return std::unexpected(db.error());
    }
    return prepare(std::move(*db));
}

std::expected<void, DbError> SignalStore::write(std::span<const SignalRecord> records) {
    if (records.empty()) {
        return {};
    }
    auto transaction = storage::Transaction::begin(db_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    // A re-evaluated minute replaces the signal; its outcome is worked out again (SG3).
    auto upsert = db_.prepare("INSERT OR REPLACE INTO signal (" + std::string(kColumns) +
                              ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, NULL, ?7, ?8, NULL, NULL, NULL)");
    if (!upsert) {
        return std::unexpected(upsert.error());
    }
    for (const auto& record : records) {
        const Signal& signal = record.signal;
        upsert->reset();
        if (auto done = upsert->bind(1, name(signal.strategy))
                            .bind(2, std::string_view(record.underlying))
                            .bind(3, signal.minute)
                            .bind(4, std::int64_t{signal.horizon_min})
                            .bind(5, std::int64_t{signal.score})
                            .bind(6, std::int64_t{static_cast<std::int8_t>(signal.label)})
                            .bind(7, std::string_view(factors_json(signal)))
                            .bind(8, record.spot_raw)
                            .run();
            !done) {
            return std::unexpected(done.error());
        }
    }
    return transaction->commit();
}

std::expected<std::vector<StoredSignal>, DbError> SignalStore::range(std::string_view underlying,
                                                                     std::int64_t from,
                                                                     std::int64_t to) {
    auto statement = db_.prepare("SELECT " + std::string(kColumns) +
                                 " FROM signal WHERE underlying = ?1 AND minute >= ?2 AND minute < ?3"
                                 " ORDER BY minute, strategy");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, underlying).bind(2, from).bind(3, to);
    return collect(*statement);
}

std::expected<std::optional<std::int64_t>, DbError> SignalStore::latest_minute(
    std::string_view underlying) {
    auto statement = db_.prepare("SELECT MAX(minute) FROM signal WHERE underlying = ?1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, underlying);
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    return optional_int(*statement, 0);
}

std::expected<std::vector<StoredSignal>, DbError> SignalStore::pending(std::string_view underlying) {
    auto statement = db_.prepare("SELECT " + std::string(kColumns) +
                                 " FROM signal WHERE underlying = ?1 AND spot_after IS NULL"
                                 " ORDER BY minute, strategy");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, underlying);
    return collect(*statement);
}

std::expected<void, DbError> SignalStore::set_outcomes(std::span<const OutcomeUpdate> updates) {
    if (updates.empty()) {
        return {};
    }
    auto transaction = storage::Transaction::begin(db_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    auto update = db_.prepare(
        "UPDATE signal SET spot_after = ?4, return_bp = ?5, hit = ?6"
        " WHERE strategy = ?1 AND underlying = ?2 AND minute = ?3");
    if (!update) {
        return std::unexpected(update.error());
    }
    for (const auto& u : updates) {
        update->reset();
        update->bind(1, std::string_view(u.strategy))
            .bind(2, std::string_view(u.underlying))
            .bind(3, u.minute)
            .bind(4, u.outcome.spot_after_raw);
        if (u.outcome.return_bp) {
            update->bind(5, std::int64_t{*u.outcome.return_bp});
        } else {
            update->bind_null(5);
        }
        if (u.outcome.hit) {
            update->bind(6, std::int64_t{*u.outcome.hit ? 1 : 0});
        } else {
            update->bind_null(6);
        }
        if (auto done = update->run(); !done) {
            return std::unexpected(done.error());
        }
    }
    return transaction->commit();
}

std::expected<std::vector<TrackRecord>, DbError> SignalStore::record(std::string_view underlying,
                                                                     std::int64_t from,
                                                                     std::int64_t to) {
    // p: the same strategy's signal one horizon earlier, for the momentum baseline (did the
    // spot's move over the previous horizon point the right way?).
    auto statement = db_.prepare(R"sql(
SELECT s.strategy, MAX(s.horizon_min), COUNT(*),
       SUM(s.label = 1), SUM(s.label = -1), SUM(s.label = 0),
       SUM(s.hit = 1), SUM(s.hit = 0), SUM(s.label <> 0 AND s.hit IS NULL),
       SUM(s.hit IS NOT NULL AND s.return_bp > 0),
       SUM(s.hit IS NOT NULL AND p.spot > 0 AND s.spot <> p.spot
           AND (s.spot > p.spot) = (s.return_bp > 0)),
       SUM(s.hit IS NOT NULL AND p.spot > 0 AND s.spot <> p.spot)
FROM signal s
LEFT JOIN signal p ON p.strategy = s.strategy AND p.underlying = s.underlying
                  AND p.minute = s.minute - s.horizon_min * 60
WHERE s.underlying = ?1 AND s.minute >= ?2 AND s.minute < ?3 AND s.spot_after IS NOT NULL
      AND s.return_bp IS NOT NULL
GROUP BY s.strategy ORDER BY s.strategy)sql");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, underlying).bind(2, from).bind(3, to);
    std::vector<TrackRecord> records;
    while (true) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row == StepResult::Done) {
            return records;
        }
        const auto& s = *statement;
        const auto count = [&s](int column) { return static_cast<int>(s.column_int64(column)); };
        records.push_back(TrackRecord{.strategy = s.column_text(0),
                                      .horizon_min = count(1),
                                      .settled = count(2),
                                      .bullish = count(3),
                                      .bearish = count(4),
                                      .neutral = count(5),
                                      .hits = count(6),
                                      .misses = count(7),
                                      .flats = count(8),
                                      .up_moves = count(9),
                                      .momentum_hits = count(10),
                                      .momentum_calls = count(11)});
    }
}

}  // namespace mda::signals
