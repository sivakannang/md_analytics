#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <string_view>

#include <yaml-cpp/yaml.h>

#include <mda/runtime/config.hpp>

namespace mda::runtime {
namespace {

struct ConfigError {
    std::string message;
};

[[noreturn]] void fail(const YAML::Node& node, const std::string& message) {
    const auto mark = node.Mark();
    throw ConfigError{message +
                      (mark.is_null() ? "" : " (line " + std::to_string(mark.line + 1) + ")")};
}

/// Rejects keys not in `allowed` (catches typos such as "capactiy_mb").
void check_keys(const YAML::Node& map, std::string_view section,
                std::initializer_list<std::string_view> allowed) {
    if (!map || map.IsNull()) {
        return;  // section absent: defaults apply
    }
    if (!map.IsMap()) {
        fail(map, std::string(section) + " must be a mapping");
    }
    for (const auto& item : map) {
        const auto key = item.first.as<std::string>();
        if (std::ranges::find(allowed, key) == allowed.end()) {
            fail(item.first, "unknown key '" + key + "' in " + std::string(section));
        }
    }
}

template <typename T>
void read(const YAML::Node& map, const char* key, T& out) {
    if (map && map.IsMap() && map[key]) {
        try {
            out = map[key].as<T>();
        } catch (const YAML::Exception&) {
            fail(map[key], std::string("invalid value for '") + key + "'");
        }
    }
}

void read_path(const YAML::Node& map, const char* key, std::filesystem::path& out) {
    std::string text = out.string();
    read(map, key, text);
    out = text;
}

template <typename Duration>
void read_duration(const YAML::Node& map, const char* key, Duration& out) {
    std::int64_t count = out.count();
    read(map, key, count);
    if (count < 0) {
        fail(map[key], std::string("'") + key + "' must not be negative");
    }
    out = Duration(count);
}

void read_list(const YAML::Node& map, const char* key, std::vector<std::string>& out) {
    if (map && map.IsMap() && map[key]) {
        if (!map[key].IsSequence()) {
            fail(map[key], std::string("'") + key + "' must be a list");
        }
        out.clear();
        for (const auto& item : map[key]) {
            out.push_back(item.as<std::string>());
        }
    }
}

Config parse(const YAML::Node& root) {
    Config config;
    check_keys(root, "config",
               {"logging", "database", "bus", "kite", "feed", "engine", "gateway", "notifier",
                "recorder"});

    const YAML::Node logging = root["logging"];
    check_keys(logging, "logging", {"level", "directory", "console"});
    read(logging, "level", config.logging.level);
    read_path(logging, "directory", config.logging.directory);
    read(logging, "console", config.logging.console);
    static constexpr std::array<std::string_view, 5> kLevels = {"trace", "debug", "info", "warn",
                                                                "error"};
    if (std::ranges::find(kLevels, config.logging.level) == kLevels.end()) {
        fail(logging["level"], "logging.level must be trace, debug, info, warn or error");
    }

    const YAML::Node database = root["database"];
    check_keys(database, "database", {"path"});
    read_path(database, "path", config.database.path);

    const YAML::Node bus = root["bus"];
    check_keys(bus, "bus", {"md", "an"});
    for (const auto& [name, target] :
         {std::pair{"md", &config.md_bus}, std::pair{"an", &config.an_bus}}) {
        const YAML::Node node = bus ? bus[name] : YAML::Node();
        const std::string section = std::string("bus.") + name;
        check_keys(node, section, {"name", "capacity_mb"});
        read(node, "name", target->name);
        read(node, "capacity_mb", target->capacity_mb);
        if (!std::has_single_bit(target->capacity_mb)) {
            fail(node["capacity_mb"],
                 section + ".capacity_mb must be a power of two (e.g. 64, 256)");
        }
    }

    const YAML::Node kite = root["kite"];
    check_keys(kite, "kite", {"session_file", "ca_file", "registry_file", "instruments_dir"});
    read_path(kite, "session_file", config.kite.session_file);
    read_path(kite, "ca_file", config.kite.ca_file);
    read_path(kite, "registry_file", config.kite.registry_file);
    read_path(kite, "instruments_dir", config.kite.instruments_dir);

    const YAML::Node feed = root["feed"];
    check_keys(feed, "feed",
               {"mode", "stale_after_seconds", "reconnect_initial_ms", "reconnect_max_ms",
                "summary_interval_seconds", "watchlists_refresh_seconds", "circuit_refresh_minutes",
                "auto_backfill_days", "instruments", "futures_of", "chains"});
    read(feed, "mode", config.feed.mode);
    if (config.feed.mode != "ltp" && config.feed.mode != "quote" && config.feed.mode != "full") {
        fail(feed["mode"], "feed.mode must be ltp, quote or full");
    }
    read_duration(feed, "stale_after_seconds", config.feed.stale_after);
    read_duration(feed, "reconnect_initial_ms", config.feed.reconnect_initial);
    read_duration(feed, "reconnect_max_ms", config.feed.reconnect_max);
    read_duration(feed, "summary_interval_seconds", config.feed.summary_interval);
    read_duration(feed, "watchlists_refresh_seconds", config.feed.watchlists_refresh);
    read_duration(feed, "circuit_refresh_minutes", config.feed.circuit_refresh);
    read_duration(feed, "auto_backfill_days", config.feed.auto_backfill);
    if (config.feed.auto_backfill.count() > 365) {
        fail(feed["auto_backfill_days"], "feed.auto_backfill_days must be 0-365");
    }
    read_list(feed, "instruments", config.feed.instruments);
    read_list(feed, "futures_of", config.feed.futures_of);
    if (feed && feed.IsMap() && feed["chains"]) {
        const YAML::Node chains = feed["chains"];
        if (!chains.IsSequence()) {
            fail(chains, "'chains' must be a list");
        }
        for (const auto& node : chains) {
            check_keys(node, "feed.chains[]", {"underlying", "spot", "expiries", "strikes"});
            ChainConfig chain;
            read(node, "underlying", chain.underlying);
            read(node, "spot", chain.spot);
            read(node, "expiries", chain.expiries);
            read(node, "strikes", chain.strikes);
            if (chain.underlying.empty() || chain.spot.empty()) {
                fail(node, "a chain needs 'underlying' and 'spot'");
            }
            if (chain.expiries < 1 || chain.expiries > 12 || chain.strikes < 1 ||
                chain.strikes > 100) {
                fail(node, "chain 'expiries' must be 1-12 and 'strikes' 1-100");
            }
            config.feed.chains.push_back(std::move(chain));
        }
    }

    const YAML::Node engine = root["engine"];
    check_keys(engine, "engine", {"bars_db", "last_quotes_db"});
    read_path(engine, "bars_db", config.engine.bars_db);
    read_path(engine, "last_quotes_db", config.engine.last_quotes_db);

    const YAML::Node gateway = root["gateway"];
    check_keys(gateway, "gateway", {"listen", "allow_signup", "publish_interval_ms", "web_dir"});
    read(gateway, "listen", config.gateway.listen);
    read(gateway, "allow_signup", config.gateway.allow_signup);
    read_duration(gateway, "publish_interval_ms", config.gateway.publish_interval);
    read_path(gateway, "web_dir", config.gateway.web_dir);

    const YAML::Node notifier = root["notifier"];
    check_keys(notifier, "notifier", {"secrets_file", "telegram_bot", "interval_seconds"});
    read_path(notifier, "secrets_file", config.notifier.secrets_file);
    read(notifier, "telegram_bot", config.notifier.telegram_bot);
    read_duration(notifier, "interval_seconds", config.notifier.interval);
    if (config.notifier.interval.count() == 0) {
        fail(notifier["interval_seconds"], "notifier.interval_seconds must be at least 1");
    }

    const YAML::Node recorder = root["recorder"];
    check_keys(recorder, "recorder", {"enabled", "journal_dir", "keep_days"});
    read(recorder, "enabled", config.recorder.enabled);
    read_path(recorder, "journal_dir", config.recorder.journal_dir);
    read(recorder, "keep_days", config.recorder.keep_days);
    if (config.recorder.keep_days < 1) {
        fail(recorder["keep_days"], "recorder.keep_days must be at least 1");
    }
    return config;
}

}  // namespace

std::expected<Config, std::string> load_config(const std::filesystem::path& path) {
    try {
        Config config = parse(YAML::LoadFile(path.string()));
        config.source = path;
        return config;
    } catch (const ConfigError& error) {
        return std::unexpected(path.string() + ": " + error.message);
    } catch (const YAML::BadFile&) {
        return std::unexpected(path.string() + ": cannot open config file");
    } catch (const YAML::Exception& error) {
        return std::unexpected(path.string() + ": " + error.what());
    }
}

std::filesystem::path config_path_from_args(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--config") {
            return argv[i + 1];
        }
    }
    return "config/mda.yaml";
}

}  // namespace mda::runtime
