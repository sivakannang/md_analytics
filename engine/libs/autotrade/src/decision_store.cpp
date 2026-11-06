#include <mda/autotrade/decision_store.hpp>

#include <array>
#include <system_error>
#include <utility>

namespace mda::autotrade {
namespace {

constexpr std::array<std::string_view, 1> kMigrations = {R"sql(
CREATE TABLE decisions (
    id              INTEGER PRIMARY KEY,
    run             TEXT NOT NULL,
    at              INTEGER NOT NULL,    -- Unix ms
    state_before    TEXT NOT NULL,
    action          TEXT NOT NULL,       -- enter long, place stop, exit, blocked, …
    reason          TEXT NOT NULL,
    client_order_id INTEGER NOT NULL,
    price_raw       INTEGER NOT NULL
);
CREATE INDEX decisions_by_run ON decisions (run, at);

CREATE TABLE runs_day (
    run        TEXT NOT NULL,
    day        INTEGER NOT NULL,         -- IST day, days since 1970-01-01
    entries    INTEGER NOT NULL,
    realised   INTEGER NOT NULL,         -- paise
    charges    INTEGER NOT NULL,
    last_exit  INTEGER NOT NULL,         -- Unix ms
    counter    INTEGER NOT NULL,         -- client order ids used
    last_entry INTEGER NOT NULL,
    PRIMARY KEY (run, day)
) WITHOUT ROWID;
)sql"};

State state_of(std::string_view text) {
    for (const auto s : {State::Flat, State::Entering, State::Long, State::Short, State::Exiting}) {
        if (name(s) == text) {
            return s;
        }
    }
    return State::Flat;
}

}  // namespace

std::expected<DecisionStore, DbError> DecisionStore::prepare(storage::Database db) {
    if (auto migrated = db.migrate(kMigrations); !migrated) {
        return std::unexpected(migrated.error());
    }
    return DecisionStore(std::move(db));
}

std::expected<DecisionStore, DbError> DecisionStore::open(const std::filesystem::path& path) {
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

std::expected<DecisionStore, DbError> DecisionStore::open_in_memory() {
    auto db = storage::Database::open_in_memory();
    if (!db) {
        return std::unexpected(db.error());
    }
    return prepare(std::move(*db));
}

std::expected<void, DbError> DecisionStore::save(std::string_view run, std::span<const Decision> decisions,
                                                 const RunDay& day) {
    auto transaction = storage::Transaction::begin(db_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    auto insert = db_.prepare(
        "INSERT INTO decisions (run, at, state_before, action, reason, client_order_id, price_raw) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (!insert) {
        return std::unexpected(insert.error());
    }
    for (const auto& d : decisions) {
        insert->reset();
        if (auto done = insert->bind(1, run)
                            .bind(2, d.at)
                            .bind(3, name(d.before))
                            .bind(4, std::string_view(d.action))
                            .bind(5, std::string_view(d.reason))
                            .bind(6, static_cast<std::int64_t>(d.client_order_id))
                            .bind(7, d.price.raw())
                            .run();
            !done) {
            return std::unexpected(done.error());
        }
    }
    auto upsert = db_.prepare(
        "INSERT OR REPLACE INTO runs_day (run, day, entries, realised, charges, last_exit, counter, last_entry) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    if (!upsert) {
        return std::unexpected(upsert.error());
    }
    if (auto done = upsert->bind(1, run)
                        .bind(2, std::int64_t{day.day})
                        .bind(3, std::int64_t{day.entries})
                        .bind(4, day.realised)
                        .bind(5, day.charges)
                        .bind(6, day.last_exit)
                        .bind(7, std::int64_t{day.counter})
                        .bind(8, day.last_entry)
                        .run();
        !done) {
        return std::unexpected(done.error());
    }
    return transaction->commit();
}

std::expected<std::optional<RunDay>, DbError> DecisionStore::day(std::string_view run, std::int32_t day) {
    auto statement = db_.prepare(
        "SELECT entries, realised, charges, last_exit, counter, last_entry FROM runs_day WHERE run = ?1 AND day = ?2");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, run).bind(2, std::int64_t{day});
    const auto step = statement->step();
    if (!step) {
        return std::unexpected(step.error());
    }
    if (*step == storage::StepResult::Done) {
        return std::nullopt;
    }
    const auto& s = *statement;
    return RunDay{.day = day,
                  .entries = static_cast<int>(s.column_int64(0)),
                  .realised = s.column_int64(1),
                  .charges = s.column_int64(2),
                  .last_exit = s.column_int64(3),
                  .counter = static_cast<std::uint32_t>(s.column_int64(4)),
                  .last_entry = s.column_int64(5)};
}

std::expected<std::vector<Decision>, DbError> DecisionStore::decisions(std::string_view run, Millis from, Millis to) {
    auto statement = db_.prepare(
        "SELECT at, state_before, action, reason, client_order_id, price_raw FROM decisions "
        "WHERE run = ?1 AND at >= ?2 AND at < ?3 ORDER BY id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, run).bind(2, from).bind(3, to);
    std::vector<Decision> out;
    while (true) {
        const auto step = statement->step();
        if (!step) {
            return std::unexpected(step.error());
        }
        if (*step == storage::StepResult::Done) {
            return out;
        }
        const auto& s = *statement;
        out.push_back({.at = s.column_int64(0),
                       .before = state_of(s.column_text(1)),
                       .action = s.column_text(2),
                       .reason = s.column_text(3),
                       .client_order_id = static_cast<std::uint64_t>(s.column_int64(4)),
                       .price = core::Price::from_raw(s.column_int64(5))});
    }
}

}  // namespace mda::autotrade
