#pragma once

// Option-chain subscription windows (ADR-0020, docs/design/option-chain-and-bars.md §2).
//
// For each configured underlying: its nearest N listed expiries, and per expiry the listed
// strikes within ATM ± (strikes + buffer). ATM follows the spot price with hysteresis: a
// window re-centres only when ATM has moved more than `buffer` strikes from its centre, and
// strikes leaving a window stay subscribed for a grace period. The spot index and every
// listed future of the underlying are always included.
//
// Pure logic over the instrument registry; no broker or I/O. Times are Unix seconds.

#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>
#include <mda/instruments/registry.hpp>

namespace mda::subscriptions {

struct ChainSpec {
    std::string underlying;   ///< F&O underlying name, e.g. NIFTY
    core::InstrumentId spot;  ///< the index (or stock) whose price sets ATM
    int expiries = 1;         ///< nearest N listed expiries
    int strikes = 20;         ///< ATM ± strikes
};

struct ChainOptions {
    int buffer = 5;                       ///< extra strikes per side; also the re-centre threshold
    std::chrono::seconds grace{10 * 60};  ///< how long a strike stays after leaving a window
};

class ChainWindows {
public:
    ChainWindows(const instruments::InstrumentRegistry& registry, std::vector<ChainSpec> specs,
                 ChainOptions options = {});

    /// A spot price for one of the chains. Returns true if any window (re-)centred.
    bool on_spot(core::InstrumentId spot, core::Price price);

    /// Everything to subscribe at `now`: spots, futures, window strikes and strikes still in
    /// their grace period (sorted, unique). Starts grace periods for strikes that left.
    [[nodiscard]] std::vector<core::InstrumentId> wanted(std::int64_t now);

    [[nodiscard]] bool is_spot(core::InstrumentId id) const;
    [[nodiscard]] std::vector<core::InstrumentId> spots() const;

    /// A window re-centre, for the log.
    struct Recentre {
        std::string underlying;
        std::chrono::year_month_day expiry;
        core::Price from;  ///< 0 for the first centring
        core::Price to;
    };
    [[nodiscard]] std::vector<Recentre> take_recentres();

    /// One line per chain for the startup log, e.g. "NIFTY: 3 expiries (2026-10-27 ..., 81
    /// strikes ...), 3 futures".
    [[nodiscard]] std::vector<std::string> describe() const;

private:
    struct Expiry {
        std::chrono::year_month_day date;
        std::vector<core::Price> strikes;       // ascending
        std::vector<core::InstrumentId> calls;  // aligned with strikes; invalid if not listed
        std::vector<core::InstrumentId> puts;
        int centre = -1;  // index into strikes; -1 until the first spot price
    };
    struct Chain {
        ChainSpec spec;
        std::vector<Expiry> expiries;
        std::vector<core::InstrumentId> futures;
    };

    std::vector<Chain> chains_;
    ChainOptions options_;
    std::set<std::uint32_t> current_;                       // window members at the last wanted()
    std::unordered_map<std::uint32_t, std::int64_t> held_;  // id -> grace period ends
    std::vector<Recentre> recentres_;
};

}  // namespace mda::subscriptions
