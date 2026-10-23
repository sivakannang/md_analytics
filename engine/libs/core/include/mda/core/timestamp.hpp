#pragma once

// Wall-clock time (ADR-0009): int64 nanoseconds since the Unix epoch, UTC.
//
// A plain integer inside, so its layout is fixed in shared memory and files (unlike
// std::chrono::time_point, whose clock and representation are implementation-defined).
// IST conversion is a display/trading-day concern, not done here.

#include <chrono>
#include <compare>
#include <cstdint>

namespace mda::core {

class Timestamp {
public:
    using Duration = std::chrono::nanoseconds;

    constexpr Timestamp() noexcept = default;

    [[nodiscard]] static constexpr Timestamp from_ns(std::int64_t ns) noexcept {
        return Timestamp(ns);
    }
    [[nodiscard]] static constexpr Timestamp from_seconds(std::int64_t seconds) noexcept {
        return Timestamp(seconds * 1'000'000'000);
    }
    /// Current wall-clock time (system clock).
    [[nodiscard]] static Timestamp now() noexcept {
        return Timestamp(std::chrono::duration_cast<Duration>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count());
    }

    [[nodiscard]] constexpr std::int64_t ns() const noexcept { return ns_; }
    [[nodiscard]] constexpr bool is_set() const noexcept { return ns_ != 0; }

    friend constexpr auto operator<=>(Timestamp, Timestamp) noexcept = default;
    friend constexpr Duration operator-(Timestamp a, Timestamp b) noexcept {
        return Duration(a.ns_ - b.ns_);
    }
    friend constexpr Timestamp operator+(Timestamp t, Duration d) noexcept {
        return Timestamp(t.ns_ + d.count());
    }

private:
    constexpr explicit Timestamp(std::int64_t ns) noexcept : ns_(ns) {}

    std::int64_t ns_ = 0;  // 0 = not set
};

}  // namespace mda::core
