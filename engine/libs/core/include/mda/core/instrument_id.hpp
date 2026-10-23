#pragma once

// Permanent internal instrument identifier (ADR-0008).
//
// Dense uint32 assigned once per ContractKey by the instrument registry and never
// reused, so per-instrument state can live in arrays indexed by id. 0 is invalid.
// Broker tokens (e.g. Kite's instrument_token) never appear outside the broker module.

#include <compare>
#include <cstdint>

namespace mda::core {

class InstrumentId {
public:
    constexpr InstrumentId() noexcept = default;
    constexpr explicit InstrumentId(std::uint32_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr auto operator<=>(InstrumentId, InstrumentId) noexcept = default;

private:
    std::uint32_t value_ = 0;
};

}  // namespace mda::core
