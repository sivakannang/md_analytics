#include <mda/gateway/last_quotes.hpp>
#include <mda/messages/circuit_limits.hpp>
#include <mda/messages/depth_snapshot.hpp>
#include <mda/messages/market_snapshot.hpp>

namespace mda::gateway {

std::expected<std::size_t, storage::DbError> load_last_quotes(QuoteCache& cache,
                                                              quotestore::LastQuoteStore& store) {
    std::size_t used = 0;
    auto loaded = store.load([&](std::uint32_t type, std::span<const std::byte> payload) {
        if (auto snapshot = messages::decode<messages::MarketSnapshot>(type, payload)) {
            cache.update(*snapshot);
        } else if (auto depth = messages::decode<messages::DepthSnapshot>(type, payload)) {
            cache.update_depth(*depth);
        } else if (auto limits = messages::decode<messages::CircuitLimits>(type, payload)) {
            cache.update_limits(*limits);
        } else {
            return;  // another schema version: skipped
        }
        ++used;
    });
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return used;
}

}  // namespace mda::gateway
