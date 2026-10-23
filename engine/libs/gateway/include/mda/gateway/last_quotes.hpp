#pragma once

// Fills the quote cache from data/last_quotes.db at startup (ADR-0024), so the watchlist
// shows the last price, depth and circuits before the first tick. Live ticks replace them.

#include <cstddef>
#include <expected>

#include <mda/gateway/quote_cache.hpp>
#include <mda/quotestore/last_quote_store.hpp>

namespace mda::gateway {

/// Loads every saved quote, depth and circuit-limit message; returns how many were used.
[[nodiscard]] std::expected<std::size_t, storage::DbError> load_last_quotes(
    QuoteCache& cache, quotestore::LastQuoteStore& store);

}  // namespace mda::gateway
