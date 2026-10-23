#include <array>
#include <string_view>

#include <mda/quotestore/last_quote_store.hpp>

namespace mda::quotestore {
namespace {

using storage::DbError;
using storage::StepResult;

constexpr std::array<std::string_view, 1> kMigrations = {R"sql(
CREATE TABLE last_messages (
    instrument_id INTEGER NOT NULL,
    type          INTEGER NOT NULL,
    payload       BLOB NOT NULL,
    PRIMARY KEY (instrument_id, type)
) WITHOUT ROWID;
)sql"};

}  // namespace

std::expected<LastQuoteStore, DbError> LastQuoteStore::prepare(storage::Database db) {
    if (auto migrated = db.migrate(kMigrations); !migrated) {
        return std::unexpected(migrated.error());
    }
    return LastQuoteStore(std::move(db));
}

std::expected<LastQuoteStore, DbError> LastQuoteStore::open(const std::filesystem::path& path) {
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

std::expected<LastQuoteStore, DbError> LastQuoteStore::open_in_memory() {
    auto db = storage::Database::open_in_memory();
    if (!db) {
        return std::unexpected(db.error());
    }
    return prepare(std::move(*db));
}

std::expected<std::size_t, DbError> LastQuoteStore::flush() {
    if (pending_.empty()) {
        return 0;
    }
    auto transaction = storage::Transaction::begin(db_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    auto upsert = db_.prepare(
        "INSERT INTO last_messages (instrument_id, type, payload) VALUES (?1, ?2, ?3) "
        "ON CONFLICT (instrument_id, type) DO UPDATE SET payload = excluded.payload");
    if (!upsert) {
        return std::unexpected(upsert.error());
    }
    for (const auto& [key, payload] : pending_) {
        upsert->bind(1, static_cast<std::int64_t>(key.first))
            .bind(2, static_cast<std::int64_t>(key.second))
            .bind(3, std::span<const std::byte>(payload));
        if (auto done = upsert->run(); !done) {
            return std::unexpected(done.error());
        }
        upsert->reset();
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    const std::size_t written = pending_.size();
    pending_.clear();
    return written;
}

std::expected<std::size_t, DbError> LastQuoteStore::load(
    const std::function<void(std::uint32_t, std::span<const std::byte>)>& on_message) {
    auto rows = db_.prepare("SELECT type, payload FROM last_messages");
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::size_t count = 0;
    while (true) {
        const auto row = rows->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row == StepResult::Done) {
            return count;
        }
        const auto payload = rows->column_blob(1);
        on_message(static_cast<std::uint32_t>(rows->column_int64(0)), payload);
        ++count;
    }
}

}  // namespace mda::quotestore
