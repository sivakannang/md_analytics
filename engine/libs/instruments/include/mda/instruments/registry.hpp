#pragma once

// The instrument registry: ContractKey <-> InstrumentId (ADR-0008,
// docs/design/instruments.md).
//
// IDs are dense, start at 1 and are never reused, so they stay valid for history even
// after an exchange reuses the contract's token. Only the id <-> key mapping is
// permanent; attributes (symbol, lot size, tick size, underlying link) are refreshed by
// each day's instrument import.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <mda/core/instrument_id.hpp>
#include <mda/core/price.hpp>
#include <mda/core/quantity.hpp>
#include <mda/instruments/contract_key.hpp>

namespace mda::instruments {

struct Instrument {
    core::InstrumentId id;
    ContractKey key;
    // Daily attributes (empty until today's import):
    std::string trading_symbol;
    std::string name;  ///< broker's display name (company name for equities)
    core::Quantity lot_size;
    core::Price tick_size;
    core::InstrumentId underlying;  ///< derivatives: the index/equity they settle on
    bool listed_today = false;      ///< present in today's instrument list
};

enum class RegistryError {
    Io,             ///< file could not be read or written
    MalformedLine,  ///< a complete line that doesn't parse
    DuplicateId,
    DuplicateKey,
};

struct RegistryLoadResult;

class InstrumentRegistry {
public:
    /// The registered id for `key`, or invalid.
    [[nodiscard]] core::InstrumentId find(const ContractKey& key) const;

    /// The id for `key`, registering it with the next free id if new.
    core::InstrumentId assign(const ContractKey& key);

    /// Null if `id` is not registered.
    [[nodiscard]] const Instrument* get(core::InstrumentId id) const noexcept;
    [[nodiscard]] Instrument* get(core::InstrumentId id) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }
    /// All instruments, ordered by id (index = id - 1).
    [[nodiscard]] const std::vector<Instrument>& all() const noexcept { return instruments_; }

    /// Clears daily attributes before a new day's import.
    void begin_day();

    // ----------------------------------------------------------------- persistence

    /// Loads id <-> key pairs from `path`. A missing file gives an empty registry. A
    /// final line without a newline (a write interrupted by a crash) is ignored.
    [[nodiscard]] static std::expected<RegistryLoadResult, RegistryError> load(
        const std::filesystem::path& path);

    /// Appends instruments registered since load (or the last save) to `path`, then
    /// flushes them to disk. Append-only: existing lines are never rewritten.
    [[nodiscard]] std::expected<std::size_t, RegistryError> save_new(
        const std::filesystem::path& path);

    [[nodiscard]] std::size_t unsaved() const noexcept {
        return instruments_.size() - saved_count_;
    }

private:
    std::vector<Instrument> instruments_;  // index = id - 1
    std::map<ContractKey, core::InstrumentId> ids_;
    std::size_t saved_count_ = 0;
};

struct RegistryLoadResult {
    InstrumentRegistry registry;
    bool ignored_partial_line = false;
};

}  // namespace mda::instruments
