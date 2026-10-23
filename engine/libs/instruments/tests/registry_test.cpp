#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include <mda/instruments/registry.hpp>

namespace mda::instruments {
namespace {

using core::InstrumentId;
using core::Price;
using namespace std::chrono_literals;

ContractKey nifty_call(std::chrono::year_month_day expiry, std::int64_t strike) {
    return {.exchange = Exchange::Nse,
            .market = Market::Fo,
            .kind = Kind::Option,
            .underlying = "NIFTY",
            .expiry = expiry,
            .strike = Price::from_raw(strike * Price::kScale),
            .option_type = OptionType::Call};
}

const ContractKey kNiftyIndex{.exchange = Exchange::Nse,
                              .market = Market::Cash,
                              .kind = Kind::Index,
                              .underlying = "NIFTY 50"};

// --------------------------------------------------------------------------- ContractKey

TEST(ContractKeyTest, CanonicalTextRoundTrips) {
    const ContractKey call = nifty_call(2026y / 10 / 27, 25'000);
    EXPECT_EQ(to_string(call), "NSE|FO|OPT|NIFTY|2026-10-27|25000.00|CE");
    EXPECT_EQ(to_string(kNiftyIndex), "NSE|CASH|IDX|NIFTY 50");

    for (const char* text :
         {"NSE|CASH|IDX|NIFTY 50", "BSE|CASH|EQ|RELIANCE", "NSE|FO|FUT|BANKNIFTY|2026-11-25",
          "BSE|FO|OPT|SENSEX|2026-10-30|82000.50|PE"}) {
        const auto parsed = parse_contract_key(text);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(to_string(*parsed), text);
    }
}

TEST(ContractKeyTest, RejectsMalformedText) {
    for (const char* bad :
         {"", "NSE|CASH|IDX", "XXX|CASH|EQ|A", "NSE|FO|EQ|A", "NSE|CASH|FUT|A|2026-10-27",
          "NSE|FO|FUT|A|2026-13-01", "NSE|FO|OPT|A|2026-10-27|100|XX",
          "NSE|FO|OPT|A|2026-10-27|abc|CE", "NSE|FO|FUT|A", "NSE|CASH|EQ|"}) {
        EXPECT_FALSE(parse_contract_key(bad).has_value()) << bad;
    }
}

// --------------------------------------------------------------------------- registry

TEST(RegistryTest, AssignsDenseStableIds) {
    InstrumentRegistry registry;
    const InstrumentId index = registry.assign(kNiftyIndex);
    const InstrumentId call = registry.assign(nifty_call(2026y / 10 / 27, 25'000));
    EXPECT_EQ(index, InstrumentId(1));
    EXPECT_EQ(call, InstrumentId(2));
    EXPECT_EQ(registry.assign(kNiftyIndex), index);  // same key, same id
    EXPECT_EQ(registry.find(nifty_call(2026y / 10 / 27, 25'000)), call);
    EXPECT_FALSE(registry.find(nifty_call(2026y / 10 / 27, 25'100)).valid());
    ASSERT_NE(registry.get(call), nullptr);
    EXPECT_EQ(registry.get(call)->key.underlying, "NIFTY");
    EXPECT_EQ(registry.get(InstrumentId(99)), nullptr);
    EXPECT_EQ(registry.get(InstrumentId{}), nullptr);
}

TEST(RegistryTest, BeginDayClearsDailyAttributesButKeepsIds) {
    InstrumentRegistry registry;
    const InstrumentId id = registry.assign(kNiftyIndex);
    registry.get(id)->trading_symbol = "NIFTY 50";
    registry.get(id)->listed_today = true;
    registry.begin_day();
    EXPECT_TRUE(registry.get(id)->trading_symbol.empty());
    EXPECT_FALSE(registry.get(id)->listed_today);
    EXPECT_EQ(registry.find(kNiftyIndex), id);
}

// --------------------------------------------------------------------------- persistence

class RegistryFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("mda_registry_" + std::to_string(::getpid()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".csv");
        std::filesystem::remove(path_);
    }
    void TearDown() override { std::filesystem::remove(path_); }

    void write_file(const std::string& content) const {
        std::ofstream(path_, std::ios::binary) << content;
    }

    std::filesystem::path path_;
};

TEST_F(RegistryFileTest, MissingFileIsAnEmptyRegistry) {
    const auto loaded = InstrumentRegistry::load(path_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->registry.size(), 0u);
}

TEST_F(RegistryFileTest, SavedIdsSurviveReloadAndNewIdsContinue) {
    {
        InstrumentRegistry registry;
        registry.assign(kNiftyIndex);
        registry.assign(nifty_call(2026y / 10 / 27, 25'000));
        EXPECT_EQ(registry.save_new(path_).value(), 2u);
        EXPECT_EQ(registry.unsaved(), 0u);
        EXPECT_EQ(registry.save_new(path_).value(), 0u);  // nothing new
    }
    auto loaded = InstrumentRegistry::load(path_);
    ASSERT_TRUE(loaded.has_value());
    InstrumentRegistry& registry = loaded->registry;
    EXPECT_EQ(registry.find(kNiftyIndex), InstrumentId(1));
    EXPECT_EQ(registry.find(nifty_call(2026y / 10 / 27, 25'000)), InstrumentId(2));
    EXPECT_EQ(registry.unsaved(), 0u);

    // Next day: a new contract gets the next id and is appended, not rewritten.
    EXPECT_EQ(registry.assign(nifty_call(2026y / 11 / 3, 25'000)), InstrumentId(3));
    EXPECT_EQ(registry.save_new(path_).value(), 1u);
    const auto reloaded = InstrumentRegistry::load(path_);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->registry.size(), 3u);
}

TEST_F(RegistryFileTest, IgnoresAPartialLastLineFromAnInterruptedWrite) {
    write_file("# header\n1,NSE|CASH|IDX|NIFTY 50\n2,NSE|FO|OPT|NIF");  // crash mid-append
    const auto loaded = InstrumentRegistry::load(path_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->ignored_partial_line);
    EXPECT_EQ(loaded->registry.size(), 1u);
}

TEST_F(RegistryFileTest, RejectsCorruptFiles) {
    write_file("1,NSE|CASH|IDX|NIFTY 50\nnot a line\n");
    EXPECT_EQ(InstrumentRegistry::load(path_).error(), RegistryError::MalformedLine);

    write_file("1,NSE|CASH|IDX|NIFTY 50\n3,NSE|CASH|EQ|INFY\n");  // gap in ids
    EXPECT_EQ(InstrumentRegistry::load(path_).error(), RegistryError::DuplicateId);

    write_file("1,NSE|CASH|IDX|NIFTY 50\n2,NSE|CASH|IDX|NIFTY 50\n");
    EXPECT_EQ(InstrumentRegistry::load(path_).error(), RegistryError::DuplicateKey);
}

}  // namespace
}  // namespace mda::instruments
