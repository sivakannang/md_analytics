#pragma once

// One run of the follow-signal-options strategy (docs/design/auto-trader.md §10): the same
// signal and guards as follow-signal (run.hpp), trading bought NIFTY options. Bullish buys
// the ATM call, Bearish the ATM put (contracts chosen by the process from the chain and the
// spot); entries are LIMIT at the ask + a few ticks with a timeout, a resting SL on the
// premium protects the position, and exits, measured on the index, are LIMIT at the bid −
// a few ticks, re-priced until filled. Pure logic, like Run.
//
// It never sells an option it doesn't hold: if a stop fills while an exit is resting, the
// exit is cancelled at once; if the run ever finds itself short, it buys back.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include <mda/autotrade/run.hpp>

namespace mda::autotrade {

struct OptionsRunConfig : RunConfig {
    double index_stop_points = 25;     ///< NIFTY moves this far against: exit
    double premium_stop_percent = 30;  ///< the resting SL: this far below the entry premium
    int entry_slip_ticks = 2;          ///< BUY LIMIT at the ask + this; exits at the bid − this
    int entry_timeout_s = 10;          ///< an entry not filled by then is cancelled
    int stop_limit_ticks = 10;         ///< the SL's limit, below its trigger
    int reprice_s = 3;                 ///< an unfilled exit is re-priced this often
};

/// The ATM call and put the run would buy now (the process picks them from the chain).
struct OptionPair {
    std::optional<Contract> call;
    std::optional<Contract> put;
};

/// One listed option of the underlying (from the instrument catalog).
struct ListedOption {
    Contract contract;
    core::Price strike;
    bool call = true;
};

/// The call and put to buy at `spot`: the nearest expiry after `today` (an option expiring
/// today is never bought), at the strike nearest the spot (the lower one on a tie).
[[nodiscard]] OptionPair choose_atm(std::span<const ListedOption> options, core::Price spot,
                                    std::int32_t today);

/// One option's market.
struct OptionQuote {
    core::Price ltp;
    core::Price bid;
    core::Price ask;
    Millis at = 0;
};

struct OptionsMarket {
    core::Price spot;  ///< the index
    Millis spot_at = 0;
    bool feed_ok = true;
    std::function<std::optional<OptionQuote>(core::InstrumentId)> quote;
};

class OptionsRun {
public:
    explicit OptionsRun(OptionsRunConfig config) : config_(std::move(config)) {}

    /// The ATM pair to choose from at the next entry (only used while flat).
    void set_options(OptionPair pair) { pair_ = std::move(pair); }
    void on_signal(const SignalView& signal) { signal_ = signal; }
    void set_kill_switch(bool on) { kill_switch_ = on; }

    [[nodiscard]] Output on_report(const broker::ExecutionReport& report, Millis now);
    [[nodiscard]] Output evaluate(const OptionsMarket& market, Millis now);

    /// After a restart: the option held (from paper.db), its entry premium, when it was
    /// entered, the index level then (0 if unknown: measured from the first evaluation), and
    /// its resting stop.
    void restore(const Contract& held, bool call, std::int64_t position, core::Price entry,
                 Millis entered_at, core::Price entry_spot, std::int64_t stop_order_id,
                 std::uint64_t stop_client_id);
    void restore_day(std::int32_t day, int entries, Paise realised, Paise charges, Millis last_exit,
                     std::uint32_t counter);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::int64_t position() const noexcept { return position_; }
    [[nodiscard]] const OptionsRunConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::optional<Contract>& held() const noexcept { return held_; }
    [[nodiscard]] bool holds_call() const noexcept { return call_; }
    [[nodiscard]] core::Price entry_spot() const noexcept { return entry_spot_; }
    [[nodiscard]] int entries_today() const noexcept { return entries_; }
    [[nodiscard]] Paise pnl_today() const noexcept { return realised_ - charges_; }
    [[nodiscard]] Paise realised_today() const noexcept { return realised_; }
    [[nodiscard]] Paise charges_today() const noexcept { return charges_; }
    [[nodiscard]] Millis last_exit() const noexcept { return last_exit_; }
    [[nodiscard]] std::uint32_t counter() const noexcept { return counter_; }
    [[nodiscard]] std::int32_t day() const noexcept { return day_; }

private:
    [[nodiscard]] std::optional<std::string> blocked(const OptionsMarket& market, Millis now,
                                                     const std::optional<Contract>& contract,
                                                     const std::optional<OptionQuote>& quote) const;
    [[nodiscard]] std::uint64_t next_client_id(Millis now);
    void new_day(Millis now);
    [[nodiscard]] core::Price tick() const;
    void exit(const OptionsMarket& market, Millis now, std::string reason, Output& out);
    void close_order(const OptionsMarket& market, Millis now, Output& out);

    OptionsRunConfig config_;
    OptionPair pair_;
    std::optional<SignalView> signal_;
    bool kill_switch_ = false;

    State state_ = State::Flat;
    std::optional<Contract> held_;  ///< the option bought (or being bought)
    bool call_ = true;
    std::int64_t position_ = 0;
    core::Price entry_;       ///< average premium
    core::Price entry_spot_;  ///< the index at entry: exits are measured from it
    Millis entered_at_ = 0;
    std::int64_t last_signal_used_ = 0;
    std::string last_blocked_;

    std::uint64_t pending_client_ = 0;  ///< the entry or exit order
    std::int64_t pending_order_id_ = 0;
    std::int64_t pending_quantity_ = 0;  ///< the order's total quantity (for a re-price)
    Millis pending_since_ = 0;
    bool pending_cancel_sent_ = false;
    std::uint64_t stop_client_ = 0;
    std::int64_t stop_order_id_ = 0;
    bool cancel_stop_when_known_ = false;
    Millis retry_at_ = 0;
    int failed_exits_ = 0;

    std::int32_t day_ = 0;
    int entries_ = 0;
    Paise realised_ = 0;
    Paise charges_ = 0;
    Millis last_exit_ = 0;
    std::uint32_t counter_ = 0;
};

}  // namespace mda::autotrade
