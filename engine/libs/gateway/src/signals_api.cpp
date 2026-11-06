#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/gateway/chain_api.hpp>
#include <mda/gateway/signals_api.hpp>
#include <mda/signals/market_state.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using detail::from_error;
using detail::run_on;
using detail::token_of;

constexpr int kDefaultRecordDays = 30;
constexpr int kMaxRecordDays = 365;

json::value price_or_null(std::int64_t raw) {
    return raw > 0
               ? json::value(static_cast<double>(raw) / static_cast<double>(core::Price::kScale))
               : json::value(nullptr);
}

std::string date_of(std::int64_t t) {
    const std::chrono::year_month_day day{
        std::chrono::sys_days{std::chrono::days{signals::ist_day(t)}}};
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(day.year()),
                  static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
    return text;
}

/// 00:00 IST of "YYYY-MM-DD" as Unix seconds; nullopt if it isn't a valid date.
std::optional<std::int64_t> day_start_of(std::string_view text) {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    const auto digits = [&](std::size_t from, std::size_t count) {
        return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(from),
                           text.begin() + static_cast<std::ptrdiff_t>(from + count),
                           [](char c) { return c >= '0' && c <= '9'; });
    };
    if (text.size() != 10 || text[4] != '-' || text[7] != '-' || !digits(0, 4) || !digits(5, 2) ||
        !digits(8, 2) || std::sscanf(std::string(text).c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3) {
        return std::nullopt;
    }
    const std::chrono::year_month_day day{std::chrono::year{y}, std::chrono::month{m},
                                          std::chrono::day{d}};
    if (!day.ok()) {
        return std::nullopt;
    }
    return std::int64_t{std::chrono::sys_days{day}.time_since_epoch().count()} *
               signals::kSecondsPerDay -
           signals::kIstOffset;
}

json::value rate(int part, int whole) {
    return whole > 0 ? json::value(std::round(1000.0 * part / whole) / 1000.0)
                     : json::value(nullptr);
}

json::object stored_json(const signals::StoredSignal& row) {
    boost::system::error_code ec;
    json::value factors = json::parse(row.factors_json, ec);
    if (ec) {
        factors = json::array{};
    }
    json::value outcome = nullptr;
    if (row.spot_after_raw) {
        outcome = json::object{
            {"spot_after", price_or_null(*row.spot_after_raw)},
            {"return_bp", row.return_bp ? json::value(*row.return_bp) : json::value(nullptr)},
            {"hit", row.hit ? json::value(*row.hit) : json::value(nullptr)}};
    }
    return {{"minute", row.minute},
            {"strategy", row.strategy},
            {"horizon_min", row.horizon_min},
            {"score", row.score},
            {"label", signals::name(row.label)},
            {"prob", row.prob_bp ? json::value(*row.prob_bp / 10'000.0) : json::value(nullptr)},
            {"spot", price_or_null(row.spot_raw)},
            {"factors", std::move(factors)},
            {"outcome", std::move(outcome)}};
}

json::object record_json(const signals::TrackRecord& r) {
    const int decided = r.hits + r.misses;
    return {{"strategy", r.strategy},
            {"horizon_min", r.horizon_min},
            {"settled", r.settled},
            {"bullish", r.bullish},
            {"bearish", r.bearish},
            {"neutral", r.neutral},
            {"hits", r.hits},
            {"misses", r.misses},
            {"flats", r.flats},
            {"hit_rate", rate(r.hits, decided)},
            {"always_up_rate", rate(r.up_moves, decided)},
            {"momentum_rate", rate(r.momentum_hits, r.momentum_calls)},
            {"momentum_calls", r.momentum_calls}};
}

std::string underlying_of(const PathParams& params) {
    for (const auto& [name, value] : params) {
        if (name == "underlying") {
            return value;
        }
    }
    return {};
}

Response internal(const storage::DbError& error) {
    spdlog::error("signals: query failed: {}", error.message);
    return failure(http::status::internal_server_error, "GeneralException", "internal error");
}

}  // namespace

// ------------------------------------------------------------------------ live signals

void LiveSignals::update(const signals::MarketSignal& signal) {
    const std::scoped_lock lock(mutex_);
    auto found = std::ranges::find_if(entries_, [&](const Entry& e) {
        return e.signal.header.instrument_id == signal.header.instrument_id &&
               e.signal.strategy == signal.strategy;
    });
    if (found == entries_.end()) {
        entries_.push_back({});
        found = entries_.end() - 1;
    } else if (found->signal.minute > signal.minute) {
        return;  // an older minute (a re-sent signal) doesn't replace a newer one
    }
    found->signal = signal;
    found->version = ++version_;
}

std::vector<signals::MarketSignal> LiveSignals::changed(std::span<const std::uint32_t> spots,
                                                        SeenVersions& seen) const {
    std::vector<signals::MarketSignal> out;
    const std::scoped_lock lock(mutex_);
    for (const std::uint32_t spot : spots) {
        std::uint64_t& sent = seen[spot];
        std::uint64_t newest = sent;
        for (const Entry& entry : entries_) {
            if (entry.signal.header.instrument_id.value() == spot && entry.version > sent) {
                out.push_back(entry.signal);
                newest = std::max(newest, entry.version);
            }
        }
        sent = newest;
    }
    return out;
}

SignalFeed::SignalFeed(LiveSignals& signals, std::string bus_name)
    : signals_(&signals)
    , bus_name_(std::move(bus_name))
    , thread_([this](const std::stop_token& stop) { run(stop); }) {}

SignalFeed::~SignalFeed() = default;

void SignalFeed::run(const std::stop_token& stop) {
    bool reported = false;
    while (!stop.stop_requested()) {
        auto reader = bus::Reader::attach(bus_name_, {.name = "mda-gateway-sig"});
        if (!reader) {
            if (!reported) {
                spdlog::warn("signals: bus '{}' not available ({}); waiting for mda-signald",
                             bus_name_, bus::to_string(reader.error()));
                reported = true;
            }
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }
        spdlog::info("signals: reading bus '{}'", bus_name_);
        reported = false;
        bus::Backoff wait;
        auto next_heartbeat = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            const auto result =
                reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
                    if (auto signal = messages::decode<signals::MarketSignal>(type, payload)) {
                        signals_->update(*signal);
                    }
                });
            if (result.status == bus::PollStatus::Empty) {
                wait.idle();
            } else {
                wait.reset();
            }
            if (const auto now = std::chrono::steady_clock::now(); now >= next_heartbeat) {
                next_heartbeat = now + std::chrono::seconds(1);
                reader->heartbeat();
            }
        }
    }
}

json::object signal_json(const signals::MarketSignal& signal) {
    json::array top;
    for (const auto& factor : signal.top) {
        if (factor.factor != 0) {
            top.push_back(json::object{
                {"factor", signals::name(static_cast<signals::FactorId>(factor.factor))},
                {"points", factor.points}});
        }
    }
    return {{"spot_id", signal.header.instrument_id.value()},
            {"minute", signal.minute},
            {"strategy", signals::name(static_cast<signals::StrategyId>(signal.strategy))},
            {"horizon_min", signal.horizon_min},
            {"score", signal.score},
            {"label", signals::name(signals::label_of(signal))},
            {"prob",
             signal.prob_bp > 0 ? json::value(signal.prob_bp / 10'000.0) : json::value(nullptr)},
            {"top", std::move(top)}};
}

// ------------------------------------------------------------------------------ REST

void register_signal_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                            asio::any_io_executor db_executor, signals::SignalStore& store,
                            asio::any_io_executor signals_executor,
                            std::vector<SignalIndex> indices,
                            std::vector<SignalStrategy> strategies) {
    // GET /signals — the indices with signals (their spot ids and latest signal per
    // strategy), the strategies and the factor names.
    router.add(
        http::verb::get, "/api/v1/signals",
        [&auth, &db, db_executor, &store, signals_executor, indices, strategies](
            const Request& request, const PathParams&) -> asio::awaitable<Response> {
            const std::string token = token_of(request);
            const auto spot_ids = co_await run_on(
                db_executor, [&]() -> std::expected<std::vector<std::int64_t>, auth::AuthError> {
                    const auto user = auth.authenticate(token);
                    if (!user) {
                        return std::unexpected(user.error());
                    }
                    std::vector<std::int64_t> ids;
                    for (const auto& index : indices) {
                        ids.push_back(chain_spot_id(db, index.spot));
                    }
                    return ids;
                });
            if (!spot_ids) {
                co_return from_error(spot_ids.error());
            }
            co_return co_await run_on(signals_executor, [&]() -> Response {
                json::array index_list;
                for (std::size_t i = 0; i < indices.size(); ++i) {
                    const auto latest = store.latest_minute(indices[i].underlying);
                    if (!latest) {
                        return internal(latest.error());
                    }
                    json::array newest;
                    if (*latest) {
                        const auto rows =
                            store.range(indices[i].underlying, **latest, **latest + 60);
                        if (!rows) {
                            return internal(rows.error());
                        }
                        for (const auto& row : *rows) {
                            newest.push_back(stored_json(row));
                        }
                    }
                    index_list.push_back(json::object{{"underlying", indices[i].underlying},
                                                      {"spot", indices[i].spot},
                                                      {"spot_id", (*spot_ids)[i]},
                                                      {"latest", std::move(newest)}});
                }
                json::array strategy_list;
                for (const auto& strategy : strategies) {
                    strategy_list.push_back(json::object{{"id", strategy.id},
                                                         {"horizon_min", strategy.horizon_min},
                                                         {"bullish", strategy.bullish},
                                                         {"bearish", strategy.bearish}});
                }
                json::array factors;
                for (std::size_t i = 1; i <= signals::kFactorCount; ++i) {
                    factors.push_back(
                        json::string(signals::name(static_cast<signals::FactorId>(i))));
                }
                return success(json::object{{"indices", std::move(index_list)},
                                            {"strategies", std::move(strategy_list)},
                                            {"factors", std::move(factors)}});
            });
        });

    // GET /signals/{underlying}?date=YYYY-MM-DD — one IST day's signals, oldest first;
    // without a date, the latest day that has any.
    router.add(
        http::verb::get, "/api/v1/signals/{underlying}",
        [&auth, db_executor, &store, signals_executor, indices](
            const Request& request, const PathParams& params) -> asio::awaitable<Response> {
            const std::string underlying = underlying_of(params);
            if (std::ranges::find(indices, underlying, &SignalIndex::underlying) == indices.end()) {
                co_return failure(http::status::not_found, "NotFoundException",
                                  "no signals for this underlying");
            }
            std::optional<std::int64_t> from;
            if (const auto date = query_param(request, "date")) {
                from = day_start_of(*date);
                if (!from) {
                    co_return detail::bad_input("date must be YYYY-MM-DD");
                }
            }
            const std::string token = token_of(request);
            const auto user =
                co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            co_return co_await run_on(signals_executor, [&]() -> Response {
                if (!from) {
                    const auto latest = store.latest_minute(underlying);
                    if (!latest) {
                        return internal(latest.error());
                    }
                    if (!*latest) {
                        return success(json::object{{"underlying", underlying},
                                                    {"date", nullptr},
                                                    {"signals", json::array{}}});
                    }
                    from = signals::ist_day_start(**latest);
                }
                const auto rows = store.range(underlying, *from, *from + signals::kSecondsPerDay);
                if (!rows) {
                    return internal(rows.error());
                }
                json::array list;
                list.reserve(rows->size());
                for (const auto& row : *rows) {
                    list.push_back(stored_json(row));
                }
                return success(json::object{{"underlying", underlying},
                                            {"date", date_of(*from)},
                                            {"signals", std::move(list)}});
            });
        });

    // GET /signals/{underlying}/record?days=30 — each strategy's track record over the
    // last `days` calendar days (today included).
    router.add(
        http::verb::get, "/api/v1/signals/{underlying}/record",
        [&auth, db_executor, &store, signals_executor, indices](
            const Request& request, const PathParams& params) -> asio::awaitable<Response> {
            const std::string underlying = underlying_of(params);
            if (std::ranges::find(indices, underlying, &SignalIndex::underlying) == indices.end()) {
                co_return failure(http::status::not_found, "NotFoundException",
                                  "no signals for this underlying");
            }
            int days = kDefaultRecordDays;
            if (const auto text = query_param(request, "days")) {
                const auto [end, error] =
                    std::from_chars(text->data(), text->data() + text->size(), days);
                if (error != std::errc{} || end != text->data() + text->size() || days < 1 ||
                    days > kMaxRecordDays) {
                    co_return detail::bad_input("days must be 1-365");
                }
            }
            const std::string token = token_of(request);
            const auto user =
                co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            const std::int64_t today = signals::ist_day_start(auth::system_clock_seconds());
            const std::int64_t from = today - std::int64_t{days - 1} * signals::kSecondsPerDay;
            co_return co_await run_on(signals_executor, [&]() -> Response {
                const auto records =
                    store.record(underlying, from, today + signals::kSecondsPerDay);
                if (!records) {
                    return internal(records.error());
                }
                json::array list;
                for (const auto& record : *records) {
                    list.push_back(record_json(record));
                }
                return success(json::object{{"underlying", underlying},
                                            {"from", date_of(from)},
                                            {"days", days},
                                            {"strategies", std::move(list)}});
            });
        });
}

}  // namespace mda::gateway
