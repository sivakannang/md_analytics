#include <chrono>
#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>
#include <mda/core/quantity.hpp>
#include <mda/core/timestamp.hpp>

namespace mda::core {
namespace {

// The point of strong types: these must not compile.
static_assert(!std::is_convertible_v<std::int64_t, Price>);
static_assert(!std::is_convertible_v<Price, std::int64_t>);
static_assert(!std::is_convertible_v<std::int64_t, Quantity>);
static_assert(!std::is_convertible_v<std::uint32_t, InstrumentId>);
template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };
static_assert(!Addable<Price, Quantity>);
static_assert(!Addable<Timestamp, Timestamp>);

// Layout guarantees relied on by bus messages and files.
static_assert(sizeof(Price) == 8 && std::is_trivially_copyable_v<Price>);
static_assert(sizeof(Quantity) == 8 && std::is_trivially_copyable_v<Quantity>);
static_assert(sizeof(Timestamp) == 8 && std::is_trivially_copyable_v<Timestamp>);
static_assert(sizeof(InstrumentId) == 4 && std::is_trivially_copyable_v<InstrumentId>);

TEST(PriceTest, ConvertsKitePaiseExactly) {
    const Price price = Price::from_paise(12'345);  // ₹123.45
    EXPECT_EQ(price.raw(), 1'234'500);
    EXPECT_EQ(to_string(price), "123.45");
    EXPECT_DOUBLE_EQ(price.to_double(), 123.45);
}

TEST(PriceTest, ArithmeticAndComparisonAreExact) {
    // The classic floating-point failure: 0.1 + 0.2 != 0.3.
    const Price a = Price::from_raw(1'000);  // 0.1
    const Price b = Price::from_raw(2'000);  // 0.2
    EXPECT_EQ(a + b, Price::from_raw(3'000));
    EXPECT_LT(a, b);
    EXPECT_EQ(b - a, a);
    EXPECT_EQ(-a, Price::from_raw(-1'000));
}

TEST(PriceTest, FormatsWithAtLeastTwoDecimals) {
    EXPECT_EQ(to_string(Price::from_raw(0)), "0.00");
    EXPECT_EQ(to_string(Price::from_raw(25)), "0.0025");      // currency tick
    EXPECT_EQ(to_string(Price::from_raw(-75'000)), "-7.50");  // negative change
    EXPECT_EQ(to_string(Price::from_raw(250'000'000)), "25000.00");
}

TEST(PriceTest, ParsesDecimalTextExactly) {
    EXPECT_EQ(parse_price("25000"), Price::from_raw(250'000'000));
    EXPECT_EQ(parse_price("24550.5"), Price::from_raw(245'505'000));
    EXPECT_EQ(parse_price("0.05"), Price::from_raw(500));
    EXPECT_EQ(parse_price("0.0025"), Price::from_raw(25));
    EXPECT_EQ(parse_price("-7.25"), Price::from_raw(-72'500));
    EXPECT_EQ(parse_price("25000.000000"), Price::from_raw(250'000'000));  // trailing zeros ok
    EXPECT_EQ(parse_price("1."), Price::from_raw(10'000));
    EXPECT_EQ(parse_price(".5"), Price::from_raw(5'000));
}

TEST(PriceTest, RejectsInvalidDecimalText) {
    for (const char* bad :
         {"", "-", ".", "1.2.3", "12a", "0.00001", "1e5", " 1", "99999999999999999999"}) {
        EXPECT_FALSE(parse_price(bad).has_value()) << bad;
    }
}

TEST(QuantityTest, Arithmetic) {
    EXPECT_EQ(Quantity(75) + Quantity(25), Quantity(100));
    EXPECT_EQ((Quantity(75) - Quantity(100)).value(), -25);
}

TEST(TimestampTest, DifferencesAreDurations) {
    const Timestamp start = Timestamp::from_seconds(1'761'000'000);
    const Timestamp later = start + std::chrono::milliseconds(1500);
    EXPECT_EQ(later - start, std::chrono::milliseconds(1500));
    EXPECT_LT(start, later);
    EXPECT_FALSE(Timestamp{}.is_set());
    EXPECT_GT(Timestamp::now(), Timestamp::from_seconds(1'700'000'000));  // after Nov 2023
}

TEST(InstrumentIdTest, ZeroIsInvalid) {
    EXPECT_FALSE(InstrumentId{}.valid());
    EXPECT_TRUE(InstrumentId(48'213).valid());
    EXPECT_EQ(InstrumentId(7), InstrumentId(7));
}

}  // namespace
}  // namespace mda::core
