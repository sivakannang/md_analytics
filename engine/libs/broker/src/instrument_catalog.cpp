#include <mda/broker/instrument_catalog.hpp>

#include <chrono>
#include <cstdio>
#include <utility>

#include <mda/instruments/mcx.hpp>
#include <mda/instruments/session.hpp>

namespace mda::broker {

InstrumentCatalog::InstrumentCatalog(storage::Database& db, std::map<std::string, std::int64_t> freeze,
                                     McxMargins mcx)
    : db_(&db), freeze_(std::move(freeze)), mcx_(std::move(mcx)) {}

std::optional<rms::InstrumentInfo> InstrumentCatalog::get(core::InstrumentId id) {
    if (const auto found = cache_.find(id.value()); found != cache_.end()) {
        return found->second;
    }
    auto info = read(id);
    cache_[id.value()] = info;
    return info;
}

std::optional<rms::InstrumentInfo> InstrumentCatalog::read(core::InstrumentId id) {
    auto statement = db_->prepare(
        "SELECT exchange, kind, underlying, expiry, strike_raw, option_type, lot_size, tick_size_raw, listed, "
        "underlying_id, (SELECT u.kind FROM instruments u WHERE u.id = instruments.underlying_id) "
        "FROM instruments WHERE id = ?1");
    if (!statement) {
        return std::nullopt;
    }
    statement->bind(1, std::int64_t{id.value()});
    if (statement->step().value_or(storage::StepResult::Done) != storage::StepResult::Row) {
        return std::nullopt;
    }
    const std::string kind = statement->column_text(1);
    if (kind != "FUT" && kind != "OPT") {
        return std::nullopt;  // paper trading v1: F&O only
    }
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    const std::string expiry = statement->column_text(3);
    if (std::sscanf(expiry.c_str(), "%d-%u-%u", &y, &m, &d) != 3) {
        return std::nullopt;
    }
    const std::chrono::year_month_day day{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
    const std::string underlying = statement->column_text(2);
    const std::string exchange = statement->column_text(0);
    const bool mcx = exchange == "MCX";
    if (mcx && !instruments::mcx_units_per_lot(underlying)) {
        return std::nullopt;  // lot size unknown: watched, not traded
    }
    const auto freeze = freeze_.find(underlying);
    rms::InstrumentInfo info{
        .contract = {.id = id,
                     .segment = kind == "FUT" ? oms::Segment::Future : oms::Segment::Option,
                     .exchange = mcx ? oms::Exchange::Mcx : exchange == "BSE" ? oms::Exchange::Bse : oms::Exchange::Nse,
                     .lot_size = statement->column_int64(6),
                     .strike = core::Price::from_raw(statement->column_is_null(4) ? 0 : statement->column_int64(4)),
                     .call = statement->column_text(5) == "CE"},
        .tick = core::Price::from_raw(statement->column_int64(7)),
        .freeze_quantity = freeze == freeze_.end() ? 0 : freeze->second,
        .listed = statement->column_int64(8) != 0,
        .expiry_days = static_cast<std::int32_t>(std::chrono::sys_days{day}.time_since_epoch().count()),
        .underlying = core::InstrumentId(
            static_cast<std::uint32_t>(statement->column_is_null(9) ? 0 : statement->column_int64(9))),
        .index = !statement->column_is_null(10) && statement->column_text(10) == "IDX"};
    if (mcx) {
        const auto margin = mcx_.by_commodity.find(underlying);
        info.futures_margin = margin != mcx_.by_commodity.end() ? margin->second : mcx_.fallback;
        const auto session = instruments::trading_session(
            instruments::Exchange::Mcx, instruments::Market::Fo,
            instruments::ist_day(std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()));
        info.market_open = session.open * 60;
        info.market_close = session.close * 60;
    }
    return info;
}

}  // namespace mda::broker
