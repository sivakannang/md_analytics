#pragma once

// Today's Kite instrument_token -> InstrumentId mapping (ADR-0008). Rebuilt each trading
// day from the instrument master; tokens never leave the kite module.

#include <cstdint>
#include <unordered_map>

#include <mda/core/instrument_id.hpp>

namespace mda::kite {

/// Kite exchange segment, the low byte of an instrument_token.
enum class Segment : std::uint8_t {
    Nse = 1,
    Nfo = 2,
    Cds = 3,
    Bse = 4,
    Bfo = 5,
    Bcd = 6,
    Mcx = 7,
    Mcxsx = 8,
    Indices = 9,
};

[[nodiscard]] constexpr Segment segment_of(std::uint32_t token) noexcept {
    return static_cast<Segment>(token & 0xFF);
}

/// Segments whose instruments carry open interest.
[[nodiscard]] constexpr bool is_derivative(Segment segment) noexcept {
    return segment == Segment::Nfo || segment == Segment::Cds || segment == Segment::Bfo ||
           segment == Segment::Bcd || segment == Segment::Mcx;
}

class TokenMap {
public:
    void add(std::uint32_t token, core::InstrumentId id) {
        ids_[token] = id;
        tokens_[id.value()] = token;
    }

    /// Invalid InstrumentId if the token is not mapped today.
    [[nodiscard]] core::InstrumentId find(std::uint32_t token) const noexcept {
        const auto it = ids_.find(token);
        return it == ids_.end() ? core::InstrumentId{} : it->second;
    }

    /// Today's Kite token for an instrument, or 0 if it isn't listed today.
    [[nodiscard]] std::uint32_t token_for(core::InstrumentId id) const noexcept {
        const auto it = tokens_.find(id.value());
        return it == tokens_.end() ? 0 : it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

private:
    std::unordered_map<std::uint32_t, core::InstrumentId> ids_;
    std::unordered_map<std::uint32_t, std::uint32_t> tokens_;  // InstrumentId value -> token
};

}  // namespace mda::kite
