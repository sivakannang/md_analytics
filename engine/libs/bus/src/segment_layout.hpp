#pragma once

// Layout of a bus segment in shared memory (docs/design/market-data-bus.md §4).
//
//   offset 0            SegmentHeader (metadata, writer ownership, ring counters)
//   ring_offset         ring, `capacity` bytes
//
// Every process that maps the segment must agree on this layout byte for byte, so it
// uses only fixed-width types and lock-free atomics, and is versioned: any change to
// this struct must bump kLayoutVersion.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <mda/bus/ring.hpp>

namespace mda::bus::detail {

inline constexpr std::uint64_t kMagic = 0x4D44'4142'5553'0001;  // "MDABUS" + 0x0001
inline constexpr std::uint32_t kLayoutVersion = 2;              // 2: writer stats + reader slots
/// The ring starts at this offset: a multiple of every page size we target (4 KB x86,
/// 16 KB Apple Silicon, 64 KB some ARM Linux) so the ring is page-aligned everywhere.
inline constexpr std::size_t kRingOffset = 64 * 1024;
inline constexpr std::size_t kBusNameCapacity = 32;

inline constexpr std::size_t kReaderSlotCount = 16;
inline constexpr std::size_t kReaderNameWords = 3;  // 24 characters
inline constexpr std::size_t kReaderNameCapacity = kReaderNameWords * 8;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

/// Written only by the writer (plain relaxed stores, no read-modify-write needed).
struct WriterStatsBlock {
    alignas(kCacheLineSize) std::atomic<std::uint64_t> messages;
    std::atomic<std::uint64_t> bytes;     // payload bytes
    std::atomic<std::uint64_t> rejected;  // publishes refused (too large / reserved type)
};

/// One registered reader. Ownership is the pid: a reader claims the slot by CAS'ing
/// `pid` from 0 (free) or from a dead process's pid (stale) to its own. Only the owner
/// writes the other fields; tools read them. `active` = 1 once the fields are valid.
struct ReaderSlot {
    alignas(kCacheLineSize) std::atomic<std::uint32_t> pid;
    std::atomic<std::uint32_t> active;
    // Name stored as atomic words so a tool reading it while it is being (re)written
    // is not a data race.
    std::atomic<std::uint64_t> name[kReaderNameWords];
    std::atomic<std::uint64_t> position;
    std::atomic<std::uint64_t> messages;
    std::atomic<std::uint64_t> laps;
    std::atomic<std::uint64_t> lost_bytes;
    std::atomic<std::uint64_t> heartbeat_ns;
};
static_assert(sizeof(ReaderSlot) == kCacheLineSize);

struct SegmentHeader {
    /// Written last (release) by the creator; readers check it (acquire) before
    /// trusting any other field. 0 means "still being initialized".
    std::atomic<std::uint64_t> magic;
    std::uint32_t layout_version;
    std::uint32_t reserved;
    std::uint64_t ring_offset;
    std::uint64_t capacity;
    std::uint64_t created_ns;
    char bus_name[kBusNameCapacity];

    alignas(kCacheLineSize) std::atomic<std::uint32_t> writer_pid;  // 0 = no writer
    std::atomic<std::uint64_t> writer_epoch;  // incremented each time a writer takes over
    std::atomic<std::uint64_t> heartbeat_ns;  // writer liveness, system clock

    RingCounters counters;
    WriterStatsBlock writer_stats;
    ReaderSlot readers[kReaderSlotCount];  // never read by the writer
};

static_assert(std::is_standard_layout_v<SegmentHeader>);
static_assert(sizeof(SegmentHeader) <= kRingOffset);

}  // namespace mda::bus::detail
