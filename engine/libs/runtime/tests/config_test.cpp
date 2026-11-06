#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include <mda/runtime/config.hpp>

namespace mda::runtime {
namespace {

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() /
                ("mda_config_" + std::to_string(::getpid()) + ".yaml");
    }
    void TearDown() override { std::filesystem::remove(path_); }

    std::expected<Config, std::string> load(const std::string& yaml) const {
        std::ofstream(path_) << yaml;
        return load_config(path_);
    }

    std::filesystem::path path_;
};

TEST_F(ConfigTest, TheRepositoryConfigLoads) {
    const auto config = load_config(MDA_REPO_CONFIG);
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->md_bus.name, "md");
    EXPECT_FALSE(config->feed.instruments.empty());
    ASSERT_FALSE(config->feed.chains.empty());
    EXPECT_EQ(config->feed.chains[0].underlying, "NIFTY");
    EXPECT_EQ(config->an_bus.name, "an");
    EXPECT_EQ(config->sig_bus.name, "sig");
    EXPECT_FALSE(config->signals.strategies.empty());
}

TEST_F(ConfigTest, TheReplayConfigLoadsAndUsesItsOwnBusesAndFiles) {
    const auto config = load_config(MDA_REPLAY_CONFIG);
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->md_bus.name, "md-replay");
    EXPECT_EQ(config->an_bus.name, "an-replay");
    EXPECT_EQ(config->sig_bus.name, "sig-replay");
    EXPECT_EQ(config->signals.db, "data/replay/signals.db");
    EXPECT_EQ(config->ord_gw_bus.name, "ord-gw-replay");
    EXPECT_EQ(config->exec_bus.name, "exec-replay");
    EXPECT_EQ(config->paper.db, "data/replay/paper.db");
    EXPECT_EQ(config->autotrader.db, "data/replay/autotrader.db");
    EXPECT_EQ(config->engine.bars_db, "data/replay/bars.db");
    EXPECT_EQ(config->gateway.listen, "127.0.0.1:8093");
}

TEST_F(ConfigTest, ReadsAndChecksChains) {
    const auto config = load(
        "feed:\n  chains:\n    - { underlying: NIFTY, spot: \"NSE|CASH|IDX|NIFTY 50\", expiries: "
        "2, strikes: 10 }\n"
        "engine:\n  bars_db: /tmp/b.db\n  last_quotes_db: /tmp/q.db\n");
    ASSERT_TRUE(config.has_value()) << config.error();
    ASSERT_EQ(config->feed.chains.size(), 1u);
    EXPECT_EQ(config->feed.chains[0].spot, "NSE|CASH|IDX|NIFTY 50");
    EXPECT_EQ(config->feed.chains[0].expiries, 2);
    EXPECT_EQ(config->engine.bars_db, "/tmp/b.db");
    EXPECT_EQ(config->engine.last_quotes_db, "/tmp/q.db");

    // No spot: an MCX chain, priced on each expiry's future (ADR-0031).
    EXPECT_TRUE(load("feed:\n  chains:\n    - { underlying: CRUDEOIL }\n").has_value());
    EXPECT_FALSE(load("feed:\n  chains:\n    - { spot: Y }\n").has_value());  // no underlying
    EXPECT_FALSE(load("feed:\n  chains:\n    - { underlying: X, spot: Y, strikse: 3 }\n")
                     .has_value());  // typo
    EXPECT_FALSE(
        load("feed:\n  chains:\n    - { underlying: X, spot: Y, strikes: 0 }\n").has_value());
}

TEST_F(ConfigTest, ReadsAndChecksSignals) {
    const auto config = load(
        "bus:\n  sig: { name: sig-test, capacity_mb: 8 }\n"
        "signals:\n  db: /tmp/s.db\n  underlyings: [NIFTY, SENSEX]\n"
        "  sectors: [\"NSE|CASH|IDX|NIFTY IT\"]\n  vix: \"NSE|CASH|IDX|INDIA VIX\"\n  flat_bp: 3\n"
        "  strategies:\n    - { id: oi-bias-v1, horizon_min: 15, bullish: 30, bearish: -30 }\n");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->sig_bus.name, "sig-test");
    EXPECT_EQ(config->signals.db, "/tmp/s.db");
    EXPECT_EQ(config->signals.underlyings.size(), 2u);
    EXPECT_EQ(config->signals.sectors[0], "NSE|CASH|IDX|NIFTY IT");
    EXPECT_EQ(config->signals.vix, "NSE|CASH|IDX|INDIA VIX");
    EXPECT_EQ(config->signals.flat_bp, 3);
    ASSERT_EQ(config->signals.strategies.size(), 1u);
    EXPECT_EQ(config->signals.strategies[0].horizon_min, 15);
    EXPECT_EQ(config->signals.strategies[0].bearish, -30);

    EXPECT_FALSE(load("signals:\n  strategies:\n    - { horizon_min: 15 }\n").has_value());
    EXPECT_FALSE(load("signals:\n  strategies:\n    - { id: x, horizon_min: 0 }\n").has_value());
    EXPECT_FALSE(load("signals:\n  strategies:\n    - { id: x, bullish: -30, bearish: 30 }\n")
                     .has_value());
    EXPECT_FALSE(load("signals:\n  strategys: []\n").has_value());  // typo
    EXPECT_FALSE(load("signals:\n  flat_bp: -1\n").has_value());
}

TEST_F(ConfigTest, ReadsAndChecksPaper) {
    const auto config = load(
        "bus:\n  exec: { name: exec-test, capacity_mb: 8 }\n"
        "paper:\n  db: /tmp/p.db\n  opening_cash: 500000\n  latency_ms: 50\n"
        "  futures_margin_percent: 15\n  freeze_quantity: { NIFTY: 1800, SENSEX: 1000 }\n"
        "  mcx_margin_percent: { default: 12, CRUDEOIL: 32 }\n"
        "  mis_square_off: \"15:20\"\n");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->exec_bus.name, "exec-test");
    EXPECT_EQ(config->ord_gw_bus.name, "ord-gw");
    EXPECT_EQ(config->paper.db, "/tmp/p.db");
    EXPECT_EQ(config->paper.opening_cash, 500000);
    EXPECT_EQ(config->paper.latency_ms, 50);
    EXPECT_DOUBLE_EQ(config->paper.futures_margin_percent, 15);
    EXPECT_EQ(config->paper.freeze_quantity.at("SENSEX"), 1000);
    EXPECT_DOUBLE_EQ(config->paper.mcx_margin_percent.at("CRUDEOIL"), 32);
    EXPECT_DOUBLE_EQ(config->paper.mcx_default_margin_percent, 12);
    EXPECT_FALSE(load("paper:\n  mcx_margin_percent: { GOLD: 0 }\n").has_value());
    EXPECT_EQ(config->paper.mis_square_off, 15 * 3600 + 20 * 60);
    EXPECT_EQ(config->paper.daily_summary, 15 * 3600 + 35 * 60);  // the default
    EXPECT_FALSE(load("paper:\n  daily_summary: \"25:00\"\n").has_value());
    EXPECT_FALSE(load("paper:\n  futures_margin_percent: 0\n").has_value());
    EXPECT_FALSE(load("paper:\n  latncy_ms: 5\n").has_value());  // typo
}

TEST_F(ConfigTest, ReadsAndChecksAutotraderRuns) {
    const auto config = load(
        "autotrader:\n  db: /tmp/a.db\n  runs:\n"
        "    - { id: nifty-follow, user: 1, underlying: NIFTY, stop_points: 30, enabled: false }\n");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->autotrader.db, "/tmp/a.db");
    ASSERT_EQ(config->autotrader.runs.size(), 1u);
    const auto& run = config->autotrader.runs[0];
    EXPECT_EQ(run.signal, "oi-bias-v1");
    EXPECT_EQ(run.product, "MIS");
    EXPECT_DOUBLE_EQ(run.stop_points, 30);
    EXPECT_FALSE(run.enabled);
    EXPECT_FALSE(load("autotrader:\n  runs:\n    - { id: x, underlying: NIFTY }\n").has_value());  // no user
    EXPECT_FALSE(load("autotrader:\n  runs:\n    - { id: x, user: 1, underlying: NIFTY, lots: 20 }\n").has_value());
    EXPECT_FALSE(load("autotrader:\n  runs:\n    - { id: x, user: 1, underlying: NIFTY }\n"
                      "    - { id: y, user: 1, underlying: NIFTY }\n")
                     .has_value());  // two runs on one account and index
}

TEST_F(ConfigTest, ReadsOptionsRuns) {
    const auto config = load(
        "autotrader:\n  runs:\n"
        "    - { id: nifty-follow, user: 1, underlying: NIFTY }\n"
        "    - { id: nifty-options, user: 1, underlying: NIFTY, strategy: follow-signal-options,\n"
        "        index_stop_points: 20, premium_stop_percent: 25, reprice_s: 5, exit_score: 5 }\n");
    ASSERT_TRUE(config.has_value()) << config.error();  // futures and options on one index
    ASSERT_EQ(config->autotrader.runs.size(), 2u);
    const auto& run = config->autotrader.runs[1];
    EXPECT_EQ(run.strategy, "follow-signal-options");
    EXPECT_DOUBLE_EQ(run.index_stop_points, 20);
    EXPECT_DOUBLE_EQ(run.premium_stop_percent, 25);
    EXPECT_EQ(run.entry_slip_ticks, 2);
    EXPECT_EQ(run.reprice_s, 5);
    EXPECT_EQ(run.exit_score, 5);
    EXPECT_EQ(config->autotrader.runs[0].exit_score, 10);
    EXPECT_FALSE(load("autotrader:\n  runs:\n    - { id: x, user: 1, underlying: NIFTY, exit_score: -1 }\n").has_value());
    EXPECT_FALSE(load("autotrader:\n  runs:\n    - { id: x, user: 1, underlying: NIFTY, strategy: sell-straddle }\n")
                     .has_value());
    EXPECT_FALSE(load("autotrader:\n  runs:\n    - { id: x, user: 1, underlying: NIFTY, strategy: follow-signal-options,"
                      " premium_stop_percent: 100 }\n")
                     .has_value());
}

TEST_F(ConfigTest, EmptyFileGivesDefaults) {
    const auto config = load("{}\n");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->logging.level, "info");
    EXPECT_EQ(config->md_bus.capacity_mb, 256u);
    EXPECT_EQ(config->feed.mode, "full");
    EXPECT_EQ(config->gateway.publish_interval, std::chrono::milliseconds(250));
}

TEST_F(ConfigTest, ReadsValues) {
    const auto config = load(R"(
logging: {level: debug, directory: "", console: false}
bus: {md: {name: test, capacity_mb: 64}}
feed:
  mode: quote
  stale_after_seconds: 7
  auto_backfill_days: 10
  instruments: ["NSE|CASH|IDX|NIFTY 50"]
  futures_of: [NIFTY, BANKNIFTY]
gateway: {listen: "127.0.0.1:9000", publish_interval_ms: 0}
notifier: {telegram_bot: md_analytics_bot, interval_seconds: 5}
recorder: {enabled: false}
)");
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->logging.level, "debug");
    EXPECT_TRUE(config->logging.directory.empty());
    EXPECT_EQ(config->md_bus.capacity_mb, 64u);
    EXPECT_EQ(config->feed.mode, "quote");
    EXPECT_EQ(config->feed.stale_after, std::chrono::seconds(7));
    EXPECT_EQ(config->feed.auto_backfill, std::chrono::days(10));
    EXPECT_EQ(config->feed.futures_of.size(), 2u);
    EXPECT_EQ(config->gateway.publish_interval, std::chrono::milliseconds(0));
    EXPECT_EQ(config->notifier.telegram_bot, "md_analytics_bot");
    EXPECT_EQ(config->notifier.interval, std::chrono::seconds(5));
    EXPECT_EQ(config->notifier.secrets_file, "secrets/notifier.json");
    EXPECT_FALSE(config->recorder.enabled);
}

TEST_F(ConfigTest, RejectsTyposAndBadValuesWithLineNumbers) {
    const auto typo = load("bus:\n  md:\n    capactiy_mb: 64\n");
    ASSERT_FALSE(typo.has_value());
    EXPECT_NE(typo.error().find("unknown key 'capactiy_mb'"), std::string::npos) << typo.error();
    EXPECT_NE(typo.error().find("line 3"), std::string::npos) << typo.error();

    EXPECT_FALSE(load("bus: {md: {capacity_mb: 100}}\n").has_value());  // not a power of two
    EXPECT_FALSE(load("feed: {mode: everything}\n").has_value());
    EXPECT_FALSE(load("logging: {level: loud}\n").has_value());
    EXPECT_FALSE(load("gateway: {publish_interval_ms: -5}\n").has_value());
    EXPECT_FALSE(load("feed: {stale_after_seconds: soon}\n").has_value());
}

TEST(ConfigPathTest, ComesFromArgsOrDefault) {
    char program[] = "mda";
    char flag[] = "--config";
    char value[] = "/tmp/x.yaml";
    char* with[] = {program, flag, value};
    char* without[] = {program};
    EXPECT_EQ(config_path_from_args(3, with), "/tmp/x.yaml");
    EXPECT_EQ(config_path_from_args(1, without), "config/mda.yaml");
}

}  // namespace
}  // namespace mda::runtime
