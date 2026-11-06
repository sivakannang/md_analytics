#include <array>
#include <system_error>
#include <utility>

#include <mda/broker/paper_store.hpp>

namespace mda::broker {
namespace {

using storage::StepResult;

constexpr std::array<std::string_view, 3> kMigrations = {
    R"sql(
CREATE TABLE accounts (
    id           INTEGER PRIMARY KEY,
    user_id      INTEGER NOT NULL,
    created_at   INTEGER NOT NULL,          -- Unix ms
    opening_cash INTEGER NOT NULL,          -- paise
    active       INTEGER NOT NULL,
    kill_switch  INTEGER NOT NULL
);
CREATE INDEX accounts_by_user ON accounts (user_id, active);

CREATE TABLE orders (
    id               INTEGER PRIMARY KEY,
    account_id       INTEGER NOT NULL,
    user_id          INTEGER NOT NULL,
    client_order_id  INTEGER NOT NULL,
    source           INTEGER NOT NULL,      -- oms::Source
    instrument_id    INTEGER NOT NULL,
    side             INTEGER NOT NULL,      -- oms::Side
    type             INTEGER NOT NULL,      -- as placed (oms::OrderType)
    current_type     INTEGER NOT NULL,      -- now: an SL becomes LIMIT once triggered
    product          INTEGER NOT NULL,
    quantity         INTEGER NOT NULL,
    price_raw        INTEGER NOT NULL,
    trigger_raw      INTEGER NOT NULL,
    status           INTEGER NOT NULL,      -- oms::OrderStatus
    filled           INTEGER NOT NULL,
    filled_value_raw INTEGER NOT NULL,
    brokerage        INTEGER NOT NULL,
    charges          INTEGER NOT NULL,
    margin           INTEGER NOT NULL,
    reason           TEXT NOT NULL,
    tag              TEXT NOT NULL,
    created_at       INTEGER NOT NULL,
    updated_at       INTEGER NOT NULL,
    eligible_at      INTEGER NOT NULL,      -- the paper venue's state of the order
    resting          INTEGER NOT NULL,
    UNIQUE (account_id, client_order_id)
);
CREATE INDEX orders_live ON orders (status);

CREATE TABLE trades (
    id            INTEGER PRIMARY KEY,
    order_id      INTEGER NOT NULL,
    account_id    INTEGER NOT NULL,
    instrument_id INTEGER NOT NULL,
    side          INTEGER NOT NULL,
    product       INTEGER NOT NULL,
    quantity      INTEGER NOT NULL,
    price_raw     INTEGER NOT NULL,
    brokerage     INTEGER NOT NULL,
    stt           INTEGER NOT NULL,
    exchange      INTEGER NOT NULL,
    sebi          INTEGER NOT NULL,
    stamp         INTEGER NOT NULL,
    gst           INTEGER NOT NULL,
    realised      INTEGER NOT NULL,
    at            INTEGER NOT NULL
);
CREATE INDEX trades_by_account ON trades (account_id, at);

CREATE TABLE positions (
    account_id    INTEGER NOT NULL,
    instrument_id INTEGER NOT NULL,
    product       INTEGER NOT NULL,
    quantity      INTEGER NOT NULL,
    cost_raw      INTEGER NOT NULL,
    realised      INTEGER NOT NULL,
    updated_at    INTEGER NOT NULL,
    PRIMARY KEY (account_id, instrument_id, product)
) WITHOUT ROWID;

CREATE TABLE ledger (
    id            INTEGER PRIMARY KEY,
    account_id    INTEGER NOT NULL,
    at            INTEGER NOT NULL,
    kind          INTEGER NOT NULL,         -- oms::LedgerKind
    amount        INTEGER NOT NULL,         -- paise, credit positive
    order_id      INTEGER NOT NULL,
    instrument_id INTEGER NOT NULL
);
CREATE INDEX ledger_by_account ON ledger (account_id, id);

CREATE TABLE events (
    seq     INTEGER PRIMARY KEY,
    at      INTEGER NOT NULL,
    kind    TEXT NOT NULL,                  -- request, report, …
    payload TEXT NOT NULL
);
)sql",
    // Entry prices. The daily mark moved each open position's cost by
    // its MTM (paise × 100 = raw), so undoing the marks gives the cost
    // at fill prices. Exact when an instrument's marks all belong to
    // one open position, true of every position at this migration.
    R"sql(
ALTER TABLE positions ADD COLUMN entry_cost_raw INTEGER NOT NULL DEFAULT 0;
UPDATE positions SET entry_cost_raw = cost_raw - 100 * (
    SELECT COALESCE(SUM(amount), 0) FROM ledger
    WHERE ledger.account_id = positions.account_id
      AND ledger.instrument_id = positions.instrument_id
      AND ledger.kind = 5)
WHERE quantity != 0;
)sql",
    // Each closing fill's P&L from the entry price. No fill so far
    // closed a marked position, so it's the realised P&L.
    R"sql(
ALTER TABLE trades ADD COLUMN entry_realised INTEGER NOT NULL DEFAULT 0;
UPDATE trades SET entry_realised = realised;
)sql"};

constexpr std::string_view kOrderColumns =
    "id, account_id, user_id, client_order_id, source, instrument_id, side, type, current_type, "
    "product, "
    "quantity, price_raw, trigger_raw, status, filled, filled_value_raw, brokerage, charges, "
    "margin, reason, "
    "tag, created_at, updated_at, eligible_at, resting";

template <typename E>
std::int64_t num(E value) {
    return static_cast<std::int64_t>(value);
}

template <typename E>
E as(const storage::Statement& s, int column) {
    return static_cast<E>(s.column_int64(column));
}

OrderRow read_order(const storage::Statement& s) {
    OrderRow row;
    auto& o = row.order;
    o.id = s.column_int64(0);
    o.spec.account = s.column_int64(1);
    row.user_id = s.column_int64(2);
    o.spec.client_order_id = static_cast<std::uint64_t>(s.column_int64(3));
    o.spec.source = as<oms::Source>(s, 4);
    o.spec.instrument = core::InstrumentId(static_cast<std::uint32_t>(s.column_int64(5)));
    o.spec.side = as<oms::Side>(s, 6);
    o.spec.type = as<oms::OrderType>(s, 7);
    o.type = as<oms::OrderType>(s, 8);
    o.spec.product = as<oms::Product>(s, 9);
    o.spec.quantity = s.column_int64(10);
    o.spec.price = core::Price::from_raw(s.column_int64(11));
    o.spec.trigger = core::Price::from_raw(s.column_int64(12));
    o.status = as<oms::OrderStatus>(s, 13);
    o.filled = s.column_int64(14);
    o.filled_value_raw = s.column_int64(15);
    o.brokerage = s.column_int64(16);
    o.charges = s.column_int64(17);
    row.margin = s.column_int64(18);
    o.reason = s.column_text(19);
    o.spec.tag = s.column_text(20);
    o.created_at = s.column_int64(21);
    o.updated_at = s.column_int64(22);
    row.venue.eligible_at = s.column_int64(23);
    row.venue.resting = s.column_int64(24) != 0;
    return row;
}

TradeRow read_trade(const storage::Statement& s) {
    return TradeRow{.order_id = s.column_int64(0),
                    .account_id = s.column_int64(1),
                    .instrument = core::InstrumentId(static_cast<std::uint32_t>(s.column_int64(2))),
                    .side = as<oms::Side>(s, 3),
                    .product = as<oms::Product>(s, 4),
                    .quantity = s.column_int64(5),
                    .price = core::Price::from_raw(s.column_int64(6)),
                    .charges = {.brokerage = s.column_int64(7),
                                .stt = s.column_int64(8),
                                .exchange = s.column_int64(9),
                                .sebi = s.column_int64(10),
                                .stamp = s.column_int64(11),
                                .gst = s.column_int64(12)},
                    .realised = s.column_int64(13),
                    .at = s.column_int64(14),
                    .entry_realised = s.column_int64(15)};
}

template <typename T, typename Read>
std::expected<std::vector<T>, DbError> collect(storage::Statement& statement, Read read) {
    std::vector<T> rows;
    while (true) {
        const auto step = statement.step();
        if (!step) {
            return std::unexpected(step.error());
        }
        if (*step == StepResult::Done) {
            return rows;
        }
        rows.push_back(read(statement));
    }
}

}  // namespace

std::expected<PaperStore, DbError> PaperStore::prepare(storage::Database db) {
    if (auto migrated = db.migrate(kMigrations); !migrated) {
        return std::unexpected(migrated.error());
    }
    return PaperStore(std::move(db));
}

std::expected<PaperStore, DbError> PaperStore::open(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
    auto db = storage::Database::open(path);
    if (!db) {
        return std::unexpected(db.error());
    }
    return prepare(std::move(*db));
}

std::expected<PaperStore, DbError> PaperStore::open_in_memory() {
    auto db = storage::Database::open_in_memory();
    if (!db) {
        return std::unexpected(db.error());
    }
    return prepare(std::move(*db));
}

std::expected<void, DbError> PaperStore::in_transaction(
    const std::function<std::expected<void, DbError>()>& work) {
    auto transaction = storage::Transaction::begin(db_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    if (auto done = work(); !done) {
        return done;  // the transaction rolls back when it goes out of scope
    }
    return transaction->commit();
}

std::expected<void, DbError> PaperStore::upsert_account(const AccountRow& a) {
    auto statement = db_.prepare(
        "INSERT OR REPLACE INTO accounts (id, user_id, created_at, opening_cash, active, "
        "kill_switch) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, a.id)
        .bind(2, a.user_id)
        .bind(3, a.created_at)
        .bind(4, a.opening_cash)
        .bind(5, std::int64_t{a.active ? 1 : 0})
        .bind(6, std::int64_t{a.kill_switch ? 1 : 0})
        .run();
}

std::expected<void, DbError> PaperStore::upsert_order(const OrderRow& row) {
    auto statement =
        db_.prepare("INSERT OR REPLACE INTO orders (" + std::string(kOrderColumns) +
                    ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, "
                    "?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    const auto& o = row.order;
    return statement->bind(1, o.id)
        .bind(2, o.spec.account)
        .bind(3, row.user_id)
        .bind(4, static_cast<std::int64_t>(o.spec.client_order_id))
        .bind(5, num(o.spec.source))
        .bind(6, std::int64_t{o.spec.instrument.value()})
        .bind(7, num(o.spec.side))
        .bind(8, num(o.spec.type))
        .bind(9, num(o.type))
        .bind(10, num(o.spec.product))
        .bind(11, o.spec.quantity)
        .bind(12, o.spec.price.raw())
        .bind(13, o.spec.trigger.raw())
        .bind(14, num(o.status))
        .bind(15, o.filled)
        .bind(16, o.filled_value_raw)
        .bind(17, o.brokerage)
        .bind(18, o.charges)
        .bind(19, row.margin)
        .bind(20, std::string_view(o.reason))
        .bind(21, std::string_view(o.spec.tag))
        .bind(22, o.created_at)
        .bind(23, o.updated_at)
        .bind(24, row.venue.eligible_at)
        .bind(25, std::int64_t{row.venue.resting ? 1 : 0})
        .run();
}

std::expected<void, DbError> PaperStore::insert_trade(const TradeRow& t) {
    auto statement = db_.prepare(
        "INSERT INTO trades (order_id, account_id, instrument_id, side, product, quantity, "
        "price_raw, "
        "brokerage, stt, exchange, sebi, stamp, gst, realised, at, entry_realised) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, t.order_id)
        .bind(2, t.account_id)
        .bind(3, std::int64_t{t.instrument.value()})
        .bind(4, num(t.side))
        .bind(5, num(t.product))
        .bind(6, t.quantity)
        .bind(7, t.price.raw())
        .bind(8, t.charges.brokerage)
        .bind(9, t.charges.stt)
        .bind(10, t.charges.exchange)
        .bind(11, t.charges.sebi)
        .bind(12, t.charges.stamp)
        .bind(13, t.charges.gst)
        .bind(14, t.realised)
        .bind(15, t.at)
        .bind(16, t.entry_realised)
        .run();
}

std::expected<void, DbError> PaperStore::upsert_position(std::int64_t account_id,
                                                         const oms::PositionKey& key,
                                                         const oms::Position& position,
                                                         std::int64_t at) {
    auto statement = db_.prepare(
        "INSERT OR REPLACE INTO positions (account_id, instrument_id, product, quantity, cost_raw, "
        "realised, "
        "updated_at, entry_cost_raw) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, account_id)
        .bind(2, std::int64_t{key.instrument.value()})
        .bind(3, num(key.product))
        .bind(4, position.quantity())
        .bind(5, position.cost_raw())
        .bind(6, position.realised())
        .bind(7, at)
        .bind(8, position.entry_cost_raw())
        .run();
}

std::expected<void, DbError> PaperStore::insert_ledger(std::int64_t account_id,
                                                       const oms::LedgerEntry& e) {
    auto statement = db_.prepare(
        "INSERT INTO ledger (account_id, at, kind, amount, order_id, instrument_id) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, account_id)
        .bind(2, e.at)
        .bind(3, num(e.kind))
        .bind(4, e.amount)
        .bind(5, e.order_id)
        .bind(6, std::int64_t{e.instrument.value()})
        .run();
}

std::expected<void, DbError> PaperStore::insert_event(std::int64_t at, std::string_view kind,
                                                      std::string_view payload) {
    auto statement = db_.prepare("INSERT INTO events (at, kind, payload) VALUES (?1, ?2, ?3)");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return statement->bind(1, at).bind(2, kind).bind(3, payload).run();
}

std::expected<std::vector<AccountRow>, DbError> PaperStore::accounts() {
    auto statement = db_.prepare(
        "SELECT id, user_id, created_at, opening_cash, active, kill_switch FROM accounts WHERE "
        "active = 1 "
        "ORDER BY id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return collect<AccountRow>(*statement, [](const storage::Statement& s) {
        return AccountRow{.id = s.column_int64(0),
                          .user_id = s.column_int64(1),
                          .created_at = s.column_int64(2),
                          .opening_cash = s.column_int64(3),
                          .active = s.column_int64(4) != 0,
                          .kill_switch = s.column_int64(5) != 0};
    });
}

std::expected<std::vector<AccountRow>, DbError> PaperStore::lost_accounts() {
    auto statement = db_.prepare(
        "SELECT a.id, (SELECT user_id FROM orders WHERE account_id = a.id ORDER BY id LIMIT 1), "
        "COALESCE((SELECT MIN(at) FROM ledger WHERE account_id = a.id), 0), "
        "COALESCE((SELECT amount FROM ledger WHERE account_id = a.id AND kind = 1 ORDER BY id DESC "
        "LIMIT 1), 0) "
        "FROM (SELECT account_id AS id FROM orders UNION SELECT account_id FROM ledger) a "
        "WHERE a.id NOT IN (SELECT id FROM accounts) ORDER BY a.id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    auto rows = collect<AccountRow>(*statement, [](const storage::Statement& s) {
        return AccountRow{.id = s.column_int64(0),
                          .user_id = s.column_is_null(1) ? 0 : s.column_int64(1),
                          .created_at = s.column_int64(2),
                          .opening_cash = s.column_int64(3),
                          .active = true,
                          .kill_switch = false};
    });
    if (rows) {
        std::erase_if(*rows, [](const AccountRow& row) {
            return row.user_id <= 0;
        });  // no order: no owner known
    }
    return rows;
}

std::expected<std::vector<oms::LedgerEntry>, DbError> PaperStore::ledger(std::int64_t account_id) {
    auto statement = db_.prepare(
        "SELECT at, kind, amount, order_id, instrument_id FROM ledger WHERE account_id = ?1 ORDER "
        "BY id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, account_id);
    return collect<oms::LedgerEntry>(*statement, [](const storage::Statement& s) {
        return oms::LedgerEntry{
            .at = s.column_int64(0),
            .kind = as<oms::LedgerKind>(s, 1),
            .amount = s.column_int64(2),
            .order_id = s.column_int64(3),
            .instrument = core::InstrumentId(static_cast<std::uint32_t>(s.column_int64(4)))};
    });
}

std::expected<std::map<oms::PositionKey, oms::Position>, DbError> PaperStore::positions(
    std::int64_t account_id) {
    auto statement = db_.prepare(
        "SELECT instrument_id, product, quantity, cost_raw, realised, entry_cost_raw FROM "
        "positions "
        "WHERE account_id = ?1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, account_id);
    using Entry = std::pair<oms::PositionKey, oms::Position>;
    auto rows = collect<Entry>(*statement, [](const storage::Statement& s) {
        return Entry{
            {.instrument = core::InstrumentId(static_cast<std::uint32_t>(s.column_int64(0))),
             .product = as<oms::Product>(s, 1)},
            oms::Position(s.column_int64(2), s.column_int64(3), s.column_int64(4),
                          s.column_int64(5))};
    });
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return std::map<oms::PositionKey, oms::Position>(rows->begin(), rows->end());
}

std::expected<std::vector<OrderRow>, DbError> PaperStore::live_orders() {
    auto statement = db_.prepare("SELECT " + std::string(kOrderColumns) +
                                 " FROM orders WHERE status IN (1, 2) ORDER BY id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return collect<OrderRow>(*statement, read_order);
}

std::expected<std::optional<OrderRow>, DbError> PaperStore::order_by_client(
    std::int64_t account_id, std::uint64_t client_order_id) {
    auto statement = db_.prepare("SELECT " + std::string(kOrderColumns) +
                                 " FROM orders WHERE account_id = ?1 AND client_order_id = ?2");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, account_id).bind(2, static_cast<std::int64_t>(client_order_id));
    auto rows = collect<OrderRow>(*statement, read_order);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return rows->empty() ? std::nullopt : std::optional(std::move(rows->front()));
}

std::expected<std::int64_t, DbError> PaperStore::max_order_id() {
    auto statement = db_.prepare("SELECT COALESCE(MAX(id), 0) FROM orders");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    const auto step = statement->step();
    if (!step) {
        return std::unexpected(step.error());
    }
    return statement->column_int64(0);
}

std::expected<std::optional<AccountRow>, DbError> PaperStore::account_for_user(
    std::int64_t user_id) {
    auto statement = db_.prepare(
        "SELECT id, user_id, created_at, opening_cash, active, kill_switch FROM accounts "
        "WHERE user_id = ?1 AND active = 1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, user_id);
    auto rows = collect<AccountRow>(*statement, [](const storage::Statement& s) {
        return AccountRow{.id = s.column_int64(0),
                          .user_id = s.column_int64(1),
                          .created_at = s.column_int64(2),
                          .opening_cash = s.column_int64(3),
                          .active = s.column_int64(4) != 0,
                          .kill_switch = s.column_int64(5) != 0};
    });
    if (!rows) {
        return std::unexpected(rows.error());
    }
    return rows->empty() ? std::nullopt : std::optional(rows->front());
}

std::expected<std::vector<OrderRow>, DbError> PaperStore::orders(std::int64_t account_id,
                                                                 std::int64_t from,
                                                                 std::int64_t to) {
    auto statement =
        db_.prepare("SELECT " + std::string(kOrderColumns) +
                    " FROM orders WHERE account_id = ?1 AND created_at >= ?2 AND created_at < ?3 "
                    "ORDER BY id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, account_id).bind(2, from).bind(3, to);
    return collect<OrderRow>(*statement, read_order);
}

std::expected<std::vector<TradeRow>, DbError> PaperStore::trades(std::int64_t account_id,
                                                                 std::int64_t from,
                                                                 std::int64_t to) {
    auto statement = db_.prepare(
        "SELECT order_id, account_id, instrument_id, side, product, quantity, price_raw, "
        "brokerage, stt, exchange, "
        "sebi, stamp, gst, realised, at, entry_realised FROM trades WHERE account_id = ?1 AND at "
        ">= ?2 AND at < ?3 ORDER BY id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, account_id).bind(2, from).bind(3, to);
    return collect<TradeRow>(*statement, read_trade);
}

std::expected<std::vector<TradeRow>, DbError> PaperStore::tagged_trades(std::int64_t account_id,
                                                                        std::string_view tag,
                                                                        std::int64_t from,
                                                                        std::int64_t to) {
    auto statement = db_.prepare(
        "SELECT t.order_id, t.account_id, t.instrument_id, t.side, t.product, t.quantity, "
        "t.price_raw, t.brokerage, "
        "t.stt, t.exchange, t.sebi, t.stamp, t.gst, t.realised, t.at, t.entry_realised FROM trades "
        "t JOIN orders o ON o.id = t.order_id "
        "WHERE t.account_id = ?1 AND o.tag = ?2 AND t.at >= ?3 AND t.at < ?4 ORDER BY t.id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, account_id).bind(2, tag).bind(3, from).bind(4, to);
    return collect<TradeRow>(*statement, read_trade);
}

std::expected<std::vector<PaperStore::TagTotals>, DbError> PaperStore::auto_totals(
    std::int64_t account_id, std::int64_t from, std::int64_t to) {
    auto statement = db_.prepare(
        "SELECT o.tag, COUNT(*), SUM(t.realised), SUM(t.brokerage + t.stt + t.exchange + t.sebi + "
        "t.stamp + t.gst) "
        "FROM trades t JOIN orders o ON o.id = t.order_id "
        "WHERE t.account_id = ?1 AND o.tag LIKE 'auto:%' AND t.at >= ?2 AND t.at < ?3 GROUP BY "
        "o.tag ORDER BY o.tag");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    statement->bind(1, account_id).bind(2, from).bind(3, to);
    return collect<TagTotals>(*statement, [](const storage::Statement& s) {
        return TagTotals{.tag = s.column_text(0),
                         .fills = s.column_int64(1),
                         .realised = s.column_int64(2),
                         .charges = s.column_int64(3)};
    });
}

std::expected<std::vector<std::int64_t>, DbError> PaperStore::active_instruments() {
    auto statement = db_.prepare(
        "SELECT instrument_id FROM orders WHERE status IN (1, 2) "
        "UNION SELECT instrument_id FROM positions WHERE quantity <> 0 ORDER BY 1");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    return collect<std::int64_t>(*statement,
                                 [](const storage::Statement& s) { return s.column_int64(0); });
}

std::expected<std::string, DbError> PaperStore::dump() {
    // Each table's rows as text, every column, in key order.
    const std::pair<std::string_view, std::string_view> tables[] = {
        {"accounts",
         "SELECT "
         "quote(id)||','||quote(user_id)||','||quote(created_at)||','||quote(opening_cash)||','||"
         "quote(active)||','||quote(kill_switch) FROM accounts ORDER BY id"},
        {"orders",
         "SELECT "
         "quote(id)||','||quote(account_id)||','||quote(client_order_id)||','||quote(status)||','||"
         "quote(current_type)||','||quote(quantity)||','||quote(filled)||','||quote(filled_value_"
         "raw)||','||"
         "quote(brokerage)||','||quote(charges)||','||quote(margin)||','||quote(reason)||','||"
         "quote(created_at)||','||quote(updated_at)||','||quote(eligible_at)||','||quote(resting) "
         "FROM orders ORDER BY id"},
        {"trades",
         "SELECT "
         "quote(order_id)||','||quote(quantity)||','||quote(price_raw)||','||quote(brokerage)||','|"
         "|"
         "quote(stt)||','||quote(exchange)||','||quote(sebi)||','||quote(stamp)||','||quote(gst)||'"
         ",'||"
         "quote(realised)||','||quote(at)||','||quote(entry_realised) FROM trades ORDER BY id"},
        {"positions",
         "SELECT quote(account_id)||','||quote(instrument_id)||','||quote(product)||','||"
         "quote(quantity)||','||quote(cost_raw)||','||quote(realised)||','||quote(entry_cost_raw) "
         "FROM positions "
         "ORDER BY account_id, instrument_id, product"},
        {"ledger",
         "SELECT quote(account_id)||','||quote(at)||','||quote(kind)||','||quote(amount)||','||"
         "quote(order_id)||','||quote(instrument_id) FROM ledger ORDER BY id"},
    };
    std::string text;
    for (const auto& [name, sql] : tables) {
        text += "[" + std::string(name) + "]\n";
        auto statement = db_.prepare(sql);
        if (!statement) {
            return std::unexpected(statement.error());
        }
        auto rows = collect<std::string>(
            *statement, [](const storage::Statement& s) { return s.column_text(0); });
        if (!rows) {
            return std::unexpected(rows.error());
        }
        for (const auto& row : *rows) {
            text += row + "\n";
        }
    }
    return text;
}

}  // namespace mda::broker
