#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include <mda/storage/database.hpp>

namespace mda::storage {
namespace {

constexpr std::array<std::string_view, 2> kMigrations = {
    "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, photo BLOB);",
    "ALTER TABLE people ADD COLUMN age INTEGER;",
};

Database memory_db() {
    auto db = Database::open_in_memory();
    EXPECT_TRUE(db.has_value());
    return std::move(*db);
}

TEST(DatabaseTest, MigratesInOrderAndIsIdempotent) {
    Database db = memory_db();
    EXPECT_EQ(db.schema_version().value(), 0);
    ASSERT_TRUE(db.migrate(kMigrations).has_value());
    EXPECT_EQ(db.schema_version().value(), 2);
    ASSERT_TRUE(db.migrate(kMigrations).has_value());  // nothing left to apply
    EXPECT_EQ(db.schema_version().value(), 2);
}

TEST(DatabaseTest, RefusesADatabaseNewerThanTheBuild) {
    Database db = memory_db();
    ASSERT_TRUE(db.migrate(kMigrations).has_value());
    const auto older = std::span(kMigrations).first(1);
    EXPECT_FALSE(db.migrate(older).has_value());
}

TEST(DatabaseTest, BindsAndReadsBackAllTypes) {
    Database db = memory_db();
    ASSERT_TRUE(db.migrate(kMigrations).has_value());

    const std::vector<std::byte> photo = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}};
    auto insert = db.prepare("INSERT INTO people (name, photo, age) VALUES (?1, ?2, ?3)");
    ASSERT_TRUE(insert.has_value());
    ASSERT_TRUE(
        insert->bind(1, std::string_view("asha")).bind(2, photo).bind(3, std::int64_t{41}).run());
    EXPECT_EQ(db.last_insert_id(), 1);
    EXPECT_EQ(db.changes(), 1);

    auto select = db.prepare("SELECT name, photo, age FROM people WHERE id = ?1");
    ASSERT_TRUE(select.has_value());
    select->bind(1, std::int64_t{1});
    ASSERT_EQ(select->step().value(), StepResult::Row);
    EXPECT_EQ(select->column_text(0), "asha");
    EXPECT_EQ(select->column_blob(1), photo);
    EXPECT_EQ(select->column_int64(2), 41);
    EXPECT_EQ(select->step().value(), StepResult::Done);
}

TEST(DatabaseTest, BoundValuesAreNeverInterpretedAsSql) {
    Database db = memory_db();
    ASSERT_TRUE(db.migrate(kMigrations).has_value());
    const std::string_view hostile = "x'); DROP TABLE people; --";
    auto insert = db.prepare("INSERT INTO people (name) VALUES (?1)");
    ASSERT_TRUE(insert->bind(1, hostile).run());

    auto select = db.prepare("SELECT name FROM people");
    ASSERT_EQ(select->step().value(), StepResult::Row);
    EXPECT_EQ(select->column_text(0), hostile);  // stored verbatim, table intact
}

TEST(DatabaseTest, ReportsConstraintViolations) {
    Database db = memory_db();
    ASSERT_TRUE(db.migrate(kMigrations).has_value());
    auto insert = db.prepare("INSERT INTO people (name) VALUES (?1)");
    ASSERT_TRUE(insert->bind(1, std::string_view("dup")).run());
    insert->reset();
    const auto second = insert->bind(1, std::string_view("dup")).run();
    ASSERT_FALSE(second.has_value());
    EXPECT_TRUE(second.error().is_constraint());
}

TEST(DatabaseTest, TransactionRollsBackUnlessCommitted) {
    Database db = memory_db();
    ASSERT_TRUE(db.migrate(kMigrations).has_value());
    {
        auto transaction = Transaction::begin(db);
        ASSERT_TRUE(transaction.has_value());
        ASSERT_TRUE(db.exec("INSERT INTO people (name) VALUES ('temp')"));
    }  // destroyed without commit
    auto count = db.prepare("SELECT COUNT(*) FROM people");
    ASSERT_EQ(count->step().value(), StepResult::Row);
    EXPECT_EQ(count->column_int64(0), 0);

    {
        auto transaction = Transaction::begin(db);
        ASSERT_TRUE(db.exec("INSERT INTO people (name) VALUES ('kept')"));
        ASSERT_TRUE(transaction->commit());
    }
    count->reset();
    ASSERT_EQ(count->step().value(), StepResult::Row);
    EXPECT_EQ(count->column_int64(0), 1);
}

TEST(DatabaseTest, FileDatabasePersistsAcrossOpens) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("mda_db_" + std::to_string(::getpid())) / "app.db";
    {
        auto db = Database::open(path);
        ASSERT_TRUE(db.has_value()) << db.error().message;
        ASSERT_TRUE(db->migrate(kMigrations).has_value());
        ASSERT_TRUE(db->exec("INSERT INTO people (name) VALUES ('persisted')"));
    }
    auto db = Database::open(path);
    ASSERT_TRUE(db.has_value());
    EXPECT_EQ(db->schema_version().value(), 2);
    auto select = db->prepare("SELECT name FROM people");
    ASSERT_EQ(select->step().value(), StepResult::Row);
    EXPECT_EQ(select->column_text(0), "persisted");
    std::filesystem::remove_all(path.parent_path());
}

}  // namespace
}  // namespace mda::storage
