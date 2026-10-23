#include <algorithm>

#include <mda/bars/bar_builder.hpp>

namespace mda::bars {
namespace {

constexpr std::int64_t kIstOffset = 5 * 3600 + 30 * 60;
constexpr std::int64_t kNanos = 1'000'000'000;

// Receive time can trail the exchange-time minute by a little; it is never before it.
std::uint16_t oi_update_offset(std::int64_t received, std::int64_t start) {
    return static_cast<std::uint16_t>(std::clamp<std::int64_t>(received - start, 0, kNoOiUpdateTime - 1));
}

}  // namespace

int ist_minute_of_day(std::int64_t unix_seconds) {
    const std::int64_t local = (unix_seconds + kIstOffset) % 86'400;
    return static_cast<int>((local < 0 ? local + 86'400 : local) / 60);
}

BarBuilder::BarBuilder(BuilderOptions options, Sink on_closed)
    : options_(options), on_closed_(std::move(on_closed)) {}

void BarBuilder::on_snapshot(const messages::MarketSnapshot& s) {
    const std::uint32_t id = s.header.instrument_id.value();
    if (id == 0) {
        return;
    }
    const bool has_exchange_time =
        (s.fields & messages::kFieldExchangeTime) != 0 && s.exchange_ts.is_set();
    const std::int64_t at = (has_exchange_time ? s.exchange_ts : s.receive_ts).ns() / kNanos;
    const std::int64_t minute = at - at % 60;
    const int minute_of_day = ist_minute_of_day(minute);

    State& state = states_[id];
    if (state.session_end < 0) {
        const bool derivative = options_.is_derivative && options_.is_derivative(s.header.instrument_id);
        state.session_end = derivative ? options_.derivatives_session_end : options_.session_end;
    }
    const bool in_session = minute_of_day >= options_.session_start && minute_of_day < state.session_end;
    if (state.open && minute != state.bar.start) {
        if (minute < state.bar.start) {
            return;  // a late tick from a minute already closed: ignored
        }
        close(state);
    }

    const bool has_ltp = (s.fields & messages::kFieldLtp) != 0 && s.ltp.raw() > 0;
    const bool has_volume = (s.fields & messages::kFieldVolume) != 0;
    const bool has_oi = (s.fields & messages::kFieldOpenInterest) != 0 && s.oi.value() > 0;

    if (in_session && !state.open) {
        state.bar = Bar{};
        state.bar.header = messages::make_header<Bar>(s.header.instrument_id, 0);
        state.bar.start = minute;
        state.bar.source = BarSource::Live;
        if (state.last_oi > 0) {  // OI at the start of the minute is the last one seen
            state.bar.oi_open = state.bar.oi_high = state.bar.oi_low = state.bar.oi_close =
                core::Quantity(state.last_oi);
        }
        state.priced = false;
        state.open = true;
    }

    bool traded = false;
    if (has_volume) {
        const std::int64_t volume = s.volume.value();
        if (state.seeded_volume >= 0) {  // first tick after a restart: baseline only
            state.last_volume = std::max(volume, state.seeded_volume);
            state.seeded_volume = -1;
        } else if (state.last_volume < 0) {
            state.last_volume = volume;
        } else if (volume > state.last_volume) {
            if (state.open) {
                state.bar.volume =
                    core::Quantity(state.bar.volume.value() + (volume - state.last_volume));
            }
            traded = true;
            state.last_volume = volume;
        }  // lower: the closing auction's matched quantity; the day total comes back after it
    }
    if (has_ltp) {
        if (state.last_close.raw() != 0 && s.ltp != state.last_close) {
            traded = true;
        }
        if (state.open) {
            Bar& bar = state.bar;
            if (!state.priced) {
                bar.open = bar.high = bar.low = bar.close = s.ltp;
                state.priced = true;
            } else {
                bar.high = std::max(bar.high, s.ltp);
                bar.low = std::min(bar.low, s.ltp);
                bar.close = s.ltp;
            }
        }
        state.last_close = s.ltp;
    }
    if (has_oi) {
        const std::int64_t oi = s.oi.value();
        if (state.open) {
            Bar& bar = state.bar;
            if (bar.oi_open.value() == 0) {
                bar.oi_open = bar.oi_high = bar.oi_low = core::Quantity(oi);
            }
            bar.oi_high = core::Quantity(std::max(bar.oi_high.value(), oi));
            bar.oi_low = core::Quantity(std::min(bar.oi_low.value(), oi));
            bar.oi_close = core::Quantity(oi);
            if (state.last_oi > 0 && oi != state.last_oi) {
                ++bar.oi_updates;
                bar.oi_update_offset = oi_update_offset(s.receive_ts.ns() / kNanos, bar.start);
            }
        }
        state.last_oi = oi;
    }
    if (traded && state.open) {
        ++state.bar.trades;
    }
}

void BarBuilder::close(State& state) {
    if (!state.open) {
        return;
    }
    state.open = false;
    Bar& bar = state.bar;
    if (!state.priced) {
        if (state.last_close.raw() == 0) {
            return;  // nothing to draw: no price seen yet
        }
        bar.open = bar.high = bar.low = bar.close = state.last_close;  // OI-only minute
    }
    bar.header.flags = 0;
    on_closed_(bar);
}

void BarBuilder::close_due(std::int64_t now) {
    for (auto& [id, state] : states_) {
        if (state.open && now >= state.bar.start + 60 + options_.close_delay.count()) {
            close(state);
        }
    }
}

std::vector<Bar> BarBuilder::forming() const {
    std::vector<Bar> out;
    for (const auto& [id, state] : states_) {
        if (!state.open || (!state.priced && state.last_close.raw() == 0)) {
            continue;
        }
        Bar bar = state.bar;
        if (!state.priced) {
            bar.open = bar.high = bar.low = bar.close = state.last_close;
        }
        bar.header.flags = kBarForming;
        out.push_back(bar);
    }
    return out;
}

void BarBuilder::seed(const Bar& last_closed, std::int64_t day_volume) {
    State& state = states_[last_closed.header.instrument_id.value()];
    state.seeded_volume = day_volume;
    state.last_close = last_closed.close;
    if (last_closed.oi_close.value() > 0) {
        state.last_oi = last_closed.oi_close.value();
    }
}

}  // namespace mda::bars
