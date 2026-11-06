// mda-backtest — runs the market signal strategies over past days from bars.db
// (ADR-0028, docs/design/signals.md §12): a check that each factor behaves, not a track
// record.
//
//   mda-backtest [--config config/mda.yaml] [--days 10] [--out data/research/signals_backtest.db]
//
// For each of the last `days` trading days (those with spot bars, today excluded) and each
// index in signals.underlyings: feeds that day's 1-minute bars (spot, near-month future,
// the nearest chain expiries' options, sector indices) minute by minute into the same
// MarketState mda-signald uses, rebuilds each minute's chain summary with the engine's
// ChainAnalytics, runs the configured strategies, and settles every signal against the
// day's spot closes. Signals and outcomes go to `--out` (never data/signals.db); a report
// per index and factor goes to stdout.
//
// Limits: only contracts still in bars.db (live-recorded or backfilled) take part; the
// chain summaries come from minute closes, not ticks. Reads bars.db only.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <mda/bars/bar_store.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/options/chain_analytics.hpp>
#include <mda/runtime/config.hpp>
#include <mda/signals/market_state.hpp>
#include <mda/signals/outcome.hpp>
#include <mda/signals/signal_store.hpp>
#include <mda/signals/strategies.hpp>

namespace {

using namespace mda;

std::int32_t days_since_epoch(std::chrono::year_month_day day) {
    return static_cast<std::int32_t>(std::chrono::sys_days{day}.time_since_epoch().count());
}

std::string date_of(std::int32_t day) {
    const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{day}}};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                       static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
}

/// Running statistics of one factor against the forward return.
struct FactorStats {
    std::size_t evaluated = 0;
    std::size_t present = 0;
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0, sum_xy = 0;
    std::size_t agree = 0;     ///< |value| ≥ 0.2 and the move beyond the flat band went its way
    std::size_t disagree = 0;  ///< … and the other way

    void add(double x, double y, int flat_bp) {
        ++present;
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_yy += y * y;
        sum_xy += x * y;
        if (std::abs(x) >= 0.2 && std::abs(y) > flat_bp) {
            ((x > 0) == (y > 0) ? agree : disagree) += 1;
        }
    }

    [[nodiscard]] std::optional<double> correlation() const {
        if (present < 30) {
            return std::nullopt;
        }
        const auto n = static_cast<double>(present);
        const double cov = sum_xy / n - (sum_x / n) * (sum_y / n);
        const double vx = sum_xx / n - (sum_x / n) * (sum_x / n);
        const double vy = sum_yy / n - (sum_y / n) * (sum_y / n);
        if (vx <= 1e-12 || vy <= 1e-12) {
            return std::nullopt;
        }
        return cov / std::sqrt(vx * vy);
    }
};

struct IndexReport {
    std::map<signals::FactorId, FactorStats> factors;
    FactorStats score;  ///< the score / 100 as one more "factor"
    std::size_t signals = 0;
};

std::string percent(std::size_t part, std::size_t whole) {
    return whole == 0 ? "   —"
                      : std::format("{:3.0f}%",
                                    100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

std::string stats_line(std::string_view name, const FactorStats& s, std::size_t evaluated) {
    const auto corr = s.correlation();
    return std::format("  {:<16} present {}  mean {:+.2f}  corr {}  direction right {} of {:>4}",
                       name, percent(s.present, evaluated),
                       s.present > 0 ? s.sum_x / static_cast<double>(s.present) : 0.0,
                       corr ? std::format("{:+.2f}", *corr) : std::string("   — "),
                       percent(s.agree, s.agree + s.disagree), s.agree + s.disagree);
}

struct Options {
    std::filesystem::path config = "config/mda.yaml";
    int days = 10;
    std::filesystem::path out = "data/research/signals_backtest.db";
};

std::optional<Options> parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        if (arg == "--config") {
            options.config = argv[++i];
        } else if (arg == "--days") {
            options.days = std::atoi(argv[++i]);
        } else if (arg == "--out") {
            options.out = argv[++i];
        } else {
            return std::nullopt;
        }
    }
    if (options.days < 1 || options.days > 60) {
        return std::nullopt;
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse_args(argc, argv);
    if (!options) {
        std::fprintf(stderr,
                     "usage: mda-backtest [--config config/mda.yaml] [--days 1-60] [--out FILE]\n");
        return 2;
    }
    const auto config = runtime::load_config(options->config);
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    if (options->out == config->signals.db) {
        std::fprintf(stderr, "error: --out must not be the live signals.db\n");
        return 1;
    }
    auto bars = bars::BarStore::open(config->engine.bars_db);
    auto out = signals::SignalStore::open(options->out);
    auto loaded = instruments::InstrumentRegistry::load(config->kite.registry_file);
    if (!bars || !out || !loaded) {
        std::fprintf(stderr, "error: cannot open bars.db, %s or the registry\n",
                     options->out.string().c_str());
        return 1;
    }
    const auto& registry = loaded->registry;
    std::vector<std::unique_ptr<signals::Strategy>> strategies;
    for (const auto& s : config->signals.strategies) {
        if (auto made = signals::make_strategy(s.id, s.horizon_min, s.bullish, s.bearish)) {
            strategies.push_back(std::move(made));
        }
    }
    if (strategies.empty() || config->signals.underlyings.empty()) {
        std::fprintf(stderr, "error: no signals.strategies or signals.underlyings configured\n");
        return 1;
    }

    // Trading days: those with bars of the first index's spot, today excluded.
    const std::int64_t now = core::Timestamp::now().ns() / 1'000'000'000;
    const auto spot_of = [&](const std::string& underlying) -> core::InstrumentId {
        const auto chain =
            std::ranges::find(config->feed.chains, underlying, &runtime::ChainConfig::underlying);
        if (chain == config->feed.chains.end()) {
            return {};
        }
        const auto key = instruments::parse_contract_key(chain->spot);
        return key ? registry.find(*key) : core::InstrumentId{};
    };
    std::set<std::int32_t> trading_days;
    if (const auto spot = spot_of(config->signals.underlyings.front()); spot.valid()) {
        const std::int64_t today = signals::ist_day_start(now);
        if (const auto history =
                bars->range(spot.value(), today - 90 * signals::kSecondsPerDay, today)) {
            for (const auto& bar : *history) {
                trading_days.insert(signals::ist_day(bar.start));
            }
        }
    }
    std::vector<std::int32_t> days(trading_days.begin(), trading_days.end());
    if (days.size() > static_cast<std::size_t>(options->days)) {
        days.erase(days.begin(), days.end() - options->days);
    }
    if (days.empty()) {
        std::fprintf(stderr, "error: no past trading days with spot bars in bars.db\n");
        return 1;
    }
    std::printf("Backtest of %zu trading days, %s to %s, into %s\n", days.size(),
                date_of(days.front()).c_str(), date_of(days.back()).c_str(),
                options->out.string().c_str());

    std::vector<core::InstrumentId> sectors;
    for (const auto& text : config->signals.sectors) {
        const auto key = instruments::parse_contract_key(text);
        if (const auto id = key ? registry.find(*key) : core::InstrumentId{}; id.valid()) {
            sectors.push_back(id);
        }
    }

    core::InstrumentId vix;
    if (const auto key = instruments::parse_contract_key(config->signals.vix); key) {
        vix = registry.find(*key);
    }

    std::map<std::string, IndexReport> reports;
    for (const auto& underlying : config->signals.underlyings) {
        const auto chain =
            std::ranges::find(config->feed.chains, underlying, &runtime::ChainConfig::underlying);
        const core::InstrumentId spot = spot_of(underlying);
        if (!spot.valid()) {
            std::printf("%s: no spot; skipped\n", underlying.c_str());
            continue;
        }
        IndexReport& report = reports[underlying];
        for (const std::int32_t day : days) {
            const std::int64_t day_start =
                std::int64_t{day} * signals::kSecondsPerDay - signals::kIstOffset;

            // That day's instruments: the nearest future, the nearest chain expiries' options.
            std::optional<std::pair<std::int32_t, core::InstrumentId>> future;
            std::map<std::int32_t, std::vector<const instruments::Instrument*>> by_expiry;
            for (const auto& instrument : registry.all()) {
                const auto& k = instrument.key;
                if (k.underlying != underlying || !k.expiry.ok()) {
                    continue;
                }
                const std::int32_t expiry = days_since_epoch(k.expiry);
                if (expiry < day) {
                    continue;
                }
                if (k.kind == instruments::Kind::Future && (!future || expiry < future->first)) {
                    future = {expiry, instrument.id};
                } else if (k.kind == instruments::Kind::Option) {
                    by_expiry[expiry].push_back(&instrument);
                }
            }
            signals::MarketState state({.underlying = underlying,
                                        .spot = spot,
                                        .future = future ? future->second : core::InstrumentId{}});
            options::ChainDefinition definition{
                .underlying = underlying, .spot = spot, .expiries = {}};
            std::vector<core::InstrumentId> ids = {spot};
            if (future) {
                ids.push_back(future->second);
            }
            for (const auto& [expiry, contracts] : by_expiry) {
                if (static_cast<int>(definition.expiries.size()) >= chain->expiries) {
                    break;
                }
                options::ExpiryChain expiry_chain{
                    .expiry = std::chrono::year_month_day{std::chrono::sys_days{
                        std::chrono::days{expiry}}},
                    .options = {}};
                for (const auto* option : contracts) {
                    const bool call = option->key.option_type == instruments::OptionType::Call;
                    expiry_chain.options.push_back(
                        {.id = option->id, .strike = option->key.strike, .call = call});
                    state.add_option({.id = option->id,
                                      .expiry_days = expiry,
                                      .strike = option->key.strike.to_double(),
                                      .call = call});
                    ids.push_back(option->id);
                }
                definition.expiries.push_back(std::move(expiry_chain));
            }
            options::ChainAnalytics analytics({definition});
            for (const auto id : sectors) {
                const auto previous = bars->latest_before(id.value(), day_start);
                state.add_sector(id, previous && *previous ? (*previous)->close.to_double() : 0);
                ids.push_back(id);
            }
            if (vix.valid()) {
                state.set_vix(vix);
                ids.push_back(vix);
            }

            // The day's bars by minute; options also get yesterday's closing OI.
            std::map<std::int64_t, std::vector<bars::Bar>> minutes;
            for (const auto id : ids) {
                const auto day_bars =
                    bars->range(id.value(), day_start, day_start + signals::kSecondsPerDay);
                if (!day_bars) {
                    continue;
                }
                for (const auto& bar : *day_bars) {
                    minutes[bar.start].push_back(bar);
                }
                if (const auto previous = bars->latest_before(id.value(), day_start);
                    previous && *previous && (*previous)->oi_close.value() > 0) {
                    analytics.set_prev_oi(id, (*previous)->oi_close.value());
                }
            }

            std::map<std::uint32_t, std::int64_t> day_volume;
            std::vector<signals::SpotClose> spot_closes;
            std::vector<signals::SignalRecord> records;
            std::vector<options::OptionResult> option_results;
            std::vector<options::ChainSummary> summaries;
            for (const auto& [minute, minute_bars] : minutes) {
                bool spot_closed = false;
                for (const auto& bar : minute_bars) {
                    state.on_bar(bar);
                    auto& volume = day_volume[bar.header.instrument_id.value()];
                    volume += bar.volume.value();
                    messages::MarketSnapshot snapshot{};
                    snapshot.header.instrument_id = bar.header.instrument_id;
                    snapshot.fields =
                        messages::kFieldLtp | messages::kFieldVolume |
                        (bar.oi_close.value() > 0 ? messages::kFieldOpenInterest : 0u);
                    snapshot.ltp = bar.close;
                    snapshot.volume = core::Quantity(volume);
                    snapshot.oi = bar.oi_close;
                    analytics.on_snapshot(snapshot);
                    if (bar.header.instrument_id == spot) {
                        spot_closed = true;
                        spot_closes.push_back({.minute = minute, .close_raw = bar.close.raw()});
                    }
                }
                option_results.clear();
                summaries.clear();
                analytics.compute(minute + 60, option_results, summaries);
                for (const auto& summary : summaries) {
                    state.on_summary(options::to_message(summary), minute);
                }
                if (!spot_closed || minute - day_start > signals::kCashLastMinute) {
                    continue;
                }
                for (const auto& strategy : strategies) {
                    records.push_back({.underlying = underlying,
                                       .signal = strategy->evaluate(state, minute),
                                       .spot_raw = spot_closes.back().close_raw});
                }
            }

            // Settle against the day's spot closes, and add up each factor against the move.
            std::vector<signals::OutcomeUpdate> updates;
            for (const auto& record : records) {
                const auto& signal = record.signal;
                const auto price =
                    signals::settle_price(spot_closes, signal.minute, signal.horizon_min, true);
                if (!price) {
                    continue;
                }
                const auto outcome = signals::outcome(signal.label, record.spot_raw, *price,
                                                      config->signals.flat_bp);
                updates.push_back({.strategy = std::string(signals::name(signal.strategy)),
                                   .underlying = underlying,
                                   .minute = signal.minute,
                                   .outcome = outcome});
                if (!outcome.return_bp) {
                    continue;
                }
                const double move = *outcome.return_bp;
                ++report.signals;
                for (const auto& factor : signal.factors) {
                    auto& stats = report.factors[factor.id];
                    ++stats.evaluated;
                    if (factor.status == signals::FactorStatus::Present) {
                        stats.add(factor.value, move, config->signals.flat_bp);
                    }
                }
                ++report.score.evaluated;
                report.score.add(signal.score / 100.0, move, config->signals.flat_bp);
            }
            if (auto written = out->write(records); !written) {
                std::fprintf(stderr, "error: writing signals: %s\n",
                             written.error().message.c_str());
                return 1;
            }
            if (auto settled = out->set_outcomes(updates); !settled) {
                std::fprintf(stderr, "error: writing outcomes: %s\n",
                             settled.error().message.c_str());
                return 1;
            }
        }
    }

    // Report: per index, each factor's presence, mean, correlation with the forward return,
    // and how often its direction was right on clear readings; then the strategy's record.
    const std::int64_t from =
        std::int64_t{days.front()} * signals::kSecondsPerDay - signals::kIstOffset;
    const std::int64_t to =
        std::int64_t{days.back() + 1} * signals::kSecondsPerDay - signals::kIstOffset;
    for (const auto& [underlying, report] : reports) {
        std::printf("\n== %s: %zu signals settled\n", underlying.c_str(), report.signals);
        for (std::size_t i = 1; i <= signals::kFactorCount; ++i) {
            const auto id = static_cast<signals::FactorId>(i);
            const auto found = report.factors.find(id);
            const FactorStats empty;
            const FactorStats& stats = found == report.factors.end() ? empty : found->second;
            std::printf("%s\n", stats_line(signals::name(id), stats, report.signals).c_str());
        }
        std::printf("%s\n", stats_line("SCORE", report.score, report.signals).c_str());
        if (const auto records = out->record(underlying, from, to)) {
            for (const auto& r : *records) {
                const int decided = r.hits + r.misses;
                std::printf(
                    "  %s: hit rate %s (%d of %d) · always up %s · momentum %s of %d · %d bullish, "
                    "%d bearish, %d neutral, %d flat\n",
                    r.strategy.c_str(),
                    percent(static_cast<std::size_t>(r.hits), static_cast<std::size_t>(decided))
                        .c_str(),
                    r.hits, decided,
                    percent(static_cast<std::size_t>(r.up_moves), static_cast<std::size_t>(decided))
                        .c_str(),
                    percent(static_cast<std::size_t>(r.momentum_hits),
                            static_cast<std::size_t>(r.momentum_calls))
                        .c_str(),
                    r.momentum_calls, r.bullish, r.bearish, r.neutral, r.flats);
            }
        }
    }
    return 0;
}
