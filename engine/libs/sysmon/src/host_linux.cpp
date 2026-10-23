// Linux: everything from /proc and /sys (no extra libraries).

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <unistd.h>

#include <mda/sysmon/sysmon.hpp>

namespace mda::sysmon {
namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/// A "Key:   1234 kB" line of /proc/meminfo, in bytes.
std::optional<std::uint64_t> meminfo(const std::string& text, const std::string& key) {
    const auto at = text.find(key + ":");
    if (at == std::string::npos) {
        return std::nullopt;
    }
    std::istringstream line(text.substr(at + key.size() + 1));
    std::uint64_t kb = 0;
    if (!(line >> kb)) {
        return std::nullopt;
    }
    return kb * 1024;
}

std::optional<std::int64_t> boot_time() {
    std::istringstream stat(read_text("/proc/stat"));
    std::string key;
    while (stat >> key) {
        if (key == "btime") {
            std::int64_t value = 0;
            if (stat >> value) {
                return value;
            }
        }
    }
    return std::nullopt;
}

void read_power(MachineStats& out) {
    std::error_code ec;
    bool on_battery = false;
    bool on_mains = false;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
        std::istringstream type_text(read_text(entry.path() / "type"));
        std::string type;
        type_text >> type;
        if (type == "Battery") {
            int capacity = -1;
            std::istringstream(read_text(entry.path() / "capacity")) >> capacity;
            if (capacity >= 0) {
                out.battery_percent = capacity;
            }
            std::string status;
            std::istringstream(read_text(entry.path() / "status")) >> status;
            on_battery = on_battery || status == "Discharging";
        } else if (type == "Mains") {
            int online = 0;
            std::istringstream(read_text(entry.path() / "online")) >> online;
            on_mains = on_mains || online == 1;
        }
    }
    if (on_battery) {
        out.power = "battery";
    } else if (on_mains || out.battery_percent) {
        out.power = "ac";
    }
}

}  // namespace

MachineStats machine_stats() {
    MachineStats out;
    double loads[1] = {};
    if (getloadavg(loads, 1) == 1) {
        out.load1 = loads[0];
    }
    if (const long cores = sysconf(_SC_NPROCESSORS_ONLN); cores > 0) {
        out.cores = static_cast<unsigned>(cores);
    }
    const std::string memory = read_text("/proc/meminfo");
    out.memory_total = meminfo(memory, "MemTotal");
    if (const auto available = meminfo(memory, "MemAvailable"); available && out.memory_total) {
        out.memory_used = *out.memory_total > *available ? *out.memory_total - *available : 0;
    }
    double uptime = 0;
    if (std::istringstream(read_text("/proc/uptime")) >> uptime) {
        out.uptime_s = static_cast<std::int64_t>(uptime);
    }
    read_power(out);
    return out;
}

std::optional<ProcessStats> process_stats(std::uint32_t pid) {
    if (pid == 0) {
        return std::nullopt;
    }
    const std::filesystem::path dir = "/proc/" + std::to_string(pid);
    const std::string stat = read_text(dir / "stat");
    // "pid (comm) state ppid …": the command may hold spaces and ')', so parse after the last ')'.
    const auto close = stat.rfind(')');
    if (close == std::string::npos) {
        return std::nullopt;
    }
    std::istringstream fields(stat.substr(close + 1));
    std::vector<std::string> values;
    for (std::string value; fields >> value;) {
        values.push_back(value);
    }
    ProcessStats out{.pid = pid};
    // After ')': state is field 3, utime 14, stime 15, starttime 22 (proc(5)).
    const long ticks = sysconf(_SC_CLK_TCK);
    if (values.size() > 19 && ticks > 0) {
        const auto per_tick = static_cast<std::uint64_t>(1'000'000'000 / ticks);
        const std::uint64_t cpu = std::strtoull(values[11].c_str(), nullptr, 10) +
                                  std::strtoull(values[12].c_str(), nullptr, 10);
        out.cpu_time_ns = cpu * per_tick;
        if (const auto boot = boot_time()) {
            out.started =
                *boot + static_cast<std::int64_t>(std::strtoull(values[19].c_str(), nullptr, 10) /
                                                  static_cast<std::uint64_t>(ticks));
        }
    }
    std::uint64_t size = 0;
    std::uint64_t resident = 0;
    if (std::istringstream(read_text(dir / "statm")) >> size >> resident) {
        out.rss_bytes = resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }
    return out;
}

std::vector<std::uint32_t> descendant_pids(std::uint32_t pid) {
    // One pass over /proc for every process's parent, then a walk down from `pid`.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> parent_of;  // (pid, ppid)
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        const std::string stat = read_text(entry.path() / "stat");
        const auto close = stat.rfind(')');
        if (close == std::string::npos) {
            continue;
        }
        std::istringstream fields(stat.substr(close + 1));
        std::string state;
        std::uint32_t ppid = 0;
        if (fields >> state >> ppid) {
            parent_of.emplace_back(
                static_cast<std::uint32_t>(std::strtoul(name.c_str(), nullptr, 10)), ppid);
        }
    }
    std::vector<std::uint32_t> out;
    std::vector<std::uint32_t> parents{pid};
    for (int depth = 0; depth < 6 && !parents.empty(); ++depth) {
        std::vector<std::uint32_t> next;
        for (const auto& [child, parent] : parent_of) {
            if (std::ranges::find(parents, parent) != parents.end()) {
                next.push_back(child);
            }
        }
        out.insert(out.end(), next.begin(), next.end());
        parents = std::move(next);
    }
    return out;
}

std::string command_line(std::uint32_t pid) {
    if (pid == 0) {
        return {};
    }
    std::string text = read_text("/proc/" + std::to_string(pid) + "/cmdline");
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    for (char& c : text) {
        if (c == '\0') {
            c = ' ';
        }
    }
    return text;
}

}  // namespace mda::sysmon
