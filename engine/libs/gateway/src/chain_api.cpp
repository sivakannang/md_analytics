#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/gateway/chain_api.hpp>
#include <mda/instruments/contract_key.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using detail::from_error;
using detail::run_on;
using detail::as_user;
using detail::token_of;

std::string date_text(std::chrono::year_month_day d) {
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(d.year()), static_cast<unsigned>(d.month()),
                  static_cast<unsigned>(d.day()));
    return text;
}

std::int32_t days_of(std::string_view text) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(std::string(text).c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) {
        return 0;
    }
    return static_cast<std::int32_t>(
        std::chrono::sys_days{std::chrono::year{y} / m / d}.time_since_epoch().count());
}

double round2(double value) {
    return std::round(value * 100.0) / 100.0;
}


}  // namespace

/// The catalog id of a chain's spot ("NSE|CASH|IDX|NIFTY 50"); 0 if unknown.
std::int64_t chain_spot_id(storage::Database& db, std::string_view key_text) {
    const auto key = instruments::parse_contract_key(key_text);
    if (!key) {
        return 0;
    }
    auto statement = db.prepare(
        "SELECT id FROM instruments WHERE exchange = ?1 AND kind = ?2 AND underlying = ?3 AND listed = 1");
    if (!statement) {
        return 0;
    }
    const std::string_view kind = key->kind == instruments::Kind::Index ? "IDX" : "EQ";
    statement->bind(1, key->exchange == instruments::Exchange::Nse ? std::string_view("NSE") : std::string_view("BSE"))
        .bind(2, kind)
        .bind(3, std::string_view(key->underlying));
    return statement->step().value_or(storage::StepResult::Done) == storage::StepResult::Row ? statement->column_int64(0)
                                                                                            : 0;
}

void ChainSummaries::update(const options::ChainSummaryMessage& message) {
    const std::scoped_lock lock(mutex_);
    const auto spot = message.header.instrument_id;
    auto found = std::ranges::find_if(entries_, [&](const Entry& e) {
        return e.summary.spot == spot && e.expiry_days == message.expiry_days;
    });
    if (found == entries_.end()) {
        entries_.push_back({.expiry_days = message.expiry_days});
        found = entries_.end() - 1;
    }
    found->summary = options::from_message(message);
    found->version = ++version_;
}

std::vector<options::ChainSummary> ChainSummaries::changed(std::span<const std::uint32_t> spots,
                                                           SeenVersions& seen) const {
    std::vector<options::ChainSummary> out;
    const std::scoped_lock lock(mutex_);
    for (const std::uint32_t spot : spots) {
        std::uint64_t newest = 0;
        std::uint64_t& sent = seen[spot];
        for (const Entry& entry : entries_) {
            if (entry.summary.spot.value() == spot && entry.version > sent) {
                out.push_back(entry.summary);
                newest = std::max(newest, entry.version);
            }
        }
        sent = std::max(sent, newest);
    }
    return out;
}

std::optional<options::ChainSummary> ChainSummaries::get(std::uint32_t spot, std::int32_t expiry_days) const {
    const std::scoped_lock lock(mutex_);
    for (const Entry& entry : entries_) {
        if (entry.summary.spot.value() == spot && entry.expiry_days == expiry_days) {
            return entry.summary;
        }
    }
    return std::nullopt;
}

json::object chain_summary_json(const options::ChainSummary& s) {
    json::object out{{"underlying", s.underlying},
                     {"spot_id", static_cast<std::int64_t>(s.spot.value())},
                     {"expiry", date_text(s.expiry)},
                     {"pcr", round2(s.pcr)},
                     {"max_pain", s.max_pain},
                     {"call_oi", s.call_oi},
                     {"put_oi", s.put_oi},
                     {"days_to_expiry", round2(s.years * 365.0)}};
    out["forward"] = s.forward > 0 ? json::value(round2(s.forward)) : json::value(nullptr);
    out["atm_strike"] = s.atm_strike > 0 ? json::value(s.atm_strike) : json::value(nullptr);
    out["atm_iv"] = s.atm_iv > 0 ? json::value(round2(s.atm_iv * 100.0)) : json::value(nullptr);
    return out;
}

void register_chain_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                           asio::any_io_executor db_executor, const QuoteCache& quotes,
                           const ChainSummaries& summaries, std::vector<ChainInfo> chains) {
    // GET /chains — underlyings with chains, their spot and listed expiries.
    router.add(http::verb::get, "/api/v1/chains",
               [&auth, &db, db_executor, chains](const Request& request, const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await as_user(auth, db_executor, request, [&](const auth::User&) {
                       json::array out;
                       for (const auto& chain : chains) {
                           auto expiries = db.prepare(
                               "SELECT DISTINCT expiry FROM instruments WHERE kind = 'OPT' AND underlying = ?1 "
                               "AND listed = 1 ORDER BY expiry LIMIT ?2");
                           if (!expiries) {
                               continue;
                           }
                           expiries->bind(1, std::string_view(chain.underlying))
                               .bind(2, static_cast<std::int64_t>(chain.expiries));
                           json::array list;
                           while (expiries->step().value_or(storage::StepResult::Done) == storage::StepResult::Row) {
                               list.emplace_back(expiries->column_text(0));
                           }
                           out.push_back(json::object{{"underlying", chain.underlying},
                                                      {"spot_id", chain_spot_id(db, chain.spot)},
                                                      {"expiries", std::move(list)}});
                       }
                       return success(std::move(out));
                   });
               });

    // GET /chains/{underlying}/{expiry}?strikes=20 — strike rows around ATM.
    router.add(
        http::verb::get, "/api/v1/chains/{underlying}/{expiry}",
        [&auth, &db, db_executor, &quotes, &summaries, chains](const Request& request,
                                                               const PathParams& params) -> asio::awaitable<Response> {
            std::string underlying;
            std::string expiry;
            for (const auto& [name, value] : params) {
                if (name == "underlying") {
                    underlying = value;
                } else if (name == "expiry") {
                    expiry = value;
                }
            }
            const auto chain = std::ranges::find_if(chains, [&](const auto& c) { return c.underlying == underlying; });
            const std::int32_t expiry_days = days_of(expiry);
            if (chain == chains.end() || expiry_days == 0) {
                co_return failure(http::status::not_found, "NotFoundException", "no such chain");
            }
            int strikes = 20;
            if (const auto text = query_param(request, "strikes")) {
                strikes = std::clamp(std::atoi(text->c_str()), 1, 60);
            }
            co_return co_await as_user(auth, db_executor, request, [&](const auth::User&) {
                auto rows = db.prepare(
                    "SELECT id, strike_raw, option_type, lot_size FROM instruments WHERE kind = 'OPT' AND "
                    "underlying = ?1 AND expiry = ?2 AND listed = 1 ORDER BY strike_raw");
                if (!rows) {
                    return failure(http::status::internal_server_error, "GeneralException", "internal error");
                }
                rows->bind(1, std::string_view(underlying)).bind(2, std::string_view(expiry));
                std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> by_strike;  // raw -> (call, put)
                std::int64_t lot_size = 0;
                while (rows->step().value_or(storage::StepResult::Done) == storage::StepResult::Row) {
                    auto& pair = by_strike[rows->column_int64(1)];
                    (rows->column_text(2) == "CE" ? pair.first : pair.second) = rows->column_int64(0);
                    lot_size = rows->column_int64(3);
                }
                if (by_strike.empty()) {
                    return failure(http::status::not_found, "NotFoundException", "no listed options for this expiry");
                }
                const std::int64_t spot = chain_spot_id(db, chain->spot);
                const auto summary = summaries.get(static_cast<std::uint32_t>(spot), expiry_days);
                // ATM: the engine's (nearest the forward), else nearest the spot's last price.
                double centre = summary && summary->atm_strike > 0 ? summary->atm_strike : 0;
                if (centre == 0) {
                    if (const auto quote = quotes.get(static_cast<std::uint32_t>(spot))) {
                        centre = quote->snapshot.ltp.to_double();
                    }
                }
                std::vector<std::int64_t> keys;
                for (const auto& [raw, pair] : by_strike) {
                    keys.push_back(raw);
                }
                std::size_t atm = keys.size() / 2;
                double best = -1;
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    const double gap = std::abs(core::Price::from_raw(keys[i]).to_double() - centre);
                    if (best < 0 || gap < best) {
                        best = gap;
                        atm = i;
                    }
                }
                const std::size_t first = atm > static_cast<std::size_t>(strikes) ? atm - static_cast<std::size_t>(strikes) : 0;
                const std::size_t last = std::min(keys.size() - 1, atm + static_cast<std::size_t>(strikes));
                json::array out_rows;
                for (std::size_t i = first; i <= last; ++i) {
                    const auto& [call, put] = by_strike[keys[i]];
                    out_rows.push_back(json::object{{"strike", core::Price::from_raw(keys[i]).to_double()},
                                                    {"call", call != 0 ? json::value(call) : json::value(nullptr)},
                                                    {"put", put != 0 ? json::value(put) : json::value(nullptr)}});
                }
                json::object out{{"underlying", underlying},
                                 {"expiry", expiry},
                                 {"spot_id", spot},
                                 {"lot_size", lot_size},
                                 {"atm_strike", core::Price::from_raw(keys[atm]).to_double()},
                                 {"rows", std::move(out_rows)}};
                out["summary"] = summary ? json::value(chain_summary_json(*summary)) : json::value(nullptr);
                return success(std::move(out));
            });
        });
}

}  // namespace mda::gateway
