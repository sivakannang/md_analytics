#pragma once

// Option-chain analytics (ADR-0022, docs/design/option-chain-and-bars.md §4).
//
// Feed it every market snapshot; once a second, compute() gives, per option, IV, Greeks, OI
// change against the previous day's close and the buildup class, and per chain expiry the
// parity-implied forward, ATM strike / IV, PCR and max pain (over the strikes we receive).
// Single-threaded.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <mda/core/buildup.hpp>
#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>
#include <mda/messages/market_snapshot.hpp>
#include <mda/messages/message.hpp>

namespace mda::options {

/// The chain uses None (no previous OI, or no change) and the four quadrants.
using Buildup = core::Buildup;

struct OptionContract {
    core::InstrumentId id;
    core::Price strike;
    bool call = true;
};

struct ExpiryChain {
    std::chrono::year_month_day expiry;
    std::vector<OptionContract> options;
};

struct ChainDefinition {
    std::string underlying;  ///< e.g. NIFTY
    core::InstrumentId spot;
    std::vector<ExpiryChain> expiries;
};

struct OptionResult {
    core::InstrumentId id;
    bool has_iv = false;
    double iv = 0;  ///< fraction (0.15 = 15%)
    double delta = 0, gamma = 0, theta = 0, vega = 0;
    bool has_prev_oi = false;
    std::int64_t prev_oi = 0;  ///< previous day's closing OI
    Buildup buildup = Buildup::None;
};

struct ChainSummary {
    std::string underlying;
    core::InstrumentId spot;
    std::chrono::year_month_day expiry;
    double forward = 0;  ///< 0 when unknown
    double atm_strike = 0;
    double atm_iv = 0;  ///< fraction; 0 when unknown
    double pcr = 0;     ///< put OI / call OI over the received strikes
    double max_pain = 0;
    double years = 0;  ///< time to expiry
    std::int64_t call_oi = 0;
    std::int64_t put_oi = 0;
};

class ChainAnalytics {
public:
    explicit ChainAnalytics(std::vector<ChainDefinition> chains);

    void on_snapshot(const messages::MarketSnapshot& snapshot);
    /// The previous trading day's closing OI of an option (from bars.db).
    void set_prev_oi(core::InstrumentId id, std::int64_t oi);

    /// Analytics at `now` (Unix seconds) for every option with a price, and a summary per
    /// expiry with data.
    void compute(std::int64_t now, std::vector<OptionResult>& options,
                 std::vector<ChainSummary>& summaries) const;

    /// Every option instrument the chains know about.
    [[nodiscard]] std::vector<core::InstrumentId> option_ids() const;

private:
    struct Quote {
        double ltp = 0;
        double prev_close = 0;
        std::int64_t oi = 0;
        std::int64_t volume = 0;
    };
    std::vector<ChainDefinition> chains_;
    std::unordered_map<std::uint32_t, Quote> quotes_;  // options and spots
    std::unordered_map<std::uint32_t, std::int64_t> prev_oi_;
};

/// Years from `now` to 15:30 IST on `expiry`, at least one hour.
[[nodiscard]] double years_to_expiry(std::int64_t now, std::chrono::year_month_day expiry);

/// The strike minimising option writers' payout if the underlying settled there.
[[nodiscard]] double max_pain(const std::vector<double>& strikes,
                              const std::vector<std::int64_t>& call_oi,
                              const std::vector<std::int64_t>& put_oi);

[[nodiscard]] Buildup classify(double price_change, std::int64_t oi_change);

// ------------------------------------------------------------------ bus messages ("an")

/// Per-option analytics. header.instrument_id = the option. 72 bytes.
struct OptionAnalyticsMessage {
    messages::EventHeader header;
    double iv = 0;  ///< fraction; valid if flags & kHasIv
    double delta = 0, gamma = 0, theta = 0, vega = 0;
    std::int64_t prev_oi = 0;  ///< valid if flags & kHasPrevOi
    Buildup buildup = Buildup::None;
    std::uint8_t flags = 0;
    std::uint8_t reserved[6] = {};
};
inline constexpr std::uint8_t kHasIv = 1;
inline constexpr std::uint8_t kHasPrevOi = 2;

/// Per chain expiry. header.instrument_id = the spot. 104 bytes.
struct ChainSummaryMessage {
    messages::EventHeader header;
    std::int32_t expiry_days = 0;  ///< days since 1970-01-01
    std::uint32_t reserved = 0;
    char underlying[16] = {};
    double forward = 0, atm_strike = 0, atm_iv = 0, pcr = 0, max_pain = 0, years = 0;
    std::int64_t call_oi = 0, put_oi = 0;
};

[[nodiscard]] OptionAnalyticsMessage to_message(const OptionResult& result);
[[nodiscard]] ChainSummaryMessage to_message(const ChainSummary& summary);
[[nodiscard]] ChainSummary from_message(const ChainSummaryMessage& message);

static_assert(sizeof(OptionAnalyticsMessage) == 72);
static_assert(sizeof(ChainSummaryMessage) == 104);

}  // namespace mda::options

namespace mda::messages {

template <>
struct MessageTraits<options::OptionAnalyticsMessage> {
    static constexpr MessageType type = MessageType::OptionAnalytics;
    static constexpr std::uint16_t version = 1;
};
template <>
struct MessageTraits<options::ChainSummaryMessage> {
    static constexpr MessageType type = MessageType::ChainSummary;
    static constexpr std::uint16_t version = 1;
};
static_assert(Message<options::OptionAnalyticsMessage>);
static_assert(Message<options::ChainSummaryMessage>);

}  // namespace mda::messages
