# ADR-0018: Live prices to browsers: conflated JSON snapshots over a WebSocket

- **Status:** Proposed
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

Watchlists exist (W1/W2); the next step is live prices in the browser. Three questions:

1. **How does the gateway get ticks?** `mda-feedd` already publishes `MarketSnapshot`s to
   the shared-memory bus `md` (ADR-0007).
2. **What does `mda-feedd` subscribe to?** Today a static list from the config. Users now
   add arbitrary instruments to watchlists, and those need ticks too.
3. **How do ticks reach the browser, and how often?** This resolves the backlog item
   "Gateway WebSocket push: JSON vs binary ticks".

## Options considered

### Gateway ← ticks
- **Bus reader thread + quote cache (chosen).** One thread polls the bus and keeps the
  latest snapshot per instrument. Browser sessions read the cache. The bus is lossy and
  snapshot-based, so a cache of "latest" is exactly the right shape: losing an
  intermediate tick loses nothing.
- **Poll the bus from the I/O thread on a timer.** No lock, but a slow client write
  delays draining, and the drain delays HTTP. Coupling we don't need.

The cache is guarded by one mutex. Contention is tiny (a few hundred instruments, a few
browsers, pushes every 250 ms). A seqlock per instrument is the low-latency alternative.
It's worth doing once the engine reads the same data, not for browsers.

### Which instruments feedd subscribes to
- **feedd polls the app DB (chosen).** Every 2 s (`feed.watchlists_refresh_seconds`) it
  reads the union of all watchlist instruments, adds the static config list, and sends
  Kite only the difference (subscribe + mode / unsubscribe) on the live connection.
  SQLite in WAL mode lets this reader run beside the gateway's writes.
- **Gateway tells feedd (control channel / Unix socket).** Instant, but it adds a new IPC
  protocol and error handling for a change that happens a few times a day. Revisit with
  the subscription manager (rolling ATM option windows), which needs a control channel
  anyway.
- **Subscribe everything.** Kite allows 3,000 instruments per connection and ~68k are
  listed: no.

Changing subscriptions never reconnects. A reconnect would leave a gap in every
instrument's data, which the recorder would see.

### Browser push
- **JSON snapshots, conflated per instrument (chosen).** Every `gateway.publish_interval_ms`
  (250 ms default) a session sends the instruments whose snapshot changed since the last
  push, each as its latest state. Slow clients never queue stale ticks, and the payload
  is readable in DevTools. At watchlist scale (≤ 50 instruments, 4 pushes/s) that's a few
  KB/s.
- **Binary frames (like Kite's).** Smaller and faster to parse, but needs a decoder in the
  browser and is opaque when debugging. Worth it for option chains with hundreds of
  strikes. The message envelope has a `type`, so a binary variant can be added later.
- **Every tick, unconflated.** A browser can't render more than ~60 updates/s, and a slow
  link would back up. `publish_interval_ms: 0` gives the fastest setting (pushes are
  checked every 10 ms) for experiments.

### Security of the WebSocket
- Authenticated by the same HttpOnly `mda_session` cookie, checked at the upgrade and
  re-checked every 60 s. Sign-out, a password change elsewhere or account deletion close
  the stream (close code 4001).
- **Origin check:** browsers send cookies on cross-site WebSocket upgrades when cookie
  rules allow (CSWSH). `SameSite=Strict` already blocks that. As a second layer, the
  upgrade is refused when `Origin` is present and its host differs from `Host`.
- Limits: 200 subscribed instruments per connection; inbound messages ≤ 4 KB.

## Decision

As chosen above. The protocol is in [`docs/api/websocket.md`](../api/websocket.md).

## Consequences

- After a gateway restart the cache is empty. Each instrument fills on its next tick
  (Kite sends full-mode snapshots often for liquid instruments; illiquid ones can stay
  blank for a while). Fix later: feedd republishes all snapshots when a new reader
  attaches, or the cache is persisted.
- A newly added watchlist instrument starts ticking within ~2 s (the DB poll) plus Kite's
  first tick.
- Two places know the instrument union (feedd's poll, the DB), but one source of truth (the DB).
