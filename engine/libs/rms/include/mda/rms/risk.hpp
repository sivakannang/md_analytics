#pragma once

// Order validation and pre-trade risk (docs/design/paper-trading.md §4, §8, §9). Every order
// from every source passes `RiskManager::check` before the OMS accepts it:
//
// 1. Validation, as an exchange would: a listed, unexpired future or option; quantity a
//    positive multiple of the lot and within the freeze quantity; prices on the tick and
//    within the exchange's circuit limits; an SL trigger on the right side of the LTP; no
//    option writing (v1); market hours.
// 2. Reducing orders (they only shrink a position) pass the kill switch, the daily loss
//    limit and the trading window, and need no margin.
// 3. Opening orders: the trading window; the kill switch and the daily loss limit; lots per
//    order and per instrument; gross notional; the price band; margin within available
//    funds.
// 4. All orders: the order rate.
//
// Limits differ by source: an automatic trader gets stricter ones than a person. A person's
// match Zerodha's (2026-10-27): no lot or notional caps and no price band beyond the circuit
// limits; margin and the daily loss limit are the checks. Orders above the freeze quantity
// are sliced by the broker, not here.
//
// MCX (ADR-0031): the instrument carries its own market hours (09:00 to 23:30 / 23:55) and
// futures margin; the trading window keeps the same distance from that open and close as it
// has from 09:15 and 15:30.

#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <mda/oms/account.hpp>
#include <mda/oms/charges.hpp>
#include <mda/oms/order.hpp>
#include <mda/oms/types.hpp>

namespace mda::rms {

using oms::Millis;
using oms::Paise;
using oms::Ppb;

/// 0 turns a cap off.
struct Limits {
    std::int64_t max_lots_per_order = 0;
    std::int64_t max_open_lots_per_instrument = 0;
    Paise max_gross_notional = 0;
    Ppb price_band = 0;                          ///< LIMIT / SL price within ±x of the LTP
    int orders_per_minute = 30;
    Ppb daily_loss = 50'000'000;                 ///< 5% of the day's opening cash
    int window_start = 9 * 3600 + 15 * 60;       ///< seconds after 00:00 IST (NSE/BSE; an
    int window_end = 15 * 3600 + 25 * 60;        ///< MCX window keeps the distance from its
                                                 ///< open/close); after this, reducing only
};

/// The defaults for an automatic trader (§9).
[[nodiscard]] Limits auto_limits() noexcept;

struct RiskConfig {
    Limits ui;
    Limits automatic = auto_limits();
    Ppb futures_margin = 120'000'000;        ///< index futures: 12% of notional
    Ppb stock_futures_margin = 200'000'000;  ///< stock futures: 20% (Zerodha's SPAN + exposure, ~15–40%)
    Ppb market_buffer = 20'000'000;    ///< MARKET option buys block 2% above the ask
    int market_open = 9 * 3600 + 15 * 60;
    int market_close = 15 * 3600 + 30 * 60;
};

/// What risk needs to know about the instrument.
struct InstrumentInfo {
    oms::Contract contract;
    core::Price tick;
    std::int64_t freeze_quantity = 0;  ///< 0: no limit known
    bool listed = true;
    std::int32_t expiry_days = 0;      ///< days since 1970-01-01
    core::InstrumentId underlying;     ///< the index or stock it settles on; invalid if unknown
    bool index = false;                ///< on an index (else a stock): its futures margin
    Ppb futures_margin = 0;            ///< MCX: the commodity's margin; 0 = RiskConfig's
    int market_open = 0;               ///< MCX: seconds after 00:00 IST; 0 = RiskConfig's
    int market_close = 0;
};

/// The market as risk sees it: zero prices are unknown.
struct Quote {
    core::Price ltp;
    core::Price best_ask;
    core::Price best_bid;
    core::Price lower_circuit;  ///< the exchange's price limits (mda-feedd, every 60 s)
    core::Price upper_circuit;
};

/// The account's state, kept by the OMS.
struct AccountView {
    const oms::Account* account = nullptr;
    Paise blocked = 0;              ///< margin of open orders and positions
    Paise unrealised = 0;           ///< at the LTP, all positions
    Paise day_pnl = 0;              ///< realised today + unrealised
    Paise opening_cash = 0;         ///< cash at the start of the day
    Paise gross_notional = 0;       ///< of all open positions at the LTP
    Paise auto_notional = 0;        ///< the auto trader's share of them: its notional limit
    std::int64_t auto_position = 0; ///< the auto trader's share of the position in this instrument
    std::int64_t open_order_quantity = 0;  ///< same instrument and side, still to fill
    bool kill_switch = false;
};

enum class RejectCode : std::uint8_t {
    NotTradable = 1,
    BadQuantity = 2,
    FreezeQuantity = 3,
    BadPrice = 4,
    BadTrigger = 5,
    OptionWriting = 6,
    MarketClosed = 7,
    OutsideWindow = 8,
    KillSwitch = 9,
    DailyLoss = 10,
    OrderSize = 11,
    PositionSize = 12,
    Notional = 13,
    PriceBand = 14,
    NoPrice = 15,
    Funds = 16,
    OrderRate = 17,
};

[[nodiscard]] std::string_view name(RejectCode code) noexcept;

struct Rejection {
    RejectCode code = RejectCode::NotTradable;
    std::string message;  ///< for the user
};

struct Approval {
    bool reducing = false;
    Paise margin = 0;  ///< to block while the order is open (0 for reducing orders)
};

class RiskManager {
public:
    explicit RiskManager(RiskConfig config = {}) : config_(std::move(config)) {}

    /// Checks one new order. On approval the order counts toward the order rate.
    [[nodiscard]] std::expected<Approval, Rejection> check(const oms::OrderSpec& order,
                                                           const InstrumentInfo& instrument,
                                                           const Quote& quote,
                                                           const AccountView& view, Millis now);

    /// Margin an open position or order of `quantity` blocks at `price`.
    /// A position conversion to `to` (MIS ↔ NRML): a listed contract, whole lots, the
    /// market open; into MIS only before the person's trading window ends (the square-off).
    /// Margin doesn't change: both products block the same.
    [[nodiscard]] std::expected<void, Rejection> check_convert(const InstrumentInfo& instrument, oms::Product to,
                                                               std::int64_t quantity, Millis now) const;

    [[nodiscard]] Paise margin(const InstrumentInfo& instrument, oms::Side side,
                               std::int64_t quantity, core::Price price) const noexcept;

    /// The global kill switch (admin): every account may only reduce.
    void set_global_kill_switch(bool on) noexcept { global_kill_ = on; }
    [[nodiscard]] bool global_kill_switch() const noexcept { return global_kill_; }

    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] const Limits& limits(oms::Source source) const noexcept;
    [[nodiscard]] bool rate_ok(std::int64_t account, oms::Source source, Millis now);

    RiskConfig config_;
    bool global_kill_ = false;
    std::map<std::pair<std::int64_t, oms::Source>, std::deque<Millis>> recent_;  ///< order times
};

}  // namespace mda::rms
