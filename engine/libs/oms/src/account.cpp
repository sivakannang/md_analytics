#include <mda/oms/account.hpp>

#include <algorithm>
#include <cstdlib>

namespace mda::oms {

std::string_view name(LedgerKind kind) noexcept {
    switch (kind) {
    case LedgerKind::Opening:
        return "opening";
    case LedgerKind::Pnl:
        return "pnl";
    case LedgerKind::Charges:
        return "charges";
    case LedgerKind::Expiry:
        return "expiry";
    case LedgerKind::Mtm:
        return "mtm";
    }
    return "?";
}

Account::Account(std::int64_t id, Paise opening_cash, std::int64_t at) : id_(id) {
    book(LedgerKind::Opening, opening_cash, 0, {}, at);
}

Account Account::restore(std::int64_t id, std::vector<LedgerEntry> ledger,
                         std::map<PositionKey, Position> positions) {
    Account account;
    account.id_ = id;
    for (const auto& entry : ledger) {
        account.cash_ += entry.amount;
    }
    account.ledger_ = std::move(ledger);
    account.positions_ = std::move(positions);
    return account;
}

void Account::book(LedgerKind kind, Paise amount, std::int64_t order_id,
                   core::InstrumentId instrument, std::int64_t at) {
    if (amount == 0 && kind != LedgerKind::Opening) {
        return;
    }
    cash_ += amount;
    ledger_.push_back({.at = at, .kind = kind, .amount = amount, .order_id = order_id, .instrument = instrument});
}

FillResult Account::apply_fill(const ChargeRates& rates, const Contract& contract, Product product,
                               Side side, std::int64_t quantity, core::Price price,
                               Paise order_brokerage, std::int64_t order_id, std::int64_t at) {
    FillResult result;
    result.charges = fill_charges(rates, {.segment = contract.segment,
                                          .exchange = contract.exchange,
                                          .side = side,
                                          .quantity = quantity,
                                          .price = price,
                                          .order_brokerage = order_brokerage});
    Position& position = positions_[{.instrument = contract.id, .product = product}];
    result.entry_realised = position.entry_pnl(side, quantity, price);
    result.realised = position.apply(side, quantity, price);
    book(LedgerKind::Pnl, result.realised, order_id, contract.id, at);
    book(LedgerKind::Charges, -result.charges.total(), order_id, contract.id, at);
    return result;
}

Paise Account::settle_expiry(const ChargeRates& rates, const Contract& contract, core::Price fsp,
                             std::int64_t at) {
    Paise moved = 0;
    for (auto& [key, position] : positions_) {
        if (key.instrument != contract.id || position.quantity() == 0) {
            continue;
        }
        const std::int64_t quantity = std::abs(position.quantity());
        const Side closing = position.quantity() > 0 ? Side::Sell : Side::Buy;
        core::Price settle = fsp;
        Paise stt = 0;
        if (contract.segment == Segment::Option) {
            const std::int64_t intrinsic_raw =
                std::max<std::int64_t>(0, contract.call ? fsp.raw() - contract.strike.raw()
                                                        : contract.strike.raw() - fsp.raw());
            settle = core::Price::from_raw(intrinsic_raw);
            if (position.quantity() > 0 && contract.exchange != Exchange::Mcx) {  // MCX: no tax
                stt = exercise_stt(rates, value_paise(settle, quantity));
            }
        }
        const Paise pnl = position.apply(closing, quantity, settle);
        book(LedgerKind::Expiry, pnl - stt, 0, contract.id, at);
        moved += pnl - stt;
    }
    return moved;
}

std::optional<Paise> Account::convert(core::InstrumentId instrument, Product from, Product to,
                                      std::int64_t quantity, std::int64_t at) {
    const auto source = positions_.find({.instrument = instrument, .product = from});
    if (from == to || quantity <= 0 || source == positions_.end() ||
        std::abs(source->second.quantity()) < quantity) {
        return std::nullopt;
    }
    const std::int64_t direction = source->second.quantity() > 0 ? 1 : -1;
    const Position::Taken taken = source->second.take(quantity);
    const Paise realised = positions_[{.instrument = instrument, .product = to}].put(direction * quantity, taken);
    book(LedgerKind::Pnl, realised, 0, instrument, at);
    return realised;
}

Paise Account::close_at(core::InstrumentId instrument, Product product, core::Price price, std::int64_t at) {
    const auto found = positions_.find({.instrument = instrument, .product = product});
    if (found == positions_.end() || found->second.quantity() == 0) {
        return 0;
    }
    const std::int64_t quantity = found->second.quantity();
    const Paise pnl = found->second.apply(quantity > 0 ? Side::Sell : Side::Buy, std::abs(quantity), price);
    book(LedgerKind::Pnl, pnl, 0, instrument, at);
    return pnl;
}

std::int64_t Account::last_activity(core::InstrumentId instrument) const noexcept {
    std::int64_t latest = 0;
    for (const auto& entry : ledger_) {
        if (entry.instrument == instrument && entry.kind != LedgerKind::Opening) {
            latest = std::max(latest, entry.at);
        }
    }
    return latest;
}

std::int64_t Account::last_fill(core::InstrumentId instrument) const noexcept {
    std::int64_t latest = 0;
    for (const auto& entry : ledger_) {
        if (entry.instrument == instrument && (entry.kind == LedgerKind::Pnl || entry.kind == LedgerKind::Charges)) {
            latest = std::max(latest, entry.at);
        }
    }
    return latest;
}

Paise Account::mark_to_market(core::InstrumentId instrument, core::Price close, std::int64_t at) {
    Paise moved = 0;
    for (auto& [key, position] : positions_) {
        if (key.instrument == instrument && position.quantity() != 0) {
            const Paise pnl = position.mark(close);
            book(LedgerKind::Mtm, pnl, 0, instrument, at);
            moved += pnl;
        }
    }
    return moved;
}

const Position* Account::position(core::InstrumentId instrument, Product product) const {
    const auto found = positions_.find({.instrument = instrument, .product = product});
    return found == positions_.end() ? nullptr : &found->second;
}

}  // namespace mda::oms
