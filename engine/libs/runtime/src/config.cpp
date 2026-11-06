#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
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
                "recorder", "signals", "paper", "autotrader"});

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
    check_keys(bus, "bus", {"md", "an", "sig", "ord_gw", "ord_auto", "exec"});
    for (const auto& [name, target] :
         {std::pair{"md", &config.md_bus}, std::pair{"an", &config.an_bus},
          std::pair{"sig", &config.sig_bus}, std::pair{"ord_gw", &config.ord_gw_bus},
          std::pair{"ord_auto", &config.ord_auto_bus}, std::pair{"exec", &config.exec_bus}}) {
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
            if (chain.underlying.empty()) {
                fail(node, "a chain needs 'underlying'");  // no spot: MCX, on each expiry's future
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

    const YAML::Node signals = root["signals"];
    check_keys(signals, "signals",
               {"db", "underlyings", "sectors", "vix", "flat_bp", "strategies"});
    read_path(signals, "db", config.signals.db);
    read_list(signals, "underlyings", config.signals.underlyings);
    read_list(signals, "sectors", config.signals.sectors);
    read(signals, "vix", config.signals.vix);
    read(signals, "flat_bp", config.signals.flat_bp);
    if (config.signals.flat_bp < 0 || config.signals.flat_bp > 100) {
        fail(signals["flat_bp"], "signals.flat_bp must be 0-100");
    }
    if (signals && signals.IsMap() && signals["strategies"]) {
        const YAML::Node strategies = signals["strategies"];
        if (!strategies.IsSequence()) {
            fail(strategies, "'strategies' must be a list");
        }
        for (const auto& node : strategies) {
            check_keys(node, "signals.strategies[]", {"id", "horizon_min", "bullish", "bearish"});
            StrategyConfig strategy;
            read(node, "id", strategy.id);
            read(node, "horizon_min", strategy.horizon_min);
            read(node, "bullish", strategy.bullish);
            read(node, "bearish", strategy.bearish);
            if (strategy.id.empty()) {
                fail(node, "a strategy needs an 'id'");
            }
            if (strategy.horizon_min < 1 || strategy.horizon_min > 375) {
                fail(node, "strategy 'horizon_min' must be 1-375 (one session)");
            }
            if (strategy.bearish >= strategy.bullish || strategy.bullish > 100 ||
                strategy.bearish < -100) {
                fail(node, "strategy needs -100 <= bearish < bullish <= 100");
            }
            config.signals.strategies.push_back(std::move(strategy));
        }
    }

    const YAML::Node paper = root["paper"];
    check_keys(paper, "paper",
               {"db", "opening_cash", "latency_ms", "overflow_ticks", "stale_after_ms",
                "futures_margin_percent", "stock_futures_margin_percent", "freeze_quantity",
                "mcx_margin_percent", "mis_square_off", "daily_summary"});
    read_path(paper, "db", config.paper.db);
    read(paper, "opening_cash", config.paper.opening_cash);
    read(paper, "latency_ms", config.paper.latency_ms);
    read(paper, "overflow_ticks", config.paper.overflow_ticks);
    read(paper, "stale_after_ms", config.paper.stale_after_ms);
    read(paper, "futures_margin_percent", config.paper.futures_margin_percent);
    read(paper, "stock_futures_margin_percent", config.paper.stock_futures_margin_percent);
    for (const auto& [key, target] : {std::pair{"mis_square_off", &config.paper.mis_square_off},
                                      std::pair{"daily_summary", &config.paper.daily_summary}}) {
        std::string text;
        read(paper, key, text);
        if (text.empty()) {
            continue;
        }
        int hour = 0;
        int minute = 0;
        if (std::sscanf(text.c_str(), "%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 ||
            minute < 0 || minute > 59) {
            fail(paper[key], std::string("paper.") + key + " must be HH:MM (IST)");
        }
        *target = hour * 3600 + minute * 60;
    }
    if (config.paper.opening_cash <= 0 || config.paper.latency_ms < 0 ||
        config.paper.overflow_ticks < 0 || config.paper.stale_after_ms <= 0 ||
        config.paper.futures_margin_percent <= 0 || config.paper.futures_margin_percent > 100 ||
        config.paper.stock_futures_margin_percent <= 0 ||
        config.paper.stock_futures_margin_percent > 100) {
        fail(
            paper,
            "paper: opening_cash, stale_after_ms and the futures margins (0-100] must be positive; "
            "latency_ms and overflow_ticks not negative");
    }
    if (paper && paper.IsMap() && paper["freeze_quantity"]) {
        const YAML::Node freeze = paper["freeze_quantity"];
        if (!freeze.IsMap()) {
            fail(freeze, "'freeze_quantity' must map underlyings to quantities");
        }
        for (const auto& item : freeze) {
            config.paper.freeze_quantity[item.first.as<std::string>()] =
                item.second.as<std::int64_t>();
        }
    }
    if (paper && paper.IsMap() && paper["mcx_margin_percent"]) {
        const YAML::Node margins = paper["mcx_margin_percent"];
        if (!margins.IsMap()) {
            fail(margins,
                 "'mcx_margin_percent' must map commodities (and 'default') to percentages");
        }
        for (const auto& item : margins) {
            const auto name = item.first.as<std::string>();
            const auto percent = item.second.as<double>();
            if (percent <= 0 || percent > 100) {
                fail(item.second, "an MCX margin must be in (0, 100]");
            }
            if (name == "default") {
                config.paper.mcx_default_margin_percent = percent;
            } else {
                config.paper.mcx_margin_percent[name] = percent;
            }
        }
    }

    const YAML::Node autotrader = root["autotrader"];
    check_keys(autotrader, "autotrader", {"db", "runs"});
    read_path(autotrader, "db", config.autotrader.db);
    if (autotrader && autotrader.IsMap() && autotrader["runs"]) {
        const YAML::Node runs = autotrader["runs"];
        if (!runs.IsSequence()) {
            fail(runs, "'runs' must be a list");
        }
        for (const auto& node : runs) {
            check_keys(node, "autotrader.runs[]",
                       {"id",
                        "user",
                        "strategy",
                        "signal",
                        "underlying",
                        "lots",
                        "product",
                        "stop_points",
                        "target_points",
                        "max_hold_min",
                        "max_trades_per_day",
                        "cooldown_min",
                        "exit_score",
                        "daily_loss_stop",
                        "enabled",
                        "index_stop_points",
                        "premium_stop_percent",
                        "entry_slip_ticks",
                        "entry_timeout_s",
                        "stop_limit_ticks",
                        "reprice_s"});
            AutoRunConfig run;
            read(node, "id", run.id);
            read(node, "user", run.user);
            read(node, "strategy", run.strategy);
            read(node, "signal", run.signal);
            read(node, "underlying", run.underlying);
            read(node, "lots", run.lots);
            read(node, "product", run.product);
            read(node, "stop_points", run.stop_points);
            read(node, "target_points", run.target_points);
            read(node, "max_hold_min", run.max_hold_min);
            read(node, "max_trades_per_day", run.max_trades_per_day);
            read(node, "cooldown_min", run.cooldown_min);
            read(node, "exit_score", run.exit_score);
            read(node, "daily_loss_stop", run.daily_loss_stop);
            read(node, "enabled", run.enabled);
            read(node, "index_stop_points", run.index_stop_points);
            read(node, "premium_stop_percent", run.premium_stop_percent);
            read(node, "entry_slip_ticks", run.entry_slip_ticks);
            read(node, "entry_timeout_s", run.entry_timeout_s);
            read(node, "stop_limit_ticks", run.stop_limit_ticks);
            read(node, "reprice_s", run.reprice_s);
            if (run.id.empty() || run.user <= 0 || run.underlying.empty()) {
                fail(node, "a run needs 'id', 'user' and 'underlying'");
            }
            if (run.strategy != "follow-signal" && run.strategy != "follow-signal-options") {
                fail(node, "run 'strategy' must be follow-signal or follow-signal-options");
            }
            if (run.index_stop_points <= 0 || run.premium_stop_percent <= 0 ||
                run.premium_stop_percent >= 100 || run.entry_slip_ticks < 0 ||
                run.entry_timeout_s < 1 || run.stop_limit_ticks < 1 || run.reprice_s < 1) {
                fail(node,
                     "run: index_stop_points positive, premium_stop_percent 0-100, "
                     "entry_slip_ticks not "
                     "negative; entry_timeout_s, stop_limit_ticks and reprice_s at least 1");
            }
            if (run.exit_score < 0 || run.exit_score > 100) {
                fail(node, "run 'exit_score' must be 0-100");
            }
            if (run.product != "MIS" && run.product != "NRML") {
                fail(node, "run 'product' must be MIS or NRML");
            }
            if (run.lots < 1 || run.lots > 10 || run.stop_points <= 0 || run.target_points < 0 ||
                run.max_hold_min < 1 || run.max_trades_per_day < 1 || run.cooldown_min < 0 ||
                run.daily_loss_stop <= 0) {
                fail(node,
                     "run: lots 1-10; stop_points, max_hold_min, max_trades_per_day, "
                     "daily_loss_stop "
                     "positive; target_points and cooldown_min not negative");
            }
            for (const auto& other : config.autotrader.runs) {
                if (other.id == run.id ||
                    (other.user == run.user && other.underlying == run.underlying &&
                     other.strategy == run.strategy)) {
                    fail(node,
                         "run ids must be unique, and one run per user, underlying and strategy");
                }
            }
            config.autotrader.runs.push_back(std::move(run));
        }
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
