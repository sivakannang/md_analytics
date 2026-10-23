#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <mda/appdb/schema.hpp>
#include <mda/catalog/catalog.hpp>

namespace mda::catalog {
namespace {

using instruments::ContractKey;
using instruments::Exchange;
using instruments::InstrumentRegistry;
using instruments::Kind;
using instruments::Market;
using instruments::OptionType;
using namespace std::chrono_literals;

/// Adds a listed instrument with daily attributes.
core::InstrumentId add(InstrumentRegistry& registry, ContractKey key, std::string symbol,
                       std::string name, std::int64_t lot = 1) {
    const auto id = registry.assign(key);
    auto* instrument = registry.get(id);
    instrument->trading_symbol = std::move(symbol);
    instrument->name = std::move(name);
    instrument->lot_size = core::Quantity(lot);
    instrument->tick_size = core::Price::from_raw(500);
    instrument->listed_today = true;
    return id;
}

ContractKey cash(Kind kind, std::string underlying, Exchange exchange = Exchange::Nse) {
    return {.exchange = exchange,
            .market = Market::Cash,
            .kind = kind,
            .underlying = std::move(underlying)};
}

ContractKey future(std::string underlying, std::chrono::year_month_day expiry) {
    return {.exchange = Exchange::Nse,
            .market = Market::Fo,
            .kind = Kind::Future,
            .underlying = std::move(underlying),
            .expiry = expiry};
}

ContractKey option(std::chrono::year_month_day expiry, std::int64_t strike, OptionType type) {
    return {.exchange = Exchange::Nse,
            .market = Market::Fo,
            .kind = Kind::Option,
            .underlying = "NIFTY",
            .expiry = expiry,
            .strike = core::Price::from_raw(strike * core::Price::kScale),
            .option_type = type};
}

class CatalogTest : public ::testing::Test {
protected:
    CatalogTest() : db_(storage::Database::open_in_memory().value()) {
        EXPECT_TRUE(db_.migrate(appdb::migrations()).has_value());
        nifty_ = add(registry_, cash(Kind::Index, "NIFTY 50"), "NIFTY 50", "NIFTY 50");
        reliance_ =
            add(registry_, cash(Kind::Equity, "RELIANCE"), "RELIANCE", "RELIANCE INDUSTRIES");
        add(registry_, cash(Kind::Equity, "RELIANCE", Exchange::Bse), "RELIANCE",
            "RELIANCE INDUSTRIES");
        add(registry_, future("RELIANCE", 2026y / 10 / 27), "RELIANCE26OCTFUT", "RELIANCE", 500);
        add(registry_, future("NIFTY", 2026y / 11 / 24), "NIFTY26NOVFUT", "NIFTY", 75);
        add(registry_, future("NIFTY", 2026y / 10 / 27), "NIFTY26OCTFUT", "NIFTY", 75);
        call_ = add(registry_, option(2026y / 10 / 27, 25'000, OptionType::Call),
                    "NIFTY26O2725000CE", "NIFTY", 75);
        add(registry_, option(2026y / 10 / 27, 25'000, OptionType::Put), "NIFTY26O2725000PE",
            "NIFTY", 75);
        add(registry_, option(2026y / 11 / 3, 25'000, OptionType::Call), "NIFTY26N0325000CE",
            "NIFTY", 75);
        add(registry_, cash(Kind::Equity, "HDFCBANK"), "HDFCBANK", "HDFC BANK");
        add(registry_,
            {.exchange = Exchange::Nse,
             .market = Market::Fo,
             .kind = Kind::Option,
             .underlying = "FINNIFTY",
             .expiry = 2026y / 10 / 27,
             .strike = core::Price::from_raw(25'000 * core::Price::kScale),
             .option_type = OptionType::Call},
            "FINNIFTY26OCT25000CE", "FINNIFTY", 60);
        EXPECT_TRUE(sync(db_, registry_, 1'761'100'000).has_value());
    }

    std::vector<std::string> symbols(std::string_view query, std::size_t limit = 20) {
        std::vector<std::string> out;
        const auto found = search(db_, query, limit);
        for (const auto& info : found.value()) {
            out.push_back(info.exchange + ":" + info.trading_symbol);
        }
        return out;
    }

    storage::Database db_;
    InstrumentRegistry registry_;
    core::InstrumentId nifty_, reliance_, call_;
};

TEST_F(CatalogTest, SyncWritesListedInstruments) {
    const auto info = get(db_, call_.value()).value();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, "OPT");
    EXPECT_EQ(info->expiry, "2026-10-27");
    EXPECT_EQ(info->strike, core::Price::from_raw(250'000'000));
    EXPECT_EQ(info->option_type, "CE");
    EXPECT_EQ(info->lot_size, 75);
    EXPECT_TRUE(info->listed);
}

TEST_F(CatalogTest, EveryWordMustMatch) {
    EXPECT_EQ(
        symbols("nifty 25000 ce"),
        (std::vector<std::string>{"NSE:NIFTY26O2725000CE", "NSE:NIFTY26N0325000CE",
                                  "NSE:FINNIFTY26OCT25000CE"}));  // exact underlying, then expiry
    EXPECT_EQ(symbols("nifty 25000 pe"), std::vector<std::string>{"NSE:NIFTY26O2725000PE"});
    EXPECT_EQ(symbols("nifty nov fut"), std::vector<std::string>{"NSE:NIFTY26NOVFUT"});
    EXPECT_TRUE(symbols("nifty 99999").empty());
}

TEST_F(CatalogTest, RanksCashBeforeFuturesBeforeOptionsAndNseFirst) {
    const auto results = symbols("reliance");
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], "NSE:RELIANCE");
    EXPECT_EQ(results[1], "BSE:RELIANCE");
    EXPECT_EQ(results[2], "NSE:RELIANCE26OCTFUT");
    EXPECT_EQ(symbols("nifty")[0], "NSE:NIFTY 50");  // the index before its derivatives
}

TEST_F(CatalogTest, MatchesCompanyNamesCaseInsensitively) {
    EXPECT_EQ(symbols("Hdfc BANK"), std::vector<std::string>{"NSE:HDFCBANK"});
    EXPECT_EQ(symbols("industries")[0], "NSE:RELIANCE");
}

TEST_F(CatalogTest, LimitsAndOddInputAreSafe) {
    EXPECT_EQ(symbols("nifty", 2).size(), 2u);
    EXPECT_TRUE(symbols("").empty());
    EXPECT_TRUE(symbols("   ").empty());
    EXPECT_TRUE(symbols("%").empty());  // LIKE wildcards are escaped, not interpreted
    EXPECT_TRUE(symbols("'; DROP TABLE instruments; --").empty());
    EXPECT_FALSE(symbols("reliance").empty());  // table intact
}

TEST_F(CatalogTest, InstrumentsNoLongerListedDropOutOfSearchButRemain) {
    registry_.begin_day();
    add(registry_, cash(Kind::Index, "NIFTY 50"), "NIFTY 50",
        "NIFTY 50");  // only this is listed today
    ASSERT_TRUE(sync(db_, registry_, 1'761'200'000).has_value());
    EXPECT_TRUE(symbols("reliance").empty());
    const auto old = get(db_, reliance_.value()).value();
    ASSERT_TRUE(old.has_value());  // still there for watchlists and history
    EXPECT_FALSE(old->listed);
}

}  // namespace
}  // namespace mda::catalog
