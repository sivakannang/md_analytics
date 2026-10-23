#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/admin/admin.hpp>
#include <mda/bus/bus.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/gateway/system_monitor.hpp>
#include <mda/sysmon/sysmon.hpp>

namespace mda::gateway {
namespace {

namespace json = boost::json;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kIst = 19'800;
constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
/// The journal folders are summed at most this often (a day folder holds a few big files).
constexpr auto kJournalEvery = std::chrono::seconds(60);
/// A recorder status file older than this means the recorder isn't writing it.
constexpr std::int64_t kRecorderStatusStale = 180;

/// tools/stack.sh's names, and what each one's command line contains.
struct Expected {
    const char* name;
    const char* signature;
};
constexpr Expected kProcesses[] = {{"feedd", "mda-feedd"},       {"engine", "mda-engine"},
                                   {"gateway", "mda-gateway"},   {"notifier", "mda-notifier"},
                                   {"recorder", "mda-recorder"}, {"web", "npm"}};

std::int64_t unix_now() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::uint32_t read_pid(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::uint32_t pid = 0;
    in >> pid;
    return pid;
}

json::value maybe_u(const std::optional<std::uint64_t>& v) {
    return v ? json::value(*v) : json::value(nullptr);
}

json::value maybe_i(const std::optional<std::int64_t>& v) {
    return v ? json::value(*v) : json::value(nullptr);
}

std::string_view feed_state_name(std::uint8_t state) {
    switch (static_cast<messages::FeedState>(state)) {
        case messages::FeedState::Connecting:
            return "connecting";
        case messages::FeedState::Connected:
            return "connected";
        case messages::FeedState::Disconnected:
            return "disconnected";
        case messages::FeedState::Stale:
            return "stale";
        case messages::FeedState::AuthExpired:
            return "auth_expired";
    }
    return "unknown";
}

std::string gb(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / kGb);
    return text;
}

json::object summary_json(const LatencyHistogram::Summary& s) {
    return {{"count", s.count}, {"p50", s.p50}, {"p99", s.p99}, {"max", s.max}};
}

json::array checks_json(const std::vector<Check>& checks) {
    json::array out;
    for (const Check& c : checks) {
        out.push_back(json::object{{"card", c.card}, {"level", c.level}, {"text", c.text}});
    }
    return out;
}

}  // namespace

std::string worst_level(const std::vector<Check>& checks) {
    std::string worst = "ok";
    for (const Check& c : checks) {
        if (c.level == "bad") {
            return "bad";
        }
        if (c.level == "warn") {
            worst = "warn";
        }
    }
    return worst;
}

bool SystemMonitor::market_hours(std::int64_t unix_seconds) {
    const std::int64_t local = unix_seconds + kIst;
    const std::int64_t days = local / 86'400;
    const std::int64_t weekday = (days + 4) % 7;  // 1970-01-01 was a Thursday; 0 = Sunday
    const std::int64_t minute = (local % 86'400) / 60;
    return weekday >= 1 && weekday <= 5 && minute >= 9 * 60 + 15 && minute < 15 * 60 + 30;
}

SystemMonitor::SystemMonitor(FeedMetrics& metrics, SystemMonitorOptions options)
    : metrics_(&metrics), options_(std::move(options)), counters_at_(Clock::now()) {
    counters_seen_ = metrics_->counters();
    if (options_.start_thread) {
        thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
    }
}

SystemMonitor::~SystemMonitor() {
    thread_.request_stop();
}

void SystemMonitor::run(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
        try {
            sample();
        } catch (const std::exception& e) {
            spdlog::error("system monitor: sample failed: {}", e.what());
        }
        const auto until = Clock::now() + options_.interval;
        while (!stop.stop_requested() && Clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void SystemMonitor::sample() {
    const auto now_steady = Clock::now();
    const std::int64_t now = unix_now();
    const std::int64_t now_ns = core::Timestamp::now().ns();
    const bool market = market_hours(now);
    std::vector<Check> checks;
    const auto check = [&](std::string card, std::string level, std::string text) {
        checks.push_back({std::move(card), std::move(level), std::move(text)});
    };

    // Machine.
    const sysmon::MachineStats machine = sysmon::machine_stats();
    json::object machine_json{
        {"load1", machine.load1 ? json::value(*machine.load1) : json::value(nullptr)},
        {"cores", machine.cores},
        {"memory_used", maybe_u(machine.memory_used)},
        {"memory_total", maybe_u(machine.memory_total)},
        {"power", machine.power ? json::value(*machine.power) : json::value(nullptr)},
        {"battery_percent",
         machine.battery_percent ? json::value(*machine.battery_percent) : json::value(nullptr)},
        {"uptime_s", maybe_i(machine.uptime_s)}};
    if (machine.power == "battery") {
        const int percent = machine.battery_percent.value_or(100);
        check("machine", percent < 20 ? "bad" : "warn",
              "on battery (" + std::to_string(percent) + "%)");
    }

    // Processes, from tools/stack.sh's pid files; CPU % from the CPU time used since the last
    // sample.
    json::array processes;
    std::map<std::string, bool> running;
    for (const auto& [name, signature] : kProcesses) {
        const std::uint32_t pid = read_pid(options_.run_dir / (std::string(name) + ".pid"));
        std::optional<sysmon::ProcessStats> stats;
        if (pid != 0 && sysmon::command_line(pid).find(signature) != std::string::npos) {
            stats = sysmon::process_stats(pid);
        }
        running[name] = stats.has_value();
        json::object row{{"name", name}, {"running", stats.has_value()}};
        if (stats) {
            std::uint64_t cpu = stats->cpu_time_ns.value_or(0);
            std::uint64_t rss = stats->rss_bytes.value_or(0);
            if (std::string_view(name) == "web") {
                // npm does nothing itself; the dev server is its descendant.
                for (const std::uint32_t child : sysmon::descendant_pids(pid)) {
                    if (const auto c = sysmon::process_stats(child)) {
                        cpu += c->cpu_time_ns.value_or(0);
                        rss += c->rss_bytes.value_or(0);
                    }
                }
            }
            row["pid"] = pid;
            row["started"] = maybe_i(stats->started);
            row["rss_bytes"] = rss;
            json::value percent = nullptr;
            if (const auto seen = cpu_seen_.find(name);
                seen != cpu_seen_.end() && cpu >= seen->second.first) {
                const auto wall =
                    std::chrono::duration<double, std::nano>(now_steady - seen->second.second)
                        .count();
                if (wall > 0) {
                    percent =
                        std::round(static_cast<double>(cpu - seen->second.first) / wall * 1000.0) /
                        10.0;
                }
            }
            row["cpu_percent"] = percent;
            cpu_seen_[name] = {cpu, now_steady};
        } else {
            cpu_seen_.erase(name);
        }
        processes.push_back(std::move(row));
    }
    for (const char* name : {"feedd", "engine", "gateway"}) {
        if (!running[name] && (market || std::string_view(name) == "gateway")) {
            check("processes", "bad", std::string(name) + " is not running");
        }
    }
    if (!running["notifier"]) {
        check("processes", "warn", "notifier is not running");
    }
    if (options_.recorder_enabled && market && !running["recorder"]) {
        check("processes", "warn", "recorder is enabled but not running");
    }

    // Disk.
    json::object disk_json;
    if (const auto space = sysmon::disk_space(options_.data_dir)) {
        disk_json["free"] = space->free;
        disk_json["total"] = space->total;
        if (space->free < 5ull << 30) {
            check("disk", "bad", gb(space->free) + " free");
        } else if (space->free < 20ull << 30) {
            check("disk", "warn", gb(space->free) + " free");
        }
    }
    disk_json["files"] =
        json::object{{"bars_db", sysmon::size_of_file(options_.bars_db)},
                     {"app_db", sysmon::size_of_file(options_.app_db)},
                     {"last_quotes_db", sysmon::size_of_file(options_.last_quotes_db)}};
    if (journal_at_ == Clock::time_point{} || now_steady - journal_at_ >= kJournalEvery) {
        journal_at_ = now_steady;
        journal_today_ = sysmon::size_of_folder(options_.journal_dir / admin::ist_day(now));
        journal_all_ = sysmon::size_of_folder(options_.journal_dir);
    }
    disk_json["journal_today"] = journal_today_;
    disk_json["journal_all"] = journal_all_;

    // Buses: rates from the writer's counters since the last sample; each reader's lag.
    json::array buses;
    double md_max_lag_pct = 0;
    for (const std::string& name : options_.buses) {
        const auto snapshot = bus::inspect_bus(name);
        if (!snapshot) {
            buses.push_back(json::object{{"name", name}, {"available", false}});
            if (market) {
                check("buses", "bad", "bus " + name + " is not available");
            }
            continue;
        }
        json::object row{{"name", name},
                         {"available", true},
                         {"capacity", snapshot->capacity},
                         {"writer_alive", snapshot->writer_alive},
                         {"writer_pid", snapshot->writer.pid}};
        json::value heartbeat_age = nullptr;
        if (snapshot->writer.pid != 0 && snapshot->writer.heartbeat_ns > 0) {
            const std::int64_t age_ms =
                (now_ns - static_cast<std::int64_t>(snapshot->writer.heartbeat_ns)) / 1'000'000;
            heartbeat_age = age_ms;
            if (snapshot->writer_alive && age_ms > 10'000) {
                check("buses", "bad",
                      name + " writer heartbeat " + std::to_string(age_ms / 1000) + " s old");
            } else if (snapshot->writer_alive && age_ms > 2'000) {
                check("buses", "warn",
                      name + " writer heartbeat " + std::to_string(age_ms / 1000) + " s old");
            }
        }
        row["heartbeat_age_ms"] = heartbeat_age;
        double bytes_per_s = 0;
        double messages_per_s = 0;
        if (const auto seen = bus_seen_.find(name); seen != bus_seen_.end()) {
            const double seconds =
                std::chrono::duration<double>(now_steady - seen->second.at).count();
            if (seconds > 0 && snapshot->writer_stats.bytes >= seen->second.bytes &&
                snapshot->writer_stats.messages >= seen->second.messages) {
                bytes_per_s =
                    static_cast<double>(snapshot->writer_stats.bytes - seen->second.bytes) /
                    seconds;
                messages_per_s =
                    static_cast<double>(snapshot->writer_stats.messages - seen->second.messages) /
                    seconds;
            }
        }
        bus_seen_[name] = {snapshot->writer_stats.bytes, snapshot->writer_stats.messages,
                           now_steady};
        row["bytes_per_s"] = std::round(bytes_per_s);
        row["messages_per_s"] = std::round(messages_per_s);
        json::array readers;
        std::uint64_t stale_slots = 0;
        for (const bus::ReaderInfo& reader : snapshot->readers) {
            if (!reader.process_alive) {
                ++stale_slots;  // a stopped reader's slot: its position means nothing now
                continue;
            }
            const double lag_pct = snapshot->capacity > 0
                                       ? static_cast<double>(reader.lag_bytes) * 100.0 /
                                             static_cast<double>(snapshot->capacity)
                                       : 0.0;
            json::value reader_age = nullptr;
            if (reader.heartbeat_ns > 0) {
                reader_age = (now_ns - static_cast<std::int64_t>(reader.heartbeat_ns)) / 1'000'000;
            }
            readers.push_back(json::object{{"name", reader.name},
                                           {"pid", reader.pid},
                                           {"alive", reader.process_alive},
                                           {"lag_bytes", reader.lag_bytes},
                                           {"lag_percent", std::round(lag_pct * 100.0) / 100.0},
                                           {"laps", reader.stats.laps},
                                           {"lost_bytes", reader.stats.lost_bytes},
                                           {"heartbeat_age_ms", reader_age}});
            if (name == "md") {
                md_max_lag_pct = std::max(md_max_lag_pct, lag_pct);
            }
            const std::string who = name + " / " + reader.name;
            if (lag_pct > 60) {
                check("buses", "bad",
                      who + " lag " + std::to_string(static_cast<int>(lag_pct)) + "% of the ring");
            } else if (lag_pct > 25) {
                check("buses", "warn",
                      who + " lag " + std::to_string(static_cast<int>(lag_pct)) + "% of the ring");
            }
            if (reader.stats.laps > 0) {
                check("buses", "warn",
                      who + " was lapped " + std::to_string(reader.stats.laps) + " times");
            }
        }
        row["readers"] = std::move(readers);
        row["stale_slots"] = stale_slots;
        buses.push_back(std::move(row));
    }

    // Feed, engine and latency from the gateway's own bus readers.
    const FeedMetrics::Counters counters = metrics_->counters();
    const double since =
        std::max(1e-3, std::chrono::duration<double>(now_steady - counters_at_).count());
    const auto per_s = [&](std::uint64_t now_count, std::uint64_t then_count) {
        return now_count >= then_count ? static_cast<double>(now_count - then_count) / since : 0.0;
    };
    const double ticks_per_s = per_s(counters.snapshots, counters_seen_.snapshots);
    json::value last_tick_age = nullptr;
    if (counters.last_tick_ns > 0) {
        const std::int64_t age_ms = (now_ns - counters.last_tick_ns) / 1'000'000;
        last_tick_age = age_ms;
        if (market && age_ms > 30'000) {
            check("feed", "bad", "no tick for " + std::to_string(age_ms / 1000) + " s");
        } else if (market && age_ms > 5'000) {
            check("feed", "warn", "no tick for " + std::to_string(age_ms / 1000) + " s");
        }
    }
    const std::string_view state = feed_state_name(counters.feed_state);
    if (market && counters.feed_state != 0 && state != "connected") {
        check("feed", "bad", "feed " + std::string(state));
    }
    if (counters.reconnects > 0) {
        check("feed", "warn",
              std::to_string(counters.reconnects) + " reconnects since mda-feedd started");
    }
    json::object feed_json{
        {"state", counters.feed_state == 0 ? json::value(nullptr) : json::value(state)},
        {"subscribed", counters.subscribed},
        {"reconnects", counters.reconnects},
        {"ticks_per_s", std::round(ticks_per_s)},
        {"frames_per_s", std::round(per_s(counters.frames, counters_seen_.frames) * 10.0) / 10.0},
        {"last_tick_age_ms", last_tick_age}};
    const LatencyHistogram::Summary latency = metrics_->take_latency();
    const LatencyHistogram::Summary exchange = metrics_->take_exchange_lag();
    if (latency.count > 0 && latency.p99 > 50'000) {
        check("latency", "bad", "feed → gateway p99 " + std::to_string(latency.p99 / 1000) + " ms");
    } else if (latency.count > 0 && latency.p99 > 5'000) {
        check("latency", "warn",
              "feed → gateway p99 " + std::to_string(latency.p99 / 1000) + " ms");
    }
    bars_seen_.emplace_back(now_steady, counters.bars_closed);
    while (bars_seen_.size() > 1 && now_steady - bars_seen_[1].first >= std::chrono::seconds(60)) {
        bars_seen_.pop_front();  // keep the newest sample at least a minute old
    }
    json::value bars_per_min = nullptr;
    if (now_steady - bars_seen_.front().first >= std::chrono::seconds(55)) {
        bars_per_min = counters.bars_closed - bars_seen_.front().second;
    }
    json::object engine_json{
        {"bars_per_min", bars_per_min},
        {"last_bar",
         counters.last_bar_start > 0 ? json::value(counters.last_bar_start) : json::value(nullptr)},
        {"analytics_per_s", std::round(per_s(counters.analytics, counters_seen_.analytics))}};
    counters_seen_ = counters;
    counters_at_ = now_steady;

    // Recorder: its status file (written each minute) and the journal sizes.
    json::object recorder_json{{"enabled", options_.recorder_enabled},
                               {"running", running["recorder"]},
                               {"journal_today", journal_today_},
                               {"journal_all", journal_all_}};
    {
        std::ifstream in(options_.run_dir / "recorder.status");
        std::stringstream text;
        text << in.rdbuf();
        boost::system::error_code ec;
        const json::value status = json::parse(text.str(), ec);
        if (!ec && status.is_object()) {
            const auto& s = status.as_object();
            const std::int64_t at = s.contains("time") ? s.at("time").to_number<std::int64_t>() : 0;
            recorder_json["status_age_s"] = now - at;
            if (now - at <= kRecorderStatusStale) {
                const auto gaps =
                    s.contains("gaps_today") ? s.at("gaps_today").to_number<std::int64_t>() : 0;
                recorder_json["gaps_today"] = gaps;
                recorder_json["records_last_minute"] = s.contains("records_last_minute")
                                                           ? s.at("records_last_minute")
                                                           : json::value(nullptr);
                if (gaps > 0) {
                    check("recorder", "warn", std::to_string(gaps) + " gaps in today's journal");
                }
            }
        }
    }

    Point point{.time = now,
                .load1 = machine.load1.value_or(0),
                .ticks_per_s = ticks_per_s,
                .md_max_lag_pct = md_max_lag_pct,
                .feed_to_gateway_p99_us = latency.p99};

    json::object report{{"time", now},
                        {"market_hours", market},
                        {"processes", std::move(processes)},
                        {"machine", std::move(machine_json)},
                        {"disk", std::move(disk_json)},
                        {"buses", std::move(buses)},
                        {"feed", std::move(feed_json)},
                        {"latency", json::object{{"feed_to_gateway_us", summary_json(latency)},
                                                 {"exchange_to_feed_s", summary_json(exchange)}}},
                        {"engine", std::move(engine_json)},
                        {"recorder", std::move(recorder_json)},
                        {"checks", checks_json(checks)}};
    std::scoped_lock lock(mutex_);
    current_ = std::move(report);
    history_.push_back(point);
    while (history_.size() > options_.history) {
        history_.pop_front();
    }
}

json::object SystemMonitor::report() const {
    std::scoped_lock lock(mutex_);
    if (current_.empty()) {
        return {};
    }
    json::object out = current_;
    json::array load1;
    json::array ticks;
    json::array lag;
    json::array p99;
    for (const Point& p : history_) {
        load1.push_back(std::round(p.load1 * 100.0) / 100.0);
        ticks.push_back(std::round(p.ticks_per_s));
        lag.push_back(std::round(p.md_max_lag_pct * 100.0) / 100.0);
        p99.push_back(p.feed_to_gateway_p99_us);
    }
    out["history"] = json::object{{"step_s", options_.interval.count()},
                                  {"start", history_.empty() ? 0 : history_.front().time},
                                  {"load1", std::move(load1)},
                                  {"ticks_per_s", std::move(ticks)},
                                  {"md_max_lag_pct", std::move(lag)},
                                  {"feed_to_gateway_p99_us", std::move(p99)}};
    return out;
}

}  // namespace mda::gateway
