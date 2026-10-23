# Multistrike OI

> Status: **Approved 2026-10-23; MS1 done** (MS2, several days, later) · Builds on [OI by strike](oi-change-by-strike.md) §8 (the
> session's OI as step series) · Reference: Sensibull → Analyse → Open Interest →
> Multistrike OI, observed 2026-10-23

## 1. Why

OI by strike answers "where did writers add or cut between two times". Multistrike OI
answers "**how did these few contracts' OI move through the day, against price**": a call
wall being built through the morning, puts covered as price fell, two strikes trading
places. Sensibull draws the OI of a few chosen contracts as lines on a time axis, with the
spot on a second scale.

## 2. What the user sees

```
Option chain   [OI] [Greeks] [OI by strike] [OI over time]
NIFTY 22,418 · Expiry (27 Oct) (3 Nov) (9 Nov) · [1m 5m 15m] · ☑ Spot
┌ Contracts ─────────────┐ ┌──────────────────────────────────────────────────────────────┐
│ Most active today      │ │ 1.6Cr ┤                                    ╱── 22800 CE      │
│ ☑ ● 22800 CE  +1.10Cr  │ │       │                         ╱─────────╯                  │
│ ☐ ● 22700 CE  +0.82Cr  │ │ 1.2Cr ┤              ╭─────────╯           ─── 22500 PE      │
│ ☐ ● 23000 CE  +0.61Cr  │ │       │ ─────────────╯───────╮                              │
│ ☑ ● 22500 PE  +0.95Cr  │ │ 0.8Cr ┤                       ╰─────────────── NIFTY (right) │
│ ☐ ● 22400 PE  +0.40Cr  │ │       └──────┬──────┬──────┬──────┬──────┬──────┬──────┬─  │
│ ☐ ● 22600 PE  −0.22Cr  │ │            09:30  10:00  10:30  11:00  11:30  12:00  12:30   │
│ + Add strike [22650 ▾] │ │ legend: 22800 CE 1.52Cr (+1.10Cr) · 22500 PE … · NIFTY 22,418 │
└────────────────────────┘ └──────────────────────────────────────────────────────────────┘
```

| Topic | Decision (default) | Why |
|---|---|---|
| Where | A fourth view on the option chain page: **OI over time** | Same underlying picker and strikes as the other views |
| Contracts | Per **contract** (one expiry, one strike, CE or PE), not summed over expiries | A line is one position; Sensibull does the same |
| Expiry | One at a time, nearest by default | Keeps the list short; contracts from two expiries on one chart can come later |
| Default selection | A **"Most active today"** list: the 3 calls and 3 puts with the largest OI change since the open, among ATM ±20; the top call and top put ticked | Opens on something useful without setup |
| Add more | "+ Add strike" picks any strike in the window, CE or PE; at most 8 lines | More lines stop being readable |
| Chart | lightweight-charts (time axis, crosshair, zoom as on the instrument page): **OI lines on the left scale** (L/Cr), **spot on the right scale** (thin, grey) | It has a real time axis, unlike OI by strike; same feel as our price chart |
| Colours | A fixed palette of 8 distinct colours, the same colour in the list and the chart; calls solid, puts dashed | Calls and puts tell apart at a glance |
| Interval | 1m · 5m · 15m (default 5m): the step series sampled at each interval end | 1m is noisy for far strikes that update every 3 minutes |
| Legend | Under the cursor (else the latest minute): each line's OI and change since the open, and spot | As our price chart's legend |
| Live | Reloads 2 s after each minute closes, like OI by strike | Same data, same rule |
| Range | **Today's session** in v1 | Our timeline route is one session |

**Later:** several days (Sensibull's default spans two to three sessions; needs a `days`
parameter on the timeline route, MS2), the future instead of spot, saved groups of strikes,
and contracts from two expiries together.

## 3. Data

No new route for v1. `GET /chains/{u}/oi-timeline?expiries=<one>&strikes=20` (design §8 of
OI by strike) with one expiry ticked returns exactly each contract's OI as a step series
(ATM ±20) plus spot per minute. The "most active" list and every line are computed in the
browser from it; the browser samples each series at the interval ends.

## 4. Units

| Unit | Scope |
|---|---|
| **MS1** | Web: the OI-over-time view: contract list (most active, add, tick), lightweight-charts lines with two scales, interval, legend, live refresh |
| MS2 (later) | Several sessions: `days=1..5` on the timeline route (+ tests), a days control |

## 5. Open questions (defaults chosen)

1. "Most active" by **OI change since the open** (default) or by total OI? Change shows
   today's activity; total OI shows the big walls. Default: change, with total OI shown in
   the list too.
2. View name: **"OI over time"** (default) or Sensibull's "Multistrike OI".
