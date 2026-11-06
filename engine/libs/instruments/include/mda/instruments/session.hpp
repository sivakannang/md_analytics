#pragma once

// Trading hours per exchange (ADR-0031): the one place that knows 09:15–15:30.
// - NSE and BSE: 09:15–15:30 IST; F&O prints until 15:40 (the closing auction session).
// - MCX: 09:00 until 23:30 while the US is on daylight saving time (second Sunday of March
//   to first Sunday of November), 23:55 otherwise. Agri contracts close earlier; their
//   ticks just stop.
// Holidays are not known here.

#include <chrono>
#include <cstdint>

#include <mda/instruments/contract_key.hpp>

namespace mda::instruments {

/// Minutes after IST midnight.
struct Session {
    int open = 0;   ///< continuous trading starts
    int close = 0;  ///< continuous trading ends: orders are refused from here
    int last = 0;   ///< prints (bars) end here: 15:40 for NSE/BSE F&O, else = close
};

inline constexpr std::int64_t kIstOffsetSeconds = 19'800;

/// Whether US daylight saving time is in effect on this (IST) trading day.
[[nodiscard]] bool us_daylight_saving(std::chrono::year_month_day day) noexcept;

[[nodiscard]] Session trading_session(Exchange exchange, Market market,
                                      std::chrono::year_month_day day) noexcept;

/// The IST calendar day of a Unix time in seconds.
[[nodiscard]] std::chrono::year_month_day ist_day(std::int64_t unix_seconds) noexcept;

/// The latest close of any exchange that day (MCX's when it trades): when the market
/// processes may stop.
[[nodiscard]] int last_close(std::chrono::year_month_day day) noexcept;

}  // namespace mda::instruments
