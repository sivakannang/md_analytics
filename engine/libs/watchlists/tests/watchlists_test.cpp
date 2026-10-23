#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <mda/appdb/schema.hpp>
#include <mda/watchlists/watchlists.hpp>

namespace mda::watchlists {
namespace {

using Code = WatchlistError::Code;
using namespace std::chrono_literals;

class WatchlistsTest : public ::testing::Test {
protected:
    WatchlistsTest()
        : db_(storage::Database::open_in_memory().value())
        , lists_(db_, {.max_lists = 3, .max_items = 2}) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
        for (const char* name : {"asha", "bala"}) {
            EXPECT_TRUE(
                db_.exec(std::string("INSERT INTO users (username, password_hash, created_at, "
                                     "password_changed_at) VALUES ('") +
                         name + "', 'x', 0, 0)"));
        }
        instruments::InstrumentRegistry registry;
        for (const char* symbol : {"RELIANCE", "INFY", "TCS", "OLDCO"}) {
            const auto id = registry.assign({.exchange = instruments::Exchange::Nse,
                                             .market = instruments::Market::Cash,
                                             .kind = instruments::Kind::Equity,
                                             .underlying = symbol});
            auto* instrument = registry.get(id);
            instrument->trading_symbol = symbol;
            instrument->name = symbol;
            instrument->lot_size = core::Quantity(1);
            instrument->listed_today = std::string_view(symbol) != "OLDCO";
        }
        EXPECT_TRUE(catalog::sync(db_, registry, 0).has_value());
    }

    storage::Database db_;
    WatchlistService lists_;
    static constexpr std::int64_t kAsha = 1, kBala = 2;
    static constexpr std::int64_t kReliance = 1, kInfy = 2, kTcs = 3, kOldCo = 4;
};

TEST_F(WatchlistsTest, NewUserGetsADefaultList) {
    const auto lists = lists_.list(kAsha, 100).value();
    ASSERT_EQ(lists.size(), 1u);
    EXPECT_EQ(lists[0].name, kDefaultName);
    EXPECT_TRUE(lists[0].items.empty());
    EXPECT_EQ(lists_.list(kAsha, 200).value().size(), 1u);  // not created twice
}

TEST_F(WatchlistsTest, AddsItemsInOrderWithInstrumentDetails) {
    const auto list = lists_.create(kAsha, "  Banks  ", 100).value();
    EXPECT_EQ(list.name, "Banks");
    ASSERT_TRUE(lists_.add_item(kAsha, list.id, kInfy, 110).has_value());
    const auto updated = lists_.add_item(kAsha, list.id, kReliance, 120).value();
    ASSERT_EQ(updated.items.size(), 2u);
    EXPECT_EQ(updated.items[0].instrument.trading_symbol, "INFY");
    EXPECT_EQ(updated.items[1].instrument.trading_symbol, "RELIANCE");
    EXPECT_EQ(updated.items[1].added_at, 120);

    const auto removed = lists_.remove_item(kAsha, list.id, kInfy).value();
    ASSERT_EQ(removed.items.size(), 1u);
    EXPECT_EQ(lists_.remove_item(kAsha, list.id, kInfy).error().code, Code::NotFound);
}

TEST_F(WatchlistsTest, EnforcesRules) {
    const auto list = lists_.create(kAsha, "Mine", 100).value();
    EXPECT_EQ(lists_.create(kAsha, "Mine", 100).error().code, Code::Conflict);
    EXPECT_EQ(lists_.create(kAsha, "", 100).error().code, Code::Input);
    EXPECT_EQ(lists_.create(kAsha, std::string(41, 'x'), 100).error().code, Code::Input);
    EXPECT_TRUE(lists_.create(kBala, "Mine", 100).has_value());  // names are per user

    EXPECT_EQ(lists_.add_item(kAsha, list.id, kOldCo, 100).error().code,
              Code::Input);                                                          // not listed
    EXPECT_EQ(lists_.add_item(kAsha, list.id, 999, 100).error().code, Code::Input);  // unknown
    ASSERT_TRUE(lists_.add_item(kAsha, list.id, kInfy, 100).has_value());
    EXPECT_EQ(lists_.add_item(kAsha, list.id, kInfy, 100).error().code, Code::Conflict);
    ASSERT_TRUE(lists_.add_item(kAsha, list.id, kTcs, 100).has_value());
    EXPECT_EQ(lists_.add_item(kAsha, list.id, kReliance, 100).error().code, Code::Limit);  // max 2

    ASSERT_TRUE(lists_.create(kAsha, "Second", 100).has_value());
    ASSERT_TRUE(lists_.create(kAsha, "Third", 100).has_value());
    EXPECT_EQ(lists_.create(kAsha, "Fourth", 100).error().code, Code::Limit);  // max 3
}

TEST_F(WatchlistsTest, OtherUsersListsLookMissing) {
    const auto list = lists_.create(kAsha, "Private", 100).value();
    EXPECT_EQ(lists_.add_item(kBala, list.id, kInfy, 100).error().code, Code::NotFound);
    EXPECT_EQ(lists_.rename(kBala, list.id, "Stolen").error().code, Code::NotFound);
    EXPECT_EQ(lists_.remove(kBala, list.id).error().code, Code::NotFound);
    EXPECT_TRUE(lists_.remove(kAsha, list.id).has_value());
    EXPECT_EQ(lists_.remove(kAsha, list.id).error().code, Code::NotFound);
}

TEST_F(WatchlistsTest, RenameAndFeedUnion) {
    const auto a = lists_.create(kAsha, "A", 100).value();
    const auto b = lists_.create(kBala, "B", 100).value();
    EXPECT_EQ(lists_.rename(kAsha, a.id, "Alpha").value().name, "Alpha");
    ASSERT_TRUE(lists_.add_item(kAsha, a.id, kTcs, 100).has_value());
    ASSERT_TRUE(lists_.add_item(kBala, b.id, kTcs, 100).has_value());
    ASSERT_TRUE(lists_.add_item(kBala, b.id, kInfy, 100).has_value());
    EXPECT_EQ(lists_.all_instrument_ids().value(), (std::vector<std::int64_t>{kInfy, kTcs}));
}

TEST_F(WatchlistsTest, DeletingAUserDeletesTheirLists) {
    const auto list = lists_.create(kAsha, "Gone", 100).value();
    ASSERT_TRUE(lists_.add_item(kAsha, list.id, kInfy, 100).has_value());
    ASSERT_TRUE(db_.exec("DELETE FROM users WHERE id = 1"));
    auto count = db_.prepare("SELECT COUNT(*) FROM watchlist_items");
    ASSERT_EQ(count->step().value(), storage::StepResult::Row);
    EXPECT_EQ(count->column_int64(0), 0);
}

std::vector<std::string> symbols(const Watchlist& list) {
    std::vector<std::string> out;
    for (const auto& item : list.items) {
        out.push_back(item.instrument.trading_symbol + "@" + std::to_string(item.group_id));
    }
    return out;
}

TEST_F(WatchlistsTest, GroupsHoldItemsAndDeletingOneKeepsItsItems) {
    const auto list = lists_.create(kAsha, "Mine", 100).value();
    auto updated = lists_.create_group(kAsha, list.id, "IT").value();
    ASSERT_EQ(updated.groups.size(), 1u);
    const std::int64_t it = updated.groups[0].id;
    EXPECT_EQ(lists_.create_group(kAsha, list.id, "IT").error().code, Code::Conflict);
    EXPECT_EQ(lists_.create_group(kAsha, list.id, "").error().code, Code::Input);

    ASSERT_TRUE(lists_.add_item(kAsha, list.id, kReliance, 100).has_value());  // ungrouped
    updated = lists_.add_item(kAsha, list.id, kInfy, 100, it).value();
    EXPECT_EQ(symbols(updated),
              (std::vector<std::string>{"RELIANCE@0", "INFY@" + std::to_string(it)}));
    EXPECT_EQ(lists_.add_item(kAsha, list.id, kTcs, 100, 9999).error().code, Code::NotFound);

    EXPECT_EQ(lists_.rename_group(kAsha, list.id, it, "Tech").value().groups[0].name, "Tech");
    EXPECT_EQ(lists_.rename_group(kBala, list.id, it, "Mine").error().code, Code::NotFound);

    updated = lists_.remove_group(kAsha, list.id, it).value();
    EXPECT_TRUE(updated.groups.empty());
    EXPECT_EQ(symbols(updated), (std::vector<std::string>{"RELIANCE@0", "INFY@0"}));  // kept
    EXPECT_EQ(lists_.remove_group(kAsha, list.id, it).error().code, Code::NotFound);
}

TEST_F(WatchlistsTest, SetOrderRearrangesGroupsAndItems) {
    const auto list = lists_.create(kAsha, "Mine", 100).value();
    const std::int64_t a = lists_.create_group(kAsha, list.id, "A").value().groups[0].id;
    const std::int64_t b = lists_.create_group(kAsha, list.id, "B").value().groups[1].id;
    ASSERT_TRUE(lists_.add_item(kAsha, list.id, kInfy, 100, a).has_value());
    ASSERT_TRUE(lists_.add_item(kAsha, list.id, kTcs, 100, a).has_value());

    // B first; TCS moves to B ahead of INFY.
    const auto updated =
        lists_.set_order(kAsha, list.id, {.groups = {b, a}, .items = {{kTcs, b}, {kInfy, a}}})
            .value();
    ASSERT_EQ(updated.groups.size(), 2u);
    EXPECT_EQ(updated.groups[0].name, "B");
    EXPECT_EQ(symbols(updated),
              (std::vector<std::string>{"TCS@" + std::to_string(b), "INFY@" + std::to_string(a)}));

    // The order must match what is there.
    EXPECT_EQ(
        lists_.set_order(kAsha, list.id, {.groups = {b, a}, .items = {{kTcs, b}}}).error().code,
        Code::Conflict);  // an item missing
    EXPECT_EQ(lists_.set_order(kAsha, list.id, {.groups = {a}, .items = {{kTcs, b}, {kInfy, a}}})
                  .error()
                  .code,
              Code::Conflict);  // a group missing
    EXPECT_EQ(
        lists_.set_order(kAsha, list.id, {.groups = {b, a}, .items = {{kTcs, 777}, {kInfy, a}}})
            .error()
            .code,
        Code::Input);  // unknown group
    EXPECT_EQ(lists_.set_order(kBala, list.id, {.groups = {b, a}, .items = {{kTcs, b}, {kInfy, a}}})
                  .error()
                  .code,
              Code::NotFound);  // not hers
}

}  // namespace
}  // namespace mda::watchlists
