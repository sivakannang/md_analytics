// mda-busctl — inspect and manage market data buses (docs/design/market-data-bus.md §8a).
//
//   mda-busctl stat <bus>... [--watch <seconds>]   writer and reader status, lag, losses
//   mda-busctl list                                 buses present (Linux only)
//   mda-busctl rm <bus>...                          remove bus segments
//
// Maps buses read-only: it can never disturb the processes using them.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include <mda/bus/bus.hpp>

namespace {

using mda::bus::BusSnapshot;

std::string format_bytes(std::uint64_t bytes) {
    constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", value, kUnits[unit]);
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::string format_age(std::uint64_t timestamp_ns) {
    const std::uint64_t now = now_ns();
    if (timestamp_ns == 0 || timestamp_ns > now) {
        return "-";
    }
    return std::format("{:.1f} s ago", static_cast<double>(now - timestamp_ns) / 1e9);
}

/// Previous sample of a bus, to turn counters into rates in --watch mode.
struct Sample {
    std::uint64_t taken_ns = 0;
    std::uint64_t writer_messages = 0;
};

void print_snapshot(const BusSnapshot& bus, const Sample* previous, std::uint64_t taken_ns) {
    std::string rate = "-";
    if (previous != nullptr && taken_ns > previous->taken_ns) {
        const double seconds = static_cast<double>(taken_ns - previous->taken_ns) / 1e9;
        rate = std::format(
            "{:.0f} msg/s",
            static_cast<double>(bus.writer_stats.messages - previous->writer_messages) / seconds);
    }
    const std::string writer = bus.writer.pid == 0 ? "none"
                                                   : std::format("pid {}{}", bus.writer.pid,
                                                                 bus.writer_alive ? "" : " (DEAD)");

    std::printf("bus %s   capacity %s   epoch %llu   writer %s   heartbeat %s   %s\n",
                bus.name.c_str(), format_bytes(bus.capacity).c_str(),
                static_cast<unsigned long long>(bus.writer.epoch), writer.c_str(),
                format_age(bus.writer.heartbeat_ns).c_str(), rate.c_str());
    std::printf("  published %llu msgs, %s, rejected %llu\n",
                static_cast<unsigned long long>(bus.writer_stats.messages),
                format_bytes(bus.writer_stats.bytes).c_str(),
                static_cast<unsigned long long>(bus.writer_stats.rejected));

    if (bus.readers.empty()) {
        std::printf("  no registered readers\n");
        return;
    }
    std::printf("  %-24s %-14s %-11s %-8s %-12s %-6s %-10s %s\n", "reader", "pid", "lag", "lag %",
                "messages", "laps", "lost", "heartbeat");
    for (const auto& reader : bus.readers) {
        const std::string pid =
            std::format("{}{}", reader.pid, reader.process_alive ? "" : " (DEAD)");
        // A dead reader's position is frozen, so its lag is meaningless; lag beyond the
        // ring size means the writer has already overwritten what it hasn't read.
        std::string lag = "-";
        std::string lag_percent = "-";
        if (reader.process_alive && reader.lag_bytes >= bus.capacity) {
            lag = "LAPPED";
        } else if (reader.process_alive) {
            lag = format_bytes(reader.lag_bytes);
            lag_percent = std::format("{:.2f}%", 100.0 * static_cast<double>(reader.lag_bytes) /
                                                     static_cast<double>(bus.capacity));
        }
        std::printf("  %-24s %-14s %-11s %-8s %-12llu %-6llu %-10s %s\n", reader.name.c_str(),
                    pid.c_str(), lag.c_str(), lag_percent.c_str(),
                    static_cast<unsigned long long>(reader.stats.messages),
                    static_cast<unsigned long long>(reader.stats.laps),
                    format_bytes(reader.stats.lost_bytes).c_str(),
                    format_age(reader.heartbeat_ns).c_str());
    }
}

int cmd_stat(const std::vector<std::string>& buses, double watch_seconds) {
    std::map<std::string, Sample> samples;
    const bool clear_screen = watch_seconds > 0 && ::isatty(STDOUT_FILENO);
    while (true) {
        if (clear_screen) {
            std::printf("\033[2J\033[H");
        }
        int failures = 0;
        for (const auto& name : buses) {
            const auto snapshot = mda::bus::inspect_bus(name);
            const std::uint64_t taken = now_ns();
            if (!snapshot) {
                std::fprintf(stderr, "bus %s: %s\n", name.c_str(),
                             mda::bus::to_string(snapshot.error()).c_str());
                ++failures;
                continue;
            }
            const auto found = samples.find(name);
            print_snapshot(*snapshot, found == samples.end() ? nullptr : &found->second, taken);
            samples[name] = {.taken_ns = taken, .writer_messages = snapshot->writer_stats.messages};
            std::printf("\n");
        }
        std::fflush(stdout);
        if (watch_seconds <= 0) {
            return failures == 0 ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(watch_seconds));
    }
}

int cmd_list() {
    const auto buses = mda::bus::list_buses();
#ifndef __linux__
    std::fprintf(stderr,
                 "list is not supported on this OS (POSIX shared memory can't be "
                 "enumerated); use 'stat <bus>'\n");
    return 1;
#endif
    for (const auto& name : buses) {
        std::printf("%s\n", name.c_str());
    }
    return 0;
}

int cmd_remove(const std::vector<std::string>& buses) {
    int failures = 0;
    for (const auto& name : buses) {
        if (auto removed = mda::bus::remove_bus(name); !removed) {
            std::fprintf(stderr, "bus %s: %s\n", name.c_str(),
                         mda::bus::to_string(removed.error()).c_str());
            ++failures;
        } else {
            std::printf("removed %s\n", name.c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}

int usage() {
    std::fprintf(stderr,
                 "usage: mda-busctl stat <bus>... [--watch <seconds>]\n"
                 "       mda-busctl list\n"
                 "       mda-busctl rm <bus>...\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string_view command = argv[1];
    std::vector<std::string> buses;
    double watch_seconds = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--watch" && i + 1 < argc) {
            watch_seconds = std::stod(argv[++i]);
        } else {
            buses.emplace_back(arg);
        }
    }

    if (command == "stat" && !buses.empty()) {
        return cmd_stat(buses, watch_seconds);
    }
    if (command == "list" && buses.empty()) {
        return cmd_list();
    }
    if (command == "rm" && !buses.empty()) {
        return cmd_remove(buses);
    }
    return usage();
}
