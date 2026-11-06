#pragma once

// Strategy oi-bias-v1 (docs/design/signals.md §4): a weighted sum of factors F1–F7, each
// in [-1, 1], with weights adding up to 100, so the score runs from -100 to +100. Candidate
// factors (F8, F9, §13) are scored and stored at weight 0 until calibration gives them points.

#include <array>

#include <mda/signals/strategy.hpp>

namespace mda::signals {

struct OiBiasConfig {
    int horizon_min = 30;
    int bullish = 25;   ///< score at or above: Bullish
    int bearish = -25;  ///< score at or below: Bearish
    /// Points per factor, indexed by FactorId - 1.
    std::array<double, kFactorCount> weights{20, 20, 15, 15, 10, 10, 10, 0, 0};
    FactorParams factors;
};

class OiBiasV1 final : public Strategy {
public:
    explicit OiBiasV1(OiBiasConfig config = {}) : config_(config) {}

    [[nodiscard]] StrategyId id() const noexcept override { return StrategyId::OiBiasV1; }
    [[nodiscard]] int horizon_minutes() const noexcept override { return config_.horizon_min; }
    [[nodiscard]] Signal evaluate(const MarketState& state, std::int64_t minute) const override;

private:
    OiBiasConfig config_;
};

}  // namespace mda::signals
