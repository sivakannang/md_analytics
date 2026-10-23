#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include <mda/core/byte_order.hpp>

namespace mda::core {
namespace {

template <typename... Bytes>
constexpr std::array<std::byte, sizeof...(Bytes)> bytes(Bytes... values) {
    return {static_cast<std::byte>(values)...};
}

// Decodes exactly sizeof(T) bytes; passing the wrong number of bytes fails to compile.
template <WireInteger T, typename... Bytes>
constexpr T decode(Bytes... values) {
    const auto buffer = bytes(values...);
    return load_be<T>(std::span<const std::byte, sizeof...(Bytes)>{buffer});
}

// Decoding is constexpr, so correctness is also checked at compile time.
static_assert(decode<std::uint16_t>(0x12, 0x34) == 0x1234);
static_assert(decode<std::uint32_t>(0x01, 0x02, 0x03, 0x04) == 0x01020304);

TEST(ByteOrderTest, DecodesUnsignedIntegersOfEachWidth) {
    EXPECT_EQ(decode<std::uint8_t>(0xAB), 0xABu);
    EXPECT_EQ(decode<std::uint16_t>(0xAB, 0xCD), 0xABCDu);
    EXPECT_EQ(decode<std::uint32_t>(0xDE, 0xAD, 0xBE, 0xEF), 0xDEADBEEFu);
    EXPECT_EQ(decode<std::uint64_t>(0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF),
              0x0123456789ABCDEFull);
}

TEST(ByteOrderTest, DecodesNegativeSignedIntegers) {
    // Kite sends net change and similar fields as signed 32-bit values.
    EXPECT_EQ(decode<std::int32_t>(0xFF, 0xFF, 0xFF, 0xFE), -2);
    EXPECT_EQ(decode<std::int16_t>(0x80, 0x00), INT16_MIN);
}

TEST(ByteOrderTest, ReadsAtUnalignedOffsets) {
    // A field at offset 1 is misaligned for a 4-byte integer; decoding must still work.
    const auto buffer = bytes(0x00, 0x00, 0x01, 0x86, 0xA0, 0xFF);
    EXPECT_EQ(load_be<std::uint32_t>(std::span<const std::byte>{buffer}, 1), 100'000u);
}

TEST(ByteOrderTest, ReadsFieldEndingExactlyAtBufferEnd) {
    const auto buffer = bytes(0xAA, 0x00, 0x2A);
    EXPECT_EQ(load_be<std::uint16_t>(std::span<const std::byte>{buffer}, 1), 42u);
}

}  // namespace
}  // namespace mda::core
