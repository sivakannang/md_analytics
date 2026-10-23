#pragma once

// How a reader waits when poll() finds nothing new (docs/design/market-data-bus.md §9).
//
// A compile-time policy (no virtual call on the hot path). Typical loop:
//
//   Backoff wait;
//   while (running) {
//       if (reader.poll(handler).status == PollStatus::Empty) wait.idle();
//       else wait.reset();
//   }

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <thread>

namespace mda::bus {

/// Tells the CPU we are spinning: saves power and frees resources for a sibling
/// hyper-thread, and avoids a memory-order pipeline flush when the spin ends.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

template <typename W>
concept WaitStrategy = requires(W wait) {
    { wait.idle() } noexcept;
    { wait.reset() } noexcept;
};

/// Lowest latency; burns 100% of a core. For a dedicated, pinned core.
struct BusySpin {
    void idle() noexcept { cpu_relax(); }
    void reset() noexcept {}
};

/// Spin, then yield, then sleep with exponentially growing intervals. Reacts within
/// nanoseconds when busy and costs almost no CPU when idle. The default.
class Backoff {
public:
    struct Config {
        std::uint32_t spins = 1000;
        std::uint32_t yields = 100;
        std::chrono::microseconds min_sleep{1};
        std::chrono::microseconds max_sleep{1000};
    };

    Backoff() noexcept : Backoff(Config{}) {}
    explicit Backoff(Config config) noexcept : config_(config), sleep_(config.min_sleep) {}

    void idle() noexcept {
        if (count_ < config_.spins) {
            ++count_;
            cpu_relax();
        } else if (count_ < config_.spins + config_.yields) {
            ++count_;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(sleep_);
            sleep_ = std::min(sleep_ * 2, config_.max_sleep);
        }
    }

    void reset() noexcept {
        count_ = 0;
        sleep_ = config_.min_sleep;
    }

    /// Current phase, for tests and diagnostics.
    [[nodiscard]] std::chrono::microseconds next_sleep() const noexcept { return sleep_; }

private:
    Config config_;
    std::uint32_t count_ = 0;
    std::chrono::microseconds sleep_;
};

/// Fixed sleep between polls. Simple and cheap; latency is roughly the interval.
class Sleep {
public:
    explicit Sleep(std::chrono::microseconds interval = std::chrono::microseconds(1000)) noexcept
        : interval_(interval) {}

    void idle() noexcept { std::this_thread::sleep_for(interval_); }
    void reset() noexcept {}

private:
    std::chrono::microseconds interval_;
};

static_assert(WaitStrategy<BusySpin>);
static_assert(WaitStrategy<Backoff>);
static_assert(WaitStrategy<Sleep>);

}  // namespace mda::bus
