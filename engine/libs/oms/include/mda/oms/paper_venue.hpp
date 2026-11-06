#pragma once

// The paper venue (docs/design/paper-trading.md §5): decides when and at what price an
// order fills, from the live book (the md bus's last price and 5-level depth). One rule set:
//
// - Latency: an order may act only `latency_ms` after it was accepted (or triggered).
// - Stale data (no update for `stale_after_ms`, or the feed down): nothing fills; a MARKET
//   order that becomes eligible then is rejected.
// - Taking (MARKET, and a LIMIT that is marketable when it first becomes eligible or right
//   after its trigger): walks the opposite side, never past a LIMIT's price; a MARKET
//   remainder beyond 5 levels fills at the 5th level ± `overflow_ticks` ticks; no
//   opposite side at all rejects a MARKET order.
// - Resting LIMIT: fills only when the market trades through it (a BUY: the LTP below its
//   price, or the ask below it), all of its remaining quantity at its own price. Touching
//   isn't enough: the queue position is unknown.
// - SL / SL-M trigger when the LTP reaches the trigger (BUY: LTP ≥ trigger; SELL: LTP ≤
//   trigger), then act as LIMIT / MARKET.
//
// No market impact: two orders can take the same displayed quantity. Deterministic: the
// result depends only on the order, the book and `now`.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <mda/core/price.hpp>
#include <mda/oms/order.hpp>
#include <mda/oms/types.hpp>

namespace mda::oms {

struct Level {
    core::Price price;
    std::int64_t quantity = 0;  ///< 0: no level
};

/// One instrument's market as the venue sees it.
struct Book {
    core::Price ltp;
    Millis ltp_at = 0;            ///< when the LTP last changed
    std::array<Level, 5> bids{};  ///< best first
    std::array<Level, 5> asks{};  ///< best first
    Millis updated_at = 0;        ///< the last quote or depth update
    core::Price tick;             ///< the instrument's tick size
    bool feed_ok = true;          ///< the feed is connected
};

struct VenueConfig {
    Millis latency_ms = 100;
    int overflow_ticks = 2;
    Millis stale_after_ms = 5'000;
};

struct Execution {
    std::int64_t quantity = 0;
    core::Price price;
};

/// What should happen to one order now. The caller applies it to the Order and Account.
struct VenueResult {
    bool triggered = false;             ///< an SL / SL-M reached its trigger
    std::vector<Execution> fills;       ///< in the order they happen
    std::optional<std::string> reject;  ///< a MARKET order that can't fill (cancel the rest)
};

class PaperVenue {
public:
    explicit PaperVenue(VenueConfig config = {}) : config_(config) {}

    /// Registers an accepted order; it may act from `now + latency`.
    void accepted(const Order& order, Millis now);
    /// Forgets an order that is final (complete, cancelled, rejected).
    void forget(std::int64_t order_id);

    /// What `order` does against `book` at `now`. Call it on every book update and timer
    /// tick for each live order; it records the order's progress (triggered, resting).
    [[nodiscard]] VenueResult evaluate(const Order& order, const Book& book, Millis now);

    [[nodiscard]] const VenueConfig& config() const noexcept { return config_; }

    /// An order's progress, stored (paper.db) so a restart fills exactly as before.
    struct State {
        Millis eligible_at = 0;
        bool resting = false;  ///< has had its chance to take; now fills only on trade-through
    };
    [[nodiscard]] std::optional<State> state(std::int64_t order_id) const;
    void restore(std::int64_t order_id, State state);

private:
    [[nodiscard]] bool fresh(const Book& book, Millis now) const noexcept;
    [[nodiscard]] std::vector<Execution> take(const Order& order, const Book& book,
                                              std::int64_t quantity) const;

    VenueConfig config_;
    std::unordered_map<std::int64_t, State> states_;
};

}  // namespace mda::oms
