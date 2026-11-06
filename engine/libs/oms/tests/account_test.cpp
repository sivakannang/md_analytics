#include <gtest/gtest.h>

#include <mda/oms/account.hpp>

namespace mda::oms {
namespace {

core::Price rupees(double value) {
    return core::Price::from_raw(static_cast<std::int64_t>(value * core::Price::kScale + 0.5));
}

const ChargeRates kRates = zerodha_2026_10();
constexpr Paise kOpening = 1'000'000 * 100;  // ₹10 lakh

const Contract kFuture{.id = core::InstrumentId(1), .segment = Segment::Future, .exchange = Exchange::Nse,
                       .lot_size = 75, .strike = {}, .call = false};

Contract option(std::uint32_t id, double strike, bool call) {
    return {.id = core::InstrumentId(id), .segment = Segment::Option, .exchange = Exchange::Nse,
            .lot_size = 75, .strike = rupees(strike), .call = call};
}

TEST(Account, OpensWithItsCash) {
    const Account account(5, kOpening, 100);
    EXPECT_EQ(account.cash(), kOpening);
    ASSERT_EQ(account.ledger().size(), 1u);
    EXPECT_EQ(account.ledger()[0].kind, LedgerKind::Opening);
}

TEST(Account, FuturesRoundTrip) {
    Account account(5, kOpening, 100);
    const auto buy = account.apply_fill(kRates, kFuture, Product::Mis, Side::Buy, 75,
                                        rupees(22'517.35), 0, 11, 200);
    EXPECT_EQ(buy.realised, 0);
    EXPECT_EQ(buy.charges.total(), 9'607);
    EXPECT_EQ(account.cash(), kOpening - 9'607);
    EXPECT_EQ(account.position(kFuture.id, Product::Mis)->quantity(), 75);

    const auto sell = account.apply_fill(kRates, kFuture, Product::Mis, Side::Sell, 75,
                                         rupees(22'603.85), 0, 12, 300);
    EXPECT_EQ(sell.realised, 648'750);  // ₹86.50 × 75
    EXPECT_EQ(account.cash(), kOpening + 648'750 - 9'607 - 91'022);  // net ₹5,481.21
    EXPECT_EQ(account.position(kFuture.id, Product::Mis)->quantity(), 0);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml), nullptr);

    // Cash is the sum of the ledger.
    Paise sum = 0;
    for (const auto& entry : account.ledger()) {
        sum += entry.amount;
    }
    EXPECT_EQ(sum, account.cash());
    EXPECT_EQ(account.ledger().back().order_id, 12);
}

TEST(Account, ProductsAreSeparatePositions) {
    Account account(5, kOpening, 100);
    account.apply_fill(kRates, kFuture, Product::Mis, Side::Buy, 75, rupees(22'500), 0, 1, 200);
    account.apply_fill(kRates, kFuture, Product::Nrml, Side::Sell, 75, rupees(22'510), 0, 2, 200);
    EXPECT_EQ(account.position(kFuture.id, Product::Mis)->quantity(), 75);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->quantity(), -75);
}

TEST(Account, ConvertMovesAPositionBetweenProductsAtItsAverage) {
    Account account(5, kOpening, 100);
    account.apply_fill(kRates, kFuture, Product::Mis, Side::Buy, 75, rupees(22'500), 0, 1, 200);
    account.apply_fill(kRates, kFuture, Product::Mis, Side::Buy, 75, rupees(22'510), 0, 2, 200);
    const Paise cash = account.cash();
    EXPECT_EQ(account.convert(kFuture.id, Product::Mis, Product::Nrml, 75, 300), 0);  // half
    EXPECT_EQ(account.cash(), cash);                                                 // no P&L, no charges
    EXPECT_EQ(account.position(kFuture.id, Product::Mis)->quantity(), 75);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->quantity(), 75);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->average_price(), rupees(22'505));
    EXPECT_EQ(account.position(kFuture.id, Product::Mis)->average_price(), rupees(22'505));
    EXPECT_EQ(account.convert(kFuture.id, Product::Mis, Product::Nrml, 150, 300), std::nullopt);  // only 75 left
    EXPECT_EQ(account.convert(kFuture.id, Product::Mis, Product::Nrml, 75, 300), 0);
    EXPECT_EQ(account.position(kFuture.id, Product::Mis)->quantity(), 0);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->quantity(), 150);
    // Back, then against an opposite NRML short: it nets, realising against that short.
    EXPECT_EQ(account.convert(kFuture.id, Product::Nrml, Product::Mis, 150, 400), 0);
    account.apply_fill(kRates, kFuture, Product::Nrml, Side::Sell, 75, rupees(22'525), 0, 3, 500);
    const Paise before = account.cash();
    EXPECT_EQ(account.convert(kFuture.id, Product::Mis, Product::Nrml, 75, 600), 150'000);  // (22,525 − 22,505) × 75
    EXPECT_EQ(account.cash(), before + 150'000);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->quantity(), 0);
}

TEST(Account, DailyMtmSettlesIntoCashAndResetsTheAverage) {
    Account account(5, kOpening, 100);
    account.apply_fill(kRates, kFuture, Product::Nrml, Side::Buy, 75, rupees(22'500), 0, 1, 200);
    const Paise cash = account.cash();
    EXPECT_EQ(account.mark_to_market(kFuture.id, rupees(22'540), 300), 300'000);  // ₹40 × 75
    EXPECT_EQ(account.cash(), cash + 300'000);
    EXPECT_EQ(account.ledger().back().kind, LedgerKind::Mtm);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->average_price(), rupees(22'540));
    EXPECT_EQ(account.mark_to_market(kFuture.id, rupees(22'540), 400), 0);  // again: nothing
    // Next day it closes at 22,520: ₹20 × 75 from the new average, not the first fill.
    account.apply_fill(kRates, kFuture, Product::Nrml, Side::Sell, 75, rupees(22'520), 0, 2, 500);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->quantity(), 0);
    EXPECT_EQ(account.position(kFuture.id, Product::Nrml)->realised(), 300'000 - 150'000);
}

TEST(Account, ExpirySettlesAtIntrinsicValue) {
    Account account(5, kOpening, 100);
    const auto itm_call = option(2, 22'500, true);
    const auto otm_put = option(3, 22'000, false);
    account.apply_fill(kRates, itm_call, Product::Nrml, Side::Buy, 75, rupees(100), 0, 1, 200);
    account.apply_fill(kRates, otm_put, Product::Nrml, Side::Buy, 75, rupees(20), 0, 2, 200);
    account.apply_fill(kRates, kFuture, Product::Nrml, Side::Sell, 75, rupees(22'500), 0, 3, 200);
    const Paise before = account.cash();

    // Final settlement price: the index closes at 22,600.
    // Call: intrinsic ₹100 = what was paid, so no P&L; exercise STT 0.15% of ₹7,500 → ₹11.
    EXPECT_EQ(account.settle_expiry(kRates, itm_call, rupees(22'600), 900), -1'100);
    // Put: expires worthless, the premium (₹20 × 75) is lost.
    EXPECT_EQ(account.settle_expiry(kRates, otm_put, rupees(22'600), 900), -150'000);
    // Short future: settles at 22,600, ₹100 × 75 against it; no charges.
    EXPECT_EQ(account.settle_expiry(kRates, kFuture, rupees(22'600), 900), -750'000);

    EXPECT_EQ(account.cash(), before - 1'100 - 150'000 - 750'000);
    for (const auto& [key, position] : account.positions()) {
        EXPECT_EQ(position.quantity(), 0) << key.instrument.value();
    }
    EXPECT_EQ(account.ledger().back().kind, LedgerKind::Expiry);
    EXPECT_EQ(name(LedgerKind::Expiry), "expiry");
}

}  // namespace
}  // namespace mda::oms
