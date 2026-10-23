# Option chain, dynamic subscriptions and 1-minute bars

Status: **Accepted.** O1, O2a (live since 2026-10-22 14:37 IST), O2b (backfill), O3
(analytics), O4 (gateway) and O5 (option chain page) are done. Decisions are in [ADR-0020](../adr/0020-dynamic-option-subscriptions.md),
[ADR-0021](../adr/0021-bar-storage.md) and [ADR-0022](../adr/0022-option-analytics-in-engine.md).
Builds on the [OI buildup chart](oi-buildup-chart.md) design and the process architecture
([ADR-0006](../adr/0006-multi-process-from-the-start.md), [overview](../architecture/overview.md)).

## 0. What we checked first (2026-10-22)

The Kite plan includes the **historical API with OI**:

| Request | Result |
|---|---|
| NIFTY 50, 1-minute, yesterday | 375 candles (full session) |
| NIFTY 50, daily, 1 year | 246 candles |
| NIFTY Nov future, 1-minute + OI | 376 candles, OI per candle |
| NIFTY 22700 CE, 5-minute + OI | 75 candles, OI per candle |
| NIFTY 50, 1-minute, 60 days back | 375 candles |

So charts can **backfill history on day one**. Our own recording adds what the API lacks:
- our exact tick-derived bars;
- OI update counts (needed for hollow "no OI update" candles);
- expired contracts (Kite has no history for them).

Kite limits: about 3 historical requests per second; 1-minute data comes in at most 60 days per request.

## 1. The pieces

```
              Kite WS ───────────────┐
                                      ▼
  ┌──────────── mda-feedd ─────────────────────────────┐
  │ subscription manager (O1): static + watchlists +   │
  │   option windows (ATM ± N, re-centred on spot)     │
  └────────────────────┬───────────────────────────────┘
                       │ bus "md": snapshots, depth, circuits
            ┌──────────┴──────────────────────┐
            ▼                                 ▼
  ┌──── mda-engine (O2, O3) ─────┐      ┌──── mda-gateway (O4) ─────────────┐
  │ 1m bars (OHLC, vol, OI,      │      │ quote cache + analytics fields    │
  │   OI-update count) → bars.db │      │ chain REST + stream               │
  │ option analytics: forward,   │─bus─▶│ bars history REST + live bars     │
  │   IV, Greeks, OI change,     │"an"  └──────────────┬────────────────────┘
  │   PCR, max pain, ATM IV      │                     ▼
  └──────────────────────────────┘              browser: chain (O5), charts (C1)
  mda-backfill (O2): Kite historical → bars.db
```

- A **second bus, `an` (analytics)**: the engine publishes there and the gateway reads it.
  The `md` bus keeps a single writer (mda-feedd), as ADR-0007 requires.

## 2. Subscription manager (O1, in mda-feedd)

### What it subscribes
- **Static list** (config) + **all watchlist instruments** (as today).
- **Option windows**, from config:

```yaml
feed:
  chains:
    - underlying: NIFTY          # F&O underlying name in the instrument list
      spot: NSE|CASH|IDX|NIFTY 50
      expiries: 3                # nearest 3 expiries (weekly + monthly)
      strikes: 20                # ATM ± 20 strikes
    - underlying: BANKNIFTY
      spot: NSE|CASH|IDX|NIFTY BANK
      expiries: 1
      strikes: 20
    - underlying: SENSEX
      spot: BSE|CASH|IDX|SENSEX
      expiries: 2
      strikes: 20
```

- Each underlying's **futures** (all listed expiries): they give the forward price and the
  combined futures OI.

### Strike grid and ATM
- The strikes are the listed strikes of that underlying and expiry, from today's instrument
  list. The grid is irregular near some expiries, so we never assume a fixed step.
- **ATM** = the listed strike nearest to the spot price, seen live from the index snapshots
  that mda-feedd itself decodes.
- Before the first tick (for example during pre-open), we use Kite's quote API LTP at startup.

### Re-centring without churn
- The window is ATM ± `strikes`, plus a **buffer of 5 strikes** on each side.
- It is re-centred only when ATM has moved more than 5 strikes from the window's centre.
  NIFTY must move about 250 points before anything changes.
- Strikes leaving the window are **unsubscribed only 10 minutes later**, so a price
  oscillating at the edge doesn't flap subscriptions.
- Changes go through the existing live diff (`set_tokens`): no reconnects.

### Budget
| Chain | Expiries × strikes × 2 (+ buffer) | Instruments |
|---|---|---|
| NIFTY | 3 × (41 + 10) × 2 | ~306 |
| BANKNIFTY | 1 × 51 × 2 | ~102 |
| SENSEX | 2 × 51 × 2 | ~204 |
| futures, indices, watchlists | | ~60 |
| **Total** | | **~670** of 3,000 per connection |

- F&O **stocks** (~200 underlyings × ±10 strikes × 2 ≈ 8,400) don't fit one connection.
  That's a later unit with **multiple connections** (Kite allows 3) and smaller windows.

### Logged
- Each re-centre: `chains: NIFTY 27-Oct re-centred 22650 → 22900 (+44 −0, 166 total)`.
- The budget at start and at each change. A warning at 80% of 3,000.

## 3. Bars (O2, mda-engine + mda-backfill)

### The bar (fields from the OI buildup design §5)
| Field | Meaning |
|---|---|
| instrument_id, start (minute, IST-aligned) | key |
| open, high, low, close | from LTP |
| volume | traded quantity in the minute = Δ of Kite's cumulative day volume |
| oi_open, oi_high, oi_low, oi_close | OI within the minute (0 for cash) |
| oi_update_count | how many times OI changed in the minute |
| oi_update_offset | seconds after the minute start of the last OI change (receive time); 65535 = unknown |
| trades | ticks that changed LTP or volume (activity indicator) |

### Building
- **Time** comes from the **exchange timestamp** (1 s resolution), not our receive time. A
  slow network then can't shift a trade into the wrong minute.
- A minute closes when a tick from a later minute arrives, **or** 2 s after the minute
  ends by our clock (timer). Illiquid instruments still close on time.
- **Minutes with no ticks produce no bar.** Charts show a gap, as Kite does. An option
  whose OI changed without a trade still gets a bar (OI-only: OHLC = previous close,
  volume 0).
- The **forming bar** (the current minute) is published every second on the `an` bus.
  Closed bars are published once, then written to disk.

### Storage: `data/bars.db` (SQLite, WAL) — ADR-0021
- One table `bars_1m (instrument_id, start, o, h, l, c, volume, oi_open, oi_high, oi_low,
  oi_close, oi_updates, trades, source)`, `PRIMARY KEY (instrument_id, start)`,
  `WITHOUT ROWID`. Prices are int64 ticks (same fixed point as on the bus).
- `source`: `live` (built by us) or `kite` (backfilled). A live bar always replaces a
  backfilled one.
- Size: ~670 instruments × 375 minutes ≈ 250k rows/day ≈ 20 MB/day ≈ 5 GB/year. That's
  fine for SQLite. Older days can later be exported to Parquet for heavy analytics
  (DuckDB). The export format stays a future decision.
- **Its own file**, not `app.db`: different size, write rate and backups. The engine is
  the only writer; the gateway opens it read-only.
- Higher timeframes (3m, 5m, 15m, 1h, day) are **aggregated at query time** from 1m bars.
  That's cheap for one instrument and keeps one source of truth.

### Backfill: `mda-backfill`
- A small tool (and later a scheduled job) that fetches Kite historical **1m + OI** for
  the instruments in bars.db's scope and inserts **only missing minutes** (`source = kite`).
- Default: the last 30 days for subscribed instruments, at 3 requests per second (~10 min
  for 670 instruments). On demand: `mda-backfill NSE|CASH|IDX|NIFTY 50 --days 60`.
- Backfilled bars carry no OI-update count. Per the OI design §5, "OI changed vs the
  previous bar" counts as one update.

### Restart safety
- At startup the engine reads today's closed bars back from bars.db, so the bar series
  carries on after a restart.
- Minutes lost while the engine was down are filled by `mda-backfill` (Kite has them).
  This is the gap-filling path from [recovery-and-gap-filling.md](recovery-and-gap-filling.md).

## 4. Option analytics (O3, mda-engine) — ADR-0022

Computed **per underlying and expiry, once a second** (and when its forward moves):

| Output | How |
|---|---|
| **Forward** F | From put–call parity at the 3 strikes nearest ATM: F = K + C − P (median). It uses live option prices, needs no rate or dividend assumption, and works for weekly expiries without a future. Falls back to the expiry's future, then to spot. |
| **Time to expiry** T | Calendar time to 15:30 IST on expiry day, in years (ACT/365). T has a floor of 1 hour, so expiry-day IV doesn't blow up. |
| **IV** | **Black-76** on F: Newton–Raphson from 20%, falling back to bisection; r = 0 (the forward already carries the cost of carry). No IV when the option price is below intrinsic value or it hasn't traded today. |
| **Greeks** | Black-76 closed form. Delta and gamma per 1 point of underlying, vega per 1 vol point, theta per calendar day. |
| **OI change** | OI − **previous day's closing OI**, from yesterday's last bar (our bars, or backfill). Kite's tick stream doesn't carry it. |
| **Buildup per strike** | Price change vs OI change today → long buildup / short buildup / short covering / long unwinding (same classes as the OI chart). |
| **PCR** | Σ put OI / Σ call OI over the subscribed window (labelled "window PCR"; the full chain needs every strike). |
| **Max pain** | The strike minimising total option-writer payout, over the window. |
| **ATM IV** | Average of the call and put IV at the ATM strike. |
| **IV percentile** | Later: needs a daily ATM IV history (we start recording it with O3). |

- Results go on the `an` bus as fixed-layout messages:
  - `OptionAnalytics`: instrument_id, iv, delta, gamma, theta, vega, prev_oi, buildup;
  - `ChainSummary`: underlying, expiry, forward, atm_strike, atm_iv, pcr, max_pain, T.
- The gateway merges them into its quote cache, exactly as it does circuit limits today.
- Cost: ~600 options × an IV solve (~2 µs) per second, which is negligible. It still lives
  in the engine because the same numbers will feed signals, alerts and the OI charts, not
  just browsers.

## 5. Gateway and API (O4)

- `GET /api/v1/chains` → underlyings with chains, and their expiries (from the catalog +
  the config's chains).
- `GET /api/v1/chains/{underlying}/{expiry}?strikes=20` → the structure: rows of
  `{strike, call: instrument, put: instrument}` around ATM, with the current
  `ChainSummary`.
- Live values reuse the **existing stream**:
  - the browser subscribes the row instruments as quotes (82 for ±20, within the 200
    limit);
  - quotes now also carry `iv, delta, gamma, theta, vega, oi_change, buildup` for options;
  - a new `{"a":"subscribe","mode":"chain","v":["NIFTY:2026-10-27"]}` delivers
    `{"type":"chain", …summary…}` once a second.
- `GET /api/v1/instruments/{id}/bars?interval=5m&from=&to=` → history (for charts, C1),
  plus `mode: "bars"` on the stream for the forming bar.

## 6. Option chain UI (O5)

Kite's chain is the minimum. The default view is the **OI view**:

```
 Expiry: [27 Oct · 5d] [3 Nov · 12d] [24 Nov · 33d]     View: [OI] [Greeks]   Strikes: ±20
 ───────────────────────────────────────────────────────────────────────────────────
     OI     OI chg   Chg%    LTP  │ STRIKE │  LTP    Chg%   OI chg    OI
   ███ 33.4L  +12.1L  -13.1% 186.05│ 22650 │ 144.25  +16.8% +18.2L  32.4L ███
   …                    (ATM row highlighted; ITM side tinted; OI bars scaled to max)
 ───────────────────────────────────────────────────────────────────────────────────
  Forward 22,671 · PCR 0.74 · Max pain 22,800 · ATM IV 15.2% · 5d to expiry
```

- **OI view:** OI (lakhs) with bars, OI change (bars, green adds / red cuts), change %, LTP
  on each side; **buildup tag** per side (LB / SB / SC / LU) on hover.
- **Greeks view:** IV, Δ, Γ, Θ, Vega on each side (Kite's layout).
- Rows centred on ATM and auto-scrolled to it; the ATM strike re-centres as spot moves.
- Clicking a strike's call or put opens that option in the right pane (depth, stats, chart
  later). An action adds it to a watchlist.
- **Beyond Kite (later units):** OI and OI-change bar charts by strike (the classic
  "where are writers" view), PCR and max pain over the day, IV smile.

Placement: a new **Option chain** page in the right pane (Default workspace). It becomes a
widget when the Analyst workspace (UI6) arrives.

## 7. Units and order

| Unit | What | Testable |
|---|---|---|
| **O1** | Subscription manager: chain windows, hysteresis, budget, logs; config | Unit tests (window maths, re-centring, grace period); live tomorrow 09:00 |
| **O2a** | mda-engine skeleton: reads `md`, builds 1m bars, writes bars.db, publishes forming/closed bars on `an` | Unit tests with synthetic ticks; replay of today's bus |
| **O2b** | mda-backfill: Kite historical 1m + OI → bars.db (missing minutes only) | Against Kite (read-only) |
| **O3** | Option analytics: forward, IV, Greeks, OI change, buildup, PCR, max pain → `an` | Black-76 tests against known values; put–call parity checks |
| **O4** | Gateway: reads `an`, chain REST, stream chain mode, bars history REST | End-to-end tests |
| **O5** | Option chain page | Browser, live market |
| C1 | Charts (price + volume + OI, buildup colouring) on bars | After O2/O4 |

O1 and O2a can be built this evening and tested at tomorrow's pre-open, which lets us
**start recording bars tomorrow morning**.

## 8. First live run (2026-10-22, 14:33–14:45 IST)

- Windows were centred from the quote API before the first tick: NIFTY 22650 (3 weeklies),
  BANKNIFTY 54800, SENSEX 73200. That made **641 instruments** (21% of one connection),
  connected in 0.24 s, at about 600 ticks/s.
- Circuit limits for 633 instruments fetched in 1.7 s (2 quote requests).
- mda-engine wrote 3,091 bars in the first 5 minutes (641 instruments).
- Compared with Kite's historical candles for the NIFTY Nov future: **high, low, close,
  volume and OI identical**.
- The open can differ slightly (22793.70 vs 22790 in one minute). Kite's stream is
  conflated snapshots, so the very first trade of a minute may not be seen. Our open is
  the first price we observed. Backfill (O2b) only fills *missing* minutes and does not
  "correct" live ones. Revisit if it matters for signals.

## 9. Open questions (defaults chosen)

1. Window size: ±20 strikes and 3 NIFTY expiries (default above). More strikes cost only
   budget.
2. BANKNIFTY has monthly expiries only now; SENSEX has weeklies (the config reflects this).
3. F&O stocks: deferred to a multi-connection unit (not O1).
4. Raw-frame recorder (full tick journal for exact replay): not in this plan. Bars and
   backfill cover charts. The recorder stays planned separately (ADR-0006).
