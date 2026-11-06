# Market signals: direction bias for the indices

Design for ADR-0028: `mda-signald`, the `signals` library, the `sig` bus and how signals
reach the web app. Phase 1 is one transparent rule-based strategy with a measured track
record; later strategies plug into the same frame.

## 1. Goal and non-goals

- Every minute, for NIFTY, SENSEX, BANKNIFTY and BANKEX: a **score from −100 to +100**, a
  label (Bullish, Neutral, Bearish), the horizon it is meant for and the **factors** that
  produced it.
- Every signal is scored against what the index then did, so each strategy has a
  **track record** on screen next to its signal.
- Not a trading system: no orders and no position sizing. A probability is shown only
  once a strategy is calibrated (§6).

## 2. Data flow

```
md ─► mda-engine ─► an ─┬─► mda-gateway ─► web
                        └─► mda-signald ─► sig ─► mda-gateway ─► web
                                 │
                                 └─► data/signals.db ◄── mda-gateway (read-only)
```

mda-signald reads from `an`:

- **`Bar` (6):** closed 1-minute bars for spots, futures and options (OI in `oi_close`);
- **`OptionAnalyticsMessage` (7):** IV per option;
- **`ChainSummaryMessage` (8):** PCR, max pain, ATM strike and ATM IV per expiry.

The instrument catalog (read-only) maps an option to its underlying, expiry, strike and
right.

**Clock:** strategies run once a minute, when the spot's bar for that minute closes (a
`Bar` without `kBarForming`). A signal at minute *t* uses only bars closed by *t*, so it
can't see the future.

**Warm-up:** on start, signald reads today's bars from `bars.db` (read-only) so that
windows such as "OI change over 30 minutes" are full straight away. Chain summaries aren't
stored, so after a restart F3 and F5 stay absent until their 30-minute window has passed.
A restart costs nothing beyond the lost minutes.

**Evaluation timing:** the engine publishes a minute's closed bars in one batch. signald
marks the minute when the spot's closed bar arrives and evaluates it once the bus is
drained (at most ~2 s later), so the options' bars of the same minute are in. Summaries
are stamped with the newest bar minute seen on the bus, not the wall clock, so a replay
(ADR-0026, `config/replay.yaml`) gives the same signals as the live session did.

## 3. The `signals` library

The library holds pure code with no I/O, which keeps it testable with synthetic bars.

```cpp
// One underlying's rolling state, fed from the bus (or bars.db during warm-up).
class MarketState {
    void on_bar(const bars::Bar&);                       // spot, future, options
    void on_option(const options::OptionAnalyticsMessage&);
    void on_summary(const options::ChainSummaryMessage&);
    // queries used by factors: spot/future bars, VWAP, ATM ± N option OI at t and t-k,
    // nearest-expiry summary at t and t-k, sector indices' day change …
};

struct FactorValue { FactorId id; double value; bool present; };  // value in [-1, 1]

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual StrategyId id() const = 0;
    virtual int horizon_minutes() const = 0;
    virtual Signal evaluate(const MarketState&, std::int64_t minute) const = 0;
};
```

- A `Signal` holds the score, the label and up to eight factor contributions.
- Strategies are compiled in. `config/mda.yaml` enables them per underlying and sets their
  weights and thresholds:
  ```yaml
  signals:
    db: data/signals.db
    underlyings: [NIFTY, SENSEX, BANKNIFTY, BANKEX]   # each needs a feed.chains entry
    sectors: [NSE|CASH|IDX|NIFTY IT, …]               # F7 breadth
    strategies:
      - { id: oi-bias-v1, horizon_min: 30, bullish: 25, bearish: -25 }
  ```
- Factors are shared functions, so a second strategy can reuse them with different weights.

## 4. Strategy 1: `oi-bias-v1` (30-minute horizon)

Each factor gives a value in [−1, 1], squashed with `tanh` so that one extreme reading
can't dominate. The score is Σ weight × value, and the weights add up to 100.

| # | Factor | Value (bullish is positive) | Weight |
|---|---|---|---|
| F1 | Futures buildup, near month, last 15 min | Long buildup +1, short covering +0.5, long unwinding −0.5, short buildup −1 (`classify` from `options`) | 20 |
| F2 | ATM OI shift, nearest expiry, ATM ± 5 strikes, last 30 min | tanh((ΔputOI − ΔcallOI) / total OI / 0.02) | 20 |
| F3 | PCR change, nearest expiry, last 30 min | tanh(ΔPCR / 0.10) | 15 |
| F4 | Future vs its day VWAP | tanh((close − VWAP) / VWAP / 0.002) | 15 |
| F5 | ATM IV change, last 30 min | −tanh(ΔIV / 0.01) (IV falling is bullish) | 10 |
| F6 | Max-pain pull, expiry day and the day before only | tanh((max pain − spot) / spot / 0.005) | 10 |
| F7 | Sector breadth (the dashboard's sector indices) | 2 × (share up on the day) − 1 | 10 |

- **Label:** Bullish when the score ≥ +25, Bearish when ≤ −25, Neutral otherwise.
- **Missing data:** a factor without data (no future subscribed, chain not yet populated)
  is marked absent and contributes 0. The UI lists absent factors so a weak score isn't
  mistaken for a neutral market.
- **Constants:** the scales (0.02, 0.10 …) are starting guesses, set from a week of real
  data in SG1 and kept in config.

## 5. `MarketSignal` (type 9, `sig` bus, 48 bytes)

```cpp
struct FactorContribution { std::uint16_t factor; std::int16_t points; };  // points = weight × value

struct MarketSignal {
    messages::EventHeader header;   // instrument_id = the spot
    std::int64_t minute = 0;        // minute start, Unix seconds (UTC)
    std::uint16_t strategy = 0;     // StrategyId
    std::uint16_t horizon_min = 0;
    std::int16_t score = 0;         // -100 … +100
    std::uint16_t prob_bp = 0;      // calibrated P(up) in basis points; 0 = not calibrated
    FactorContribution top[4] = {}; // largest |points| first; unused entries factor = 0
};
static_assert(sizeof(MarketSignal) == 48);
```

The bus carries the top four factors. `signals.db` keeps all of them.

## 6. Track record

The outcome of a signal at minute *t* with horizon *h* is the spot's return from *t* to
*t + h*, filled in by signald once *t + h* has closed (or at the close, if that comes
first).

- **Settling:** at the first spot close at or after *t + h*; a missing bar moves it to
  the next one. Once the session's last minute (15:29) has closed, open signals settle at
  the day's last close. Signals left open while signald was down settle from `bars.db` when
  it starts.
- **Flat band:** |return| ≤ 5 bp counts as flat (`signals.flat_bp`).
- **Hit:** Bullish and up, or Bearish and down, beyond the band. Neutral signals aren't
  counted in the hit rate; they're reported separately.
- **Baselines**, shown next to the hit rate and measured on the same minutes as the
  strategy's hits and misses:
  - **momentum:** the direction of the previous *h* minutes;
  - **always up**.

  A strategy that doesn't beat both has no edge.
- **Calibration (SG6):** once there are enough outcomes (≥ 20 trading days), fit
  P(up | score) with a logistic curve per strategy and publish it in `prob_bp`. Until then
  the UI shows the score, not a percentage.

`data/signals.db` has its own versioned schema:

```sql
CREATE TABLE signal (
  strategy TEXT, underlying TEXT, minute INTEGER,   -- Unix seconds
  horizon_min INTEGER, score INTEGER, label INTEGER, prob_bp INTEGER,
  factors TEXT,                                      -- JSON: all factors, incl. absent
  spot REAL, spot_after REAL, return_bp INTEGER, hit INTEGER,  -- outcome, NULL until known
  PRIMARY KEY (strategy, underlying, minute)
);
```

## 7. Gateway and API

- **REST:**
  - `GET /api/v1/signals?underlying=NIFTY&date=YYYY-MM-DD` returns one row per minute and
    strategy, with factors.
  - `GET /api/v1/signals/record?underlying=NIFTY&days=20` returns, per strategy: hit rate,
    the counts, the baselines and the calibration status.
- **WebSocket:** a new mode, `signals`, for live `MarketSignal`s per spot id. The gateway
  maps factor and strategy ids to names.
- `docs/api/rest.md` and `websocket.md` are updated in the same unit.

## 8. Web

- **Dashboard:** the NIFTY, SENSEX, BANKNIFTY and BANKEX cards get a badge such as
  `▲ Bullish +42 · 30m`. Hovering shows the top factors. `GET /signals` gives each index's
  latest signal, so the badge has a value before the stream sends one; a signal more than
  3 minutes old shows its time (market closed, or signald down).
- **Instrument page (an index):** a **Signals** tab next to Chart (`?view=signals`), not a
  pane inside the price chart: the chart's intervals (1m–1d) and OI panes don't fit
  per-minute signals. It shows:
  - the current signal and the strategy's thresholds;
  - the day's score (left scale, −100…+100, with the Bullish/Bearish lines) against the
    spot (right scale), with a day picker;
  - each factor's points as a bar left or right of zero, with absent and not-applicable
    factors marked;
  - the track record over 5, 20 or 60 days next to both baselines;
  - the last 15 Bullish/Bearish calls and what happened after them.
- **Colours:** `--up`, `--down`, `--muted` and `--accent`, the same as quotes.

## 9. Operations

- mda-signald gets an entry in `tools/stack.sh` and a row in Admin → System, alongside
  the other processes.
- It is safe to restart during market hours: it only loses the minutes it was down.
- `sig` bus: a 4 MB ring, since signals are tiny and arrive once a minute.

## 10. Units

| Unit | Scope | Done when |
|---|---|---|
| SG1 | `libs/signals`: `MarketState`, factors F1–F7, `oi-bias-v1`; gtest with synthetic bars | Each factor and the score tested, including absent factors and the no-look-ahead rule |
| SG2 | `MarketSignal` message, `sig` bus, `mda-signald` (bus reader, minute clock, warm-up, `signals.db`), stack.sh, sysmon row | Signals published live and stored; restart mid-session resumes |
| SG3 | Outcome scoring and the record query | Outcomes filled at *t + h*; record numbers checked by hand on one day |
| SG4 | Gateway REST and WS `signals` mode, API docs | Tests in `gateway_test.cpp` and `stream_test.cpp` |
| SG5 | Web: dashboard badges, instrument-page Signals tab, record panel | Checked live on the test stack |
| SG6 | Calibration (`prob_bp`) and a second strategy (Python research in `tools/research/`, coefficients exported to C++) | Only after ≥ 20 trading days of outcomes |

## 11. Open questions

- **Horizons:** 30 minutes to start. Does a second record "at the close" for the same
  signal help, or is it noise?
- **Thresholds:** are ±25 right? Revisit after the first week of data.
- **Futures:** F1 and F4 need the near-month future. `futures_of` has NIFTY and
  BANKNIFTY only, so SENSEX and BANKEX would run without them (35 of 100 points absent).
  Proposal: add SENSEX and BANKEX to `futures_of` in SG2.

## 12. Backtest check

`mda-backtest` (engine/apps/tools) runs the configured strategies over the last N trading
days from `bars.db`, with the same `MarketState` and factors as mda-signald, chain
summaries rebuilt per minute by the engine's `ChainAnalytics` from option minute closes.
It writes to `data/research/signals_backtest.db` (never `signals.db`) and prints, per index
and factor, how often it was present, its correlation with the forward return, and how
often a clear reading (|value| ≥ 0.2) pointed the right way, plus the strategy's record
next to both baselines.

It is a check that factors behave, not a track record: only contracts still in `bars.db`
take part, summaries come from minute closes rather than ticks (on 2026-10-23 the scores
correlated 0.97 with the tick replay; labels agreed 85–88%), and 10 days of overlapping
30-minute windows are a few hundred independent samples at most.

**First run (2026-10-09 to 2026-10-23, 10 days):** no factor broken (present 88–100%;
max pain only near expiry, by design). Correlations with the next 30 minutes' move were
all within ±0.10. oi-bias-v1 hit 60% (SENSEX) and 56% (NIFTY), tying momentum (60%, 58%),
and 44–46% on BANKNIFTY and BANKEX, below both baselines: in effect a trend follower, with
no edge yet. Weights stay as they are; tuning on 10 days would overfit. Revisit with ≥ 20
live days and the tick journal (recorder on since 2026-10-24, 30 days kept).

## 13. Candidate factors F8 and F9 (tracked at zero weight)

Two new factors, scored every minute and kept in `signals.db` with the others, but weighted
0 in `oi-bias-v1`, so the label doesn't change. SG6 calibration decides whether they earn
points. Both came from a review of a common "intraday metrics" list (VWAP deviation, ΔOI,
cumulative volume delta, India VIX): the first two are F4 and F1/F2 already. Its claims
("institutional accumulation", "high probability") are untested and play no part here.

| # | Factor | Value (bullish is positive) | Weight |
|---|---|---|---|
| F8 | India VIX change, last 30 min | −tanh((VIX_t − VIX_t−30) / VIX_t−30 / `vix_scale`) (VIX rising is bearish) | 0 |
| F9 | Signed volume of the near-month future, last 15 min | tanh(((buy − sell) / (buy + sell)) / `delta_scale`) | 0 |

### F8: India VIX

- **Instrument:** `NSE|CASH|IDX|INDIA VIX` (catalog id 18305), added to `feed.instruments`;
  auto-backfill (ADR-0025) then loads 30 days of its minute bars, so `mda-backtest` can score
  F8 over past days straight away.
- **Data path:** a spot-like bar on `an`, like the sector indices; signald learns its id
  from a new `signals.vix` key (`NSE|CASH|IDX|INDIA VIX`). One VIX serves every underlying,
  as breadth does.
- **Window:** 30 minutes, not "since 09:15": the change from the open mixes in the
  overnight gap and the first minutes' noise, and a 30-minute window matches the
  strategy's horizon and F5.
- **Scale:** not a fixed 2 % (a 2–5 % daily VIX move is ordinary). `vix_scale` is twice
  the median |30-minute change|, so a typical move scores about ±0.46 and tanh reaches ±0.76
  at twice that. Set by hand from `bars.db` and compiled in with the other factor
  constants. Measured on 2026-10-26 over 19 backfilled days (6,555 windows, VIX 12.9–18.0):
  median 0.65 %, p75 1.19 %, p90 1.95 %, p99 4.14 %, so `vix_scale` = 0.013. The
  article's "> 2 %" is roughly a one-in-ten 30-minute move.
- **Overlap with F5** (ATM IV of the nearest expiry): VIX is the 30-day implied volatility
  from the near and next month, so it doesn't jump around weekly expiries as F5 does.
  Calibration will show whether either adds anything once the other is known.
- **Absent** before 09:45 (window not full) or when the VIX bar is missing.

### F9: signed volume (cumulative volume delta)

Who is aggressing, buyers or sellers, which no current factor measures.

**Classification:** Kite sends a full-mode snapshot about once a second, not every trade,
so each snapshot's new volume (Δ of the cumulative `volume`) is classified as one block:

1. **Quote rule**, against the best bid and ask *before* this snapshot (the previous
   `DepthSnapshot` of the instrument): LTP ≥ previous ask → buy; LTP ≤ previous bid → sell.
2. **Tick rule** when the LTP is inside the spread: above the last different LTP → buy,
   below → sell.
3. Otherwise (no previous quote yet, LTP unchanged since the start) → unclassified, kept
   as its own count.

This is Lee–Ready adapted to snapshots. The internet version used the tick rule alone and
threw away volume whenever LTP didn't move (most snapshots in a quiet market). The limit
that remains: a one-second block may hold trades on both sides, so the delta is a lower
bound on the true imbalance. `total_buy_qty` / `total_sell_qty` are resting orders, not
trades, and aren't used.

**Where it's computed:** in mda-signald, which now also reads `md` (snapshots and depth of
each underlying's near-month future only; everything else is skipped on the instrument id).

| Option | For | Against |
|---|---|---|
| **A. signald reads `md`** (chosen) | No message or schema change; contained in signald and `libs/signals`; reversible | No history: after a restart F9 is absent until its 15-minute window refills; `mda-backtest` (bars only) can't score it |
| B. mda-engine adds `buy_volume` / `sell_volume` to `Bar` | History in `bars.db`, a CVD line on charts later | `Bar` grows from 112 bytes; every bus reader, `bars.db` and the gateway change; backfilled bars have none |

If F9 proves useful in calibration, it moves to B in its own unit.

**State:** `MarketState::on_snapshot` and `on_depth` for the future keep the previous
LTP, bid, ask and volume, and add each classified block to the current minute's buy, sell
and unclassified totals; F9 reads the last 15 closed minutes. The minute comes from the
snapshot's exchange time, so a replay classifies as the live run did.

**Absent** while fewer than 10 of the last 15 minutes have data, or when unclassified
volume is over half the total (depth missing: `feed.mode` not full).

**Divergence** (price makes a lower low while CVD makes a higher low) is not a factor yet:
it needs exact swing rules to be testable. It's a candidate once F9 alone has been measured.

### Checks

- **F8:** `mda-backtest --days 20` reports its presence and its correlation with the next
  30 minutes, next to F5.
- **F9:** replay a recorded day (`config/replay.yaml`, journal since 2026-10-24) and check:
  share of volume classified (expect > 80 %); per-minute delta against price direction
  (should agree more often than not); scores identical between two replays.
- Both then run live at weight 0, and SG6 (≥ 20 trading days) decides their weights from
  the outcomes.

### Changes outside `libs/signals`

- `config/mda.yaml`: INDIA VIX in `feed.instruments`; `signals.vix`. The weights stay
  compiled in (`OiBiasConfig::weights`, now nine entries ending 0, 0) and the scales join
  the other factor constants, so the existing seven keep exactly their scores.
- mda-signald: an `md` reader for the near-month futures.
- Factor names in the gateway's signal JSON and the web's factor list ("VIX change",
  "Volume delta"); `docs/api` lists the new factor ids 8 and 9.
- `kFactorCount` 7 → 9; `signals.db` stores all factors as JSON, so no schema change. The
  bus keeps the top four by |points|, so zero-weight factors never appear there.

### SG7 result (2026-10-26)

F8 is live at weight 0: India VIX is subscribed and backfilled (19 days), signald and
`mda-backtest` read it, and it's listed as `vix_change` in the API and the web.
`mda-backtest --days 20` (2026-09-24 to 2026-10-23), F8 against the next 30 minutes:

| Index | Present | Corr | Direction right (clear readings) |
|---|---|---|---|
| NIFTY | 83 % | +0.12 | 54 % of 3,416 |
| SENSEX | 83 % | +0.11 | 54 % of 3,420 |
| BANKNIFTY | 83 % | +0.06 | 49 % of 3,612 |
| BANKEX | 83 % | +0.05 | 49 % of 3,578 |

Present 83 %: absent in each day's first 30 minutes, by design. On NIFTY and SENSEX its
correlation is the highest of the eight factors (F4 +0.08, F7 +0.05 to +0.07), and above
ATM IV (F5, +0.08 and +0.02), the factor it overlaps. On the bank indices it adds nothing.
Twenty days of overlapping windows are a few hundred independent samples, so a correlation
of 0.1 is a hint, not an edge: the weight stays 0 until SG6.

### SG8 result (2026-10-26)

F9 is live at weight 0: mda-signald reads `md` for the four near-month futures and logs,
every half hour, how much of their volume it classified. Two changes the replay forced on
the design above:

- **Pre-open:** NSE futures have a pre-open auction (09:00–09:15). Its match has no
  aggressor, so snapshots before 09:15 only move the baseline.
- **"Minutes with data"** means minutes with traded volume, not minutes with a snapshot:
  otherwise quiet minutes, the pre-open and BANKEX's barely traded future (60 contracts on
  23 Oct) counted, and F9 read ±1 from a handful of lots.

**Replay of 2026-10-23** (journal, 13.4 M snapshots at 120×), against the checks above:

| Index | Present | Classified | Agrees with the future's 15-min move (|v| ≥ 0.2) | Corr, next 30 min |
|---|---|---|---|---|
| NIFTY | 366 of 375 min (from 09:24) | 100 % | 74 % of 293 | +0.16 |
| SENSEX | 175 (thin future) | 100 % | 80 % of 163 | +0.15 |
| BANKNIFTY | 366 | 100 % | 73 % of 318 | +0.05 |
| BANKEX | 0 (absent, by design) | — | — | — |

- **Classified 100 %:** the tick rule carries the last change over an unchanged LTP, so
  only the first snapshots of a day can stay unknown. The unclassified guard stays for a
  feed without depth.
- **Deterministic:** two replays gave identical F9 values on all 1,500 index-minutes.
  (The oi-bias score itself differed on 551 of them between the two runs: chain summaries
  are stamped by bus arrival, an existing replay effect noted in §12, not from F9, which
  weighs 0.)
- **Scale:** |imbalance| medians 0.14 (NIFTY), 0.17 (BANKNIFTY), 0.31 (SENSEX, thin);
  `flow_scale` = 0.3, twice the liquid median, as for F8.
- One day is one day: the correlations are a reason to keep scoring F9, not to weight it.
  SG6 decides, with ≥ 20 live days.

Seen while replaying: mda-signald exited with "database is locked" when it opened a fresh
`bars.db` while mda-engine was creating it, the known race in fresh SQLite files (CLAUDE.md
hardening). Live files already exist; for replays, start the engine a few seconds first.

### Units

| Unit | Scope | Done when |
|---|---|---|
| SG7 (done) | F8: config, VIX subscription and backfill, factor, gateway and web names | Unit tests (window, absent, scale); `mda-backtest` over 20 days reports F8 |
| SG8 (done) | F9: `md` reader in signald, snapshot classifier and `MarketState` totals, factor | Unit tests for each classification rule and the absent cases; replay of a recorded day meets the checks above |

The stock direction labels discussed on 2026-10-26 (segments, `stock-trend-v1`) come
after these and will take the next unit numbers.

## 14. Replays score the same (2026-10-27)

Two replays of 2026-10-23 gave a different score on 551 of 1,500 index-minutes (§13, SG8
result). F9 was identical; the differences were in ATM IV change (1,112 minutes), PCR change
(492), ATM OI shift (136), future vs VWAP (45) and futures buildup (5). Live scores ran once,
so they weren't wrong, but SG6 calibrates on replays, and a replay must reproduce what the
live auto trader saw. There were two causes:

- **Chain summaries came from wall-clock timing.** mda-engine computes them on its 1 s loop.
  At 120× one loop spans two session minutes, so the summary signald had as each minute
  ended depended on where the loop landed. Signald also stamped each summary with the
  latest bar minute it had seen, forming bars included.
- **A minute was scored before all its bars were in.** Signald scored as soon as the spot's
  bar closed. The future's and options' bars for that minute close on their own next tick,
  or `close_delay` (2 s) later, so some were sometimes missing.

**The fix.**
- **Closing summaries.** mda-engine computes a *closing* chain summary when the ticks cross
  into the next minute. It uses the bar builder's minute rule and computes before applying
  that tick, so the state is the minute's ticks whatever the speed. If no tick crosses the
  boundary, the 1 s loop closes the minute. The engine holds the closing summary until
  `close_delay` has passed, so every bar of the minute is on `an` before it.
- **The message.** `ChainSummaryMessage::minute`, formerly `reserved`, is the closed minute;
  it is 0 on the per-second summaries. The layout doesn't change, so the version stays 1.
- **Signald** keeps only closing summaries, stamped with their own minute. It scores a minute
  once that minute's closing summaries have arrived. As a fallback, it scores at most two
  minutes late if they never come.
- **The gateway** ignores closing summaries: they are older than the per-second ones it shows.
- **The cost:** signals are published about 2 s after the minute ends instead of on the
  spot's closing tick.

**SQLite, the same night.** Two processes creating a fresh file together failed with
"database is locked" instead of waiting, as signald and the engine did on a replay's new
`bars.db`. There were three gaps in `storage::Database`:
- The busy timeout was set after `journal_mode = WAL`; it is now set first.
- Two connections switching the same fresh file to WAL is a lock-upgrade deadlock, and SQLite
  returns BUSY to one of them without waiting. The switch is now retried for up to the
  timeout.
- `migrate` read the schema version before taking the write lock. It now re-reads it inside
  the lock, so the second process skips migrations the first one applied.

Unit tests hold a lock while opening and race two threads creating and migrating a fresh file;
both tests failed before the fix.

**Feed outage, the same day.** After 120 s with no snapshot from any instrument
(`kFeedGap`, e.g. the Wi-Fi dropped), F9's next future snapshot is a baseline. Otherwise the
volume traded during the outage counts as one minute's buy or sell. The engine's bars do the
same (`option-chain-and-bars.md` §3, Restart safety).

## 15. Order flow and queue imbalance probe (2026-10-28)

Should order flow imbalance (OFI, Cont, Kukanov and Stoikov 2014) or queue imbalance from
the 5-level depth become factors F10 and F11? `tools/research/ofi_probe.py` replayed the
journal (23, 26 afternoon and 27 Oct) for the NIFTY and SENSEX near-month futures on a
1-second grid (Kite sends about 1.4 depth updates a second), 09:20–15:25, and correlated
each feature with the future's mid return 10 s, 1 min and 5 min later.

- **Only one effect is consistent:** NIFTY OFI over the last 10 s against the next 10 s,
  r = +0.057, +0.024, +0.048 on the three days (about 2 standard errors a day). It is gone
  by 1 minute. That matches the literature: OFI is a seconds-scale signal.
- **Everything at 1 and 5 minutes is noise.** Overlapping samples leave about 280
  independent 1-minute windows a day (standard error ≈ 0.06) and 60 for 5 minutes
  (≈ 0.13). The values range from −0.2 to +0.09 and change sign from day to day.
- **SENSEX queue imbalance is negative** on two days (r down to −0.2 at 5 min) and positive
  on the third. SENSEX's book is thin (best-level spread often 20–40 points), so its mid
  bounces between stale quotes. Not usable.
- **The micro-price offset is queue imbalance** (r = +1.00): normalised by the half-spread,
  micro − mid = half-spread × qi1, an identity.
- **Decision:** no F10 or F11. The one real effect lives at a horizon a 30-minute strategy
  on Kite's feed can't use. Rerun the probe after 20 recorded days (about 20 Nov) for the
  record.

**Overlap between the live factors** (signals.db, about 2,300 minutes, all underlyings):
F2 ATM OI shift and F3 PCR change correlate at r = +0.76. Both measure put OI rising
against call OI, so 35 of the 100 weight points are largely one signal. F4 future vs VWAP
and F6 max-pain pull correlate at −0.55 (max pain pulls against the day's trend on expiry
days). Every other pair is below 0.5. SG6 calibration should fit weights jointly (or merge
F2 and F3) rather than score each factor alone.
