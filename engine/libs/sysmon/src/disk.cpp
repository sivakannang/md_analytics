// Portable parts: disk space (statvfs) and file / folder sizes.

#include <cerrno>
#include <cstring>

#include <sys/statvfs.h>

#include <mda/sysmon/sysmon.hpp>

namespace mda::sysmon {

std::expected<DiskSpace, std::string> disk_space(const std::filesystem::path& path) {
    struct statvfs fs {};
    if (::statvfs(path.c_str(), &fs) != 0) {
        return std::unexpected("statvfs " + path.string() + ": " + std::strerror(errno));
    }
    const auto block = static_cast<std::uint64_t>(fs.f_frsize);
    return DiskSpace{.free = static_cast<std::uint64_t>(fs.f_bavail) * block,
                     .total = static_cast<std::uint64_t>(fs.f_blocks) * block};
}

std::uint64_t size_of_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

std::uint64_t size_of_folder(const std::filesystem::path& path) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec)) {
            const auto size = it->file_size(entry_ec);
            total += entry_ec ? 0 : static_cast<std::uint64_t>(size);
        }
    }
    return total;
}

}  // namespace mda::sysmon
