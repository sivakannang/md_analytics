#include <chrono>

#include <gtest/gtest.h>

#include <mda/instruments/mcx.hpp>
#include <mda/instruments/session.hpp>

namespace mda::instruments {
namespace {

using namespace std::chrono_literals;

TEST(SessionTest, NseAndBseCloseAt1530AndFoPrintsUntil1540) {
    const Session cash = trading_session(Exchange::Nse, Market::Cash, 2026y / 10 / 27);
    EXPECT_EQ(cash.open, 9 * 60 + 15);
    EXPECT_EQ(cash.close, 15 * 60 + 30);
    EXPECT_EQ(cash.last, 15 * 60 + 30);
    const Session fo = trading_session(Exchange::Bse, Market::Fo, 2026y / 10 / 27);
    EXPECT_EQ(fo.close, 15 * 60 + 30);
    EXPECT_EQ(fo.last, 15 * 60 + 40);
}

TEST(SessionTest, McxClosesLaterWhenTheUsIsOffDaylightSaving) {
    // US DST 2026: 8 March to 1 November.
    EXPECT_EQ(trading_session(Exchange::Mcx, Market::Fo, 2026y / 10 / 27).close, 23 * 60 + 30);
    EXPECT_EQ(trading_session(Exchange::Mcx, Market::Fo, 2026y / 10 / 30).close, 23 * 60 + 30);
    EXPECT_EQ(trading_session(Exchange::Mcx, Market::Fo, 2026y / 11 / 2).close, 23 * 60 + 55);
    EXPECT_EQ(trading_session(Exchange::Mcx, Market::Fo, 2026y / 3 / 6).close, 23 * 60 + 55);
    EXPECT_EQ(trading_session(Exchange::Mcx, Market::Fo, 2026y / 3 / 9).close, 23 * 60 + 30);
    EXPECT_EQ(trading_session(Exchange::Mcx, Market::Fo, 2026y / 10 / 27).open, 9 * 60);
    EXPECT_EQ(last_close(2026y / 12 / 1), 23 * 60 + 55);
}

TEST(SessionTest, IstDayTurnsAtIstMidnight) {
    // 2026-10-27 18:29:59 UTC is 23:59:59 IST; a second later it is the 28th.
    EXPECT_EQ(ist_day(1'793'125'799), 2026y / 10 / 27);
    EXPECT_EQ(ist_day(1'793'125'800), 2026y / 10 / 28);
}

TEST(McxTest, UnitsPerLot) {
    EXPECT_EQ(mcx_units_per_lot("CRUDEOIL"), 100);
    EXPECT_EQ(mcx_units_per_lot("GOLD"), 100);
    EXPECT_EQ(mcx_units_per_lot("SILVERM"), 5);
    EXPECT_EQ(mcx_units_per_lot("KAPAS"), std::nullopt);
}

}  // namespace
}  // namespace mda::instruments
