# OI buildup chart

> Status: **Draft for review** · Related: [architecture overview](../architecture/overview.md),
> [recovery and gap filling](recovery-and-gap-filling.md)

## 1. What the user sees

Two charts on a shared time axis:

1. **Price chart** — standard candles, two colours (up / down).
2. **OI chart** — candles of **open interest** (OI open, high, low, close per interval),
   each coloured by what price and OI did together.

| State | Price change | OI change | Display |
|---|---|---|---|
| Long buildup | ↑ | ↑ | Strong green |
| Short covering | ↑ | ↓ | Light green |
| Short buildup | ↓ | ↑ | Strong red |
| Long unwinding | ↓ | ↓ | Amber |
| Neutral | any | below threshold | Solid gray |
| No OI update | — | OI not published in this bar | Hollow (outline) candle |

Colours are grouped by direction: greens lean bullish and red/amber lean bearish, while
the shade separates new positions (strong) from closing ones (light). An option's candle
describes that contract's own price and OI, never the underlying: a call's short buildup
stays red even though it reads bearish for the underlying. That reading belongs in a
label (later), not in the colour.

The UI always shows **"OI as of HH:MM:SS"** so staleness is visible.

## 2. Classification rule

### 2.1 Change is measured close-to-close
Price and OI changes are measured from the **previous reference close** to this bar's
close (not this bar's open → close).

### 2.2 OI is published periodically, not per trade
NSE disseminates OI at intervals, so Kite's OI moves in steps. A bar in which OI did not
change because nothing was published must not be read as "positions unchanged".

Therefore the reference point is **the last bar that contained an OI update**:

```
on bar close:
    if bar.oi_update_count == 0:
        state = NoOiUpdate
    else:
        ref       = last earlier bar with oi_update_count > 0   (previous OI update span)
        d_oi      = bar.oi_close    - ref.oi_close
        d_price   = bar.price_close - ref.price_close
        if |d_oi| < threshold(instrument):  state = Neutral
        else: state = quadrant(sign(d_price), sign(d_oi))   // d_price == 0 counts as up
```

When every bar has an OI update, `ref` is simply the previous bar — the plain
close-to-close rule.

**Example (1-minute bars):** OI updates arrive in the 10:03 and 10:06 bars. Bars 10:04
and 10:05 are `NoOiUpdate`. The 10:06 bar is classified from price and OI change
between the 10:03 and 10:06 closes.

### 2.3 No repainting
A bar's state is decided at its close using only information available then, and never
changes afterwards (except through the explicit bar-repair path in
[recovery](recovery-and-gap-filling.md), which emits a correction event). This keeps
alerts, signals and backtests honest.

### 2.4 Neutral threshold
Configurable per instrument class, defaulting to the larger of:
- a fraction of OI (default **0.1%**), and
- a number of lots (default **1 lot**).

## 3. OI for each instrument type

| Instrument | OI used |
|---|---|
| Option contract | Its own OI |
| Future contract | Its own OI |
| Underlying (stock or index) | **Combined OI of all live futures expiries** (default); near-month only as an option |

**Phase 1 covers futures and options only.** The combined series for an underlying comes
later.

For the combined series, an "OI update" is any update to any of the constituent futures.

## 4. Timeframes

- All timeframes (1m, 3m, 5m, 15m, 1h, …) are aggregated from **1-minute bars**.
- Aggregated OI candle: OI open = first bar's OI open; high/low = max/min; close = last
  bar's OI close; `oi_update_count` = sum.
- Classification is recomputed on the aggregated series with the same rule.
- **Default OI chart timeframe** ≥ typical OI update interval, set from measurement
  (see §7). 1m remains available; hollow candles make gaps visible.

## 5. Bar fields (affects `messages/` and `market/`)

| Field | Type | Notes |
|---|---|---|
| `price_open/high/low/close` | fixed-point price | |
| `volume` | u64 | from the change in cumulative day volume |
| `oi_open/high/low/close` | u64 | carried forward when no update |
| `oi_update_count` | u32 | OI changes observed in the bar |
| `oi_update_offset` | u16 | seconds after the bar's start of the last OI change (receive time); 65535 = none or unknown. In two reserved bytes, so the bar stays 112 bytes (message version 2). |
| `buildup` | enum | the six states above |
| `quality` | enum | `live` / `partial` / `backfilled` / `missing` ([recovery](recovery-and-gap-filling.md)) |

OI update time is the **receive time** of the tick in which the OI value changed (Kite
gives no OI timestamp; precision ≈ the tick interval, ~1 s).

**Backfilled bars:** Kite historical candles carry OI but not update counts. Treat a
backfilled bar as having an OI update when its OI differs from the previous bar's;
otherwise `NoOiUpdate`.

## 6. Where it is computed

In the `bars` library (`buildup.hpp`), as a pure function over a bar series. Higher
timeframes are aggregated at query time (ADR-0021) and §4 reclassifies them, so the
gateway runs it after aggregating, for REST history and the live `bars` stream alike.
Bars are stored closed and each one only looks back, so every request gives the same
answer (§2.3). Alerts, signals and Python research use the same function; the browser
only renders it. The live stream carries the forming OI candle without a state; the chart
re-reads the latest REST bars as minutes close, so states come only from the server. Nothing about the state is stored in `bars.db` or put on the bus.

Backfilled minutes get their OI update count (§5) before aggregation, so an aggregated
candle that mixes live and backfilled minutes is classified correctly.

## 7. Measuring OI update frequency

Before fixing defaults, measure real OI update intervals per segment (index options,
index futures, stock options, stock futures) with the throwaway probe in
[`tools/research/oi_probe.py`](../../tools/research/oi_probe.py)
([runbook](../runbooks/oi-probe.md)). Questions to answer:

1. Typical (median, p90) interval between OI changes per segment.
2. Are updates synchronized across instruments (published in batches)?
3. Does the interval vary across the session?

Results will be recorded here and drive the default OI timeframe and the neutral threshold.
A first hint: Sensibull's OI page says the exchange updates OI every 3 minutes for many
strikes and every minute for some.

## 8. Open questions

- Colours for colour-blind accessibility (consider adding a candle-fill pattern).
- Should the option chain show the latest buildup state per strike (per the selected timeframe)?
