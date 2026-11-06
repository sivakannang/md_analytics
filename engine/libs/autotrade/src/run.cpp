#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <utility>

#include <mda/autotrade/run.hpp>

namespace mda::autotrade {
namespace {

constexpr Millis kDayMs = 86'400'000;
constexpr Millis kIstMs = 19'800'000;
constexpr Millis kRetryMs = 5'000;       ///< a rejected exit is tried again this much later …
constexpr Millis kMaxRetryMs = 300'000;  ///< … doubling each time up to this
constexpr std::int64_t kMarketOpen = 9 * 3600 + 15 * 60;
constexpr std::int64_t kMarketClose = 15 * 3600 + 30 * 60;

std::string label_text(signals::Label label) {
    switch (label) {
        case signals::Label::Bullish:
            return "Bullish";
        case signals::Label::Bearish:
            return "Bearish";
        case signals::Label::Neutral:
            break;
    }
    return "Neutral";
}

bool final_status(oms::OrderStatus status) {
    return oms::is_final(status);
}

}  // namespace

std::int32_t ist_day(Millis now) noexcept {
    return static_cast<std::int32_t>((now + kIstMs) / kDayMs);
}

std::int64_t seconds_ist(Millis now) noexcept {
    return (now + kIstMs) % kDayMs / 1000;
}

std::optional<Contract> choose_future(std::span<const Contract> futures, std::int32_t today) {
    std::optional<Contract> best;
    for (const auto& f : futures) {
        if (f.expiry_days <= today) {
            continue;  // expired, or expiring today: no new positions in it
        }
        if (!best || f.expiry_days < best->expiry_days) {
            best = f;
        }
    }
    return best;
}

std::uint64_t client_order_id(std::string_view run_id, std::int32_t day, std::uint32_t counter) {
    std::uint32_t hash = 2166136261u;  // FNV-1a
    for (const char c : run_id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    return (std::uint64_t{hash & 0x7fffu} << 48) |
           (std::uint64_t{static_cast<std::uint32_t>(day) & 0xffffu} << 32) | counter;
}

std::string_view name(State state) noexcept {
    switch (state) {
        case State::Flat:
            return "flat";
        case State::Entering:
            return "entering";
        case State::Long:
            return "long";
        case State::Short:
            return "short";
        case State::Exiting:
            return "exiting";
    }
    return "?";
}

void Run::set_contract(const Contract& contract) {
    if (state_ == State::Flat && position_ == 0) {
        contract_ = contract;
    }
}

void Run::restore(std::int64_t position, core::Price entry, Millis entered_at,
                  std::int64_t stop_order_id, std::uint64_t stop_client_id) {
    position_ = position;
    entry_ = entry;
    entered_at_ = entered_at;
    stop_order_id_ = stop_order_id;
    stop_client_ = stop_client_id;
    state_ = position > 0 ? State::Long : position < 0 ? State::Short : State::Flat;
}

void Run::restore_day(std::int32_t day, int entries, Paise realised, Paise charges,
                      Millis last_exit, std::uint32_t counter) {
    day_ = day;
    entries_ = entries;
    realised_ = realised;
    charges_ = charges;
    last_exit_ = last_exit;
    counter_ = counter;
}

void Run::new_day(Millis now) {
    if (const std::int32_t day = ist_day(now); day != day_) {
        day_ = day;
        entries_ = 0;
        realised_ = 0;
        charges_ = 0;
        counter_ = 0;
    }
}

std::uint64_t Run::next_client_id(Millis now) {
    new_day(now);
    return client_order_id(config_.id, day_, ++counter_);
}

std::string_view factor_label(signals::FactorId factor) noexcept {
    switch (factor) {
        case signals::FactorId::FuturesBuildup:
            return "futures buildup";
        case signals::FactorId::AtmOiShift:
            return "OI shift";
        case signals::FactorId::PcrChange:
            return "PCR change";
        case signals::FactorId::FutureVsVwap:
            return "futures vs VWAP";
        case signals::FactorId::AtmIvChange:
            return "IV change";
        case signals::FactorId::MaxPainPull:
            return "max pain";
        case signals::FactorId::SectorBreadth:
            return "sector breadth";
        case signals::FactorId::VixChange:
            return "VIX change";
        case signals::FactorId::VolumeDelta:
            return "volume delta";
    }
    return "factor";
}

std::string describe(const SignalView& signal, bool factors) {
    std::string text = std::format("{} {:+}", label_text(signal.label), signal.score);
    if (factors && !signal.top.empty()) {
        // What drove it: the largest shares of the score.
        for (std::size_t i = 0; i < signal.top.size(); ++i) {
            text += std::format("{}{} {:+}", i == 0 ? ": " : ", ",
                                factor_label(signal.top[i].factor), signal.top[i].points);
        }
    }
    return text;
}

bool signal_holds(const SignalView& signal, bool long_side, int exit_score) {
    if (signal.label == (long_side ? signals::Label::Bearish : signals::Label::Bullish)) {
        return false;
    }
    return long_side ? signal.score >= exit_score : signal.score <= -exit_score;
}

std::string Run::signal_text(bool factors) const {
    return signal_ ? describe(*signal_, factors) : "no signal";
}

std::optional<std::string> Run::blocked(const Market& market, Millis now) const {
    const std::int64_t time_of_day = seconds_ist(now);
    if (!config_.enabled) {
        return "disabled";
    }
    if (kill_switch_) {
        return "kill switch";
    }
    if (time_of_day < config_.entry_start || time_of_day >= config_.entry_end) {
        return "outside the entry window";
    }
    if (!contract_) {
        return "no contract";
    }
    if (contract_->expiry_days <= ist_day(now)) {
        return "contract expires today";
    }
    if (now - (signal_->minute + 60) * 1000 > Millis{config_.signal_max_age_s} * 1000) {
        return "signal is stale";
    }
    if (!market.feed_ok || market.ltp.raw() <= 0 ||
        now - market.quote_at > config_.quote_max_age_ms) {
        return "no fresh quote";
    }
    if (entries_ >= config_.max_trades_per_day) {
        return "trades per day";
    }
    if (last_exit_ > 0 && now - last_exit_ < Millis{config_.cooldown_min} * 60'000) {
        return "cooldown";
    }
    if (realised_ - charges_ <= -config_.daily_loss_stop) {
        return "daily loss stop";
    }
    return std::nullopt;
}

void Run::exit(Millis now, std::string reason, Output& out) {
    const State before = state_;
    if (stop_order_id_ != 0) {
        out.actions.push_back({.kind = broker::Action::Cancel, .order_id = stop_order_id_});
        stop_order_id_ = 0;  // cancel asked for once; its report still matches by client id
    } else if (stop_client_ != 0) {
        cancel_stop_when_known_ = true;  // cancelled once the broker acknowledges it
    }
    const std::uint64_t client = next_client_id(now);
    out.actions.push_back({.kind = broker::Action::New,
                           .side = position_ > 0 ? oms::Side::Sell : oms::Side::Buy,
                           .type = oms::OrderType::Market,
                           .quantity = std::abs(position_),
                           .client_order_id = client,
                           .instrument = contract_ ? contract_->id : core::InstrumentId{}});
    pending_client_ = client;
    state_ = State::Exiting;
    out.decisions.push_back({.at = now,
                             .before = before,
                             .action = "exit",
                             .reason = std::move(reason),
                             .client_order_id = client});
}

void Run::flatten_if_needed(Millis now, Output& out) {
    if (position_ == 0 || pending_client_ != 0 || now < retry_at_) {
        return;
    }
    // Only while the market is open: outside it every order is rejected (the broker's own
    // 15:25 MIS square-off has run by then).
    if (const std::int64_t t = seconds_ist(now); t < kMarketOpen || t >= kMarketClose) {
        return;
    }
    exit(now, "flatten: a position with no order closing it", out);
}

Output Run::evaluate(const Market& market, Millis now) {
    new_day(now);
    Output out;
    const std::int64_t time_of_day = seconds_ist(now);

    switch (state_) {
        case State::Flat:
        case State::Exiting:
            flatten_if_needed(now, out);
            if (state_ != State::Flat || position_ != 0 || pending_client_ != 0) {
                break;
            }
            if (!signal_ || signal_->label == signals::Label::Neutral ||
                signal_->minute <= last_signal_used_) {
                break;
            }
            if (const auto why = blocked(market, now)) {
                // Once per signal minute and reason, not every second.
                const std::string key = std::format("{}:{}", signal_->minute, *why);
                if (key != last_blocked_) {
                    last_blocked_ = key;
                    out.decisions.push_back({.at = now,
                                             .before = state_,
                                             .action = "blocked",
                                             .reason = signal_text() + ": " + *why});
                }
                break;
            }
            {
                const bool buy = signal_->label == signals::Label::Bullish;
                const std::uint64_t client = next_client_id(now);
                out.actions.push_back({.kind = broker::Action::New,
                                       .side = buy ? oms::Side::Buy : oms::Side::Sell,
                                       .type = oms::OrderType::Market,
                                       .quantity = config_.lots * contract_->lot_size,
                                       .client_order_id = client,
                                       .instrument = contract_->id});
                out.decisions.push_back({.at = now,
                                         .before = state_,
                                         .action = buy ? "enter long" : "enter short",
                                         .reason = signal_text(true),
                                         .client_order_id = client,
                                         .price = market.ltp});
                pending_client_ = client;
                last_signal_used_ = signal_->minute;
                ++entries_;
                state_ = State::Entering;
            }
            break;
        case State::Entering:
            break;
        case State::Long:
        case State::Short: {
            const bool long_side = state_ == State::Long;
            if (stop_client_ == 0 && pending_client_ == 0) {
                // A position always has a resting stop at the broker (it survives our restart).
                const std::int64_t tick =
                    contract_ && contract_->tick.raw() > 0 ? contract_->tick.raw() : 500;
                const auto distance = static_cast<std::int64_t>(
                    std::llround(config_.stop_points * core::Price::kScale));
                const std::int64_t raw = long_side
                                             ? (entry_.raw() - distance) / tick * tick
                                             : (entry_.raw() + distance + tick - 1) / tick * tick;
                const std::uint64_t client = next_client_id(now);
                out.actions.push_back(
                    {.kind = broker::Action::New,
                     .side = long_side ? oms::Side::Sell : oms::Side::Buy,
                     .type = oms::OrderType::StopMarket,
                     .quantity = std::abs(position_),
                     .trigger = core::Price::from_raw(raw),
                     .client_order_id = client,
                     .instrument = contract_ ? contract_->id : core::InstrumentId{}});
                out.decisions.push_back(
                    {.at = now,
                     .before = state_,
                     .action = "place stop",
                     .reason = std::format("{} points from {:.2f}", config_.stop_points,
                                           entry_.to_double()),
                     .client_order_id = client,
                     .price = core::Price::from_raw(raw)});
                stop_client_ = client;
            }
            const double move =
                market.ltp.raw() > 0
                    ? (market.ltp.to_double() - entry_.to_double()) * (long_side ? 1 : -1)
                    : 0;
            if (time_of_day >= config_.exit_by) {
                exit(now, "end of day", out);
            } else if (signal_ && !signal_holds(*signal_, long_side, config_.exit_score)) {
                exit(now, "signal changed: " + signal_text(true), out);
            } else if (config_.target_points > 0 && market.ltp.raw() > 0 &&
                       move >= config_.target_points) {
                exit(now, std::format("target: {:+.2f} points", move), out);
            } else if (now - entered_at_ >= Millis{config_.max_hold_min} * 60'000) {
                exit(now, "max hold", out);
            }
            break;
        }
    }
    return out;
}

Output Run::on_report(const broker::ExecutionReport& r, Millis now) {
    new_day(now);
    Output out;
    const bool is_stop = stop_client_ != 0 && r.client_order_id == stop_client_;
    const bool is_pending = pending_client_ != 0 && r.client_order_id == pending_client_;
    if (r.user != config_.user || (!is_stop && !is_pending)) {
        return out;  // not one of ours
    }
    const State before = state_;

    if (r.event == broker::ReportEvent::Fill) {
        const std::int64_t signed_qty = (r.side == oms::Side::Buy ? 1 : -1) * r.last_quantity;
        if (position_ != 0 && (position_ > 0) != (signed_qty > 0)) {
            const std::int64_t closing = std::min(std::abs(signed_qty), std::abs(position_));
            const double points =
                (r.last_price.to_double() - entry_.to_double()) * (position_ > 0 ? 1 : -1);
            realised_ +=
                static_cast<Paise>(std::llround(points * static_cast<double>(closing) * 100.0));
        }
        position_ += signed_qty;
        charges_ += r.charges;
    }

    if (is_stop) {
        if (r.event == broker::ReportEvent::Accepted) {
            stop_order_id_ = r.order_id;
            if (cancel_stop_when_known_) {
                cancel_stop_when_known_ = false;
                out.actions.push_back({.kind = broker::Action::Cancel, .order_id = r.order_id});
            }
        } else if (final_status(r.status)) {
            stop_client_ = 0;
            stop_order_id_ = 0;
            cancel_stop_when_known_ = false;
            if (r.status == oms::OrderStatus::Complete) {
                out.decisions.push_back({.at = now,
                                         .before = before,
                                         .action = "stopped out",
                                         .reason = "stop filled",
                                         .client_order_id = r.client_order_id,
                                         .price = r.last_price});
                if (state_ == State::Long || state_ == State::Short) {
                    state_ = position_ == 0 ? State::Flat : state_;
                    last_exit_ = now;
                }
            } else if (state_ == State::Long || state_ == State::Short) {
                // The broker dropped our stop (rejected, expired): don't sit unprotected.
                exit(now, "stop lost: " + std::string(broker::text_of(r.reason)), out);
            }
        }
        return out;
    }

    // The entry or exit order.
    if (!final_status(r.status)) {
        return out;
    }
    pending_client_ = 0;
    if (state_ == State::Entering) {
        if (position_ != 0) {
            entry_ = r.average_price.raw() > 0 ? r.average_price : r.last_price;
            entered_at_ = now;
            state_ = position_ > 0 ? State::Long : State::Short;
            out.decisions.push_back({.at = now,
                                     .before = before,
                                     .action = position_ > 0 ? "long" : "short",
                                     .reason = "entry filled",
                                     .client_order_id = r.client_order_id,
                                     .price = entry_});
        } else {
            // Not a trade: give back the day's entry, but wait the cooldown before trying
            // again so a standing rejection (risk limits) isn't retried every minute.
            state_ = State::Flat;
            entries_ = std::max(0, entries_ - 1);
            last_exit_ = now;
            out.decisions.push_back({.at = now,
                                     .before = before,
                                     .action = "entry failed",
                                     .reason = std::string(broker::text_of(r.reason)),
                                     .client_order_id = r.client_order_id});
        }
    } else if (state_ == State::Exiting) {
        if (position_ == 0) {
            state_ = State::Flat;
            last_exit_ = now;
            failed_exits_ = 0;
            out.decisions.push_back({.at = now,
                                     .before = before,
                                     .action = "flat",
                                     .reason = "exit filled",
                                     .client_order_id = r.client_order_id,
                                     .price = r.average_price});
        } else {
            // Not flat: the exit was rejected, or a stop filled on the way and the exit
            // turned the position over. Close what is left, a little later.
            failed_exits_ = r.status == oms::OrderStatus::Complete ? 0 : failed_exits_ + 1;
            retry_at_ =
                now + std::min(kMaxRetryMs,
                               kRetryMs << std::min(failed_exits_ > 0 ? failed_exits_ - 1 : 0, 6));
            out.decisions.push_back({.at = now,
                                     .before = before,
                                     .action = "not flat",
                                     .reason = r.status == oms::OrderStatus::Complete
                                                   ? std::string("the stop filled first")
                                                   : std::string(broker::text_of(r.reason)),
                                     .client_order_id = r.client_order_id});
        }
    }
    return out;
}

}  // namespace mda::autotrade
