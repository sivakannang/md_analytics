#include <atomic>
#include <bit>
#include <cstdint>

#include <mda/bus/ring.hpp>

namespace mda::bus {

std::expected<void, RingError> validate_ring(std::span<const std::byte> data) noexcept {
    if (!std::has_single_bit(data.size())) {
        return std::unexpected(RingError::CapacityNotPowerOfTwo);
    }
    if (data.size() < kMinCapacity) {
        return std::unexpected(RingError::CapacityTooSmall);
    }
    const auto address = reinterpret_cast<std::uintptr_t>(data.data());
    if (address % std::atomic_ref<std::uint64_t>::required_alignment != 0) {
        return std::unexpected(RingError::Misaligned);
    }
    return {};
}

}  // namespace mda::bus
