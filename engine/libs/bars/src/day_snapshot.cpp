#include <algorithm>

#include <mda/bars/day_snapshot.hpp>

namespace mda::bars {

messages::MarketSnapshot day_snapshot(std::span<const Bar> day, core::Price previous_close) {
    const Bar& first = day.front();
    const Bar& last = day.back();
    messages::MarketSnapshot s{};
    s.header = messages::make_header<messages::MarketSnapshot>(first.header.instrument_id, 0);
    s.fields = messages::kFieldLtp | messages::kFieldOhlc | messages::kFieldVolume |
               messages::kFieldExchangeTime;
    s.mode = messages::FeedMode::Quote;
    s.ltp = last.close;
    s.open = first.open;
    s.high = first.high;
    s.low = first.low;
    s.close = previous_close;
    std::int64_t volume = 0;
    std::int64_t oi_high = 0;
    std::int64_t oi_low = 0;
    for (const Bar& bar : day) {
        s.high = std::max(s.high, bar.high);
        s.low = std::min(s.low, bar.low);
        volume += bar.volume.value();
        if (bar.oi_close.value() > 0) {
            oi_high = std::max(oi_high, bar.oi_high.value());
            oi_low = oi_low == 0 ? bar.oi_low.value() : std::min(oi_low, bar.oi_low.value());
            s.oi = bar.oi_close;
        }
    }
    s.volume = core::Quantity(volume);
    if (s.oi.value() > 0) {
        s.fields |= messages::kFieldOpenInterest;
        s.mode = messages::FeedMode::Full;
        s.oi_day_high = core::Quantity(oi_high);
        s.oi_day_low = core::Quantity(oi_low);
    }
    s.exchange_ts = s.receive_ts = core::Timestamp::from_seconds(last.start + 60);
    return s;
}

}  // namespace mda::bars
