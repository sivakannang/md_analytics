#pragma once

// RAII owner of a POSIX shared-memory mapping (shm_open + mmap).
//
// The mapping lives until the object is destroyed; the shared-memory object itself
// (the name) lives until explicitly unlinked, so it survives process crashes.

#include <cstddef>
#include <expected>
#include <span>
#include <string>

#include <mda/bus/error.hpp>

namespace mda::bus {

class SharedMemory {
public:
    /// Creates a new object of `size` bytes (fails with AlreadyExists if the name is
    /// taken), maps it read-write and pre-faults every page. Permissions: owner only.
    [[nodiscard]] static std::expected<SharedMemory, BusError> create(const std::string& name,
                                                                      std::size_t size);

    enum class Access {
        ReadWrite,
        ReadOnly,  ///< monitoring tools: any write would fault
    };

    /// Maps an existing object; the size is taken from the object.
    [[nodiscard]] static std::expected<SharedMemory, BusError> open(
        const std::string& name, Access access = Access::ReadWrite);

    /// Removes the name. Existing mappings stay valid until unmapped.
    [[nodiscard]] static std::expected<void, BusError> unlink(const std::string& name);

    SharedMemory(SharedMemory&& other) noexcept;
    SharedMemory& operator=(SharedMemory&& other) noexcept;
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    ~SharedMemory();

    [[nodiscard]] std::span<std::byte> bytes() const noexcept { return {data_, size_}; }

    enum class Prefault {
        Read,   ///< safe on memory other processes are using
        Write,  ///< also allocates backing pages; only on memory nobody else uses yet
    };

    /// Touches every page so later accesses don't take page faults on the hot path.
    void prefault(Prefault mode) noexcept;

private:
    SharedMemory(std::byte* data, std::size_t size) noexcept : data_(data), size_(size) {}

    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace mda::bus
