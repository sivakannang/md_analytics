#include <mda/gateway/paper_api.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <expected>
#include <map>

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <mda/autotrade/metrics.hpp>
#include <mda/broker/instrument_catalog.hpp>
#include <mda/bus/wait_strategy.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/rms/risk.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

namespace json = boost::json;
using broker::ExecutionReport;
using broker::ReportEvent;
using detail::from_error;
using detail::run_on;
using detail::token_of;
using SteadyClock = std::chrono::steady_clock;

constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kIstMs = 19'800'000;

std::int64_t day_start_ms(std::int64_t ms) {
    return (ms + kIstMs) / kDayMs * kDayMs - kIstMs;
}

std::int64_t now_ms() {
    return auth::system_clock_seconds() * 1000;
}

/// 00:00 IST of "YYYY-MM-DD" in Unix ms; nullopt unless a valid date.
std::optional<std::int64_t> date_start(std::string_view text) {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (text.size() != 10 || text[4] != '-' || text[7] != '-' ||
        std::sscanf(std::string(text).c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3) {
        return std::nullopt;
    }
    const std::chrono::year_month_day day{std::chrono::year{y}, std::chrono::month{m}, std::chrono::day{d}};
    if (!day.ok()) {
        return std::nullopt;
    }
    return std::int64_t{std::chrono::sys_days{day}.time_since_epoch().count()} * kDayMs - kIstMs;
}

json::value price_json(core::Price price) {
    return price.raw() > 0 ? json::value(price.to_double()) : json::value(nullptr);
}

double rupees(oms::Paise amount) {
    return static_cast<double>(amount) / 100.0;
}

std::string_view event_name(ReportEvent event) {
    switch (event) {
    case ReportEvent::Accepted:
        return "accepted";
    case ReportEvent::Rejected:
        return "rejected";
    case ReportEvent::Triggered:
        return "triggered";
    case ReportEvent::Fill:
        return "fill";
    case ReportEvent::Cancelled:
        return "cancelled";
    case ReportEvent::Modified:
        return "modified";
    case ReportEvent::Status:
        return "status";
    case ReportEvent::KillSwitchOn:
        return "kill_switch_on";
    case ReportEvent::KillSwitchOff:
        return "kill_switch_off";
    case ReportEvent::Settled:
        return "settled";
    case ReportEvent::Converted:
        return "converted";
    }
    return "?";
}

std::string_view source_name(oms::Source source) {
    switch (source) {
    case oms::Source::Auto:
        return "auto";
    case oms::Source::System:
        return "system";
    case oms::Source::Ui:
        break;
    }
    return "ui";
}

json::object order_json(const broker::OrderRow& row) {
    const auto& o = row.order;
    const core::Price average =
        o.filled > 0 ? core::Price::from_raw((o.filled_value_raw + o.filled / 2) / o.filled) : core::Price{};
    return {{"order_id", o.id},
            {"client_order_id", o.spec.client_order_id},
            {"instrument_id", o.spec.instrument.value()},
            {"side", oms::name(o.spec.side)},
            {"type", oms::name(o.type)},
            {"placed_type", oms::name(o.spec.type)},
            {"product", oms::name(o.spec.product)},
            {"quantity", o.spec.quantity},
            {"filled", o.filled},
            {"price", price_json(o.spec.price)},
            {"trigger_price", price_json(o.spec.trigger)},
            {"average_price", price_json(average)},
            {"status", oms::name(o.status)},
            {"reason", o.reason},
            {"tag", o.spec.tag},
            {"source", source_name(o.spec.source)},
            {"charges", rupees(o.charges)},
            {"created_at", o.created_at},
            {"updated_at", o.updated_at}};
}

json::object trade_json(const broker::TradeRow& t) {
    return {{"order_id", t.order_id},
            {"instrument_id", t.instrument.value()},
            {"side", oms::name(t.side)},
            {"product", oms::name(t.product)},
            {"quantity", t.quantity},
            {"price", t.price.to_double()},
            {"charges", json::object{{"brokerage", rupees(t.charges.brokerage)},
                                     {"stt", rupees(t.charges.stt)},
                                     {"exchange", rupees(t.charges.exchange)},
                                     {"sebi", rupees(t.charges.sebi)},
                                     {"stamp", rupees(t.charges.stamp)},
                                     {"gst", rupees(t.charges.gst)},
                                     {"total", rupees(t.charges.total())}}},
            {"realised", rupees(t.realised)},
            {"at", t.at}};
}

std::optional<double> number_field(const json::object& body, std::string_view name) {
    const auto* value = body.if_contains(name);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    if (value->is_int64()) {
        return static_cast<double>(value->as_int64());
    }
    if (value->is_uint64()) {
        return static_cast<double>(value->as_uint64());
    }
    if (value->is_double()) {
        return value->as_double();
    }
    return std::nullopt;
}

core::Price to_price(double rupees_value) {
    return core::Price::from_raw(std::llround(rupees_value * static_cast<double>(core::Price::kScale)));
}

/// Whose paper account a read is about: the signed-in user's, or with ?user=<id> another
/// user's, for an admin only (Admin → Users). Order, cancel and kill-switch routes never take
/// it: nobody trades on someone else's account.
std::expected<std::int64_t, Response> subject_of(const Request& request, const auth::User& user) {
    const auto text = query_param(request, "user");
    if (!text) {
        return user.id;
    }
    std::int64_t id = 0;
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), id);
    if (error != std::errc{} || end != text->data() + text->size() || id <= 0) {
        return std::unexpected(detail::bad_input("user must be a user id"));
    }
    if (!user.is_admin()) {
        spdlog::warn("paper: user {} '{}' denied {}", user.id, user.username, std::string(request.target()));
        return std::unexpected(failure(http::status::forbidden, "PermissionException", "admin only"));
    }
    return id;
}

std::int64_t path_id(const PathParams& params) {
    return param_int(params, "id").value_or(0);
}

/// Sends `request` until a report that `match` accepts arrives (newer than `after`), or
/// gives up. The same request every time: a repeat is never a new order.
asio::awaitable<std::optional<ExecutionReport>> await_report(
    OrderSender& sender, const OrderUpdates& updates, broker::OrderRequest request, std::uint64_t after,
    std::function<bool(const ExecutionReport&)> match, PaperSettings settings) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    const auto deadline = SteadyClock::now() + settings.give_up_after;
    auto resend_at = SteadyClock::now();
    while (SteadyClock::now() < deadline) {
        if (SteadyClock::now() >= resend_at) {
            sender.send(request);
            resend_at = SteadyClock::now() + settings.resend_after;
        }
        if (auto found = updates.find(request.user, after, match)) {
            co_return found;
        }
        timer.expires_after(std::chrono::milliseconds(10));
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return std::nullopt;
}

Response answer(const std::optional<ExecutionReport>& report, std::uint64_t client_order_id) {
    if (!report) {
        // mda-broker didn't answer in time; the request may still be applied.
        return success(json::object{{"client_order_id", client_order_id}, {"status", "PENDING"}},
                       http::status::accepted);
    }
    if (report->event == ReportEvent::Rejected) {
        return failure(http::status::bad_request, "OrderException", broker::text_of(report->reason));
    }
    return success(json::object{{"order", report_json(*report)}});
}

}  // namespace

// ---------------------------------------------------------------- order updates

void OrderUpdates::add(const ExecutionReport& report) {
    const std::scoped_lock lock(mutex_);
    recent_.emplace_back(++version_, report);
    if (recent_.size() > kKept) {
        recent_.pop_front();
    }
}

std::vector<ExecutionReport> OrderUpdates::since(std::int64_t user, std::uint64_t& seen) const {
    std::vector<ExecutionReport> out;
    const std::scoped_lock lock(mutex_);
    for (const auto& [version, report] : recent_) {
        if (version > seen && report.user == user) {
            out.push_back(report);
        }
    }
    seen = version_;
    return out;
}

std::optional<ExecutionReport> OrderUpdates::find(std::int64_t user, std::uint64_t after,
                                                  const std::function<bool(const ExecutionReport&)>& match) const {
    const std::scoped_lock lock(mutex_);
    for (auto it = recent_.rbegin(); it != recent_.rend() && it->first > after; ++it) {
        if (it->second.user == user && match(it->second)) {
            return it->second;
        }
    }
    return std::nullopt;
}

std::uint64_t OrderUpdates::version() const {
    const std::scoped_lock lock(mutex_);
    return version_;
}

ExecFeed::ExecFeed(OrderUpdates& updates, std::string bus_name)
    : updates_(&updates), bus_name_(std::move(bus_name)), thread_([this](const std::stop_token& stop) { run(stop); }) {}

ExecFeed::~ExecFeed() = default;

void ExecFeed::run(const std::stop_token& stop) {
    bool reported = false;
    while (!stop.stop_requested()) {
        auto reader = bus::Reader::attach(bus_name_, {.name = "mda-gateway-exec"});
        if (!reader) {
            if (!reported) {
                spdlog::warn("paper: bus '{}' not available ({}); waiting for mda-broker", bus_name_,
                             bus::to_string(reader.error()));
                reported = true;
            }
            for (int i = 0; i < 20 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }
        spdlog::info("paper: reading bus '{}'", bus_name_);
        reported = false;
        bus::Backoff wait;
        auto next_heartbeat = SteadyClock::now();
        while (!stop.stop_requested()) {
            const auto result = reader->poll([&](bus::MsgType type, std::span<const std::byte> payload) {
                if (auto report = messages::decode<ExecutionReport>(type, payload)) {
                    updates_->add(*report);
                }
            });
            if (result.status == bus::PollStatus::Empty) {
                wait.idle();
            } else {
                wait.reset();
            }
            if (const auto now = SteadyClock::now(); now >= next_heartbeat) {
                next_heartbeat = now + std::chrono::seconds(1);
                reader->heartbeat();
            }
        }
    }
}

// ----------------------------------------------------------------- order sender

std::unique_ptr<OrderSender> OrderSender::open(const std::string& bus_name, std::size_t capacity) {
    auto writer = bus::Writer::create_or_attach({.name = bus_name, .capacity = capacity});
    if (!writer) {
        spdlog::error("paper: bus {}: {}", bus_name, bus::to_string(writer.error()));
        return nullptr;
    }
    spdlog::info("paper: writer on '{}' (epoch {})", bus_name, writer->epoch());
    return std::make_unique<OrderSender>(std::move(*writer));
}

void OrderSender::send(broker::OrderRequest request) {
    const std::scoped_lock lock(mutex_);
    request.header.schema_version = messages::MessageTraits<broker::OrderRequest>::version;
    request.header.sequence = ++sequence_;
    (void)writer_.publish(messages::to_wire(messages::MessageType::OrderRequest), messages::as_bytes(request));
}

std::uint64_t OrderSender::next_client_order_id() {
    const std::scoped_lock lock(mutex_);
    // Milliseconds × 1000 plus a counter: unique across restarts, and readable.
    last_id_ = std::max(last_id_ + 1, static_cast<std::uint64_t>(now_ms()) * 1000);
    return last_id_;
}

json::object report_json(const ExecutionReport& r) {
    return {{"order_id", r.order_id},
            {"client_order_id", r.client_order_id},
            {"instrument_id", r.header.instrument_id.value()},
            {"event", event_name(r.event)},
            {"status", oms::name(r.status)},
            {"side", oms::name(r.side)},
            {"type", oms::name(r.type)},
            {"product", oms::name(r.product)},
            {"source", source_name(r.source)},
            {"quantity", r.quantity},
            {"filled", r.filled},
            {"last_quantity", r.last_quantity},
            {"last_price", price_json(r.last_price)},
            {"average_price", price_json(r.average_price)},
            {"price", price_json(r.price)},
            {"trigger_price", price_json(r.trigger)},
            {"charges", rupees(r.charges)},
            {"reason", broker::text_of(r.reason)},
            {"reject_code", r.reject_code == 0 ? json::value(nullptr)
                                               : json::value(rms::name(static_cast<rms::RejectCode>(r.reject_code)))},
            {"at", r.at}};
}

// ----------------------------------------------------------------- auto trader

namespace {

std::string date_of_day(std::int64_t day_start) {
    const std::chrono::year_month_day ymd{
        std::chrono::sys_days{std::chrono::days{(day_start + kIstMs) / kDayMs}}};
    char text[16];
    std::snprintf(text, sizeof text, "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return text;
}

json::object metrics_json(const autotrade::Metrics& m) {
    json::object exits;
    for (const auto& [category, count] : m.exits) {
        exits[category] = count;
    }
    const int decided = m.wins + m.losses;
    return {{"round_trips", m.round_trips},
            {"wins", m.wins},
            {"losses", m.losses},
            {"win_rate", decided > 0 ? json::value(std::round(1000.0 * m.wins / decided) / 1000.0) : json::value(nullptr)},
            {"gross", rupees(m.gross)},
            {"charges", rupees(m.charges)},
            {"net", rupees(m.net)},
            {"average_win", rupees(m.average_win)},
            {"average_loss", rupees(m.average_loss)},
            {"largest_loss", rupees(m.largest_loss)},
            {"max_drawdown", rupees(m.max_drawdown)},
            {"exits", std::move(exits)}};
}

}  // namespace

void register_auto_routes(Router& router, auth::AuthService& auth, asio::any_io_executor db_executor,
                          broker::PaperStore& store, autotrade::DecisionStore& decisions,
                          asio::any_io_executor paper_executor, std::vector<AutoRunInfo> runs) {
    router.add(
        http::verb::get, "/api/v1/paper/auto",
        [&auth, db_executor, &store, &decisions, paper_executor, runs](const Request& request,
                                                                       const PathParams&) -> asio::awaitable<Response> {
            int days = 30;
            if (const auto text = query_param(request, "days")) {
                const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), days);
                if (error != std::errc{} || end != text->data() + text->size() || days < 1 || days > 365) {
                    co_return detail::bad_input("days must be 1-365");
                }
            }
            const std::string token = token_of(request);
            const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            const auto subject = subject_of(request, *user);
            if (!subject) {
                co_return subject.error();
            }
            const std::int64_t today = day_start_ms(now_ms());
            const std::int64_t from = today - std::int64_t{days - 1} * kDayMs;
            co_return co_await run_on(paper_executor, [&]() -> Response {
                const auto account = store.account_for_user(*subject).value_or(std::nullopt);
                json::array list;
                for (const auto& run : runs) {
                    if (run.user != *subject) {
                        continue;
                    }
                    char field[16] = {};
                    broker::set_text(field, "auto:" + run.id);
                    const std::string tag(broker::text_of(field));
                    const auto trades = account ? store.tagged_trades(account->id, tag, from, today + kDayMs)
                                                      .value_or(std::vector<broker::TradeRow>{})
                                                : std::vector<broker::TradeRow>{};
                    const auto all = decisions.decisions(run.id, from, today + kDayMs)
                                         .value_or(std::vector<autotrade::Decision>{});
                    const auto m = autotrade::metrics(trades, all);
                    // What it holds now, per instrument (an options run trades many contracts).
                    std::map<std::uint32_t, std::int64_t> net;
                    for (const auto& t : trades) {
                        net[t.instrument.value()] += (t.side == oms::Side::Buy ? 1 : -1) * t.quantity;
                    }
                    json::array held;
                    for (const auto& [instrument, quantity] : net) {
                        if (quantity != 0) {
                            held.push_back(json::object{{"instrument_id", instrument}, {"quantity", quantity}});
                        }
                    }
                    json::array recent;
                    for (auto it = all.rbegin(); it != all.rend() && recent.size() < 50; ++it) {
                        if (it->at < today) {
                            break;
                        }
                        recent.push_back(json::object{{"at", it->at},
                                                      {"state", autotrade::name(it->before)},
                                                      {"action", it->action},
                                                      {"reason", it->reason},
                                                      {"price", price_json(it->price)}});
                    }
                    const auto day = decisions.day(run.id, static_cast<std::int32_t>((today + kIstMs) / kDayMs))
                                         .value_or(std::nullopt);
                    list.push_back(json::object{
                        {"id", run.id},
                        {"strategy", run.strategy},
                        {"signal", run.signal},
                        {"underlying", run.underlying},
                        {"enabled", run.enabled},
                        {"settings", run.settings},
                        {"position", m.open_position},
                        {"held", std::move(held)},
                        {"entries_today", day ? day->entries : 0},
                        {"metrics", metrics_json(m)},
                        {"decisions_today", std::move(recent)}});
                }
                return success(json::object{{"from", date_of_day(from)}, {"days", days}, {"runs", std::move(list)}});
            });
        });
}

// ----------------------------------------------------------------------- routes

void register_paper_routes(Router& router, auth::AuthService& auth, storage::Database& db,
                           asio::any_io_executor db_executor, broker::PaperStore& store,
                           asio::any_io_executor paper_executor, OrderSender& sender, const OrderUpdates& updates,
                           const QuoteCache& quotes, PaperSettings settings) {
    // POST /paper/orders — place an order; answers once mda-broker accepts or rejects it.
    router.add(
        http::verb::post, "/api/v1/paper/orders",
        [&auth, db_executor, &sender, &updates, settings](const Request& request,
                                                          const PathParams&) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            if (!body) {
                co_return detail::bad_input("JSON body expected");
            }
            const auto instrument = number_field(*body, "instrument_id");
            const auto quantity = number_field(*body, "quantity");
            const auto side = string_field(*body, "side");
            const std::string type = string_field(*body, "type").value_or("MARKET");
            const std::string product = string_field(*body, "product").value_or("MIS");
            const bool known_type = type == "MARKET" || type == "LIMIT" || type == "SL" || type == "SL-M";
            if (!instrument || *instrument <= 0 || !quantity || !side || (*side != "BUY" && *side != "SELL") ||
                !known_type || (product != "MIS" && product != "NRML")) {
                co_return detail::bad_input(
                    "instrument_id, quantity, side BUY|SELL, type MARKET|LIMIT|SL|SL-M and product MIS|NRML expected");
            }
            const std::string token = token_of(request);
            const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            broker::OrderRequest r{};
            r.header.instrument_id = core::InstrumentId(static_cast<std::uint32_t>(*instrument));
            r.at = now_ms();
            r.user = user->id;
            const auto client = number_field(*body, "client_order_id");
            r.client_order_id = client && *client > 0 ? static_cast<std::uint64_t>(*client)
                                                      : sender.next_client_order_id();
            r.action = broker::Action::New;
            r.side = *side == "SELL" ? oms::Side::Sell : oms::Side::Buy;
            r.type = type == "LIMIT"  ? oms::OrderType::Limit
                     : type == "SL"   ? oms::OrderType::StopLimit
                     : type == "SL-M" ? oms::OrderType::StopMarket
                                      : oms::OrderType::Market;
            r.product = product == "NRML" ? oms::Product::Nrml : oms::Product::Mis;
            r.quantity = static_cast<std::int64_t>(*quantity);
            r.price = to_price(number_field(*body, "price").value_or(0));
            r.trigger = to_price(number_field(*body, "trigger_price").value_or(0));
            r.source = oms::Source::Ui;
            broker::set_text(r.tag, string_field(*body, "tag").value_or(""));
            const std::uint64_t client_order_id = r.client_order_id;
            const auto report = co_await await_report(
                sender, updates, r, 0, [client_order_id](const ExecutionReport& e) { return e.client_order_id == client_order_id; },
                settings);
            co_return answer(report, client_order_id);
        });

    // PUT /paper/orders/{id} — new quantity, price and trigger; DELETE — cancel.
    const auto change = [&auth, db_executor, &sender, &updates, settings](
                            broker::Action action, const Request& request,
                            const PathParams& params) -> asio::awaitable<Response> {
        const std::int64_t id = path_id(params);
        std::optional<json::object> body;
        if (action == broker::Action::Modify) {
            body = json_body(request);
            if (!body || !number_field(*body, "quantity")) {
                co_return detail::bad_input("JSON body with quantity (and price, trigger_price) expected");
            }
        }
        if (id <= 0) {
            co_return detail::bad_input("numeric order id expected");
        }
        const std::string token = token_of(request);
        const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
        if (!user) {
            co_return from_error(user.error());
        }
        broker::OrderRequest r{};
        r.at = now_ms();
        r.user = user->id;
        r.client_order_id = sender.next_client_order_id();
        r.order_id = id;
        r.action = action;
        r.source = oms::Source::Ui;
        if (body) {
            r.quantity = static_cast<std::int64_t>(*number_field(*body, "quantity"));
            r.price = to_price(number_field(*body, "price").value_or(0));
            r.trigger = to_price(number_field(*body, "trigger_price").value_or(0));
        }
        const std::uint64_t client_order_id = r.client_order_id;
        const auto report = co_await await_report(
            sender, updates, r, updates.version(),
            [id, client_order_id](const ExecutionReport& e) {
                return e.order_id == id || e.client_order_id == client_order_id;
            },
            settings);
        co_return answer(report, client_order_id);
    };
    router.add(http::verb::put, "/api/v1/paper/orders/{id}",
               [change](const Request& request, const PathParams& params) -> asio::awaitable<Response> {
                   co_return co_await change(broker::Action::Modify, request, params);
               });
    router.add(http::verb::delete_, "/api/v1/paper/orders/{id}",
               [change](const Request& request, const PathParams& params) -> asio::awaitable<Response> {
                   co_return co_await change(broker::Action::Cancel, request, params);
               });

    // POST /paper/kill-switch {"on": true|false}
    router.add(
        http::verb::post, "/api/v1/paper/kill-switch",
        [&auth, db_executor, &sender, &updates, settings](const Request& request,
                                                          const PathParams&) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            const auto* on = body ? body->if_contains("on") : nullptr;
            if (on == nullptr || !on->is_bool()) {
                co_return detail::bad_input(R"(JSON body {"on": true|false} expected)");
            }
            const std::string token = token_of(request);
            const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            broker::OrderRequest r{};
            r.at = now_ms();
            r.user = user->id;
            r.client_order_id = sender.next_client_order_id();
            r.action = on->as_bool() ? broker::Action::KillSwitchOn : broker::Action::KillSwitchOff;
            r.source = oms::Source::Ui;
            const std::uint64_t client_order_id = r.client_order_id;
            const auto report = co_await await_report(
                sender, updates, r, updates.version(),
                [client_order_id](const ExecutionReport& e) {
                    return e.client_order_id == client_order_id &&
                           (e.event == ReportEvent::KillSwitchOn || e.event == ReportEvent::KillSwitchOff);
                },
                settings);
            if (!report) {
                co_return answer(report, client_order_id);
            }
            co_return success(json::object{{"kill_switch", report->event == ReportEvent::KillSwitchOn}});
        });

    // POST /paper/positions/convert {"instrument_id", "quantity", "to": "NRML"|"MIS"} — moves
    // that much of the position from the other product, as Zerodha's position conversion.
    router.add(
        http::verb::post, "/api/v1/paper/positions/convert",
        [&auth, db_executor, &sender, &updates, settings](const Request& request,
                                                          const PathParams&) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            const auto instrument = body ? number_field(*body, "instrument_id") : std::nullopt;
            const auto quantity = body ? number_field(*body, "quantity") : std::nullopt;
            const std::string to = body ? string_field(*body, "to").value_or("") : "";
            if (!instrument || *instrument <= 0 || !quantity || *quantity <= 0 || (to != "MIS" && to != "NRML")) {
                co_return detail::bad_input(R"(JSON body {"instrument_id", "quantity", "to": "NRML"|"MIS"} expected)");
            }
            const std::string token = token_of(request);
            const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            broker::OrderRequest r{};
            r.header.instrument_id = core::InstrumentId(static_cast<std::uint32_t>(*instrument));
            r.at = now_ms();
            r.user = user->id;
            r.client_order_id = sender.next_client_order_id();
            r.action = broker::Action::Convert;
            r.quantity = static_cast<std::int64_t>(*quantity);
            r.product = to == "NRML" ? oms::Product::Nrml : oms::Product::Mis;
            r.source = oms::Source::Ui;
            const std::uint64_t client_order_id = r.client_order_id;
            const auto report = co_await await_report(
                sender, updates, r, updates.version(),
                [client_order_id](const ExecutionReport& e) { return e.client_order_id == client_order_id; },
                settings);
            co_return answer(report, client_order_id);
        });

    // GET /paper/orders?date= and /paper/trades?date= — one IST day (default today).
    const auto day_list = [&auth, db_executor, &store, paper_executor](bool trades, const Request& request)
        -> asio::awaitable<Response> {
        std::int64_t from = day_start_ms(now_ms());
        if (const auto date = query_param(request, "date")) {
            const auto parsed = date_start(*date);
            if (!parsed) {
                co_return detail::bad_input("date must be YYYY-MM-DD");
            }
            from = *parsed;
        }
        const std::string token = token_of(request);
        const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
        if (!user) {
            co_return from_error(user.error());
        }
        const auto subject = subject_of(request, *user);
        if (!subject) {
            co_return subject.error();
        }
        co_return co_await run_on(paper_executor, [&]() -> Response {
            const auto account = store.account_for_user(*subject);
            if (!account) {
                return failure(http::status::internal_server_error, "GeneralException", "internal error");
            }
            json::array list;
            if (*account) {
                if (trades) {
                    const auto rows = store.trades((*account)->id, from, from + kDayMs);
                    for (const auto& row : rows.value_or(std::vector<broker::TradeRow>{})) {
                        list.push_back(trade_json(row));
                    }
                } else {
                    const auto rows = store.orders((*account)->id, from, from + kDayMs);
                    for (const auto& row : rows.value_or(std::vector<broker::OrderRow>{})) {
                        list.push_back(order_json(row));
                    }
                }
            }
            return success(std::move(list));
        });
    };
    router.add(http::verb::get, "/api/v1/paper/orders",
               [day_list](const Request& request, const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await day_list(false, request);
               });
    router.add(http::verb::get, "/api/v1/paper/trades",
               [day_list](const Request& request, const PathParams&) -> asio::awaitable<Response> {
                   co_return co_await day_list(true, request);
               });

    // GET /paper/positions — open positions, and those traded or settled today, at the last
    // price, as Kite shows them: realised is today's (from today's trades and settlements),
    // and positions closed on an earlier day are left out (they stay in paper.db).
    router.add(
        http::verb::get, "/api/v1/paper/positions",
        [&auth, db_executor, &store, paper_executor, &quotes](const Request& request,
                                                              const PathParams&) -> asio::awaitable<Response> {
            const std::string token = token_of(request);
            const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            const auto subject = subject_of(request, *user);
            if (!subject) {
                co_return subject.error();
            }
            co_return co_await run_on(paper_executor, [&]() -> Response {
                json::array list;
                const auto account = store.account_for_user(*subject).value_or(std::nullopt);
                if (!account) {
                    return success(std::move(list));
                }
                const auto positions = store.positions(account->id).value_or(std::map<oms::PositionKey, oms::Position>{});
                const std::int64_t today = day_start_ms(now_ms());
                std::map<oms::PositionKey, oms::Paise> realised_today;  // keys traded or settled today
                std::map<oms::PositionKey, oms::Paise> closed_today;    // the same, from the entry price
                for (const auto& trade :
                     store.trades(account->id, today, today + kDayMs).value_or(std::vector<broker::TradeRow>{})) {
                    realised_today[{.instrument = trade.instrument, .product = trade.product}] += trade.realised;
                    closed_today[{.instrument = trade.instrument, .product = trade.product}] += trade.entry_realised;
                }
                // A settlement's or daily MTM's ledger row has no product: it goes to that
                // instrument's first flat position (expiry) or open one (MTM); MIS is squared
                // off before either, so it's NRML.
                for (const auto& entry : store.ledger(account->id).value_or(std::vector<oms::LedgerEntry>{})) {
                    const bool expiry = entry.kind == oms::LedgerKind::Expiry;
                    if ((!expiry && entry.kind != oms::LedgerKind::Mtm) || entry.at < today) {
                        continue;
                    }
                    for (const auto& [key, position] : positions) {
                        if (key.instrument == entry.instrument && (position.quantity() == 0) == expiry) {
                            realised_today[key] += entry.amount;
                            if (expiry) {
                                closed_today[key] += entry.amount;
                            }
                            break;
                        }
                    }
                }
                for (const auto& [key, position] : positions) {
                    const auto today_it = realised_today.find(key);
                    if (position.quantity() == 0 && today_it == realised_today.end()) {
                        continue;
                    }
                    const auto quote = quotes.get(key.instrument.value());
                    const core::Price last = quote ? quote->snapshot.ltp : core::Price{};
                    const auto closed_it = closed_today.find(key);
                    list.push_back(json::object{
                        {"instrument_id", key.instrument.value()},
                        {"product", oms::name(key.product)},
                        {"quantity", position.quantity()},
                        {"average_price", price_json(position.average_price())},
                        {"entry_price", price_json(position.entry_price())},
                        {"last_price", price_json(last)},
                        {"realised", rupees(today_it == realised_today.end() ? 0 : today_it->second)},
                        {"closed_pnl", rupees(closed_it == closed_today.end() ? 0 : closed_it->second)},
                        {"unrealised", last.raw() > 0 ? json::value(rupees(position.unrealised(last)))
                                                      : json::value(nullptr)}});
                }
                return success(std::move(list));
            });
        });

    // GET /paper/funds — cash, blocked margin, available funds and today's P&L.
    router.add(
        http::verb::get, "/api/v1/paper/funds",
        [&auth, &db, db_executor, &store, paper_executor, &quotes, settings](
            const Request& request, const PathParams&) -> asio::awaitable<Response> {
            const std::string token = token_of(request);
            const auto user = co_await run_on(db_executor, [&] { return auth.authenticate(token); });
            if (!user) {
                co_return from_error(user.error());
            }
            const auto subject = subject_of(request, *user);
            if (!subject) {
                co_return subject.error();
            }
            struct State {
                std::optional<broker::AccountRow> account;
                std::vector<oms::LedgerEntry> ledger;
                std::map<oms::PositionKey, oms::Position> positions;
                std::vector<broker::OrderRow> live;
            };
            const auto state = co_await run_on(paper_executor, [&]() {
                State s;
                s.account = store.account_for_user(*subject).value_or(std::nullopt);
                if (s.account) {
                    s.ledger = store.ledger(s.account->id).value_or(std::vector<oms::LedgerEntry>{});
                    s.positions = store.positions(s.account->id).value_or(std::map<oms::PositionKey, oms::Position>{});
                    for (auto& row : store.live_orders().value_or(std::vector<broker::OrderRow>{})) {
                        if (row.order.spec.account == s.account->id) {
                            s.live.push_back(std::move(row));
                        }
                    }
                }
                return s;
            });
            if (!state.account) {
                co_return success(json::object{{"opening_cash", rupees(settings.opening_cash)},
                                               {"cash", rupees(settings.opening_cash)},
                                               {"blocked", 0.0},
                                               {"available", rupees(settings.opening_cash)},
                                               {"realised_today", 0.0},
                                               {"charges_today", 0.0},
                                               {"unrealised", 0.0},
                                               {"kill_switch", false}});
            }
            // Margin of open positions needs the catalog (app.db, its own thread).
            const oms::Paise position_margin = co_await run_on(db_executor, [&]() {
                broker::InstrumentCatalog catalog(db, settings.freeze_quantity, settings.mcx_margins);
                rms::RiskConfig config;
                config.futures_margin = settings.futures_margin;
                config.stock_futures_margin = settings.stock_futures_margin;
                const rms::RiskManager risk(config);
                oms::Paise margin = 0;
                for (const auto& [key, position] : state.positions) {
                    if (position.quantity() == 0) {
                        continue;
                    }
                    if (const auto info = catalog.get(key.instrument)) {
                        margin += risk.margin(*info, position.quantity() > 0 ? oms::Side::Buy : oms::Side::Sell,
                                              std::abs(position.quantity()), position.average_price());
                    }
                }
                return margin;
            });
            const std::int64_t today = day_start_ms(now_ms());
            oms::Paise cash = 0;
            oms::Paise opening = 0;
            oms::Paise realised = 0;
            oms::Paise charges = 0;
            for (const auto& entry : state.ledger) {
                cash += entry.amount;
                if (entry.at < today || entry.kind == oms::LedgerKind::Opening) {
                    opening += entry.amount;
                } else if (entry.kind == oms::LedgerKind::Charges) {
                    charges -= entry.amount;
                } else {
                    realised += entry.amount;
                }
            }
            oms::Paise unrealised = 0;
            for (const auto& [key, position] : state.positions) {
                if (const auto quote = quotes.get(key.instrument.value()); quote && quote->snapshot.ltp.raw() > 0) {
                    unrealised += position.unrealised(quote->snapshot.ltp);
                }
            }
            oms::Paise blocked = position_margin;
            for (const auto& row : state.live) {
                const auto& o = row.order;
                blocked += o.spec.quantity > 0 ? row.margin * (o.spec.quantity - o.filled) / o.spec.quantity : 0;
            }
            co_return success(json::object{{"opening_cash", rupees(opening)},
                                           {"cash", rupees(cash)},
                                           {"blocked", rupees(blocked)},
                                           {"available", rupees(cash - blocked + std::min<oms::Paise>(0, unrealised))},
                                           {"realised_today", rupees(realised)},
                                           {"charges_today", rupees(charges)},
                                           {"unrealised", rupees(unrealised)},
                                           {"kill_switch", state.account->kill_switch}});
        });
}

}  // namespace mda::gateway
