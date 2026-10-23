// mda-gateway — REST API for the dashboard (docs/api/rest.md).
//
//   mda-gateway [--config config/mda.yaml]
//
// Settings come from the `gateway` section of the config file (ADR-0017).
// Threads: one I/O thread (HTTP and the live stream), one auth worker (Argon2 hashing and
// all SQLite access) and one bus reader (market data -> quote cache).
// Stops cleanly on Ctrl+C / SIGTERM.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <mda/admin/admin.hpp>
#include <mda/appdb/schema.hpp>
#include <mda/auth/auth_service.hpp>
#include <mda/auth/user_store.hpp>
#include <mda/bars/bar_store.hpp>
#include <mda/gateway/admin_api.hpp>
#include <mda/gateway/bars_api.hpp>
#include <mda/gateway/bus_feed.hpp>
#include <mda/gateway/last_quotes.hpp>
#include <mda/gateway/notifications_api.hpp>
#include <mda/gateway/oi_change_api.hpp>
#include <mda/gateway/chain_api.hpp>
#include <mda/gateway/live_bars.hpp>
#include <mda/gateway/quote_cache.hpp>
#include <mda/gateway/server.hpp>
#include <mda/gateway/static_site.hpp>
#include <mda/gateway/stream_api.hpp>
#include <mda/gateway/system_api.hpp>
#include <mda/gateway/system_monitor.hpp>
#include <mda/gateway/usage_tracker.hpp>
#include <mda/gateway/user_api.hpp>
#include <mda/gateway/watchlist_api.hpp>
#include <mda/runtime/config.hpp>
#include <mda/runtime/logging.hpp>
#include <mda/storage/database.hpp>

namespace {

using namespace mda;
namespace asio = boost::asio;

constexpr auto kUsageFlush = std::chrono::seconds(60);
constexpr auto kEventRetention = std::chrono::days(90);

/// Adds the tracker's unsaved counts to usage_daily. Runs on the DB thread.
void flush_usage(gateway::UsageTracker& usage, storage::Database& db) {
    for (const auto& pending : usage.take()) {
        if (auto added = admin::add_usage(db, pending.user_id, pending.day, pending.usage);
            !added) {
            spdlog::error("usage: saving user {} {} failed: {}", pending.user_id, pending.day,
                          added.error().message);
        }
    }
}

/// Daily housekeeping on the DB thread: expired sessions (logged) and old auth events.
void housekeeping(auth::AuthService& auth, storage::Database& db) {
    const int expired = auth.purge_expired_sessions();
    const auto cutoff = auth::system_clock_seconds() -
                        std::chrono::duration_cast<std::chrono::seconds>(kEventRetention).count();
    const auto purged = admin::purge_events(db, cutoff);
    spdlog::info(
        "housekeeping: {} expired sessions removed, {} auth events older than 90 days purged",
        expired, purged.value_or(0));
}

asio::awaitable<void> every(std::chrono::steady_clock::duration period,
                            std::function<void()> work) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (true) {
        timer.expires_after(period);
        co_await timer.async_wait(asio::use_awaitable);
        work();
    }
}

int run(const runtime::Config& config) {
    const auto& settings = config.gateway;
    const std::size_t colon = settings.listen.rfind(':');
    std::uint16_t port = 0;
    const std::string host = settings.listen.substr(0, colon);
    if (colon == std::string::npos ||
        std::from_chars(settings.listen.data() + colon + 1,
                        settings.listen.data() + settings.listen.size(), port)
                .ec != std::errc{}) {
        spdlog::error("config: gateway.listen must be HOST:PORT, got '{}'", settings.listen);
        return 1;
    }

    // Database: open and migrate before serving anything.
    auto db = storage::Database::open(config.database.path);
    if (!db) {
        spdlog::error("database: cannot open {}: {}", config.database.path.string(),
                      db.error().message);
        return 1;
    }
    const int version_before = db->schema_version().value_or(-1);
    if (auto migrated = db->migrate(appdb::migrations()); !migrated) {
        spdlog::error("database: migrating {}: {}", config.database.path.string(),
                      migrated.error().message);
        return 1;
    }
    const int version = db->schema_version().value_or(-1);
    if (version != version_before) {
        spdlog::info("database: {} migrated from schema {} to {}", config.database.path.string(),
                     version_before, version);
    } else {
        spdlog::info("database: {} at schema {}", config.database.path.string(), version);
    }
    auth::UserStore store(*db);
    const auth::AuthOptions auth_options{.allow_signup = settings.allow_signup};
    auth::AuthService auth(store, auth_options);
    spdlog::info("auth: signup {}", settings.allow_signup ? "open" : "disabled");
    housekeeping(auth, *db);

    // Per-user traffic and live connections for the admin dashboard (ADR-0019).
    gateway::UsageTracker usage(auth::system_clock_seconds);
    auth.set_token_observer([&usage](std::string_view token, std::int64_t user_id) {
        usage.remember_token(token, user_id);
    });

    // Chart history (bars.db, written by mda-engine): its own connection and thread, so
    // chart queries never delay sign-ins or watchlists.
    auto bar_store = bars::BarStore::open(config.engine.bars_db);
    if (!bar_store) {
        spdlog::error("bars: cannot open {}: {}", config.engine.bars_db.string(),
                      bar_store.error().message);
        return 1;
    }
    spdlog::info("bars: reading chart history from {}", config.engine.bars_db.string());

    asio::io_context io;
    asio::thread_pool bars_worker(1);
    asio::thread_pool auth_worker(1);  // the only thread that touches the app DB from now on
    gateway::Router router;
    watchlists::WatchlistService lists(*db);
    gateway::register_user_routes(router, auth, auth_worker.get_executor());
    gateway::register_watchlist_routes(router, auth, lists, *db, auth_worker.get_executor());
    gateway::register_admin_routes(router, auth, *db, auth_worker.get_executor(), usage,
                                   auth_options.session_idle_timeout);
    gateway::register_bars_routes(router, auth, *db, auth_worker.get_executor(), *bar_store,
                                  bars_worker.get_executor());
    gateway::register_notification_routes(router, auth, *db, auth_worker.get_executor(), config.notifier.telegram_bot);
    // System monitor (docs/design/system-monitor.md): fed by the bus readers below; the
    // data folder is where bars.db lives (tools/stack.sh keeps pid files in its run/).
    gateway::FeedMetrics feed_metrics;
    const auto data_dir = config.engine.bars_db.parent_path();
    gateway::SystemMonitor system_monitor(feed_metrics,
                                          {.run_dir = data_dir / "run",
                                           .data_dir = data_dir,
                                           .bars_db = config.engine.bars_db,
                                           .app_db = config.database.path,
                                           .last_quotes_db = config.engine.last_quotes_db,
                                           .journal_dir = config.recorder.journal_dir,
                                           .recorder_enabled = config.recorder.enabled,
                                           .buses = {config.md_bus.name, config.an_bus.name}});
    gateway::register_system_routes(router, auth, *db, auth_worker.get_executor(), system_monitor);
    // The built website on the same origin (ADR-0027): any non-/api path no route matches.
    std::optional<gateway::StaticSite> site;
    if (!settings.web_dir.empty()) {
        site.emplace(settings.web_dir);
        spdlog::info("web: serving {}{}", site->root().string(),
                     site->available() ? "" : " (not built yet: npm --prefix web run build)");
        router.set_fallback([&site](const gateway::Request& request,
                                    const gateway::PathParams&) -> asio::awaitable<gateway::Response> {
            co_return site->serve(request);
        });
    }
    gateway::HttpServer server(io.get_executor(), router);
    gateway::QuoteCache quotes;
    gateway::LiveBars live_bars;
    gateway::ChainSummaries chain_summaries;
    std::vector<gateway::ChainInfo> chain_info;
    for (const auto& chain : config.feed.chains) {
        chain_info.push_back({.underlying = chain.underlying, .spot = chain.spot, .expiries = chain.expiries});
    }
    gateway::register_oi_change_routes(router, auth, *db, auth_worker.get_executor(), *bar_store,
                                       bars_worker.get_executor(), chain_info);
    gateway::register_chain_routes(router, auth, *db, auth_worker.get_executor(), quotes, chain_summaries,
                                   std::move(chain_info));
    gateway::register_stream(
        server, auth, auth_worker.get_executor(), quotes,
        {.publish_interval = settings.publish_interval, .usage = &usage, .bars = &live_bars, .chains = &chain_summaries});
    server.on_response([&usage](const gateway::Request& request, std::size_t bytes) {
        usage.meter_http(request, bytes);
    });
    // Last-known quotes (ADR-0024) before the bus, so live ticks replace them.
    if (auto saved = quotestore::LastQuoteStore::open(config.engine.last_quotes_db); !saved) {
        spdlog::warn("last quotes: cannot open {}: {}", config.engine.last_quotes_db.string(),
                     saved.error().message);
    } else if (const auto used = gateway::load_last_quotes(quotes, *saved); !used) {
        spdlog::warn("last quotes: loading failed: {}", used.error().message);
    } else {
        spdlog::info("last quotes: {} saved messages loaded", *used);
    }
    gateway::BusFeed bus_feed(quotes, {.bus_name = config.md_bus.name, .metrics = &feed_metrics});
    gateway::AnalyticsFeed analytics_feed(live_bars, quotes, chain_summaries, config.an_bus.name, &feed_metrics);

    boost::system::error_code ec;
    const auto address = asio::ip::make_address(host, ec);
    if (!ec) {
        ec = server.listen({address, port});
    }
    if (ec) {
        spdlog::error("http: cannot listen on {}: {}", settings.listen, ec.message());
        return 1;
    }
    spdlog::info("http: listening on http://{}:{} (live stream at {}, push interval {} ms)", host,
                 server.local_endpoint().port(), gateway::kStreamPath,
                 settings.publish_interval.count());

    asio::co_spawn(
        io, every(kUsageFlush, [&] { asio::post(auth_worker, [&] { flush_usage(usage, *db); }); }),
        asio::detached);
    asio::co_spawn(io,
                   every(std::chrono::hours(24),
                         [&] { asio::post(auth_worker, [&] { housekeeping(auth, *db); }); }),
                   asio::detached);

    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        spdlog::info("shutting down");
        server.stop();
        io.stop();
    });
    io.run();
    bars_worker.join();
    auth_worker.join();
    flush_usage(usage, *db);  // the last minute's counts (the DB thread has stopped)
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto config = runtime::load_config(runtime::config_path_from_args(argc, argv));
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    runtime::setup_logging("mda-gateway", config->logging);
    spdlog::info("mda-gateway starting (config {}, log level {})", config->source.string(),
                 config->logging.level);
    const int code = run(*config);
    spdlog::info("mda-gateway stopped (exit code {})", code);
    runtime::shutdown_logging();
    return code;
}
