# ADR-0021: 1-minute bars in SQLite (bars.db), built by mda-engine, backfilled from Kite

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

Charts and OI analytics need 1-minute bars (price, volume, OI, OI update counts) with
history. Kite's historical API (available on our plan, with OI) can backfill. Our own
bars add OI update counts, exact tick-derived values and expired contracts. This resolves
the "Bar store" backlog item. Details:
[option-chain-and-bars.md §3](../design/option-chain-and-bars.md).

## Options considered

- **SQLite file `bars.db` (chosen for now).** About 250k rows/day (~5 GB/year at our
  scope). It is an embedded, single file, which we already use (ADR-0012). Point queries
  per instrument and time range are exactly what charts need, and WAL mode lets the
  gateway read while the engine writes.
- **DuckDB / Parquet.** Superb for scans across many instruments (research, backtests),
  weaker for small, frequent appends. Kept as the archive format later: daily export of
  old days to Parquet.
- **A time-series server (TimescaleDB, QuestDB, ClickHouse).** Another service to run and
  back up, which this scale doesn't need.
- **Inside app.db.** It mixes small, precious user data with bulk, rebuildable market data,
  with different sizes, write rates and backup policies.

## Decision

- `data/bars.db`, table `bars_1m` keyed by (instrument_id, minute start), `WITHOUT ROWID`.
  It holds int64 fixed-point prices and a `source` column (`live` | `kite`).
- **mda-engine is the only live writer.** The gateway reads it read-only.
  **mda-backfill** inserts only missing minutes, and a live bar always replaces a
  backfilled one.
- Bars are timed by the **exchange timestamp**. A minute closes on the first later tick or
  2 s after it ends. Minutes without ticks produce no bar.
- Higher timeframes are aggregated at query time from 1m.

## Consequences

- One source of truth for history; charts can show months of history on day one.
- If the engine is down, live bars for that period come from backfill (no OI update
  counts there).
- Should research workloads grow, the Parquet archive is an export, not a migration.
