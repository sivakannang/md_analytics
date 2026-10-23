#pragma once

// Typed configuration loaded from one YAML file (ADR-0017), default config/mda.yaml.
// Every field has a default; unknown keys are rejected so typos don't silently do nothing.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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
    std::string spot;  ///< contract key of the price that sets ATM, e.g. NSE|CASH|IDX|NIFTY 50
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

struct Config {
    std::filesystem::path source;  ///< file it was loaded from
    LoggingConfig logging;
    DatabaseConfig database;
    BusConfig md_bus;
    BusConfig an_bus{.name = "an", .capacity_mb = 64};  ///< analytics: mda-engine -> gateway
    KiteConfig kite;
    FeedConfig feed;
    EngineConfig engine;
    GatewayConfig gateway;
    NotifierConfig notifier;
    RecorderConfig recorder;
};

/// Loads and validates the file. The error names the problem and its line.
[[nodiscard]] std::expected<Config, std::string> load_config(const std::filesystem::path& path);

/// Value of "--config PATH" in argv, else config/mda.yaml.
[[nodiscard]] std::filesystem::path config_path_from_args(int argc, char** argv);

}  // namespace mda::runtime
