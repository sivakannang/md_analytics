#pragma once

// MCX contract sizes (ADR-0031). Kite lists every MCX contract with lot size 1 and counts
// quantity, depth, volume and OI in lots; prices are per quotation unit (₹/barrel, ₹/10 g).
// We keep quantities in units, so a lot's size is the number of quotation units it holds.
// Checked on 2026-10-27 against Kite's margin calculator (tools/research/mcx_margin_probe.py:
// each mini and full contract comes out at the same margin share).

#include <cstdint>
#include <optional>
#include <string_view>

namespace mda::instruments {

/// Quotation units per lot of an MCX commodity ("CRUDEOIL" -> 100 barrels), or nullopt if
/// unknown: such a contract keeps lot size 1 and can be watched but not paper traded.
[[nodiscard]] std::optional<std::int64_t> mcx_units_per_lot(std::string_view name) noexcept;

}  // namespace mda::instruments
