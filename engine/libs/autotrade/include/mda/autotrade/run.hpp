#pragma once

// One auto-trader run of the follow-signal strategy (ADR-0030, docs/design/auto-trader.md
// §4–§6). Pure logic: mda-autotrader feeds it signals, prices, the clock and the execution
// reports of its own orders; it answers with the orders it wants (actions) and a decision
// for every action or blocked entry.
//
//   Flat → Entering (MARKET in) → Long / Short (a resting SL-M at the broker)
//        → Exiting (cancel the stop, MARKET out) → Flat (cooldown)
//
// The run counts its net position from its own fills, so whatever the order of reports
// (a stop filling while an exit is on its way), it ends flat: a position with nothing
// pending to close it gets a closing order.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <mda/broker/messages.hpp>
#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>
#include <mda/oms/types.hpp>
#include <mda/signals/strategy.hpp>

namespace mda::autotrade {

using oms::Millis;
using oms::Paise;

struct RunConfig {
    std::string id;             ///< stable; tags its orders "auto:<id>"
    std::int64_t user = 0;      ///< whose paper account
    std::string signal;         ///< the signald strategy it follows, e.g. oi-bias-v1
    std::string underlying;     ///< e.g. NIFTY: trades its near-month future
    std::int64_t lots = 1;
    oms::Product product = oms::Product::Mis;
    double stop_points = 40;    ///< resting SL-M this far from the entry
    double target_points = 80;  ///< 0 = no target
    int max_hold_min = 45;
    int max_trades_per_day = 6;
    int cooldown_min = 10;
    Paise daily_loss_stop = 5'000 * 100;  ///< P&L after charges today; no entries below −this
    bool enabled = true;
    int entry_start = 9 * 3600 + 20 * 60;  ///< seconds after 00:00 IST
    int entry_end = 15 * 3600;             ///< no new entries from here
    int exit_by = 15 * 3600 + 10 * 60;     ///< flat by here
    int signal_max_age_s = 120;            ///< a signal older than this doesn't open a position
    Millis quote_max_age_ms = 5'000;
    int exit_score = 10;  ///< hysteresis: a long holds while the score is ≥ this (a short ≤ −this)
};

/// The future the run trades.
struct Contract {
    core::InstrumentId id;
    std::int64_t lot_size = 1;
    std::int32_t expiry_days = 0;  ///< days since 1970-01-01
    core::Price tick;
};

/// The near-month future: the nearest expiry on or after `today`, or the next one when the
/// nearest expires today (no new positions in an expiring contract).
[[nodiscard]] std::optional<Contract> choose_future(std::span<const Contract> futures, std::int32_t today);

/// A client order id from the run, the day and the run's order counter: the same inputs give
/// the same id (resends, restarts); the top bit stays clear (the broker uses it).
[[nodiscard]] std::uint64_t client_order_id(std::string_view run_id, std::int32_t day, std::uint32_t counter);

/// One factor's share of a signal's score.
struct FactorPoints {
    signals::FactorId factor = signals::FactorId::FuturesBuildup;
    int points = 0;
};

/// The latest signal for the run's index and strategy.
struct SignalView {
    std::int64_t minute = 0;  ///< minute start, Unix seconds
    int score = 0;
    signals::Label label = signals::Label::Neutral;
    std::vector<FactorPoints> top;  ///< largest share first (the bus carries four)
};

/// A factor's short name for decision reasons ("OI shift", "PCR change", …).
[[nodiscard]] std::string_view factor_label(signals::FactorId factor) noexcept;

/// "Bearish -30", and with `factors` ": OI shift -20, futures vs VWAP -15, …".
[[nodiscard]] std::string describe(const SignalView& signal, bool factors);

/// Whether `signal` still backs an open position. Entries need the label; a position is kept
/// until the label turns opposite or the score falls through `exit_score` (auto-trader.md §5),
/// so a score dipping just under the entry threshold doesn't close it.
[[nodiscard]] bool signal_holds(const SignalView& signal, bool long_side, int exit_score);

/// IST helpers shared by the strategies.
[[nodiscard]] std::int32_t ist_day(Millis now) noexcept;
[[nodiscard]] std::int64_t seconds_ist(Millis now) noexcept;

/// The run's future as the market shows it.
struct Market {
    core::Price ltp;
    Millis quote_at = 0;  ///< when the quote last updated
    bool feed_ok = true;
};

/// An order the run wants sent (New), re-priced (Modify) or cancelled (Cancel).
struct Action {
    broker::Action kind = broker::Action::New;
    oms::Side side = oms::Side::Buy;
    oms::OrderType type = oms::OrderType::Market;
    std::int64_t quantity = 0;
    core::Price price;            ///< LIMIT and SL; Modify: the new price
    core::Price trigger;          ///< SL and SL-M
    std::int64_t order_id = 0;    ///< Cancel / Modify: the broker's order id
    std::uint64_t client_order_id = 0;
    core::InstrumentId instrument;
};

enum class State : std::uint8_t { Flat = 0, Entering = 1, Long = 2, Short = 3, Exiting = 4 };

[[nodiscard]] std::string_view name(State state) noexcept;

/// What the run did or declined to do, and why (recorded in autotrader.db).
struct Decision {
    Millis at = 0;
    State before = State::Flat;
    std::string action;  ///< "enter long", "place stop", "exit", "blocked", …
    std::string reason;  ///< "Bullish +38", "stop hit", "cooldown", …
    std::uint64_t client_order_id = 0;
    core::Price price;
};

struct Output {
    std::vector<Action> actions;
    std::vector<Decision> decisions;
};

class Run {
public:
    explicit Run(RunConfig config) : config_(std::move(config)) {}

    /// The contract to trade (from the catalog; changes at expiry). Only while flat.
    void set_contract(const Contract& contract);
    void on_signal(const SignalView& signal) { signal_ = signal; }
    void set_kill_switch(bool on) { kill_switch_ = on; }

    /// A report of one of the run's own orders (others are ignored); may want orders too
    /// (cancel a stop once acknowledged, exit when the broker drops the stop).
    [[nodiscard]] Output on_report(const broker::ExecutionReport& report, Millis now);
    /// What the run wants now, given the market.
    [[nodiscard]] Output evaluate(const Market& market, Millis now);

    /// After a restart: the position the run holds (from paper.db), its entry and its
    /// resting stop's broker order id (0 if none); today's counters (from autotrader.db).
    void restore(std::int64_t position, core::Price entry, Millis entered_at, std::int64_t stop_order_id,
                 std::uint64_t stop_client_id);
    void restore_day(std::int32_t day, int entries, Paise realised, Paise charges, Millis last_exit,
                     std::uint32_t counter);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::int64_t position() const noexcept { return position_; }
    [[nodiscard]] const RunConfig& config() const noexcept { return config_; }
    [[nodiscard]] int entries_today() const noexcept { return entries_; }
    [[nodiscard]] Paise pnl_today() const noexcept { return realised_ - charges_; }
    [[nodiscard]] std::uint32_t counter() const noexcept { return counter_; }
    [[nodiscard]] std::int32_t day() const noexcept { return day_; }
    [[nodiscard]] Paise realised_today() const noexcept { return realised_; }
    [[nodiscard]] Paise charges_today() const noexcept { return charges_; }
    [[nodiscard]] Millis last_exit() const noexcept { return last_exit_; }
    [[nodiscard]] const std::optional<Contract>& contract() const noexcept { return contract_; }

private:
    [[nodiscard]] std::optional<std::string> blocked(const Market& market, Millis now) const;
    [[nodiscard]] std::uint64_t next_client_id(Millis now);
    void new_day(Millis now);
    void exit(Millis now, std::string reason, Output& out);
    void flatten_if_needed(Millis now, Output& out);
    /// "Bearish -30", and with `factors` ": OI shift -20, futures vs VWAP -15, …".
    [[nodiscard]] std::string signal_text(bool factors = false) const;

    RunConfig config_;
    std::optional<Contract> contract_;
    std::optional<SignalView> signal_;
    bool kill_switch_ = false;

    State state_ = State::Flat;
    std::int64_t position_ = 0;  ///< net, from the run's own fills
    core::Price entry_;
    Millis entered_at_ = 0;
    std::int64_t last_signal_used_ = 0;  ///< an entry per signal minute at most

    std::uint64_t pending_client_ = 0;  ///< the entry or exit order on its way
    std::uint64_t stop_client_ = 0;
    std::int64_t stop_order_id_ = 0;
    bool cancel_stop_when_known_ = false;
    Millis retry_at_ = 0;        ///< after a failed exit, no new closing order before this
    int failed_exits_ = 0;       ///< in a row: the retry waits 5 s, 10 s, 20 s … up to 5 min
    std::string last_blocked_;   ///< the last blocked entry recorded (signal minute and reason)

    std::int32_t day_ = 0;
    int entries_ = 0;
    Paise realised_ = 0;
    Paise charges_ = 0;
    Millis last_exit_ = 0;
    std::uint32_t counter_ = 0;
};

}  // namespace mda::autotrade
