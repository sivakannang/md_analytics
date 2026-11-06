#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <utility>

#include <mda/autotrade/options_run.hpp>

namespace mda::autotrade {
namespace {

constexpr Millis kRetryMs = 5'000;
constexpr Millis kMaxRetryMs = 300'000;
constexpr std::int64_t kMarketOpen = 9 * 3600 + 15 * 60;
constexpr std::int64_t kMarketClose = 15 * 3600 + 30 * 60;

/// `raw` rounded to the tick: up for a buy's limit, down for a sell's; never below one tick.
std::int64_t on_tick(std::int64_t raw, std::int64_t tick, bool up) {
    const std::int64_t rounded = up ? (raw + tick - 1) / tick * tick : raw / tick * tick;
    return std::max(rounded, tick);
}

}  // namespace

OptionPair choose_atm(std::span<const ListedOption> options, core::Price spot, std::int32_t today) {
    std::int32_t expiry = 0;
    for (const auto& o : options) {
        if (o.contract.expiry_days > today && (expiry == 0 || o.contract.expiry_days < expiry)) {
            expiry = o.contract.expiry_days;
        }
    }
    OptionPair pair;
    const ListedOption* best_call = nullptr;
    const ListedOption* best_put = nullptr;
    const auto better = [&](const ListedOption* best, const ListedOption& o) {
        const auto distance = [&](const ListedOption& x) {
            return std::abs(x.strike.raw() - spot.raw());
        };
        return best == nullptr || distance(o) < distance(*best) ||
               (distance(o) == distance(*best) && o.strike.raw() < best->strike.raw());
    };
    for (const auto& o : options) {
        if (expiry == 0 || o.contract.expiry_days != expiry) {
            continue;
        }
        auto& best = o.call ? best_call : best_put;
        if (better(best, o)) {
            best = &o;
        }
    }
    if (best_call != nullptr) {
        pair.call = best_call->contract;
    }
    if (best_put != nullptr) {
        pair.put = best_put->contract;
    }
    return pair;
}

void OptionsRun::restore(const Contract& held, bool call, std::int64_t position, core::Price entry,
                         Millis entered_at, core::Price entry_spot, std::int64_t stop_order_id,
                         std::uint64_t stop_client_id) {
    held_ = held;
    call_ = call;
    position_ = position;
    entry_ = entry;
    entered_at_ = entered_at;
    entry_spot_ = entry_spot;
    stop_order_id_ = stop_order_id;
    stop_client_ = stop_client_id;
    state_ = position > 0 ? State::Long : position < 0 ? State::Exiting : State::Flat;
}

void OptionsRun::restore_day(std::int32_t day, int entries, Paise realised, Paise charges,
                             Millis last_exit, std::uint32_t counter) {
    day_ = day;
    entries_ = entries;
    realised_ = realised;
    charges_ = charges;
    last_exit_ = last_exit;
    counter_ = counter;
}

void OptionsRun::new_day(Millis now) {
    if (const std::int32_t day = ist_day(now); day != day_) {
        day_ = day;
        entries_ = 0;
        realised_ = 0;
        charges_ = 0;
        counter_ = 0;
    }
}

std::uint64_t OptionsRun::next_client_id(Millis now) {
    new_day(now);
    return client_order_id(config_.id, day_, ++counter_);
}

core::Price OptionsRun::tick() const {
    return held_ && held_->tick.raw() > 0 ? held_->tick : core::Price::from_raw(500);
}

std::optional<std::string> OptionsRun::blocked(const OptionsMarket& market, Millis now,
                                               const std::optional<Contract>& contract,
                                               const std::optional<OptionQuote>& quote) const {
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
    if (!contract) {
        return "no option to buy";
    }
    if (contract->expiry_days <= ist_day(now)) {
        return "option expires today";
    }
    if (now - (signal_->minute + 60) * 1000 > Millis{config_.signal_max_age_s} * 1000) {
        return "signal is stale";
    }
    if (!market.feed_ok || market.spot.raw() <= 0 ||
        now - market.spot_at > config_.quote_max_age_ms) {
        return "no fresh index price";
    }
    if (!quote || quote->ask.raw() <= 0 || quote->bid.raw() <= 0 ||
        now - quote->at > config_.quote_max_age_ms) {
        return "no fresh option quote";
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

void OptionsRun::close_order(const OptionsMarket& market, Millis now, Output& out) {
    const bool sell = position_ > 0;
    const std::int64_t t = tick().raw();
    std::optional<OptionQuote> quote;
    if (held_ && market.quote) {
        quote = market.quote(held_->id);
    }
    Action a{.kind = broker::Action::New,
             .side = sell ? oms::Side::Sell : oms::Side::Buy,
             .type = oms::OrderType::Limit,
             .quantity = std::abs(position_),
             .client_order_id = next_client_id(now),
             .instrument = held_ ? held_->id : core::InstrumentId{}};
    const core::Price touch = quote ? (sell ? quote->bid : quote->ask) : core::Price{};
    const core::Price reference = touch.raw() > 0 ? touch : quote ? quote->ltp : core::Price{};
    if (reference.raw() > 0) {
        const std::int64_t slip = std::int64_t{config_.entry_slip_ticks} * t;
        a.price = core::Price::from_raw(on_tick(reference.raw() + (sell ? -slip : slip), t, !sell));
    } else {
        a.type = oms::OrderType::Market;  // no quote at all: get out at any price
    }
    out.actions.push_back(a);
    pending_client_ = a.client_order_id;
    pending_order_id_ = 0;
    pending_quantity_ = a.quantity;
    pending_since_ = now;
    pending_cancel_sent_ = false;
}

void OptionsRun::exit(const OptionsMarket& market, Millis now, std::string reason, Output& out) {
    const State before = state_;
    if (stop_order_id_ != 0) {
        out.actions.push_back({.kind = broker::Action::Cancel, .order_id = stop_order_id_});
        stop_order_id_ = 0;  // asked once; its report still matches by client id
    } else if (stop_client_ != 0) {
        cancel_stop_when_known_ = true;
    }
    close_order(market, now, out);
    state_ = State::Exiting;
    out.decisions.push_back({.at = now,
                             .before = before,
                             .action = "exit",
                             .reason = std::move(reason),
                             .client_order_id = pending_client_,
                             .price = out.actions.back().price});
}

Output OptionsRun::evaluate(const OptionsMarket& market, Millis now) {
    new_day(now);
    Output out;
    const std::int64_t time_of_day = seconds_ist(now);
    const std::int64_t t = tick().raw();

    switch (state_) {
        case State::Flat:
        case State::Exiting: {
            // A position nothing is closing (an exit failed, or it turned short): close it, in
            // market hours, backing off after failures.
            if (position_ != 0 && pending_client_ == 0 && now >= retry_at_ &&
                time_of_day >= kMarketOpen && time_of_day < kMarketClose) {
                close_order(market, now, out);
                out.decisions.push_back({.at = now,
                                         .before = state_,
                                         .action = "exit",
                                         .reason = "flatten: a position with no order closing it",
                                         .client_order_id = pending_client_});
                state_ = State::Exiting;
                break;
            }
            if (state_ == State::Exiting) {
                // Re-price an exit that hasn't filled.
                if (pending_client_ != 0 && pending_order_id_ != 0 &&
                    now - pending_since_ >= Millis{config_.reprice_s} * 1000 && held_ &&
                    market.quote) {
                    if (const auto quote = market.quote(held_->id);
                        quote && (position_ > 0 ? quote->bid : quote->ask).raw() > 0) {
                        const bool sell = position_ > 0;
                        const std::int64_t slip = std::int64_t{config_.entry_slip_ticks} * t;
                        const auto price = core::Price::from_raw(
                            on_tick((sell ? quote->bid : quote->ask).raw() + (sell ? -slip : slip),
                                    t, !sell));
                        out.actions.push_back({.kind = broker::Action::Modify,
                                               .quantity = pending_quantity_,
                                               .price = price,
                                               .order_id = pending_order_id_,
                                               .instrument = held_->id});
                        out.decisions.push_back({.at = now,
                                                 .before = state_,
                                                 .action = "re-price exit",
                                                 .reason = "not filled yet",
                                                 .price = price});
                        pending_since_ = now;
                    }
                }
                break;
            }
            if (position_ != 0 || pending_client_ != 0 || !signal_ ||
                signal_->label == signals::Label::Neutral || signal_->minute <= last_signal_used_) {
                break;
            }
            const bool call = signal_->label == signals::Label::Bullish;
            const auto& contract = call ? pair_.call : pair_.put;
            std::optional<OptionQuote> quote;
            if (contract && market.quote) {
                quote = market.quote(contract->id);
            }
            if (const auto why = blocked(market, now, contract, quote)) {
                const std::string key = std::format("{}:{}", signal_->minute, *why);
                if (key != last_blocked_) {
                    last_blocked_ = key;
                    out.decisions.push_back({.at = now,
                                             .before = state_,
                                             .action = "blocked",
                                             .reason = describe(*signal_, false) + ": " + *why});
                }
                break;
            }
            held_ = contract;
            call_ = call;
            const std::int64_t tick_raw = tick().raw();
            const auto price = core::Price::from_raw(
                on_tick(quote->ask.raw() + std::int64_t{config_.entry_slip_ticks} * tick_raw,
                        tick_raw, true));
            const std::uint64_t client = next_client_id(now);
            out.actions.push_back({.kind = broker::Action::New,
                                   .side = oms::Side::Buy,
                                   .type = oms::OrderType::Limit,
                                   .quantity = config_.lots * contract->lot_size,
                                   .price = price,
                                   .client_order_id = client,
                                   .instrument = contract->id});
            out.decisions.push_back({.at = now,
                                     .before = state_,
                                     .action = call ? "buy call" : "buy put",
                                     .reason = describe(*signal_, true),
                                     .client_order_id = client,
                                     .price = price});
            pending_client_ = client;
            pending_order_id_ = 0;
            pending_quantity_ = config_.lots * contract->lot_size;
            pending_since_ = now;
            pending_cancel_sent_ = false;
            entry_spot_ = market.spot;
            last_signal_used_ = signal_->minute;
            ++entries_;
            state_ = State::Entering;
            break;
        }
        case State::Entering:
            // An entry not filled in time is cancelled (it may still fill before the cancel lands).
            if (pending_client_ != 0 && pending_order_id_ != 0 && !pending_cancel_sent_ &&
                now - pending_since_ >= Millis{config_.entry_timeout_s} * 1000) {
                out.actions.push_back(
                    {.kind = broker::Action::Cancel, .order_id = pending_order_id_});
                out.decisions.push_back({.at = now,
                                         .before = state_,
                                         .action = "cancel entry",
                                         .reason = "not filled in time"});
                pending_cancel_sent_ = true;
            }
            break;
        case State::Long:
        case State::Short: {
            if (entry_spot_.raw() <= 0 && market.spot.raw() > 0) {
                entry_spot_ = market.spot;  // restored without it: measure from now
            }
            if (stop_client_ == 0 && pending_client_ == 0) {
                const auto trigger_raw = on_tick(static_cast<std::int64_t>(std::llround(
                                                     static_cast<double>(entry_.raw()) *
                                                     (1.0 - config_.premium_stop_percent / 100.0))),
                                                 t, false);
                const auto limit_raw =
                    std::max(t, trigger_raw - std::int64_t{config_.stop_limit_ticks} * t);
                const std::uint64_t client = next_client_id(now);
                out.actions.push_back({.kind = broker::Action::New,
                                       .side = oms::Side::Sell,
                                       .type = oms::OrderType::StopLimit,
                                       .quantity = position_,
                                       .price = core::Price::from_raw(limit_raw),
                                       .trigger = core::Price::from_raw(trigger_raw),
                                       .client_order_id = client,
                                       .instrument = held_ ? held_->id : core::InstrumentId{}});
                out.decisions.push_back(
                    {.at = now,
                     .before = state_,
                     .action = "place stop",
                     .reason = std::format("{}% below {:.2f}", config_.premium_stop_percent,
                                           entry_.to_double()),
                     .client_order_id = client,
                     .price = core::Price::from_raw(trigger_raw)});
                stop_client_ = client;
            }
            const double move =
                market.spot.raw() > 0 && entry_spot_.raw() > 0
                    ? (market.spot.to_double() - entry_spot_.to_double()) * (call_ ? 1 : -1)
                    : 0;
            if (time_of_day >= config_.exit_by) {
                exit(market, now, "end of day", out);
            } else if (signal_ && !signal_holds(*signal_, call_, config_.exit_score)) {
                exit(market, now, "signal changed: " + describe(*signal_, true), out);
            } else if (config_.target_points > 0 && move >= config_.target_points) {
                exit(market, now, std::format("target: {:+.2f} index points", move), out);
            } else if (move <= -config_.index_stop_points) {
                exit(market, now, std::format("index stop: {:+.2f} index points", move), out);
            } else if (now - entered_at_ >= Millis{config_.max_hold_min} * 60'000) {
                exit(market, now, "max hold", out);
            }
            break;
        }
    }
    return out;
}

Output OptionsRun::on_report(const broker::ExecutionReport& r, Millis now) {
    new_day(now);
    Output out;
    const bool is_stop = stop_client_ != 0 && r.client_order_id == stop_client_;
    const bool is_pending =
        pending_client_ != 0 && (r.client_order_id == pending_client_ ||
                                 (pending_order_id_ != 0 && r.order_id == pending_order_id_));
    if (r.user != config_.user || (!is_stop && !is_pending)) {
        return out;
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
                stop_order_id_ = 0;
            }
        } else if (oms::is_final(r.status)) {
            stop_client_ = 0;
            stop_order_id_ = 0;
            cancel_stop_when_known_ = false;
            if (r.status == oms::OrderStatus::Complete) {
                out.decisions.push_back({.at = now,
                                         .before = before,
                                         .action = "stopped out",
                                         .reason = "premium stop filled",
                                         .client_order_id = r.client_order_id,
                                         .price = r.last_price});
                if (state_ == State::Long) {
                    state_ = State::Flat;
                    last_exit_ = now;
                } else if (state_ == State::Exiting && pending_order_id_ != 0) {
                    // Our exit is still resting: it must not sell what we no longer hold.
                    out.actions.push_back(
                        {.kind = broker::Action::Cancel, .order_id = pending_order_id_});
                    pending_cancel_sent_ = true;
                }
            } else if (state_ == State::Long) {
                out.decisions.push_back({.at = now,
                                         .before = before,
                                         .action = "stop lost",
                                         .reason = std::string(broker::text_of(r.reason))});
                // No quote here: the next evaluation closes it (flatten), within seconds.
                state_ = State::Exiting;
            }
        }
        return out;
    }

    // The entry or exit order.
    if (r.event == broker::ReportEvent::Accepted) {
        pending_order_id_ = r.order_id;
        return out;
    }
    if (!oms::is_final(r.status)) {
        return out;
    }
    pending_client_ = 0;
    pending_order_id_ = 0;
    if (state_ == State::Entering) {
        if (position_ > 0) {
            entry_ = r.average_price.raw() > 0 ? r.average_price : r.last_price;
            entered_at_ = now;
            state_ = State::Long;
            out.decisions.push_back({.at = now,
                                     .before = before,
                                     .action = call_ ? "long call" : "long put",
                                     .reason = r.status == oms::OrderStatus::Complete
                                                   ? "entry filled"
                                                   : "entry partly filled",
                                     .client_order_id = r.client_order_id,
                                     .price = entry_});
        } else {
            // Not a trade: give back the day's entry; the cooldown keeps a standing
            // rejection from being retried every minute.
            state_ = State::Flat;
            held_.reset();
            entries_ = std::max(0, entries_ - 1);
            last_exit_ = now;
            out.decisions.push_back({.at = now,
                                     .before = before,
                                     .action = "entry not filled",
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
                                     .reason = r.status == oms::OrderStatus::Complete
                                                   ? "exit filled"
                                                   : "exit cancelled: the stop filled first",
                                     .client_order_id = r.client_order_id,
                                     .price = r.average_price});
        } else {
            // Filled but still not flat (a stop filled too): close the rest now. Otherwise the
            // exit failed: back off before the next try.
            if (r.status == oms::OrderStatus::Complete) {
                failed_exits_ = 0;
                retry_at_ = now;
            } else {
                ++failed_exits_;
                retry_at_ = now + std::min(kMaxRetryMs, kRetryMs << std::min(failed_exits_ - 1, 6));
            }
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
