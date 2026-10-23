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
}

TEST_F(ConfigTest, TheReplayConfigLoadsAndUsesItsOwnBusesAndFiles) {
    const auto config = load_config(MDA_REPLAY_CONFIG);
    ASSERT_TRUE(config.has_value()) << config.error();
    EXPECT_EQ(config->md_bus.name, "md-replay");
    EXPECT_EQ(config->an_bus.name, "an-replay");
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

    EXPECT_FALSE(load("feed:\n  chains:\n    - { underlying: NIFTY }\n").has_value());  // no spot
    EXPECT_FALSE(load("feed:\n  chains:\n    - { underlying: X, spot: Y, strikse: 3 }\n")
                     .has_value());  // typo
    EXPECT_FALSE(
        load("feed:\n  chains:\n    - { underlying: X, spot: Y, strikes: 0 }\n").has_value());
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
