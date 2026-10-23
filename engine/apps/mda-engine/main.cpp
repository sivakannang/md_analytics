// mda-engine — market state and analytics from the md bus (ADR-0006, ADR-0021).
//
//   mda-engine [--config config/mda.yaml]
//
// Reads every snapshot on bus "md" and, once a second, publishes on bus "an" (for the gateway):
// - 1-minute bars (BarBuilder): closed bars also go to bars.db (ADR-0021);
// - option analytics for the configured chains (ADR-0022): IV, Greeks, OI change, buildup per
//   option, and forward / ATM IV / PCR / max pain per expiry.
// It also saves the latest quote, depth and circuit limits per instrument every 5 s
// (ADR-0024), and starts the analytics from them.
//
// One thread: poll the bus; once a second close due minutes, write, publish. SQLite writes
// are small batched transactions; the 256 MB md ring gives minutes of slack meanwhile.
// Stops cleanly on Ctrl+C / SIGTERM.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <map>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <mda/bars/event_clock.hpp>
#include <mda/bars/bar_builder.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/bars/day_snapshot.hpp>
#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/options/chain_analytics.hpp>
#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/quotestore/last_quote_store.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>

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

/// Unix time of today's 00:00 IST.
std::int64_t ist_midnight(std::int64_t now) {
    constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;
    return now - (now + kIst) % 86'400;
}

/// The configured chains' options (nearest expiries from today) from the permanent registry.
std::vector<options::ChainDefinition> chain_definitions(const runtime::Config& config,
                                                        const instruments::InstrumentRegistry& registry) {
    const std::chrono::year_month_day today{std::chrono::floor<std::chrono::days>(
        std::chrono::sys_seconds{std::chrono::seconds{unix_now() + 19'800}})};
    std::vector<options::ChainDefinition> chains;
    for (const auto& chain : config.feed.chains) {
        const auto key = instruments::parse_contract_key(chain.spot);
        const core::InstrumentId spot = key ? registry.find(*key) : core::InstrumentId{};
        if (!spot.valid()) {
            spdlog::warn("chains: spot '{}' unknown; {} skipped", chain.spot, chain.underlying);
            continue;
        }
        std::map<std::chrono::year_month_day, std::vector<options::OptionContract>> by_expiry;
        for (const auto& instrument : registry.all()) {
            const auto& k = instrument.key;
            if (k.kind == instruments::Kind::Option && k.underlying == chain.underlying &&
                k.expiry >= today) {
                by_expiry[k.expiry].push_back({.id = instrument.id,
                                               .strike = k.strike,
                                               .call = k.option_type == instruments::OptionType::Call});
            }
        }
        options::ChainDefinition definition{.underlying = chain.underlying, .spot = spot};
        for (auto& [expiry, contracts] : by_expiry) {
            if (static_cast<int>(definition.expiries.size()) >= chain.expiries) {
                break;
            }
            definition.expiries.push_back({.expiry = expiry, .options = std::move(contracts)});
        }
        spdlog::info("chains: {} analytics for {} expiries", chain.underlying,
                     definition.expiries.size());
        chains.push_back(std::move(definition));
    }
    return chains;
}

/// Waits (checking for a stop) until bus `name` exists, then attaches.
std::optional<bus::Reader> attach_md(const std::string& name) {
    bool reported = false;
    while (!g_stop.load()) {
        if (auto reader = bus::Reader::attach(name, {.name = "mda-engine"})) {
            return std::move(*reader);
        } else if (!reported) {
            spdlog::warn("md bus '{}' not available ({}); waiting for mda-feedd", name,
                         bus::to_string(reader.error()));
            reported = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return std::nullopt;
}

int run(const runtime::Config& config) {
    auto store = bars::BarStore::open(config.engine.bars_db);
    if (!store) {
        spdlog::error("bars: cannot open {}: {}", config.engine.bars_db.string(),
                      store.error().message);
        return 1;
    }
    spdlog::info("bars: writing 1-minute bars to {}", config.engine.bars_db.string());

    auto an = bus::Writer::create_or_attach(
        {.name = config.an_bus.name, .capacity = config.an_bus.capacity_mb * 1024 * 1024});
    if (!an) {
        spdlog::error("bus: {}: {}", config.an_bus.name, bus::to_string(an.error()));
        return 1;
    }
    spdlog::info("bus: writer on '{}' ({} MB, epoch {})", config.an_bus.name,
                 config.an_bus.capacity_mb, an->epoch());

    std::vector<bars::Bar> closed;  // waiting to be written
    // The minute in progress when we start was only partly seen: don't record it (backfill
    // fills it with the broker's complete candle). Set from the first snapshot read, not the
    // wall clock, so a replay of an earlier session (ADR-0026) records its minutes too.
    std::int64_t first_full_minute = std::numeric_limits<std::int64_t>::max();
    std::uint64_t sequence = 0;
    const auto publish = [&](const bars::Bar& bar) {
        bars::Bar message = bar;
        message.header.sequence = ++sequence;
        (void)an->publish(messages::to_wire(messages::MessageType::Bar),
                          messages::as_bytes(message));
    };
    // The permanent registry: which instruments are derivatives (bar session to 15:40), and
    // the option chains.
    auto loaded = instruments::InstrumentRegistry::load(config.kite.registry_file);
    if (!loaded) {
        spdlog::error("registry: cannot load {}", config.kite.registry_file.string());
        return 1;
    }
    const auto& registry = loaded->registry;
    bars::BuilderOptions builder_options;
    builder_options.is_derivative = [&registry](core::InstrumentId id) {
        const auto* instrument = registry.get(id);
        return instrument != nullptr && instrument->key.market == instruments::Market::Fo;
    };
    bars::BarBuilder builder(std::move(builder_options), [&](const bars::Bar& bar) {
        if (bar.start < first_full_minute) {
            return;
        }
        closed.push_back(bar);
        publish(bar);
    });

    // After a restart, carry on today's series (last close and OI per instrument).
    const std::int64_t today = ist_midnight(unix_now());
    const auto volumes = store->volumes(today, today + 86'400);
    if (const auto latest = store->latest_per_instrument(today, today + 86'400)) {
        for (const auto& bar : *latest) {
            std::int64_t day_volume = 0;
            if (volumes) {
                if (const auto found = volumes->find(bar.header.instrument_id.value()); found != volumes->end()) {
                    day_volume = found->second;
                }
            }
            builder.seed(bar, day_volume);
        }
        if (!latest->empty()) {
            spdlog::info("bars: resumed today's series for {} instruments", latest->size());
        }
    }

    // Option analytics: chains from the registry, yesterday's OI from bars.db.
    options::ChainAnalytics analytics(chain_definitions(config, registry));
    if (const auto previous = store->latest_per_instrument(today - 10 * 86'400, today)) {
        std::size_t seeded = 0;
        for (const auto& bar : *previous) {
            if (bar.oi_close.value() > 0) {
                analytics.set_prev_oi(bar.header.instrument_id, bar.oi_close.value());
                ++seeded;
            }
        }
        spdlog::info("chains: previous-day OI for {} instruments", seeded);
    }

    // Last-known quotes (ADR-0024): saved for the gateway, and fed to the analytics so the
    // chains have values before the first tick (after a restart, or before the open).
    // Not fed to the bar builder: those minutes are already in bars.db.
    std::unordered_set<std::uint32_t> quoted;
    auto last_quotes = quotestore::LastQuoteStore::open(config.engine.last_quotes_db);
    if (!last_quotes) {
        spdlog::warn("last quotes: cannot open {}: {}; not saving them", config.engine.last_quotes_db.string(),
                     last_quotes.error().message);
    } else if (const auto loaded_quotes = last_quotes->load([&](std::uint32_t type, std::span<const std::byte> payload) {
                   if (auto snapshot = messages::decode<messages::MarketSnapshot>(type, payload)) {
                       analytics.on_snapshot(*snapshot);
                       quoted.insert(snapshot->header.instrument_id.value());
                   }
               })) {
        spdlog::info("last quotes: {} saved messages loaded from {}", *loaded_quotes,
                     config.engine.last_quotes_db.string());
    }
    const auto save_last_quotes = [&] {
        if (!last_quotes) {
            return;
        }
        if (auto saved = last_quotes->flush(); !saved) {
            spdlog::error("last quotes: saving failed: {}", saved.error().message);
        }
    };
    auto next_quote_save = Clock::now() + std::chrono::seconds(5);
    // Instruments without a saved quote: one built from their last trading day's bars.
    if (const auto latest = store->latest_per_instrument(today - 10 * 86'400, today + 86'400)) {
        std::size_t built = 0;
        for (const auto& bar : *latest) {
            const std::uint32_t id = bar.header.instrument_id.value();
            if (quoted.contains(id)) {
                continue;
            }
            const std::int64_t day = ist_midnight(bar.start);
            const auto minutes = store->range(id, day, day + 86'400);
            if (!minutes || minutes->empty()) {
                continue;
            }
            const auto previous = store->latest_before(id, day);
            const auto snapshot =
                bars::day_snapshot(*minutes, previous && *previous ? (*previous)->close : core::Price{});
            analytics.on_snapshot(snapshot);
            if (last_quotes) {
                last_quotes->remember(snapshot);
            }
            ++built;
        }
        if (built > 0) {
            spdlog::info("last quotes: {} built from bars.db (no saved quote)", built);
            save_last_quotes();
        }
    }
    std::unordered_map<std::uint32_t, options::OptionAnalyticsMessage> last_sent;  // changes only
    // ...plus everything every 15 s, so a reader that starts later (e.g. after the close,
    // when nothing changes any more) still learns the current values.
    auto next_full_publish = Clock::now();
    std::vector<options::OptionResult> option_results;
    std::vector<options::ChainSummary> chain_summaries;

    auto reader = attach_md(config.md_bus.name);
    if (!reader) {
        return 0;  // stopped while waiting
    }
    spdlog::info("bus: reading '{}'", config.md_bus.name);

    bus::Backoff wait;
    auto next_second = Clock::now();
    auto next_summary = Clock::now() + std::chrono::seconds(60);
    std::uint64_t snapshots = 0;
    bars::EventClock event_clock;  // "now" from the ticks: a replay closes bars by session time
    std::uint64_t written = 0;
    std::uint64_t written_at_summary = 0;
    while (!g_stop.load()) {
        const auto result =
            reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
                if (auto snapshot = messages::decode<messages::MarketSnapshot>(type, payload)) {
                    event_clock.on_event(snapshot->receive_ts.ns());
                    if (first_full_minute == std::numeric_limits<std::int64_t>::max()) {
                        const bool exchange = (snapshot->fields & messages::kFieldExchangeTime) != 0 &&
                                              snapshot->exchange_ts.is_set();
                        const std::int64_t at =
                            (exchange ? snapshot->exchange_ts : snapshot->receive_ts).ns() / 1'000'000'000;
                        first_full_minute = at - at % 60 + 60;
                    }
                    builder.on_snapshot(*snapshot);
                    analytics.on_snapshot(*snapshot);
                    if (last_quotes) {
                        last_quotes->remember(*snapshot);
                    }
                    ++snapshots;
                } else if (last_quotes) {
                    if (auto depth = messages::decode<messages::DepthSnapshot>(type, payload)) {
                        last_quotes->remember(*depth);
                    } else if (auto limits = messages::decode<messages::CircuitLimits>(type, payload)) {
                        last_quotes->remember(*limits);
                    }
                }
            });
        if (result.status == bus::PollStatus::Empty) {
            wait.idle();
        } else {
            wait.reset();
            if (result.status == bus::PollStatus::Lapped) {
                spdlog::warn("bus: engine fell behind '{}'; {} bytes skipped", config.md_bus.name,
                             result.lost_bytes);
            }
        }

        if (const auto now = Clock::now(); now >= next_second) {
            next_second = now + std::chrono::seconds(1);
            builder.close_due(event_clock.now_seconds(core::Timestamp::now().ns()));
            if (!closed.empty()) {
                if (auto done = store->write(closed); !done) {
                    spdlog::error("bars: writing {} bars failed: {}", closed.size(),
                                  done.error().message);
                } else {
                    written += closed.size();
                }
                closed.clear();
            }
            for (const auto& bar : builder.forming()) {
                publish(bar);
            }
            option_results.clear();
            chain_summaries.clear();
            analytics.compute(event_clock.now_seconds(core::Timestamp::now().ns()), option_results, chain_summaries);
            if (now >= next_full_publish) {
                next_full_publish = now + std::chrono::seconds(15);
                last_sent.clear();
            }
            for (const auto& option : option_results) {
                auto message = options::to_message(option);
                auto& previous = last_sent[option.id.value()];
                message.header.sequence = previous.header.sequence;  // compare the payload only
                if (std::memcmp(&message, &previous, sizeof message) == 0) {
                    continue;
                }
                message.header.sequence = ++sequence;
                previous = message;
                (void)an->publish(messages::to_wire(messages::MessageType::OptionAnalytics),
                                  messages::as_bytes(message));
            }
            for (const auto& summary : chain_summaries) {
                auto message = options::to_message(summary);
                message.header.sequence = ++sequence;
                (void)an->publish(messages::to_wire(messages::MessageType::ChainSummary),
                                  messages::as_bytes(message));
            }
            an->heartbeat();
            reader->heartbeat();
            if (now >= next_quote_save) {
                next_quote_save = now + std::chrono::seconds(5);
                save_last_quotes();
            }

            if (now >= next_summary) {
                next_summary = now + std::chrono::seconds(60);
                spdlog::info(
                    "summary: {} snapshots, {} instruments, {} bars written in the last minute ({} "
                    "today)",
                    snapshots, builder.instruments(), written - written_at_summary, written);
                written_at_summary = written;
                snapshots = 0;
            }
        }
    }
    if (!closed.empty()) {
        (void)store->write(
            closed);  // closed minutes only; the forming one is rebuilt after a restart
    }
    save_last_quotes();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-engine", config->logging);
    spdlog::info("mda-engine starting (config {}, log level {})", config->source.string(),
                 config->logging.level);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run(*config);
    spdlog::info("mda-engine stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}
