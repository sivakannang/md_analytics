// The stats depend on the machine, so these check shapes and bounds, measured on this test
// process itself where an exact answer is known.

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>

#include <algorithm>
#include <csignal>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <mda/sysmon/sysmon.hpp>

namespace mda::sysmon {
namespace {

TEST(SysmonTest, ReadsTheMachine) {
    const MachineStats m = machine_stats();
    EXPECT_GT(m.cores, 0u);
    ASSERT_TRUE(m.load1.has_value());
    EXPECT_GE(*m.load1, 0.0);
    ASSERT_TRUE(m.memory_total.has_value());
    ASSERT_TRUE(m.memory_used.has_value());
    EXPECT_GT(*m.memory_total, 64ull << 20);
    EXPECT_GT(*m.memory_used, 0u);
    EXPECT_LE(*m.memory_used, *m.memory_total);
    ASSERT_TRUE(m.uptime_s.has_value());
    EXPECT_GT(*m.uptime_s, 0);
    if (m.power) {
        EXPECT_TRUE(*m.power == "ac" || *m.power == "battery");
    }
    if (m.battery_percent) {
        EXPECT_GE(*m.battery_percent, 0);
        EXPECT_LE(*m.battery_percent, 100);
    }
}

TEST(SysmonTest, ReadsThisProcess) {
    // Burn a little CPU so the time is measurably above zero.
    volatile std::uint64_t sink = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (std::chrono::steady_clock::now() < until) {
        sink = sink + 1;
    }
    const auto self = process_stats(static_cast<std::uint32_t>(getpid()));
    ASSERT_TRUE(self.has_value());
    EXPECT_EQ(self->pid, static_cast<std::uint32_t>(getpid()));
    ASSERT_TRUE(self->cpu_time_ns.has_value());
    EXPECT_GT(*self->cpu_time_ns, 10'000'000u);  // at least 10 ms of the 30 ms loop
    ASSERT_TRUE(self->rss_bytes.has_value());
    EXPECT_GT(*self->rss_bytes, 1u << 20);
    ASSERT_TRUE(self->started.has_value());
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    EXPECT_LE(*self->started, now);
    EXPECT_GT(*self->started, now - 3600);  // a test process started within the hour
}

TEST(SysmonTest, ReadsThisProcessesCommandLine) {
    const std::string line = command_line(static_cast<std::uint32_t>(getpid()));
    EXPECT_NE(line.find("mda_sysmon_tests"), std::string::npos) << line;
}

TEST(SysmonTest, FindsDescendantProcesses) {
    // A child that waits on its own child: both are descendants of this process.
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        execl("/bin/sh", "sh", "-c", "sleep 5 & wait", static_cast<char*>(nullptr));
        _exit(127);
    }
    std::vector<std::uint32_t> found;
    for (int i = 0; i < 50 && found.size() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        found = descendant_pids(static_cast<std::uint32_t>(getpid()));
    }
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    EXPECT_NE(std::ranges::find(found, static_cast<std::uint32_t>(child)), found.end());
    EXPECT_GE(found.size(), 2u);  // sh and its sleep
    EXPECT_TRUE(descendant_pids(4'194'305).empty());
}

TEST(SysmonTest, ReportsNoSuchProcess) {
    // pid_max is 99998 on macOS and at most 4194304 on Linux.
    EXPECT_FALSE(process_stats(4'194'305).has_value());
    EXPECT_FALSE(process_stats(0).has_value());
    EXPECT_TRUE(command_line(4'194'305).empty());
}

TEST(SysmonTest, MeasuresDiskAndFolders) {
    const auto disk = disk_space(std::filesystem::temp_directory_path());
    ASSERT_TRUE(disk.has_value()) << disk.error();
    EXPECT_GT(disk->total, 0u);
    EXPECT_LE(disk->free, disk->total);
    EXPECT_FALSE(disk_space("/no/such/folder/anywhere").has_value());

    const auto dir = std::filesystem::temp_directory_path() / ("mda_sysmon_test_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir / "day");
    std::ofstream(dir / "a.bin", std::ios::binary) << std::string(1000, 'x');
    std::ofstream(dir / "day" / "b.bin", std::ios::binary) << std::string(234, 'y');
    EXPECT_EQ(size_of_file(dir / "a.bin"), 1000u);
    EXPECT_EQ(size_of_file(dir / "missing.bin"), 0u);
    EXPECT_EQ(size_of_folder(dir), 1234u);
    EXPECT_EQ(size_of_folder(dir / "missing"), 0u);
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace mda::sysmon
