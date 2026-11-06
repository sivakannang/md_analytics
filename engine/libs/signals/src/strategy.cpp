#include <mda/signals/strategy.hpp>

namespace mda::signals {

std::string_view name(StrategyId id) noexcept {
    switch (id) {
    case StrategyId::OiBiasV1:
        return "oi-bias-v1";
    }
    return "unknown";
}

std::string_view name(Label label) noexcept {
    switch (label) {
    case Label::Bearish:
        return "bearish";
    case Label::Neutral:
        return "neutral";
    case Label::Bullish:
        return "bullish";
    }
    return "unknown";
}

}  // namespace mda::signals
