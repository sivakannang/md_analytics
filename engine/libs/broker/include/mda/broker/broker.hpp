#pragma once

// The paper broker's core (ADR-0029, docs/design/paper-trading.md §2–§9): one paper
// account per user, the OMS, pre-trade risk (rms) and the paper venue. A single-threaded
// state machine: mda-broker feeds it market data, order requests and timer ticks; every
// input that changes anything is saved to paper.db in one transaction before its execution
// reports are returned for publishing. Given the same inputs it produces the same state,
// and `load()` resumes exactly where a stopped broker left off.

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mda/broker/messages.hpp>
#include <mda/broker/paper_store.hpp>
#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/oms/account.hpp>
#include <mda/oms/charges.hpp>
#include <mda/oms/order.hpp>
#include <mda/oms/paper_venue.hpp>
#include <mda/rms/risk.hpp>

namespace mda::broker {

using oms::Millis;
using oms::Paise;

/// The catalog's view of an instrument; nullopt if unknown.
using InstrumentLookup = std::function<std::optional<rms::InstrumentInfo>(core::InstrumentId)>;

/// The close of the day (docs/design/paper-trading.md §6), seconds after 00:00 IST, for NSE
/// and BSE. MCX (ADR-0031) runs the same steps at the same distance from its own close
/// (23:30 / 23:55): MIS square-off 5 minutes before, settlement and the daily mark-to-market a
/// minute after.
struct CloseOfDay {
    int mis_square_off = 15 * 3600 + 25 * 60;  ///< MIS orders cancelled, MIS positions closed
    int close = 15 * 3600 + 30 * 60;           ///< DAY orders expire
    int settle = 15 * 3600 + 31 * 60;          ///< positions in contracts expiring today settle
    int mark_to_market = 15 * 3600 + 41 * 60;  ///< open futures marked to the close (after F&O's 15:40)
};

struct BrokerConfig {
    Paise opening_cash = 10'00'000 * 100;  ///< ₹10 lakh per new account
    oms::VenueConfig venue;
    rms::RiskConfig risk;
    oms::ChargeRates rates = oms::zerodha_2026_10();
    CloseOfDay close_of_day;
};

/// The final settlement price of contracts on `underlying` expiring on `expiry_day` (days
/// since 1970-01-01): the underlying's closing value that day; nullopt until known. An MCX
/// future settles on its own close; an MCX option on its future's.
using FspLookup = std::function<std::optional<core::Price>(core::InstrumentId underlying, std::int32_t expiry_day)>;

/// An instrument's last close before `before` (Unix ms) and when that minute ended; nullopt
/// if none. For an MIS square-off missed while the broker was down.
struct PreviousClose {
    core::Price close;
    Millis ended = 0;
};
using PreviousCloseLookup = std::function<std::optional<PreviousClose>(core::InstrumentId, Millis before)>;

/// One account's day, for the daily summary.
struct DailySummary {
    std::int64_t user = 0;
    Paise realised = 0;  ///< closing fills and expiry settlement
    Paise charges = 0;
    std::int64_t fills = 0;
    std::int64_t open_positions = 0;
    Paise cash = 0;
};

using Reports = std::vector<ExecutionReport>;

class Broker {
public:
    Broker(BrokerConfig config, PaperStore& store, InstrumentLookup lookup, FspLookup fsp = {},
           PreviousCloseLookup previous_close = {});

    /// Restores accounts, positions and live orders from paper.db, and the auto trader's
    /// share of the positions from the orders of the IST day of `now` (0: none).
    [[nodiscard]] std::expected<void, DbError> load(Millis now = 0);

    // Market data from the md bus: updates the books, never publishes.
    void on_snapshot(const messages::MarketSnapshot& snapshot);
    void on_depth(const messages::DepthSnapshot& depth);
    void on_feed(const messages::FeedStatus& status);
    /// The exchange's price limits (mda-feedd re-publishes them every 60 s).
    void on_circuit(const messages::CircuitLimits& limits);

    /// A request from a source; its reports (persisted first).
    [[nodiscard]] std::expected<Reports, DbError> on_request(const OrderRequest& request, Millis now);
    /// Runs the close of the day when due, then the paper venue over every live order;
    /// reports for what changed.
    [[nodiscard]] std::expected<Reports, DbError> on_tick(Millis now);

    /// Every account that traded or holds positions on the IST day of `now`.
    [[nodiscard]] std::vector<DailySummary> daily_summaries(Millis now) const;

    [[nodiscard]] const oms::Account* account_of_user(std::int64_t user) const;
    [[nodiscard]] const oms::Order* live_order(std::int64_t order_id) const;
    [[nodiscard]] std::size_t live_orders() const noexcept { return live_.size(); }
    /// Accounts rows `load()` had to rebuild (PaperStore::lost_accounts).
    [[nodiscard]] std::size_t restored_accounts() const noexcept { return restored_accounts_; }
    [[nodiscard]] bool kill_switch(std::int64_t user) const;
    [[nodiscard]] rms::RiskManager& risk() noexcept { return risk_; }

private:
    struct AccountState {
        AccountRow row;
        oms::Account account;
        std::size_t saved_ledger = 0;  ///< ledger entries already in paper.db
    };
    struct LiveOrder {
        oms::Order order;
        std::int64_t user = 0;
        Paise margin = 0;  ///< for the whole quantity
    };
    /// What one input changed, written in one transaction.
    struct Changes {
        std::set<std::int64_t> orders;  ///< live order ids to save
        std::vector<OrderRow> final_orders;
        std::vector<TradeRow> trades;
        std::set<std::pair<std::int64_t, oms::PositionKey>> positions;
        std::set<std::int64_t> accounts;
        std::vector<std::pair<std::string, std::string>> events;
    };

    AccountState& ensure_account(std::int64_t user, Millis now, Changes& changes);
    [[nodiscard]] rms::AccountView view_of(const AccountState& state, core::InstrumentId instrument,
                                           oms::Side side, std::int64_t except_order, Millis now) const;
    [[nodiscard]] oms::Book book_of(core::InstrumentId instrument, const rms::InstrumentInfo& info) const;
    [[nodiscard]] rms::Quote quote_of(core::InstrumentId instrument) const;
    [[nodiscard]] ExecutionReport report(const oms::Order& order, std::int64_t user, ReportEvent event,
                                         Millis now) const;
    [[nodiscard]] OrderRow row_of(const oms::Order& order, std::int64_t user, Paise margin) const;
    void finish(std::int64_t order_id, Changes& changes);
    void apply_fill(LiveOrder& live, const rms::InstrumentInfo& info, const oms::Execution& fill, Millis now,
                    Changes& changes, Reports& reports);
    [[nodiscard]] std::expected<void, DbError> save(Changes& changes, Millis now);
    void close_of_day(Millis now, Changes& changes, Reports& reports);
    /// Whether an instrument trades on MCX (its close of day is MCX's).
    [[nodiscard]] bool on_mcx(core::InstrumentId instrument) const;
    void square_off_mis(std::int32_t today, bool mcx, Millis now, Changes& changes, Reports& reports);
    /// Cancels live orders created before `before`; `mcx` limits it to one side's orders.
    void expire_orders(Millis before, std::optional<bool> mcx, std::string_view reason, Millis now, Changes& changes,
                       Reports& reports);
    /// Zerodha's daily MTM: open futures (not expiring today) settle their P&L against the
    /// day's last close and carry on from it. `due[mcx]`: that exchange's time has passed.
    /// Idempotent: a position already at the close is left alone.
    void mark_to_market(std::int32_t today, std::array<bool, 2> due, Millis now, Changes& changes);
    /// An MIS square-off missed while the broker was down (no fill, MTM or settlement since
    /// the instrument's last close before today): closed at that close, no charges. Safe to
    /// run any number of times.
    void catch_up(Millis now, Changes& changes, Reports& reports);
    /// Open futures with no fill today settle against Kite's previous close, seen on the
    /// live feed during today's session. It corrects a mark the broker missed or made on a
    /// price recorded before the Mac slept (paper-trading.md §6). Idempotent.
    void mark_to_previous_close(std::int32_t today, Millis now, Changes& changes);
    /// Marks every open position in `instrument` to `close`, unless they are all at it already.
    void mark_instrument(std::int64_t account_id, AccountState& state, core::InstrumentId instrument, core::Price close,
                         Millis now, Changes& changes);
    /// `after_close[mcx]`: whether that exchange's settlement time has passed today.
    void settle_expired(std::int32_t today, std::array<bool, 2> after_close, Millis now, Changes& changes,
                        Reports& reports);

    BrokerConfig config_;
    PaperStore* store_;
    InstrumentLookup lookup_;
    oms::PaperVenue venue_;
    rms::RiskManager risk_;
    std::map<std::int64_t, AccountState> accounts_;           ///< by account id
    std::unordered_map<std::int64_t, std::int64_t> by_user_;  ///< user → account id
    std::map<std::int64_t, LiveOrder> live_;                  ///< by order id, oldest first
    std::unordered_map<std::uint32_t, oms::Book> books_;
    std::unordered_map<std::uint32_t, std::pair<core::Price, core::Price>> circuits_;  ///< lower, upper
    /// The auto trader's net filled quantity by account and position on `auto_day_`: its
    /// share of a position, so its limits aren't used up by the user's own trades.
    std::map<std::pair<std::int64_t, oms::PositionKey>, std::int64_t> auto_net_;
    std::int32_t auto_day_ = 0;
    std::size_t restored_accounts_ = 0;
    FspLookup fsp_;
    PreviousCloseLookup previous_close_;
    struct KiteClose {
        core::Price close;
        Millis exchange_at = 0;
    };
    std::unordered_map<std::uint32_t, KiteClose> kite_close_;  ///< by instrument, latest snapshot's
    /// Conversions done, by (user, client order id): a resend gets the same report.
    std::map<std::pair<std::int64_t, std::uint64_t>, ExecutionReport> converted_;
    std::array<std::int32_t, 2> squared_off_day_{};  ///< IST days the close-of-day steps last ran,
    std::array<std::int32_t, 2> closed_day_{};       ///< [0] NSE/BSE, [1] MCX
    Millis next_settle_check_ = 0;
    bool feed_ok_ = true;
    std::int64_t next_order_id_ = 1;
    std::uint64_t sequence_ = 0;
};

}  // namespace mda::broker
