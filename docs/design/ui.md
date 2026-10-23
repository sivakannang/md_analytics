# UI design: workspaces, widgets and the analytics we add

Status: **Draft for review**. Built in units UI1 to UI6, listed at the end.

## 1. Reference: what Kite does (observed 2026-10-22)

We use Kite as the *minimum* bar for layout and behaviour, not for visual identity. The
repo is public, so md_analytics keeps its own name, colours and styling.

### Workspaces
- A **workspace switcher** in the header: `Default`, `Trade`, and `+ New workspace`
  (user-made). Each workspace has settings, share and delete.
- **Default** has a fixed two-pane layout: the marketwatch is always on the left, and a
  page (dashboard, orders, holdings…) is on the right. The header has live NIFTY 50 /
  SENSEX and the main navigation.
- **Trade** is a widget terminal. It is a grid of docked panels, and each panel holds
  tabs of widgets with a `+` to add more. The default arrangement:
  - left: Marketwatch;
  - centre: Chart (plus notes and calculators);
  - right: Option chain;
  - bottom: Positions / Orders / GTT / Alerts.

  The header scrolls a ticker of indices. Widgets can be maximised and reset.
- **Widget catalog:** searchable, grouped (Featured, Trading, Markets, Media, Productivity,
  Misc). It includes chart, marketwatch, option chain, order window, positions, orders,
  holdings, market depth, screener, alerts, GTT, calendars, notes, and more.

### Marketwatch
- Numbered lists (1–7) at the bottom, each up to 250 items, **split into named groups**
  (e.g. "Indices", "Options"). A group can be collapsed, maximised, renamed and reordered.
- Row: symbol (coloured by direction), exchange / kind tag, change, change %, an arrow,
  and LTP.
- Row hover: drag handle, **B / S**, **depth**, **chart**, **delete**, **more**.
- **Depth** expands inline under the row: Bid / Orders / Qty | Offer / Orders / Qty with
  quantity bars and totals. It expands further to 20 levels, then shows a day-range bar
  (Open, Low, High, Prev close), Volume, Avg price and circuit limits.
- Display settings: change measured from the previous close or today's open; show or hide
  change / % / direction / notes / group colours; sort within a group by %, LTP, A–Z or
  exchange.
- Search on Ctrl+K.

### Option chain
- Expiry pills (weekly / monthly, days to expiry), with an **OI** or **Greeks** view.
- OI view, per side: OI (lakhs), OI change %, LTP change %, LTP. A horizontal OI bar runs
  under each strike (calls in red to the left, puts in green to the right). The ATM strike
  is highlighted.
- Greeks view: IV, Delta, Theta, Vega, Gamma per side.
- Footer: **PCR, Max pain, ATM IV, IV percentile**.
- Settings: OI change absolute or %, strike range ±N.
- Row hover: B / S, chart, add to basket.

### Charts
- A built-in light chart: drawing tools, 1D… intervals, ranges 1D–5Y, and a few
  indicators (moving averages, momentum, ROC, volume). **There is no open-interest
  indicator.**
- "Advanced" opens a full TradingView chart.

## 2. Where md_analytics goes further

Kite is a trading app. Our edge is **derivatives analytics**:

| Feature | Kite | md_analytics |
|---|---|---|
| OI over time on a chart | no (built-in chart) | price + OI panes per instrument; combined futures OI |
| OI buildup classification | no | colour-coded candles: long buildup / short buildup / short covering / long unwinding / neutral ([oi-buildup-chart.md](oi-buildup-chart.md)) |
| Strike-wise OI change through the day | snapshot only | OI and OI-change charts per strike and per time window |
| Chain analytics | PCR, max pain, ATM IV | plus PCR / max pain **over time**, IV skew and term structure |
| Replay | no | replay any recorded day tick by tick (recorder) |
| Signals | price alerts | analytics signals (OI buildup, PCR shifts) to Telegram / email |
| Feed transparency | – | feed health, latency and the age of every quote |

## 3. Our layout

### Workspaces
- **Default:** a fixed shell, like Kite's Default.
  - header: index strip (NIFTY 50, BANKNIFTY, SENSEX; configurable), navigation,
    feed badge, account;
  - left: watchlist panel (resizable, collapsible);
  - right: a routed page (Dashboard, Chart, Option chain, OI analytics, Alerts).
- **Analyst (our "trader view"):** a widget terminal, with docked, tabbed and resizable
  panels and a widget catalog. It ships with a preset tuned for derivatives (chart + OI
  pane, option chain, strike OI chart, watchlist). The layout is saved per user on the
  server, so it follows you across browsers.
- More user workspaces come later.

**Docking library:** [dockview](https://github.com/mathuo/dockview) (MIT, no
dependencies, React bindings, serialisable layouts, tabs, splits, drag and drop).
Writing a docking system ourselves would take weeks for no gain.

### Watchlist (gaps to close against Kite)
- **Groups inside a list.** This needs a schema change: a `group` per item, or a
  `watchlist_groups` table. Raise the limits to 10 lists × 250 items.
- Hover actions: depth, chart, OI analytics, remove (no B/S until we trade).
- Inline **5-level depth** (Kite Connect's stream carries 5 levels; 20 needs Kite's own
  feed) and the day-range bar, volume, avg price, OI.
- Drag to reorder; sort within a group; Ctrl+K search; keyboard navigation (↑/↓, D for
  depth, C for chart).
- Display settings: change from close or open; which columns show.

### Charts
- **TradingView Lightweight Charts** (Apache-2.0, ~45 KB): candles, volume, and an extra
  **OI pane**; buildup colouring as a candle style.
- Intervals 1m / 3m / 5m / 15m / 1h / 1D, built server-side from ticks. The browser gets
  history via REST and then live bar updates over the existing stream.

## 4. Data each widget needs

| Widget | Needs | Have it? |
|---|---|---|
| Index strip, watchlist prices | live quotes | **yes** (W3) |
| Inline depth | DepthSnapshot over the stream | on the bus; stream `depth` message to add |
| Day range, volume, avg, OI | MarketSnapshot fields | **yes** |
| Circuit limits | Kite REST `/quote` (daily) | no — fetch once per day per instrument |
| Charts | 1m bars (price, volume, OI) + history | **no** — needs the bar builder and storage |
| Option chain | live quotes for ±N strikes around ATM, every expiry | **no** — needs the subscription manager |
| Greeks / IV | option pricer (Black-76) + risk-free rate | no |
| OI analytics | bars per strike + buildup classifier | no |

## 5. Units

| Unit | What | Depends on |
|---|---|---|
| **UI1** ✅ | Default shell: header index strip, resizable watchlist panel + routed right pane, keyboard shortcuts | — |
| **UI2** ✅ | Watchlist parity: groups, 250 items, hover actions, inline 5-depth + stats, drag reorder, sort, display settings | stream `depth` message |
| **UI3** | Bars: engine builds 1m OHLC+V+OI bars from the bus, stores them (recorder + bar store), gateway serves history + live bars | engine, storage decision (backlog) |
| **UI4** ✅ (price, volume, OI panes; buildup colouring after O3) | Chart widget: Lightweight Charts with price, volume, OI panes and buildup colouring | UI3 |
| **UI5** ✅ | Option chain: subscription manager (ATM ± N windows), chain widget (OI and Greeks views), PCR / max pain / ATM IV | Black-76 pricer |
| **UI7** ✅ | Instrument page, chart first: no depth (it's in the watchlist), stacked price/OI panes, volume overlay, ΔOI bars ([instrument-page.md](instrument-page.md), units IP1–IP3) | UI4 |
| **UI6** | Analyst workspace: dockview terminal, widget catalog, saved layouts; strike OI charts | UI4, UI5 |

UI1 and UI2 use only data we already have, so they can start now. UI3 is the large
backend step that unlocks charts.

## 6. Out of scope for now
- Order entry (B/S buttons, order window, positions, baskets, GTT). Algo trading comes
  later and will need its own design (risk checks, kill switch, audit).
- Holdings and funds: those are Kite account pages, not market data.
