#include <array>
#include <utility>

#include <mda/instruments/mcx.hpp>

namespace mda::instruments {

namespace {

// Lot ÷ quotation unit: GOLD is 1 kg quoted per 10 g = 100; SILVER 30 kg quoted per kg = 30.
constexpr std::array<std::pair<std::string_view, std::int64_t>, 24> kUnitsPerLot = {{
    {"CRUDEOIL", 100},  {"CRUDEOILM", 10},   {"NATURALGAS", 1250}, {"NATGASMINI", 250},
    {"GOLD", 100},      {"GOLDM", 10},       {"GOLDPETAL", 1},     {"GOLDGUINEA", 1},
    {"GOLDTEN", 1},     {"SILVER", 30},      {"SILVERM", 5},       {"SILVERMIC", 1},
    {"COPPER", 2500},   {"ZINC", 5000},      {"ZINCMINI", 1000},   {"LEAD", 5000},
    {"LEADMINI", 1000}, {"ALUMINIUM", 5000}, {"ALUMINI", 1000},    {"NICKEL", 250},
    {"MENTHAOIL", 360}, {"CARDAMOM", 100},   {"MCXBULLDEX", 30},   {"MCXMETLDEX", 40},
}};

}  // namespace

std::optional<std::int64_t> mcx_units_per_lot(std::string_view name) noexcept {
    for (const auto& [commodity, units] : kUnitsPerLot) {
        if (commodity == name) {
            return units;
        }
    }
    return std::nullopt;
}

}  // namespace mda::instruments
