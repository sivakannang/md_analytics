#pragma once

// The engine's notion of "now" (ADR-0026): the newest tick's receive time, advanced by the
// wall time since it arrived. Live, that is the wall clock (give or take feed latency). In a
// replay it follows the replayed session, so bars close by session time: with the wall clock
// a replay of 09:16 at 17:40 closed every bar mid-minute and lost its first ticks.
// Before any tick, it is the wall clock.

#include <chrono>
#include <cstdint>

namespace mda::bars {

class EventClock {
public:
    using Steady = std::chrono::steady_clock;

    /// A tick received at `event_ns` (Unix ns), seen at `seen` (steady clock).
    void on_event(std::int64_t event_ns, Steady::time_point seen = Steady::now()) noexcept {
        if (event_ns > last_event_ns_) {
            last_event_ns_ = event_ns;
            seen_ = seen;
        }
    }

    /// Unix seconds; `wall_ns` is used until the first event.
    [[nodiscard]] std::int64_t now_seconds(std::int64_t wall_ns, Steady::time_point at = Steady::now()) const noexcept {
        if (last_event_ns_ == 0) {
            return wall_ns / 1'000'000'000;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(at - seen_).count();
        return (last_event_ns_ + (elapsed > 0 ? elapsed : 0)) / 1'000'000'000;
    }

private:
    std::int64_t last_event_ns_ = 0;
    Steady::time_point seen_{};
};

}  // namespace mda::bars
