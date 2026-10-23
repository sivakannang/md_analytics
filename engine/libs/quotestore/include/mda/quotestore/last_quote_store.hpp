#pragma once

// data/last_quotes.db: the latest market-data message of each type per instrument, so
// quotes, depth and chains have values after a restart (ADR-0024). mda-engine is the only
// writer; it and the gateway load it at startup.
//
// Payloads are the bus's fixed-layout bytes; readers decode them with messages::decode,
// which rejects a payload from another schema version.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include <mda/messages/message.hpp>
#include <mda/storage/database.hpp>

namespace mda::quotestore {

class LastQuoteStore {
public:
    [[nodiscard]] static std::expected<LastQuoteStore, storage::DbError> open(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<LastQuoteStore, storage::DbError> open_in_memory();

    /// Keeps `message` as its instrument's latest of its type, until the next flush().
    template <messages::Message T>
    void remember(const T& message) {
        const auto bytes = messages::as_bytes(message);
        pending_[{message.header.instrument_id.value(), messages::to_wire(messages::MessageTraits<T>::type)}]
            .assign(bytes.begin(), bytes.end());
    }

    /// Writes the remembered messages in one transaction; returns how many.
    [[nodiscard]] std::expected<std::size_t, storage::DbError> flush();

    /// Calls `on_message(type, payload)` for every saved message.
    [[nodiscard]] std::expected<std::size_t, storage::DbError> load(
        const std::function<void(std::uint32_t type, std::span<const std::byte> payload)>& on_message);

private:
    explicit LastQuoteStore(storage::Database db) : db_(std::move(db)) {}
    [[nodiscard]] static std::expected<LastQuoteStore, storage::DbError> prepare(storage::Database db);

    storage::Database db_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::byte>> pending_;  // (id, type)
};

}  // namespace mda::quotestore
