#include <utility>

#include <mda/signals/market_state.hpp>

namespace mda::signals {

Aggressor classify_trade(std::int64_t ltp, std::int64_t bid, std::int64_t ask,
                         std::int64_t prev_ltp, int tick) noexcept {
    if (ask > 0 && ltp >= ask) {
        return Aggressor::Buy;
    }
    if (bid > 0 && ltp <= bid) {
        return Aggressor::Sell;
    }
    // Inside the spread (or no quote): the tick rule, the last change carrying over an
    // unchanged LTP.
    const int direction = ltp > prev_ltp ? 1 : ltp < prev_ltp ? -1 : tick;
    return direction > 0 ? Aggressor::Buy : direction < 0 ? Aggressor::Sell : Aggressor::Unknown;
}

MarketState::MarketState(MarketDefinition definition) : definition_(std::move(definition)) {
    slots_[definition_.spot.value()] = Slot{.role = Role::Spot};
    if (definition_.future.valid()) {
        slots_[definition_.future.value()] = Slot{.role = Role::Future};
    }
}

void MarketState::set_future(core::InstrumentId future) {
    if (future == definition_.future) {
        return;
    }
    if (definition_.future.valid()) {
        slots_.erase(definition_.future.value());
    }
    definition_.future = future;
    future_ = {};
    flow_ = {};
    tracker_ = {};
    if (future.valid()) {
        slots_[future.value()] = Slot{.role = Role::Future};
    }
}

void MarketState::add_option(const OptionInfo& option) {
    if (const auto it = slots_.find(option.id.value()); it != slots_.end()) {
        if (it->second.role == Role::Option) {
            options_[it->second.index].info = option;
        }
        return;
    }
    slots_[option.id.value()] = Slot{.role = Role::Option, .index = options_.size()};
    options_.push_back(OptionEntry{.info = option, .oi = {}});
}

void MarketState::add_sector(core::InstrumentId id, double prev_close) {
    if (const auto it = slots_.find(id.value()); it != slots_.end()) {
        if (it->second.role == Role::Sector) {
            sectors_[it->second.index].prev_close = prev_close;
        }
        return;
    }
    slots_[id.value()] = Slot{.role = Role::Sector, .index = sectors_.size()};
    sectors_.push_back(SectorEntry{.id = id, .prev_close = prev_close, .close = {}});
}

void MarketState::set_vix(core::InstrumentId id) {
    if (id == vix_id_ || slots_.contains(id.value())) {
        return;
    }
    if (vix_id_.valid()) {
        slots_.erase(vix_id_.value());
    }
    vix_id_ = id;
    vix_ = {};
    slots_[id.value()] = Slot{.role = Role::Vix};
}

void MarketState::on_bar(const bars::Bar& bar) {
    if ((bar.header.flags & bars::kBarForming) != 0) {
        return;
    }
    const auto it = slots_.find(bar.header.instrument_id.value());
    if (it == slots_.end()) {
        return;
    }
    const double close = bar.close.to_double();
    switch (it->second.role) {
        case Role::Spot:
            spot_.add(bar.start, close);
            break;
        case Role::Future:
            future_.add(
                bar.start,
                FutureBar{.close = close,
                          .typical = (bar.high.to_double() + bar.low.to_double() + close) / 3.0,
                          .volume = bar.volume.value(),
                          .oi = bar.oi_close.value()});
            break;
        case Role::Option:
            // A bar without OI (none published yet today) says nothing about positioning.
            if (bar.oi_close.value() > 0) {
                options_[it->second.index].oi.add(bar.start, bar.oi_close.value());
            }
            break;
        case Role::Sector:
            sectors_[it->second.index].close.add(bar.start, close);
            break;
        case Role::Vix:
            vix_.add(bar.start, close);
            break;
    }
}

void MarketState::on_snapshot(const messages::MarketSnapshot& snapshot) {
    const core::Timestamp seen_at =
        snapshot.receive_ts.is_set() ? snapshot.receive_ts : snapshot.exchange_ts;
    const std::int64_t seen = seen_at.ns() / 1'000'000'000;
    if (last_seen_ > 0 && seen - last_seen_ >= kFeedGap) {
        tracker_.volume = -1;  // e.g. the Wi-Fi was down: what traded meanwhile isn't one second's
    }
    last_seen_ = std::max(last_seen_, seen);
    if (!definition_.future.valid() || snapshot.header.instrument_id != definition_.future ||
        (snapshot.fields & messages::kFieldVolume) == 0) {
        return;
    }
    const std::int64_t volume = snapshot.volume.value();
    const std::int64_t ltp = snapshot.ltp.raw();
    FlowTracker& t = tracker_;
    const core::Timestamp at =
        snapshot.exchange_ts.is_set() ? snapshot.exchange_ts : snapshot.receive_ts;
    const std::int64_t seconds = at.ns() / 1'000'000'000;
    // The first snapshot (or one after a gap or a new day) only sets the baseline: its
    // volume is everything traded since the last one seen, not one second's trades. So does
    // the pre-open (09:00–09:15): its auction match has no aggressor.
    const bool open = seconds - ist_day_start(seconds) >= kSessionOpen;
    if (t.volume >= 0 && volume >= t.volume && open) {
        FlowMinute& minute = flow_.upsert(seconds - seconds % 60);
        if (const std::int64_t traded = volume - t.volume; traded > 0) {
            switch (classify_trade(ltp, t.bid, t.ask, t.ltp, t.tick)) {
                case Aggressor::Buy:
                    minute.buy += traded;
                    break;
                case Aggressor::Sell:
                    minute.sell += traded;
                    break;
                case Aggressor::Unknown:
                    minute.unclassified += traded;
                    break;
            }
        }
    }
    if (t.volume >= 0 && ltp != t.ltp) {
        t.tick = ltp > t.ltp ? 1 : -1;
    }
    t.volume = volume;
    t.ltp = ltp;
}

void MarketState::on_depth(const messages::DepthSnapshot& depth) {
    if (!definition_.future.valid() || depth.header.instrument_id != definition_.future) {
        return;
    }
    tracker_.bid = depth.bids[0].quantity > 0 ? depth.bids[0].price.raw() : 0;
    tracker_.ask = depth.asks[0].quantity > 0 ? depth.asks[0].price.raw() : 0;
}

void MarketState::reset_flow_baseline() noexcept {
    tracker_.volume = -1;
}

void MarketState::on_summary(const options::ChainSummaryMessage& summary, std::int64_t minute) {
    if (summary.header.instrument_id != definition_.spot) {
        return;
    }
    summaries_[summary.expiry_days].add(minute, SummaryPoint{.forward = summary.forward,
                                                             .atm_strike = summary.atm_strike,
                                                             .atm_iv = summary.atm_iv,
                                                             .pcr = summary.pcr,
                                                             .max_pain = summary.max_pain});
}

void MarketState::prune(std::int64_t before) {
    spot_.prune(before);
    future_.prune(before);
    for (auto& option : options_) {
        option.oi.prune(before);
    }
    for (auto& sector : sectors_) {
        sector.close.prune(before);
    }
    vix_.prune(before);
    flow_.prune(before);
    for (auto& [expiry, series] : summaries_) {
        series.prune(before);
    }
}

std::optional<std::int32_t> MarketState::nearest_expiry(std::int64_t t) const {
    const std::int32_t today = ist_day(t);
    const std::int64_t from = ist_day_start(t);
    std::optional<std::int32_t> nearest;
    for (const auto& [expiry, series] : summaries_) {
        if (expiry < today || series.at(t, from) == nullptr) {
            continue;
        }
        if (!nearest || expiry < *nearest) {
            nearest = expiry;
        }
    }
    return nearest;
}

const Series<SummaryPoint>* MarketState::summaries(std::int32_t expiry_days) const {
    const auto it = summaries_.find(expiry_days);
    return it == summaries_.end() ? nullptr : &it->second;
}

std::vector<double> MarketState::strikes(std::int32_t expiry_days) const {
    std::vector<double> result;
    for (const auto& option : options_) {
        if (option.info.expiry_days == expiry_days) {
            result.push_back(option.info.strike);
        }
    }
    std::ranges::sort(result);
    const auto [first, last] = std::ranges::unique(result);
    result.erase(first, last);
    return result;
}

}  // namespace mda::signals
