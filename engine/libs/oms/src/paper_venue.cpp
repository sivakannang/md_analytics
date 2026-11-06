#include <algorithm>

#include <mda/oms/paper_venue.hpp>

namespace mda::oms {

void PaperVenue::accepted(const Order& order, Millis now) {
    states_[order.id()] = {.eligible_at = now + config_.latency_ms, .resting = false};
}

void PaperVenue::forget(std::int64_t order_id) {
    states_.erase(order_id);
}

std::optional<PaperVenue::State> PaperVenue::state(std::int64_t order_id) const {
    const auto found = states_.find(order_id);
    return found == states_.end() ? std::nullopt : std::optional(found->second);
}

void PaperVenue::restore(std::int64_t order_id, State state) {
    states_[order_id] = state;
}

bool PaperVenue::fresh(const Book& book, Millis now) const noexcept {
    return book.feed_ok && book.updated_at > 0 && now - book.updated_at <= config_.stale_after_ms;
}

std::vector<Execution> PaperVenue::take(const Order& order, const Book& book,
                                        std::int64_t quantity) const {
    const bool buy = order.spec().side == Side::Buy;
    const bool limited = order.type() == OrderType::Limit;
    const core::Price limit = order.spec().price;
    const auto& levels = buy ? book.asks : book.bids;
    std::vector<Execution> fills;
    std::int64_t left = quantity;
    core::Price last;
    for (const Level& level : levels) {
        if (left == 0 || level.quantity <= 0) {
            break;
        }
        if (limited && (buy ? level.price > limit : level.price < limit)) {
            break;
        }
        const std::int64_t quantity_here = std::min(left, level.quantity);
        fills.push_back({.quantity = quantity_here, .price = level.price});
        left -= quantity_here;
        last = level.price;
    }
    if (left > 0 && !limited && !fills.empty()) {
        // Beyond the visible book: a few ticks worse than its last level.
        const std::int64_t slip = book.tick.raw() * config_.overflow_ticks;
        const std::int64_t raw =
            buy ? last.raw() + slip : std::max(book.tick.raw(), last.raw() - slip);
        fills.push_back({.quantity = left, .price = core::Price::from_raw(raw)});
    }
    return fills;
}

VenueResult PaperVenue::evaluate(const Order& order, const Book& book, Millis now) {
    VenueResult result;
    if (is_final(order.status())) {
        return result;
    }
    // An order the venue wasn't told about counts from its creation.
    State& state =
        states_
            .try_emplace(order.id(), State{.eligible_at = order.created_at() + config_.latency_ms,
                                           .resting = false})
            .first->second;
    if (now < state.eligible_at) {
        return result;
    }
    const bool market = order.type() == OrderType::Market;
    if (!fresh(book, now)) {
        if (order.status() == OrderStatus::Open && market) {
            result.reject = "no fresh market data";
        }
        return result;
    }
    const bool buy = order.spec().side == Side::Buy;

    if (order.status() == OrderStatus::TriggerPending) {
        const core::Price trigger = order.spec().trigger;
        const bool reached =
            book.ltp.raw() > 0 && (buy ? book.ltp >= trigger : book.ltp <= trigger);
        if (reached) {
            result.triggered = true;
            state = {.eligible_at = now + config_.latency_ms, .resting = false};
        }
        return result;
    }

    if (market) {
        result.fills = take(order, book, order.remaining());
        if (result.fills.empty()) {
            result.reject = "no liquidity on the other side";
        }
        return result;
    }

    // LIMIT: its first chance takes what it can; after that it rests.
    if (!state.resting) {
        result.fills = take(order, book, order.remaining());
        state.resting = true;
        return result;
    }
    const core::Price limit = order.spec().price;
    const Level& best = buy ? book.asks[0] : book.bids[0];
    const bool traded_through = book.ltp_at >= state.eligible_at && book.ltp.raw() > 0 &&
                                (buy ? book.ltp < limit : book.ltp > limit);
    const bool quoted_through =
        best.quantity > 0 && (buy ? best.price < limit : best.price > limit);
    if (traded_through || quoted_through) {
        result.fills.push_back({.quantity = order.remaining(), .price = limit});
    }
    return result;
}

}  // namespace mda::oms
