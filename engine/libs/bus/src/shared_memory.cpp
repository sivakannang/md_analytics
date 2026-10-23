#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mda/bus/shared_memory.hpp>

namespace mda::bus {
namespace {

BusError system_error(int err) {
    return {err == ENOENT   ? BusErrc::NotFound
            : err == EEXIST ? BusErrc::AlreadyExists
                            : BusErrc::SystemError,
            err};
}

/// Closes a file descriptor on scope exit; the mapping outlives the descriptor.
struct FdGuard {
    int fd;
    ~FdGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

std::expected<std::byte*, BusError> map(int fd, std::size_t size, SharedMemory::Access access) {
    const bool writable = access == SharedMemory::Access::ReadWrite;
    int flags = MAP_SHARED;
#ifdef __linux__
    if (writable) {
        flags |= MAP_POPULATE;  // pre-fault data-path mappings; tools only touch the header
    }
#endif
    const int protection = writable ? PROT_READ | PROT_WRITE : PROT_READ;
    void* address = ::mmap(nullptr, size, protection, flags, fd, 0);
    if (address == MAP_FAILED) {
        return std::unexpected(system_error(errno));
    }
    return static_cast<std::byte*>(address);
}

}  // namespace

std::expected<SharedMemory, BusError> SharedMemory::create(const std::string& name,
                                                           std::size_t size) {
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return std::unexpected(system_error(errno));
    }
    FdGuard guard{fd};

    // Any failure after shm_open must remove the half-created object.
    auto fail = [&](int err) {
        ::shm_unlink(name.c_str());
        return std::unexpected(system_error(err));
    };
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
        return fail(errno);
    }
    auto data = map(fd, size, Access::ReadWrite);
    if (!data) {
        return fail(data.error().system_errno);
    }
    SharedMemory memory(*data, size);
    memory.prefault(Prefault::Write);  // brand new: no other process can be using it
    return memory;
}

std::expected<SharedMemory, BusError> SharedMemory::open(const std::string& name, Access access) {
    const int fd = ::shm_open(name.c_str(), access == Access::ReadWrite ? O_RDWR : O_RDONLY, 0);
    if (fd < 0) {
        return std::unexpected(system_error(errno));
    }
    FdGuard guard{fd};

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        return std::unexpected(system_error(errno));
    }
    // macOS may report the size rounded up to a page; callers validate the layout
    // against the header, so a larger mapping is fine.
    const auto size = static_cast<std::size_t>(info.st_size);
    if (size == 0) {
        return std::unexpected(BusError{BusErrc::NotReady});  // created but not sized yet
    }
    auto data = map(fd, size, access);
    if (!data) {
        return std::unexpected(data.error());
    }
    return SharedMemory(*data, size);
}

std::expected<void, BusError> SharedMemory::unlink(const std::string& name) {
    if (::shm_unlink(name.c_str()) != 0) {
        return std::unexpected(system_error(errno));
    }
    return {};
}

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        if (data_ != nullptr) {
            ::munmap(data_, size_);
        }
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

SharedMemory::~SharedMemory() {
    if (data_ != nullptr) {
        ::munmap(data_, size_);
    }
}

void SharedMemory::prefault(Prefault mode) noexcept {
    // A read fault on a never-written shared page may map the kernel's zero page, so
    // the first write would fault again; a write touch allocates the real page.
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    for (std::size_t offset = 0; offset < size_; offset += page) {
        // Atomic access: other processes may be using this memory concurrently.
        std::atomic_ref<std::uint64_t> word(*reinterpret_cast<std::uint64_t*>(data_ + offset));
        if (mode == Prefault::Write) {
            word.store(0, std::memory_order_relaxed);
        } else {
            (void)word.load(std::memory_order_relaxed);
        }
    }
}

}  // namespace mda::bus
