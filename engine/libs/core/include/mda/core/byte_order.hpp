#pragma once

// Reading big-endian (network byte order) integers from raw byte buffers.
//
// Broker and exchange feeds (including Kite's binary ticks) encode integers in
// big-endian order, while x86-64 and ARM64 are little-endian. These helpers
// decode them without undefined behaviour:
//
//   * no reinterpret_cast of the buffer to an integer pointer (strict aliasing
//     and alignment UB) — bytes are copied into a std::array and std::bit_cast;
//   * std::byteswap only on little-endian hosts, resolved at compile time;
//   * constexpr, so decoding can be verified with static_assert.
//
// Optimizing compilers reduce load_be<std::uint32_t> to a single load + bswap
// (x86 `movbe`/`bswap`, ARM `rev`).

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <span>

namespace mda::core {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian platforms are not supported");

/// Integer types that can appear on the wire. `bool` is excluded on purpose:
/// its object representation is implementation-defined.
template <typename T>
concept WireInteger = std::integral<T> && !std::same_as<T, bool>;

/// Decodes a big-endian integer from exactly sizeof(T) bytes.
/// The static extent makes an out-of-bounds read a compile error.
template <WireInteger T>
[[nodiscard]] constexpr T load_be(std::span<const std::byte, sizeof(T)> bytes) noexcept {
    std::array<std::byte, sizeof(T)> raw{};
    std::ranges::copy(bytes, raw.begin());
    const auto value = std::bit_cast<T>(raw);
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(value);
    } else {
        return value;
    }
}

/// Decodes a big-endian integer starting at `offset` within `buffer`.
/// Precondition: offset + sizeof(T) <= buffer.size() (checked in debug builds).
/// Parsers validate the packet length once, then use this for each field.
template <WireInteger T>
[[nodiscard]] constexpr T load_be(std::span<const std::byte> buffer, std::size_t offset) noexcept {
    assert(offset <= buffer.size() && sizeof(T) <= buffer.size() - offset);
    return load_be<T>(buffer.subspan(offset).template first<sizeof(T)>());
}

}  // namespace mda::core
