#pragma once

// The permanent, broker-independent identity of a contract (ADR-0008).
//
// Canonical text form (stored in the registry file, shown in tools):
//   NSE|CASH|IDX|NIFTY 50
//   NSE|CASH|EQ|RELIANCE
//   NSE|FO|FUT|NIFTY|2026-10-27
//   NSE|FO|OPT|NIFTY|2026-10-27|25000.00|CE

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <mda/core/price.hpp>

namespace mda::instruments {

enum class Exchange : std::uint8_t {
    Nse = 1,
    Bse = 2,
};

enum class Market : std::uint8_t {
    Cash = 1,  ///< equities and indices
    Fo = 2,    ///< futures and options
};

enum class Kind : std::uint8_t {
    Index = 1,
    Equity = 2,
    Future = 3,
    Option = 4,
};

enum class OptionType : std::uint8_t {
    None = 0,
    Call = 1,
    Put = 2,
};

struct ContractKey {
    Exchange exchange = Exchange::Nse;
    Market market = Market::Cash;
    Kind kind = Kind::Equity;
    std::string underlying;  ///< "NIFTY 50" for an index, "RELIANCE", or the F&O name "NIFTY"
    std::chrono::year_month_day expiry{};  ///< derivatives only; default (invalid) otherwise
    core::Price strike;                    ///< options only
    OptionType option_type = OptionType::None;

    friend auto operator<=>(const ContractKey&, const ContractKey&) = default;
};

[[nodiscard]] std::string to_string(const ContractKey& key);
[[nodiscard]] std::optional<ContractKey> parse_contract_key(std::string_view text);

[[nodiscard]] std::string_view to_string(Exchange exchange) noexcept;
[[nodiscard]] std::string_view to_string(Kind kind) noexcept;

/// Parses an ISO date ("2026-10-27").
[[nodiscard]] std::optional<std::chrono::year_month_day> parse_date(std::string_view text) noexcept;
[[nodiscard]] std::string to_string(std::chrono::year_month_day date);

}  // namespace mda::instruments
