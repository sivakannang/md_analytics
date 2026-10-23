# Kite connectivity: session, REST, WebSocket, mda-feedd

> Status: **Accepted** · Decision: [ADR-0011](../adr/0011-boost-beast-for-kite-connectivity.md)
> · Related: [Kite adapter (decoder)](kite-adapter.md), [instruments](instruments.md),
> [recovery](recovery-and-gap-filling.md)

## 1. Daily lifecycle

```
morning     you log in (mda-login / your existing app) ─► secrets/kite_session.json
            │
mda-feedd   1. read session file ── missing/expired ─► FeedStatus AuthExpired, wait for file change
start       2. REST GET /user/profile   (validates the token)
            3. REST GET /instruments    (CSV) ─► import ─► registry.save_new() ─► TokenMap
            4. build subscription list (config; later: subscription manager)
            5. open WebSocket connection(s), subscribe in full mode
            6. read loop: frame ─► RawFrame + decode ─► MarketSnapshot / DepthSnapshot ─► bus "md"
during day  reconnect on failure with back-off; stale-feed detection; FeedStatus events
~06:00 IST  token expires ─► 403 on reconnect ─► AuthExpired until the next login
```

## 2. Session

| Item | Detail |
|---|---|
| File | `secrets/kite_session.json`: `{"api_key": "...", "access_token": "..."}` (same as the OI probe), mode `0600`, git-ignored (ADR-0005) |
| Override | `MDA_KITE_API_KEY`, `MDA_KITE_ACCESS_TOKEN` environment variables |
| Validation | `GET /user/profile` at start; 403 → `AuthExpired` |
| Expiry | Tokens expire ~06:00 IST next day. `mda-feedd` watches the session file and retries when it changes — no restart needed after the morning login |
| Logging | Token never logged; at most the first 4 characters |

## 3. REST client

- Base `https://api.kite.trade`; headers `X-Kite-Version: 3`,
  `Authorization: token <api_key>:<access_token>`.
- Calls in this phase: `GET /user/profile`, `GET /instruments` (CSV, ~10 MB).
- Rate limits (Kite): ~10 req/s general, 3 req/s historical — a simple per-endpoint
  limiter; irrelevant for two startup calls, essential later for backfill.
- Errors mapped to: `AuthExpired` (403), `RateLimited` (429), `Network`, `Server` (5xx),
  `BadResponse`.

## 4. WebSocket client

| Aspect | Design |
|---|---|
| URL | `wss://ws.kite.trade?api_key=…&access_token=…` (TLS, SNI, certificate verified) |
| Subscribe | text `{"a":"subscribe","v":[tokens]}` then `{"a":"mode","v":["full",[tokens]]}`, in batches |
| Limits | ≤ 3,000 tokens per connection, ≤ 3 connections per API key |
| Frames | binary → decoder; 1-byte → heartbeat; text → JSON (`error`, `message`, `order`) logged/handled |
| Stale detection | no frame (data or heartbeat) for **5 s** → treat as dead, reconnect |
| Reconnect | exponential back-off 1 s → 2 → 4 … max 30 s, ± 20 % jitter; reset after 60 s healthy |
| Auth failure | HTTP 403 on upgrade → `AuthExpired`, stop retrying until the session file changes |
| On reconnect | resubscribe the connection's tokens; publish `FeedStatus`; ticks missed meanwhile are a gap → [recovery](recovery-and-gap-filling.md) (backfill: later unit) |

## 5. mda-feedd threading (v1)

**One thread** runs the `io_context`, all connections, decoding and bus publishing:

```
socket read ─► RawFrame publish ─► decode ─► snapshot publish     (same thread, same callback)
```

Why one thread: decode ≈ 100 ns/packet and publish ≈ 5–10 ns; at ~5k packets/s that is
< 0.1 % of a core. Extra threads would add queues, hand-offs and failure modes without
measurable benefit. **Measure first**: if profiling ever shows the I/O thread busy, the
next step is network thread → SPSC queue (your ring-buffer design, hardened) → decode
thread. The bus has a single writer, so all publishing stays on one thread either way.

Sequence numbers: one feed-wide counter in `mda-feedd`, assigned at publish (raw frames
and snapshots share it), so the journal has one total order.

## 6. Configuration (v1, until the runtime/config unit)

Command-line flags: session file path, bus name/capacity, registry path, CA bundle path,
and a **static subscription list** (e.g. `NIFTY 50`, NIFTY near futures, ATM ± N strikes
of the nearest expiry, resolved through the registry). The full subscription manager
(budget, rolling ATM window, 3 connections) is the next unit after the first live run.

## 7. Tools for the first live run

| Tool | Purpose |
|---|---|
| `mda-tail` | Attaches to bus `md` and prints decoded snapshots (`NIFTY 50 24580.50 …`) — eyeball the live feed |
| `mda-busctl stat md` | Rates, lag, reader health (exists) |

## 8. Implementation units

| Unit | Scope | Needs market hours? |
|---|---|---|
| K1 ✅ | vcpkg deps (Beast, Asio, OpenSSL, JSON); session file loader; REST client (`/user/profile`, `/instruments`); a small `mda-kite-check` tool that validates the token and imports today's instruments | No — REST works any time |
| K2 ✅ | WebSocket client: connect, subscribe, read loop, heartbeat/stale, reconnect/back-off, AuthExpired | Connect/subscribe any time; ticks only during market hours |
| K3 ✅ | `mda-feedd` v1 (static subscriptions) + `mda-tail` → **first live run** | Yes |

## 8a. First live run (2026-10-22)

- 08:54 IST: connected and subscribed 12 instruments in 0.17 s; index ticks (NIFTY 50, NIFTY BANK,
  SENSEX) decoded ~2×/s.
- Kite refreshes the instrument list early morning (`instruments_meta` reported a new count):
  the daily CSV cache is only reused if written after 08:30 IST; 514 new contracts registered.
- 09:00 equity pre-open: full-mode packets with 5-level depth decoded correctly — crossed book
  during order collection, and a price-0 level holding the market orders.

## 9. Open questions

- ~~CA bundle on macOS~~ — resolved: both `/opt/homebrew/etc/openssl@3/cert.pem` and
  `/etc/ssl/cert.pem` exist on the dev Mac; default to the Homebrew one, configurable.
- **Daily login (`mda-login`):** the Kite app's registered redirect URL is
  `http://127.0.0.1:8080/auth/zerodha/callback`. `mda-login` listens there, receives the
  `request_token` after you log in, exchanges it (`POST /session/token`, checksum
  `sha256(api_key + request_token + api_secret)`) and writes `secrets/kite_session.json`.
  Needs `api_secret` in the secrets file; must not run while another app uses port 8080.
