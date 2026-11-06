#pragma once

// One paper account (docs/design/paper-trading.md §3, §6): its positions and a ledger of
// every cash movement. P&L accounting: a closing fill credits the realised P&L and every
// fill debits its charges; option premium and futures margin are blocked, not paid (rms,
// PT2). Cash is always the sum of the ledger.

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>
#include <mda/oms/charges.hpp>
#include <mda/oms/position.hpp>
#include <mda/oms/types.hpp>

namespace mda::oms {

/// What the account needs to know about an instrument.
struct Contract {
    core::InstrumentId id;
    Segment segment = Segment::Future;
    Exchange exchange = Exchange::Nse;
    std::int64_t lot_size = 1;
    core::Price strike;  ///< options
    bool call = false;   ///< options
};

enum class LedgerKind : std::uint8_t {
    Opening = 1,  ///< the account's starting cash
    Pnl = 2,      ///< realised by a closing fill
    Charges = 3,  ///< of a fill
    Expiry = 4,   ///< settlement of a position at expiry (P&L and exercise STT)
    Mtm = 5,      ///< a futures position marked to the day's closing price (as Zerodha's daily MTM)
};

[[nodiscard]] std::string_view name(LedgerKind kind) noexcept;

struct LedgerEntry {
    std::int64_t at = 0;
    LedgerKind kind = LedgerKind::Opening;
    Paise amount = 0;           ///< credit positive, debit negative
    std::int64_t order_id = 0;  ///< the order it came from; 0 when none
    core::InstrumentId instrument;
};

struct PositionKey {
    core::InstrumentId instrument;
    Product product = Product::Mis;

    friend auto operator<=>(const PositionKey&, const PositionKey&) = default;
};

struct FillResult {
    ChargeBreakdown charges;
    Paise realised = 0;        ///< from the average: what reaches cash
    Paise entry_realised = 0;  ///< from the entry price, before any daily mark: what the trader made
};

class Account {
public:
    Account(std::int64_t id, Paise opening_cash, std::int64_t at);
    /// As stored (paper.db): its whole ledger and its positions.
    [[nodiscard]] static Account restore(std::int64_t id, std::vector<LedgerEntry> ledger,
                                         std::map<PositionKey, Position> positions);

    /// Books a fill: position, realised P&L and charges. `order_brokerage` is the brokerage
    /// already charged to the order (the cap is per executed order).
    FillResult apply_fill(const ChargeRates& rates, const Contract& contract, Product product,
                          Side side, std::int64_t quantity, core::Price price, Paise order_brokerage,
                          std::int64_t order_id, std::int64_t at);

    /// Settles every open position in `contract` at the final settlement price `fsp` (the
    /// index's close on expiry day): futures at fsp, options at their intrinsic value, with
    /// STT on bought options in the money. Returns the cash it moved.
    Paise settle_expiry(const ChargeRates& rates, const Contract& contract, core::Price fsp,
                        std::int64_t at);

    /// Moves `quantity` of the position in `instrument` from product `from` to `to` (MIS ↔
    /// NRML, as Zerodha's position conversion) at its average cost: no charges and no P&L,
    /// unless it nets against an opposite position in `to`. Returns the P&L realised, or
    /// nullopt if `from` holds less than `quantity`.
    std::optional<Paise> convert(core::InstrumentId instrument, Product from, Product to,
                                 std::int64_t quantity, std::int64_t at);

    /// Closes the position in (`instrument`, `product`) at `price` without an order or charges:
    /// an MIS square-off the broker missed while it was down, caught up at that day's close.
    /// Returns the P&L realised.
    Paise close_at(core::InstrumentId instrument, Product product, core::Price price, std::int64_t at);

    /// The time of the latest ledger entry for `instrument` (a fill, MTM or settlement); 0 if none.
    [[nodiscard]] std::int64_t last_activity(core::InstrumentId instrument) const noexcept;
    /// The time of the latest fill in `instrument` (its charges or P&L, not an MTM); 0 if none.
    [[nodiscard]] std::int64_t last_fill(core::InstrumentId instrument) const noexcept;

    /// Daily mark-to-market (Zerodha's MTM settlement): every open position in the future
    /// `instrument` settles its P&L against `close` into cash and carries on from `close` as
    /// its average. Returns the cash moved; 0 again for the same close.
    Paise mark_to_market(core::InstrumentId instrument, core::Price close, std::int64_t at);

    [[nodiscard]] std::int64_t id() const noexcept { return id_; }
    [[nodiscard]] Paise cash() const noexcept { return cash_; }
    [[nodiscard]] const std::map<PositionKey, Position>& positions() const noexcept { return positions_; }
    /// The position, or nullptr if the account never traded it.
    [[nodiscard]] const Position* position(core::InstrumentId instrument, Product product) const;
    [[nodiscard]] const std::vector<LedgerEntry>& ledger() const noexcept { return ledger_; }

private:
    Account() = default;
    void book(LedgerKind kind, Paise amount, std::int64_t order_id, core::InstrumentId instrument,
              std::int64_t at);

    std::int64_t id_ = 0;
    Paise cash_ = 0;
    std::map<PositionKey, Position> positions_;
    std::vector<LedgerEntry> ledger_;
};

}  // namespace mda::oms
