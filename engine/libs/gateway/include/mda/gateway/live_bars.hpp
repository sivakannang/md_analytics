#pragma once

// The latest 1-minute bars per instrument from mda-engine (bus "an"), for live charts:
// the forming minute and the one just closed. A reader thread fills it (AnalyticsFeed);
// stream sessions read it, like QuoteCache.

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <mda/bars/bar.hpp>
#include <mda/gateway/feed_metrics.hpp>
#include <mda/gateway/quote_cache.hpp>

namespace mda::gateway {

class LiveBars {
public:
    void update(const bars::Bar& bar);

    /// The recent bars (up to 2 per instrument, oldest first) of `ids` whose bars changed
    /// since `seen`; updates `seen`.
    [[nodiscard]] std::vector<bars::Bar> changed(std::span<const std::uint32_t> ids,
                                                 SeenVersions& seen) const;

private:
    struct Entry {
        std::vector<bars::Bar> recent;  // by start, at most 2
        std::uint64_t version = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::uint64_t version_ = 0;
};

class ChainSummaries;

/// Reads bus "an" (mda-engine): bars into LiveBars, option analytics into QuoteCache and
/// chain summaries into ChainSummaries. Waits for the bus if the engine isn't up. Joins on
/// destruction.
class AnalyticsFeed {
public:
    /// `metrics` (optional): bars and analytics counted for the system monitor.
    AnalyticsFeed(LiveBars& bars, QuoteCache& quotes, ChainSummaries& chains, std::string bus_name,
                  FeedMetrics* metrics = nullptr);
    ~AnalyticsFeed();
    AnalyticsFeed(const AnalyticsFeed&) = delete;
    AnalyticsFeed& operator=(const AnalyticsFeed&) = delete;

private:
    void run(const std::stop_token& stop);

    LiveBars* bars_;
    QuoteCache* quotes_;
    ChainSummaries* chains_;
    std::string bus_name_;
    FeedMetrics* metrics_;
    std::jthread thread_;  // last: started after the members it uses
};

}  // namespace mda::gateway
