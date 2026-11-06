// mda-autotrader — paper trading on market signals (ADR-0030, docs/design/auto-trader.md).
//
//   mda-autotrader [--config config/mda.yaml]
//
// Runs the configured runs (autotrader.runs) of the follow-signal (futures) and
// follow-signal-options (bought ATM options, design §10) strategies (libs/autotrade): reads
// MarketSignal on "sig", quotes, depth and feed status on "md", and its own execution
// reports on "exec"; sends orders on "ord-auto" (its single writer), resending a request
// until mda-broker answers. Every decision goes to data/autotrader.db with the run's day
// counters. After a restart each run rebuilds its position and resting stop from paper.db
// (its orders carry the tag "auto:<run id>"), so it carries on where it stopped.
//
// Session time comes from the md bus while it flows (a replay runs on session time), the
// wall clock otherwise. One thread. Stops cleanly on Ctrl+C / SIGTERM.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>

#include <mda/autotrade/decision_store.hpp>
#include <mda/autotrade/options_run.hpp>
#include <mda/autotrade/run.hpp>
#include <mda/broker/messages.hpp>
#include <mda/broker/paper_store.hpp>
#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>
#include <mda/signals/market_signal.hpp>
#include <mda/storage/database.hpp>

namespace {

using namespace mda;
using autotrade::Millis;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
}

Millis wall_ms() {
    return core::Timestamp::now().ns() / 1'000'000;
}

constexpr Millis kDayMs = 86'400'000;
constexpr Millis kIstMs = 19'800'000;

std::int32_t ist_day(Millis t) {
    return static_cast<std::int32_t>((t + kIstMs) / kDayMs);
}

/// The tag a run's orders carry, as the broker stores it (the field holds 15 characters).
std::string tag_of(const std::string& run_id) {
    char field[16] = {};
    broker::set_text(field, "auto:" + run_id);
    return std::string(broker::text_of(field));
}

/// The index's listed futures (kind FUT) or options (OPT) from the catalog.
std::vector<autotrade::ListedOption> listed_of(storage::Database& db, const std::string& underlying,
                                               std::string_view kind) {
    std::vector<autotrade::ListedOption> out;
    auto statement = db.prepare(
        "SELECT id, lot_size, expiry, tick_size_raw, strike_raw, option_type FROM instruments "
        "WHERE underlying = ?1 AND kind = ?2 AND listed = 1");
    if (!statement) {
        return out;
    }
    statement->bind(1, std::string_view(underlying));
    statement->bind(2, kind);
    while (statement->step().value_or(storage::StepResult::Done) == storage::StepResult::Row) {
        int y = 0;
        unsigned m = 0;
        unsigned d = 0;
        const std::string expiry = statement->column_text(2);
        if (std::sscanf(expiry.c_str(), "%d-%u-%u", &y, &m, &d) != 3) {
            continue;
        }
        const std::chrono::year_month_day day{std::chrono::year{y}, std::chrono::month{m},
                                              std::chrono::day{d}};
        out.push_back({.contract = {.id = core::InstrumentId(
                                        static_cast<std::uint32_t>(statement->column_int64(0))),
                                    .lot_size = statement->column_int64(1),
                                    .expiry_days = static_cast<std::int32_t>(
                                        std::chrono::sys_days{day}.time_since_epoch().count()),
                                    .tick = core::Price::from_raw(statement->column_int64(3))},
                       .strike = core::Price::from_raw(statement->column_int64(4)),
                       .call = statement->column_text(5) == "CE"});
    }
    return out;
}

std::vector<autotrade::Contract> futures_of(storage::Database& db, const std::string& underlying) {
    std::vector<autotrade::Contract> out;
    for (auto& listed : listed_of(db, underlying, "FUT")) {
        out.push_back(listed.contract);
    }
    return out;
}

/// One instrument's market: the LTP from snapshots, the touch from depth.
struct Quote {
    core::Price ltp;
    core::Price bid;
    core::Price ask;
    Millis at = 0;
};

struct Slot {
    std::variant<autotrade::Run, autotrade::OptionsRun> run;
    std::string tag;
    core::InstrumentId spot;
    std::vector<autotrade::ListedOption>
        options;  ///< options runs: the index's listed options today
    Millis last_entry = 0;
    bool restored = false;

    /// Calls `f` with the run, whichever strategy it is (both have the same interface).
    decltype(auto) with(auto&& f) { return std::visit(f, run); }
};

/// A request on its way: resent until a report answers it.
struct Pending {
    broker::OrderRequest request;
    Millis first_sent = 0;
    Millis last_sent = 0;
};

autotrade::OptionsRunConfig run_config(const runtime::AutoRunConfig& c) {
    autotrade::OptionsRunConfig r;
    r.id = c.id;
    r.user = c.user;
    r.signal = c.signal;
    r.underlying = c.underlying;
    r.lots = c.lots;
    r.product = c.product == "NRML" ? oms::Product::Nrml : oms::Product::Mis;
    r.stop_points = c.stop_points;
    r.target_points = c.target_points;
    r.max_hold_min = c.max_hold_min;
    r.max_trades_per_day = c.max_trades_per_day;
    r.cooldown_min = c.cooldown_min;
    r.exit_score = c.exit_score;
    r.daily_loss_stop = c.daily_loss_stop * 100;
    r.enabled = c.enabled;
    r.index_stop_points = c.index_stop_points;
    r.premium_stop_percent = c.premium_stop_percent;
    r.entry_slip_ticks = c.entry_slip_ticks;
    r.entry_timeout_s = c.entry_timeout_s;
    r.stop_limit_ticks = c.stop_limit_ticks;
    r.reprice_s = c.reprice_s;
    return r;
}

/// Rebuilds a run from paper.db (its tagged orders: position, instrument, entry, resting
/// stop) and autotrader.db (today's counters).
void recover(Slot& slot, broker::PaperStore& paper, autotrade::DecisionStore& decisions,
             storage::Database& app_db, Millis now) {
    const std::int32_t today = ist_day(now);
    const autotrade::RunConfig& config =
        slot.with([](auto& run) -> const autotrade::RunConfig& { return run.config(); });
    const auto account = paper.account_for_user(config.user).value_or(std::nullopt);
    std::int64_t position = 0;
    core::Price entry;
    std::int64_t stop_order = 0;
    std::uint64_t stop_client = 0;
    core::InstrumentId traded;
    if (account) {
        slot.with([&](auto& run) { run.set_kill_switch(account->kill_switch); });
        const Millis day_start = (std::int64_t{today} * kDayMs) - kIstMs;
        for (const auto& row : paper.orders(account->id, day_start - 7 * kDayMs, day_start + kDayMs)
                                   .value_or(std::vector<broker::OrderRow>{})) {
            const auto& o = row.order;
            if (o.spec.tag != slot.tag) {
                continue;
            }
            const std::int64_t signed_qty = (o.spec.side == oms::Side::Buy ? 1 : -1) * o.filled;
            if (o.filled > 0) {
                if (position == 0 || (position > 0) == (signed_qty > 0)) {
                    entry = core::Price::from_raw((o.filled_value_raw + o.filled / 2) / o.filled);
                }
                position += signed_qty;
                traded = o.spec.instrument;
            }
            const bool live =
                o.status == oms::OrderStatus::Open || o.status == oms::OrderStatus::TriggerPending;
            if (live && (o.spec.type == oms::OrderType::StopMarket ||
                         o.spec.type == oms::OrderType::StopLimit)) {
                stop_order = o.id;
                stop_client = o.spec.client_order_id;
            }
        }
    }
    const auto day = decisions.day(config.id, today).value_or(std::nullopt);
    if (day) {
        slot.with([&](auto& run) {
            run.restore_day(today, day->entries, day->realised, day->charges, day->last_exit,
                            day->counter);
        });
        slot.last_entry = day->last_entry;
    }
    const Millis entered = slot.last_entry > 0 ? slot.last_entry : now;

    std::string what;
    if (auto* run = std::get_if<autotrade::Run>(&slot.run)) {
        // The contract: the one held, or the near-month future.
        const auto futures = futures_of(app_db, config.underlying);
        std::optional<autotrade::Contract> contract;
        if (position != 0) {
            if (const auto held = std::ranges::find(futures, traded, &autotrade::Contract::id);
                held != futures.end()) {
                contract = *held;
            }
        } else {
            contract = autotrade::choose_future(futures, today);
        }
        if (contract) {
            run->set_contract(*contract);
        }
        run->restore(position, entry, entered, stop_order, stop_client);
        what = contract ? "trading " + std::to_string(contract->id.value()) : "no contract";
    } else {
        auto& options = std::get<autotrade::OptionsRun>(slot.run);
        slot.options = listed_of(app_db, config.underlying, "OPT");
        what = std::format("{} listed options", slot.options.size());
        if (position != 0) {
            const auto held =
                std::ranges::find(slot.options, traded,
                                  [](const autotrade::ListedOption& o) { return o.contract.id; });
            if (held != slot.options.end()) {
                // The index level at entry isn't stored: exits are measured from now.
                options.restore(held->contract, held->call, position, entry, entered, core::Price{},
                                stop_order, stop_client);
            } else {
                spdlog::error(
                    "auto {}: holds {} of instrument {}, not a listed option; close it by hand",
                    config.id, position, traded.value());
            }
        }
    }
    spdlog::info(
        "auto {}: {}{}{}", config.id, what,
        position != 0 ? std::format(", restored {} at {:.2f}", position, entry.to_double())
                      : std::string(),
        stop_order != 0 ? std::format(" with its stop (order {})", stop_order) : std::string());
}

int run(const runtime::Config& config) {
    auto paper = broker::PaperStore::open(config.paper.db);
    auto decisions = autotrade::DecisionStore::open(config.autotrader.db);
    auto app_db = storage::Database::open(config.database.path);
    auto loaded = instruments::InstrumentRegistry::load(config.kite.registry_file);
    if (!paper || !decisions || !app_db || !loaded) {
        spdlog::error("autotrader: cannot open paper.db, autotrader.db, app.db or the registry");
        return 1;
    }

    std::vector<Slot> slots;
    std::set<std::string> tags;
    for (const auto& c : config.autotrader.runs) {
        if (!c.enabled) {
            spdlog::info("auto {}: disabled (autotrader.runs: enabled: false)", c.id);
            continue;  // not loaded at all: no decisions, no orders
        }
        const auto chain =
            std::ranges::find(config.feed.chains, c.underlying, &runtime::ChainConfig::underlying);
        const auto key = chain != config.feed.chains.end()
                             ? instruments::parse_contract_key(chain->spot)
                             : std::nullopt;
        const core::InstrumentId spot = key ? loaded->registry.find(*key) : core::InstrumentId{};
        if (!spot.valid()) {
            spdlog::error("auto {}: {} has no feed.chains spot; run skipped", c.id, c.underlying);
            continue;
        }
        Slot slot{.run = autotrade::Run(run_config(c)), .tag = tag_of(c.id), .spot = spot};
        if (c.strategy == "follow-signal-options") {
            slot.run.emplace<autotrade::OptionsRun>(run_config(c));
        }
        if (!tags.insert(slot.tag).second) {
            spdlog::error(
                "autotrader: runs share the order tag '{}'; make their ids differ earlier",
                slot.tag);
            return 1;
        }
        slots.push_back(std::move(slot));
    }
    if (slots.empty()) {
        spdlog::info("autotrader: no runs configured; nothing to do");
    }

    auto ord_auto =
        bus::Writer::create_or_attach({.name = config.ord_auto_bus.name,
                                       .capacity = config.ord_auto_bus.capacity_mb * 1024 * 1024});
    if (!ord_auto) {
        spdlog::error("bus: {}: {}", config.ord_auto_bus.name, bus::to_string(ord_auto.error()));
        return 1;
    }
    spdlog::info("bus: writer on '{}' (epoch {})", config.ord_auto_bus.name, ord_auto->epoch());

    std::optional<bus::Reader> sig;
    std::optional<bus::Reader> md;
    std::optional<bus::Reader> exec;
    std::unordered_map<std::uint32_t, Quote> markets;
    bool feed_ok = true;
    std::map<std::uint64_t, Pending> pending;
    std::uint64_t sequence = 0;
    std::uint64_t modifies = 0;

    oms::Millis event_ms = 0;
    auto event_seen = Clock::now() - std::chrono::hours(1);
    const auto now_ms = [&]() -> Millis {
        const auto since = Clock::now() - event_seen;
        if (since < std::chrono::seconds(2)) {
            return event_ms + std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
        }
        return wall_ms();
    };

    const auto send = [&](broker::OrderRequest request, Millis now) {
        request.header.schema_version = messages::MessageTraits<broker::OrderRequest>::version;
        request.header.sequence = ++sequence;
        (void)ord_auto->publish(messages::to_wire(messages::MessageType::OrderRequest),
                                messages::as_bytes(request));
        auto& p = pending[request.client_order_id];
        if (p.first_sent == 0) {
            p.first_sent = now;
        }
        p.request = request;
        p.last_sent = now;
    };

    // Sends what a run wants, records its decisions and day counters.
    const auto apply = [&](Slot& slot, const autotrade::Output& out, Millis now) {
        slot.with([&](auto& run) {
            for (const auto& a : out.actions) {
                broker::OrderRequest r{};
                r.header.instrument_id = a.instrument;
                r.at = now;
                r.user = run.config().user;
                r.action = a.kind;
                r.source = oms::Source::Auto;
                if (a.kind == broker::Action::Cancel) {
                    r.order_id = a.order_id;
                    r.client_order_id =
                        (std::uint64_t{1} << 62) | static_cast<std::uint64_t>(a.order_id);
                } else if (a.kind == broker::Action::Modify) {
                    // Each re-price is its own request (the broker doesn't dedupe modifies).
                    r.order_id = a.order_id;
                    r.client_order_id = (std::uint64_t{1} << 61) | ++modifies;
                    r.quantity = a.quantity;
                    r.price = a.price;
                    r.trigger = a.trigger;
                } else {
                    r.client_order_id = a.client_order_id;
                    r.side = a.side;
                    r.type = a.type;
                    r.product = run.config().product;
                    r.quantity = a.quantity;
                    r.price = a.price;
                    r.trigger = a.trigger;
                }
                broker::set_text(r.tag, slot.tag);
                send(r, now);
            }
            for (const auto& d : out.decisions) {
                spdlog::info("auto {}: {} ({})", run.config().id, d.action, d.reason);
                if (d.action.starts_with("long") || d.action == "short") {
                    slot.last_entry = d.at;
                }
            }
            if (!out.decisions.empty() || !out.actions.empty()) {
                const autotrade::RunDay day{.day = run.day(),
                                            .entries = run.entries_today(),
                                            .realised = run.realised_today(),
                                            .charges = run.charges_today(),
                                            .last_exit = run.last_exit(),
                                            .counter = run.counter(),
                                            .last_entry = slot.last_entry};
                if (auto saved = decisions->save(run.config().id, out.decisions, day); !saved) {
                    spdlog::error("auto {}: saving decisions failed: {}", run.config().id,
                                  saved.error().message);
                }
            }
        });
    };

    std::int32_t day = 0;
    bus::Backoff wait;
    auto next_evaluate = Clock::now();
    auto next_second = Clock::now();
    while (!g_stop.load()) {
        bool busy = false;
        const auto drain = [&](std::optional<bus::Reader>& reader, auto&& handle) {
            if (!reader) {
                return;
            }
            for (int i = 0; i < 256; ++i) {
                const auto result = reader->poll(handle);
                if (result.status == bus::PollStatus::Empty) {
                    break;
                }
                busy = true;
            }
        };
        drain(md, [&](bus::MsgType type, std::span<const std::byte> payload) {
            if (auto s = messages::decode<messages::MarketSnapshot>(type, payload)) {
                const Millis at = s->receive_ts.ns() / 1'000'000;
                event_ms = std::max(event_ms, at);
                event_seen = Clock::now();
                if ((s->fields & messages::kFieldLtp) != 0 && s->ltp.raw() > 0) {
                    auto& m = markets[s->header.instrument_id.value()];
                    m.ltp = s->ltp;
                    m.at = at;
                }
            } else if (auto d = messages::decode<messages::DepthSnapshot>(type, payload)) {
                auto& m = markets[d->header.instrument_id.value()];
                m.bid = d->bids[0].price;
                m.ask = d->asks[0].price;
            } else if (auto f = messages::decode<messages::FeedStatus>(type, payload)) {
                feed_ok = f->state == messages::FeedState::Connected;
            }
        });
        drain(sig, [&](bus::MsgType type, std::span<const std::byte> payload) {
            const auto m = messages::decode<signals::MarketSignal>(type, payload);
            if (!m) {
                return;
            }
            const std::string_view strategy =
                signals::name(static_cast<signals::StrategyId>(m->strategy));
            for (auto& slot : slots) {
                if (slot.spot == m->header.instrument_id &&
                    slot.with([](auto& run) { return run.config().signal; }) == strategy) {
                    autotrade::SignalView view{.minute = m->minute,
                                               .score = m->score,
                                               .label = signals::label_of(*m),
                                               .top = {}};
                    for (const auto& factor : m->top) {
                        if (factor.factor != 0) {
                            view.top.push_back(
                                {.factor = static_cast<signals::FactorId>(factor.factor),
                                 .points = factor.points});
                        }
                    }
                    slot.with([&](auto& run) { run.on_signal(view); });
                }
            }
        });
        drain(exec, [&](bus::MsgType type, std::span<const std::byte> payload) {
            const auto r = messages::decode<broker::ExecutionReport>(type, payload);
            if (!r) {
                return;
            }
            std::erase_if(pending, [&](const auto& entry) {
                return entry.first == r->client_order_id ||
                       (entry.second.request.action != broker::Action::New &&
                        entry.second.request.order_id == r->order_id);
            });
            const Millis now = now_ms();
            for (auto& slot : slots) {
                if (r->user != slot.with([](auto& run) { return run.config().user; })) {
                    continue;
                }
                if (r->event == broker::ReportEvent::KillSwitchOn ||
                    r->event == broker::ReportEvent::KillSwitchOff) {
                    slot.with([&](auto& run) {
                        run.set_kill_switch(r->event == broker::ReportEvent::KillSwitchOn);
                    });
                    continue;
                }
                apply(slot, slot.with([&](auto& run) { return run.on_report(*r, now); }), now);
            }
        });

        const auto tick = Clock::now();
        if (tick >= next_evaluate) {
            next_evaluate = tick + std::chrono::milliseconds(200);
            const Millis now = now_ms();
            for (auto& slot : slots) {
                if (!slot.restored) {
                    if (!exec) {
                        continue;  // recover once reports can be heard, so nothing is missed
                    }
                    recover(slot, *paper, *decisions, *app_db, now);
                    slot.restored = true;
                }
                if (auto* futures = std::get_if<autotrade::Run>(&slot.run)) {
                    autotrade::Market market;
                    if (const auto contract = futures->contract()) {
                        if (const auto found = markets.find(contract->id.value());
                            found != markets.end()) {
                            market.ltp = found->second.ltp;
                            market.quote_at = found->second.at;
                        }
                    }
                    market.feed_ok = feed_ok;
                    apply(slot, futures->evaluate(market, now), now);
                    continue;
                }
                auto& options = std::get<autotrade::OptionsRun>(slot.run);
                autotrade::OptionsMarket market{
                    .feed_ok = feed_ok, .quote = [&](core::InstrumentId id) {
                        const auto found = markets.find(id.value());
                        if (found == markets.end()) {
                            return std::optional<autotrade::OptionQuote>{};
                        }
                        const Quote& q = found->second;
                        return std::optional<autotrade::OptionQuote>{
                            {.ltp = q.ltp, .bid = q.bid, .ask = q.ask, .at = q.at}};
                    }};
                if (const auto found = markets.find(slot.spot.value()); found != markets.end()) {
                    market.spot = found->second.ltp;
                    market.spot_at = found->second.at;
                }
                if (options.state() == autotrade::State::Flat && options.position() == 0 &&
                    market.spot.raw() > 0) {
                    options.set_options(
                        autotrade::choose_atm(slot.options, market.spot, ist_day(now)));
                }
                apply(slot, options.evaluate(market, now), now);
            }
            // Resend what mda-broker hasn't answered (the same request: never a new order).
            for (auto it = pending.begin(); it != pending.end();) {
                if (now - it->second.first_sent > 2'000) {
                    spdlog::warn("autotrader: no answer from mda-broker to request {}; giving up",
                                 it->first);
                    it = pending.erase(it);
                } else if (now - it->second.last_sent >= 500) {
                    const auto request = it->second.request;
                    ++it;
                    send(request, now);
                } else {
                    ++it;
                }
            }
            // A new day: flat futures runs move to the new near-month future; options runs
            // reload the listed options.
            if (const std::int32_t today = ist_day(now); today != day) {
                day = today;
                for (auto& slot : slots) {
                    if (!slot.restored) {
                        continue;
                    }
                    if (auto* futures = std::get_if<autotrade::Run>(&slot.run)) {
                        if (futures->state() == autotrade::State::Flat) {
                            if (const auto contract = autotrade::choose_future(
                                    futures_of(*app_db, futures->config().underlying), today)) {
                                futures->set_contract(*contract);
                            }
                        }
                    } else {
                        slot.options = listed_of(
                            *app_db, std::get<autotrade::OptionsRun>(slot.run).config().underlying,
                            "OPT");
                    }
                }
            }
        }
        if (tick >= next_second) {
            next_second = tick + std::chrono::seconds(1);
            const auto attach = [](std::optional<bus::Reader>& reader, const std::string& name) {
                if (!reader) {
                    if (auto r = bus::Reader::attach(name, {.name = "mda-autotrader"})) {
                        spdlog::info("bus: reading '{}'", name);
                        reader.emplace(std::move(*r));
                    }
                }
            };
            attach(md, config.md_bus.name);
            attach(sig, config.sig_bus.name);
            attach(exec, config.exec_bus.name);
            for (auto* reader : {&md, &sig, &exec}) {
                if (*reader) {
                    (*reader)->heartbeat();
                }
            }
            ord_auto->heartbeat();
        }
        if (busy) {
            wait.reset();
        } else {
            wait.idle();
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-autotrader", config->logging);
    spdlog::info("mda-autotrader starting (config {}, log level {})", config->source.string(),
                 config->logging.level);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run(*config);
    spdlog::info("mda-autotrader stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}
