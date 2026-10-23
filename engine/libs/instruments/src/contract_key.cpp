#include <algorithm>
#include <array>
#include <charconv>
#include <vector>

#include <mda/instruments/contract_key.hpp>

namespace mda::instruments {
namespace {

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

template <typename Int>
std::optional<Int> parse_int(std::string_view text) {
    Int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string two_digits(unsigned value) {
    return (value < 10 ? "0" : "") + std::to_string(value);
}

}  // namespace

std::string_view to_string(Exchange exchange) noexcept {
    switch (exchange) {
        case Exchange::Nse:
            return "NSE";
        case Exchange::Bse:
            return "BSE";
    }
    return "?";
}

std::string_view to_string(Kind kind) noexcept {
    switch (kind) {
        case Kind::Index:
            return "IDX";
        case Kind::Equity:
            return "EQ";
        case Kind::Future:
            return "FUT";
        case Kind::Option:
            return "OPT";
    }
    return "?";
}

std::optional<std::chrono::year_month_day> parse_date(std::string_view text) noexcept {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    const auto year = parse_int<int>(text.substr(0, 4));
    const auto month = parse_int<unsigned>(text.substr(5, 2));
    const auto day = parse_int<unsigned>(text.substr(8, 2));
    if (!year || !month || !day) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year(*year), std::chrono::month(*month),
                                           std::chrono::day(*day)};
    if (!date.ok()) {
        return std::nullopt;
    }
    return date;
}

std::string to_string(std::chrono::year_month_day date) {
    return std::to_string(static_cast<int>(date.year())) + "-" +
           two_digits(static_cast<unsigned>(date.month())) + "-" +
           two_digits(static_cast<unsigned>(date.day()));
}

std::string to_string(const ContractKey& key) {
    std::string text(to_string(key.exchange));
    text += key.market == Market::Cash ? "|CASH|" : "|FO|";
    text += to_string(key.kind);
    text += "|";
    text += key.underlying;
    if (key.kind == Kind::Future || key.kind == Kind::Option) {
        text += "|" + to_string(key.expiry);
    }
    if (key.kind == Kind::Option) {
        text += "|" + core::to_string(key.strike);
        text += key.option_type == OptionType::Call ? "|CE" : "|PE";
    }
    return text;
}

std::optional<ContractKey> parse_contract_key(std::string_view text) {
    const auto parts = split(text, '|');
    if (parts.size() < 4) {
        return std::nullopt;
    }
    ContractKey key;
    if (parts[0] == "NSE") {
        key.exchange = Exchange::Nse;
    } else if (parts[0] == "BSE") {
        key.exchange = Exchange::Bse;
    } else {
        return std::nullopt;
    }

    if (parts[1] == "CASH") {
        key.market = Market::Cash;
    } else if (parts[1] == "FO") {
        key.market = Market::Fo;
    } else {
        return std::nullopt;
    }

    constexpr std::array<std::pair<std::string_view, Kind>, 4> kinds = {
        {{"IDX", Kind::Index}, {"EQ", Kind::Equity}, {"FUT", Kind::Future}, {"OPT", Kind::Option}}};
    const auto kind = std::ranges::find(kinds, parts[2], &std::pair<std::string_view, Kind>::first);
    if (kind == kinds.end()) {
        return std::nullopt;
    }
    key.kind = kind->second;
    key.underlying = std::string(parts[3]);
    if (key.underlying.empty()) {
        return std::nullopt;
    }

    const bool cash_kind = key.kind == Kind::Index || key.kind == Kind::Equity;
    if (cash_kind != (key.market == Market::Cash)) {
        return std::nullopt;
    }
    const std::size_t expected_parts = key.kind == Kind::Option   ? 7
                                       : key.kind == Kind::Future ? 5
                                                                  : 4;
    if (parts.size() != expected_parts) {
        return std::nullopt;
    }
    if (key.kind == Kind::Future || key.kind == Kind::Option) {
        const auto expiry = parse_date(parts[4]);
        if (!expiry) {
            return std::nullopt;
        }
        key.expiry = *expiry;
    }
    if (key.kind == Kind::Option) {
        const auto strike = core::parse_price(parts[5]);
        if (!strike) {
            return std::nullopt;
        }
        key.strike = *strike;
        if (parts[6] == "CE") {
            key.option_type = OptionType::Call;
        } else if (parts[6] == "PE") {
            key.option_type = OptionType::Put;
        } else {
            return std::nullopt;
        }
    }
    return key;
}

}  // namespace mda::instruments
