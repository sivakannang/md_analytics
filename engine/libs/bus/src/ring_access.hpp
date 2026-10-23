#pragma once

// Private helpers for reading and writing ring memory (not part of the public API).
//
// The writer may overwrite bytes while a reader is copying them. With plain loads and
// stores that would be a data race (undefined behaviour) even though readers discard
// torn copies. Every access to ring memory therefore goes through 64-bit
// std::atomic_ref with relaxed ordering: race-free by the standard, yet compiled to
// ordinary loads/stores on ARM64 and x86-64. Ordering is provided by the counters and
// fences in RingWriter/RingReader.
//
// The ring storage is a byte array (heap or mmap); 64-bit objects are implicitly
// created in it (C++20 implicit object creation), which makes the reinterpret_cast valid.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <mda/bus/ring.hpp>

namespace mda::bus::detail {

struct RecordHeader {
    std::uint32_t length;
    MsgType type;
};

[[nodiscard]] constexpr std::uint64_t pack(RecordHeader header) noexcept {
    return std::uint64_t{header.length} | (std::uint64_t{header.type} << 32);
}

[[nodiscard]] constexpr RecordHeader unpack(std::uint64_t word) noexcept {
    return {static_cast<std::uint32_t>(word), static_cast<MsgType>(word >> 32)};
}

[[nodiscard]] inline std::atomic_ref<std::uint64_t> word_at(std::byte* ring,
                                                            std::size_t offset) noexcept {
    return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(ring + offset));
}

inline void store_header(std::byte* ring, std::size_t offset, RecordHeader header) noexcept {
    word_at(ring, offset).store(pack(header), std::memory_order_relaxed);
}

[[nodiscard]] inline RecordHeader load_header(std::byte* ring, std::size_t offset) noexcept {
    return unpack(word_at(ring, offset).load(std::memory_order_relaxed));
}

/// Writes `payload` at `offset` as 64-bit words; the last word is zero-padded.
inline void store_payload(std::byte* ring, std::size_t offset,
                          std::span<const std::byte> payload) noexcept {
    std::size_t i = 0;
    for (; i + 8 <= payload.size(); i += 8) {
        std::uint64_t word;
        std::memcpy(&word, payload.data() + i, 8);
        word_at(ring, offset + i).store(word, std::memory_order_relaxed);
    }
    if (i < payload.size()) {
        std::uint64_t word = 0;
        std::memcpy(&word, payload.data() + i, payload.size() - i);
        word_at(ring, offset + i).store(word, std::memory_order_relaxed);
    }
}

/// Copies `length` bytes (rounded up to whole words) from `offset` into `out`.
inline void load_payload(std::byte* ring, std::size_t offset, std::size_t length,
                         std::uint64_t* out) noexcept {
    const std::size_t words = (length + 7) / 8;
    for (std::size_t k = 0; k < words; ++k) {
        out[k] = word_at(ring, offset + k * 8).load(std::memory_order_relaxed);
    }
}

}  // namespace mda::bus::detail
