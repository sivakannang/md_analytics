# Paper trading: mda-broker, the paper venue and pre-trade risk

Design for ADR-0029. Paper accounts for the owner and the friend: orders from the web app
(and later the auto trader, ADR-0030) go through pre-trade risk to an OMS that fills them
against the live Kite book, charges them like a real broker, and keeps positions, P&L and a
ledger. Nothing reaches a real order API.

## 1. Goals and non-goals

- **Goals:** realistic paper fills and costs; one OMS and one risk gate for every order
  source; positions, P&L and funds per user; deterministic, replayable broker state; the
  first broker piece of the ETT platform (`docs/architecture/ett-platform.md` §3.3).
- **Non-goals (v1):** real orders; option writing (needs a SPAN-like margin model);
  equities; after-market orders; GTT; brackets and covers; market impact beyond the 5
  visible levels.

## 2. Processes and data flow

```
                  md bus (quotes, 5-level depth, from mda-feedd)
                          │
web app ─REST─► mda-gateway ──ord-gw──►┐            ┌──exec──► mda-gateway ─stream─► web app
                                       ▼            │
mda-autotrader (ADR-0030) ──ord-auto──► mda-broker ──┤
                                       │  OMS ─► RMS ─► Venue (v1: paper venue)
                                       ▼            └──exec──► mda-autotrader
                                 data/paper.db (event log + state; single writer)
                                       ▲
              mda-gateway (REST reads), mda-feedd (subscriptions) read it
```

- **mda-broker**: one thread. Each loop: read order requests from `ord-gw` / `ord-auto`
  and market data from `md`; apply them in arrival order; persist (one transaction:
  event rows plus state rows); publish execution reports on `exec`.
- **Persist, then publish.** `paper.db` is the truth. A restart rebuilds state from its
  tables; a client that lost messages (lapped, or restarted) re-reads orders from REST.
- **Subscriptions:** mda-feedd also subscribes every instrument with an open order or an
  open position (read from `paper.db`, like watchlists from `app.db`), so fills and P&L
  always have live data.

## 3. Accounts, funds and ledger

- One paper account per user, created on first use with **₹10,00,000** virtual cash
  (config); the user can reset it (closes everything, restores the cash, keeps history
  under the old account id).
- **Funds:** cash, margin blocked (open orders and positions), available = cash − blocked
  + unrealised P&L (losses only, like brokers), realised P&L today, charges today.
- **Ledger:** every cash movement is a row: opening balance, realised P&L per closing
  fill, charges per fill, expiry settlement, reset. Cash = sum of the ledger.

## 4. Orders

**Fields:** account, source (`ui` / `auto`), client order id, instrument, side (BUY / SELL),
quantity (units; a multiple of the lot size), type (MARKET, LIMIT, SL, SL-M), price
(LIMIT, SL), trigger price (SL, SL-M), product (MIS, NRML), validity DAY, tag (free text).

**Validation (before risk):** instrument listed today, F&O (NFO/BFO), future or option, not
expired; quantity a positive multiple of the lot size and ≤ the exchange freeze quantity
(config per underlying); prices positive multiples of the tick size; SELL of an option
only to reduce a long position (no writing in v1); market open (09:15–15:30 IST, F&O
orders after 15:30 rejected).

**States** (an execution report on every change):

| From | Event | To |
|---|---|---|
| — | accepted by validation and risk | OPEN (or TRIGGER_PENDING for SL / SL-M) |
| — | rejected | REJECTED (reason) |
| TRIGGER_PENDING | LTP crosses the trigger | OPEN (SL becomes LIMIT, SL-M becomes MARKET) |
| OPEN | partial fill | OPEN (filled qty, average price) |
| OPEN | full fill | COMPLETE |
| OPEN / TRIGGER_PENDING | cancel by the user, the kill switch or the close | CANCELLED |
| OPEN / TRIGGER_PENDING | modify (qty, price, trigger) | the same state, re-checked by risk |

**Idempotency:** the client order id (64 bits, chosen by the source) is unique per account;
a repeated request gets the existing order's latest report (event Status), nothing else.
**Sources resend**: a reader that attaches late misses earlier messages (the replay check
on 2026-10-24 lost the very first order, sent as `mda-paperctl` created the bus), so a
source sends again with the same client order id until a report for it arrives
(`mda-paperctl`: every second, three tries; the gateway in PT4 the same).

## 5. The paper venue (fill model)

Inputs: the latest MarketSnapshot and DepthSnapshot per instrument from `md`.

- **Staleness:** no quote in the last 5 s, or the feed not connected → MARKET orders are
  rejected and resting orders don't fill (they wait).
- **Latency:** an order becomes eligible to fill `latency_ms` after acceptance (default
  100 ms), so it can't trade on a book it couldn't have seen.
- **MARKET:** walks the opposite side of the 5-level depth (BUY takes asks from the best
  up). A remainder beyond 5 levels fills at the 5th level's price plus `overflow_ticks`
  (default 2) ticks. No depth on that side → reject ("no liquidity").
- **Marketable LIMIT** (BUY at or above the ask): fills like MARKET but never past the
  limit; the rest rests.
- **Resting LIMIT:** fills only when the market **trades through** it: a BUY fills at its
  limit when the LTP trades below the limit or the ask falls below it (touching isn't
  enough: we don't know our queue position). Fill size: the whole order (v1).
- **SL / SL-M:** trigger when the LTP reaches the trigger (BUY: LTP ≥ trigger; SELL:
  LTP ≤ trigger); then as LIMIT / MARKET above.
- **Determinism:** fills depend only on the order of inputs; a journal replay (ADR-0026)
  with the same orders gives the same fills.
- **Code:** `libs/oms/paper_venue.hpp`. `PaperVenue::evaluate(order, book, now)` says what
  an order does now (trigger, fills, or a MARKET reject); the broker applies it. A LIMIT
  takes what it can on its first look (after its latency, or right after its trigger) and
  rests from then on. Times are Unix milliseconds.

## 6. Positions and P&L

- **Net position** per account, instrument and product: quantity, average price (weighted
  average of the opening fills, as Kite shows), realised P&L (on reducing fills), and
  unrealised P&L at the LTP.
- **Daily mark-to-market** (as Zerodha settles futures every night): at 15:41 (after F&O's last
  print at 15:40), or a minute after the MCX close, every open futures position not expiring
  that day settles its P&L against the future's last close into cash (ledger kind `mtm`). It
  then carries on with that close as its average, so the next day's P&L counts from it.
  Options aren't marked, since their premium is paid in full. Running it again finds nothing,
  so a restart can't mark twice.
- **Kite's previous close** corrects those marks. Every quote carries it (`close`). During an
  instrument's session, an open future with no fill today settles against it and takes it as
  its average. This covers a close the broker missed (the Mac off) and a mark made on the
  last price recorded before the Mac slept, and it also moves a normal mark from the last
  trade to Kite's close. A fill today leaves the average to that night's mark. A close seen
  before the open is ignored, and after the market shuts the night's own mark takes over.
- **Entry price:** each position also keeps the average of the fills behind its open
  quantity (`entry_cost_raw`). Marks leave it alone; reducing, conversion and netting move it
  as they move the average. Migration 2 derived it for the positions open then, by undoing
  their marks. Each closing fill also stores its P&L from the entry price
  (`trades.entry_realised`).
- **The Positions page shows the trader's view:** Avg. is the entry price and P&L is
  (LTP − Avg.) × Qty, plus today's closing fills from their entry price and today's expiry
  settlement. So a carried position's P&L runs from entry and doesn't jump when it's closed.
  Funds keeps the cash view: today's settlements and the P&L from the last mark. A future
  settling at expiry counts in the page's P&L from its last mark only.
- **Catch-up** for an MIS square-off missed while the broker was down (the Mac off at
  15:25 or 23:25): on the next start, and each minute after, an MIS position whose instrument
  has had no fill, MTM or settlement since the instrument's last close before today is closed
  at that close, with no charges (Zerodha would charge its square-off order). Contracts that
  have expired settle as before.
- **Conversion** (MIS ↔ NRML, as Zerodha's): moves quantity between the two positions at its
  average cost, with no charges and no P&L (unless it nets against an opposite position in
  the other product). Allowed while the market is open; into MIS only until the square-off.
  The order ticket defaults to NRML.
- **MIS square-off:** at `mis_square_off` (config, default 15:25 for F&O, as the broker
  does), MIS orders are cancelled and MIS positions closed with MARKET orders; the reports
  say "auto square-off".
- **NRML** positions carry over; each morning they are marked from the previous close.
- **Expiry:** at the close on expiry day, open positions settle at the final settlement
  price: the underlying index's closing value. Futures: (FSP − average) × quantity.
  Options: intrinsic value × quantity (worthless out of the money). Ledger rows record it.
- **Built (PT6), in `Broker::on_tick`:**
  - 15:25 (`paper.mis_square_off`): the traders' MIS orders are cancelled and each MIS
    position is closed by a MARKET order from the broker itself (source `system`). Its
    client order id is made from the day and the instrument, so a restart finds it instead
    of placing another.
  - 15:30: DAY orders expire ("expired at the close"); orders left from an earlier day
    (the broker was down) expire whenever they are found.
  - From 15:31, checked once a minute: positions in contracts expiring today (or before)
    settle at the FSP, the underlying's last 1-minute close of the expiry day from
    `bars.db` (catalog `underlying_id`), with exercise STT on bought options in the money; a
    `settled` report goes out. No FSP yet → tried again a minute later.
  - 15:35 (`paper.daily_summary`): mda-broker queues each trader's day
    (`paper.daily_summary`: fills, realised, charges, net, open positions, cash) in the
    app.db outbox, dedup key per user and date; mda-notifier sends it to their verified
    email and linked Telegram.
- Replayed on 2026-10-23 15:20–15:40: the MIS future was closed by the system at 15:25, the
  NRML limit expired at 15:30, and the summary was queued at 15:35. Expiry settlement is
  covered by unit tests only: the one recorded day isn't an expiry.

## 7. Charges

Per fill, as Zerodha's brokerage calculator computes them (its script was read and its
results checked on 2026-10-24; `libs/oms/charges.hpp`, rates in parts per billion):

| Charge | Futures | Options | Rounded to |
|---|---|---|---|
| Brokerage | 0.03% of turnover, capped at ₹20 per executed order | ₹20 per executed order | paisa |
| STT (sell side) | 0.05% | 0.15% of premium | rupee |
| Exchange (NSE) | 0.00173% + IPFT 0.0001%, each rounded | 0.03503% + IPFT 0.0005%, each rounded | paisa |
| Exchange (BSE) | 0 | 0.0325% | paisa |
| SEBI fee | ₹10 / crore | ₹10 / crore | paisa |
| Stamp duty (buy side) | 0.002% | 0.003% | rupee |
| GST | 18% of brokerage + exchange + SEBI (as rounded) | same | paisa |
| Exercise STT | | 0.15% of intrinsic value (bought options, in the money at expiry) | rupee |

- The cap and the flat brokerage are per **order**: a fill pays what the order hasn't
  paid yet.
- Charges are per fill (as on a contract note); the calculator works on a round trip's
  combined turnover, so totals differ by a few paise (tests pin both).
- A NIFTY futures round trip of one 75-unit lot near 22,500 costs about ₹1,006, roughly
  6 bp of notional: the bar a short-horizon signal has to clear.
- The rates live in `zerodha_2026_10()`; PT3 moves them to config (`paper.charges`, with
  `as_of`) so a rate change is a config edit.

## 8. Margin (v1)

- **Futures:** `margin_pct` of notional per underlying (config, e.g. 12%), blocked on
  order entry for the opening quantity and held while the position is open.
- **Option buying:** the premium is blocked while the position is open (on entry: the
  LIMIT price, or for MARKET the best ask plus a buffer). Cash moves only by realised P&L
  and charges (P&L accounting, `libs/oms/account.hpp`), so blocking the premium has the
  same effect on available funds as paying it.
- An order that would take available funds below zero is rejected.
- **MCX futures** (ADR-0031): the commodity's share of notional,
  `paper.mcx_margin_percent`, taken from Kite's margin calculator. Crude oil is 32%, gold 10%.

### MCX (ADR-0031)

- **Quantities** are in the quotation unit: one crude oil lot is 100. Kite's depth comes in
  lots and is converted for the fill model.
- **Hours:** 09:00 to 23:30, or 23:55 while the US is off daylight saving. The trading window
  keeps the same offsets as NSE's. Opening orders stop 5 minutes before the close, MIS is
  squared off then, DAY orders expire at the close, and settlement runs a minute after it.
- **Charges:** CTT 0.01% (futures) or 0.05% (options) on the sell side, plus MCX's
  transaction charges.
- **Expiry:** an option settles in cash at intrinsic value against its future's last
  price, with no tax on exercise. Zerodha devolves it into the future instead. A future
  settles at its own last price.
- **Tradable** only with a units-per-lot entry (`instruments/mcx.hpp`); other commodities can
  be watched but not traded.

## 9. Pre-trade risk (rms)

Every order from every source, after validation. Limits are per account, with stricter
defaults for the `auto` source:

| Check | ui default | auto default |
|---|---|---|
| Max lots per order | none (the freeze quantity; above it the order is sliced) | 2 |
| Max open lots per instrument | none | 2 |
| Max gross notional (all positions) | none | ₹40 lakh (one NIFTY futures lot is ~₹15 lakh) |
| Price: within the exchange's circuit limits (mda-feedd, every 60 s) | yes | yes |
| Price band: LIMIT within ±x% of LTP (an SL's limit: of its trigger) | none | 1% |
| Orders per minute (re-prices count) | 30 | 30 (four runs, 2026-10-25; was 6) |
| Daily loss limit (realised + unrealised) | 5% of opening cash | 2% |
| Trading window | 09:15–15:25 | 09:20–15:10 |

- **A person's limits are Zerodha's (2026-10-27):** the lot, notional and ±5% caps of v1
  rejected orders Zerodha takes (a buy resting 8% below the market, 20 lots of a NIFTY
  option). Margin, the daily loss limit, the kill switch and the circuit limits remain.
  Stock futures block 20% (`stock_futures_margin_percent`; Zerodha's SPAN + exposure is
  ~15–40%), index futures 12%.
- **Slicing above the freeze quantity:** as Kite does, a person's order above the freeze
  quantity (`paper.freeze_quantity`, NSE/FAOP/76693 from 2026-10-05) becomes legs of at
  most that many whole lots, up to 10, placed together and risk-checked one by one; each
  leg is its own order (tag `slice k/n`, client order id + k·2⁵²) and pays its own
  brokerage. More than 10 legs is rejected.
- **The auto trader's own share (2026-10-27):** for `auto` orders, open lots and gross
  notional count only the auto trader's share of each position: its net filled quantity
  that day (restored from the day's orders after a restart), at most the position. The
  user's own trades in the same account used to fill the ₹40 lakh and blocked every
  auto entry.

- **Daily loss limit hit:** only reducing orders are accepted for the rest of the day
  (and, for `auto`, open positions are closed).
- **Kill switch:** per account (the user, from the UI) and global (admin): cancels open
  orders and accepts only reducing orders until cleared.
- Every rejection has a code and a reason the user sees.
- **Order of checks** (`libs/rms/risk.hpp`): validation (tradable, lot, freeze, tick,
  trigger side, no option writing, market hours) → order rate → for opening orders only:
  window, kill switch, daily loss, lots per order, lots per instrument (counting open
  orders on the same side), price, price band, notional, funds. An order is **reducing**
  when it is opposite to the position in the same product and no larger than it.
- **Funds:** available = cash − blocked margin + unrealised losses (gains don't count).

## 10. Bus messages

Fixed layout, `static_assert` sizes, registered in `MessageTraits` (ADR-0010); exact layouts
in PT3.

- **OrderRequest (10)** on `ord-gw` / `ord-auto`: action (new, modify, cancel), account
  (user id), source, client order id, instrument, side, type, product, quantity, price,
  trigger, a short tag.
- **ExecutionReport (11)** on `exec`: account, client order id, broker order id, status,
  reason code, filled quantity, last fill quantity and price, average price, charges of
  the fill, time; one per state change or fill.
- Layouts: `libs/broker/messages.hpp` (96 and 216 bytes; the report's reason is 96 bytes so
  risk messages with rupee amounts fit). The broker sets a request's source from the bus it
  came on (`ord-gw` → ui, `ord-auto` → auto), never from the message.

## 11. paper.db

```sql
accounts (id, user_id, created_at, opening_cash, status)        -- status: active, closed (reset)
orders   (id, account_id, client_order_id, source, instrument_id, side, type, product,
          quantity, price, trigger, status, filled, average_price, reason, tag,
          created_at, updated_at, UNIQUE (account_id, client_order_id))
trades   (id, order_id, account_id, instrument_id, side, quantity, price, charges, at)
positions(account_id, instrument_id, product, quantity, average_price, realised, updated_at,
          PRIMARY KEY (account_id, instrument_id, product))
ledger   (id, account_id, at, kind, amount, ref)                 -- kind: open, pnl, charges, expiry, reset
events   (seq, at, kind, payload)                                -- every input and output, in order
```

Its own versioned migrations (like `signals.db`); one writer (mda-broker); the gateway and
mda-feedd read.

## 12. API and UI

- **REST** (`docs/api/rest.md` "Paper trading"): `POST /paper/orders`, `PUT` and `DELETE
  /paper/orders/{id}`, `GET /paper/orders`, `/paper/trades`, `/paper/positions`,
  `/paper/funds`, `POST /paper/kill-switch`, `POST /paper/reset`. POST returns once the
  broker acks (or after a timeout with the client order id, so a retry is safe).
- **Stream:** mode `orders`: the user's execution reports and position changes.
- **UI** (Kite as the minimum standard):
  - B / S buttons on watchlist rows and the instrument page open an order ticket (product,
    type, quantity in lots, price, trigger), with the margin and estimated charges shown
    before placing;
  - Orders (open, executed, rejected with reasons), Positions (live P&L, exit, exit all),
    Funds pages;
  - a **PAPER** label on the ticket and every trading page, so nobody confuses it with
    Kite.
- **Built (PT5):** `web/src/paper/OrderTicket.tsx` (ticket and its provider, with toasts
  for fills, triggers and broker cancellations from the stream), `web/src/paper/estimate.ts`
  (the ticket's margin and charges estimates, the same rules as `libs/oms`),
  `web/src/pages/Paper.tsx` (`/paper`: Positions with live P&L, Exit and Exit all; Orders
  with Modify / Cancel; Trades with the charges breakdown; Funds with the kill switch). B / S
  appear on futures and options only (v1 scope). A submission keeps its client order id
  through network retries and gets a new one after a definite answer.

## 13. Configuration

```yaml
bus:
  ord_gw:   { name: ord-gw,   capacity_mb: 4 }
  ord_auto: { name: ord-auto, capacity_mb: 4 }
  exec:     { name: exec,     capacity_mb: 8 }
paper:
  db: data/paper.db
  opening_cash: 1000000
  mis_square_off: "15:25"
  latency_ms: 100
  overflow_ticks: 2
  stale_after_seconds: 5
  margin_pct: { NIFTY: 0.12, BANKNIFTY: 0.12, SENSEX: 0.12, BANKEX: 0.12 }
  freeze_qty: { NIFTY: 1800, BANKNIFTY: 900, SENSEX: 1000, BANKEX: 900 }   # verify per expiry
  limits: { ui: {…}, auto: {…} }      # §9
  charges: {…}                         # §7
```

## 14. Units

| Unit | Scope | Done when |
|---|---|---|
| PT1 | `libs/oms`: order state machine, positions and P&L, ledger, charges | Tests for every transition, average price and realised P&L cases, charges against hand-checked broker examples |
| PT2 | `libs/oms` paper venue (fill model) and `libs/rms` | Tests with synthetic books for each order type, staleness, latency, trade-through; every risk check and the kill switch |
| PT3 | Messages 10–11, buses, `mda-broker` with `paper.db` (event log, recovery), mda-feedd subscription source | A scripted day replayed from the journal gives identical `paper.db`; a restart mid-day resumes |
| PT4 | Gateway REST and stream mode `orders`, API docs | Gateway and stream tests; idempotent retries |
| PT5 | Web: order ticket, Orders, Positions, Funds, kill switch, PAPER labels | Checked on the test stack with a replayed session |
| PT6 | Close of day: MIS square-off, expiry settlement, daily P&L summary by Telegram (notifier) | Tested on a replayed expiry day |
| PT7 | ADR-0030: mda-autotrader (signals → `ord-auto`, paper only) | Its own design |
| MX4 | ADR-0031: MCX futures and options (hours, margins, charges, depth in units, close of day) | Risk, charges and broker tests; an evening order checked live |

## 15. Open questions

- Opening cash: ₹10 lakh each, or different for the owner and the friend?
- Do equities (cash market) matter soon, or F&O only for now?
- Resting LIMIT fills: trade-through (conservative, v1) or touch with a queue estimate later?
