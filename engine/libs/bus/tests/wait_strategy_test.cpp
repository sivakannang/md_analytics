#include <chrono>

#include <gtest/gtest.h>

#include <mda/bus/wait_strategy.hpp>

namespace mda::bus {
namespace {

using std::chrono::microseconds;

TEST(WaitStrategyTest, BackoffSpinsThenYieldsThenSleepsWithGrowingIntervals) {
    Backoff wait(
        {.spins = 3, .yields = 2, .min_sleep = microseconds(1), .max_sleep = microseconds(4)});
    for (int i = 0; i < 5; ++i) {
        wait.idle();  // spin, spin, spin, yield, yield
        EXPECT_EQ(wait.next_sleep(), microseconds(1));
    }
    wait.idle();  // sleeps 1 µs
    EXPECT_EQ(wait.next_sleep(), microseconds(2));
    wait.idle();  // sleeps 2 µs
    EXPECT_EQ(wait.next_sleep(), microseconds(4));
    wait.idle();  // sleeps 4 µs, capped
    EXPECT_EQ(wait.next_sleep(), microseconds(4));
}

TEST(WaitStrategyTest, BackoffResetReturnsToSpinning) {
    Backoff wait(
        {.spins = 1, .yields = 0, .min_sleep = microseconds(1), .max_sleep = microseconds(8)});
    wait.idle();
    wait.idle();
    wait.idle();
    EXPECT_EQ(wait.next_sleep(), microseconds(4));
    wait.reset();
    EXPECT_EQ(wait.next_sleep(), microseconds(1));
}

}  // namespace
}  // namespace mda::bus
