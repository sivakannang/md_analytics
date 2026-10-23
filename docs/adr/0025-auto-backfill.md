# ADR-0025: Automatic bar backfill for newly subscribed instruments

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** owner, Claude

## Context

`bars.db` gets bars from live ticks (mda-engine) and from `mda-backfill`, which is run by
hand (ADR-0021). An instrument added to a watchlist, or an option strike entering a chain
window, is subscribed within seconds but has no history: its chart says "No bars
recorded" until someone runs a backfill. Kite's charts show history for anything at once.

Constraints: only mda-feedd and mda-backfill hold the Kite session; Kite allows about 3
historical requests per second; past-day backfills run only after market hours (cash
09:15–15:30, F&O until 15:40 IST; CLAUDE.md); live bars always beat backfilled ones.

## Options considered

### Option A — mda-feedd backfills instruments it subscribes that have no bars
- Pros: feedd knows the moment an instrument is subscribed, already holds the session and
  already calls Kite's REST API for new subscriptions (circuit limits). Covers watchlists
  and chain windows alike. No new process.
- Cons: more work in the feed process (kept off the tick path: REST is async on the I/O
  thread like the circuit loop, SQLite on a worker thread).

### Option B — the chart request triggers a backfill
- Pros: only instruments someone looks at.
- Cons: the gateway has no Kite session; it would need a request channel to feedd, and
  the first chart view would still be empty.

### Option C — run mda-backfill on a schedule
- Pros: no code in the feed.
- Cons: history appears only after the next run (after hours at best).

## Decision

Option A. Every 30 s, feedd checks the subscribed instruments it has not checked in the
current phase:
- **During market hours** (Mon–Fri 09:15–15:40 IST): an instrument with no bars today
  gets today's minutes from 09:15 up to the minute before now. Past days wait.
- **Outside market hours**: an instrument with no bars in the past `auto_backfill_days`
  (default 30) before today gets the whole window up to now.

"Checked" is kept per phase, so an instrument added intraday gets today at once and its
past days after 15:40; nothing is stored, so a restart re-checks (cheap indexed queries).
Requests are spaced 350 ms apart; an expired session pauses the loop until the next
phase. The candle conversion is shared with mda-backfill (`mda/apps/backfill.hpp`), which
stays the tool for filling gaps in instruments that already have bars.

## Rationale

It is the only option where a new instrument's chart fills within seconds without a
manual step, and it keeps the market-hours rule. A gap in an instrument that has bars
(feed down for an hour) is not detected here on purpose: telling a gap from a quiet
instrument needs more than "has bars", and the manual run covers it.
