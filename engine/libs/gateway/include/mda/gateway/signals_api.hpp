#pragma once

// Market signals (ADR-0028, docs/design/signals.md §7, docs/api/rest.md "Market signals"):
// - LiveSignals: the latest MarketSignal per index and strategy, read from mda-signald's bus
//   "sig" by SignalFeed, for the stream's "signals" mode;
// - GET /api/v1/signals, /api/v1/signals/{underlying} and /api/v1/signals/{underlying}/record:
//   the configured indices and strategies, a day's signals with factors and outcomes, and
//   each strategy's track record, from data/signals.db.

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/json/object.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/http.hpp>
#include <mda/gateway/quote_cache.hpp>
#include <mda/signals/market_signal.hpp>
#include <mda/signals/signal_store.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

class LiveSignals {
public:
    void update(const signals::MarketSignal& signal);

    /// The latest signals (one per strategy) of the spots in `spots` that changed since
    /// `seen`; updates `seen`.
    [[nodiscard]] std::vector<signals::MarketSignal> changed(std::span<const std::uint32_t> spots,
                                                             SeenVersions& seen) const;

private:
    struct Entry {
        signals::MarketSignal signal;
        std::uint64_t version = 0;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;  // one per spot and strategy: a handful
    std::uint64_t version_ = 0;
};

/// Reads bus "sig" (mda-signald) into LiveSignals. Waits for the bus if signald isn't up.
/// Joins on destruction.
class SignalFeed {
public:
    SignalFeed(LiveSignals& signals, std::string bus_name);
    ~SignalFeed();
    SignalFeed(const SignalFeed&) = delete;
    SignalFeed& operator=(const SignalFeed&) = delete;

private:
    void run(const std::stop_token& stop);

    LiveSignals* signals_;
    std::string bus_name_;
    std::jthread thread_;  // last: started after the members it uses
};

/// An index with signals (config signals.underlyings and its feed.chains spot).
struct SignalIndex {
    std::string underlying;  ///< e.g. NIFTY
    std::string spot;        ///< contract key, e.g. NSE|CASH|IDX|NIFTY 50
};

/// A configured strategy (config signals.strategies).
struct SignalStrategy {
    std::string id;  ///< e.g. oi-bias-v1
    int horizon_min = 0;
    int bullish = 25;   ///< score at or above: Bullish
    int bearish = -25;  ///< score at or below: Bearish
};

/// One live signal as sent on the stream.
[[nodiscard]] boost::json::object signal_json(const signals::MarketSignal& signal);

/// Queries run on `signals_executor` (signals.db's thread) after the session is checked on
/// `db_executor`.
void register_signal_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                            asio::any_io_executor db_executor, signals::SignalStore& store,
                            asio::any_io_executor signals_executor,
                            std::vector<SignalIndex> indices,
                            std::vector<SignalStrategy> strategies);

}  // namespace mda::gateway
