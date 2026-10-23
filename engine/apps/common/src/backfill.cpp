#include <chrono>
#include <cstdio>

#include <mda/apps/backfill.hpp>

namespace mda::apps {
namespace {

constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;
constexpr std::int64_t kDay = 86'400;
constexpr std::int64_t kSessionOpen = 9 * 3600 + 15 * 60;    // after IST midnight
constexpr std::int64_t kSessionClose = 15 * 3600 + 40 * 60;  // F&O close

std::int64_t ist_midnight(std::int64_t unix_seconds) {
    const std::int64_t local = unix_seconds + kIst;
    return local - ((local % kDay) + kDay) % kDay - kIst;
}

}  // namespace

std::string ist_text(std::int64_t unix_seconds) {
    const std::chrono::sys_seconds local{std::chrono::seconds{unix_seconds + kIst}};
    const auto day = std::chrono::floor<std::chrono::days>(local);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss hms{local - day};
    char text[32];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u %02ld:%02ld:%02ld",
                  static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()), static_cast<long>(hms.hours().count()),
                  static_cast<long>(hms.minutes().count()),
                  static_cast<long>(hms.seconds().count()));
    return text;
}

std::vector<bars::Bar> to_bars(core::InstrumentId id, const std::vector<kite::Candle>& candles,
                               std::int64_t before) {
    std::vector<bars::Bar> out;
    out.reserve(candles.size());
    std::int64_t previous_oi = -1;
    for (const auto& candle : candles) {
        if (candle.start >= before) {
            break;  // the minute still forming (or later)
        }
        bars::Bar bar{};
        bar.header = messages::make_header<bars::Bar>(id, 0);
        bar.start = candle.start;
        bar.open = candle.open;
        bar.high = candle.high;
        bar.low = candle.low;
        bar.close = candle.close;
        bar.volume = core::Quantity(candle.volume);
        if (candle.oi > 0) {
            bar.oi_open = bar.oi_high = bar.oi_low = bar.oi_close = core::Quantity(candle.oi);
            bar.oi_updates = previous_oi >= 0 && candle.oi != previous_oi ? 1 : 0;
            previous_oi = candle.oi;
        }
        bar.source = bars::BarSource::Kite;
        out.push_back(bar);
    }
    return out;
}

bool in_market_hours(std::int64_t now) {
    const std::int64_t midnight = ist_midnight(now);
    const std::chrono::weekday weekday{
        std::chrono::sys_days{std::chrono::days{(midnight + kIst) / kDay}}};
    if (weekday == std::chrono::Saturday || weekday == std::chrono::Sunday) {
        return false;
    }
    const std::int64_t time_of_day = now - midnight;
    return time_of_day >= kSessionOpen && time_of_day < kSessionClose;
}

std::expected<BarCoverage, storage::DbError> bar_coverage(bars::BarStore& store,
                                                          std::uint32_t instrument_id,
                                                          std::int64_t now, int days) {
    const std::int64_t today = ist_midnight(now);
    BarCoverage coverage;
    const auto before_today = store.latest_before(instrument_id, today);
    if (!before_today) {
        return std::unexpected(before_today.error());
    }
    coverage.past = before_today->has_value() && (*before_today)->start >= today - days * kDay;
    const auto latest = store.latest_before(instrument_id, now + 60);
    if (!latest) {
        return std::unexpected(latest.error());
    }
    coverage.today = latest->has_value() && (*latest)->start >= today;
    return coverage;
}

std::optional<BackfillWindow> auto_backfill_window(std::int64_t now, BarCoverage coverage,
                                                   int days) {
    const std::int64_t today = ist_midnight(now);
    if (in_market_hours(now)) {
        if (coverage.today) {
            return std::nullopt;
        }
        return BackfillWindow{.from = today + kSessionOpen, .to = now};
    }
    if (coverage.past) {
        return std::nullopt;
    }
    // The last `days` days including today, as mda-backfill --days.
    return BackfillWindow{.from = today - static_cast<std::int64_t>(days - 1) * kDay + kSessionOpen,
                          .to = now};
}

}  // namespace mda::apps
