#pragma once

// Thin RAII wrapper over SQLite (ADR-0012).
//
// Values only ever reach SQL through bound parameters of prepared statements — there is
// deliberately no API that builds SQL from strings, so SQL injection is impossible by
// construction. Errors are values (std::expected), never exceptions.
//
// A Database is not thread-safe: use it from one thread (the gateway confines it to its
// auth worker thread).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace mda::storage {

struct DbError {
    int code = 0;  ///< SQLite extended result code
    std::string message;

    /// UNIQUE / NOT NULL / FOREIGN KEY / CHECK constraint violated.
    [[nodiscard]] bool is_constraint() const noexcept;
};

enum class StepResult {
    Row,   ///< a result row is available
    Done,  ///< statement finished
};

class Statement {
public:
    Statement(Statement&&) noexcept = default;
    Statement& operator=(Statement&&) noexcept = default;

    // Parameters are 1-based, as in SQL ("?1", "?2", …). Bind errors are reported by step().
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, std::span<const std::byte> value);
    Statement& bind_null(int index);

    [[nodiscard]] std::expected<StepResult, DbError> step();
    /// Runs to completion, ignoring rows (INSERT/UPDATE/DELETE).
    [[nodiscard]] std::expected<void, DbError> run();
    /// Clears bindings and rewinds, so the statement can be executed again.
    void reset();

    [[nodiscard]] std::int64_t column_int64(int column) const;
    [[nodiscard]] std::string column_text(int column) const;
    [[nodiscard]] std::vector<std::byte> column_blob(int column) const;
    [[nodiscard]] bool column_is_null(int column) const;

private:
    friend class Database;
    struct Finalizer {
        void operator()(sqlite3_stmt* statement) const noexcept;
    };
    Statement(sqlite3* db, sqlite3_stmt* statement) : db_(db), statement_(statement) {}
    void remember(int result);

    sqlite3* db_;
    std::unique_ptr<sqlite3_stmt, Finalizer> statement_;
    int bind_error_ = 0;
};

class Database {
public:
    /// Opens (creating if needed) a database file and applies the standard pragmas:
    /// WAL journal, foreign keys on, 5 s busy timeout.
    [[nodiscard]] static std::expected<Database, DbError> open(const std::filesystem::path& path);
    /// A private in-memory database (tests).
    [[nodiscard]] static std::expected<Database, DbError> open_in_memory();

    /// Executes SQL without parameters (schema, pragmas). Never pass user input here.
    [[nodiscard]] std::expected<void, DbError> exec(std::string_view sql);
    [[nodiscard]] std::expected<Statement, DbError> prepare(std::string_view sql);

    [[nodiscard]] std::int64_t last_insert_id() const noexcept;
    /// Rows changed by the last INSERT/UPDATE/DELETE.
    [[nodiscard]] int changes() const noexcept;

    /// Schema version (PRAGMA user_version).
    [[nodiscard]] std::expected<int, DbError> schema_version();
    /// Applies migrations[schema_version() ..] in order, each in its own transaction,
    /// bumping the schema version. Fails if the database is newer than `migrations`.
    [[nodiscard]] std::expected<void, DbError> migrate(
        std::span<const std::string_view> migrations);

private:
    struct Closer {
        void operator()(sqlite3* db) const noexcept;
    };
    explicit Database(sqlite3* db) : db_(db) {}
    [[nodiscard]] static std::expected<Database, DbError> open_with(const std::string& name);

    std::unique_ptr<sqlite3, Closer> db_;
};

/// RAII transaction: BEGIN IMMEDIATE on construction (takes the write lock up front),
/// ROLLBACK on destruction unless commit() succeeded.
class Transaction {
public:
    [[nodiscard]] static std::expected<Transaction, DbError> begin(Database& db);

    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    ~Transaction();

    [[nodiscard]] std::expected<void, DbError> commit();

private:
    explicit Transaction(Database& db) : db_(&db) {}
    Database* db_;
};

}  // namespace mda::storage
