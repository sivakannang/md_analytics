# md_analytics WebSocket stream

Live prices for the instruments a page is showing ([ADR-0018](../adr/0018-live-prices-to-browsers.md)).

```
ws://<host>/api/v1/stream
```

- **Auth:** the `mda_session` cookie (same as REST). No session → HTTP `401` before the
  upgrade. A cross-site `Origin` → `403`: the origin's host must equal the host the browser
  used, which behind `tailscale serve` is `X-Forwarded-Host` (believed only from a loopback
  peer, ADR-0027).
- **Messages:** JSON text frames, one object each. Client → server messages have an
  action `a`; server → client messages have a `type`.
- **Conflation:** at most one update per instrument every `gateway.publish_interval_ms`
  (250 ms by default), carrying its latest state.

## Client → server

```json
{"a": "subscribe",   "v": [59350, 19258]}
{"a": "unsubscribe", "v": [19258]}
{"a": "subscribe",   "v": [59350], "mode": "depth"}
```

`v` holds `instrument_id`s from the REST API. Subscribing sends the current quote of each
instrument at once (if the gateway has seen it today), then updates as they change. At
most 200 instruments per connection. Unknown ids are accepted and simply never tick.

`mode` is `quote` (the default), `depth`, `bars` (live 1-minute bars for charts; at most
10 instruments per connection), or `chain` (option-chain summaries; `v` holds the chain's
spot `instrument_id`, the `spot_id` from `GET /chains`). Depth is the 5-level order book and is a
separate subscription, because only an expanded row needs it. At most 10 depth
instruments per connection. Unsubscribe with the same `mode`. Any other `mode` closes the
connection (1008).

## Server → client

### `quotes`

```json
{"type": "quotes", "data": [
  {"id": 59350, "ltp": 1412.3, "change": 12.1, "change_pct": 0.86,
   "open": 1401.0, "high": 1415.5, "low": 1398.2, "close": 1400.2,
   "volume": 1834567, "avg_price": 1408.55, "last_qty": 25, "buy_qty": 544280,
   "sell_qty": 733207, "exchange_time": "2026-10-22T04:05:11Z",
   "last_trade_time": "2026-10-22T04:05:10Z"}
]}
```

| Field | Meaning |
|---|---|
| `ltp` | Last traded price (index value for indices) |
| `close` | Previous day's close; `change` = `ltp − close`, `change_pct` = `change / close × 100` (rounded to 2 places) |
| `open`, `high`, `low` | Today's (0 before the first trade) |
| `volume` | Today's traded quantity (omitted for indices) |
| `avg_price` | Volume-weighted average price for the day |
| `last_qty` | Quantity of the last trade |
| `buy_qty`, `sell_qty` | Total pending buy / sell quantity in the whole book (Kite's depth "Total") |
| `oi`, `oi_day_high`, `oi_day_low` | Open interest and its day range (derivatives in full mode; 0 for cash instruments) |
| `exchange_time` | Exchange timestamp of the snapshot (UTC) |
| `last_trade_time` | Time of the last trade (UTC) |
| `iv`, `delta`, `gamma`, `theta`, `vega` | Options only, from mda-engine (ADR-0022: Black-76 on the parity forward). `iv` in percent; delta per point; theta per calendar day; vega per 1 vol point. Omitted until the option has traded today. |
| `prev_oi`, `oi_change` | Options: previous trading day's closing OI (from bars.db) and `oi − prev_oi` |
| `buildup` | Options: `long_buildup`, `short_buildup`, `short_covering` or `long_unwinding` (price change vs previous close × OI change vs `prev_oi`); omitted when unknown |
| `lower_circuit`, `upper_circuit` | Today's price band. Not in the tick stream: mda-feedd fetches it from Kite's quote API (at start, for new subscriptions within ~2 s, and every `feed.circuit_refresh_minutes`). Omitted for indices and until fetched. |

Fields that the feed mode doesn't carry are omitted (for example `ltp` mode has only `ltp`).

### `depth`

```json
{"type": "depth", "data": [
  {"id": 59350,
   "bids": [{"price": 1195.9, "qty": 56, "orders": 4}, …],
   "asks": [{"price": 1196.0, "qty": 3042, "orders": 2}, …]}
]}
```

Up to 5 levels per side, best first. Empty levels are left out. Sent for depth
subscriptions whenever the book changes (conflated like quotes).

### `bars`

```json
{"type": "bars", "data": [
  {"id": 18487, "t": 1792662600, "o": 22775.0, "h": 22781.5, "l": 22770.0, "c": 22779.0,
   "v": 845, "oi": 5887570, "oi_o": 5880000, "oi_h": 5887570, "oi_l": 5879100, "oi_n": 2,
   "oi_t": 1792662641, "forming": true}
]}
```

The latest 1-minute bars from mda-engine: the minute in progress (`forming: true`, updated
about once a second) and the one just closed. The client merges them into the current
candle of its interval.

`oi` is the OI at the close; `oi_o`, `oi_h`, `oi_l` the OI candle's open, high and low;
`oi_n` how many times OI changed in the minute; `oi_t` when it last changed (Unix
seconds, receive time; `null` when it didn't or isn't known). Bars carry no buildup
state: it is worked out on the server for each interval, so the chart re-reads the latest
REST bars when a minute closes ([OI buildup chart](../design/oi-buildup-chart.md) §6).

### `chain`

```json
{"type": "chain", "data": [
  {"underlying": "NIFTY", "spot_id": 18274, "expiry": "2026-10-27", "forward": 22713.45,
   "atm_strike": 22700, "atm_iv": 13.97, "pcr": 0.89, "max_pain": 22800,
   "call_oi": 104512300, "put_oi": 93015600, "days_to_expiry": 5.0}
]}
```

One summary per expiry of the subscribed chain, about once a second while it changes.
`forward` (parity-implied), `atm_strike` and `atm_iv` (percent) are `null` until known.
`pcr` and `max_pain` are over the strikes the feed subscribes (ATM ± `feed.chains[].strikes`).

### `status`

```json
{"type": "status", "data": {"feed": "connected"}}
```

Sent on connect and whenever it changes. `feed` is one of:
- `connected` — ticks are flowing;
- `disconnected` — mda-feedd lost Kite and is reconnecting;
- `login_required` — the daily Kite login hasn't been done (run `mda-login`);
- `unavailable` — mda-feedd isn't running.

### `error`

```json
{"type": "error", "message": "at most 200 instruments per connection"}
```

## Close codes

| Code | Meaning |
|---|---|
| 1001 | Gateway shutting down; reconnect with back-off |
| 1008 | Malformed or oversized message |
| 4001 | Session ended (signed out, password changed elsewhere, account deleted) |
