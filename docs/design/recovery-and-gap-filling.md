# Recovery and intraday gap filling

> Status: **Draft** · Related: [architecture overview](../architecture/overview.md)

## Problem

Processes crash, networks drop and machines restart during market hours. After any
such event, the system must return to a correct state: live data must resume, and
1-minute bars (price, volume, OI) must have no silent holes.

Example used throughout: a crash at **10:15:20**, back online at **10:18:40**.

## Key facts

- The Kite WebSocket **does not replay** missed ticks. Ticks not received are lost.
- Each Kite *full*-mode tick carries **cumulative** day volume, current OI, day OHLC
  and depth — so the first tick after reconnecting restores a correct *current* snapshot.
- The Kite **historical API** (included in our plan) returns 1-minute candles with OI
  (`oi=1`), but is rate-limited to roughly **3 requests/second, one instrument per request**.

## Failure cases

### Case 1 — `mda-engine` or `mda-gateway` crashes (feed still running)

No data is lost: `mda-feedd` and `mda-recorder` keep writing the tick journal.

Engine recovery (event sourcing):

1. Load the latest **checkpoint** (engine state persisted periodically, e.g. every minute).
2. **Replay the tick journal** from the checkpoint's position to the present, faster than real time.
3. Switch to the live bus.

Result: bars are **identical** to an uninterrupted run.

Gateway recovery: request a fresh snapshot from the engine, then apply sequenced
updates (see [overview](../architecture/overview.md#engine--gateway-replication)).

### Case 2 — `mda-feedd` crashes, network/Kite disconnect, or machine restart

Ticks during the outage are lost. Recovery has two layers.

**Layer 1 — live state self-heals.** The first full-mode tick per instrument after
reconnecting restores the correct snapshot. No backfill needed.

**Layer 2 — bars are backfilled** from the Kite historical API:

| Minute | Data we have | Action |
|---|---|---|
| 10:15 | Live data until :20, then nothing | **Partial** → replace with Kite candle |
| 10:16, 10:17 | None | **Missing** → fill from Kite |
| 10:18 | Live data from :40 only | **Partial** → keep live; re-fetch after 10:18 closes and replace |
| 10:19 → | Complete live data | Normal |

Rule: any minute not fully covered by live data is taken from the Kite candle once
that minute has closed.

## Bar quality flags

Every bar records where it came from, so charts can display it and analytics can
decide whether to trust it.

| Flag | Meaning |
|---|---|
| `live` | Built entirely from live ticks |
| `partial` | Some live ticks missing; repair pending |
| `backfilled` | Taken from the Kite historical API |
| `missing` | Gap known, not yet filled (e.g. rate limit, API error) |

## Backfill prioritization

At ~3 requests/second, backfilling all ~6,000 instruments takes ~33 minutes, so
requests are queued by priority:

1. NIFTY spot, futures and near-ATM options (later: other indices)
2. Instruments currently visible in a user's chart, option chain or watchlist
3. Everything else, in the background

Backfill never blocks the live path. When a chart is opened for an instrument whose
backfill is still pending, its request moves to the front of the queue.

## Startup sequence (any crash)

1. Load the engine checkpoint; replay the tick journal → state restored up to the crash.
2. Connect to Kite live; live bars resume immediately.
3. Compute the gap: last journal event time → first live tick time.
4. Queue backfill requests by priority; merge results; set quality flags.
5. Recompute dependent analytics (OI change, buildup classification, signals) for affected bars.

## Reducing the gap

- **Supervisor** restarts a crashed process in under a second; the outage shrinks to the reconnect time.
- **Hot-standby feed handler** (A/B arbitration, as exchanges do) — Phase 3+, limited by Kite's shared 3-connection cap.

## Deferred

- **End-of-day reconciliation:** compare all of the day's 1-minute bars with Kite
  historical candles each evening and correct differences. Not in Phase 1.

## Known limitation (accepted)

Our live bars are built from ~1-second Kite snapshots, not every exchange trade, so a
brief high/low between snapshots can be missed. Kite's candles are built from all trades,
so backfilled and live bars may differ slightly. Accepted for Phase 1; end-of-day
reconciliation addresses it later.

## Open questions

- Checkpoint frequency and format for the engine.
- Journal `fsync` policy (durability vs write latency) — benchmark.
- Should repaired bars publish a correction event to connected browsers? (Likely yes.)
