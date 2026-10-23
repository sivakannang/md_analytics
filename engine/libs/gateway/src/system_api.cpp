#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/gateway/system_api.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using detail::as_admin;

constexpr std::int64_t kIst = 19'800;
/// A queued delivery older than this is stuck (retries back off to an hour, but the first
/// tries are within minutes).
constexpr std::int64_t kStuckQueue = 10 * 60;

std::int64_t ist_midnight(std::int64_t t) {
    return t - (((t + kIst) % 86'400) + 86'400) % 86'400;
}

/// Queued, sent today, failed today, and the oldest queued notification's age.
json::object notifier_json(storage::Database& db, std::int64_t now, std::vector<Check>& checks) {
    const std::int64_t midnight = ist_midnight(now);
    json::object out{{"queued", nullptr},
                     {"sent_today", nullptr},
                     {"failed_today", nullptr},
                     {"oldest_queued_s", nullptr}};
    auto counts = db.prepare(
        "SELECT "
        "  (SELECT COUNT(*) FROM notification_deliveries WHERE status = 'pending'), "
        "  (SELECT MIN(n.created_at) FROM notification_deliveries d JOIN notifications n ON n.id = "
        "d.notification_id "
        "     WHERE d.status = 'pending'), "
        "  (SELECT COUNT(*) FROM notification_deliveries WHERE status = 'sent' AND sent_at >= ?1), "
        "  (SELECT COUNT(*) FROM notification_deliveries d JOIN notifications n ON n.id = "
        "d.notification_id "
        "     WHERE d.status = 'failed' AND n.created_at >= ?1)");
    if (!counts) {
        spdlog::error("admin/system: notifier query failed: {}", counts.error().message);
        return out;
    }
    counts->bind(1, midnight);
    if (counts->step().value_or(storage::StepResult::Done) != storage::StepResult::Row) {
        return out;
    }
    const std::int64_t failed = counts->column_int64(3);
    out["queued"] = counts->column_int64(0);
    out["sent_today"] = counts->column_int64(2);
    out["failed_today"] = failed;
    if (!counts->column_is_null(1)) {
        const std::int64_t age = now - counts->column_int64(1);
        out["oldest_queued_s"] = age;
        if (age > kStuckQueue) {
            checks.push_back({"notifier", "bad",
                              "a notification has waited " + std::to_string(age / 60) + " min"});
        }
    }
    if (failed > 0) {
        checks.push_back({"notifier", "warn", std::to_string(failed) + " deliveries failed today"});
    }
    return out;
}

}  // namespace

void register_system_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                            asio::any_io_executor db_executor, const SystemMonitor& monitor) {
    // GET /admin/system
    router.add(http::verb::get, "/api/v1/admin/system",
               [&auth, &db, db_executor, &monitor](const Request& request,
                                                   const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await as_admin(auth, db_executor, request, [&](const auth::User&) {
                       json::object report = monitor.report();
                       if (report.empty()) {
                           return failure(http::status::service_unavailable, "GeneralException",
                                          "the system monitor has no sample yet");
                       }
                       // The collector's checks, plus the notifier's (app.db is this thread's).
                       std::vector<Check> checks;
                       for (const auto& c : report.at("checks").as_array()) {
                           const auto& o = c.as_object();
                           checks.push_back({std::string(o.at("card").as_string()),
                                             std::string(o.at("level").as_string()),
                                             std::string(o.at("text").as_string())});
                       }
                       report["notifier"] = notifier_json(db, auth::system_clock_seconds(), checks);
                       json::array all;
                       for (const Check& c : checks) {
                           all.push_back(json::object{
                               {"card", c.card}, {"level", c.level}, {"text", c.text}});
                       }
                       report["checks"] = std::move(all);
                       report["status"] = worst_level(checks);
                       return success(std::move(report));
                   });
               });
}

}  // namespace mda::gateway
