#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/gateway/oi_change_api.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using detail::from_error;
using detail::run_on;
using detail::token_of;

constexpr std::int64_t kIst = 19'800;
constexpr std::int64_t kSessionOpen = 9 * 3600 + 15 * 60;  // IST, from midnight
/// How far back "OI as of T" looks: a week covers weekends and holidays.
constexpr std::int64_t kLookback = 7 * 86'400;
constexpr int kMaxStrikes = 20;  // the feed subscribes ATM ± 20

std::int64_t ist_midnight(std::int64_t t) {
    return t - (((t + kIst) % 86'400) + 86'400) % 86'400;
}

std::int64_t floor_minute(std::int64_t t) {
    return t - ((t % 60) + 60) % 60;
}

std::optional<std::int64_t> int_param(const Request& request, std::string_view name, bool& bad) {
    const auto text = query_param(request, name);
    if (!text) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size()) {
        bad = true;
        return std::nullopt;
    }
    return value;
}

struct Option {
    std::uint32_t id = 0;
    std::int64_t strike_raw = 0;
    bool call = false;
};

/// What the catalog says about the request (read on the app DB's thread).
struct Catalog {
    std::vector<std::string> expiries;
    std::uint32_t spot = 0;
    std::vector<Option> options;
};

/// The chain's listed expiries (as GET /chains shows them), the ticked ones (default: the
/// nearest) and their options. Error text "internal" for a DB failure, else a 400 message.
std::expected<Catalog, std::string> load_catalog(storage::Database& db, const ChainInfo& chain,
                                                 const std::vector<std::string>& wanted) {
    const std::string_view underlying = chain.underlying;
    auto listed = db.prepare(
        "SELECT DISTINCT expiry FROM instruments WHERE kind = 'OPT' AND underlying = ?1 AND listed "
        "= 1 "
        "ORDER BY expiry LIMIT ?2");
    if (!listed) {
        return std::unexpected("internal");
    }
    listed->bind(1, underlying).bind(2, static_cast<std::int64_t>(chain.expiries));
    std::vector<std::string> expiries;
    while (listed->step().value_or(storage::StepResult::Done) == storage::StepResult::Row) {
        expiries.push_back(listed->column_text(0));
    }
    Catalog out;
    if (wanted.empty() && !expiries.empty()) {
        out.expiries.push_back(expiries.front());
    }
    for (const auto& expiry : wanted) {
        if (std::ranges::find(expiries, expiry) == expiries.end()) {
            return std::unexpected("unknown expiry " + expiry);
        }
        if (std::ranges::find(out.expiries, expiry) == out.expiries.end()) {
            out.expiries.push_back(expiry);
        }
    }
    auto options = db.prepare(
        "SELECT id, strike_raw, option_type FROM instruments WHERE kind = 'OPT' AND underlying = "
        "?1 AND "
        "expiry = ?2 AND listed = 1");
    if (!options) {
        return std::unexpected("internal");
    }
    for (const auto& expiry : out.expiries) {
        options->reset();
        options->bind(1, underlying).bind(2, std::string_view(expiry));
        while (options->step().value_or(storage::StepResult::Done) == storage::StepResult::Row) {
            out.options.push_back({.id = static_cast<std::uint32_t>(options->column_int64(0)),
                                   .strike_raw = options->column_int64(1),
                                   .call = options->column_text(2) == "CE"});
        }
    }
    out.spot = static_cast<std::uint32_t>(chain_spot_id(db, chain.spot));
    return out;
}

/// ATM ± `strikes` over the union of the ticked expiries' strikes: keys[first, end).
struct Window {
    std::vector<std::int64_t> keys;  ///< all strikes, raw, ascending
    std::size_t first = 0;
    std::size_t end = 0;
    std::size_t atm = 0;
};

Window strike_window(const Catalog& catalog, double spot, int strikes) {
    Window w;
    for (const Option& o : catalog.options) {
        w.keys.push_back(o.strike_raw);
    }
    std::ranges::sort(w.keys);
    w.keys.erase(std::ranges::unique(w.keys).begin(), w.keys.end());
    const auto nearest = std::ranges::min_element(w.keys, {}, [&](std::int64_t raw) {
        return std::abs(core::Price::from_raw(raw).to_double() - spot);
    });
    w.atm = static_cast<std::size_t>(nearest - w.keys.begin());
    const auto n = static_cast<std::size_t>(strikes);
    w.first = w.atm > n ? w.atm - n : 0;
    w.end = std::min(w.keys.size(), w.atm + n + 1);
    return w;
}

/// A step series: (minute index from the open, value), one point per change. A value holds
/// from the end of minute m (session_open + m * 60) until the next point.
using Series = std::vector<std::pair<std::int64_t, std::int64_t>>;

/// OI was published in this bar (a live update, or a backfilled bar with OI): a timeline point.
bool oi_published(const bars::Bar& bar) {
    return bar.oi_close.value() > 0 && (bar.oi_updates > 0 || bar.source == bars::BarSource::Kite);
}

/// The sum of step series (one per ticked expiry): at each change of any, the sum of those known.
Series sum_series(const std::vector<Series>& parts) {
    std::vector<std::int64_t> times;
    for (const Series& part : parts) {
        for (const auto& point : part) {
            times.push_back(point.first);
        }
    }
    std::ranges::sort(times);
    times.erase(std::ranges::unique(times).begin(), times.end());
    std::vector<std::size_t> next(parts.size(), 0);
    Series out;
    for (const std::int64_t t : times) {
        std::int64_t sum = 0;
        bool known = false;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            while (next[i] < parts[i].size() && parts[i][next[i]].first <= t) {
                ++next[i];
            }
            if (next[i] > 0) {
                sum += parts[i][next[i] - 1].second;
                known = true;
            }
        }
        if (known && (out.empty() || out.back().second != sum)) {
            out.emplace_back(t, sum);
        }
    }
    return out;
}

json::array series_json(const Series& series) {
    json::array out;
    out.reserve(series.size());
    for (const auto& [minute, value] : series) {
        out.push_back(json::array{minute, value});
    }
    return out;
}

/// One side of a strike: OI summed over the ticked expiries that have a value.
struct Side {
    std::optional<std::int64_t> from;
    std::optional<std::int64_t> to;
};

void add(std::optional<std::int64_t>& sum, const std::unordered_map<std::uint32_t, bars::Bar>& bars,
         std::uint32_t id) {
    if (const auto it = bars.find(id); it != bars.end()) {
        sum = sum.value_or(0) + it->second.oi_close.value();
    }
}

json::value maybe(const std::optional<std::int64_t>& value) {
    return value ? json::value(*value) : json::value(nullptr);
}

std::vector<std::string> split_csv(std::string_view text) {
    std::vector<std::string> out;
    for (const auto part : std::views::split(text, ',')) {
        if (const std::string_view item(part.begin(), part.end()); !item.empty()) {
            out.emplace_back(item);
        }
    }
    return out;
}

}  // namespace

void register_oi_change_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                               asio::any_io_executor db_executor, bars::BarStore& store,
                               asio::any_io_executor bars_executor, std::vector<ChainInfo> chains) {
    // GET /chains/{underlying}/oi-change?expiries=a,b&from=<unix>|minutes=30&to=<unix>&strikes=10
    router.add(
        http::verb::get, "/api/v1/chains/{underlying}/oi-change",
        [&auth, &db, db_executor, &store, bars_executor, chains](
            const Request& request, const PathParams& params) -> asio::awaitable<Response> {
            const std::string underlying = params.empty() ? std::string() : params.front().second;
            const auto chain = std::ranges::find_if(
                chains, [&](const auto& c) { return c.underlying == underlying; });
            if (chain == chains.end()) {
                co_return failure(http::status::not_found, "NotFoundException", "no such chain");
            }
            bool bad = false;
            const auto from_param = int_param(request, "from", bad);
            const auto to_param = int_param(request, "to", bad);
            const auto strikes_param = int_param(request, "strikes", bad);
            const auto minutes_param = int_param(request, "minutes", bad);
            if (bad || (strikes_param && (*strikes_param < 1 || *strikes_param > kMaxStrikes)) ||
                (minutes_param && (*minutes_param < 1 || *minutes_param > 24 * 60 || from_param))) {
                co_return detail::bad_input(
                    "from and to are Unix seconds; strikes is 1-20; minutes is 1-1440, without "
                    "from");
            }
            const int strikes = static_cast<int>(strikes_param.value_or(10));
            const auto wanted = split_csv(query_param(request, "expiries").value_or(""));

            const std::string token = token_of(request);
            const auto user =
                co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            // The chain's listed expiries (as GET /chains shows them), the ticked ones, their
            // options.
            const auto catalog =
                co_await run_on(db_executor, [&] { return load_catalog(db, *chain, wanted); });
            if (!catalog) {
                co_return catalog.error() == "internal"
                    ? failure(http::status::internal_server_error, "GeneralException",
                              "internal error")
                    : detail::bad_input(catalog.error());
            }
            if (catalog->options.empty() || catalog->spot == 0) {
                co_return failure(http::status::not_found, "NotFoundException",
                                  "no listed options for this chain");
            }

            co_return co_await run_on(bars_executor, [&]() -> Response {
                const auto internal = [](const storage::DbError& error) {
                    spdlog::error("oi-change: query failed: {}", error.message);
                    return failure(http::status::internal_server_error, "GeneralException",
                                   "internal error");
                };
                const std::uint32_t spot_ids[] = {catalog->spot};
                const auto spot_at =
                    [&](std::int64_t t) -> std::expected<std::optional<double>, storage::DbError> {
                    auto bars = store.last_ended(spot_ids, t, t - kLookback, false);
                    if (!bars) {
                        return std::unexpected(bars.error());
                    }
                    if (bars->empty()) {
                        return std::optional<double>{};
                    }
                    return std::optional<double>{bars->begin()->second.close.to_double()};
                };

                // "Now" is the end of the spot's last closed minute; its IST day is the session.
                const auto last = store.last_ended(
                    spot_ids, floor_minute(auth::system_clock_seconds()), 0, false);
                if (!last) {
                    return internal(last.error());
                }
                if (last->empty()) {
                    return failure(http::status::not_found, "NotFoundException",
                                   "no bars for this chain yet");
                }
                const std::int64_t latest = last->begin()->second.start + 60;
                const std::int64_t to = to_param ? floor_minute(*to_param) : latest;
                const std::int64_t session_open = ist_midnight(to - 1) + kSessionOpen;
                // "Last N minutes" counts back from `to`, never before the open.
                const std::int64_t from = from_param ? floor_minute(*from_param)
                                          : minutes_param
                                              ? std::max(session_open, to - *minutes_param * 60)
                                              : session_open;
                if (from >= to || ist_midnight(from) != ist_midnight(to - 1)) {
                    return detail::bad_input("from must be before to, on the same day (IST)");
                }

                const auto spot_from = spot_at(from);
                const auto spot_to = spot_at(to);
                if (!spot_from || !spot_to) {
                    return internal(!spot_from ? spot_from.error() : spot_to.error());
                }
                if (!*spot_to) {
                    return failure(http::status::not_found, "NotFoundException",
                                   "no spot price at that time");
                }

                const Window window = strike_window(*catalog, **spot_to, strikes);
                const auto& keys = window.keys;
                const std::size_t first = window.first;
                const std::size_t end = window.end;
                const std::size_t atm = window.atm;
                const std::int64_t low = keys[first];
                const std::int64_t high = keys[end - 1];

                std::vector<std::uint32_t> ids;
                for (const Option& o : catalog->options) {
                    if (o.strike_raw >= low && o.strike_raw <= high) {
                        ids.push_back(o.id);
                    }
                }
                const auto oi_from = store.last_ended(ids, from, from - kLookback, true);
                const auto oi_to = store.last_ended(ids, to, to - kLookback, true);
                if (!oi_from || !oi_to) {
                    return internal(!oi_from ? oi_from.error() : oi_to.error());
                }

                std::map<std::int64_t, std::pair<Side, Side>> by_strike;  // raw -> (call, put)
                for (std::size_t i = first; i < end; ++i) {
                    by_strike[keys[i]];
                }
                for (const Option& o : catalog->options) {
                    const auto row = by_strike.find(o.strike_raw);
                    if (row == by_strike.end()) {
                        continue;
                    }
                    Side& side = o.call ? row->second.first : row->second.second;
                    add(side.from, *oi_from, o.id);
                    add(side.to, *oi_to, o.id);
                }
                // Totals over the sides known at both times, so they are changes of the same
                // strikes.
                std::int64_t totals[4] = {};  // call_from, call_to, put_from, put_to
                json::array rows;
                for (const auto& [raw, sides] : by_strike) {
                    const auto& [call, put] = sides;
                    if (call.from && call.to) {
                        totals[0] += *call.from;
                        totals[1] += *call.to;
                    }
                    if (put.from && put.to) {
                        totals[2] += *put.from;
                        totals[3] += *put.to;
                    }
                    rows.push_back(json::object{{"strike", core::Price::from_raw(raw).to_double()},
                                                {"call_from", maybe(call.from)},
                                                {"call_to", maybe(call.to)},
                                                {"put_from", maybe(put.from)},
                                                {"put_to", maybe(put.to)}});
                }
                std::optional<std::int64_t> oi_as_of;
                for (const auto& [id, bar] : *oi_to) {
                    if (bar.oi_update_offset != bars::kNoOiUpdateTime) {
                        oi_as_of = std::max(oi_as_of.value_or(0), bar.start + bar.oi_update_offset);
                    }
                }
                json::array expiries;
                for (const auto& expiry : catalog->expiries) {
                    expiries.emplace_back(expiry);
                }
                return success(json::object{
                    {"underlying", underlying},
                    {"expiries", std::move(expiries)},
                    {"from", from},
                    {"to", to},
                    {"session_open", session_open},
                    {"latest", latest},
                    {"oi_as_of", maybe(oi_as_of)},
                    {"spot_from", *spot_from ? json::value(**spot_from) : json::value(nullptr)},
                    {"spot_to", **spot_to},
                    {"atm_strike", core::Price::from_raw(keys[atm]).to_double()},
                    {"totals", json::object{{"call_from", totals[0]},
                                            {"call_to", totals[1]},
                                            {"put_from", totals[2]},
                                            {"put_to", totals[3]}}},
                    {"pcr", totals[1] > 0
                                ? json::value(std::round(static_cast<double>(totals[3]) /
                                                         static_cast<double>(totals[1]) * 100.0) /
                                              100.0)
                                : json::value(nullptr)},
                    {"rows", std::move(rows)}});
            });
        });

    // GET /chains/{underlying}/oi-timeline?expiries=a,b&strikes=10 — the session's OI per
    // strike as step series, so the browser can compute any window while a slider is dragged
    // (docs/design/oi-change-by-strike.md §8).
    router.add(
        http::verb::get, "/api/v1/chains/{underlying}/oi-timeline",
        [&auth, &db, db_executor, &store, bars_executor, chains](
            const Request& request, const PathParams& params) -> asio::awaitable<Response> {
            const std::string underlying = params.empty() ? std::string() : params.front().second;
            const auto chain = std::ranges::find_if(
                chains, [&](const auto& c) { return c.underlying == underlying; });
            if (chain == chains.end()) {
                co_return failure(http::status::not_found, "NotFoundException", "no such chain");
            }
            bool bad = false;
            const auto strikes_param = int_param(request, "strikes", bad);
            if (bad || (strikes_param && (*strikes_param < 1 || *strikes_param > kMaxStrikes))) {
                co_return detail::bad_input("strikes is 1-20");
            }
            const int strikes = static_cast<int>(strikes_param.value_or(10));
            const auto wanted = split_csv(query_param(request, "expiries").value_or(""));

            const std::string token = token_of(request);
            const auto user =
                co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            const auto catalog =
                co_await run_on(db_executor, [&] { return load_catalog(db, *chain, wanted); });
            if (!catalog) {
                co_return catalog.error() == "internal"
                    ? failure(http::status::internal_server_error, "GeneralException",
                              "internal error")
                    : detail::bad_input(catalog.error());
            }
            if (catalog->options.empty() || catalog->spot == 0) {
                co_return failure(http::status::not_found, "NotFoundException",
                                  "no listed options for this chain");
            }

            co_return co_await run_on(bars_executor, [&]() -> Response {
                const auto internal = [](const storage::DbError& error) {
                    spdlog::error("oi-timeline: query failed: {}", error.message);
                    return failure(http::status::internal_server_error, "GeneralException",
                                   "internal error");
                };
                const std::uint32_t spot_ids[] = {catalog->spot};
                const auto last = store.last_ended(
                    spot_ids, floor_minute(auth::system_clock_seconds()), 0, false);
                if (!last) {
                    return internal(last.error());
                }
                if (last->empty()) {
                    return failure(http::status::not_found, "NotFoundException",
                                   "no bars for this chain yet");
                }
                const std::int64_t latest = last->begin()->second.start + 60;
                const std::int64_t session_open = ist_midnight(latest - 1) + kSessionOpen;
                const auto minute_of = [&](const bars::Bar& bar) {
                    return (bar.start + 60 - session_open) / 60;
                };

                // Spot: the close before the open, then each minute's.
                json::array spot;
                const auto spot_before =
                    store.last_ended(spot_ids, session_open, session_open - kLookback, false);
                const auto spot_bars = store.range(catalog->spot, session_open, latest);
                if (!spot_before || !spot_bars) {
                    return internal(!spot_before ? spot_before.error() : spot_bars.error());
                }
                if (!spot_before->empty()) {
                    spot.push_back(json::array{0, spot_before->begin()->second.close.to_double()});
                }
                for (const bars::Bar& bar : *spot_bars) {
                    spot.push_back(json::array{minute_of(bar), bar.close.to_double()});
                }
                const double spot_latest = last->begin()->second.close.to_double();

                const Window window = strike_window(*catalog, spot_latest, strikes);
                const std::int64_t low = window.keys[window.first];
                const std::int64_t high = window.keys[window.end - 1];
                std::vector<std::uint32_t> ids;
                for (const Option& o : catalog->options) {
                    if (o.strike_raw >= low && o.strike_raw <= high) {
                        ids.push_back(o.id);
                    }
                }
                const auto before =
                    store.last_ended(ids, session_open, session_open - kLookback, true);
                if (!before) {
                    return internal(before.error());
                }
                // Per strike: the call and put series of each ticked expiry, summed below.
                std::map<std::int64_t, std::pair<std::vector<Series>, std::vector<Series>>>
                    by_strike;
                for (std::size_t i = window.first; i < window.end; ++i) {
                    by_strike[window.keys[i]];
                }
                std::optional<std::int64_t> oi_as_of;
                for (const Option& o : catalog->options) {
                    const auto row = by_strike.find(o.strike_raw);
                    if (row == by_strike.end()) {
                        continue;
                    }
                    Series series;
                    if (const auto it = before->find(o.id); it != before->end()) {
                        series.emplace_back(0, it->second.oi_close.value());
                    }
                    const auto minutes = store.range(o.id, session_open, latest);
                    if (!minutes) {
                        return internal(minutes.error());
                    }
                    for (const bars::Bar& bar : *minutes) {
                        if (!oi_published(bar)) {
                            continue;
                        }
                        if (bar.oi_update_offset != bars::kNoOiUpdateTime) {
                            oi_as_of =
                                std::max(oi_as_of.value_or(0), bar.start + bar.oi_update_offset);
                        }
                        if (series.empty() || series.back().second != bar.oi_close.value()) {
                            series.emplace_back(minute_of(bar), bar.oi_close.value());
                        }
                    }
                    (o.call ? row->second.first : row->second.second).push_back(std::move(series));
                }
                json::array rows;
                for (const auto& [raw, sides] : by_strike) {
                    rows.push_back(json::object{{"strike", core::Price::from_raw(raw).to_double()},
                                                {"call", series_json(sum_series(sides.first))},
                                                {"put", series_json(sum_series(sides.second))}});
                }
                json::array expiries;
                for (const auto& expiry : catalog->expiries) {
                    expiries.emplace_back(expiry);
                }
                return success(json::object{
                    {"underlying", underlying},
                    {"expiries", std::move(expiries)},
                    {"session_open", session_open},
                    {"latest", latest},
                    {"oi_as_of", maybe(oi_as_of)},
                    {"atm_strike", core::Price::from_raw(window.keys[window.atm]).to_double()},
                    {"spot", std::move(spot)},
                    {"rows", std::move(rows)}});
            });
        });
}

}  // namespace mda::gateway
