#include <algorithm>
#include <charconv>
#include <optional>

#include <spdlog/spdlog.h>

#include <mda/catalog/catalog.hpp>
#include <mda/gateway/watchlist_api.hpp>

#include "api_common.hpp"

namespace mda::gateway {
namespace {

using namespace detail;
using watchlists::WatchlistError;

constexpr std::size_t kDefaultSearchLimit = 20;
constexpr std::size_t kMaxSearchLimit = 50;

struct ApiFailure {
    http::status status = http::status::internal_server_error;
    std::string type = "GeneralException";
    std::string message = "internal error";
};
using ApiResult = std::expected<boost::json::value, ApiFailure>;

ApiFailure failure_of(const AuthError& error) {
    switch (error.code) {
        case Code::Auth:
            return {http::status::unauthorized, "AuthException", error.message};
        case Code::Input:
            return {http::status::bad_request, "InputException", error.message};
        default:
            return {};
    }
}

ApiFailure failure_of(const WatchlistError& error) {
    switch (error.code) {
        case WatchlistError::Code::Input:
            return {http::status::bad_request, "InputException", error.message};
        case WatchlistError::Code::NotFound:
            return {http::status::not_found, "NotFoundException", error.message};
        case WatchlistError::Code::Conflict:
            return {http::status::conflict, "ConflictException", error.message};
        case WatchlistError::Code::Limit:
            return {http::status::bad_request, "InputException", error.message};
        case WatchlistError::Code::Internal:
            break;
    }
    spdlog::error("watchlists: database error: {}", error.message);
    return {};
}

Response respond(const ApiResult& result, http::status ok = http::status::ok) {
    if (!result) {
        return failure(result.error().status, result.error().type, result.error().message);
    }
    return success(*result, ok);
}

boost::json::object instrument_json(const catalog::InstrumentInfo& i) {
    boost::json::object json{{"instrument_id", i.id},
                             {"exchange", i.exchange},
                             {"market", i.market},
                             {"kind", i.kind},
                             {"underlying", i.underlying},
                             {"trading_symbol", i.trading_symbol},
                             {"name", i.name},
                             {"lot_size", i.lot_size},
                             {"tick_size", i.tick_size.to_double()},
                             {"listed", i.listed}};
    json["expiry"] = i.expiry.empty() ? boost::json::value(nullptr) : boost::json::value(i.expiry);
    json["strike"] =
        i.strike ? boost::json::value(i.strike->to_double()) : boost::json::value(nullptr);
    json["option_type"] =
        i.option_type.empty() ? boost::json::value(nullptr) : boost::json::value(i.option_type);
    return json;
}

boost::json::object watchlist_json(const watchlists::Watchlist& list) {
    boost::json::array groups;
    for (const auto& group : list.groups) {
        groups.push_back(boost::json::object{{"id", group.id}, {"name", group.name}});
    }
    boost::json::array items;
    for (const auto& item : list.items) {
        boost::json::object json = instrument_json(item.instrument);
        json["added_at"] = iso_utc(item.added_at);
        json["group_id"] =
            item.group_id != 0 ? boost::json::value(item.group_id) : boost::json::value(nullptr);
        items.push_back(std::move(json));
    }
    return {{"id", list.id},
            {"name", list.name},
            {"created_at", iso_utc(list.created_at)},
            {"groups", std::move(groups)},
            {"items", std::move(items)}};
}

/// {"groups": [3, 1], "items": [{"instrument_id": 7, "group_id": 3 | null}, ...]}
std::optional<watchlists::Order> order_body(const boost::json::object& body) {
    const auto* groups = body.if_contains("groups");
    const auto* items = body.if_contains("items");
    if (groups == nullptr || !groups->is_array() || items == nullptr || !items->is_array()) {
        return std::nullopt;
    }
    watchlists::Order order;
    for (const auto& id : groups->as_array()) {
        if (!id.is_int64() || id.as_int64() <= 0) {
            return std::nullopt;
        }
        order.groups.push_back(id.as_int64());
    }
    for (const auto& entry : items->as_array()) {
        if (!entry.is_object()) {
            return std::nullopt;
        }
        const auto* instrument = entry.as_object().if_contains("instrument_id");
        const auto* group = entry.as_object().if_contains("group_id");
        if (instrument == nullptr || !instrument->is_int64() ||
            (group != nullptr && !group->is_null() && !group->is_int64())) {
            return std::nullopt;
        }
        order.items.emplace_back(instrument->as_int64(),
                                 group == nullptr || group->is_null() ? 0 : group->as_int64());
    }
    return order;
}

std::optional<std::int64_t> int_field(const boost::json::object& body, std::string_view name) {
    const auto* value = body.if_contains(name);
    if (value != nullptr && value->is_int64() && value->as_int64() > 0) {
        return value->as_int64();
    }
    return std::nullopt;
}

}  // namespace

void register_watchlist_routes(Router& router, auth::AuthService& auth,
                               watchlists::WatchlistService& lists, storage::Database& db,
                               asio::any_io_executor db_executor) {
    // Runs `work(user)` for the signed-in user on the database thread.
    auto as_user = [&auth, db_executor](const Request& request,
                                        auto work) -> asio::awaitable<ApiResult> {
        const std::string token = token_of(request);
        co_return co_await run_on(db_executor, [&]() -> ApiResult {
            const auto user = auth.authenticate(token);
            if (!user) {
                return std::unexpected(failure_of(user.error()));
            }
            return work(*user);
        });
    };

    // GET /instruments/search?q=nifty+25000+ce&limit=20
    router.add(
        http::verb::get, "/api/v1/instruments/search",
        [&db, as_user](const Request& request, const PathParams&) -> asio::awaitable<Response> {
            const std::string query = query_param(request, "q").value_or("");
            std::size_t limit = kDefaultSearchLimit;
            if (const auto text = query_param(request, "limit")) {
                std::from_chars(text->data(), text->data() + text->size(), limit);
                limit = std::clamp<std::size_t>(limit, 1, kMaxSearchLimit);
            }
            co_return respond(co_await as_user(request, [&](const auth::User&) -> ApiResult {
                const auto found = catalog::search(db, query.substr(0, 100), limit);
                if (!found) {
                    spdlog::error("instruments: search failed: {}", found.error().message);
                    return std::unexpected(ApiFailure{});
                }
                boost::json::array results;
                for (const auto& info : *found) {
                    results.push_back(instrument_json(info));
                }
                return results;
            }));
        });

    // GET /instruments/{id}
    router.add(
        http::verb::get, "/api/v1/instruments/{id}",
        [&db, as_user](const Request& request,
                       const PathParams& params) -> asio::awaitable<Response> {
            const auto id = param_int(params, "id");
            if (!id) {
                co_return bad_input("numeric instrument id expected");
            }
            co_return respond(co_await as_user(request, [&](const auth::User&) -> ApiResult {
                const auto found = catalog::get(db, *id);
                if (!found) {
                    return std::unexpected(ApiFailure{});
                }
                if (!*found) {
                    return std::unexpected(ApiFailure{http::status::not_found, "NotFoundException",
                                                      "instrument not found"});
                }
                return instrument_json(**found);
            }));
        });

    // GET /watchlists
    router.add(
        http::verb::get, "/api/v1/watchlists",
        [&lists, as_user](const Request& request, const PathParams&) -> asio::awaitable<Response> {
            co_return respond(co_await as_user(request, [&](const auth::User& user) -> ApiResult {
                const auto found = lists.list(user.id, auth::system_clock_seconds());
                if (!found) {
                    return std::unexpected(failure_of(found.error()));
                }
                boost::json::array json;
                for (const auto& list : *found) {
                    json.push_back(watchlist_json(list));
                }
                return json;
            }));
        });

    // POST /watchlists {name}
    router.add(
        http::verb::post, "/api/v1/watchlists",
        [&lists, as_user](const Request& request, const PathParams&) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            const auto name = body ? string_field(*body, "name") : std::nullopt;
            if (!name) {
                co_return bad_input("expected JSON {\"name\"}");
            }
            co_return respond(
                co_await as_user(request,
                                 [&](const auth::User& user) -> ApiResult {
                                     const auto created =
                                         lists.create(user.id, *name, auth::system_clock_seconds());
                                     if (!created) {
                                         return std::unexpected(failure_of(created.error()));
                                     }
                                     spdlog::info("watchlists: user {} created list {} '{}'",
                                                  user.id, created->id, created->name);
                                     return watchlist_json(*created);
                                 }),
                http::status::created);
        });

    // PUT /watchlists/{id} {name}
    router.add(
        http::verb::put, "/api/v1/watchlists/{id}",
        [&lists, as_user](const Request& request,
                          const PathParams& params) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            const auto name = body ? string_field(*body, "name") : std::nullopt;
            const auto id = param_int(params, "id");
            if (!name || !id) {
                co_return bad_input("expected JSON {\"name\"} and a numeric watchlist id");
            }
            co_return respond(co_await as_user(request, [&](const auth::User& user) -> ApiResult {
                const auto renamed = lists.rename(user.id, *id, *name);
                if (!renamed) {
                    return std::unexpected(failure_of(renamed.error()));
                }
                spdlog::info("watchlists: user {} renamed list {} to '{}'", user.id, *id,
                             renamed->name);
                return watchlist_json(*renamed);
            }));
        });

    // DELETE /watchlists/{id}
    router.add(
        http::verb::delete_, "/api/v1/watchlists/{id}",
        [&lists, as_user](const Request& request,
                          const PathParams& params) -> asio::awaitable<Response> {
            const auto id = param_int(params, "id");
            co_return respond(co_await as_user(request, [&](const auth::User& user) -> ApiResult {
                const auto removed =
                    id ? lists.remove(user.id, *id)
                       : std::unexpected(
                             WatchlistError{WatchlistError::Code::NotFound, "watchlist not found"});
                if (!removed) {
                    return std::unexpected(failure_of(removed.error()));
                }
                spdlog::info("watchlists: user {} deleted list {}", user.id, *id);
                return true;
            }));
        });

    // POST /watchlists/{id}/items {instrument_id}
    router.add(
        http::verb::post, "/api/v1/watchlists/{id}/items",
        [&lists, as_user](const Request& request,
                          const PathParams& params) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            const auto instrument = body ? int_field(*body, "instrument_id") : std::nullopt;
            const auto group = body ? int_field(*body, "group_id") : std::nullopt;  // optional
            const auto id = param_int(params, "id");
            if (!instrument || !id) {
                co_return bad_input("expected JSON {\"instrument_id\"} and a numeric watchlist id");
            }
            co_return respond(
                co_await as_user(request,
                                 [&](const auth::User& user) -> ApiResult {
                                     const auto updated = lists.add_item(
                                         user.id, *id, *instrument, auth::system_clock_seconds(),
                                         group.value_or(0));
                                     if (!updated) {
                                         return std::unexpected(failure_of(updated.error()));
                                     }
                                     spdlog::info(
                                         "watchlists: user {} added instrument {} to list {}",
                                         user.id, *instrument, *id);
                                     return watchlist_json(*updated);
                                 }),
                http::status::created);
        });

    // DELETE /watchlists/{id}/items/{instrument_id}
    router.add(
        http::verb::delete_, "/api/v1/watchlists/{id}/items/{instrument_id}",
        [&lists, as_user](const Request& request,
                          const PathParams& params) -> asio::awaitable<Response> {
            const auto id = param_int(params, "id");
            const auto instrument = param_int(params, "instrument_id");
            if (!id || !instrument) {
                co_return bad_input("numeric watchlist and instrument ids expected");
            }
            co_return respond(co_await as_user(request, [&](const auth::User& user) -> ApiResult {
                const auto updated = lists.remove_item(user.id, *id, *instrument);
                if (!updated) {
                    return std::unexpected(failure_of(updated.error()));
                }
                spdlog::info("watchlists: user {} removed instrument {} from list {}", user.id,
                             *instrument, *id);
                return watchlist_json(*updated);
            }));
        });

    // Shared by the group and order routes: run `change` and answer with the whole list.
    auto list_change = [as_user](const Request& request,
                                 auto change) -> asio::awaitable<ApiResult> {
        co_return co_await as_user(request, [&](const auth::User& user) -> ApiResult {
            const auto updated = change(user);
            if (!updated) {
                return std::unexpected(failure_of(updated.error()));
            }
            return watchlist_json(*updated);
        });
    };

    // POST /watchlists/{id}/groups {name}
    router.add(http::verb::post, "/api/v1/watchlists/{id}/groups",
               [&lists, list_change](const Request& request,
                                     const PathParams& params) -> asio::awaitable<Response> {
                   const auto body = json_body(request);
                   const auto name = body ? string_field(*body, "name") : std::nullopt;
                   const auto id = param_int(params, "id");
                   if (!name || !id) {
                       co_return bad_input("expected JSON {\"name\"} and a numeric watchlist id");
                   }
                   co_return respond(co_await list_change(request,
                                                          [&](const auth::User& user) {
                                                              return lists.create_group(user.id,
                                                                                        *id, *name);
                                                          }),
                                     http::status::created);
               });

    // PUT /watchlists/{id}/groups/{group_id} {name}
    router.add(
        http::verb::put, "/api/v1/watchlists/{id}/groups/{group_id}",
        [&lists, list_change](const Request& request,
                              const PathParams& params) -> asio::awaitable<Response> {
            const auto body = json_body(request);
            const auto name = body ? string_field(*body, "name") : std::nullopt;
            const auto id = param_int(params, "id");
            const auto group = param_int(params, "group_id");
            if (!name || !id || !group) {
                co_return bad_input("expected JSON {\"name\"} and numeric watchlist and group ids");
            }
            co_return respond(co_await list_change(request, [&](const auth::User& user) {
                return lists.rename_group(user.id, *id, *group, *name);
            }));
        });

    // DELETE /watchlists/{id}/groups/{group_id} (its instruments become ungrouped)
    router.add(http::verb::delete_, "/api/v1/watchlists/{id}/groups/{group_id}",
               [&lists, list_change](const Request& request,
                                     const PathParams& params) -> asio::awaitable<Response> {
                   const auto id = param_int(params, "id");
                   const auto group = param_int(params, "group_id");
                   if (!id || !group) {
                       co_return bad_input("numeric watchlist and group ids expected");
                   }
                   co_return respond(co_await list_change(request, [&](const auth::User& user) {
                       return lists.remove_group(user.id, *id, *group);
                   }));
               });

    // PUT /watchlists/{id}/order {groups: [ids], items: [{instrument_id, group_id}]}
    router.add(http::verb::put, "/api/v1/watchlists/{id}/order",
               [&lists, list_change](const Request& request,
                                     const PathParams& params) -> asio::awaitable<Response> {
                   const auto body = json_body(request);
                   const auto order = body ? order_body(*body) : std::nullopt;
                   const auto id = param_int(params, "id");
                   if (!order || !id) {
                       co_return bad_input(
                           "expected JSON {\"groups\": [ids], \"items\": [{\"instrument_id\", "
                           "\"group_id\"}]}");
                   }
                   co_return respond(co_await list_change(request, [&](const auth::User& user) {
                       return lists.set_order(user.id, *id, *order);
                   }));
               });
}

}  // namespace mda::gateway
