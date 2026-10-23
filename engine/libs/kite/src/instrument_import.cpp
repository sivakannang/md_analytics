#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <mda/kite/instrument_import.hpp>

namespace mda::kite {
namespace {

using instruments::ContractKey;
using instruments::Exchange;
using instruments::Kind;
using instruments::Market;
using instruments::OptionType;

/// Splits one CSV record, honouring double-quoted fields ("a,b" and "" escapes).
std::vector<std::string> split_csv(std::string_view line) {
    std::vector<std::string> fields(1);
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                fields.back() += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                fields.back() += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields.emplace_back();
        } else if (c != '\r') {
            fields.back() += c;
        }
    }
    return fields;
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

enum Column : std::size_t {
    kToken,
    kTradingSymbol,
    kName,
    kExpiry,
    kStrike,
    kTickSize,
    kLotSize,
    kInstrumentType,
    kSegment,
    kExchange,
    kColumnCount,
};

constexpr std::array<std::string_view, kColumnCount> kColumnNames = {
    "instrument_token", "tradingsymbol",   "name",    "expiry",  "strike", "tick_size",
    "lot_size",         "instrument_type", "segment", "exchange"};

/// F&O underlying names that differ from the index's trading symbol.
/// Verified against Kite's instrument list of 2026-10-22.
constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kIndexAliases = {{
    {"NIFTY", "NIFTY 50"},
    {"BANKNIFTY", "NIFTY BANK"},
    {"FINNIFTY", "NIFTY FIN SERVICE"},
    {"MIDCPNIFTY", "NIFTY MID SELECT"},
    {"NIFTYNXT50", "NIFTY NEXT 50"},
    {"NIFTYFPI", "NIFTY FPI 150"},
    {"SENSEX", "SENSEX"},
    {"SENSEX50", "SNSX50"},
    {"BANKEX", "BANKEX"},
    {"FOCIT", "BSE FOCUSED IT"},
}};

std::string_view index_symbol_for(std::string_view fo_name) {
    for (const auto& [name, symbol] : kIndexAliases) {
        if (name == fo_name) {
            return symbol;
        }
    }
    return fo_name;
}

enum class RowOutcome { Key, Unsupported, Malformed };

/// Builds the ContractKey for a row; Unsupported for segments we don't handle yet.
RowOutcome contract_key_for(const std::vector<std::string>& row, ContractKey& key) {
    const std::string& segment = row[kSegment];
    const std::string& exchange = row[kExchange];
    const std::string& type = row[kInstrumentType];

    if (segment == "INDICES" || segment == "NSE" || segment == "BSE") {
        if (exchange != "NSE" && exchange != "BSE") {
            return RowOutcome::Unsupported;
        }
        if (segment != "INDICES" && type != "EQ") {
            return RowOutcome::Unsupported;
        }
        key.exchange = exchange == "NSE" ? Exchange::Nse : Exchange::Bse;
        key.market = Market::Cash;
        key.kind = segment == "INDICES" ? Kind::Index : Kind::Equity;
        key.underlying = row[kTradingSymbol];
        return key.underlying.empty() ? RowOutcome::Malformed : RowOutcome::Key;
    }

    const bool nfo = segment.starts_with("NFO-");
    const bool bfo = segment.starts_with("BFO-");
    if (!nfo && !bfo) {
        return RowOutcome::Unsupported;  // CDS, BCD, MCX, NCO, …
    }
    key.exchange = nfo ? Exchange::Nse : Exchange::Bse;
    key.market = Market::Fo;
    key.underlying = row[kName];
    const auto expiry = instruments::parse_date(row[kExpiry]);
    if (key.underlying.empty() || !expiry) {
        return RowOutcome::Malformed;
    }
    key.expiry = *expiry;

    if (segment.ends_with("-FUT") && type == "FUT") {
        key.kind = Kind::Future;
        return RowOutcome::Key;
    }
    if (segment.ends_with("-OPT") && (type == "CE" || type == "PE")) {
        const auto strike = core::parse_price(row[kStrike]);
        if (!strike) {
            return RowOutcome::Malformed;
        }
        key.kind = Kind::Option;
        key.strike = *strike;
        key.option_type = type == "CE" ? OptionType::Call : OptionType::Put;
        return RowOutcome::Key;
    }
    return RowOutcome::Malformed;
}

/// Links each derivative listed today to its underlying index (preferred) or equity.
std::size_t link_underlyings(instruments::InstrumentRegistry& registry) {
    std::size_t unlinked = 0;
    for (const instruments::Instrument& instrument : registry.all()) {
        if (!instrument.listed_today || instrument.key.market != Market::Fo) {
            continue;
        }
        ContractKey index{.exchange = instrument.key.exchange,
                          .market = Market::Cash,
                          .kind = Kind::Index,
                          .underlying = std::string(index_symbol_for(instrument.key.underlying))};
        core::InstrumentId underlying = registry.find(index);
        if (!underlying.valid()) {
            ContractKey equity = index;
            equity.kind = Kind::Equity;
            equity.underlying = instrument.key.underlying;
            underlying = registry.find(equity);
        }
        if (underlying.valid()) {
            registry.get(instrument.id)->underlying = underlying;
        } else {
            ++unlinked;
        }
    }
    return unlinked;
}

}  // namespace

std::expected<ImportResult, ImportError> import_instruments(
    std::string_view csv, instruments::InstrumentRegistry& registry) {
    std::size_t start = 0;
    auto next_line = [&]() -> std::optional<std::string_view> {
        while (start < csv.size()) {
            std::size_t end = csv.find('\n', start);
            if (end == std::string_view::npos) {
                end = csv.size();
            }
            std::string_view line = csv.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (!line.empty()) {
                return line;
            }
        }
        return std::nullopt;
    };

    const auto header_line = next_line();
    if (!header_line) {
        return std::unexpected(ImportError::Empty);
    }
    // Locate columns by name, so column order or extra columns don't matter.
    const auto header = split_csv(*header_line);
    std::array<std::size_t, kColumnCount> position{};
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        const auto found = std::ranges::find(header, kColumnNames[c]);
        if (found == header.end()) {
            return std::unexpected(ImportError::MissingColumns);
        }
        position[c] = static_cast<std::size_t>(found - header.begin());
    }

    registry.begin_day();
    const std::size_t registered_before = registry.size();
    ImportResult result;
    ImportStats& stats = result.stats;

    while (const auto line = next_line()) {
        ++stats.rows;
        const auto fields = split_csv(*line);
        if (fields.size() < header.size()) {
            ++stats.malformed;
            continue;
        }
        std::vector<std::string> row(kColumnCount);
        for (std::size_t c = 0; c < kColumnCount; ++c) {
            row[c] = fields[position[c]];
        }

        ContractKey key;
        switch (contract_key_for(row, key)) {
            case RowOutcome::Unsupported:
                ++stats.skipped_unsupported;
                continue;
            case RowOutcome::Malformed:
                ++stats.malformed;
                continue;
            case RowOutcome::Key:
                break;
        }
        const auto token = parse_int<std::uint32_t>(row[kToken]);
        const auto lot_size = parse_int<std::int64_t>(row[kLotSize]);
        const auto tick_size = core::parse_price(row[kTickSize]);
        if (!token || !lot_size || !tick_size) {
            ++stats.malformed;
            continue;
        }

        const core::InstrumentId id = registry.assign(key);
        instruments::Instrument& instrument = *registry.get(id);
        instrument.trading_symbol = row[kTradingSymbol];
        instrument.name = row[kName];
        instrument.lot_size = core::Quantity(*lot_size);
        instrument.tick_size = *tick_size;
        instrument.listed_today = true;
        result.tokens.add(*token, id);
        ++stats.imported;
    }

    stats.new_instruments = registry.size() - registered_before;
    stats.unlinked = link_underlyings(registry);
    return result;
}

}  // namespace mda::kite
