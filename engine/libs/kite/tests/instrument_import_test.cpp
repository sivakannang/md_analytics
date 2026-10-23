#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <mda/kite/instrument_import.hpp>
#include <mda/kite/tick_decoder.hpp>

namespace mda::kite {
namespace {

using core::InstrumentId;
using core::Price;
using core::Quantity;
using instruments::ContractKey;
using instruments::Exchange;
using instruments::InstrumentRegistry;
using instruments::Kind;
using instruments::Market;
using instruments::OptionType;
using namespace std::chrono_literals;

// Kite's real column layout; tokens = exchange_token << 8 | segment.
constexpr const char* kHeader =
    "instrument_token,exchange_token,tradingsymbol,name,last_price,expiry,strike,tick_size,"
    "lot_size,instrument_type,segment,exchange\n";

const std::string kDayOne =
    std::string(kHeader) +
    "256265,1001,NIFTY 50,NIFTY 50,0,,0,0,0,EQ,INDICES,NSE\n"
    "265,1,SENSEX,SENSEX,0,,0,0,0,EQ,INDICES,BSE\n"
    "738561,2885,RELIANCE,\"RELIANCE INDUSTRIES, LTD\",0,,0,0.1,1,EQ,NSE,NSE\n"
    "13368834,52222,NIFTY26OCTFUT,\"NIFTY\",0,2026-10-27,0,0.1,75,FUT,NFO-FUT,NFO\n"
    "13369090,52223,NIFTY26O2725000CE,\"NIFTY\",0,2026-10-27,25000,0.05,75,CE,NFO-OPT,NFO\n"
    "13369346,52224,NIFTY26O2725000PE,\"NIFTY\",0,2026-10-27,25000,0.05,75,PE,NFO-OPT,NFO\n"
    "13370114,52227,RELIANCE26OCTFUT,\"RELIANCE\",0,2026-10-27,0,0.1,500,FUT,NFO-FUT,NFO\n"
    "270085,1055,SENSEX26O3082000CE,\"SENSEX\",0,2026-10-30,82000,0.05,20,CE,BFO-OPT,BFO\n"
    "4242179,16571,USDINR26OCTFUT,\"USDINR\",0,2026-10-29,0,0.0025,1,FUT,CDS-FUT,CDS\n"
    "2559746,9999,BADOPT,\"NIFTY\",0,2026-10-27,abc,0.05,75,CE,NFO-OPT,NFO\n"
    "13371138,52230,XYZ26OCTFUT,\"XYZ\",0,2026-10-27,0,0.05,100,FUT,NFO-FUT,NFO\n";

ContractKey option(std::chrono::year_month_day expiry, std::int64_t strike, OptionType type) {
    return {.exchange = Exchange::Nse,
            .market = Market::Fo,
            .kind = Kind::Option,
            .underlying = "NIFTY",
            .expiry = expiry,
            .strike = Price::from_raw(strike * Price::kScale),
            .option_type = type};
}

ContractKey cash(Exchange exchange, Kind kind, std::string symbol) {
    return {.exchange = exchange, .market = Market::Cash, .kind = kind, .underlying = symbol};
}

TEST(InstrumentImportTest, ImportsSupportedRowsAndCountsTheRest) {
    InstrumentRegistry registry;
    const auto result = import_instruments(kDayOne, registry);
    ASSERT_TRUE(result.has_value());
    const ImportStats& stats = result->stats;
    EXPECT_EQ(stats.rows, 11u);
    EXPECT_EQ(stats.imported, 9u);
    EXPECT_EQ(stats.new_instruments, 9u);
    EXPECT_EQ(stats.skipped_unsupported, 1u);  // CDS
    EXPECT_EQ(stats.malformed, 1u);            // strike "abc"
    EXPECT_EQ(stats.unlinked, 1u);             // XYZ has no underlying listed
    EXPECT_EQ(result->tokens.size(), 9u);
}

TEST(InstrumentImportTest, SetsAttributesAndTokenMapping) {
    InstrumentRegistry registry;
    const auto result = import_instruments(kDayOne, registry);
    ASSERT_TRUE(result.has_value());

    const InstrumentId call = registry.find(option(2026y / 10 / 27, 25'000, OptionType::Call));
    ASSERT_TRUE(call.valid());
    EXPECT_EQ(result->tokens.find(13'369'090), call);
    const auto* instrument = registry.get(call);
    EXPECT_EQ(instrument->trading_symbol, "NIFTY26O2725000CE");
    EXPECT_EQ(instrument->lot_size, Quantity(75));
    EXPECT_EQ(instrument->tick_size, Price::from_raw(500));  // 0.05
    EXPECT_TRUE(instrument->listed_today);

    // Quoted field with an embedded comma parsed correctly.
    const InstrumentId reliance = registry.find(cash(Exchange::Nse, Kind::Equity, "RELIANCE"));
    EXPECT_EQ(result->tokens.find(738'561), reliance);
}

TEST(InstrumentImportTest, LinksDerivativesToTheirUnderlying) {
    InstrumentRegistry registry;
    ASSERT_TRUE(import_instruments(kDayOne, registry).has_value());

    const InstrumentId nifty_index = registry.find(cash(Exchange::Nse, Kind::Index, "NIFTY 50"));
    const InstrumentId sensex_index = registry.find(cash(Exchange::Bse, Kind::Index, "SENSEX"));
    const InstrumentId reliance = registry.find(cash(Exchange::Nse, Kind::Equity, "RELIANCE"));

    const InstrumentId call = registry.find(option(2026y / 10 / 27, 25'000, OptionType::Call));
    EXPECT_EQ(registry.get(call)->underlying, nifty_index);  // "NIFTY" -> "NIFTY 50"

    for (const auto& instrument : registry.all()) {
        if (instrument.key.underlying == "RELIANCE" && instrument.key.kind == Kind::Future) {
            EXPECT_EQ(instrument.underlying, reliance);  // stock future -> equity
        }
        if (instrument.key.exchange == Exchange::Bse && instrument.key.kind == Kind::Option) {
            EXPECT_EQ(instrument.underlying, sensex_index);  // BFO -> BSE index
        }
    }
}

// ADR-0008's reason to exist: NSE reuses F&O tokens after expiry.
TEST(InstrumentImportTest, ReusedTokenMapsToTheNewContractWithoutDisturbingHistory) {
    InstrumentRegistry registry;
    const auto day_one = import_instruments(kDayOne, registry);
    ASSERT_TRUE(day_one.has_value());
    const ContractKey old_contract = option(2026y / 10 / 27, 25'000, OptionType::Call);
    const InstrumentId old_id = registry.find(old_contract);
    ASSERT_EQ(day_one->tokens.find(13'369'090), old_id);

    // After expiry, the exchange hands the same token to a new contract.
    const std::string day_two =
        std::string(kHeader) +
        "256265,1001,NIFTY 50,NIFTY 50,0,,0,0,0,EQ,INDICES,NSE\n"
        "13369090,52223,NIFTY26N0325000CE,\"NIFTY\",0,2026-11-03,25000,0.05,75,CE,NFO-OPT,NFO\n";
    const auto result = import_instruments(day_two, registry);
    ASSERT_TRUE(result.has_value());

    const InstrumentId new_id = registry.find(option(2026y / 11 / 3, 25'000, OptionType::Call));
    EXPECT_TRUE(new_id.valid());
    EXPECT_NE(new_id, old_id);
    EXPECT_EQ(result->tokens.find(13'369'090), new_id);  // today's meaning of the token
    EXPECT_EQ(registry.find(old_contract), old_id);      // history keeps its id
    EXPECT_FALSE(registry.get(old_id)->listed_today);
    EXPECT_EQ(result->stats.new_instruments, 1u);
}

TEST(InstrumentImportTest, FindsColumnsByNameNotPosition) {
    const std::string reordered =
        "exchange,segment,instrument_type,lot_size,tick_size,strike,expiry,name,tradingsymbol,"
        "instrument_token,extra_column\n"
        "NSE,INDICES,EQ,0,0,0,,NIFTY 50,NIFTY 50,256265,x\n";
    InstrumentRegistry registry;
    const auto result = import_instruments(reordered, registry);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->stats.imported, 1u);
    EXPECT_TRUE(result->tokens.find(256'265).valid());
}

TEST(InstrumentImportTest, RejectsMissingHeaderOrColumns) {
    InstrumentRegistry registry;
    EXPECT_EQ(import_instruments("", registry).error(), ImportError::Empty);
    EXPECT_EQ(import_instruments("instrument_token,tradingsymbol\n1,A\n", registry).error(),
              ImportError::MissingColumns);
}

// End to end: today's token map lets the decoder tag ticks with permanent ids.
TEST(InstrumentImportTest, ImportedTokenMapDrivesTheTickDecoder) {
    InstrumentRegistry registry;
    const auto result = import_instruments(kDayOne, registry);
    ASSERT_TRUE(result.has_value());
    TickDecoder decoder(result->tokens);

    // Frame: 1 packet, 8 bytes: token 13369090, LTP 152.35 (15235 paise).
    const std::vector<std::byte> frame = {std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
                                          std::byte{0x08}, std::byte{0x00}, std::byte{0xCB},
                                          std::byte{0xFF}, std::byte{0x02}, std::byte{0x00},
                                          std::byte{0x00}, std::byte{0x3B}, std::byte{0x83}};
    InstrumentId tagged;
    Price ltp;
    decoder.decode(frame, core::Timestamp::now(),
                   [&](const messages::MarketSnapshot& s, const messages::DepthSnapshot*) {
                       tagged = s.header.instrument_id;
                       ltp = s.ltp;
                   });
    EXPECT_EQ(tagged, registry.find(option(2026y / 10 / 27, 25'000, OptionType::Call)));
    EXPECT_EQ(core::to_string(ltp), "152.35");
}

}  // namespace
}  // namespace mda::kite
