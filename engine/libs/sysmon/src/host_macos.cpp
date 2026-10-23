// macOS: load and uptime from sysctl, memory from the Mach VM statistics, power from IOKit,
// processes from libproc (CPU times are in Mach absolute-time units: converted to ns).

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <mda/sysmon/sysmon.hpp>

namespace mda::sysmon {
namespace {

mach_port_t host_port() {
    static const mach_port_t port = mach_host_self();  // one send right for the process
    return port;
}

std::uint64_t mach_to_ns(std::uint64_t ticks) {
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    if (base.denom == 0) {
        return ticks;
    }
    return ticks * base.numer / base.denom;
}

bool same(CFStringRef a, const char* b) {
    const CFStringRef other =
        CFStringCreateWithCString(kCFAllocatorDefault, b, kCFStringEncodingUTF8);
    const bool equal =
        a != nullptr && other != nullptr && CFStringCompare(a, other, 0) == kCFCompareEqualTo;
    if (other != nullptr) {
        CFRelease(other);
    }
    return equal;
}

void read_power(MachineStats& out) {
    const CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr) {
        return;
    }
    const CFStringRef providing = IOPSGetProvidingPowerSourceType(info);
    if (same(providing, kIOPMACPowerKey)) {
        out.power = "ac";
    } else if (same(providing, kIOPMBatteryPowerKey)) {
        out.power = "battery";
    }
    if (const CFArrayRef sources = IOPSCopyPowerSourcesList(info)) {
        for (CFIndex i = 0; i < CFArrayGetCount(sources); ++i) {
            const CFDictionaryRef source =
                IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(sources, i));
            if (source == nullptr) {
                continue;
            }
            const auto type =
                static_cast<CFStringRef>(CFDictionaryGetValue(source, CFSTR(kIOPSTypeKey)));
            if (!same(type, kIOPSInternalBatteryType)) {
                continue;
            }
            int current = 0;
            int max = 0;
            const auto current_ref = static_cast<CFNumberRef>(
                CFDictionaryGetValue(source, CFSTR(kIOPSCurrentCapacityKey)));
            const auto max_ref =
                static_cast<CFNumberRef>(CFDictionaryGetValue(source, CFSTR(kIOPSMaxCapacityKey)));
            if (current_ref != nullptr && max_ref != nullptr &&
                CFNumberGetValue(current_ref, kCFNumberIntType, &current) &&
                CFNumberGetValue(max_ref, kCFNumberIntType, &max) && max > 0) {
                out.battery_percent = current * 100 / max;
            }
        }
        CFRelease(sources);
    }
    CFRelease(info);
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

    std::uint64_t memsize = 0;
    std::size_t length = sizeof memsize;
    if (sysctlbyname("hw.memsize", &memsize, &length, nullptr, 0) == 0) {
        out.memory_total = memsize;
    }
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (host_statistics64(host_port(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                          &count) == KERN_SUCCESS &&
        host_page_size(host_port(), &page) == KERN_SUCCESS) {
        // As Activity Monitor's "Memory Used": app memory + wired + compressed.
        const std::uint64_t app = vm.internal_page_count > vm.purgeable_count
                                      ? std::uint64_t{vm.internal_page_count} - vm.purgeable_count
                                      : 0;
        out.memory_used =
            (app + vm.wire_count + vm.compressor_page_count) * static_cast<std::uint64_t>(page);
    }

    timeval boot{};
    length = sizeof boot;
    int boot_mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(boot_mib, 2, &boot, &length, nullptr, 0) == 0 && boot.tv_sec > 0) {
        out.uptime_s =
            static_cast<std::int64_t>(std::time(nullptr)) - static_cast<std::int64_t>(boot.tv_sec);
    }
    read_power(out);
    return out;
}

std::optional<ProcessStats> process_stats(std::uint32_t pid) {
    if (pid == 0) {
        return std::nullopt;
    }
    const int id = static_cast<int>(pid);
    proc_bsdinfo bsd{};
    if (proc_pidinfo(id, PROC_PIDTBSDINFO, 0, &bsd, sizeof bsd) != static_cast<int>(sizeof bsd)) {
        return std::nullopt;  // no such process (or not ours to read)
    }
    ProcessStats out{.pid = pid};
    out.started = static_cast<std::int64_t>(bsd.pbi_start_tvsec);
    proc_taskinfo task{};
    if (proc_pidinfo(id, PROC_PIDTASKINFO, 0, &task, sizeof task) ==
        static_cast<int>(sizeof task)) {
        out.cpu_time_ns = mach_to_ns(task.pti_total_user + task.pti_total_system);
        out.rss_bytes = task.pti_resident_size;
    }
    return out;
}

std::vector<std::uint32_t> descendant_pids(std::uint32_t pid) {
    std::vector<std::uint32_t> out;
    std::vector<std::uint32_t> parents{pid};
    // A few levels are plenty (npm → sh → node); the bound guards against a pid reuse loop.
    for (int depth = 0; depth < 6 && !parents.empty(); ++depth) {
        std::vector<std::uint32_t> next;
        for (const std::uint32_t parent : parents) {
            pid_t children[256] = {};
            const int bytes = proc_listpids(PROC_PPID_ONLY, parent, children, sizeof children);
            for (int i = 0; bytes > 0 && i < bytes / static_cast<int>(sizeof(pid_t)); ++i) {
                if (children[i] > 0) {
                    next.push_back(static_cast<std::uint32_t>(children[i]));
                }
            }
        }
        out.insert(out.end(), next.begin(), next.end());
        parents = std::move(next);
    }
    return out;
}

std::string command_line(std::uint32_t pid) {
    int argmax = 0;
    std::size_t length = sizeof argmax;
    int argmax_mib[2] = {CTL_KERN, KERN_ARGMAX};
    if (pid == 0 || sysctl(argmax_mib, 2, &argmax, &length, nullptr, 0) != 0 || argmax <= 0) {
        return {};
    }
    // KERN_PROCARGS2: int argc, the executable path, NUL padding, then argc strings.
    std::vector<char> buffer(static_cast<std::size_t>(argmax));
    length = buffer.size();
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    if (sysctl(mib, 3, buffer.data(), &length, nullptr, 0) != 0 || length < sizeof(int)) {
        return {};
    }
    int argc = 0;
    std::memcpy(&argc, buffer.data(), sizeof argc);
    std::size_t at = sizeof argc;
    while (at < length && buffer[at] != '\0') {
        ++at;  // the executable path
    }
    while (at < length && buffer[at] == '\0') {
        ++at;  // padding
    }
    std::string out;
    for (int i = 0; i < argc && at < length; ++i) {
        const char* arg = buffer.data() + at;
        const std::size_t size = strnlen(arg, length - at);
        if (!out.empty()) {
            out += ' ';
        }
        out.append(arg, size);
        at += size + 1;
    }
    return out;
}

}  // namespace mda::sysmon
