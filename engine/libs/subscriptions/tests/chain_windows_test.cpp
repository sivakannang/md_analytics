#include <algorithm>
#include <chrono>
#include <set>

#include <gtest/gtest.h>

#include <mda/subscriptions/chain_windows.hpp>

namespace mda::subscriptions {
namespace {

using instruments::ContractKey;
using instruments::Exchange;
using instruments::Kind;
using instruments::Market;
using instruments::OptionType;
using namespace std::chrono_literals;

core::Price rupees(std::int64_t value) {
    return core::Price::from_raw(value * core::Price::kScale);
}

class ChainWindowsTest : public ::testing::Test {
protected:
    ChainWindowsTest() {
        spot_ = add({.exchange = Exchange::Nse,
                     .market = Market::Cash,
                     .kind = Kind::Index,
                     .underlying = "NIFTY 50"});
        for (const auto expiry :
             {2026y / 11 / 3, 2026y / 10 / 27, 2026y / 11 / 24}) {  // unsorted on purpose
            add({.exchange = Exchange::Nse,
                 .market = Market::Fo,
                 .kind = Kind::Future,
                 .underlying = "NIFTY",
                 .expiry = expiry});
            for (std::int64_t strike = 22'000; strike <= 23'000; strike += 50) {
                for (const auto type : {OptionType::Call, OptionType::Put}) {
                    if (strike == 22'950 && type == OptionType::Put) {
                        continue;  // a strike listed on one side only
                    }
                    add({.exchange = Exchange::Nse,
                         .market = Market::Fo,
                         .kind = Kind::Option,
                         .underlying = "NIFTY",
                         .expiry = expiry,
                         .strike = rupees(strike),
                         .option_type = type});
                }
            }
        }
        // Another underlying's option must not leak in.
        add({.exchange = Exchange::Nse,
             .market = Market::Fo,
             .kind = Kind::Option,
             .underlying = "FINNIFTY",
             .expiry = 2026y / 10 / 27,
             .strike = rupees(22'500),
             .option_type = OptionType::Call});
    }

    core::InstrumentId add(ContractKey key) {
        const auto id = registry_.assign(key);
        registry_.get(id)->listed_today = true;
        return id;
    }

    ChainWindows make(int expiries = 2, int strikes = 3) {
        return ChainWindows(
            registry_,
            {{.underlying = "NIFTY", .spot = spot_, .expiries = expiries, .strikes = strikes}},
            {.buffer = 2, .grace = 600s});
    }

    /// Strikes (rupees) of the options in `ids` for one expiry and side.
    std::set<std::int64_t> strikes(const std::vector<core::InstrumentId>& ids,
                                   std::chrono::year_month_day expiry, OptionType type) const {
        std::set<std::int64_t> out;
        for (const auto id : ids) {
            const auto& key = registry_.get(id)->key;
            if (key.kind == Kind::Option && key.expiry == expiry && key.option_type == type) {
                out.insert(key.strike.raw() / core::Price::kScale);
            }
        }
        return out;
    }

    instruments::InstrumentRegistry registry_;
    core::InstrumentId spot_;
};

TEST_F(ChainWindowsTest, BeforeAnyPriceOnlySpotAndFutures) {
    auto windows = make();
    const auto wanted = windows.wanted(0);
    EXPECT_EQ(wanted.size(), 4u);  // index + 3 futures
    EXPECT_TRUE(std::ranges::find(wanted, spot_) != wanted.end());
    EXPECT_TRUE(windows.is_spot(spot_));
}

TEST_F(ChainWindowsTest, WindowIsAtmPlusMinusStrikesAndBufferForTheNearestExpiries) {
    auto windows = make();
    EXPECT_TRUE(windows.on_spot(spot_, rupees(22'512)));  // ATM 22500
    const auto wanted = windows.wanted(0);
    // ±(3 + 2) strikes: 22250..22750, on the 2 nearest expiries only.
    std::set<std::int64_t> expected;
    for (std::int64_t s = 22'250; s <= 22'750; s += 50) {
        expected.insert(s);
    }
    EXPECT_EQ(strikes(wanted, 2026y / 10 / 27, OptionType::Call), expected);
    EXPECT_EQ(strikes(wanted, 2026y / 11 / 3, OptionType::Put), expected);
    EXPECT_TRUE(strikes(wanted, 2026y / 11 / 24, OptionType::Call).empty());
    EXPECT_EQ(wanted.size(), 1 + 3 + 2 * 11 * 2u);

    const auto recentres = windows.take_recentres();
    ASSERT_EQ(recentres.size(), 2u);
    EXPECT_EQ(recentres[0].to, rupees(22'500));
    EXPECT_EQ(recentres[0].from, core::Price{});  // first centring
}

TEST_F(ChainWindowsTest, SmallMovesDoNotRecentreLargeOnesDo) {
    auto windows = make(1);
    ASSERT_TRUE(windows.on_spot(spot_, rupees(22'500)));
    (void)windows.take_recentres();
    EXPECT_FALSE(windows.on_spot(spot_, rupees(22'610)));  // ATM 22600: 2 strikes, within buffer
    EXPECT_TRUE(windows.on_spot(spot_, rupees(22'660)));   // ATM 22650: 3 strikes > buffer
    const auto recentres = windows.take_recentres();
    ASSERT_EQ(recentres.size(), 1u);
    EXPECT_EQ(recentres[0].from, rupees(22'500));
    EXPECT_EQ(recentres[0].to, rupees(22'650));
}

TEST_F(ChainWindowsTest, StrikesLeavingAWindowStayForTheGracePeriod) {
    auto windows = make(1);
    ASSERT_TRUE(windows.on_spot(spot_, rupees(22'500)));
    ASSERT_EQ(strikes(windows.wanted(1'000), 2026y / 10 / 27, OptionType::Call).count(22'250), 1u);

    ASSERT_TRUE(windows.on_spot(spot_, rupees(22'700)));  // window now 22450..22950
    auto wanted = windows.wanted(1'010);
    EXPECT_EQ(strikes(wanted, 2026y / 10 / 27, OptionType::Call).count(22'250), 1u);  // held
    EXPECT_EQ(strikes(wanted, 2026y / 10 / 27, OptionType::Call).count(22'950), 1u);  // new
    EXPECT_EQ(strikes(wanted, 2026y / 10 / 27, OptionType::Put).count(22'950), 0u);   // not listed

    wanted = windows.wanted(1'010 + 601);
    EXPECT_EQ(strikes(wanted, 2026y / 10 / 27, OptionType::Call).count(22'250), 0u);  // grace over
    EXPECT_EQ(strikes(wanted, 2026y / 10 / 27, OptionType::Call).count(22'450), 1u);
}

TEST_F(ChainWindowsTest, ReturningStrikesLeaveTheGraceList) {
    auto windows = make(1);
    ASSERT_TRUE(windows.on_spot(spot_, rupees(22'500)));
    (void)windows.wanted(1'000);
    ASSERT_TRUE(windows.on_spot(spot_, rupees(22'700)));
    (void)windows.wanted(1'001);  // 22250..22400 held
    ASSERT_TRUE(windows.on_spot(spot_, rupees(22'500)));
    (void)windows.wanted(1'002);  // back in the window
    ASSERT_TRUE(windows.on_spot(spot_, rupees(22'700)));
    // Left again at 1'003: a fresh grace period, not the one started at 1'001.
    const auto wanted = windows.wanted(1'003 + 599);
    EXPECT_EQ(strikes(wanted, 2026y / 10 / 27, OptionType::Call).count(22'250), 1u);
}

TEST_F(ChainWindowsTest, ClampsAtTheEdgesOfTheStrikeGrid) {
    auto windows = make(1, 20);
    ASSERT_TRUE(windows.on_spot(spot_, rupees(21'000)));  // far below the lowest strike
    const auto calls = strikes(windows.wanted(0), 2026y / 10 / 27, OptionType::Call);
    EXPECT_EQ(*calls.begin(), 22'000);
    EXPECT_EQ(*calls.rbegin(), 23'000);  // ±25 covers the whole grid
}

TEST_F(ChainWindowsTest, DescribesEachChain) {
    const auto lines = make(2).describe();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0],
              "NIFTY: 2 expiries (2026-10-27 21 strikes, 2026-11-03 21 strikes), 3 futures, ATM ± "
              "3 (+2 buffer)");
}

}  // namespace
}  // namespace mda::subscriptions
