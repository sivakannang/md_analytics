#include <chrono>

#include <gtest/gtest.h>

#include <mda/bars/event_clock.hpp>

namespace mda::bars {
namespace {

using namespace std::chrono_literals;
constexpr std::int64_t kNs = 1'000'000'000;

TEST(EventClockTest, IsTheWallClockUntilATickArrives) {
    const EventClock clock;
    EXPECT_EQ(clock.now_seconds(1'792'765'200 * kNs), 1'792'765'200);
}

TEST(EventClockTest, FollowsTheTicksNotTheWallClock) {
    // A replay of 09:16 running at 17:40: "now" is 09:16 plus the wall time since the tick.
    EventClock clock;
    const auto t0 = EventClock::Steady::now();
    constexpr std::int64_t k0916 = 1'792'727'160;
    clock.on_event(k0916 * kNs, t0);
    const std::int64_t wall_1740 = 1'792'756'200 * kNs;
    EXPECT_EQ(clock.now_seconds(wall_1740, t0), k0916);
    EXPECT_EQ(clock.now_seconds(wall_1740, t0 + 2500ms), k0916 + 2);  // quiet: time still moves
    // An older tick (out of order) doesn't move it back.
    clock.on_event((k0916 - 30) * kNs, t0 + 3s);
    EXPECT_EQ(clock.now_seconds(wall_1740, t0 + 3s), k0916 + 3);
    // A newer one resets the base.
    clock.on_event((k0916 + 120) * kNs, t0 + 4s);
    EXPECT_EQ(clock.now_seconds(wall_1740, t0 + 4s), k0916 + 120);
}

}  // namespace
}  // namespace mda::bars
