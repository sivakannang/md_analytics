#include <mda/instruments/session.hpp>

namespace mda::instruments {

namespace {

using std::chrono::year_month_day;

/// The n-th Sunday (1-based) of a month.
year_month_day nth_sunday(std::chrono::year year, std::chrono::month month, unsigned n) {
    return year_month_day{std::chrono::sys_days{year / month / std::chrono::Sunday[n]}};
}

}  // namespace

bool us_daylight_saving(year_month_day day) noexcept {
    const auto starts = nth_sunday(day.year(), std::chrono::March, 2);
    const auto ends = nth_sunday(day.year(), std::chrono::November, 1);
    return day >= starts && day < ends;
}

Session trading_session(Exchange exchange, Market market, year_month_day day) noexcept {
    if (exchange == Exchange::Mcx) {
        const int close = us_daylight_saving(day) ? 23 * 60 + 30 : 23 * 60 + 55;
        return {.open = 9 * 60, .close = close, .last = close};
    }
    return {.open = 9 * 60 + 15,
            .close = 15 * 60 + 30,
            .last = market == Market::Fo ? 15 * 60 + 40 : 15 * 60 + 30};
}

year_month_day ist_day(std::int64_t unix_seconds) noexcept {
    const std::int64_t days = (unix_seconds + kIstOffsetSeconds) / 86'400 -
                              ((unix_seconds + kIstOffsetSeconds) % 86'400 < 0 ? 1 : 0);
    return year_month_day{std::chrono::sys_days{std::chrono::days{days}}};
}

int last_close(year_month_day day) noexcept {
    return trading_session(Exchange::Mcx, Market::Fo, day).close;
}

}  // namespace mda::instruments
