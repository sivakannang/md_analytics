#include <array>
#include <cctype>

#include <mda/catalog/catalog.hpp>

namespace mda::catalog {
namespace {

using instruments::Kind;
using storage::StepResult;

constexpr std::array<const char*, 12> kMonths = {"jan", "feb", "mar", "apr", "may", "jun",
                                                 "jul", "aug", "sep", "oct", "nov", "dec"};
constexpr std::size_t kMaxQueryWords = 6;

std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string kind_code(const instruments::ContractKey& key) {
    return std::string(instruments::to_string(key.kind));  // IDX | EQ | FUT | OPT
}

/// Strike without trailing ".00" ("25000", "24550.50").
std::string strike_text(core::Price strike) {
    std::string text = core::to_string(strike);
    if (text.ends_with(".00")) {
        text.resize(text.size() - 3);
    }
    return text;
}

/// LIKE pattern matching `word` anywhere, with %, _ and \ escaped.
std::string like_pattern(std::string_view word) {
    std::string pattern = "%";
    for (const char c : word) {
        if (c == '%' || c == '_' || c == '\\') {
            pattern += '\\';
        }
        pattern += c;
    }
    return pattern + "%";
}

}  // namespace

std::string search_text(const instruments::Instrument& instrument) {
    const auto& key = instrument.key;
    std::string text = lower(instrument.trading_symbol) + " " + lower(key.underlying) + " " +
                       lower(instrument.name) + " " + lower(instruments::to_string(key.exchange));
    switch (key.kind) {
        case Kind::Index:
            text += " index idx";
            break;
        case Kind::Equity:
            text += " eq equity";
            break;
        case Kind::Future:
            text += " fut future";
            break;
        case Kind::Option:
            text +=
                key.option_type == instruments::OptionType::Call ? " ce call opt" : " pe put opt";
            text += " " + strike_text(key.strike);
            break;
    }
    if (key.kind == Kind::Future || key.kind == Kind::Option) {
        const unsigned day = static_cast<unsigned>(key.expiry.day());
        const unsigned month = static_cast<unsigned>(key.expiry.month());
        text += " " + std::to_string(day) + " " + kMonths[month - 1] + " " +
                std::to_string(static_cast<int>(key.expiry.year()));
    }
    return text;
}

InstrumentInfo read_info(const storage::Statement& row, int first) {
    InstrumentInfo info;
    info.id = row.column_int64(first);
    info.exchange = row.column_text(first + 1);
    info.market = row.column_text(first + 2);
    info.kind = row.column_text(first + 3);
    info.underlying = row.column_text(first + 4);
    info.expiry = row.column_text(first + 5);
    if (!row.column_is_null(first + 6)) {
        info.strike = core::Price::from_raw(row.column_int64(first + 6));
    }
    info.option_type = row.column_text(first + 7);
    info.trading_symbol = row.column_text(first + 8);
    info.name = row.column_text(first + 9);
    info.lot_size = row.column_int64(first + 10);
    info.tick_size = core::Price::from_raw(row.column_int64(first + 11));
    info.underlying_id = row.column_int64(first + 12);
    info.listed = row.column_int64(first + 13) != 0;
    return info;
}

std::expected<SyncStats, storage::DbError> sync(storage::Database& db,
                                                const instruments::InstrumentRegistry& registry,
                                                std::int64_t now) {
    auto transaction = storage::Transaction::begin(db);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    if (auto cleared = db.exec("UPDATE instruments SET listed = 0"); !cleared) {
        return std::unexpected(cleared.error());
    }
    auto upsert = db.prepare(
        "INSERT INTO instruments (id, exchange, market, kind, underlying, expiry, strike_raw, "
        "option_type, trading_symbol, name, lot_size, tick_size_raw, underlying_id, listed, "
        "search_text, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, "
        "1, ?14, ?15) ON CONFLICT(id) DO UPDATE SET trading_symbol = excluded.trading_symbol, "
        "name = excluded.name, lot_size = excluded.lot_size, tick_size_raw = "
        "excluded.tick_size_raw, "
        "underlying_id = excluded.underlying_id, listed = 1, search_text = excluded.search_text, "
        "updated_at = excluded.updated_at");
    if (!upsert) {
        return std::unexpected(upsert.error());
    }
    SyncStats stats;
    for (const instruments::Instrument& instrument : registry.all()) {
        if (!instrument.listed_today) {
            continue;
        }
        const auto& key = instrument.key;
        const bool derivative = key.kind == Kind::Future || key.kind == Kind::Option;
        upsert->reset();
        upsert->bind(1, static_cast<std::int64_t>(instrument.id.value()))
            .bind(2, instruments::to_string(key.exchange))
            .bind(3, std::string_view(key.market == instruments::Market::Cash ? "CASH" : "FO"))
            .bind(4, std::string_view(kind_code(key)))
            .bind(5, std::string_view(key.underlying))
            .bind(9, std::string_view(instrument.trading_symbol))
            .bind(10, std::string_view(instrument.name))
            .bind(11, instrument.lot_size.value())
            .bind(12, instrument.tick_size.raw())
            .bind(14, std::string_view(search_text(instrument)))
            .bind(15, now);
        const std::string expiry = derivative ? instruments::to_string(key.expiry) : std::string();
        if (derivative) {
            upsert->bind(6, std::string_view(expiry));
        } else {
            upsert->bind_null(6);
        }
        if (key.kind == Kind::Option) {
            upsert->bind(7, key.strike.raw())
                .bind(8, std::string_view(key.option_type == instruments::OptionType::Call ? "CE"
                                                                                           : "PE"));
        } else {
            upsert->bind_null(7).bind_null(8);
        }
        if (instrument.underlying.valid()) {
            upsert->bind(13, static_cast<std::int64_t>(instrument.underlying.value()));
        } else {
            upsert->bind_null(13);
        }
        if (auto done = upsert->run(); !done) {
            return std::unexpected(done.error());
        }
        ++stats.listed;
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(committed.error());
    }
    return stats;
}

std::expected<std::vector<InstrumentInfo>, storage::DbError> search(storage::Database& db,
                                                                    std::string_view query,
                                                                    std::size_t limit) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : lower(query) + " ") {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty() && words.size() < kMaxQueryWords) {
                words.push_back(word);
            }
            word.clear();
        } else {
            word += c;
        }
    }
    if (words.empty()) {
        return std::vector<InstrumentInfo>{};
    }

    // Only bound parameters carry user text; the SQL shape depends on the word count alone.
    std::string sql =
        "SELECT " + std::string(kInfoColumns) + " FROM instruments i WHERE i.listed = 1";
    for (std::size_t n = 0; n < words.size(); ++n) {
        sql += " AND i.search_text LIKE ?" + std::to_string(n + 1) + " ESCAPE '\\'";
    }
    const std::string exact = "?" + std::to_string(words.size() + 1);
    const std::string first = "?" + std::to_string(words.size() + 3);
    // Exact symbol, then kind; within a kind an exact underlying wins ("nifty" before FINNIFTY).
    sql += " ORDER BY (lower(i.trading_symbol) = " + exact +
           ") DESC, "
           "CASE i.kind WHEN 'IDX' THEN 0 WHEN 'EQ' THEN 1 WHEN 'FUT' THEN 2 ELSE 3 END, "
           "(lower(i.underlying) IN (" +
           first + ", " + exact +
           ")) DESC, "
           "i.exchange = 'BSE', i.expiry, i.strike_raw, i.option_type, i.trading_symbol LIMIT ?" +
           std::to_string(words.size() + 2);

    auto statement = db.prepare(sql);
    if (!statement) {
        return std::unexpected(statement.error());
    }
    for (std::size_t n = 0; n < words.size(); ++n) {
        statement->bind(static_cast<int>(n + 1), std::string_view(like_pattern(words[n])));
    }
    std::string joined;
    for (const auto& w : words) {
        joined += (joined.empty() ? "" : " ") + w;
    }
    statement->bind(static_cast<int>(words.size() + 1), std::string_view(joined))
        .bind(static_cast<int>(words.size() + 2), static_cast<std::int64_t>(limit))
        .bind(static_cast<int>(words.size() + 3), std::string_view(words.front()));

    std::vector<InstrumentInfo> results;
    while (true) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        if (*row == StepResult::Done) {
            return results;
        }
        results.push_back(read_info(*statement));
    }
}

std::expected<std::optional<InstrumentInfo>, storage::DbError> get(storage::Database& db,
                                                                   std::int64_t id) {
    auto statement =
        db.prepare("SELECT " + std::string(kInfoColumns) + " FROM instruments i WHERE i.id = ?1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, id);
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (*row == StepResult::Done) {
        return std::optional<InstrumentInfo>{};
    }
    return read_info(*statement);
}

}  // namespace mda::catalog
