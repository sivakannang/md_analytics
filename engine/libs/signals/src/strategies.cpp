#include <mda/signals/strategies.hpp>

#include <mda/signals/oi_bias.hpp>

namespace mda::signals {

std::unique_ptr<Strategy> make_strategy(std::string_view id, int horizon_min, int bullish,
                                        int bearish) {
    if (id == name(StrategyId::OiBiasV1)) {
        OiBiasConfig config;
        config.horizon_min = horizon_min;
        config.bullish = bullish;
        config.bearish = bearish;
        return std::make_unique<OiBiasV1>(config);
    }
    return nullptr;
}

}  // namespace mda::signals
