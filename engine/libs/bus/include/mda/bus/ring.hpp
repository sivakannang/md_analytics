#pragma once

// Shared definitions for the broadcast ring (docs/design/market-data-bus.md).
//
// The ring is a region of `capacity` bytes plus a set of counters. Neither knows
// whether it lives in shared memory or on the heap: Unit 2 places them in a
// POSIX shared-memory segment, tests place them in ordinary memory.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace mda::bus {

/// Application-defined message type. 0 is reserved for padding records.
using MsgType = std::uint32_t;
inline constexpr MsgType kPaddingMsgType = 0;

/// Records start on 8-byte boundaries so they can be copied as 64-bit words.
inline constexpr std::size_t kRecordAlignment = 8;
/// Record header: u32 payload length + u32 message type, stored as one 64-bit word.
inline constexpr std::size_t kRecordHeaderSize = 8;
/// Apple M-series caches use 128-byte lines and x86 prefetches 64-byte line pairs;
/// 128-byte spacing keeps the counters from false-sharing on both.
inline constexpr std::size_t kCacheLineSize = 128;
inline constexpr std::size_t kMinCapacity = 1024;

// Atomics shared between processes must be lock-free (and therefore address-free).
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

/// Counters written by the single writer and read by all readers. All values are
/// byte positions: monotonically increasing; `position & (capacity - 1)` is the offset.
struct RingCounters {
    /// "I am about to write up to here." Readers compare against it to detect being lapped.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_intent{0};
    /// "Everything below here is published." Readers poll this.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> tail{0};
    /// Position of the most recently published record; where a lapped reader resumes.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> latest{0};
};

enum class RingError {
    CapacityNotPowerOfTwo,
    CapacityTooSmall,
    Misaligned,
};

/// Rounds a record size up to the record alignment.
[[nodiscard]] constexpr std::uint64_t align_record(std::uint64_t size) noexcept {
    return (size + kRecordAlignment - 1) & ~std::uint64_t{kRecordAlignment - 1};
}

/// Largest payload a ring of `capacity` bytes accepts (capacity / 8, as in Agrona),
/// so a single message can never consume a large part of a reader's slack.
[[nodiscard]] constexpr std::size_t max_message_length(std::size_t capacity) noexcept {
    return capacity / 8;
}

/// Checks that `data` can hold a ring: power-of-two size >= kMinCapacity, aligned for
/// 64-bit atomic access.
[[nodiscard]] std::expected<void, RingError> validate_ring(
    std::span<const std::byte> data) noexcept;

}  // namespace mda::bus
