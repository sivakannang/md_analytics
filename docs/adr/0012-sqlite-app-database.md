# ADR-0012: SQLite for the app database

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

The gateway needs durable, transactional storage for **application data**: users,
password hashes, login sessions, and next watchlists and settings. Volumes are tiny (a few
users, hundreds of rows), but correctness matters (unique usernames, atomic password
changes, consistent sessions). This is separate from market data: ticks go to the journal
and candles to the bar store, whose technology is still open.

## Options considered

- **SQLite** — embedded library, single file, ACID transactions, SQL, no server to run or
  secure; ubiquitous and extremely well tested; one writer at a time (fine for us).
- **PostgreSQL** — full server, concurrent writers, network access; another service to
  install, configure, secure and back up — unjustified for a few users.
- **Hand-written files** (like the instrument registry) — no transactions or queries;
  fine for append-only id lists, wrong for mutable relational data.
- **DuckDB** — analytical, column-oriented; excellent for bars, wrong tool for small
  transactional updates.

## Decision

**SQLite** (vcpkg `sqlite3`) for the app database, file `data/app.db` (git-ignored):

- A thin C++ RAII wrapper (`storage` module): `Database`, prepared `Statement`s with
  bound parameters only — **never string-concatenated SQL** (no SQL injection by construction).
- `PRAGMA journal_mode=WAL`, `foreign_keys=ON`, `busy_timeout`.
- Schema versioned with `PRAGMA user_version` and ordered, idempotent migrations run at
  startup inside a transaction.
- Accessed from the gateway's I/O thread only (one connection); queries are microseconds.

The instrument registry stays an append-only file for now; moving it into SQLite is an
option later (ADR-0008 unaffected).

## Rationale

Transactions and SQL without operating a server, at our scale, with a mature C API that
teaches prepared statements and migrations properly.

## Consequences

- Positive: zero administration; backup = copy one file (or `VACUUM INTO`); fast.
- Negative: single machine, single writer; moving to a server DB later means a migration.
- The bar store decision stays open (DuckDB/Parquet) — this ADR covers app data only.

## Learning notes

- Prepared statements and why they prevent SQL injection; WAL mode; schema migrations.
- Interview: "Where do you keep reference/user data vs market data, and why different stores?"
