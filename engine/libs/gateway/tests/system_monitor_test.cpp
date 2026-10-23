// The system monitor's pieces: the latency histogram, the feed counters, and the collector's
// checks on a scratch run folder (docs/design/system-monitor.md).

#include <filesystem>
#include <fstream>
#include <string>

#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <unistd.h>

#include <mda/gateway/feed_metrics.hpp>
#include <mda/gateway/system_monitor.hpp>

namespace mda::gateway {
namespace {

TEST(LatencyHistogramTest, BucketsCoverEveryValueWithinAQuarter) {
    std::size_t last = 0;
    for (std::uint64_t v : {0ull, 1ull, 15ull, 16ull, 17ull, 31ull, 32ull, 100ull, 999ull,
                            1'000'000ull, 1ull << 40}) {
        const std::size_t i = LatencyHistogram::index_of(v);
        EXPECT_GE(i, last) << v;  // monotonic
        last = i;
        EXPECT_GE(LatencyHistogram::upper_of(i), v) << v;
        EXPECT_LE(LatencyHistogram::upper_of(i), v + v / 4 + 1) << v;  // within ~25 %
    }
}

TEST(LatencyHistogramTest, TakesPercentilesAndResets) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 1000; ++v) {
        h.record(v);  // uniform 1..1000
    }
    const auto s = h.take();
    EXPECT_EQ(s.count, 1000u);
    EXPECT_EQ(s.max, 1000u);
    EXPECT_GE(s.p50, 500u);
    EXPECT_LE(s.p50, 640u);  // the bucket holding 500 ends at 511 or a little above
    EXPECT_GE(s.p99, 990u);
    EXPECT_LE(s.p99, 1000u);        // never above the max
    EXPECT_EQ(h.take().count, 0u);  // reset
}

TEST(FeedMetricsTest, CountsTicksBarsAndLatency) {
    FeedMetrics m;
    messages::MarketSnapshot snapshot{};
    snapshot.receive_ts = core::Timestamp::from_ns(1'000'000'000'000);
    snapshot.exchange_ts = core::Timestamp::from_ns(998'000'000'000);  // 2 s before
    snapshot.fields = messages::kFieldExchangeTime;
    m.on_snapshot(snapshot, 1'000'000'250'000);  // read 250 µs after it was received
    m.on_frame();
    bars::Bar forming{};
    forming.header.flags = bars::kBarForming;
    forming.start = 900;
    bars::Bar closed{};
    closed.start = 840;
    m.on_bar(forming);
    m.on_bar(closed);
    m.on_analytics();
    messages::FeedStatus status{};
    status.state = messages::FeedState::Connected;
    status.subscribed_instruments = 749;
    status.reconnects = 1;
    m.on_feed_status(status);

    const auto c = m.counters();
    EXPECT_EQ(c.snapshots, 1u);
    EXPECT_EQ(c.frames, 1u);
    EXPECT_EQ(c.bars_closed, 1u);  // the forming one isn't counted
    EXPECT_EQ(c.last_bar_start, 840);
    EXPECT_EQ(c.analytics, 1u);
    EXPECT_EQ(c.last_tick_ns, 1'000'000'000'000);
    EXPECT_EQ(c.feed_state, static_cast<std::uint8_t>(messages::FeedState::Connected));
    EXPECT_EQ(c.subscribed, 749u);
    const auto latency = m.take_latency();
    EXPECT_EQ(latency.max, 250u);
    EXPECT_EQ(m.take_exchange_lag().max, 2u);
}

TEST(SystemMonitorTest, KnowsMarketHours) {
    constexpr std::int64_t kThursday0915 = 1'792'607'400 + 9 * 3600 + 15 * 60;  // 2026-10-22 IST
    EXPECT_TRUE(SystemMonitor::market_hours(kThursday0915));
    EXPECT_FALSE(SystemMonitor::market_hours(kThursday0915 - 60));
    EXPECT_TRUE(SystemMonitor::market_hours(kThursday0915 + 6 * 3600 + 14 * 60));   // 15:29
    EXPECT_FALSE(SystemMonitor::market_hours(kThursday0915 + 6 * 3600 + 15 * 60));  // 15:30
    EXPECT_FALSE(SystemMonitor::market_hours(kThursday0915 + 2 * 86'400));          // Saturday
}

TEST(SystemMonitorTest, ReportsProcessesBusesAndTheRecordersStatus) {
    const auto dir =
        std::filesystem::temp_directory_path() / ("mda_monitor_test_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir / "run");
    std::filesystem::create_directories(dir / "journal");
    // A pid file naming this test process: alive, but not the program the name stands for.
    std::ofstream(dir / "run" / "engine.pid") << getpid();
    std::ofstream(dir / "run" / "recorder.status")
        << R"({"time":)" << std::time(nullptr) << R"(,"gaps_today":2,"records_last_minute":320})";

    FeedMetrics metrics;
    SystemMonitor monitor(metrics, {.run_dir = dir / "run",
                                    .data_dir = dir,
                                    .journal_dir = dir / "journal",
                                    .recorder_enabled = true,
                                    .buses = {"monitor-test-no-bus"},
                                    .start_thread = false});
    EXPECT_TRUE(monitor.report().empty());
    monitor.sample();
    const auto report = monitor.report();
    ASSERT_FALSE(report.empty());

    for (const auto& p : report.at("processes").as_array()) {
        EXPECT_FALSE(p.as_object().at("running").as_bool()) << p;  // engine's pid is not mda-engine
    }
    EXPECT_FALSE(report.at("buses").as_array()[0].as_object().at("available").as_bool());
    const auto& recorder = report.at("recorder").as_object();
    EXPECT_EQ(recorder.at("gaps_today").as_int64(), 2);
    EXPECT_EQ(recorder.at("records_last_minute").as_int64(), 320);

    bool gaps_warned = false;
    bool gateway_down = false;
    for (const auto& c : report.at("checks").as_array()) {
        const std::string text(c.as_object().at("text").as_string());
        gaps_warned = gaps_warned || text == "2 gaps in today's journal";
        gateway_down = gateway_down || text == "gateway is not running";
    }
    EXPECT_TRUE(gaps_warned);
    EXPECT_TRUE(gateway_down);

    EXPECT_EQ(worst_level({}), "ok");
    EXPECT_EQ(worst_level({{"a", "ok", ""}, {"b", "warn", ""}}), "warn");
    EXPECT_EQ(worst_level({{"a", "warn", ""}, {"b", "bad", ""}, {"c", "ok", ""}}), "bad");
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace mda::gateway
