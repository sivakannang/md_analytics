// mda-broker — the paper broker (ADR-0029, docs/design/paper-trading.md).
//
//   mda-broker [--config config/mda.yaml]
//
// Reads quotes, depth and feed status on bus "md", order requests on "ord-gw" (the
// gateway) and "ord-auto" (the auto trader), and runs them through the Broker: pre-trade
// risk, the OMS and the paper venue filling against the live book. Every change is saved
// to data/paper.db before its execution reports go out on "exec". No real orders: there is
// no broker order client in this process.
//
// A request's source comes from the bus it arrived on, not from the message. Time is the
// md bus's (receive times) while it is flowing, so a replay (ADR-0026) runs in session
// time; the wall clock otherwise. At the close of the day it squares off MIS, expires DAY
// orders and settles expiring contracts at the underlying's close (bars.db), and at
// paper.daily_summary queues each trader's day for mda-notifier (app.db outbox).
// One thread. Stops cleanly on Ctrl+C / SIGTERM.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/appdb/outbox.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/broker/broker.hpp>
#include <mda/broker/instrument_catalog.hpp>
#include <mda/bus/bus.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/feed_status.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>
#include <mda/storage/database.hpp>

namespace {

using namespace mda;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};

void on_signal(int) {
    g_stop.store(true);
}

oms::Millis wall_ms() {
    return core::Timestamp::now().ns() / 1'000'000;
}

std::string money(oms::Paise amount) {
    return std::format("₹{:.2f}", static_cast<double>(amount) / 100.0);
}

/// Queues each trader's day (the daily summary) in app.db's outbox; the dedup key makes a
/// restart or a second run that day a no-op.
void queue_summaries(storage::Database& app_db, broker::PaperStore& paper,
                     const std::vector<broker::DailySummary>& summaries, oms::Millis session_now) {
    const std::int64_t day = (session_now + 19'800'000) / 86'400'000;
    const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{day}}};
    const std::string date =
        std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                    static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    for (const auto& s : summaries) {
        auto user = app_db.prepare("SELECT username, email FROM users WHERE id = ?1");
        if (!user) {
            continue;
        }
        user->bind(1, s.user);
        if (user->step().value_or(storage::StepResult::Done) != storage::StepResult::Row) {
            continue;  // the user was deleted
        }
        const std::string username = user->column_text(0);
        const std::string email = user->column_text(1);
        // The auto trader's share of the day, per run (its orders are tagged "auto:<run>").
        std::string automatic;
        if (const auto account = paper.account_for_user(s.user).value_or(std::nullopt)) {
            const oms::Millis from = std::int64_t{day} * 86'400'000 - 19'800'000;
            for (const auto& t : paper.auto_totals(account->id, from, from + 86'400'000)
                                     .value_or(std::vector<broker::PaperStore::TagTotals>{})) {
                automatic += std::format("{}{}: {} fills, net {}", automatic.empty() ? "" : "; ",
                                         t.tag.substr(5), t.fills, money(t.realised - t.charges));
            }
        }
        const std::string payload =
            boost::json::serialize(boost::json::object{{"username", username},
                                                       {"date", date},
                                                       {"fills", s.fills},
                                                       {"realised", money(s.realised)},
                                                       {"charges", money(s.charges)},
                                                       {"net", money(s.realised - s.charges)},
                                                       {"open_positions", s.open_positions},
                                                       {"cash", money(s.cash)},
                                                       {"auto", automatic}});
        const std::string dedup = std::format("paper.daily_summary:{}:{}", s.user, date);
        if (auto queued = appdb::enqueue(app_db,
                                         {.user_id = s.user,
                                          .type = "paper.daily_summary",
                                          .email = email,
                                          .payload = payload,
                                          .dedup_key = dedup},
                                         wall_ms() / 1000);
            !queued) {
            spdlog::error("paper: queueing the daily summary failed: {}", queued.error().message);
        }
    }
    spdlog::info("paper: daily summary for {} queued for {} traders", date, summaries.size());
}

/// Attaches to `name` as a reader if the bus exists; nullopt otherwise (try again later).
std::optional<bus::Reader> try_attach(const std::string& name, const char* as) {
    if (auto reader = bus::Reader::attach(name, {.name = as})) {
        spdlog::info("bus: reading '{}'", name);
        return std::move(*reader);
    }
    return std::nullopt;
}

int run(const runtime::Config& config) {
    auto store = broker::PaperStore::open(config.paper.db);
    if (!store) {
        spdlog::error("paper: cannot open {}: {}", config.paper.db.string(), store.error().message);
        return 1;
    }
    auto app_db = storage::Database::open(config.database.path);
    if (!app_db) {
        spdlog::error("catalog: cannot open {}: {}", config.database.path.string(),
                      app_db.error().message);
        return 1;
    }
    broker::InstrumentCatalog catalog(
        *app_db, config.paper.freeze_quantity,
        broker::McxMargins::from_percent(config.paper.mcx_margin_percent,
                                         config.paper.mcx_default_margin_percent));

    broker::BrokerConfig broker_config;
    broker_config.opening_cash = config.paper.opening_cash * 100;
    broker_config.venue = {.latency_ms = config.paper.latency_ms,
                           .overflow_ticks = config.paper.overflow_ticks,
                           .stale_after_ms = config.paper.stale_after_ms};
    broker_config.risk.futures_margin =
        static_cast<oms::Ppb>(config.paper.futures_margin_percent * 10'000'000.0);
    broker_config.risk.stock_futures_margin =
        static_cast<oms::Ppb>(config.paper.stock_futures_margin_percent * 10'000'000.0);
    broker_config.close_of_day.mis_square_off = config.paper.mis_square_off;
    // Settlement prices: the underlying's last 1-minute close of the expiry day (bars.db).
    auto bars = bars::BarStore::open(config.engine.bars_db);
    if (!bars) {
        spdlog::error("bars: cannot open {}: {}", config.engine.bars_db.string(),
                      bars.error().message);
        return 1;
    }
    const auto fsp = [&bars](core::InstrumentId underlying,
                             std::int32_t day) -> std::optional<core::Price> {
        if (!underlying.valid()) {
            return std::nullopt;
        }
        // The last minute of the day: 15:29 for an index, up to 23:54 for MCX.
        const std::int64_t day_start = std::int64_t{day} * 86'400 - 19'800;
        const auto last = bars->latest_before(underlying.value(), day_start + 86'400);
        if (!last || !*last || (*last)->start < day_start) {
            return std::nullopt;
        }
        return (*last)->close;
    };
    // The last close before a time, for a close of day missed while the broker was down.
    const auto previous_close =
        [&bars](core::InstrumentId id, oms::Millis before) -> std::optional<broker::PreviousClose> {
        const auto last = bars->latest_before(id.value(), before / 1000);
        if (!last || !*last) {
            return std::nullopt;
        }
        return broker::PreviousClose{.close = (*last)->close,
                                     .ended = ((*last)->start + 60) * 1000};
    };
    broker::Broker broker(
        broker_config, *store, [&catalog](core::InstrumentId id) { return catalog.get(id); }, fsp,
        previous_close);
    if (auto loaded = broker.load(wall_ms()); !loaded) {
        spdlog::error("paper: cannot load {}: {}", config.paper.db.string(),
                      loaded.error().message);
        return 1;
    }
    if (broker.restored_accounts() > 0) {
        spdlog::error(
            "paper: {} accounts rows were missing from {} and were rebuilt from their orders and "
            "ledger",
            broker.restored_accounts(), config.paper.db.string());
    }
    spdlog::info("paper: {} live orders restored from {}", broker.live_orders(),
                 config.paper.db.string());

    auto exec = bus::Writer::create_or_attach(
        {.name = config.exec_bus.name, .capacity = config.exec_bus.capacity_mb * 1024 * 1024});
    if (!exec) {
        spdlog::error("bus: {}: {}", config.exec_bus.name, bus::to_string(exec.error()));
        return 1;
    }
    spdlog::info("bus: writer on '{}' ({} MB, epoch {})", config.exec_bus.name,
                 config.exec_bus.capacity_mb, exec->epoch());
    const auto publish = [&](const broker::Reports& reports) {
        for (const auto& r : reports) {
            (void)exec->publish(messages::to_wire(messages::MessageType::ExecutionReport),
                                messages::as_bytes(r));
            if (r.event != broker::ReportEvent::Status) {
                spdlog::info("order {} user {}: event {} {} {}/{}{}{}", r.order_id, r.user,
                             static_cast<int>(r.event), oms::name(r.status), r.filled, r.quantity,
                             r.last_quantity > 0 ? std::format(" last {}@{:.2f}", r.last_quantity,
                                                               r.last_price.to_double())
                                                 : std::string(),
                             broker::text_of(r.reason).empty()
                                 ? std::string()
                                 : " (" + std::string(broker::text_of(r.reason)) + ")");
            }
        }
    };

    // Readers: md (market data), ord-gw and ord-auto (order sources); attached as they appear.
    std::optional<bus::Reader> md;
    std::optional<bus::Reader> ord_gw;
    std::optional<bus::Reader> ord_auto;

    // Session time: the md bus's receive times while it flows, the wall clock otherwise.
    oms::Millis event_ms = 0;
    auto event_seen = Clock::now() - std::chrono::hours(1);
    const auto now_ms = [&]() -> oms::Millis {
        const auto since = Clock::now() - event_seen;
        if (since < std::chrono::seconds(2)) {
            return event_ms + std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
        }
        return wall_ms();
    };

    bus::Backoff wait;
    auto next_tick = Clock::now();
    auto next_second = Clock::now();
    auto next_catalog = Clock::now() + std::chrono::minutes(5);
    std::int64_t summarised_day = 0;
    std::uint64_t requests = 0;
    std::uint64_t reports_sent = 0;
    while (!g_stop.load()) {
        bool busy = false;
        if (md) {
            for (int i = 0; i < 256; ++i) {  // drain market data in batches
                const auto result = md->poll([&](bus::MsgType type,
                                                 std::span<const std::byte> payload) {
                    if (auto s = messages::decode<messages::MarketSnapshot>(type, payload)) {
                        broker.on_snapshot(*s);
                        event_ms = std::max(event_ms, s->receive_ts.ns() / 1'000'000);
                        event_seen = Clock::now();
                    } else if (auto d = messages::decode<messages::DepthSnapshot>(type, payload)) {
                        broker.on_depth(*d);
                    } else if (auto f = messages::decode<messages::FeedStatus>(type, payload)) {
                        broker.on_feed(*f);
                    } else if (auto c = messages::decode<messages::CircuitLimits>(type, payload)) {
                        broker.on_circuit(*c);
                    }
                });
                if (result.status == bus::PollStatus::Empty) {
                    break;
                }
                busy = true;
                if (result.status == bus::PollStatus::Lapped) {
                    spdlog::warn("bus: broker fell behind '{}'; {} bytes skipped",
                                 config.md_bus.name, result.lost_bytes);
                }
            }
        }
        for (auto* source : {&ord_gw, &ord_auto}) {
            if (!*source) {
                continue;
            }
            const oms::Source origin = source == &ord_gw ? oms::Source::Ui : oms::Source::Auto;
            const auto result = (*source)->poll([&](bus::MsgType type,
                                                    std::span<const std::byte> payload) {
                auto request = messages::decode<broker::OrderRequest>(type, payload);
                if (!request) {
                    return;
                }
                request->source = origin;  // the bus decides, not the message
                ++requests;
                if (auto reports = broker.on_request(*request, now_ms())) {
                    publish(*reports);
                    reports_sent += reports->size();
                } else {
                    spdlog::error("paper: saving a request failed: {}", reports.error().message);
                }
            });
            if (result.status != bus::PollStatus::Empty) {
                busy = true;
                if (result.status == bus::PollStatus::Lapped) {
                    // Sources resend after their ack timeout; paper.db stays the truth.
                    spdlog::error("bus: order requests lost on {} ({} bytes)",
                                  origin == oms::Source::Ui ? config.ord_gw_bus.name
                                                            : config.ord_auto_bus.name,
                                  result.lost_bytes);
                }
            }
        }

        const auto now = Clock::now();
        if (now >= next_tick) {
            next_tick = now + std::chrono::milliseconds(20);
            if (auto reports = broker.on_tick(now_ms())) {
                publish(*reports);
                reports_sent += reports->size();
            } else {
                spdlog::error("paper: saving fills failed: {}", reports.error().message);
            }
        }
        if (now >= next_second) {
            next_second = now + std::chrono::seconds(1);
            // The daily summary: once a session day, after paper.daily_summary.
            const oms::Millis session = now_ms();
            const std::int64_t day = (session + 19'800'000) / 86'400'000;
            const std::int64_t time_of_day = (session + 19'800'000) % 86'400'000 / 1000;
            if (time_of_day >= config.paper.daily_summary && day != summarised_day) {
                summarised_day = day;
                if (const auto summaries = broker.daily_summaries(session); !summaries.empty()) {
                    queue_summaries(*app_db, *store, summaries, session);
                }
            }
            if (!md) {
                if (auto reader = try_attach(config.md_bus.name, "mda-broker")) {
                    md.emplace(std::move(*reader));
                }
            }
            if (!ord_gw) {
                if (auto reader = try_attach(config.ord_gw_bus.name, "mda-broker")) {
                    ord_gw.emplace(std::move(*reader));
                }
            }
            if (!ord_auto) {
                if (auto reader = try_attach(config.ord_auto_bus.name, "mda-broker")) {
                    ord_auto.emplace(std::move(*reader));
                }
            }
            for (auto* reader : {&md, &ord_gw, &ord_auto}) {
                if (*reader) {
                    (*reader)->heartbeat();
                }
            }
            exec->heartbeat();
        }
        if (now >= next_catalog) {
            next_catalog = now + std::chrono::minutes(5);
            catalog.clear();
            spdlog::info("summary: {} live orders, {} requests and {} reports since start",
                         broker.live_orders(), requests, reports_sent);
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
    runtime::setup_logging("mda-broker", config->logging);
    spdlog::info("mda-broker starting (config {}, log level {})", config->source.string(),
                 config->logging.level);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run(*config);
    spdlog::info("mda-broker stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}
