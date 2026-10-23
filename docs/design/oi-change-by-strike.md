# OI change by strike

> Status: **Done 2026-10-23: OC1–OC3, OC5** (OC4, past days, later) · Builds on [option chain and bars](option-chain-and-bars.md)
> §3 (minute bars with OI) and §6 (chain page) · Reference: Sensibull → Analyse → Open
> Interest → OI Change, observed 2026-10-23

## 1. Why

The OI chart on the instrument page follows **one contract over time**. Option writers'
positioning is read the other way: **all strikes at one moment**, or between two moments.
"Where were calls written since 09:15? Did puts get added below spot in the last 30
minutes?" That is the view traders open first on Sensibull. Every number it needs is
already in `data/bars.db`: OI per option per minute, for each chain's subscribed strikes.

## 2. What the user sees

```
Option chain   [Chain] [Greeks] [OI by strike]
NIFTY  22,538  ·  Expiries ☑ 27 Oct  ☐ 03 Nov  ☐ 09 Nov  ·  Strikes ±[5 10 15 20]  ·  ☑ Show OI
[5m 15m 30m 1h 2h 3h Full day]   09:15 ●━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━● 11:20     OI as of 11:19:37
┌─────────────────────────────────────────────────────────────────────────────────────────┐
│        ▓                                         ┆                    ▒               │
│ ▓      ▓▒          ▓            ▓                ┆         ▒          ▒      ▒        │
│ ▓▒  ▓  ▓▒   ▓▒   ▓ ▓▒  ▓▒  ▓▒   ▓▒  ▓▒  ▓  ▒  ▓▒ ┆  ▓▒  ▒  ▒▒  ▓▒  ▒   ▒▒  ▒  ▒▒  ▒      │
│ 22000  22050  22100  …                     spot 22,538                      … 23000     │
└─────────────────────────────────────────────────────────────────────────────────────────┘
Put OI ■ +Δ ▨ −Δ ▢     Call OI ■ +Δ ▨ −Δ ▢
09:15 → 11:20   Call OI +5.97 Cr   Put OI +2.70 Cr   PCR 1.04   NIFTY 22,687.65 → 22,538.00
```

| Topic | Decision | Why |
|---|---|---|
| Where | A third view on the **option chain page** (Chain · Greeks · **OI by strike**), sharing its underlying picker | Same strikes and expiries as the chain; no new page to find |
| Bars | Per strike, a **put bar (green) and a call bar (red)** side by side | Sensibull's and the market's convention: put writing reads as support, call writing as resistance |
| Show OI on | Solid part = OI at the start; **striped** = increase on top; **hollow outline** = the part that was removed | Shows the change and the size of the position it changed |
| Show OI off | Only the change, from a zero line: up for additions, down for reductions | Small changes on big strikes become visible |
| Spot | A dashed vertical line at spot (between strikes, interpolated), labelled with the price at the end time | Where the writing is relative to price |
| Expiries | Checkboxes for the chain's expiries (default: nearest); OI of the ticked ones is **added** per strike | As Sensibull; weekly + monthly together shows total positioning |
| Strikes | ATM ±5 / 10 / 15 / 20 (default 10), ATM from spot at the end time | Our subscription window is ±20 (§2 of the chain design), so ±20 is the most we have |
| Time | Presets **5m · 15m · 30m · 1h · 2h · 3h · Full day** (default Full day), and a **two-handle slider** over the day's minutes for any from/to | Presets for the common questions, the slider for "what happened between 10:00 and 10:30" |
| Live | When the end handle is at "now", the view refreshes 2 s after each minute closes (as the price chart does); the bars move, the slider grows | OI changes at most once a minute per strike |
| Summary | Total call and put OI change, PCR at the end time, spot at both times | Sensibull's footer; one line |
| Hover | Strike · Call OI from → to (Δ, Δ %) · Put OI from → to (Δ, Δ %) | The exact numbers behind the bars |
| Units | Contracts × lot = shares, as the chain shows; axis in L / Cr | Same numbers as the chain's OI column |

**Not in v1:** past days (a date picker, OC4), a replay "play" button that animates the end
handle, and Sensibull's other OI tabs (§6).

## 3. Data: OI at a time

"OI at time T" for one option = `oi_close` of its **last minute bar that ended at or
before T** in which OI was published (`start + 60 ≤ T`, `oi_close > 0`, and an OI update in
the bar or a backfilled bar), looking back up to 7 days. The value is the same as the newest
bar's (OI is carried between updates), and the update time is known for "OI as of".

- **Full day** is from 09:15: the OI at 09:15 is the previous session's last OI, so "Full
  day" means "since yesterday's close" (OI doesn't change before the open).
- A strike that **wasn't subscribed** at T (the window re-centred later, or the contract is
  new) has no bar: its value is `null`, the bar is drawn hatched-grey with "no data at
  09:15" on hover, and it is left out of the totals. Auto-backfill (ADR-0025) fills most of
  these after the close.
- Spot at T = the close of the spot instrument's last minute bar ending at or before T.
- Times snap to whole minutes. "Now" = the last closed minute.

This is one indexed lookup per instrument and time on `bars_1m (instrument_id, start)`:
for ±20 strikes × 2 sides × 3 expiries × 2 times ≈ 500 lookups, a few ms in SQLite.

## 4. API

`GET /api/v1/chains/{underlying}/oi-change?expiries=2026-10-27,2026-11-03&from=<unix>&to=<unix>&strikes=10`
(or `minutes=30` instead of `from`: the presets, counted back from `to` on the server)

- `expiries`: one or more of the chain's listed expiries (default: the nearest).
- `from`, `to`: Unix seconds, snapped down to the minute; `to` defaults to the last closed
  minute, `from` to 09:15 IST of `to`'s day; `from < to`, same IST day in v1.
- `strikes`: 1–20 around ATM (default 10).

```json
{"status": "success", "data": {
  "underlying": "NIFTY", "expiries": ["2026-10-27"],
  "from": 1792727100, "to": 1792734600, "oi_as_of": 1792734577,
  "spot_from": 22687.65, "spot_to": 22538.0, "atm_strike": 22550,
  "session_open": 1792727100, "latest": 1792734600,
  "totals": {"call_from": 101326615, "call_to": 116312560, "put_from": 119887040, "put_to": 121002505},
  "pcr": 1.04,
  "rows": [{"strike": 22000, "call_from": 848250, "call_to": 866320, "put_from": 15060240, "put_to": 15680795}]
}}
```

`session_open`/`latest` bound the slider (`latest` = the end of the spot's last closed
minute); `oi_as_of` is the newest OI time among the
rows (for the "OI as of" label). Values are shares (contracts × lot), summed over the
ticked expiries; a side is `null` when no ticked expiry has a value for it.

## 5. Units

| Unit | Scope |
|---|---|
| **OC1** | Gateway: `BarStore::last_ended` lookup (+ tests: no bar, gap, lookback, snapping), the `oi-change` route (+ gateway tests on the in-memory fixture), `rest.md` |
| **OC2** | Web: the OI-by-strike view on the chain page: SVG bar chart (no lightweight-charts: it has no category axis), expiry checkboxes, ±N, presets, Show OI, summary, hover |
| **OC3** | Web: the two-handle time slider, minute refresh when the end is "now", "OI as of" |
| OC4 (later) | Past days: date picker, `from`/`to` on another day |
| **OC5** | Scrubbing (§8): `oi-timeline` route (+ tests, `rest.md`); the view computes every window in the browser |

## 6. Later (Sensibull's other OI tabs, from the same data)

- **Open Interest**: OI by strike at one time (OC2 with `from = to`, Show OI on).
- **Multistrike OI**: chosen strikes' OI as lines over time, with spot.
- **Option OI vs time**: daily total call and put OI with PCR and spot.
- **Fut OI vs time**: daily futures OI bars coloured by buildup (our classifier, per day,
  current + next month combined).

## 7. Open questions (defaults chosen)

1. Colours: puts green, calls red (as Sensibull). Alternative: our `--up`/`--down`.
   **Default: puts green, calls red**, new tokens `--oi-put`, `--oi-call`.
2. Strike set when expiries have different strike steps (weekly 50, monthly 100): use the
   union of strikes in the window. **Default: union**; a strike missing in one expiry just
   adds nothing from it.

## 8. Scrubbing: the session's OI in the browser (OC5, 2026-10-23)

Asked for: the bars should follow the slider **while it is dragged**, not only when it is let
go (Sensibull reloads on release). A request per drag step (~50 ms) would give 6–10 updates a
second and lag over the internet; instead the view loads the **whole session once** and
computes any window itself, at the speed of the mouse.

- `GET /chains/{u}/oi-timeline?expiries=&strikes=` returns, for the strikes in view (ATM ±N
  at the latest minute), each side's OI as a **step series** `[[m, oi], …]`: a point only when
  OI changes, `m` = minutes from 09:15 (the value holds from 09:15 + m minutes), `m = 0` the
  value before the open; ticked expiries are summed per strike. Spot is the same, per minute.
  Size, measured 2026-10-23 at 12:10 (3 h of session): 95 KB for one expiry at ±10, 164 KB
  for three expiries at ±20 (far strikes and expiries change less often); about twice that
  by the close. Loads in 40–80 ms; a slider move then redraws in ~3 ms with no request.
- "OI at T" in the browser = the series' last point with `session_open + m·60 ≤ T`: the same
  rule as `oi-change` (§3), so both give the same numbers.
- While live (a preset, or the end handle at the latest minute), the view reloads the
  timeline 2 s after each minute closes. A whole reload each minute keeps it simple at two
  users; an incremental `since=` is possible later.
- The strike set stays fixed while scrubbing (from spot at the latest minute); the spot line
  moves across it. `oi-change` stays for other clients and as the reference in tests.
- Next, cheap with this data: a ▶ Replay button (play the end handle forward), and
  Multistrike OI (§6).
