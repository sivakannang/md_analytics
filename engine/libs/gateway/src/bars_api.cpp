#include <algorithm>
#include <charconv>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/auth/session_token.hpp>
#include <mda/bars/aggregate.hpp>
#include <mda/bars/buildup.hpp>
#include <mda/gateway/bars_api.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using detail::from_error;
using detail::run_on;
using detail::token_of;

constexpr std::size_t kMaxBars = 10'000;

/// Bars read before `from` so the first bar shown has a reference to be classified
/// against (the last earlier bar with an OI update): a week covers weekends and holidays.
constexpr std::int64_t kBuildupLookback = 7 * 86'400;

/// How far back a request without `from` goes, per interval.
std::int64_t default_span(bars::Interval interval) {
    if (interval.daily)
        return 400 * 86'400;
    if (interval.minutes >= 60)
        return 60 * 86'400;
    if (interval.minutes >= 15)
        return 30 * 86'400;
    if (interval.minutes >= 3)
        return 7 * 86'400;
    return 2 * 86'400;
}

std::int64_t bucket_end(const bars::Bar& bar, bars::Interval interval) {
    return bar.start + (interval.daily ? 86'400 : static_cast<std::int64_t>(interval.minutes) * 60);
}

/// [time, open, high, low, close, volume, oi, oi_open, oi_high, oi_low, oi_updates,
///  oi_time, buildup, oi_change]; oi_time, buildup and oi_change are null when unknown.
json::array bar_row(const bars::Bar& bar, const bars::BuildupPoint& point) {
    const core::Buildup buildup = point.state;
    json::value oi_time = nullptr;
    if (bar.oi_update_offset != bars::kNoOiUpdateTime) {
        oi_time = bar.start + bar.oi_update_offset;
    }
    json::value state = nullptr;
    if (const auto name = detail::buildup_name(buildup); !name.empty()) {
        state = name;
    }
    return json::array{bar.start,
                       bar.open.to_double(),
                       bar.high.to_double(),
                       bar.low.to_double(),
                       bar.close.to_double(),
                       bar.volume.value(),
                       bar.oi_close.value(),
                       bar.oi_open.value(),
                       bar.oi_high.value(),
                       bar.oi_low.value(),
                       bar.oi_updates,
                       std::move(oi_time),
                       std::move(state),
                       point.oi_change ? json::value(*point.oi_change) : json::value(nullptr)};
}

std::optional<std::int64_t> int_param(const Request& request, std::string_view name) {
    const auto text = query_param(request, name);
    if (!text) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

void register_bars_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                          asio::any_io_executor db_executor, bars::BarStore& store,
                          asio::any_io_executor bars_executor) {
    // GET /instruments/{id}/bars?interval=5m&from=<unix>&to=<unix>
    router.add(
        http::verb::get, "/api/v1/instruments/{id}/bars",
        [&auth, &db, db_executor, &store, bars_executor](
            const Request& request, const PathParams& params) -> asio::awaitable<Response> {
            const auto id = param_int(params, "id");
            const auto interval =
                bars::parse_interval(query_param(request, "interval").value_or("5m"));
            if (!id || *id <= 0 || !interval) {
                co_return detail::bad_input(
                    "numeric instrument id and interval 1m|3m|5m|15m|30m|1h|1d expected");
            }
            const std::string token = token_of(request);
            const auto user =
                co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            // 0 (unknown instrument, or none in the catalog) leaves the OI fraction alone.
            const core::Quantity lot_size = co_await run_on(db_executor, [&] {
                auto lookup = db.prepare("SELECT lot_size FROM instruments WHERE id = ?1");
                if (!lookup) {
                    return core::Quantity{};
                }
                lookup->bind(1, *id);
                if (lookup->step().value_or(storage::StepResult::Done) !=
                    storage::StepResult::Row) {
                    return core::Quantity{};
                }
                return core::Quantity(lookup->column_int64(0));
            });
            const std::int64_t now = auth::system_clock_seconds();
            const std::int64_t to = int_param(request, "to").value_or(now + 60);
            const std::int64_t from = bars::bucket_start(
                int_param(request, "from").value_or(to - default_span(*interval)), *interval);

            co_return co_await run_on(bars_executor, [&]() -> Response {
                auto minutes =
                    store.range(static_cast<std::uint32_t>(*id), from - kBuildupLookback, to);
                if (!minutes) {
                    spdlog::error("bars: query failed: {}", minutes.error().message);
                    return failure(http::status::internal_server_error, "GeneralException",
                                   "internal error");
                }
                bars::mark_backfill_oi_updates(*minutes);
                auto series = bars::aggregate(*minutes, *interval);
                // A bucket still in progress is peeked, so its state may change until it closes.
                if (!series.empty() && bucket_end(series.back(), *interval) > now) {
                    series.back().header.flags = bars::kBarForming;
                }
                const auto points = bars::classify_buildup_points(series, lot_size);
                const auto first = std::ranges::find_if(
                    series, [&](const bars::Bar& b) { return b.start >= from; });
                std::size_t begin = static_cast<std::size_t>(first - series.begin());
                begin = std::max(begin, series.size() > kMaxBars ? series.size() - kMaxBars
                                                                 : std::size_t{0});  // the latest
                json::array rows;
                rows.reserve(series.size() - begin);
                for (std::size_t i = begin; i < series.size(); ++i) {
                    rows.push_back(bar_row(series[i], points[i]));
                }
                return success(
                    json::object{{"instrument_id", *id},
                                 {"interval", query_param(request, "interval").value_or("5m")},
                                 {"bars", std::move(rows)}});
            });
        });
}

}  // namespace mda::gateway
