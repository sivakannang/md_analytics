# md_analytics REST API

> Status: **Draft** · Conventions: [ADR-0013](../adr/0013-gateway-http-server-and-api-conventions.md)
> · Auth: [ADR-0014](../adr/0014-authentication.md)

Base URL: `http://127.0.0.1:8090/api/v1` (default; `mda-gateway --listen`).

Every path outside `/api` is the website (ADR-0027) when `gateway.web_dir` is set (default
`web/dist`, built with `npm --prefix web run build`): files from that folder, `index.html` for
the app's own routes (`/chain`, `/admin/system`, …), `404` for a missing file. An unknown path
under `/api` is still a JSON `404`.

## Conventions

- Request and response bodies are JSON (`Content-Type: application/json`).
- Every response has the same envelope:

  ```json
  {"status": "success", "data": { … }}
  {"status": "error", "error_type": "InputException", "message": "username already taken"}
  ```

| `error_type` | HTTP | Meaning |
|---|---|---|
| `InputException` | 400 | Missing/invalid fields |
| `AuthException` | 401 | Not signed in, session expired, or wrong credentials |
| `PermissionException` | 403 | Signed in but not allowed (e.g. signup disabled) |
| `NotFoundException` | 404 | No such resource/route |
| `ConflictException` | 409 | Already exists (e.g. username taken) |
| `RateLimitException` | 429 | Too many failed attempts; retry later |
| `GeneralException` | 500 | Unexpected server error |

- **Authentication:** a session cookie `mda_session` (HttpOnly, SameSite=Strict, and
  `Secure` when the request came over HTTPS: `X-Forwarded-Proto: https` from a loopback
  proxy such as `tailscale serve`, ADR-0027) set by sign-in. Browsers send it automatically; with curl use a cookie jar (`-c`/`-b`).
  Endpoints marked 🔒 return `401 AuthException` without a valid session.

## Users and sessions

### Sign up — `POST /user/signup`

```json
{"username": "siva", "email": "siva@example.com", "phone": "98765 43210", "password": "correct horse battery"}
```

| Field | Rules |
|---|---|
| `username` | 3–32 characters: letters, digits, `_ . -`; unique (case-insensitive); stored lower-case |
| `email` | Required; stored lower-case; unique — used for notifications and (later) password reset |
| `phone` | Required; an Indian mobile number in any common form (`98765 43210`, `+91-98765-43210`, `09876543210`); stored as E.164 `+919876543210`; not unique |
| `password` | 8–128 characters |

`201` →
```json
{"status": "success", "data": {"user_id": 1, "username": "siva", "email": "siva@example.com",
  "phone": "+919876543210", "email_verified": false, "phone_verified": false,
  "created_at": "2026-10-22T05:10:00Z"}}
```

Errors: `400` invalid field, `409` username taken / email already registered, `403` signup disabled.
Email and phone start **unverified**; verification arrives with the notification service.
Signing up does **not** sign you in.

### Sign in — `POST /session`

```json
{"username": "siva", "password": "correct horse battery"}
```

`200` + `Set-Cookie: mda_session=…; HttpOnly; SameSite=Strict; Path=/` (`; Secure` over HTTPS) →
```json
{"status": "success", "data": {"user_id": 1, "username": "siva"}}
```

Errors: `401` invalid username or password (same message either way), `429` too many attempts.

### Sign out — `DELETE /session` 🔒

`200` → `{"status": "success", "data": true}`; the cookie is cleared and the session deleted.

### Current user — `GET /user/profile` 🔒

`200` → the same user object as sign-up returns (including `email`, `phone` and their
`*_verified` flags).

### Change password — `PUT /user/password` 🔒

```json
{"current_password": "correct horse battery", "new_password": "a new long passphrase"}
```

`200` → `{"status": "success", "data": true}`. All **other** sessions of the user are
signed out; the current one stays valid.

Errors: `400` new password invalid, `401` current password wrong.

### Delete account — `DELETE /user` 🔒

```json
{"password": "correct horse battery"}
```

`200` → `{"status": "success", "data": true}`, and the session cookie is cleared. The user,
all their sessions and all their watchlists are deleted at once. An `account.deleted`
notification is queued with a copy of the email address, so the confirmation can still be
sent after the row is gone.

Errors: `400` password missing, `401` password wrong.

## Notifications

The signed-in user's channels ([notifications design](../design/notifications.md) §3).
The gateway only records codes and switches; `mda-notifier` sends the mail and runs the
Telegram bot.

### Channels — `GET /notifications` 🔒

```json
{"status": "success", "data": {
  "email": {"address": "si***@gmail.com", "verified": false, "enabled": true},
  "telegram": {"available": true, "linked": false, "enabled": false},
  "phone_verified": false}}
```

`address` is masked. `telegram.available` is false when `notifier.telegram_bot` isn't set.
A channel with no switch stored is on.

### Email code — `POST /notifications/email/code` 🔒

Mails a 6-digit code to the account's address (an `account.email_code` outbox row).
`200` → `{"expires_in": 900}`. Errors: `409` already verified, `429` asked again within
60 s, `400` no address.

### Verify email — `POST /notifications/email/verify` 🔒

`{"code": "482913"}` → `200` `true`, and the email is verified. `400` wrong, expired or used
code; the fifth wrong guess cancels the code.

### Telegram link — `POST /notifications/telegram/link` 🔒

`200` → `{"url": "https://t.me/<bot>?start=<code>", "expires_in": 600}`. Opening it and
tapping Start links that chat to the account (the bot then offers "Share phone number",
which verifies a matching mobile number). A new link replaces an unused one. `404` when
Telegram isn't set up.

### Disconnect Telegram — `DELETE /notifications/telegram` 🔒

`200` `true`.

### Switch a channel — `PUT /notifications/{email|telegram}` 🔒

`{"enabled": false}` → `200` `true`. `404` for Telegram when it isn't connected.

## Instruments

Instruments listed today (from the broker's daily instrument list). `instrument_id` is
md_analytics' own permanent id: it never changes, unlike the broker's daily token.

```json
{
  "instrument_id": 1234, "exchange": "NSE", "market": "FO", "kind": "OPT",
  "underlying": "NIFTY", "trading_symbol": "NIFTY26O2725000CE", "name": "NIFTY",
  "expiry": "2026-10-27", "strike": 25000.0, "option_type": "CE",
  "lot_size": 75, "tick_size": 0.05, "listed": true
}
```

`kind` is `IDX`, `EQ`, `FUT` or `OPT`. `expiry`, `strike` and `option_type` are `null`
when they don't apply. `listed` is `false` for a contract that has expired or been
delisted; it stays in watchlists that already hold it, but search no longer returns it.

### Search — `GET /instruments/search?q=&limit=` 🔒

Every word of `q` must match the symbol, company name, exchange, kind, strike or expiry,
case-insensitively and in any order. For example: `reliance`, `hdfc bank`,
`nifty 25000 ce`, `banknifty nov fut`, `nifty 27 oct`. Results come back in this order:
exact symbol match, then index, equity, future, option; NSE before BSE; nearest expiry,
then strike. `limit` defaults to 20 (max 50). An empty `q` returns `[]`.

`200` → `{"status": "success", "data": [instrument, …]}`

### One instrument — `GET /instruments/{instrument_id}` 🔒

`200` → `{"status": "success", "data": instrument}`. Errors: `400` id not numeric,
`404` unknown.

### Chart bars — `GET /instruments/{instrument_id}/bars?interval=&from=&to=` 🔒

History for charts from `bars.db` ([ADR-0021](../adr/0021-bar-storage.md)): our own live 1-minute
bars plus Kite backfill, aggregated on the fly.

- `interval`: `1m`, `3m`, `5m` (default), `15m`, `30m`, `1h`, `1d`. Intraday buckets start at
  09:15 IST like Kite's (5m: 09:15, 09:20 …; 1h: 09:15, 10:15 …). Daily bars are IST days,
  stamped 00:00 IST.
- `from`, `to`: Unix seconds. Defaults: up to now. 1m → 2 days back, 3m/5m → 7 days,
  15m/30m → 30 days, 1h → 60 days, 1d → 400 days.
- At most 10,000 bars (the latest).

```json
{"status": "success", "data": {"instrument_id": 18487, "interval": "5m",
  "bars": [[1792662000, 22775.0, 22779.0, 22772.0, 22772.0, 2015, 5887570,
            5880000, 5887570, 5879100, 3, 1792662281, "long_buildup", 7570], …]}}
```

Each row is, oldest first:

| # | Field | Meaning |
|---|---|---|
| 0–5 | `time, open, high, low, close, volume` | the price candle |
| 6 | `oi` | OI at the bar's close (0 for instruments without OI) |
| 7–9 | `oi_open, oi_high, oi_low` | the OI candle |
| 10 | `oi_updates` | times OI changed in the bar (backfilled minutes: 1 when OI differs from the minute before) |
| 11 | `oi_time` | when OI last changed, Unix seconds (receive time); `null` when unknown |
| 12 | `buildup` | `long_buildup`, `short_buildup`, `short_covering`, `long_unwinding`, `neutral`, `no_oi_update`; `null` when unknown (no OI, or nothing earlier to compare with) |
| 13 | `oi_change` | the OI change `buildup` was measured on (from the last earlier bar with an OI update); `null` with no OI update or nothing to compare with — the height of the chart's ΔOI bars |

`buildup` follows the [OI buildup chart](../design/oi-buildup-chart.md) rule: price and
OI change from the last earlier bar of the same interval that had an OI update (bars
before `from` are read for this), `neutral` below the larger of 0.1% of OI and one lot.
The bucket still in progress gets a provisional state that may change until it closes;
every other bar's state is fixed.

The minute in progress isn't included; it arrives on the stream (`mode: "bars"`).

## Option chains

Underlyings and expiries come from `feed.chains` in `config/mda.yaml` (ADR-0020); live
values arrive on the stream: quotes for the rows' instruments (with IV, Greeks, OI change
and buildup) and `mode: "chain"` for the summary.

MCX chains (ADR-0031) have no spot. Each expiry is priced on its own underlying future, and
`spot_id` is that future's id: in `GET /chains`, the nearest expiry's future; in strike rows,
the requested expiry's. Chain summaries for an MCX expiry are keyed by that future's id. The
OI views' minutes count from the MCX open at 09:00 (`session_open`).

Instruments carry `exchange` `"MCX"` for commodities. MCX quantities, including `lot_size`,
are in the units the price is quoted in (barrels, 10 g of gold). MCX volume, OI and depth
are in lots, as Kite sends them.

### Chains — `GET /chains` 🔒

```json
{"status": "success", "data": [
  {"underlying": "NIFTY", "spot_id": 18274, "expiries": ["2026-10-27", "2026-11-03", "2026-11-09"]}
]}
```

### Strike rows — `GET /chains/{underlying}/{expiry}?strikes=20` 🔒

`strikes` (1–60, default 20) strikes either side of ATM. ATM is the engine's (nearest the
forward) when it has a summary, else nearest the spot's last price.

```json
{"status": "success", "data": {"underlying": "NIFTY", "expiry": "2026-10-27", "spot_id": 18274,
  "lot_size": 65, "atm_strike": 22700,
  "rows": [{"strike": 22650, "call": 19151, "put": 19152}, {"strike": 22700, "call": 19153, "put": 19154}, …],
  "summary": {"forward": 22713.45, "atm_strike": 22700, "atm_iv": 13.97, "pcr": 0.89, …}}}
```

`call` / `put` are `instrument_id`s (`null` if a side isn't listed); `summary` has the shape
of the stream's `chain` message, or `null` before the engine's first one. 404 for an
underlying that isn't configured or an expiry with no listed options.

### OI change by strike — `GET /chains/{underlying}/oi-change?expiries=&from=&to=&minutes=&strikes=10` 🔒

Call and put OI per strike at two times, from the minute bars (design:
`docs/design/oi-change-by-strike.md`).

| Parameter | Meaning |
|---|---|
| `expiries` | Comma-separated, each one of the chain's expiries from `GET /chains`; OI is summed over them per strike. Default: the nearest |
| `from`, `to` | Unix seconds, snapped down to the minute, on the same IST day. Default `to`: `latest`; default `from`: 09:15 IST of `to`'s day |
| `minutes` | Instead of `from`: the last N minutes (1–1440) before `to`, never before 09:15 (the view's "Last 30 mins") |
| `strikes` | 1–20 either side of ATM (default 10); ATM is the strike nearest the spot at `to`, over all the ticked expiries' strikes |

"OI at T" is the OI of each option's last minute bar that ended by T and in which OI was
published (a live OI update, or a backfilled bar), up to 7 days back: OI at 09:15 is the
previous session's last OI. Between updates (SENSEX options get one about every 3 minutes)
the value is the last published one, and `oi_as_of` its time.

```json
{"status": "success", "data": {"underlying": "NIFTY", "expiries": ["2026-10-27"],
  "from": 1792640700, "to": 1792648200, "session_open": 1792640700, "latest": 1792648200,
  "oi_as_of": 1792648177, "spot_from": 22687.65, "spot_to": 22538.0, "atm_strike": 22550,
  "totals": {"call_from": 101326615, "call_to": 116312560, "put_from": 119887040, "put_to": 121002505},
  "pcr": 1.04,
  "rows": [{"strike": 22000, "call_from": 848250, "call_to": 866320, "put_from": 15060240, "put_to": 15680795}, …]}}
```

- Values are shares (contracts × lot). A side is `null` when no ticked expiry has OI for it at
  that time (e.g. a strike subscribed later in the day).
- `totals` and `pcr` (put ÷ call OI at `to`) count only sides known at both times;
  `pcr` is `null` without call OI.
- `latest` is the end of the spot's last closed minute (the newest `to` with data), and
  `session_open` is 09:15 IST of `to`'s day; they bound a time slider.
- `oi_as_of` is the newest OI update time among the rows at `to`, or `null`.
- `400` for an expiry that isn't one of the chain's, a bad number, `strikes` outside 1–20,
  `minutes` outside 1–1440 or given with `from`, or `from` not before `to` on the same day. `404` for an unknown underlying, a chain with no
  bars yet, or no spot price at `to`.

### OI timeline — `GET /chains/{underlying}/oi-timeline?expiries=&strikes=10` 🔒

The session's OI per strike as step series, for a view that computes any window itself
(design §8). `expiries` and `strikes` as for `oi-change`; the strikes are ATM ± N at the
latest minute; the session is the one `latest` falls in.

```json
{"status": "success", "data": {"underlying": "NIFTY", "expiries": ["2026-10-27"],
  "session_open": 1792727100, "latest": 1792736400, "oi_as_of": 1792736377, "atm_strike": 22450,
  "spot": [[0, 22711.3], [1, 22687.65], [2, 22690.1], …],
  "rows": [{"strike": 22350, "call": [[0, 169520], [3, 171340], …], "put": [[0, 2173405], …]}, …]}}
```

- A point `[m, value]` means "from 09:15 + m minutes (the end of that minute's bar) the
  value is this", until the next point; `m = 0` is the value before the open (the previous
  session's last). Points appear only when the value changes.
- OI is in shares, summed over the ticked expiries; a side with no data has `[]`, and a side
  that gets its first value later in the day starts at that `m`.
- Errors as for `oi-change` (no `from`/`to`/`minutes`).

## Market signals

Direction signals for the indices from mda-signald (ADR-0028, `docs/design/signals.md`):
one per index, strategy and minute, with the factors behind it and, once its horizon has
passed, what the index did. Indices and strategies come from `signals` in
`config/mda.yaml`. Live signals arrive on the stream with `mode: "signals"`.

### Indices and strategies — `GET /signals` 🔒

```json
{"status": "success", "data": {
  "indices": [{"underlying": "NIFTY", "spot": "NSE|CASH|IDX|NIFTY 50", "spot_id": 18274,
               "latest": [{"minute": 1792749540, "strategy": "oi-bias-v1", "score": 10,
                           "label": "neutral", "…": "as in a day's signals"}]}],
  "strategies": [{"id": "oi-bias-v1", "horizon_min": 30, "bullish": 25, "bearish": -25}],
  "factors": ["futures_buildup", "atm_oi_shift", "pcr_change", "future_vs_vwap",
              "atm_iv_change", "max_pain_pull", "sector_breadth", "vix_change",
              "volume_delta"]
}}
```

`latest`: the newest minute's signals (one per strategy; empty when there are none), so a
page has a value before the stream sends one.

### A day's signals — `GET /signals/{underlying}?date=YYYY-MM-DD` 🔒

One IST day, oldest first, every strategy; without `date`, the latest day that has
signals (`date` is `null` and `signals` empty when there are none). 404 for an underlying
without signals.

```json
{"status": "success", "data": {"underlying": "NIFTY", "date": "2026-10-23", "signals": [
  {"minute": 1792727160, "strategy": "oi-bias-v1", "horizon_min": 30, "score": -32,
   "label": "bearish", "prob": null, "spot": 22479.55,
   "factors": [{"factor": "futures_buildup", "status": "present", "value": 0.0, "points": 0.0},
               {"factor": "max_pain_pull", "status": "not_applicable", "value": 0.0, "points": 0.0}],
   "outcome": {"spot_after": 22451.2, "return_bp": -13, "hit": true}}
]}}
```

- `minute`: the minute's start (Unix seconds); the signal uses bars closed by its end.
- `score` −100…+100; `label` `bullish` / `neutral` / `bearish` by the strategy's thresholds.
- `prob`: calibrated P(up), `null` until the strategy is calibrated.
- `factors[].status`: `present`, `absent` (no data yet) or `not_applicable`; `points` is
  the factor's share of the score.
- `outcome`: `null` until the horizon has passed (or the session closed). `hit` is `null`
  for neutral signals and for moves within ±`signals.flat_bp` (flat).

### Track record — `GET /signals/{underlying}/record?days=30` 🔒

Each strategy's record over the last `days` calendar days (1–365, today included), on
signals with an outcome.

```json
{"status": "success", "data": {"underlying": "NIFTY", "from": "2026-09-25", "days": 30,
  "strategies": [{"strategy": "oi-bias-v1", "horizon_min": 30, "settled": 374,
    "bullish": 0, "bearish": 240, "neutral": 134, "hits": 87, "misses": 97, "flats": 56,
    "hit_rate": 0.473, "always_up_rate": 0.462, "momentum_rate": 0.522,
    "momentum_calls": 184}]}}
```

- `hit_rate` = hits / (hits + misses); `null` with no decided calls.
- Baselines, on the same minutes: `always_up_rate` (share of those moves that were up)
  and `momentum_rate` (the previous horizon's direction was right; over `momentum_calls`).
  A strategy that doesn't beat both has no edge.

## Paper trading

Simulated orders against the live market (ADR-0029, `docs/design/paper-trading.md`): the
gateway sends them to mda-broker, which checks risk, fills them from the live order book and
charges them like Zerodha. Nothing reaches a real broker. Each user has one paper account,
created with ₹10 lakh (`paper.opening_cash`) on their first order. F&O only: index futures
and buying options. Live updates arrive on the stream with `mode: "orders"`.

**Another user's paper (admins):** the reads below (`/paper/orders`, `/paper/trades`,
`/paper/positions`, `/paper/funds`, `/paper/auto`) take `user=<user_id>` (from
`GET /admin/users`) to show that user's account instead of one's own. Only an admin may pass
it: anyone else gets 403 `PermissionException`; a non-numeric id gets 400. Placing,
modifying and cancelling orders and the kill switch ignore `user`, so they always act on the
caller's own account: an admin can look but can't trade for someone else.

### Place an order — `POST /paper/orders` 🔒

```json
{"instrument_id": 18486, "side": "BUY", "quantity": 65, "type": "LIMIT", "price": 22650,
 "trigger_price": null, "product": "NRML", "tag": "breakout", "client_order_id": 1792856203937001}
```

- `type`: `MARKET` (default), `LIMIT` (`price`), `SL` (`price` and `trigger_price`), `SL-M`
  (`trigger_price`). `product`: `MIS` (default in the API; intraday) or `NRML` (the web
  ticket's default). `quantity` in units, a multiple of the lot size.
- `client_order_id` (optional, a positive integer): makes retries safe. Sending the same id
  again returns that order, never a second one. Without it the gateway picks one.
- Answers once mda-broker has accepted or rejected the order (it resends meanwhile):
  - `200` `{"order": {…an execution report…}}` with `event` `accepted` (or `status` for a
    repeat);
  - `400` `OrderException` with the risk check's reason, e.g. "insufficient funds: needs
    ₹202500.45, available ₹200000.00";
  - `202` `{"client_order_id": …, "status": "PENDING"}` if mda-broker didn't answer within
    2 s; send again with the same `client_order_id` to learn the outcome.

An execution report:

```json
{"order_id": 12, "client_order_id": 1792856203937001, "instrument_id": 18486, "event": "fill",
 "status": "COMPLETE", "side": "BUY", "type": "LIMIT", "product": "NRML", "source": "ui",
 "quantity": 65, "filled": 65, "last_quantity": 65, "last_price": 22650.0, "average_price": 22650.0,
 "price": 22650.0, "trigger_price": null, "charges": 86.12, "reason": "", "reject_code": null,
 "at": 1792728733000}
```

`event`: `accepted`, `rejected`, `triggered`, `fill`, `cancelled`, `modified`, `status`,
`kill_switch_on`, `kill_switch_off`, `settled`, `converted`. `status`: `TRIGGER_PENDING`, `OPEN`, `COMPLETE`,
`CANCELLED`, `REJECTED`. `charges` are this fill's, in rupees. `reject_code` names the
risk check (`bad_quantity`, `funds`, `price_band`, `kill_switch`, …).

### Modify — `PUT /paper/orders/{order_id}` 🔒 and cancel — `DELETE /paper/orders/{order_id}` 🔒

`PUT` takes `{"quantity": 130, "price": 22640, "trigger_price": null}`: the order is checked
by risk again and looks at the book afresh. Both answer like a placement (`event`
`modified` / `cancelled`); `400` when the order is no longer open or the change is rejected.

### Kill switch — `POST /paper/kill-switch` 🔒

`{"on": true}` cancels the account's open orders; until `{"on": false}` only orders that
reduce a position are accepted. Answers `{"kill_switch": true}`.

### Convert a position — `POST /paper/positions/convert` 🔒

`{"instrument_id": 73371, "quantity": 100, "to": "NRML"}` moves that much of the position from
the other product (MIS ↔ NRML), as Zerodha's position conversion. It keeps the average price
and has no charges; margin is the same for both products. Allowed while the market is open,
into MIS only until the square-off (15:25, MCX 5 minutes before its close). Answers like a
placement: `{"order": {…}}` with `event` `converted`, `order_id` 0, `product` the new one,
`side` the position's (`BUY` long), `last_quantity`, and `last_price` the average. `400`
`OrderException` when the other product holds less than `quantity`, or outside those hours.

### Orders and trades — `GET /paper/orders?date=` 🔒, `GET /paper/trades?date=` 🔒

One IST day (`YYYY-MM-DD`, default today), oldest first.

```json
{"order_id": 12, "client_order_id": 1792856203937001, "instrument_id": 18486, "side": "BUY",
 "type": "LIMIT", "placed_type": "LIMIT", "product": "NRML", "quantity": 65, "filled": 65,
 "price": 22650.0, "trigger_price": null, "average_price": 22650.0, "status": "COMPLETE",
 "reason": "", "tag": "breakout", "source": "ui", "charges": 86.12,
 "created_at": 1792728710000, "updated_at": 1792728733000}
```

A trade: `{"order_id", "instrument_id", "side", "product", "quantity", "price", "charges":
{"brokerage", "stt", "exchange", "sebi", "stamp", "gst", "total"}, "realised", "at"}` (rupees).

### Positions — `GET /paper/positions` 🔒

```json
[{"instrument_id": 18486, "product": "NRML", "quantity": 65, "average_price": 22650.0,
  "entry_price": 22650.0, "last_price": 22633.6, "realised": 0.0, "closed_pnl": 0.0,
  "unrealised": -1066.0}]
```

As Kite shows them: open positions (whenever opened), and those traded or settled at expiry
today. `realised` is today's: the sum of today's trades, today's expiry settlement and today's
daily MTM. After the MTM, a carried future's `average_price` is the last close it was marked
to, and `unrealised` counts from it. `entry_price` is the average of the fills behind the open
quantity, which marks don't change; `null` when flat. `closed_pnl` is today's closing fills
measured from their entry price, plus today's expiry settlement. The web's P&L is
`closed_pnl + (LTP − entry_price) × quantity`.
Positions closed on an earlier day are left out (they stay in `paper.db`; their orders and
trades are under `?date=`). `last_price` and `unrealised` are `null` when the gateway has
no live quote.

### Funds — `GET /paper/funds` 🔒

```json
{"opening_cash": 1000000.0, "cash": 999913.88, "blocked": 176670.0, "available": 822177.88,
 "realised_today": 0.0, "charges_today": 86.12, "unrealised": -1066.0, "kill_switch": false}
```

`blocked`: margin of open positions (futures 12% of notional, bought options their premium)
and open orders. `available` = cash − blocked + unrealised losses (gains don't count).

### Auto-trader runs — `GET /paper/auto?days=30` 🔒

The signed-in user's runs of mda-autotrader (ADR-0030, `autotrader.runs` in config; enabled
or not), measured over the last `days` (1–365, today included) from their tagged orders.

```json
{"status": "success", "data": {"from": "2026-09-25", "days": 30, "runs": [
  {"id": "nifty-follow", "strategy": "follow-signal", "signal": "oi-bias-v1", "underlying": "NIFTY",
   "enabled": true, "settings": {"lots": 1, "product": "MIS", "stop_points": 40, "target_points": 80,
   "max_hold_min": 45, "max_trades_per_day": 6, "cooldown_min": 10, "exit_score": 10, "daily_loss_stop": 5000},
   "position": 0, "held": [], "entries_today": 3,
   "metrics": {"round_trips": 3, "wins": 2, "losses": 1, "win_rate": 0.667, "gross": 3321.5,
     "charges": 2631.09, "net": 690.41, "average_win": 2098.37, "average_loss": -3506.33,
     "largest_loss": -3506.33, "max_drawdown": 3506.33, "exits": {"signal": 1, "stop": 1, "target": 1}},
   "decisions_today": [{"at": 1792731717000, "state": "flat", "action": "enter short",
     "reason": "Bearish -73", "price": 22489.5}]}
]}}
```

- `metrics` (rupees): round trips are flat → position → flat; a trip with net ≤ 0 (after
  charges) is a loss. `gross`, `charges` and `net` cover every fill, an open trip included;
  `max_drawdown` is the largest fall of cumulative net P&L over closed trips. `exits`
  counts the run's exits by reason: `signal`, `target`, `index stop` (options), `max hold`,
  `end of day`, `stop` (the resting stop filled), `stop lost`, `flatten`.
- `position` is the net quantity over the run's trades; `held` lists what it holds now per
  instrument, `[{"instrument_id": 61234, "quantity": 65}]` (empty when flat). An options run
  (`strategy: follow-signal-options`) has `index_stop_points`, `premium_stop_percent`,
  `entry_slip_ticks` and `entry_timeout_s` in `settings` instead of `stop_points`.
- `decisions_today`: newest first, at most 50: every action and blocked entry with its
  reason (`state` is the run's state before it).

## Watchlists

Each user has up to 10 lists of up to 250 instruments, and each list up to 20 named
**groups** (like Kite's "Indices", "Options"). A user with no lists gets an empty one called
"My watchlist" on their first `GET`. Another user's list behaves exactly like a list that
doesn't exist (`404`).

```json
{
  "id": 7, "name": "Banks", "created_at": "2026-10-22T03:41:00Z",
  "groups": [ {"id": 3, "name": "Indices"}, {"id": 4, "name": "PSU"} ],
  "items": [ {…instrument fields…, "added_at": "2026-10-22T03:41:05Z", "group_id": 3} ]
}
```

`groups` and `items` are in display order. An item's `group_id` is `null` when it is
ungrouped; the UI shows ungrouped items first, then each group. A new item goes to the end
of its group.

| Method and path | Body | Success |
|---|---|---|
| `GET /watchlists` 🔒 | — | `200`, all lists with items |
| `POST /watchlists` 🔒 | `{"name": "Banks"}` | `201`, the new list |
| `PUT /watchlists/{id}` 🔒 | `{"name": "PSU banks"}` | `200`, the renamed list |
| `DELETE /watchlists/{id}` 🔒 | — | `200`, `true` |
| `POST /watchlists/{id}/items` 🔒 | `{"instrument_id": 1234, "group_id": 3}` (`group_id` optional) | `201`, the updated list |
| `DELETE /watchlists/{id}/items/{instrument_id}` 🔒 | — | `200`, the updated list |
| `POST /watchlists/{id}/groups` 🔒 | `{"name": "Indices"}` | `201`, the updated list |
| `PUT /watchlists/{id}/groups/{group_id}` 🔒 | `{"name": "Index"}` | `200`, the updated list |
| `DELETE /watchlists/{id}/groups/{group_id}` 🔒 | — | `200`, the updated list (its items become ungrouped) |
| `PUT /watchlists/{id}/order` 🔒 | see below | `200`, the updated list |

**Order** replaces the whole arrangement at once. It is used after drag and drop or a sort:

```json
{"groups": [4, 3],
 "items": [{"instrument_id": 59350, "group_id": null}, {"instrument_id": 18489, "group_id": 4}]}
```

It must list exactly the list's current groups and items, each once. If it doesn't (for
example the list changed in another tab) the answer is `409`; reload and retry.

Errors:
- `400` (`InputException`): a name that is empty or longer than 40 characters, or an instrument that is unknown or not listed today;
- `404`: the list, group or item doesn't exist;
- `409` (`ConflictException`): a duplicate list or group name, an instrument already in the list, or an out-of-date order.

Going over a limit is also a `400`, with a message such as `"at most 10 watchlists"`.

## Admin 🔒 (admin role)

For users whose `role` is `admin` ([ADR-0019](../adr/0019-admin-dashboard.md)). Everyone
else gets `403` (`PermissionException`). Roles are set only from the command line:

```bash
build/release/engine/apps/tools/mda-useradm grant-admin <username>
```

Contact details are masked (`si***@example.com`, `+91•••••43210`). Times are UTC ISO-8601.
"Data" is what the gateway sent (WebSocket payloads + HTTP responses, without TLS/TCP
overhead), counted per Indian trading day.

### Users — `GET /admin/users`

```json
{"status": "success", "data": [
  {"user_id": 2, "username": "friend", "email": "fr***@example.com", "phone": "+91•••••43210",
   "role": "user", "created_at": "…", "last_sign_in_at": "…", "last_sign_out_at": null,
   "last_seen_at": "…", "active_sessions": 1, "online": 1, "streaming_instruments": 12,
   "today": {"ws_bytes": 111398, "ws_messages": 185, "http_bytes": 22916,
             "http_requests": 24, "total_bytes": 134314}}
]}
```

`online` is the number of live streams open now. `active_sessions` counts sessions that
haven't expired.

### One user — `GET /admin/users/{id}`

`{"user": {…as above…}, "sessions": [{"id", "created_at", "last_seen_at", "user_agent",
"ip"}], "events": [{"type", "at", "ip", "user_agent"}], "usage": [{"day", …usage…}],
"connections": [{"since", "instruments"}]}`

- `events` are the last 50, newest first. The types are `sign_in`, `sign_in_failed`,
  `sign_out`, `password_changed`, `session_expired`, `session_revoked` and
  `account_deleted`. They are kept for 90 days.
- `usage` covers the last 30 days.
- A session's `id` is the hex SHA-256 of its token. The token itself is never shown.

### Force sign-out — `DELETE /admin/sessions/{session_id}`

`200` → `true`. The session ends at once, and an open live stream closes within 60 s.
`404` if there is no such session.

### System monitor — `GET /admin/system`

The stack's health (design: `docs/design/system-monitor.md`): sampled by the gateway every
5 s, with the last hour of trend values. `503` before the first sample (just after the
gateway starts).

| Key | What |
|---|---|
| `processes` | `tools/stack.sh`'s six (feedd, engine, gateway, notifier, recorder, web): `running`, `pid`, `started`, `cpu_percent` (since the last sample; can exceed 100 on several cores), `rss_bytes` (includes the mapped buses; "web" adds the dev server's child processes) |
| `machine` | `load1`, `cores`, `memory_used` / `memory_total`, `power` (`ac` / `battery`), `battery_percent`, `uptime_s` |
| `disk` | `free` / `total` of the data volume, `files` (`bars_db`, `app_db`, `last_quotes_db` bytes), `journal_today`, `journal_all` |
| `buses` | Per bus: `available`, `capacity`, `writer_alive`, `writer_pid`, `heartbeat_age_ms`, `bytes_per_s`, `messages_per_s`, `readers` (running readers only: `name`, `pid`, `alive`, `lag_bytes`, `lag_percent` of capacity, `laps`, `lost_bytes`, `heartbeat_age_ms`), `stale_slots` (slots of readers that stopped) |
| `feed` | `state` (`connecting`, `connected`, `disconnected`, `stale`, `auth_expired`; `null` before a status), `subscribed`, `reconnects` (since mda-feedd started), `ticks_per_s`, `frames_per_s`, `last_tick_age_ms` |
| `latency` | `feed_to_gateway_us` (from mda-feedd reading the frame to the gateway reading the snapshot) and `exchange_to_feed_s` (whole seconds): `count`, `p50`, `p99`, `max` over the last 5 s |
| `engine` | `bars_per_min` (closed minute bars over the last 60 s; `null` in the gateway's first minute), `last_bar` (its minute, Unix s), `analytics_per_s` |
| `recorder` | `enabled`, `running`, `journal_today`, `journal_all`, and from its status file (written each minute): `gaps_today`, `records_last_minute`, `status_age_s` |
| `notifier` | `queued`, `sent_today`, `failed_today`, `oldest_queued_s` |
| `history` | `step_s` (5), `start` (Unix s of the first value), and arrays `load1`, `ticks_per_s`, `md_max_lag_pct`, `feed_to_gateway_p99_us` |
| `checks` | `[{"card", "level", "text"}]`, only the ones not ok; `level` is `warn` or `bad` (thresholds in the design §5) |
| `status` | The worst level: `ok`, `warn` or `bad` |

`time` is the sample's Unix time; `market_hours` says whether it fell in 09:15–15:30 IST on a
weekday (some checks apply only then).

## Example with curl

```bash
curl -s -X POST http://127.0.0.1:8090/api/v1/user/signup -H 'Content-Type: application/json' -d '{"username":"siva","email":"siva@example.com","phone":"9876543210","password":"correct horse battery"}'
```

```bash
curl -s -c cookies.txt -X POST http://127.0.0.1:8090/api/v1/session -H 'Content-Type: application/json' -d '{"username":"siva","password":"correct horse battery"}'
```

```bash
curl -s -b cookies.txt http://127.0.0.1:8090/api/v1/user/profile
```

```bash
curl -s -b cookies.txt 'http://127.0.0.1:8090/api/v1/instruments/search?q=nifty%20fut'
```

```bash
curl -s -b cookies.txt http://127.0.0.1:8090/api/v1/watchlists
```

## Coming next

Session phases (pre-open and the closing auction): reference / indicative prices,
imbalance and the auction price band.
