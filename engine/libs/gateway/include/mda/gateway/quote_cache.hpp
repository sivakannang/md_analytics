#pragma once

// Latest market snapshot per instrument, shared between the bus reader thread (writer)
// and browser stream sessions (readers). ADR-0018.
//
// One mutex guards everything: a few hundred instruments and pushes every 250 ms make
// contention negligible. (A per-instrument seqlock is the low-latency alternative.)

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mda/messages/circuit_limits.hpp>
#include <mda/options/chain_analytics.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/market_snapshot.hpp>

namespace mda::gateway {

/// What browsers are told about the market data feed.
enum class FeedHealth : std::uint8_t {
    Unavailable,    ///< no mda-feedd (bus missing or writer heartbeat stale)
    LoginRequired,  ///< Kite rejected the session token; run mda-login
    Disconnected,   ///< mda-feedd is (re)connecting to Kite
    Connected,      ///< ticks are flowing
};

[[nodiscard]] std::string_view to_string(FeedHealth health) noexcept;

/// An instrument's latest snapshot plus today's circuit limits (0 = unknown / none) and, for
/// options, the engine's analytics (ADR-0022).
struct CachedQuote {
    messages::MarketSnapshot snapshot;
    core::Price lower_circuit;
    core::Price upper_circuit;
    bool has_analytics = false;
    options::OptionAnalyticsMessage analytics{};
};

/// What a stream session has already sent: instrument id -> cache version.
using SeenVersions = std::unordered_map<std::uint32_t, std::uint64_t>;

class QuoteCache {
public:
    /// Merges a snapshot into the instrument's latest state. Field groups the snapshot
    /// doesn't carry (see SnapshotFields) keep their previous values, so an LTP-only
    /// update never wipes the day's OHLC.
    void update(const messages::MarketSnapshot& snapshot);

    /// Sets today's circuit limits; a re-sent, unchanged band is not a change.
    void update_limits(const messages::CircuitLimits& limits);

    /// Sets an option's analytics; unchanged values are not a change.
    void update_analytics(const options::OptionAnalyticsMessage& analytics);

    /// The instrument's current quote, if any.
    [[nodiscard]] std::optional<CachedQuote> get(std::uint32_t id) const;

    /// Replaces the instrument's 5-level book.
    void update_depth(const messages::DepthSnapshot& depth);

    void set_feed(FeedHealth health);
    [[nodiscard]] FeedHealth feed() const;

    /// Snapshots of `ids` that changed since `seen` recorded them; updates `seen`.
    /// An id absent from `seen` counts as never sent (so a new subscriber gets the
    /// current state at once).
    [[nodiscard]] std::vector<CachedQuote> changed(std::span<const std::uint32_t> ids,
                                                   SeenVersions& seen) const;

    /// Same as changed(), for depth (a separate subscription: few rows show depth).
    [[nodiscard]] std::vector<messages::DepthSnapshot> changed_depth(
        std::span<const std::uint32_t> ids, SeenVersions& seen) const;

    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        CachedQuote quote;
        std::uint64_t version = 0;
    };

    struct DepthEntry {
        messages::DepthSnapshot depth;
        std::uint64_t version = 0;
    };

    mutable std::mutex mutex_;  // guards everything below
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::unordered_map<std::uint32_t, DepthEntry> depths_;
    std::uint64_t version_ = 0;  // bumped on every update; versions are cache-wide
    FeedHealth feed_ = FeedHealth::Unavailable;
};

}  // namespace mda::gateway
