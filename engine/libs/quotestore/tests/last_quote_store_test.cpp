#include <algorithm>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/quotestore/last_quote_store.hpp>

namespace mda::quotestore {
namespace {

messages::MarketSnapshot snapshot(std::uint32_t id, std::int64_t ltp_raw) {
    messages::MarketSnapshot s{};
    s.header = messages::make_header<messages::MarketSnapshot>(core::InstrumentId(id), 1);
    s.fields = messages::kFieldLtp;
    s.ltp = core::Price::from_raw(ltp_raw);
    return s;
}

std::vector<messages::MarketSnapshot> snapshots_in(LastQuoteStore& store) {
    std::vector<messages::MarketSnapshot> out;
    EXPECT_TRUE(store
                    .load([&](std::uint32_t type, std::span<const std::byte> payload) {
                        if (auto s = messages::decode<messages::MarketSnapshot>(type, payload)) {
                            out.push_back(*s);
                        }
                    })
                    .has_value());
    return out;
}

TEST(LastQuoteStoreTest, KeepsTheLatestMessagePerInstrumentAndType) {
    auto store = LastQuoteStore::open_in_memory().value();
    store.remember(snapshot(7, 100));
    store.remember(snapshot(7, 105));  // replaces 100 before the flush
    store.remember(snapshot(8, 200));
    messages::DepthSnapshot depth{};
    depth.header = messages::make_header<messages::DepthSnapshot>(core::InstrumentId(7), 1);
    store.remember(depth);
    EXPECT_EQ(store.flush().value(), 3u);
    EXPECT_EQ(store.flush().value(), 0u);  // nothing new

    store.remember(snapshot(8, 210));  // replaces the saved row
    EXPECT_EQ(store.flush().value(), 1u);

    auto saved = snapshots_in(store);
    ASSERT_EQ(saved.size(), 2u);
    std::ranges::sort(saved, {}, [](const auto& s) { return s.header.instrument_id.value(); });
    EXPECT_EQ(saved[0].ltp, core::Price::from_raw(105));
    EXPECT_EQ(saved[1].ltp, core::Price::from_raw(210));

    std::size_t depths = 0;
    ASSERT_TRUE(store
                    .load([&](std::uint32_t type, std::span<const std::byte> payload) {
                        depths +=
                            messages::decode<messages::DepthSnapshot>(type, payload).has_value()
                                ? 1
                                : 0;
                    })
                    .has_value());
    EXPECT_EQ(depths, 1u);
}

TEST(LastQuoteStoreTest, SurvivesReopeningTheFile) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("mda_last_quotes_" +
                       std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".db");
    std::filesystem::remove(path);
    {
        auto store = LastQuoteStore::open(path).value();
        store.remember(snapshot(9, 300));
        messages::CircuitLimits limits{};
        limits.header = messages::make_header<messages::CircuitLimits>(core::InstrumentId(9), 1);
        store.remember(limits);
        ASSERT_EQ(store.flush().value(), 2u);
    }
    auto reopened = LastQuoteStore::open(path).value();
    const auto saved = snapshots_in(reopened);
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved[0].ltp, core::Price::from_raw(300));
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::filesystem::remove(path.string() + suffix);
    }
}

TEST(LastQuoteStoreTest, AnOlderSchemaVersionIsNotDecoded) {
    auto store = LastQuoteStore::open_in_memory().value();
    auto old = snapshot(7, 100);
    old.header.schema_version = static_cast<std::uint16_t>(old.header.schema_version + 1);
    store.remember(old);
    ASSERT_EQ(store.flush().value(), 1u);
    EXPECT_TRUE(snapshots_in(store).empty());
}

}  // namespace
}  // namespace mda::quotestore
