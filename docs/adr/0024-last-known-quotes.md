# ADR-0024: Last-known quotes survive restarts

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** owner, Claude

## Context

Quotes, depth and circuit limits live only in memory: the gateway's `QuoteCache` and the
engine's option analytics are filled by ticks from the `md` bus, and a reader attaching
to the bus starts at the live tail (ADR-0010). After a restart, or each morning before
09:15, nothing ticks, so the watchlist shows "—" and the option chain stays empty until
the market opens. Kite shows the last price, day change and depth until the next
pre-open; that is the minimum (CLAUDE.md, "Kite's UI is the minimum standard").

## Options considered

### Option A — replay the `md` ring on attach
- Pros: no new storage; exactly the bus messages.
- Cons: records only link forward. Once the ring has wrapped, the oldest boundary still
  intact is unknown, so the safe window is "since the ring last wrapped": anywhere from
  nothing to the whole 256 MB. Quiet instruments' last ticks are often gone. Unreliable.

### Option B — ask Kite again (feedd re-subscribes, or calls the quote REST API)
- Pros: Kite's own values.
- Cons: only feedd holds the session; it would need a request channel from the gateway
  and engine. Re-subscribing to get a fresh tick is undocumented behaviour. Fails when
  the session has expired (overnight).

### Option C — the engine saves the latest message per instrument to SQLite
The engine already reads every `md` message. It keeps the latest MarketSnapshot,
DepthSnapshot and CircuitLimits per instrument and writes the changed ones every 5 s
(and on shutdown) to `data/last_quotes.db`. At startup the engine feeds the saved
snapshots to option analytics (not to the bar builder), and the gateway loads them into
`QuoteCache` before reading the bus.
- Pros: works after any restart and overnight; one writer; payloads are the bus's
  fixed-layout bytes, so `messages::decode` rejects ones from an older schema version.
- Cons: up to 5 s stale if the engine dies hard; one more file.

## Decision

Option C. Table `last_messages(instrument_id, type, payload)`, keyed by instrument and
message type, in its own file (`engine.last_quotes_db`, default `data/last_quotes.db`),
written only by mda-engine (library `mda::quotestore`). Saved values are shown as they
are, like Kite's after hours; the next live tick replaces them.

Fallback: an instrument with no saved quote (a new install, the first run after this
change, or an instrument that never ticked) gets one built from its last trading day's
bars in `bars.db` (`bars::day_snapshot`): LTP = last close, day open/high/low, volume, OI,
and the previous day's last close for the day change. It has no depth or bid/ask.

## Rationale

It is the only option that always has a value to show, and it costs a few thousand row
upserts every 5 s. Keeping it out of `bars.db` keeps that file to bars (ADR-0021).
Analytics outputs are not saved: the engine republishes everything every 15 s and chain
summaries every second, so once its inputs are back, so are the chain and the Greeks.
