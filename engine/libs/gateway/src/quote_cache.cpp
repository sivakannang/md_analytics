#include <cstring>

#include <mda/gateway/quote_cache.hpp>

namespace mda::gateway {

using messages::MarketSnapshot;

std::string_view to_string(FeedHealth health) noexcept {
    switch (health) {
        case FeedHealth::Unavailable:
            return "unavailable";
        case FeedHealth::LoginRequired:
            return "login_required";
        case FeedHealth::Disconnected:
            return "disconnected";
        case FeedHealth::Connected:
            return "connected";
    }
    return "unavailable";
}

void QuoteCache::update(const MarketSnapshot& in) {
    const std::uint32_t id = in.header.instrument_id.value();
    if (id == 0) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    Entry& entry = entries_[id];
    MarketSnapshot& out = entry.quote.snapshot;
    const std::uint32_t fields = in.fields;

    out.header = in.header;
    out.mode = in.mode;
    out.receive_ts = in.receive_ts;
    if ((fields & messages::kFieldLtp) != 0) {
        out.ltp = in.ltp;
    }
    if ((fields & messages::kFieldOhlc) != 0) {
        out.open = in.open;
        out.high = in.high;
        out.low = in.low;
        out.close = in.close;
    }
    if ((fields & messages::kFieldVolume) != 0) {
        out.last_qty = in.last_qty;
        out.avg_price = in.avg_price;
        out.volume = in.volume;
        out.total_buy_qty = in.total_buy_qty;
        out.total_sell_qty = in.total_sell_qty;
    }
    if ((fields & messages::kFieldOpenInterest) != 0) {
        out.oi = in.oi;
        out.oi_day_high = in.oi_day_high;
        out.oi_day_low = in.oi_day_low;
    }
    if ((fields & messages::kFieldExchangeTime) != 0) {
        out.exchange_ts = in.exchange_ts;
    }
    if ((fields & messages::kFieldLastTradeTime) != 0) {
        out.last_trade_ts = in.last_trade_ts;
    }
    out.fields |= fields;
    entry.version = ++version_;
}

void QuoteCache::update_limits(const messages::CircuitLimits& limits) {
    const std::uint32_t id = limits.header.instrument_id.value();
    if (id == 0) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    Entry& entry = entries_[id];
    if (entry.quote.lower_circuit == limits.lower && entry.quote.upper_circuit == limits.upper) {
        return;  // the 60 s re-publish: nothing new to push
    }
    entry.quote.lower_circuit = limits.lower;
    entry.quote.upper_circuit = limits.upper;
    entry.quote.snapshot.header.instrument_id = limits.header.instrument_id;
    entry.version = ++version_;
}

void QuoteCache::update_analytics(const options::OptionAnalyticsMessage& analytics) {
    const std::uint32_t id = analytics.header.instrument_id.value();
    if (id == 0) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    Entry& entry = entries_[id];
    options::OptionAnalyticsMessage incoming = analytics;
    incoming.header.sequence = entry.quote.analytics.header.sequence;  // compare the payload only
    if (entry.quote.has_analytics && std::memcmp(&incoming, &entry.quote.analytics, sizeof incoming) == 0) {
        return;
    }
    entry.quote.analytics = analytics;
    entry.quote.has_analytics = true;
    entry.quote.snapshot.header.instrument_id = analytics.header.instrument_id;
    entry.version = ++version_;
}

std::optional<CachedQuote> QuoteCache::get(std::uint32_t id) const {
    const std::scoped_lock lock(mutex_);
    const auto found = entries_.find(id);
    return found == entries_.end() ? std::nullopt : std::optional(found->second.quote);
}

void QuoteCache::update_depth(const messages::DepthSnapshot& depth) {
    const std::uint32_t id = depth.header.instrument_id.value();
    if (id == 0) {
        return;
    }
    const std::scoped_lock lock(mutex_);
    DepthEntry& entry = depths_[id];
    entry.depth = depth;
    entry.version = ++version_;
}

void QuoteCache::set_feed(FeedHealth health) {
    const std::scoped_lock lock(mutex_);
    feed_ = health;
}

FeedHealth QuoteCache::feed() const {
    const std::scoped_lock lock(mutex_);
    return feed_;
}

std::vector<CachedQuote> QuoteCache::changed(std::span<const std::uint32_t> ids,
                                             SeenVersions& seen) const {
    std::vector<CachedQuote> out;
    const std::scoped_lock lock(mutex_);
    for (const std::uint32_t id : ids) {
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            continue;
        }
        std::uint64_t& sent = seen[id];
        if (found->second.version > sent) {
            out.push_back(found->second.quote);
            sent = found->second.version;
        }
    }
    return out;
}

std::vector<messages::DepthSnapshot> QuoteCache::changed_depth(std::span<const std::uint32_t> ids,
                                                               SeenVersions& seen) const {
    std::vector<messages::DepthSnapshot> out;
    const std::scoped_lock lock(mutex_);
    for (const std::uint32_t id : ids) {
        const auto found = depths_.find(id);
        if (found == depths_.end()) {
            continue;
        }
        std::uint64_t& sent = seen[id];
        if (found->second.version > sent) {
            out.push_back(found->second.depth);
            sent = found->second.version;
        }
    }
    return out;
}

std::size_t QuoteCache::size() const {
    const std::scoped_lock lock(mutex_);
    return entries_.size();
}

}  // namespace mda::gateway
