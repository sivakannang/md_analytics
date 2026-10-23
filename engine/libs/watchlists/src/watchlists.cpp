#include <algorithm>
#include <set>

#include <mda/watchlists/watchlists.hpp>

namespace mda::watchlists {
namespace {

using Code = WatchlistError::Code;
using storage::StepResult;

WatchlistError error(Code code, std::string message) {
    return WatchlistError{.code = code, .message = std::move(message)};
}

WatchlistError internal(const storage::DbError& db_error) {
    return error(Code::Internal, db_error.message);
}

/// Runs `sql` with `bind` and returns the first column of the first row (or 0 if none).
template <typename Bind>
std::expected<std::int64_t, WatchlistError> scalar(storage::Database& db, std::string_view sql,
                                                   Bind bind) {
    auto statement = db.prepare(sql);
    if (!statement) {
        return std::unexpected(internal(statement.error()));
    }
    bind(*statement);
    const auto row = statement->step();
    if (!row) {
        return std::unexpected(internal(row.error()));
    }
    return *row == StepResult::Row ? statement->column_int64(0) : 0;
}

}  // namespace

std::expected<std::string, WatchlistError> WatchlistService::clean_name(
    std::string_view name) const {
    while (!name.empty() && name.front() == ' ') {
        name.remove_prefix(1);
    }
    while (!name.empty() && name.back() == ' ') {
        name.remove_suffix(1);
    }
    const auto characters = std::ranges::count_if(
        name, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
    if (name.empty() || static_cast<std::size_t>(characters) > limits_.max_name_length ||
        std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
        return std::unexpected(
            error(Code::Input,
                  "name must be 1-" + std::to_string(limits_.max_name_length) + " characters"));
    }
    return std::string(name);
}

std::expected<Watchlist, WatchlistError> WatchlistService::load(std::int64_t user_id,
                                                                std::int64_t list_id) {
    auto header =
        db_->prepare("SELECT id, name, created_at FROM watchlists WHERE id = ?1 AND user_id = ?2");
    if (!header) {
        return std::unexpected(internal(header.error()));
    }
    header->bind(1, list_id).bind(2, user_id);
    const auto found = header->step();
    if (!found) {
        return std::unexpected(internal(found.error()));
    }
    if (*found == StepResult::Done) {
        return std::unexpected(error(Code::NotFound, "watchlist not found"));
    }
    Watchlist watchlist{.id = header->column_int64(0),
                        .name = header->column_text(1),
                        .created_at = header->column_int64(2)};

    auto groups = db_->prepare(
        "SELECT id, name FROM watchlist_groups WHERE watchlist_id = ?1 ORDER BY position, id");
    if (!groups) {
        return std::unexpected(internal(groups.error()));
    }
    groups->bind(1, list_id);
    while (true) {
        const auto row = groups->step();
        if (!row) {
            return std::unexpected(internal(row.error()));
        }
        if (*row == StepResult::Done) {
            break;
        }
        watchlist.groups.push_back({.id = groups->column_int64(0), .name = groups->column_text(1)});
    }

    auto items = db_->prepare("SELECT " + std::string(catalog::kInfoColumns) +
                              ", w.added_at, COALESCE(w.group_id, 0) FROM watchlist_items w JOIN "
                              "instruments i ON i.id = w.instrument_id WHERE w.watchlist_id = ?1 "
                              "ORDER BY w.position");
    if (!items) {
        return std::unexpected(internal(items.error()));
    }
    items->bind(1, list_id);
    while (true) {
        const auto row = items->step();
        if (!row) {
            return std::unexpected(internal(row.error()));
        }
        if (*row == StepResult::Done) {
            return watchlist;
        }
        watchlist.items.push_back({.instrument = catalog::read_info(*items),
                                   .added_at = items->column_int64(14),
                                   .group_id = items->column_int64(15)});
    }
}

std::expected<std::vector<Watchlist>, WatchlistError> WatchlistService::list(std::int64_t user_id,
                                                                             std::int64_t now) {
    std::vector<std::int64_t> ids;
    {
        auto statement =
            db_->prepare("SELECT id FROM watchlists WHERE user_id = ?1 ORDER BY position, id");
        if (!statement) {
            return std::unexpected(internal(statement.error()));
        }
        statement->bind(1, user_id);
        while (true) {
            const auto row = statement->step();
            if (!row) {
                return std::unexpected(internal(row.error()));
            }
            if (*row == StepResult::Done) {
                break;
            }
            ids.push_back(statement->column_int64(0));
        }
    }
    std::vector<Watchlist> lists;
    if (ids.empty()) {
        auto created = create(user_id, kDefaultName, now);
        if (!created) {
            return std::unexpected(created.error());
        }
        lists.push_back(std::move(*created));
        return lists;
    }
    for (const std::int64_t id : ids) {
        auto loaded = load(user_id, id);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        lists.push_back(std::move(*loaded));
    }
    return lists;
}

std::expected<Watchlist, WatchlistError> WatchlistService::create(std::int64_t user_id,
                                                                  std::string_view name,
                                                                  std::int64_t now) {
    const auto clean = clean_name(name);
    if (!clean) {
        return std::unexpected(clean.error());
    }
    const auto count = scalar(*db_, "SELECT COUNT(*) FROM watchlists WHERE user_id = ?1",
                              [&](storage::Statement& s) { s.bind(1, user_id); });
    if (!count) {
        return std::unexpected(count.error());
    }
    if (static_cast<std::size_t>(*count) >= limits_.max_lists) {
        return std::unexpected(
            error(Code::Limit, "at most " + std::to_string(limits_.max_lists) + " watchlists"));
    }
    auto insert = db_->prepare(
        "INSERT INTO watchlists (user_id, name, position, created_at) VALUES (?1, ?2, "
        "(SELECT COALESCE(MAX(position), 0) + 1 FROM watchlists WHERE user_id = ?1), ?3)");
    if (!insert) {
        return std::unexpected(internal(insert.error()));
    }
    if (auto done = insert->bind(1, user_id).bind(2, std::string_view(*clean)).bind(3, now).run();
        !done) {
        return std::unexpected(
            done.error().is_constraint()
                ? error(Code::Conflict, "a watchlist with this name already exists")
                : internal(done.error()));
    }
    return load(user_id, db_->last_insert_id());
}

std::expected<Watchlist, WatchlistError> WatchlistService::rename(std::int64_t user_id,
                                                                  std::int64_t list_id,
                                                                  std::string_view name) {
    const auto clean = clean_name(name);
    if (!clean) {
        return std::unexpected(clean.error());
    }
    auto update = db_->prepare("UPDATE watchlists SET name = ?1 WHERE id = ?2 AND user_id = ?3");
    if (!update) {
        return std::unexpected(internal(update.error()));
    }
    if (auto done =
            update->bind(1, std::string_view(*clean)).bind(2, list_id).bind(3, user_id).run();
        !done) {
        return std::unexpected(
            done.error().is_constraint()
                ? error(Code::Conflict, "a watchlist with this name already exists")
                : internal(done.error()));
    }
    if (db_->changes() == 0) {
        return std::unexpected(error(Code::NotFound, "watchlist not found"));
    }
    return load(user_id, list_id);
}

std::expected<void, WatchlistError> WatchlistService::remove(std::int64_t user_id,
                                                             std::int64_t list_id) {
    auto statement = db_->prepare("DELETE FROM watchlists WHERE id = ?1 AND user_id = ?2");
    if (!statement) {
        return std::unexpected(internal(statement.error()));
    }
    if (auto done = statement->bind(1, list_id).bind(2, user_id).run(); !done) {
        return std::unexpected(internal(done.error()));
    }
    if (db_->changes() == 0) {
        return std::unexpected(error(Code::NotFound, "watchlist not found"));
    }
    return {};
}

std::expected<Watchlist, WatchlistError> WatchlistService::add_item(std::int64_t user_id,
                                                                    std::int64_t list_id,
                                                                    std::int64_t instrument_id,
                                                                    std::int64_t now,
                                                                    std::int64_t group_id) {
    auto watchlist = load(user_id, list_id);  // also checks ownership
    if (!watchlist) {
        return std::unexpected(watchlist.error());
    }
    if (group_id != 0 &&
        std::ranges::none_of(watchlist->groups, [&](const Group& g) { return g.id == group_id; })) {
        return std::unexpected(error(Code::NotFound, "group not found"));
    }
    if (watchlist->items.size() >= limits_.max_items) {
        return std::unexpected(error(Code::Limit, "at most " + std::to_string(limits_.max_items) +
                                                      " instruments per watchlist"));
    }
    const auto listed = scalar(*db_, "SELECT listed FROM instruments WHERE id = ?1",
                               [&](storage::Statement& s) { s.bind(1, instrument_id); });
    if (!listed) {
        return std::unexpected(listed.error());
    }
    if (*listed != 1) {
        return std::unexpected(error(Code::Input, "instrument not found or not listed today"));
    }
    // Appending puts it last in its group: items show grouped, in position order.
    auto insert = db_->prepare(
        "INSERT INTO watchlist_items (watchlist_id, instrument_id, position, added_at, group_id) "
        "VALUES (?1, ?2, "
        "(SELECT COALESCE(MAX(position), 0) + 1 FROM watchlist_items WHERE watchlist_id = ?1), "
        "?3, NULLIF(?4, 0))");
    if (!insert) {
        return std::unexpected(internal(insert.error()));
    }
    if (auto done =
            insert->bind(1, list_id).bind(2, instrument_id).bind(3, now).bind(4, group_id).run();
        !done) {
        return std::unexpected(
            done.error().is_constraint()
                ? error(Code::Conflict, "instrument is already in this watchlist")
                : internal(done.error()));
    }
    return load(user_id, list_id);
}

std::expected<Watchlist, WatchlistError> WatchlistService::remove_item(std::int64_t user_id,
                                                                       std::int64_t list_id,
                                                                       std::int64_t instrument_id) {
    if (auto owned = load(user_id, list_id); !owned) {
        return std::unexpected(owned.error());
    }
    auto statement =
        db_->prepare("DELETE FROM watchlist_items WHERE watchlist_id = ?1 AND instrument_id = ?2");
    if (!statement) {
        return std::unexpected(internal(statement.error()));
    }
    if (auto done = statement->bind(1, list_id).bind(2, instrument_id).run(); !done) {
        return std::unexpected(internal(done.error()));
    }
    if (db_->changes() == 0) {
        return std::unexpected(error(Code::NotFound, "instrument is not in this watchlist"));
    }
    return load(user_id, list_id);
}

std::expected<Watchlist, WatchlistError> WatchlistService::create_group(std::int64_t user_id,
                                                                        std::int64_t list_id,
                                                                        std::string_view name) {
    const auto clean = clean_name(name);
    if (!clean) {
        return std::unexpected(clean.error());
    }
    auto watchlist = load(user_id, list_id);
    if (!watchlist) {
        return std::unexpected(watchlist.error());
    }
    if (watchlist->groups.size() >= limits_.max_groups) {
        return std::unexpected(error(Code::Limit, "at most " + std::to_string(limits_.max_groups) +
                                                      " groups per watchlist"));
    }
    auto insert = db_->prepare(
        "INSERT INTO watchlist_groups (watchlist_id, name, position) VALUES (?1, ?2, "
        "(SELECT COALESCE(MAX(position), 0) + 1 FROM watchlist_groups WHERE watchlist_id = ?1))");
    if (!insert) {
        return std::unexpected(internal(insert.error()));
    }
    if (auto done = insert->bind(1, list_id).bind(2, std::string_view(*clean)).run(); !done) {
        return std::unexpected(done.error().is_constraint()
                                   ? error(Code::Conflict, "a group with this name already exists")
                                   : internal(done.error()));
    }
    return load(user_id, list_id);
}

std::expected<Watchlist, WatchlistError> WatchlistService::rename_group(std::int64_t user_id,
                                                                        std::int64_t list_id,
                                                                        std::int64_t group_id,
                                                                        std::string_view name) {
    const auto clean = clean_name(name);
    if (!clean) {
        return std::unexpected(clean.error());
    }
    if (auto owned = load(user_id, list_id); !owned) {
        return std::unexpected(owned.error());
    }
    auto update =
        db_->prepare("UPDATE watchlist_groups SET name = ?1 WHERE id = ?2 AND watchlist_id = ?3");
    if (!update) {
        return std::unexpected(internal(update.error()));
    }
    if (auto done =
            update->bind(1, std::string_view(*clean)).bind(2, group_id).bind(3, list_id).run();
        !done) {
        return std::unexpected(done.error().is_constraint()
                                   ? error(Code::Conflict, "a group with this name already exists")
                                   : internal(done.error()));
    }
    if (db_->changes() == 0) {
        return std::unexpected(error(Code::NotFound, "group not found"));
    }
    return load(user_id, list_id);
}

std::expected<Watchlist, WatchlistError> WatchlistService::remove_group(std::int64_t user_id,
                                                                        std::int64_t list_id,
                                                                        std::int64_t group_id) {
    if (auto owned = load(user_id, list_id); !owned) {
        return std::unexpected(owned.error());
    }
    auto statement =
        db_->prepare("DELETE FROM watchlist_groups WHERE id = ?1 AND watchlist_id = ?2");
    if (!statement) {
        return std::unexpected(internal(statement.error()));
    }
    if (auto done = statement->bind(1, group_id).bind(2, list_id).run(); !done) {
        return std::unexpected(internal(done.error()));
    }
    if (db_->changes() == 0) {
        return std::unexpected(error(Code::NotFound, "group not found"));
    }
    return load(user_id, list_id);  // its items are now ungrouped (ON DELETE SET NULL)
}

std::expected<Watchlist, WatchlistError> WatchlistService::set_order(std::int64_t user_id,
                                                                     std::int64_t list_id,
                                                                     const Order& order) {
    auto watchlist = load(user_id, list_id);
    if (!watchlist) {
        return std::unexpected(watchlist.error());
    }
    // The order must be a permutation of what is there: no additions or removals here.
    std::set<std::int64_t> groups;
    for (const auto& g : watchlist->groups) {
        groups.insert(g.id);
    }
    std::set<std::int64_t> items;
    for (const auto& i : watchlist->items) {
        items.insert(i.instrument.id);
    }
    const std::set<std::int64_t> new_groups(order.groups.begin(), order.groups.end());
    std::set<std::int64_t> new_items;
    for (const auto& [instrument, group] : order.items) {
        new_items.insert(instrument);
        if (group != 0 && !groups.contains(group)) {
            return std::unexpected(error(Code::Input, "order names an unknown group"));
        }
    }
    if (new_groups != groups || order.groups.size() != groups.size() || new_items != items ||
        order.items.size() != items.size()) {
        return std::unexpected(error(
            Code::Conflict, "order doesn't match the watchlist (it changed); reload and retry"));
    }

    auto transaction = storage::Transaction::begin(*db_);
    if (!transaction) {
        return std::unexpected(internal(transaction.error()));
    }
    auto group_update = db_->prepare(
        "UPDATE watchlist_groups SET position = ?1 WHERE id = ?2 AND watchlist_id = ?3");
    auto item_update = db_->prepare(
        "UPDATE watchlist_items SET position = ?1, group_id = NULLIF(?2, 0) WHERE watchlist_id = "
        "?3 "
        "AND instrument_id = ?4");
    if (!group_update || !item_update) {
        return std::unexpected(
            internal(!group_update ? group_update.error() : item_update.error()));
    }
    for (std::size_t i = 0; i < order.groups.size(); ++i) {
        group_update->reset();
        if (auto done = group_update->bind(1, static_cast<std::int64_t>(i + 1))
                            .bind(2, order.groups[i])
                            .bind(3, list_id)
                            .run();
            !done) {
            return std::unexpected(internal(done.error()));
        }
    }
    for (std::size_t i = 0; i < order.items.size(); ++i) {
        item_update->reset();
        if (auto done = item_update->bind(1, static_cast<std::int64_t>(i + 1))
                            .bind(2, order.items[i].second)
                            .bind(3, list_id)
                            .bind(4, order.items[i].first)
                            .run();
            !done) {
            return std::unexpected(internal(done.error()));
        }
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(internal(committed.error()));
    }
    return load(user_id, list_id);
}

std::expected<std::vector<std::int64_t>, WatchlistError> WatchlistService::all_instrument_ids() {
    auto statement =
        db_->prepare("SELECT DISTINCT instrument_id FROM watchlist_items ORDER BY instrument_id");
    if (!statement) {
        return std::unexpected(internal(statement.error()));
    }
    std::vector<std::int64_t> ids;
    while (true) {
        const auto row = statement->step();
        if (!row) {
            return std::unexpected(internal(row.error()));
        }
        if (*row == StepResult::Done) {
            return ids;
        }
        ids.push_back(statement->column_int64(0));
    }
}

}  // namespace mda::watchlists
