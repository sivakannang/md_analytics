#pragma once

// Machine, process and disk statistics for the admin system monitor
// (docs/design/system-monitor.md §3, SM1). One interface, with a macOS and a Linux
// implementation; a value a platform can't give is empty and the page shows "—".
// Everything here only reads: it never signals or changes another process.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mda::sysmon {

struct MachineStats {
    std::optional<double> load1;               ///< 1-minute load average
    unsigned cores = 0;                        ///< online logical CPUs
    std::optional<std::uint64_t> memory_used;  ///< bytes in use (apps + wired + compressed)
    std::optional<std::uint64_t> memory_total;
    std::optional<std::string> power;          ///< "ac" or "battery"; empty if unknown
    std::optional<int> battery_percent;        ///< empty without a battery
    std::optional<std::int64_t> uptime_s;
};

struct ProcessStats {
    std::uint32_t pid = 0;
    std::optional<std::int64_t> started;        ///< Unix seconds
    std::optional<std::uint64_t> cpu_time_ns;   ///< user + system, since it started
    std::optional<std::uint64_t> rss_bytes;     ///< resident memory
};

struct DiskSpace {
    std::uint64_t free = 0;   ///< available to an unprivileged user
    std::uint64_t total = 0;
};

[[nodiscard]] MachineStats machine_stats();

/// Stats of a running process; empty when there is no such process.
[[nodiscard]] std::optional<ProcessStats> process_stats(std::uint32_t pid);

/// The process's children, their children and so on (a launcher like `npm run dev` does its
/// work in a descendant). Unordered; empty when there are none.
[[nodiscard]] std::vector<std::uint32_t> descendant_pids(std::uint32_t pid);

/// The process's full command line, arguments separated by spaces; empty when it can't be
/// read (gone, or another user's). Used to check a pid file still names the right program.
[[nodiscard]] std::string command_line(std::uint32_t pid);

[[nodiscard]] std::expected<DiskSpace, std::string> disk_space(const std::filesystem::path& path);

/// A file's size; 0 when it doesn't exist.
[[nodiscard]] std::uint64_t size_of_file(const std::filesystem::path& path);

/// The total size of the regular files under a folder (recursive); 0 when it doesn't exist.
[[nodiscard]] std::uint64_t size_of_folder(const std::filesystem::path& path);

}  // namespace mda::sysmon
