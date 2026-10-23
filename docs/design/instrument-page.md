# Instrument page: chart first

> Status: **Done (2026-10-23): IP1–IP3** · Builds on [ui.md](ui.md) §3 and the
> [OI buildup chart](oi-buildup-chart.md) · Reference: Kite web's chart tab (TradingView and
> ChartIQ), observed 2026-10-23

## 1. Why

On Kite web the right side of the screen is the chart and only the chart (tabs: Chart ·
Option chain · Fundamentals); market depth opens from the watchlist row. Our instrument
page stacks a ~460 px chart card, then market depth, then a stats block, so the chart gets
less than half the screen, the OI pane is a thin strip, and depth (empty before the open)
pushes the useful stats below the fold.

## 2. Decisions

| Topic | Decision | Why |
|---|---|---|
| Market depth | **Removed from the instrument page** | It is in the watchlist already (inline 5-level depth, UI2), as on Kite |
| Stats | One compact line under the header: O · H · L · Prev. close · OI · OI day high/low · circuits · lot | What depth used to push below the fold |
| Price and OI | **Stacked panes on one time axis**: price on top (~60 %), OI below (~40 %); one crosshair; zoom and scroll move both | Buildup is price *and* OI on the same candle: the two must line up vertically. Side by side (two time axes, half the width each) and a Price/OI switch (never both at once) lose that |
| Price / OI focus | A control in the chart toolbar: **collapse · normal · maximise** the OI pane (price : OI = 24:1, 3:2, 1:3); the divider stays draggable; the mode is remembered per browser | The switch's benefit (full height for one) without losing the shared axis |
| Volume | **Over the price pane** (translucent bars in its bottom ~25 %, own scale), not a separate pane | As Kite; the height goes to price and OI |
| OI pane content | **ΔOI bars** coloured by buildup state (body = OI change from the bar the state was measured against, up for additions, down for reductions; a **wick** over the bar's OI high and low measured from the same OI, so +500K reached but +300K kept shows), with the OI level as a thin line on its own scale; **no bar** (a gap) for "no OI update" | OI candles are slivers: a 15m OI move is 1–2 % of a multi-day OI range. ΔOI makes the size of each addition visible, and it is what the colour describes; the wick shows positions opened and closed inside the bar, as price wicks do |
| Legend | **Always visible**: interval · O H L C · change · volume, and for F&O · OI · ΔOI · OI range (only when the wick goes beyond the body) · state; the latest candle by default, the hovered one under the cursor | Kite shows it without hovering |
| Range | Presets under the chart: 1D · 5D · 1M · 3M · 6M · All (each picks a sensible default interval); intervals stay in the chart's top bar | Both Kite charts have them |
| Tabs | **Chart** (default) · **Option chain** (for options and futures: opens the chain of the underlying at this strike / expiry) | Kite's layout; room for Fundamentals later |
| Stocks and indices | No OI pane; price gets the full height | They have no OI (combined futures OI is a later idea) |

## 3. Layout

```
BAJFINANCE 23 Nov FUT   944.95  +16.25 (+1.75%)               [Chart] [Option chain]
O 915  H 947  L 915  Prev 928.70 · OI 29.28M (H 29.9M L 28.1M) · Circuit 851–1039 · Lot 750
[1m 3m 5m 15m 30m 1h 1D]        15m · O 943.1 H 945.0 L 942.6 C 944.95 · Vol 42.7K · OI 29.28M · ΔOI +61K · range −4K … +88K · Short buildup
┌ Price, volume over the bottom ─────────────────────────────────────────────── ~60% ┐
├────────────────────────────────────── drag ─────────────────────────────────────────┤
│ ΔOI bars (state colours) + OI line                                [–] [▢] [⤢]  ~40% │
└─────────────────────────────────────────────────────────────────────────────────────┘
[1D 5D 1M 3M 6M All]                                                     IST · auto
```

The chart fills the window's height below the header and stats line (minimum ~420 px).

## 4. Data

- REST bar rows gain **`oi_change`** (column 13): OI change from the reference bar the
  state was measured against (same rule as `buildup`; `null` when unknown). The colour and
  the bar height then come from one server-side calculation. Live minutes still form the
  current bar's OI from the stream; its ΔOI and state arrive with the minute-close refresh.
- Everything else (OHLC, volume, OI open/high/low/close, update time, state, quote stats)
  exists already.

## 5. Units

| Unit | Scope |
|---|---|
| **IP1** | Page: remove depth; stats line; Chart / Option chain tabs; chart fills the height |
| **IP2** | Price pane: volume overlay, always-visible legend, range presets |
| **IP3** | OI pane: `oi_change` in the bars API (+ tests, rest.md); ΔOI bars + OI line; collapse / normal / maximise; remembered split |

## 6. Later

Fundamentals tab; drawing tools and indicators (Kite has them; not our focus); the
implied-view label and combined futures OI ([OI buildup chart](oi-buildup-chart.md) §8).
