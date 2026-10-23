#include <utility>

#include <sqlite3.h>

#include <mda/storage/database.hpp>

namespace mda::storage {
namespace {

DbError error_from(sqlite3* db, int code) {
    return DbError{.code = code,
                   .message = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(code)};
}

}  // namespace

bool DbError::is_constraint() const noexcept {
    return (code & 0xFF) == SQLITE_CONSTRAINT;
}

// --------------------------------------------------------------------------- Statement

void Statement::Finalizer::operator()(sqlite3_stmt* statement) const noexcept {
    sqlite3_finalize(statement);
}

void Statement::remember(int result) {
    if (result != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = result;
    }
}

Statement& Statement::bind(int index, std::int64_t value) {
    remember(sqlite3_bind_int64(statement_.get(), index, value));
    return *this;
}

Statement& Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT: SQLite copies the bytes, so the caller's buffer may go away.
    remember(sqlite3_bind_text(statement_.get(), index, value.data(),
                               static_cast<int>(value.size()), SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::bind(int index, std::span<const std::byte> value) {
    remember(sqlite3_bind_blob(statement_.get(), index, value.data(),
                               static_cast<int>(value.size()), SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::bind_null(int index) {
    remember(sqlite3_bind_null(statement_.get(), index));
    return *this;
}

std::expected<StepResult, DbError> Statement::step() {
    if (bind_error_ != 0) {
        return std::unexpected(error_from(db_, bind_error_));
    }
    const int result = sqlite3_step(statement_.get());
    if (result == SQLITE_ROW) {
        return StepResult::Row;
    }
    if (result == SQLITE_DONE) {
        return StepResult::Done;
    }
    return std::unexpected(error_from(db_, sqlite3_extended_errcode(db_)));
}

std::expected<void, DbError> Statement::run() {
    while (true) {
        const auto result = step();
        if (!result) {
            return std::unexpected(result.error());
        }
        if (*result == StepResult::Done) {
            return {};
        }
    }
}

void Statement::reset() {
    sqlite3_reset(statement_.get());
    sqlite3_clear_bindings(statement_.get());
    bind_error_ = 0;
}

std::int64_t Statement::column_int64(int column) const {
    return sqlite3_column_int64(statement_.get(), column);
}

std::string Statement::column_text(int column) const {
    const auto* text = sqlite3_column_text(statement_.get(), column);
    const int size = sqlite3_column_bytes(statement_.get(), column);
    return text == nullptr
               ? std::string{}
               : std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
}

std::vector<std::byte> Statement::column_blob(int column) const {
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(statement_.get(), column));
    const int size = sqlite3_column_bytes(statement_.get(), column);
    return data == nullptr ? std::vector<std::byte>{} : std::vector<std::byte>(data, data + size);
}

bool Statement::column_is_null(int column) const {
    return sqlite3_column_type(statement_.get(), column) == SQLITE_NULL;
}

// --------------------------------------------------------------------------- Database

void Database::Closer::operator()(sqlite3* db) const noexcept {
    sqlite3_close_v2(db);
}

std::expected<Database, DbError> Database::open_with(const std::string& name) {
    sqlite3* raw = nullptr;
    const int result =
        sqlite3_open_v2(name.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    Database db(raw);  // owns the handle even on failure
    if (result != SQLITE_OK) {
        return std::unexpected(error_from(raw, result));
    }
    sqlite3_extended_result_codes(raw, 1);
    for (const char* pragma :
         {"PRAGMA journal_mode = WAL", "PRAGMA foreign_keys = ON", "PRAGMA busy_timeout = 5000"}) {
        if (auto applied = db.exec(pragma); !applied) {
            return std::unexpected(applied.error());
        }
    }
    return db;
}

std::expected<Database, DbError> Database::open(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
    return open_with(path.string());
}

std::expected<Database, DbError> Database::open_in_memory() {
    return open_with(":memory:");
}

std::expected<void, DbError> Database::exec(std::string_view sql) {
    const std::string text(sql);
    char* message = nullptr;
    const int result = sqlite3_exec(db_.get(), text.c_str(), nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        DbError error{.code = sqlite3_extended_errcode(db_.get()),
                      .message = message != nullptr ? message : sqlite3_errstr(result)};
        sqlite3_free(message);
        return std::unexpected(std::move(error));
    }
    return {};
}

std::expected<Statement, DbError> Database::prepare(std::string_view sql) {
    sqlite3_stmt* statement = nullptr;
    const int result = sqlite3_prepare_v2(db_.get(), sql.data(), static_cast<int>(sql.size()),
                                          &statement, nullptr);
    if (result != SQLITE_OK) {
        return std::unexpected(error_from(db_.get(), result));
    }
    return Statement(db_.get(), statement);
}

std::int64_t Database::last_insert_id() const noexcept {
    return sqlite3_last_insert_rowid(db_.get());
}

int Database::changes() const noexcept {
    return sqlite3_changes(db_.get());
}

std::expected<int, DbError> Database::schema_version() {
    auto statement = prepare("PRAGMA user_version");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    if (auto row = statement->step(); !row) {
        return std::unexpected(row.error());
    }
    return static_cast<int>(statement->column_int64(0));
}

std::expected<void, DbError> Database::migrate(std::span<const std::string_view> migrations) {
    const auto current = schema_version();
    if (!current) {
        return std::unexpected(current.error());
    }
    if (static_cast<std::size_t>(*current) > migrations.size()) {
        return std::unexpected(DbError{
            .code = SQLITE_ERROR,
            .message = "database schema version " + std::to_string(*current) +
                       " is newer than this build (" + std::to_string(migrations.size()) + ")"});
    }
    for (std::size_t i = static_cast<std::size_t>(*current); i < migrations.size(); ++i) {
        auto transaction = Transaction::begin(*this);
        if (!transaction) {
            return std::unexpected(transaction.error());
        }
        if (auto applied = exec(migrations[i]); !applied) {
            return std::unexpected(applied.error());
        }
        // PRAGMA can't take bound parameters; i + 1 is our own integer, not input.
        if (auto bumped = exec("PRAGMA user_version = " + std::to_string(i + 1)); !bumped) {
            return std::unexpected(bumped.error());
        }
        if (auto committed = transaction->commit(); !committed) {
            return std::unexpected(committed.error());
        }
    }
    return {};
}

// --------------------------------------------------------------------------- Transaction

std::expected<Transaction, DbError> Transaction::begin(Database& db) {
    if (auto begun = db.exec("BEGIN IMMEDIATE"); !begun) {
        return std::unexpected(begun.error());
    }
    return Transaction(db);
}

Transaction::Transaction(Transaction&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Transaction::~Transaction() {
    if (db_ != nullptr) {
        (void)db_->exec("ROLLBACK");
    }
}

std::expected<void, DbError> Transaction::commit() {
    auto committed = db_->exec("COMMIT");
    if (committed) {
        db_ = nullptr;
    }
    return committed;
}

}  // namespace mda::storage
