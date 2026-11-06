#include <gtest/gtest.h>

#include <mda/rms/risk.hpp>

namespace mda::rms {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

constexpr std::int64_t kMidnightIst = 1'792'607'400;  // 2026-10-22 00:00 IST
constexpr std::int32_t kToday = 20'748;
constexpr Millis at(int hour, int minute) {
    return (kMidnightIst + hour * 3600 + minute * 60) * 1000;
}
constexpr Millis kTen = at(10, 0);
constexpr Paise kLakh = 1'00'000 * 100;

const oms::ChargeRates kRates = oms::zerodha_2026_10();

const InstrumentInfo kFuture{
    .contract = {.id = core::InstrumentId(1), .segment = oms::Segment::Future, .exchange = oms::Exchange::Nse,
                 .lot_size = 75, .strike = {}, .call = false},
    .tick = rupees(0.05),
    .freeze_quantity = 1800,
    .listed = true,
    .expiry_days = kToday + 5,
    .index = true};

const InstrumentInfo kCall{
    .contract = {.id = core::InstrumentId(2), .segment = oms::Segment::Option, .exchange = oms::Exchange::Nse,
                 .lot_size = 75, .strike = rupees(22'500), .call = true},
    .tick = rupees(0.05),
    .freeze_quantity = 1800,
    .listed = true,
    .expiry_days = kToday + 5,
    .index = true};

const Quote kFutureQuote{.ltp = rupees(22'500), .best_ask = rupees(22'500.05), .best_bid = rupees(22'499.95)};
const Quote kCallQuote{.ltp = rupees(120), .best_ask = rupees(120), .best_bid = rupees(119.95)};

oms::OrderSpec spec(const InstrumentInfo& instrument, oms::Side side, std::int64_t qty,
                    oms::OrderType type = oms::OrderType::Market, double price = 0, double trigger = 0,
                    oms::Source source = oms::Source::Ui) {
    return {.account = 1, .client_order_id = 1, .source = source, .instrument = instrument.contract.id,
            .side = side, .type = type, .product = oms::Product::Nrml, .quantity = qty,
            .price = rupees(price), .trigger = rupees(trigger), .tag = {}};
}

class RiskTest : public ::testing::Test {
protected:
    /// The account (created on first use with `opening` cash) and a neutral view of it.
    AccountView view(Paise opening = 10 * kLakh) {
        if (!account_) {
            account_.emplace(1, opening, kTen);
        }
        return {.account = &*account_, .blocked = 0, .unrealised = 0, .day_pnl = 0,
                .opening_cash = opening, .gross_notional = 0, .open_order_quantity = 0, .kill_switch = false};
    }
    void hold(const InstrumentInfo& instrument, oms::Side side, std::int64_t qty, double price) {
        (void)view();
        account_->apply_fill(kRates, instrument.contract, oms::Product::Nrml, side, qty, rupees(price), 0, 1, kTen);
    }
    RejectCode code(const oms::OrderSpec& order, const InstrumentInfo& instrument, const Quote& quote,
                    const AccountView& v, Millis now = kTen) {
        const auto result = risk_.check(order, instrument, quote, v, now);
        EXPECT_FALSE(result.has_value());
        return result ? RejectCode::NotTradable : result.error().code;
    }

    RiskManager risk_;
    std::optional<oms::Account> account_;
};

TEST_F(RiskTest, ApprovesAFuturesBuyWithItsMargin) {
    const auto approval = risk_.check(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, view(), kTen);
    ASSERT_TRUE(approval.has_value()) << approval.error().message;
    EXPECT_FALSE(approval->reducing);
    EXPECT_EQ(approval->margin, 20'250'045);  // 12% of 22,500.05 × 75 at the ask
    InstrumentInfo stock = kFuture;
    stock.index = false;
    EXPECT_EQ(risk_.margin(stock, oms::Side::Buy, 75, rupees(22'500)), 33'750'000);  // 20% for a stock future
}

TEST_F(RiskTest, ValidatesLikeTheExchange) {
    const auto v = view();
    InstrumentInfo expired = kFuture;
    expired.expiry_days = kToday - 1;
    EXPECT_EQ(code(spec(expired, oms::Side::Buy, 75), expired, kFutureQuote, v), RejectCode::NotTradable);
    InstrumentInfo unlisted = kFuture;
    unlisted.listed = false;
    EXPECT_EQ(code(spec(unlisted, oms::Side::Buy, 75), unlisted, kFutureQuote, v), RejectCode::NotTradable);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 50), kFuture, kFutureQuote, v), RejectCode::BadQuantity);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 1875), kFuture, kFutureQuote, v), RejectCode::FreezeQuantity);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Limit, 22'500.03), kFuture, kFutureQuote, v),
              RejectCode::BadPrice);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Limit, 0), kFuture, kFutureQuote, v),
              RejectCode::BadPrice);
    // A buy stop triggers above the last price; its limit is at or above the trigger.
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::StopMarket, 0, 22'450), kFuture, kFutureQuote, v),
              RejectCode::BadTrigger);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::StopLimit, 22'540, 22'550), kFuture, kFutureQuote, v),
              RejectCode::BadPrice);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::StopMarket, 0, 22'550), kFuture, Quote{}, v),
              RejectCode::NoPrice);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v, at(9, 10)), RejectCode::MarketClosed);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v, at(15, 30)), RejectCode::MarketClosed);
    EXPECT_TRUE(risk_.check(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::StopLimit, 22'560, 22'550), kFuture,
                            kFutureQuote, v, kTen)
                    .has_value());
}

TEST_F(RiskTest, NoOptionWritingButClosingIsFine) {
    EXPECT_EQ(code(spec(kCall, oms::Side::Sell, 75), kCall, kCallQuote, view()), RejectCode::OptionWriting);
    hold(kCall, oms::Side::Buy, 75, 100);
    const auto close = risk_.check(spec(kCall, oms::Side::Sell, 75), kCall, kCallQuote, view(), kTen);
    ASSERT_TRUE(close.has_value());
    EXPECT_TRUE(close->reducing);
    EXPECT_EQ(close->margin, 0);
    EXPECT_EQ(code(spec(kCall, oms::Side::Sell, 150), kCall, kCallQuote, view()), RejectCode::OptionWriting);
}

TEST_F(RiskTest, KillSwitchDailyLossAndWindowLeaveOnlyReducing) {
    hold(kFuture, oms::Side::Buy, 75, 22'500);
    auto v = view();
    v.kill_switch = true;
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v), RejectCode::KillSwitch);
    EXPECT_TRUE(risk_.check(spec(kFuture, oms::Side::Sell, 75), kFuture, kFutureQuote, v, kTen).has_value());

    v.kill_switch = false;
    risk_.set_global_kill_switch(true);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v), RejectCode::KillSwitch);
    risk_.set_global_kill_switch(false);

    v.day_pnl = -50'000 * 100;  // 5% of ₹10 lakh
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v), RejectCode::DailyLoss);
    EXPECT_TRUE(risk_.check(spec(kFuture, oms::Side::Sell, 75), kFuture, kFutureQuote, v, kTen).has_value());

    v.day_pnl = 0;
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v, at(15, 26)), RejectCode::OutsideWindow);
    EXPECT_TRUE(risk_.check(spec(kFuture, oms::Side::Sell, 75), kFuture, kFutureQuote, v, at(15, 26)).has_value());
    // The automatic trader's window is narrower.
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Market, 0, 0, oms::Source::Auto), kFuture,
                   kFutureQuote, v, at(9, 17)),
              RejectCode::OutsideWindow);
}

// A person's orders have no lot or notional caps (as at Zerodha: margin decides); the
// automatic trader's do.
TEST_F(RiskTest, SizeLimits) {
    auto v = view(1000 * kLakh);
    v.open_order_quantity = 40 * 75;
    v.gross_notional = 500 * kLakh;
    const auto person = risk_.check(spec(kFuture, oms::Side::Buy, 24 * 75), kFuture, kFutureQuote, v, kTen);
    EXPECT_TRUE(person.has_value()) << person.error().message;
    v.open_order_quantity = 0;
    v.gross_notional = 0;
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 3 * 75, oms::OrderType::Market, 0, 0, oms::Source::Auto), kFuture,
                   kFutureQuote, v),
              RejectCode::OrderSize);
    v.open_order_quantity = 75;  // still to fill on the same side
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 2 * 75, oms::OrderType::Market, 0, 0, oms::Source::Auto), kFuture,
                   kFutureQuote, v),
              RejectCode::PositionSize);
    v.open_order_quantity = 0;
    v.auto_notional = 39 * kLakh;
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Market, 0, 0, oms::Source::Auto), kFuture,
                   kFutureQuote, v),
              RejectCode::Notional);
}

// The exchange's circuit limits bound every price and trigger; the automatic trader also
// keeps its ±1% band.
TEST_F(RiskTest, PriceBand) {
    const auto v = view();
    Quote circuits = kFutureQuote;
    circuits.lower_circuit = rupees(20'250);
    circuits.upper_circuit = rupees(24'750);
    const auto resting = risk_.check(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Limit, 21'000), kFuture,
                                     circuits, v, kTen);
    EXPECT_TRUE(resting.has_value()) << resting.error().message;  // −6.7%: rests, as at Zerodha
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Limit, 24'800), kFuture, circuits, v),
              RejectCode::PriceBand);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Sell, 75, oms::OrderType::StopMarket, 0, 20'000), kFuture, circuits, v),
              RejectCode::PriceBand);
    EXPECT_TRUE(risk_.check(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Limit, 24'800), kFuture, kFutureQuote,
                            v, kTen)
                    .has_value());  // circuits not known yet
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Limit, 22'800, 0, oms::Source::Auto), kFuture,
                   kFutureQuote, v),
              RejectCode::PriceBand);  // +1.3% for the automatic trader
    // An SL is banded around its trigger: a stop 2.2% below the market is fine, a limit
    // 1.4% below its trigger isn't.
    EXPECT_TRUE(risk_.check(spec(kFuture, oms::Side::Sell, 75, oms::OrderType::StopLimit, 21'950, 22'000,
                                 oms::Source::Auto),
                            kFuture, kFutureQuote, v, kTen)
                    .has_value());
    EXPECT_EQ(code(spec(kFuture, oms::Side::Sell, 75, oms::OrderType::StopLimit, 21'690, 22'000, oms::Source::Auto),
                   kFuture, kFutureQuote, v),
              RejectCode::PriceBand);
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, Quote{}, v), RejectCode::NoPrice);
}

TEST_F(RiskTest, FundsCountBlockedMarginAndLosses) {
    auto v = view(3 * kLakh);
    EXPECT_TRUE(risk_.check(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v, kTen).has_value());
    v.blocked = 1 * kLakh;  // ₹2 lakh left; needs ₹2.03 lakh
    const auto short_of = risk_.check(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v, kTen);
    ASSERT_FALSE(short_of.has_value());
    EXPECT_EQ(short_of.error().code, RejectCode::Funds);
    EXPECT_EQ(short_of.error().message, "insufficient funds: needs ₹202500.45, available ₹200000.00");
    v.blocked = 0;
    v.unrealised = -1 * kLakh;  // losses count, gains don't
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v), RejectCode::Funds);
}

TEST_F(RiskTest, OptionBuysBlockThePremiumWithABufferForMarket) {
    const auto market = risk_.check(spec(kCall, oms::Side::Buy, 75), kCall, kCallQuote, view(), kTen);
    ASSERT_TRUE(market.has_value());
    EXPECT_EQ(market->margin, 918'000);  // 120 × 1.02 × 75
    const auto limit = risk_.check(spec(kCall, oms::Side::Buy, 75, oms::OrderType::Limit, 119), kCall, kCallQuote,
                                   view(), kTen);
    ASSERT_TRUE(limit.has_value());
    EXPECT_EQ(limit->margin, 892'500);
}

// MCX (ADR-0031): its own hours, the commodity's margin, quantities in barrels.
TEST_F(RiskTest, McxTradesInTheEveningWithItsOwnMargin) {
    const InstrumentInfo crude{
        .contract = {.id = core::InstrumentId(3), .segment = oms::Segment::Future, .exchange = oms::Exchange::Mcx,
                     .lot_size = 100, .strike = {}, .call = false},
        .tick = rupees(1),
        .listed = true,
        .expiry_days = kToday + 20,
        .futures_margin = 320'000'000,  // 32%
        .market_open = 9 * 3600,
        .market_close = 23 * 3600 + 30 * 60};
    const Quote quote{.ltp = rupees(9'600), .best_ask = rupees(9'601), .best_bid = rupees(9'599)};
    const auto evening = risk_.check(spec(crude, oms::Side::Buy, 100), crude, quote, view(), at(21, 0));
    ASSERT_TRUE(evening.has_value()) << evening.error().message;
    EXPECT_EQ(evening->margin, 30'723'200);  // 32% of ₹9,601 × 100 barrels
    EXPECT_TRUE(risk_.check(spec(crude, oms::Side::Buy, 100), crude, quote, view(), at(9, 5)).has_value());
    EXPECT_EQ(code(spec(crude, oms::Side::Buy, 50), crude, quote, view(), at(21, 0)), RejectCode::BadQuantity);
    // Opening orders stop 5 minutes before the close (as 15:25 on NSE); the market at 23:30.
    EXPECT_EQ(code(spec(crude, oms::Side::Buy, 100), crude, quote, view(), at(23, 25)), RejectCode::OutsideWindow);
    const auto closed = risk_.check(spec(crude, oms::Side::Buy, 100), crude, quote, view(), at(23, 30));
    ASSERT_FALSE(closed.has_value());
    EXPECT_EQ(closed.error().code, RejectCode::MarketClosed);
    EXPECT_EQ(closed.error().message, "the market is closed (09:00–23:30)");
    // An NSE future stays 09:15–15:30.
    EXPECT_EQ(code(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, view(), at(21, 0)),
              RejectCode::MarketClosed);
}

TEST_F(RiskTest, ConversionsNeedTheMarketAndIntoMisTheWindow) {
    EXPECT_TRUE(risk_.check_convert(kFuture, oms::Product::Nrml, 75, kTen).has_value());
    EXPECT_TRUE(risk_.check_convert(kFuture, oms::Product::Mis, 150, kTen).has_value());
    EXPECT_EQ(risk_.check_convert(kFuture, oms::Product::Nrml, 50, kTen).error().code, RejectCode::BadQuantity);
    // After the square-off (15:25) only out of MIS; after the close, nothing.
    EXPECT_EQ(risk_.check_convert(kFuture, oms::Product::Mis, 75, at(15, 25)).error().code,
              RejectCode::OutsideWindow);
    EXPECT_TRUE(risk_.check_convert(kFuture, oms::Product::Nrml, 75, at(15, 27)).has_value());
    EXPECT_EQ(risk_.check_convert(kFuture, oms::Product::Nrml, 75, at(15, 30)).error().code,
              RejectCode::MarketClosed);
}

TEST_F(RiskTest, OrderRatePerSourcePerMinute) {
    const auto v = view(100 * kLakh);
    const auto automatic = spec(kFuture, oms::Side::Buy, 75, oms::OrderType::Market, 0, 0, oms::Source::Auto);
    for (int i = 0; i < 30; ++i) {
        ASSERT_TRUE(risk_.check(automatic, kFuture, kFutureQuote, v, kTen + i).has_value()) << i;
    }
    EXPECT_EQ(code(automatic, kFuture, kFutureQuote, v, kTen + 40), RejectCode::OrderRate);
    // The person's own orders are counted separately.
    const auto person = risk_.check(spec(kFuture, oms::Side::Buy, 75), kFuture, kFutureQuote, v, kTen + 40);
    EXPECT_TRUE(person.has_value()) << person.error().message;
    const auto later = risk_.check(automatic, kFuture, kFutureQuote, v, kTen + 60'001);
    EXPECT_TRUE(later.has_value()) << later.error().message;
    EXPECT_EQ(name(RejectCode::OrderRate), "order_rate");
}

}  // namespace
}  // namespace mda::rms
