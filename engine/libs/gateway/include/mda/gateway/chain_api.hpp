#pragma once

// Option chains (ADR-0022, docs/api/rest.md "Option chains"):
// - ChainSummaries: the engine's per-expiry summaries (forward, ATM, IV, PCR, max pain);
// - GET /api/v1/chains and GET /api/v1/chains/{underlying}/{expiry}: which chains exist and the
//   strike rows (call / put instruments) around ATM, from the instrument catalog.
// Live values reach the browser through the stream (quotes for the rows' instruments, and
// mode "chain" for summaries).

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/json/object.hpp>

#include <mda/auth/auth_service.hpp>
#include <mda/gateway/http.hpp>
#include <mda/gateway/quote_cache.hpp>
#include <mda/options/chain_analytics.hpp>
#include <mda/storage/database.hpp>

namespace mda::gateway {

/// A chain the feed keeps subscribed (from config feed.chains).
struct ChainInfo {
    std::string underlying;  ///< e.g. NIFTY
    std::string spot;        ///< contract key, e.g. NSE|CASH|IDX|NIFTY 50
    int expiries = 1;
};

class ChainSummaries {
public:
    void update(const options::ChainSummaryMessage& message);

    /// Summaries of chains whose spot is in `spots` that changed since `seen` (keyed by
    /// spot); updates `seen`.
    [[nodiscard]] std::vector<options::ChainSummary> changed(std::span<const std::uint32_t> spots,
                                                             SeenVersions& seen) const;

    /// The latest summary for one spot and expiry, if any.
    [[nodiscard]] std::optional<options::ChainSummary> get(std::uint32_t spot, std::int32_t expiry_days) const;

private:
    struct Entry {
        options::ChainSummary summary;
        std::int32_t expiry_days = 0;
        std::uint64_t version = 0;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::uint64_t version_ = 0;
};

/// The catalog id of a chain's spot (contract key "NSE|CASH|IDX|NIFTY 50"); 0 if unknown.
[[nodiscard]] std::int64_t chain_spot_id(storage::Database& db, std::string_view key_text);

[[nodiscard]] boost::json::object chain_summary_json(const options::ChainSummary& summary);

/// `chains`: which underlyings have chains (the feed's config).
void register_chain_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                           asio::any_io_executor db_executor, const QuoteCache& quotes,
                           const ChainSummaries& summaries, std::vector<ChainInfo> chains);

}  // namespace mda::gateway
