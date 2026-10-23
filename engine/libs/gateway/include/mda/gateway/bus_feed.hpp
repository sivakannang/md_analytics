#pragma once

// Background thread that reads the market data bus into a QuoteCache (ADR-0018).
//
// - Waits (retrying every second) until mda-feedd has created the bus.
// - Market snapshots go into the cache; FeedStatus events and the writer's heartbeat
//   decide the feed health shown to browsers.
// - Joins on destruction.

#include <chrono>
#include <string>
#include <thread>

#include <mda/gateway/feed_metrics.hpp>
#include <mda/gateway/quote_cache.hpp>

namespace mda::gateway {

struct BusFeedOptions {
    std::string bus_name = "md";
    /// Writer heartbeat older than this means mda-feedd is gone.
    std::chrono::milliseconds writer_timeout{3000};
    /// Ticks, frames, feed status and latency for the system monitor; optional.
    FeedMetrics* metrics = nullptr;
};

class BusFeed {
public:
    BusFeed(QuoteCache& cache, BusFeedOptions options);
    ~BusFeed();  // stops and joins

    BusFeed(const BusFeed&) = delete;
    BusFeed& operator=(const BusFeed&) = delete;

private:
    void run(const std::stop_token& stop);

    QuoteCache* cache_;
    BusFeedOptions options_;
    std::jthread thread_;  // last: started after the members it uses
};

}  // namespace mda::gateway
