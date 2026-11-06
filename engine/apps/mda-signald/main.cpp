// mda-signald — index direction signals from the analytics bus (ADR-0028,
// docs/design/signals.md).
//
//   mda-signald [--config config/mda.yaml]
//
// Reads bars and chain summaries on bus "an" and keeps one MarketState per configured index
// (its spot, near-month future, chain options and the sector indices). Once an index's
// minute bar has closed and the engine's closing chain summaries for that minute are in (sent
// after all of the minute's bars, so a replay scores the same at any speed, §14), every
// configured strategy runs for that minute; each signal goes to
// bus "sig" (MarketSignal) and to data/signals.db. Once a signal's horizon has passed (or
// the session closed), its outcome is written next to it (§6).
//
// After a restart today's bars come from bars.db, so the bar windows are full at once.
// Chain summaries aren't stored: PCR and IV changes come back once their window has passed.
// It also reads bus "md" for the near-month futures' snapshots and depth: their signed
// volume (F9, §13) isn't stored either, so F9 is absent for 10 minutes after a restart.
// Safe to restart during market hours: only the minutes it was down are lost.
//
// One thread. Stops cleanly on Ctrl+C / SIGTERM.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <mda/bars/bar.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/options/chain_analytics.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>
#include <mda/signals/market_signal.hpp>
#include <mda/signals/market_state.hpp>
#include <mda/signals/outcome.hpp>
#include <mda/signals/signal_store.hpp>
#include <mda/signals/strategies.hpp>

namespace {

using namespace mda;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
}

std::int64_t unix_now() {
    return core::Timestamp::now().ns() / 1'000'000'000;
}

std::int32_t days_since_epoch(std::chrono::year_month_day day) {
    return static_cast<std::int32_t>(std::chrono::sys_days{day}.time_since_epoch().count());
}

struct Market {
    std::string underlying;
    std::string spot_key;
    int expiries = 1;  ///< nearest chain expiries tracked (feed.chains)
    signals::MarketState state;
    std::set<std::int64_t> pending;  ///< closed spot minutes not yet evaluated
};

std::vector<std::unique_ptr<signals::Strategy>> make_strategies(const runtime::SignalsConfig& config) {
    std::vector<std::unique_ptr<signals::Strategy>> strategies;
    for (const auto& strategy : config.strategies) {
        auto made = signals::make_strategy(strategy.id, strategy.horizon_min, strategy.bullish,
                                           strategy.bearish);
        if (!made) {
            spdlog::error("signals: unknown strategy '{}'", strategy.id);
            return {};
        }
        strategies.push_back(std::move(made));
    }
    return strategies;
}

/// The configured indices whose spot is in the registry.
std::vector<Market> make_markets(const runtime::Config& config,
                                 const instruments::InstrumentRegistry& registry) {
    std::vector<Market> markets;
    for (const auto& underlying : config.signals.underlyings) {
        const auto chain = std::ranges::find(config.feed.chains, underlying, &runtime::ChainConfig::underlying);
        if (chain == config.feed.chains.end()) {
            spdlog::warn("signals: {} has no feed.chains entry; skipped", underlying);
            continue;
        }
        const auto key = instruments::parse_contract_key(chain->spot);
        const core::InstrumentId spot = key ? registry.find(*key) : core::InstrumentId{};
        if (!spot.valid()) {
            spdlog::warn("signals: spot '{}' unknown; {} skipped", chain->spot, underlying);
            continue;
        }
        markets.push_back(Market{.underlying = underlying,
                                 .spot_key = chain->spot,
                                 .expiries = chain->expiries,
                                 .state = signals::MarketState(
                                     {.underlying = underlying, .spot = spot, .future = {}}),
                                 .pending = {}});
    }
    return markets;
}

/// Points each market at today's near-month future, its chain options and the sector
/// indices (with their previous close). Run at start and at each new day.
void refresh_instruments(std::vector<Market>& markets, const runtime::Config& config,
                         const instruments::InstrumentRegistry& registry, bars::BarStore& store,
                         std::int64_t now) {
    const std::int32_t today = signals::ist_day(now);
    const std::int64_t day_start = signals::ist_day_start(now);
    for (auto& market : markets) {
        std::optional<std::pair<std::int32_t, core::InstrumentId>> future;
        std::map<std::int32_t, std::vector<signals::OptionInfo>> by_expiry;
        for (const auto& instrument : registry.all()) {
            const auto& k = instrument.key;
            if (k.underlying != market.underlying || !k.expiry.ok()) {
                continue;
            }
            const std::int32_t expiry = days_since_epoch(k.expiry);
            if (expiry < today) {
                continue;
            }
            if (k.kind == instruments::Kind::Future && (!future || expiry < future->first)) {
                future = {expiry, instrument.id};
            } else if (k.kind == instruments::Kind::Option) {
                by_expiry[expiry].push_back({.id = instrument.id,
                                             .expiry_days = expiry,
                                             .strike = k.strike.to_double(),
                                             .call = k.option_type == instruments::OptionType::Call});
            }
        }
        market.state.set_future(future ? future->second : core::InstrumentId{});
        std::size_t options = 0;
        int expiries = 0;
        for (const auto& [expiry, contracts] : by_expiry) {
            if (expiries++ >= market.expiries) {
                break;
            }
            for (const auto& option : contracts) {
                market.state.add_option(option);
            }
            options += contracts.size();
        }
        spdlog::info("signals: {}: future {}, {} options in {} expiries", market.underlying,
                     future ? "found" : "none", options, std::min(expiries, market.expiries));
    }

    std::size_t sectors = 0;
    for (const auto& text : config.signals.sectors) {
        const auto key = instruments::parse_contract_key(text);
        const core::InstrumentId id = key ? registry.find(*key) : core::InstrumentId{};
        if (!id.valid()) {
            spdlog::warn("signals: sector '{}' unknown; skipped", text);
            continue;
        }
        const auto previous = store.latest_before(id.value(), day_start);
        const double prev_close = previous && *previous ? (*previous)->close.to_double() : 0;
        for (auto& market : markets) {
            market.state.add_sector(id, prev_close);
        }
        ++sectors;
    }
    spdlog::info("signals: {} sector indices for breadth", sectors);

    if (!config.signals.vix.empty()) {
        const auto key = instruments::parse_contract_key(config.signals.vix);
        const core::InstrumentId id = key ? registry.find(*key) : core::InstrumentId{};
        if (id.valid()) {
            for (auto& market : markets) {
                market.state.set_vix(id);
            }
            spdlog::info("signals: India VIX is instrument {}", id.value());
        } else {
            spdlog::warn("signals: vix '{}' unknown; F8 stays absent", config.signals.vix);
        }
    }
}

/// Today's bars from bars.db, so the windows are full after a restart.
void warm_up(std::vector<Market>& markets, bars::BarStore& store, std::int64_t now) {
    const std::int64_t from = signals::ist_day_start(now);
    std::set<std::uint32_t> ids;
    for (const auto& market : markets) {
        const auto& state = market.state;
        ids.insert(state.definition().spot.value());
        if (state.definition().future.valid()) {
            ids.insert(state.definition().future.value());
        }
        for (const auto& option : state.options()) {
            ids.insert(option.info.id.value());
        }
        for (const auto& sector : state.sectors()) {
            ids.insert(sector.id.value());
        }
        if (state.vix_id().valid()) {
            ids.insert(state.vix_id().value());
        }
    }
    std::size_t loaded = 0;
    for (const std::uint32_t id : ids) {
        const auto bars = store.range(id, from, from + signals::kSecondsPerDay);
        if (!bars) {
            continue;
        }
        for (const auto& bar : *bars) {
            for (auto& market : markets) {
                market.state.on_bar(bar);
            }
        }
        loaded += bars->size();
    }
    spdlog::info("signals: warm-up: {} of today's bars for {} instruments", loaded, ids.size());
}

/// Writes the outcome of every open signal of `market` that can settle, seen from the bar
/// minute `latest`. Today's spot closes come from the market state, earlier days' from
/// bars.db (signals left open when signald stopped). Returns how many settled.
std::size_t settle(Market& market, signals::SignalStore& store, bars::BarStore& bar_store,
                   std::int64_t latest, int flat_bp) {
    const auto open = store.pending(market.underlying);
    if (!open) {
        spdlog::error("signals: reading open signals failed: {}", open.error().message);
        return 0;
    }
    std::map<std::int32_t, std::vector<signals::SpotClose>> days;
    const auto closes_of = [&](std::int64_t minute) -> const std::vector<signals::SpotClose>& {
        const std::int32_t day = signals::ist_day(minute);
        auto [it, added] = days.try_emplace(day);
        if (!added) {
            return it->second;
        }
        const std::int64_t from = signals::ist_day_start(minute);
        if (day == signals::ist_day(latest)) {
            for (const auto& point : market.state.spot().between(from, latest)) {
                it->second.push_back(
                    {.minute = point.minute,
                     .close_raw = std::llround(point.value * static_cast<double>(core::Price::kScale))});
            }
        } else if (const auto stored = bar_store.range(market.state.definition().spot.value(), from,
                                                  from + signals::kSecondsPerDay)) {
            for (const auto& bar : *stored) {
                it->second.push_back({.minute = bar.start, .close_raw = bar.close.raw()});
            }
        }
        return it->second;
    };
    std::vector<signals::OutcomeUpdate> updates;
    for (const auto& row : *open) {
        const auto price = signals::settle_price(closes_of(row.minute), row.minute, row.horizon_min,
                                                 signals::day_closed(row.minute, latest));
        if (price) {
            updates.push_back({.strategy = row.strategy,
                               .underlying = row.underlying,
                               .minute = row.minute,
                               .outcome = signals::outcome(row.label, row.spot_raw, *price, flat_bp)});
        }
    }
    if (auto done = store.set_outcomes(updates); !done) {
        spdlog::error("signals: writing {} outcomes failed: {}", updates.size(), done.error().message);
        return 0;
    }
    return updates.size();
}

/// Waits (checking for a stop) until bus `name` exists, then attaches.
std::optional<bus::Reader> attach_bus(const std::string& name, std::string_view writer) {
    bool reported = false;
    while (!g_stop.load()) {
        if (auto reader = bus::Reader::attach(name, {.name = "mda-signald"})) {
            return std::move(*reader);
        } else if (!reported) {
            spdlog::warn("bus '{}' not available ({}); waiting for {}", name,
                         bus::to_string(reader.error()), writer);
            reported = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return std::nullopt;
}

std::string hh_mm_ist(std::int64_t t) {
    const std::int64_t local = (t + signals::kIstOffset) % signals::kSecondsPerDay;
    return std::format("{:02}:{:02}", local / 3600, local % 3600 / 60);
}

int run(const runtime::Config& config) {
    const auto strategies = make_strategies(config.signals);
    if (strategies.empty()) {
        spdlog::error("signals: no strategies configured (signals.strategies)");
        return 1;
    }
    auto bar_store = bars::BarStore::open(config.engine.bars_db);
    if (!bar_store) {
        spdlog::error("bars: cannot open {}: {}", config.engine.bars_db.string(),
                      bar_store.error().message);
        return 1;
    }
    auto signal_store = signals::SignalStore::open(config.signals.db);
    if (!signal_store) {
        spdlog::error("signals: cannot open {}: {}", config.signals.db.string(),
                      signal_store.error().message);
        return 1;
    }
    spdlog::info("signals: writing to {}", config.signals.db.string());

    auto loaded = instruments::InstrumentRegistry::load(config.kite.registry_file);
    if (!loaded) {
        spdlog::error("registry: cannot load {}", config.kite.registry_file.string());
        return 1;
    }
    auto markets = make_markets(config, loaded->registry);
    if (markets.empty()) {
        spdlog::error("signals: no indices to follow (signals.underlyings)");
        return 1;
    }
    std::int64_t now = unix_now();
    refresh_instruments(markets, config, loaded->registry, *bar_store, now);
    warm_up(markets, *bar_store, now);
    std::int32_t day = signals::ist_day(now);
    std::size_t settled_at_start = 0;
    for (auto& market : markets) {
        settled_at_start += settle(market, *signal_store, *bar_store, now, config.signals.flat_bp);
    }
    if (settled_at_start > 0) {
        spdlog::info("signals: {} outcomes of earlier signals settled", settled_at_start);
    }

    auto sig = bus::Writer::create_or_attach(
        {.name = config.sig_bus.name, .capacity = config.sig_bus.capacity_mb * 1024 * 1024});
    if (!sig) {
        spdlog::error("bus: {}: {}", config.sig_bus.name, bus::to_string(sig.error()));
        return 1;
    }
    spdlog::info("bus: writer on '{}' ({} MB, epoch {})", config.sig_bus.name,
                 config.sig_bus.capacity_mb, sig->epoch());

    auto reader = attach_bus(config.an_bus.name, "mda-engine");
    if (!reader) {
        return 0;  // stopped while waiting
    }
    spdlog::info("bus: reading '{}'", config.an_bus.name);
    // The futures' snapshots and depth, for F9 (signals.md §13); everything else is skipped.
    auto md = attach_bus(config.md_bus.name, "mda-feedd");
    if (!md) {
        return 0;
    }
    spdlog::info("bus: reading '{}' (futures' signed volume)", config.md_bus.name);
    // Drains `md` up to now (bounded, so a flood can't starve the an loop).
    const auto drain_md = [&](std::size_t limit) {
        for (std::size_t i = 0; i < limit; ++i) {
            const auto polled = md->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
                if (auto snapshot = messages::decode<messages::MarketSnapshot>(type, payload)) {
                    for (auto& market : markets) {
                        market.state.on_snapshot(*snapshot);
                    }
                } else if (auto depth = messages::decode<messages::DepthSnapshot>(type, payload)) {
                    for (auto& market : markets) {
                        market.state.on_depth(*depth);
                    }
                }
            });
            if (polled.status == bus::PollStatus::Empty) {
                return true;
            }
            if (polled.status == bus::PollStatus::Lapped) {
                spdlog::warn("bus: signald fell behind '{}'; {} bytes skipped", config.md_bus.name,
                             polled.lost_bytes);
                for (auto& market : markets) {
                    market.state.reset_flow_baseline();
                }
            }
        }
        return false;
    };

    // "Now" from the bars on the bus (forming bars come every second), so a replay
    // (ADR-0026) settles outcomes by session time. 0 until the first bar.
    std::int64_t current_minute = 0;
    // The latest minute whose closing chain summaries arrived: its bars are all in (§14).
    std::int64_t closed_through = 0;
    std::uint64_t sequence = 0;
    std::vector<signals::SignalRecord> records;
    std::int64_t flow_logged = 0;  ///< the last minute whose flow line was logged

    // Runs the strategies for every closed minute whose closing summaries are in. Two minutes
    // late at most: an engine not sending closing summaries doesn't stop the signals.
    const auto evaluate_pending = [&] {
        drain_md(1'000'000);  // the evaluated minute's snapshots are all in by now
        const std::int64_t ready = std::max(closed_through, current_minute - 120);
        std::string line;
        std::vector<Market*> touched;
        for (auto& market : markets) {
            const auto due = market.pending.upper_bound(ready);
            if (due != market.pending.begin()) {
                touched.push_back(&market);
            }
            for (auto it = market.pending.begin(); it != due; ++it) {
                const std::int64_t minute = *it;
                const auto* spot = market.state.spot().at(minute, signals::ist_day_start(minute));
                const std::int64_t spot_raw =
                    spot ? std::llround(*spot * static_cast<double>(core::Price::kScale)) : 0;
                for (const auto& strategy : strategies) {
                    auto signal = strategy->evaluate(market.state, minute);
                    const auto message = signals::to_message(signal, ++sequence);
                    (void)sig->publish(messages::to_wire(messages::MessageType::MarketSignal),
                                       messages::as_bytes(message));
                    line += std::format(" {} {:+} {}", market.underlying, signal.score,
                                        signals::name(signal.label));
                    records.push_back({.underlying = market.underlying,
                                       .signal = std::move(signal),
                                       .spot_raw = spot_raw});
                }
            }
            market.pending.erase(market.pending.begin(), due);
        }
        if (records.empty()) {
            return;
        }
        if (auto written = signal_store->write(records); !written) {
            spdlog::error("signals: writing {} signals failed: {}", records.size(),
                          written.error().message);
        }
        std::size_t settled = 0;
        for (Market* market : touched) {
            settled += settle(*market, *signal_store, *bar_store, current_minute, config.signals.flat_bp);
        }
        spdlog::info("signals {}:{}{}", hh_mm_ist(records.front().signal.minute), line,
                     settled > 0 ? std::format(" · {} outcomes", settled) : std::string());
        // Every half hour, how much of the futures' volume F9 could classify today.
        if (const std::int64_t minute = records.front().signal.minute;
            minute % 1800 == 0 && minute != flow_logged) {
            flow_logged = minute;
            for (const auto& market : markets) {
                signals::FlowMinute day;
                for (const auto& point : market.state.flow().between(signals::ist_day_start(minute), minute)) {
                    day.buy += point.value.buy;
                    day.sell += point.value.sell;
                    day.unclassified += point.value.unclassified;
                }
                if (const std::int64_t total = day.buy + day.sell + day.unclassified; total > 0) {
                    spdlog::info("flow {}: buy {} sell {} unclassified {} ({:.1f}% classified)",
                                 market.underlying, day.buy, day.sell, day.unclassified,
                                 100.0 * static_cast<double>(day.buy + day.sell) / static_cast<double>(total));
                }
            }
        }
        records.clear();
    };

    bus::Backoff wait;
    auto next_second = Clock::now();
    int pending_ticks = 0;
    while (!g_stop.load()) {
        const bool md_idle = drain_md(1'024);
        const auto result = reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
            if (auto bar = messages::decode<bars::Bar>(type, payload)) {
                current_minute = std::max(current_minute, bar->start);
                const bool closed = (bar->header.flags & bars::kBarForming) == 0;
                for (auto& market : markets) {
                    market.state.on_bar(*bar);
                    if (closed && bar->header.instrument_id == market.state.definition().spot) {
                        market.pending.insert(bar->start);
                    }
                }
            } else if (auto summary = messages::decode<options::ChainSummaryMessage>(type, payload)) {
                if (summary->minute != 0) {  // closing ones only: the per-second ones vary by timing
                    const std::int64_t minute = summary->minute;
                    closed_through = std::max(closed_through, minute);
                    for (auto& market : markets) {
                        market.state.on_summary(*summary, minute);
                    }
                }
            }
        });
        if (result.status == bus::PollStatus::Empty) {
            evaluate_pending();
            pending_ticks = 0;
            if (md_idle) {
                wait.idle();
            } else {
                wait.reset();
            }
        } else {
            wait.reset();
            if (result.status == bus::PollStatus::Lapped) {
                spdlog::warn("bus: signald fell behind '{}'; {} bytes skipped", config.an_bus.name,
                             result.lost_bytes);
            }
        }

        if (const auto tick = Clock::now(); tick >= next_second) {
            next_second = tick + std::chrono::seconds(1);
            // The bus never drained: don't hold a signal back for more than ~2 s.
            if (++pending_ticks >= 2) {
                evaluate_pending();
                pending_ticks = 0;
            }
            if (current_minute > 0 && signals::ist_day(current_minute) != day) {
                day = signals::ist_day(current_minute);
                spdlog::info("signals: new day; dropping yesterday's state");
                for (auto& market : markets) {
                    market.state.prune(signals::ist_day_start(current_minute));
                }
                // New expiries and contracts are registered by mda-feedd as they list.
                if (auto reloaded = instruments::InstrumentRegistry::load(config.kite.registry_file)) {
                    loaded = std::move(reloaded);
                }
                refresh_instruments(markets, config, loaded->registry, *bar_store, current_minute);
            }
            sig->heartbeat();
            reader->heartbeat();
            md->heartbeat();
        }
    }
    evaluate_pending();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-signald", config->logging);
    spdlog::info("mda-signald starting (config {}, log level {})", config->source.string(),
                 config->logging.level);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run(*config);
    spdlog::info("mda-signald stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}
