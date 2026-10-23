#pragma once

// Per-user watchlists (docs/api/rest.md, "Watchlists").
//
// Rules: lists belong to one user (others get NotFound, not "forbidden", so existence isn't
// revealed); names unique per user; limits on lists, groups and items; only instruments
// listed today can be added. A user with no lists gets a default one on first read.
//
// A list may have named groups (like Kite's "Indices", "Options"). Each item belongs to
// one group or to none ("ungrouped", shown first). Order is one sequence of groups plus one
// sequence of items; set_order() replaces both at once (drag and drop, sorting).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mda/catalog/catalog.hpp>
#include <mda/storage/database.hpp>

namespace mda::watchlists {

struct WatchlistError {
    enum class Code {
        Input,     ///< bad name, unknown instrument
        NotFound,  ///< no such list / group for this user, or item not in the list
        Conflict,  ///< duplicate name or instrument already in the list
        Limit,     ///< too many lists or items
        Internal,  ///< database failure
    };
    Code code;
    std::string message;
};

struct Item {
    catalog::InstrumentInfo instrument;
    std::int64_t added_at = 0;
    std::int64_t group_id = 0;  ///< 0 = ungrouped
};

struct Group {
    std::int64_t id = 0;
    std::string name;
};

struct Watchlist {
    std::int64_t id = 0;
    std::string name;
    std::int64_t created_at = 0;
    std::vector<Group> groups;  ///< in display order
    std::vector<Item> items;    ///< in display order (across groups)
};

/// A complete new arrangement for set_order(): every group once, every item once.
struct Order {
    std::vector<std::int64_t> groups;
    std::vector<std::pair<std::int64_t, std::int64_t>> items;  ///< (instrument_id, group_id or 0)
};

struct Limits {
    std::size_t max_lists = 10;
    std::size_t max_groups = 20;  ///< per list
    std::size_t max_items = 250;  ///< per list (Kite's limit)
    std::size_t max_name_length = 40;
};

inline constexpr std::string_view kDefaultName = "My watchlist";

class WatchlistService {
public:
    explicit WatchlistService(storage::Database& db, Limits limits = {})
        : db_(&db), limits_(limits) {}

    /// The user's lists with items; creates the default list if the user has none.
    [[nodiscard]] std::expected<std::vector<Watchlist>, WatchlistError> list(std::int64_t user_id,
                                                                             std::int64_t now);
    [[nodiscard]] std::expected<Watchlist, WatchlistError> create(std::int64_t user_id,
                                                                  std::string_view name,
                                                                  std::int64_t now);
    [[nodiscard]] std::expected<Watchlist, WatchlistError> rename(std::int64_t user_id,
                                                                  std::int64_t list_id,
                                                                  std::string_view name);
    [[nodiscard]] std::expected<void, WatchlistError> remove(std::int64_t user_id,
                                                             std::int64_t list_id);
    /// Adds at the end of `group_id`'s items (0 = ungrouped).
    [[nodiscard]] std::expected<Watchlist, WatchlistError> add_item(std::int64_t user_id,
                                                                    std::int64_t list_id,
                                                                    std::int64_t instrument_id,
                                                                    std::int64_t now,
                                                                    std::int64_t group_id = 0);
    [[nodiscard]] std::expected<Watchlist, WatchlistError> remove_item(std::int64_t user_id,
                                                                       std::int64_t list_id,
                                                                       std::int64_t instrument_id);

    [[nodiscard]] std::expected<Watchlist, WatchlistError> create_group(std::int64_t user_id,
                                                                        std::int64_t list_id,
                                                                        std::string_view name);
    [[nodiscard]] std::expected<Watchlist, WatchlistError> rename_group(std::int64_t user_id,
                                                                        std::int64_t list_id,
                                                                        std::int64_t group_id,
                                                                        std::string_view name);
    /// Deletes the group; its instruments stay in the list, ungrouped.
    [[nodiscard]] std::expected<Watchlist, WatchlistError> remove_group(std::int64_t user_id,
                                                                        std::int64_t list_id,
                                                                        std::int64_t group_id);
    /// Replaces the order of groups and items (and which group each item is in). The
    /// order must name exactly the list's current groups and items.
    [[nodiscard]] std::expected<Watchlist, WatchlistError> set_order(std::int64_t user_id,
                                                                     std::int64_t list_id,
                                                                     const Order& order);

    /// Every instrument on any user's watchlist (what the feed must subscribe).
    [[nodiscard]] std::expected<std::vector<std::int64_t>, WatchlistError> all_instrument_ids();

private:
    [[nodiscard]] std::expected<Watchlist, WatchlistError> load(std::int64_t user_id,
                                                                std::int64_t list_id);
    [[nodiscard]] std::expected<std::string, WatchlistError> clean_name(
        std::string_view name) const;

    storage::Database* db_;
    Limits limits_;
};

}  // namespace mda::watchlists
