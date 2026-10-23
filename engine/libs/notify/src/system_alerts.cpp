#include <chrono>
#include <cstdio>

#include <boost/json.hpp>

#include <mda/notify/system_alerts.hpp>

namespace mda::notify {
namespace {

namespace json = boost::json;

constexpr std::int64_t kIst = 5 * 3600 + 30 * 60;

struct IstTime {
    bool weekday = false;
    int minute = 0;  // of the day
};

IstTime ist(std::int64_t unix_seconds) {
    const std::chrono::sys_seconds local{std::chrono::seconds{unix_seconds + kIst}};
    const auto day = std::chrono::floor<std::chrono::days>(local);
    const std::chrono::weekday weekday{day};
    return {.weekday = weekday != std::chrono::Saturday && weekday != std::chrono::Sunday,
            .minute = static_cast<int>(
                std::chrono::duration_cast<std::chrono::minutes>(local - day).count())};
}

}  // namespace

std::string ist_date(std::int64_t unix_seconds) {
    const std::chrono::sys_seconds local{std::chrono::seconds{unix_seconds + kIst}};
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(local)};
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return text;
}

std::string ist_clock(std::int64_t unix_seconds) {
    const std::int64_t s = ((unix_seconds + kIst) % 86'400 + 86'400) % 86'400;
    char text[16];
    std::snprintf(text, sizeof text, "%02lld:%02lld:%02lld", static_cast<long long>(s / 3600),
                  static_cast<long long>(s / 60 % 60), static_cast<long long>(s % 60));
    return text;
}

std::vector<Alert> FeedAlerts::update(FeedCondition condition, std::string_view detail,
                                      std::int64_t now) {
    std::vector<Alert> out;
    if (condition == FeedCondition::Up) {
        if (down_alerted_) {
            out.push_back({.type = "system.feed_restored",
                           .payload = json::serialize(json::object{{"since", ist_clock(now)}}),
                           .dedup_key = "system.feed_restored:" + std::to_string(*down_since_)});
        }
        down_since_.reset();
        down_alerted_ = false;
        return out;
    }
    if (condition == FeedCondition::AuthRejected && session_alerted_on_ != ist_date(now)) {
        if (auto alert = session_rejected("mda-feedd", now)) {
            session_alerted_on_ = ist_date(now);
            out.push_back(std::move(*alert));
        }
    }
    if (!down_since_) {
        down_since_ = now;
    }
    const IstTime t = ist(now);
    const bool market_open =
        t.weekday && t.minute >= options_.market_open && t.minute < options_.market_close;
    if (!down_alerted_ && market_open && now - *down_since_ >= options_.grace.count()) {
        down_alerted_ = true;
        const std::string state =
            condition == FeedCondition::AuthRejected ? "down: session rejected" : "down";
        out.push_back({.type = "system.feed_down",
                       .payload = json::serialize(json::object{{"state", state},
                                                               {"since", ist_clock(*down_since_)},
                                                               {"detail", std::string(detail)}}),
                       .dedup_key = "system.feed_down:" + std::to_string(*down_since_)});
    }
    return out;
}

std::optional<Alert> FeedAlerts::session_rejected(std::string_view process,
                                                  std::int64_t now) const {
    const IstTime t = ist(now);
    if (t.minute < options_.session_from || t.minute >= options_.session_until) {
        return std::nullopt;
    }
    return Alert{.type = "system.session_expired",
                 .payload = json::serialize(json::object{{"process", std::string(process)}}),
                 .dedup_key = "system.session_expired:" + ist_date(now)};
}

std::optional<Alert> login_reminder(std::int64_t now, std::optional<std::int64_t> written) {
    const IstTime t = ist(now);
    if (!t.weekday || t.minute < 8 * 60 + 45 || t.minute >= 15 * 60 + 30) {
        return std::nullopt;
    }
    if (written && ist_date(*written) == ist_date(now)) {
        return std::nullopt;  // mda-login already ran today
    }
    return Alert{.type = "system.login_reminder",
                 .payload = "{}",
                 .dedup_key = "system.login_reminder:" + ist_date(now)};
}

}  // namespace mda::notify
