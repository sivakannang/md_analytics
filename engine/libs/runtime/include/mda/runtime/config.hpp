#pragma once

// Typed configuration loaded from one YAML file (ADR-0017), default config/mda.yaml.
// Every field has a default; unknown keys are rejected so typos don't silently do nothing.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mda::runtime {

struct LoggingConfig {
    std::string level = "info";                     ///< trace, debug, info, warn, error
    std::filesystem::path directory = "data/logs";  ///< empty: console only
    bool console = true;
};

struct BusConfig {
    std::string name = "md";
    std::size_t capacity_mb = 256;  ///< power of two
};

struct KiteConfig {
    std::filesystem::path session_file = "secrets/kite_session.json";
    std::filesystem::path ca_file;  ///< empty: auto-detect
    std::filesystem::path registry_file = "data/instruments/registry.csv";
    std::filesystem::path instruments_dir = "data/instruments";  ///< daily CSV cache
};

/// One option chain mda-feedd keeps subscribed around ATM (ADR-0020).
struct ChainConfig {
    std::string underlying;  ///< F&O underlying name, e.g. NIFTY
    std::string spot;  ///< contract key of the price that sets ATM, e.g. NSE|CASH|IDX|NIFTY 50;
                       ///< "" = each expiry's underlying future (MCX, ADR-0031)
    int expiries = 1;  ///< nearest N listed expiries
    int strikes = 20;  ///< ATM ± strikes
};

struct FeedConfig {
    std::string mode = "full";  ///< ltp, quote or full
    std::chrono::seconds stale_after{5};
    std::chrono::milliseconds reconnect_initial{1000};
    std::chrono::milliseconds reconnect_max{30000};
    std::chrono::seconds summary_interval{60};
    /// How often watchlist instruments are re-read from the app DB and (un)subscribed;
    /// 0 = only the static list below.
    std::chrono::seconds watchlists_refresh{2};
    /// How often every subscribed instrument's circuit limits are re-fetched from the
    /// broker's quote API (new subscriptions are fetched at once); 0 = never fetch them.
    std::chrono::minutes circuit_refresh{15};
    /// Bar history fetched for a subscribed instrument that has none (ADR-0025): this many
    /// days; 0 = never (run mda-backfill by hand).
    std::chrono::days auto_backfill{30};
    /// Contract keys to subscribe, e.g. "NSE|CASH|IDX|NIFTY 50".
    std::vector<std::string> instruments;
    /// F&O underlyings whose listed futures (all expiries) are subscribed, e.g. NIFTY.
    std::vector<std::string> futures_of;
    /// Option chains kept subscribed around ATM.
    std::vector<ChainConfig> chains;
};

struct DatabaseConfig {
    std::filesystem::path path = "data/app.db";  ///< app DB: users, watchlists, catalog, outbox
};

/// mda-engine (ADR-0021, ADR-0024).
struct EngineConfig {
    std::filesystem::path bars_db = "data/bars.db";
    std::filesystem::path last_quotes_db = "data/last_quotes.db";
};

struct GatewayConfig {
    std::string listen = "127.0.0.1:8090";
    bool allow_signup = true;
    /// How often changed quotes are pushed to browsers (conflated); 0 = fastest (10 ms).
    std::chrono::milliseconds publish_interval{250};
    /// The built website to serve (web/dist, ADR-0027); empty = pages are not served.
    std::filesystem::path web_dir;
};

/// mda-recorder (ADR-0026).
struct RecorderConfig {
    bool enabled = true;  ///< false: mda-recorder exits at once (nothing recorded)
    std::filesystem::path journal_dir = "data/journal";
    int keep_days = 90;
};

/// mda-notifier (ADR-0016, docs/design/notifications.md).
struct NotifierConfig {
    /// Bot token and SMTP login; never in this file (see config/notifier.example.json).
    std::filesystem::path secrets_file = "secrets/notifier.json";
    /// The bot's @username without "@", for the dashboard's t.me link; empty = no Telegram.
    std::string telegram_bot;
    std::chrono::seconds interval{2};  ///< how often the outbox is checked
};

/// One strategy mda-signald runs (docs/design/signals.md §3).
struct StrategyConfig {
    std::string id;        ///< e.g. oi-bias-v1
    int horizon_min = 30;  ///< minutes the signal is meant for
    int bullish = 25;      ///< score at or above: Bullish
    int bearish = -25;     ///< score at or below: Bearish
};

/// mda-signald (ADR-0028, docs/design/signals.md).
struct SignalsConfig {
    std::filesystem::path db = "data/signals.db";
    std::vector<std::string> underlyings;  ///< F&O names that have a feed.chains entry
    std::vector<std::string> sectors;      ///< contract keys of the sector indices (breadth)
    std::string vix;                       ///< India VIX's contract key (F8); empty = none
    int flat_bp = 5;  ///< outcomes within ± this many basis points are flat (no hit, no miss)
    std::vector<StrategyConfig> strategies;
};

/// mda-broker, paper trading (ADR-0029, docs/design/paper-trading.md §13).
struct PaperConfig {
    std::filesystem::path db = "data/paper.db";
    std::int64_t opening_cash = 1'000'000;  ///< rupees per new account
    std::int64_t latency_ms = 100;
    int overflow_ticks = 2;
    std::int64_t stale_after_ms = 5'000;
    double futures_margin_percent = 12.0;        ///< index futures
    double stock_futures_margin_percent = 20.0;  ///< stock futures
    int mis_square_off = 15 * 3600 + 25 * 60;    ///< seconds after 00:00 IST ("15:25" in the file)
    int daily_summary = 15 * 3600 + 35 * 60;     ///< when each trader's day is sent ("15:35")
    /// Exchange freeze quantity per underlying (units); missing = no limit.
    std::map<std::string, std::int64_t> freeze_quantity;
    /// MCX futures margin per commodity (ADR-0031); "default" for the rest.
    std::map<std::string, double> mcx_margin_percent;
    double mcx_default_margin_percent = 15.0;
};

/// One auto-trader run (ADR-0030, docs/design/auto-trader.md §3).
struct AutoRunConfig {
    std::string id;         ///< stable: tags its orders "auto:<id>"
    std::int64_t user = 0;  ///< whose paper account (app.db user id)
    std::string strategy = "follow-signal";
    std::string signal = "oi-bias-v1";  ///< the signald strategy it follows
    std::string underlying;             ///< trades this index's near-month future
    std::int64_t lots = 1;
    std::string product = "MIS";
    double stop_points = 40;
    double target_points = 80;  ///< 0 = none
    int max_hold_min = 45;
    int max_trades_per_day = 6;
    int cooldown_min = 10;
    int exit_score = 10;                   ///< hysteresis: held until the score falls through this
    std::int64_t daily_loss_stop = 5'000;  ///< rupees, P&L after charges
    bool enabled = true;
    // follow-signal-options only (auto-trader.md §10); stop_points is not used there.
    double index_stop_points = 25;     ///< the index moves this far against: exit
    double premium_stop_percent = 30;  ///< the resting SL below the entry premium
    int entry_slip_ticks = 2;
    int entry_timeout_s = 10;
    int stop_limit_ticks = 10;
    int reprice_s = 3;
};

/// mda-autotrader (ADR-0030).
struct AutotraderConfig {
    std::filesystem::path db = "data/autotrader.db";
    std::vector<AutoRunConfig> runs;
};

struct Config {
    std::filesystem::path source;  ///< file it was loaded from
    LoggingConfig logging;
    DatabaseConfig database;
    BusConfig md_bus;
    BusConfig an_bus{.name = "an", .capacity_mb = 64};         ///< analytics: mda-engine -> gateway
    BusConfig sig_bus{.name = "sig", .capacity_mb = 4};        ///< signals: mda-signald -> gateway
    BusConfig ord_gw_bus{.name = "ord-gw", .capacity_mb = 4};  ///< orders: gateway -> mda-broker
    BusConfig ord_auto_bus{.name = "ord-auto",
                           .capacity_mb = 4};              ///< orders: auto trader -> mda-broker
    BusConfig exec_bus{.name = "exec", .capacity_mb = 8};  ///< reports: mda-broker -> all
    KiteConfig kite;
    FeedConfig feed;
    EngineConfig engine;
    GatewayConfig gateway;
    NotifierConfig notifier;
    RecorderConfig recorder;
    SignalsConfig signals;
    PaperConfig paper;
    AutotraderConfig autotrader;
};

/// Loads and validates the file. The error names the problem and its line.
[[nodiscard]] std::expected<Config, std::string> load_config(const std::filesystem::path& path);

/// Value of "--config PATH" in argv, else config/mda.yaml.
[[nodiscard]] std::filesystem::path config_path_from_args(int argc, char** argv);

}  // namespace mda::runtime
