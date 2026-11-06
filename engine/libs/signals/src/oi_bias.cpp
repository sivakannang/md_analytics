#include <algorithm>
#include <cmath>

#include <mda/signals/oi_bias.hpp>

namespace mda::signals {

Signal OiBiasV1::evaluate(const MarketState& state, std::int64_t minute) const {
    Signal signal{.spot = state.definition().spot,
                  .minute = minute,
                  .strategy = id(),
                  .horizon_min = config_.horizon_min,
                  .score = 0,
                  .label = Label::Neutral,
                  .factors = {}};
    signal.factors.reserve(kFactorCount);

    double total = 0;
    for (std::size_t i = 0; i < kFactorCount; ++i) {
        const auto factor = static_cast<FactorId>(i + 1);
        const FactorValue value = evaluate_factor(factor, state, minute, config_.factors);
        // An absent factor adds nothing; it stays listed so the UI can say what's missing.
        const double points = config_.weights[i] * value.value;
        total += points;
        signal.factors.push_back(FactorContribution{
            .id = factor, .status = value.status, .value = value.value, .points = points});
    }

    signal.score = static_cast<int>(std::clamp(std::lround(total), -100L, 100L));
    if (signal.score >= config_.bullish) {
        signal.label = Label::Bullish;
    } else if (signal.score <= config_.bearish) {
        signal.label = Label::Bearish;
    }
    return signal;
}

}  // namespace mda::signals
