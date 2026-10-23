#pragma once

// The admin system monitor's collector (docs/design/system-monitor.md §3–5, SM2): every 5 s
// it samples the stack's processes, the machine, the disk, the buses, the feed counters and
// latency, the recorder's status file; keeps an hour of history for the trend lines; and
// rates each card ok / warn / bad. Read-only: it never changes anything it looks at.

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/json/object.hpp>

#include <mda/gateway/feed_metrics.hpp>

namespace mda::gateway {

struct SystemMonitorOptions {
    std::filesystem::path run_dir;  ///< pid files (tools/stack.sh) and recorder.status
    std::filesystem::path data_dir; ///< the volume whose free space is shown
    std::filesystem::path bars_db;
    std::filesystem::path app_db;
    std::filesystem::path last_quotes_db;
    std::filesystem::path journal_dir;
    bool recorder_enabled = false;
    std::vector<std::string> buses;  ///< e.g. {"md", "an"}
    std::chrono::seconds interval{5};
    std::size_t history = 720;       ///< samples kept (an hour at 5 s)
    bool start_thread = true;        ///< false in tests: call sample() directly
};

/// One status check, as shown on a card and in the page header.
struct Check {
    std::string card;
    std::string level;  ///< "ok", "warn" or "bad"
    std::string text;
};

class SystemMonitor {
public:
    SystemMonitor(FeedMetrics& metrics, SystemMonitorOptions options);
    ~SystemMonitor();
    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    /// Takes one sample (the thread does this every interval).
    void sample();

    /// The latest sample with its history and checks (empty before the first sample).
    [[nodiscard]] boost::json::object report() const;

    /// IST weekday 09:15–15:30 (holidays are not known here).
    [[nodiscard]] static bool market_hours(std::int64_t unix_seconds);

private:
    struct Point {
        std::int64_t time = 0;
        double load1 = 0;
        double ticks_per_s = 0;
        double md_max_lag_pct = 0;
        std::uint64_t feed_to_gateway_p99_us = 0;
    };
    struct BusTotals {
        std::uint64_t bytes = 0;
        std::uint64_t messages = 0;
        std::chrono::steady_clock::time_point at;
    };

    void run(const std::stop_token& stop);

    FeedMetrics* metrics_;
    SystemMonitorOptions options_;
    // Sampling state (the sampling thread only).
    std::map<std::string, std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> cpu_seen_;
    std::map<std::string, BusTotals> bus_seen_;
    FeedMetrics::Counters counters_seen_;
    std::chrono::steady_clock::time_point counters_at_;
    /// Closed bars seen at each sample of the last minute: the engine writes a minute's bars
    /// in one burst, so the rate is taken over 60 s, not 5.
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> bars_seen_;
    std::chrono::steady_clock::time_point journal_at_;
    std::uint64_t journal_today_ = 0;
    std::uint64_t journal_all_ = 0;
    // Shared with report().
    mutable std::mutex mutex_;
    boost::json::object current_;
    std::deque<Point> history_;
    std::jthread thread_;  // last: started after the members it uses
};

/// The worst level of a list of checks ("ok" when empty).
[[nodiscard]] std::string worst_level(const std::vector<Check>& checks);

}  // namespace mda::gateway
