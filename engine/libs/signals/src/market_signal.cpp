#include <mda/signals/market_signal.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace mda::signals {

MarketSignal to_message(const Signal& signal, std::uint64_t sequence) {
    MarketSignal message{.header = messages::make_header<MarketSignal>(signal.spot, sequence),
                         .minute = signal.minute,
                         .strategy = static_cast<std::uint16_t>(signal.strategy),
                         .horizon_min = static_cast<std::uint16_t>(signal.horizon_min),
                         .score = static_cast<std::int16_t>(signal.score),
                         .prob_bp = 0,
                         .top = {}};
    message.header.flags = signal.label == Label::Bullish   ? kSignalBullish
                           : signal.label == Label::Bearish ? kSignalBearish
                                                            : std::uint16_t{0};
    std::vector<const FactorContribution*> ranked;
    for (const auto& factor : signal.factors) {
        if (std::lround(factor.points) != 0) {
            ranked.push_back(&factor);
        }
    }
    std::ranges::stable_sort(ranked, [](const auto* a, const auto* b) {
        return std::abs(a->points) > std::abs(b->points);
    });
    const std::size_t count = std::min(ranked.size(), std::size(message.top));
    for (std::size_t i = 0; i < count; ++i) {
        message.top[i] = {.factor = static_cast<std::uint16_t>(ranked[i]->id),
                          .points = static_cast<std::int16_t>(std::lround(ranked[i]->points))};
    }
    return message;
}

Label label_of(const MarketSignal& message) noexcept {
    if ((message.header.flags & kSignalBullish) != 0) {
        return Label::Bullish;
    }
    if ((message.header.flags & kSignalBearish) != 0) {
        return Label::Bearish;
    }
    return Label::Neutral;
}

}  // namespace mda::signals
