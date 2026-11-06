#pragma once

// data/paper.db (ADR-0029, docs/design/paper-trading.md §11): paper accounts, orders,
// trades, positions, the ledger and an event log. mda-broker is its only writer, one
// transaction per input; the gateway and mda-feedd read it. Enums are stored as their
// numeric values, prices as core::Price raw values and money in paise.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mda/oms/account.hpp>
#include <mda/oms/order.hpp>
#include <mda/oms/paper_venue.hpp>
#include <mda/storage/database.hpp>

namespace mda::broker {

using storage::DbError;

struct AccountRow {
    std::int64_t id = 0;
    std::int64_t user_id = 0;
    std::int64_t created_at = 0;  ///< Unix ms
    oms::Paise opening_cash = 0;
    bool active = true;
    bool kill_switch = false;
};

struct OrderRow {
    oms::Order::Saved order;
    std::int64_t user_id = 0;
    oms::Paise margin = 0;  ///< blocked for the order's whole quantity when accepted
    oms::PaperVenue::State venue;
};

struct TradeRow {
    std::int64_t order_id = 0;
    std::int64_t account_id = 0;
    core::InstrumentId instrument;
    oms::Side side = oms::Side::Buy;
    oms::Product product = oms::Product::Mis;
    std::int64_t quantity = 0;
    core::Price price;
    oms::ChargeBreakdown charges;
    oms::Paise realised = 0;
    std::int64_t at = 0;
    oms::Paise entry_realised = 0;  ///< the P&L from the entry price, before any daily mark
};

class PaperStore {
public:
    [[nodiscard]] static std::expected<PaperStore, DbError> open(const std::filesystem::path& path);
    [[nodiscard]] static std::expected<PaperStore, DbError> open_in_memory();

    /// Runs `work` in one transaction; any failure rolls it all back.
    [[nodiscard]] std::expected<void, DbError> in_transaction(
        const std::function<std::expected<void, DbError>()>& work);

    [[nodiscard]] std::expected<void, DbError> upsert_account(const AccountRow& account);
    [[nodiscard]] std::expected<void, DbError> upsert_order(const OrderRow& order);
    [[nodiscard]] std::expected<void, DbError> insert_trade(const TradeRow& trade);
    [[nodiscard]] std::expected<void, DbError> upsert_position(std::int64_t account_id,
                                                               const oms::PositionKey& key,
                                                               const oms::Position& position,
                                                               std::int64_t at);
    [[nodiscard]] std::expected<void, DbError> insert_ledger(std::int64_t account_id,
                                                             const oms::LedgerEntry& entry);
    [[nodiscard]] std::expected<void, DbError> insert_event(std::int64_t at, std::string_view kind,
                                                            std::string_view payload);

    /// Active accounts (one per user).
    [[nodiscard]] std::expected<std::vector<AccountRow>, DbError> accounts();
    /// Accounts with orders or ledger entries but no accounts row (lost on 2026-10-27, cause
    /// unknown), rebuilt from them: the user of their orders, the first ledger time and the
    /// latest opening entry.
    [[nodiscard]] std::expected<std::vector<AccountRow>, DbError> lost_accounts();
    [[nodiscard]] std::expected<std::vector<oms::LedgerEntry>, DbError> ledger(
        std::int64_t account_id);
    [[nodiscard]] std::expected<std::map<oms::PositionKey, oms::Position>, DbError> positions(
        std::int64_t account_id);
    /// Orders still open or waiting for their trigger, oldest first.
    [[nodiscard]] std::expected<std::vector<OrderRow>, DbError> live_orders();
    [[nodiscard]] std::expected<std::optional<OrderRow>, DbError> order_by_client(
        std::int64_t account_id, std::uint64_t client_order_id);
    [[nodiscard]] std::expected<std::int64_t, DbError> max_order_id();
    /// The user's active account, if they have one.
    [[nodiscard]] std::expected<std::optional<AccountRow>, DbError> account_for_user(
        std::int64_t user_id);
    /// One account's orders created in [from, to) (Unix ms), oldest first.
    [[nodiscard]] std::expected<std::vector<OrderRow>, DbError> orders(std::int64_t account_id,
                                                                       std::int64_t from,
                                                                       std::int64_t to);
    /// One account's trades in [from, to) (Unix ms), oldest first.
    [[nodiscard]] std::expected<std::vector<TradeRow>, DbError> trades(std::int64_t account_id,
                                                                       std::int64_t from,
                                                                       std::int64_t to);

    /// One account's trades in [from, to) of orders tagged `tag` (an auto-trader run), oldest
    /// first.
    [[nodiscard]] std::expected<std::vector<TradeRow>, DbError> tagged_trades(
        std::int64_t account_id, std::string_view tag, std::int64_t from, std::int64_t to);

    /// One account's fills in [from, to) of orders tagged "auto:…", totalled per tag.
    struct TagTotals {
        std::string tag;
        std::int64_t fills = 0;
        oms::Paise realised = 0;
        oms::Paise charges = 0;
    };
    [[nodiscard]] std::expected<std::vector<TagTotals>, DbError> auto_totals(
        std::int64_t account_id, std::int64_t from, std::int64_t to);

    /// Instruments with a live order or an open position (mda-feedd subscribes them).
    [[nodiscard]] std::expected<std::vector<std::int64_t>, DbError> active_instruments();

    /// Every table as text, rows in key order (tests compare two stores with it).
    [[nodiscard]] std::expected<std::string, DbError> dump();

    [[nodiscard]] storage::Database& database() noexcept { return db_; }

private:
    explicit PaperStore(storage::Database db) : db_(std::move(db)) {}
    [[nodiscard]] static std::expected<PaperStore, DbError> prepare(storage::Database db);

    storage::Database db_;
};

}  // namespace mda::broker
