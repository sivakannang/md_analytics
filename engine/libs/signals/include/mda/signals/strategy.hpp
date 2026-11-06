#pragma once

// A direction strategy and the signal it gives once a minute (docs/design/signals.md §3).
// Strategies are compiled in and enabled per underlying in config (ADR-0028); ids are on
// the wire, so only append.

#include <cstdint>
#include <string_view>
#include <vector>

#include <mda/core/instrument_id.hpp>
#include <mda/signals/factors.hpp>
#include <mda/signals/market_state.hpp>

namespace mda::signals {

enum class StrategyId : std::uint16_t {
    OiBiasV1 = 1,
};

enum class Label : std::int8_t { Bearish = -1, Neutral = 0, Bullish = 1 };

struct FactorContribution {
    FactorId id = FactorId::FuturesBuildup;
    FactorStatus status = FactorStatus::Absent;
    double value = 0;   ///< the factor's value, [-1, 1]
    double points = 0;  ///< weight × value: its share of the score
};

struct Signal {
    core::InstrumentId spot;
    std::int64_t minute = 0;  ///< minute start, Unix seconds (UTC)
    StrategyId strategy = StrategyId::OiBiasV1;
    int horizon_min = 0;
    int score = 0;  ///< -100 … +100
    Label label = Label::Neutral;
    std::vector<FactorContribution> factors;  ///< every factor the strategy uses, absent ones too
};

class Strategy {
public:
    virtual ~Strategy() = default;

    [[nodiscard]] virtual StrategyId id() const noexcept = 0;
    [[nodiscard]] virtual int horizon_minutes() const noexcept = 0;
    /// The signal for the minute starting at `minute`, from data stamped by then.
    [[nodiscard]] virtual Signal evaluate(const MarketState& state, std::int64_t minute) const = 0;
};

[[nodiscard]] std::string_view name(StrategyId id) noexcept;
[[nodiscard]] std::string_view name(Label label) noexcept;

}  // namespace mda::signals
