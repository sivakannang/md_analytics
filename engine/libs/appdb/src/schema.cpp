#include <array>

#include <mda/appdb/schema.hpp>

namespace mda::appdb {
namespace {

constexpr std::array<std::string_view, 7> kMigrations = {
    // 1: users and sessions (docs/design/user-management.md)
    R"sql(
CREATE TABLE users (
    id                  INTEGER PRIMARY KEY,
    username            TEXT NOT NULL UNIQUE COLLATE NOCASE,
    password_hash       TEXT NOT NULL,
    created_at          INTEGER NOT NULL,
    password_changed_at INTEGER NOT NULL
);
CREATE TABLE sessions (
    token_hash   BLOB PRIMARY KEY,
    user_id      INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at   INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    user_agent   TEXT
);
CREATE INDEX sessions_by_user ON sessions(user_id);
)sql",

    // 2: contact details for notifications (NULL verified_at = not verified yet)
    R"sql(
ALTER TABLE users ADD COLUMN email TEXT COLLATE NOCASE;
ALTER TABLE users ADD COLUMN phone TEXT;
ALTER TABLE users ADD COLUMN email_verified_at INTEGER;
ALTER TABLE users ADD COLUMN phone_verified_at INTEGER;
CREATE UNIQUE INDEX users_by_email ON users(email) WHERE email IS NOT NULL;
)sql",

    // 3: instrument catalog, watchlists, notification outbox
    R"sql(
CREATE TABLE instruments (
    id             INTEGER PRIMARY KEY,   -- permanent InstrumentId (ADR-0008)
    exchange       TEXT NOT NULL,         -- NSE | BSE
    market         TEXT NOT NULL,         -- CASH | FO
    kind           TEXT NOT NULL,         -- IDX | EQ | FUT | OPT
    underlying     TEXT NOT NULL,
    expiry         TEXT,                  -- ISO date, derivatives only
    strike_raw     INTEGER,               -- Price raw (1/10,000), options only
    option_type    TEXT,                  -- CE | PE, options only
    trading_symbol TEXT NOT NULL,
    name           TEXT NOT NULL,
    lot_size       INTEGER NOT NULL,
    tick_size_raw  INTEGER NOT NULL,
    underlying_id  INTEGER,
    listed         INTEGER NOT NULL,      -- 1 if in today's broker list
    search_text    TEXT NOT NULL,         -- lower-case words for token search
    updated_at     INTEGER NOT NULL
);
CREATE INDEX instruments_listed ON instruments(listed, kind);

CREATE TABLE watchlists (
    id         INTEGER PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name       TEXT NOT NULL,
    position   INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (user_id, name)
);
CREATE TABLE watchlist_items (
    watchlist_id  INTEGER NOT NULL REFERENCES watchlists(id) ON DELETE CASCADE,
    instrument_id INTEGER NOT NULL REFERENCES instruments(id),
    position      INTEGER NOT NULL,
    added_at      INTEGER NOT NULL,
    PRIMARY KEY (watchlist_id, instrument_id)
);

-- Transactional outbox (ADR-0016): producers insert in the same transaction as the change;
-- mda-notifier delivers. user_id has no foreign key: account-deleted messages must outlive
-- the user, so the recipient address is copied into the row and purged after delivery.
CREATE TABLE notifications (
    id              INTEGER PRIMARY KEY,
    user_id         INTEGER,
    type            TEXT NOT NULL,        -- e.g. account.signup
    email           TEXT,                 -- recipient snapshot (personal data; purge after send)
    payload         TEXT NOT NULL,        -- JSON
    dedup_key       TEXT NOT NULL UNIQUE,
    status          TEXT NOT NULL,        -- pending | sent | failed
    attempts        INTEGER NOT NULL DEFAULT 0,
    next_attempt_at INTEGER NOT NULL,
    created_at      INTEGER NOT NULL,
    sent_at         INTEGER,
    last_error      TEXT
);
CREATE INDEX notifications_due ON notifications(status, next_attempt_at);
)sql",

    // 4: named groups inside a watchlist (UI2). Deleting a group keeps its instruments
    // (they become ungrouped). Items keep one position across the whole list.
    R"sql(
CREATE TABLE watchlist_groups (
    id           INTEGER PRIMARY KEY,
    watchlist_id INTEGER NOT NULL REFERENCES watchlists(id) ON DELETE CASCADE,
    name         TEXT NOT NULL,
    position     INTEGER NOT NULL,
    UNIQUE (watchlist_id, name)
);
ALTER TABLE watchlist_items ADD COLUMN group_id INTEGER REFERENCES watchlist_groups(id) ON DELETE SET NULL;
)sql",

    // 5: admin dashboard (ADR-0019): roles, client IP per session, the auth event log and
    // per-user daily usage. auth_events has no foreign key and copies the username, so a
    // deleted account's history stays readable; it is purged after 90 days.
    R"sql(
ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'user';
ALTER TABLE sessions ADD COLUMN ip TEXT;
CREATE TABLE auth_events (
    id         INTEGER PRIMARY KEY,
    user_id    INTEGER,
    username   TEXT NOT NULL,
    type       TEXT NOT NULL,
    at         INTEGER NOT NULL,
    ip         TEXT,
    user_agent TEXT
);
CREATE INDEX auth_events_user ON auth_events(user_id, at);
CREATE INDEX auth_events_at ON auth_events(at);
CREATE TABLE usage_daily (
    user_id       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    day           TEXT NOT NULL,
    ws_bytes      INTEGER NOT NULL DEFAULT 0,
    ws_messages   INTEGER NOT NULL DEFAULT 0,
    http_bytes    INTEGER NOT NULL DEFAULT 0,
    http_requests INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (user_id, day)
);
)sql",

    // 6: notification delivery per channel (docs/design/notifications.md §2). A notification
    // is fanned out into one delivery per channel, retried on its own; the address is a
    // snapshot, purged once the delivery is finished. user_channels holds linked channels
    // (Telegram chat id; email uses users.email); channel_codes the one-time link and
    // verification codes, stored hashed.
    R"sql(
CREATE TABLE notification_deliveries (
    id              INTEGER PRIMARY KEY,
    notification_id INTEGER NOT NULL REFERENCES notifications(id) ON DELETE CASCADE,
    user_id         INTEGER,              -- recipient (a system notification goes to each admin)
    channel         TEXT NOT NULL,        -- email | telegram | ...
    address         TEXT,                 -- recipient snapshot (personal data; purged when done)
    status          TEXT NOT NULL,        -- pending | sent | failed
    attempts        INTEGER NOT NULL DEFAULT 0,
    next_attempt_at INTEGER NOT NULL,
    sent_at         INTEGER,
    last_error      TEXT,
    UNIQUE (notification_id, user_id, channel)
);
CREATE INDEX notification_deliveries_due ON notification_deliveries(status, next_attempt_at);
CREATE TABLE user_channels (
    user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    channel   TEXT NOT NULL,
    address   TEXT,                       -- NULL for email (users.email)
    enabled   INTEGER NOT NULL DEFAULT 1,
    linked_at INTEGER NOT NULL,
    PRIMARY KEY (user_id, channel)
);
CREATE TABLE channel_codes (
    code_hash  BLOB PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    purpose    TEXT NOT NULL,             -- telegram_link | email_verify
    expires_at INTEGER NOT NULL
);
)sql",

    // 7: wrong guesses per code: a 6-digit email code dies after 5, so it can't be brute-forced.
    R"sql(
ALTER TABLE channel_codes ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0;
)sql",
};

}  // namespace

std::span<const std::string_view> migrations() {
    return kMigrations;
}

}  // namespace mda::appdb
