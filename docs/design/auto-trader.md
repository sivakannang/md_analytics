# Auto trader: mda-autotrader and the follow-signal strategy

Design for ADR-0030. mda-autotrader runs configured strategies on the market signals
(ADR-0028) and trades the owner's and friend's **paper** accounts through mda-broker
(ADR-0029), like any other participant: its orders go on `ord-auto`, the broker's auto risk
limits apply, and the account's kill switch stops it.

## 1. Goals and non-goals

- **Goals:** hands-off paper trading on signals; every decision recorded with its reason;
  P&L after charges per run, to judge a strategy honestly; safe by construction (stops
  resting at the broker, flat by 15:10, the broker's risk on every order).
- **Non-goals (v1):** real orders; options strategies; several positions per run; changing
  runs from the web app (config and a restart for now).

## 2. Data flow

```
sig (MarketSignal) ─┐
md (quotes)        ─┼─► mda-autotrader ──ord-auto──► mda-broker ──exec──► mda-autotrader
paper.db (read, at start) ┘     │                         (risk, fills, paper.db)
                                └─► data/autotrader.db (decisions; single writer)
```

- One thread, like signald: each loop drains `sig`, `md` and `exec`, then evaluates every
  run whose inputs changed and, once a second, every run (for time-based exits).
- Session time comes from the md bus (as in mda-broker), so a replay of the tick journal
  runs the whole chain (signald → autotrader → broker) on session time.

## 3. Configuration

```yaml
bus:
  ord_auto: { name: ord-auto, capacity_mb: 4 }   # already defined (ADR-0029)
autotrader:
  db: data/autotrader.db
  runs:
    - id: nifty-follow            # stable: tags its orders ("auto:nifty-follow")
      user: 1                     # whose paper account (app.db user id)
      strategy: follow-signal
      signal: oi-bias-v1          # which signald strategy it follows
      underlying: NIFTY           # trades its near-month future
      lots: 1
      product: MIS
      stop_points: 40             # resting SL-M at the broker after each entry
      target_points: 80           # 0 = none
      max_hold_min: 45
      max_trades_per_day: 6
      cooldown_min: 10            # after an exit, no new entry for this long
      exit_score: 10              # hysteresis: held until the score falls below this (a short: above −this)
      daily_loss_stop: 5000       # rupees, P&L after charges: no new entries after it
      enabled: true
```

## 4. Strategy `follow-signal`

States per run: **Flat → Entering → Long / Short → Exiting → Flat** (with a cooldown).

| When | Then |
|---|---|
| Flat; the signal (fresh, ≤ 2 min old) is Bullish / Bearish; guards allow | MARKET buy / sell `lots` of the near-month future → Entering |
| Entering; filled | Place a resting **SL-M** at entry ∓ `stop_points` at the broker → Long / Short |
| Long / Short; the signal turns opposite, or its score falls through `exit_score` (below +10 for a long, above −10 for a short) | Cancel the stop, MARKET exit → Exiting ("signal changed") |
| Long / Short; LTP reaches entry ± `target_points` | Same ("target") |
| Long / Short; held `max_hold_min` | Same ("max hold") |
| Long / Short; 15:10 | Same ("end of day"); the broker's 15:25 MIS square-off is the backstop |
| The stop fills at the broker | → Flat ("stopped out") |
| Exiting; filled | → Flat; cooldown starts |
| An order is rejected or cancelled by the broker | Record why; Entering → Flat; Exiting → retry once, then alert |

- **Contract:** the index's near-month future from the catalog (nearest expiry ≥ today;
  on its expiry day, the next month's: no new positions in an expiring contract).
- **Why a resting stop:** if mda-autotrader dies, the position is still protected; the
  broker fills the stop on the live book.
- **Code (AT1):** `libs/autotrade/run.hpp`. A `Run` takes signals, prices, the clock and
  its own execution reports and returns actions and decisions. It counts its net position
  from its own fills, so any order of reports ends flat: a position with no order closing
  it gets one (5 s after a failed exit). A position always gets a resting stop, including a
  position restored after a restart. The broker dropping the stop (rejected, expired) means
  an exit at market. Exits are checked in this order: end of day, signal change, target,
  max hold. The stop trigger is rounded on the tick away from the market.
- **Why MARKET orders:** one or two lots are far inside the visible book; a missed entry or
  exit costs more than a tick of slippage. The paper venue charges the slippage honestly.

## 5. Guards

Checked before every entry; a blocked entry is recorded with the guard's name.

| Guard | Rule |
|---|---|
| Fresh signal | The signal's minute is at most 2 minutes old |
| Feed | The md feed is connected and the future's quote is under 5 s old |
| Window | 09:20–15:00 for entries (exits until 15:10; the broker's auto window) |
| Trades per day | At most `max_trades_per_day` entries; an entry that doesn't fill (rejected, cancelled) gives its count back and starts the cooldown |
| Cooldown | `cooldown_min` after the last exit |
| Daily loss | The run's P&L today (after charges) above −`daily_loss_stop` |
| Kill switch | The account's kill switch is off (the broker would reject anyway) |
| Expiry | No entry in a contract expiring today |

## 6. Orders and recovery

- **Client order ids:** derived from the run, the day and a per-run counter, so a resend is
  the same order and a restart can tell its orders from the person's.
- **Tag:** `auto:<run id>` on every order, shown on the Paper page.
- **Resend:** as the gateway: every 500 ms until a report for that client order id arrives;
  after 2 s, record "no answer" and reconcile from paper.db.
- **Restart:** read the run's MIS position in its future and its live `auto:<run id>`
  orders from paper.db; rebuild the state (Long / Short with its stop, or Flat). Decisions
  and counters for today come from autotrader.db.

**Built (AT2):** `engine/apps/mda-autotrader`. A run's orders carry the tag `auto:<run id>`
cut to the field's 15 characters (`auto:nifty-foll`); recovery matches on that and refuses
two runs with one tag. Recovery waits until `exec` can be heard, so no report is missed
between reading paper.db and listening. The live config ships the run with
`enabled: false`; the replay config enables it.

**Replayed** on 2026-10-23 09:15–11:15 (journal → engine → signald → autotrader → broker,
scratch databases), with mda-autotrader killed and restarted right after its first entry:
it came back "restored -65 at 22695.10 with its stop (order 2)", later cancelled that stop
when it exited at the target, sat out cooldowns (one recorded decision per signal), and
traded four times in all: target, stop, its own exit and stop. Gross P&L ₹916.50 against
₹3,506 of charges: −₹2,589.50 net, as the design expected of an untested signal.

**Measured (AT3):** `libs/autotrade/metrics.hpp` and `GET /paper/auto` (docs/api/rest.md).
On a second replay of 2026-10-23 09:15–11:15 the API's metrics matched an independent
computation from paper.db exactly: 3 round trips, 2 wins, gross ₹3,321.50, charges
₹2,631.09, net +₹690.41, max drawdown ₹3,506.33; exits target, signal, stop. That replay
traded differently from AT2's (4 trips, −₹2,589.50): at 10× the run is evaluated every
200 ms of wall time, i.e. every 2 s of session time, so entries right at a cooldown's end
can land on different ticks. Replays at 10× are not exact reproductions for the auto
trader; a 1× replay is close to live.

**Shown (AT4):** the Paper page's **Auto** tab (`web/src/paper/AutoRuns.tsx`): each run's
on/off state, position, settings, metrics over 5 / 30 / 90 days and today's decisions;
orders from the auto trader carry an AUTO pill (SYSTEM for the broker's square-off) on the
Orders tab; the 15:35 daily summary adds "Auto trader: <run>: n fills, net ₹…" when the
auto trader traded that day.

**Why, in the decision itself:** an entry's reason, and an exit on a signal change, name the
signal's largest factors (the MarketSignal's top four on the bus), e.g. "Bearish -30: OI
shift -20, futures vs VWAP -15, sector breadth +5". Blocked entries keep the short form
("Bearish -60: cooldown").

**Fixed while checking AT4:** a run left holding a position when the market had closed
retried its flattening exit every 5 s forever (each rejected "market is closed"). Now a
flatten is attempted only 09:15–15:30, and failed exits back off 5 s, 10 s, 20 s … up to
5 minutes (reset once flat); a stop is cancelled once, not on every retry.

## 7. autotrader.db

```sql
decisions (id, at, run, signal_minute, score, label, state_before, action, reason,
           order_client_id, price)      -- every action or blocked entry
runs_day (run, day, entries, realised, charges, last_exit_at)   -- guards survive restarts
```

## 8. Measuring a run

From paper.db (orders tagged `auto:<run id>` and their trades) per run and period:

- P&L after charges, gross P&L, charges;
- trades, win rate, average win / average loss, largest loss;
- maximum drawdown (of cumulative P&L after charges);
- exits by reason (signal, target, stop, max hold, end of day).

The bar is clear: positive P&L after charges over weeks, with drawdown the owner accepts.

## 9. Units

| Unit | Scope | Done when |
|---|---|---|
| AT1 | `libs/autotrade`: run state machine, guards, contract choice, client order ids; tests | Every transition and guard tested on scripted signals and prices |
| AT2 | `mda-autotrader`: buses, resend, decisions in autotrader.db, recovery from paper.db, config, stack.sh, sysmon row | A replayed day (journal → signald → autotrader → broker) trades, stops and exits as designed; a restart mid-position resumes |
| AT3 | Run metrics (§8) and REST `GET /paper/auto` | Numbers checked by hand on a replayed day |
| AT4 | Web: Auto tab on the Paper page (runs, state, decisions, metrics); the daily summary gets an auto section | Checked on the replay test stack |

## 10. Strategy `follow-signal-options` (option buying)

The same signal and run rules as `follow-signal` (§4, §5), trading **bought NIFTY options**
instead of the future: a Bullish signal buys the ATM call, a Bearish one the ATM put. It runs
**beside** the futures run, so the Auto tab compares the two on the same calls, after costs.

Why (estimates, 1 lot, 30-minute hold): an ATM option costs about ₹70 a round trip in charges
against ₹1,000 for the future, because STT is on the premium, not on ₹14 lakh of notional;
with a delta near 0.5 and some time decay, it breaks even on a NIFTY move of about 5–7 points
against about 16 for the future, and the most it can lose is the premium (~₹10,000).

| Rule | Options run |
|---|---|
| **Contract** | The expiry: the nearest weekly with at least one more day to run (never trade on expiry day: time decay is steepest). The strike: the one nearest the NIFTY spot at entry (ATM). CE for Bullish, PE for Bearish |
| **Entry** | BUY 1 lot with a **LIMIT at the ask + `entry_slip_ticks`** (default 2): marketable, but it can't walk a thin book. Not filled within `entry_timeout_s` (default 10) → cancel → flat ("entry not filled") |
| **Protective stop** | A resting **SL** (stop-limit) SELL at the broker: trigger at entry premium × (1 − `premium_stop_percent`/100) (default 30%), limit `stop_limit_ticks` (default 10) below the trigger. Not SL-M: the broker (like Zerodha) is the place to keep option stops realistic, and SL-M isn't accepted on options |
| **Exits** | Measured on the **index**, as the signal is about the index: the signal turns opposite or its score falls through `exit_score`; NIFTY moves `target_points` in favour (target) or `index_stop_points` against (index stop); `max_hold_min`; 15:10. Exits are SELL LIMIT at the bid − `entry_slip_ticks`, re-priced every few seconds until filled (a MARKET sell of an option can walk far down a thin book) |
| **Size** | `lots` (default 1); the broker's auto limits apply (2 lots, 1% price band on LIMIT orders: the slip stays well inside it) |
| **Never** | Writes (sells to open) an option: the broker rejects it anyway |

Configuration (a second run; one run per user, underlying **and strategy**):

```yaml
    - id: nifty-options
      user: 1
      strategy: follow-signal-options
      signal: oi-bias-v1
      underlying: NIFTY
      lots: 1
      product: MIS
      target_points: 40          # index points in favour
      index_stop_points: 25      # index points against: exit
      premium_stop_percent: 30   # resting SL on the option
      max_hold_min: 45
      max_trades_per_day: 6
      cooldown_min: 10
      exit_score: 10
      daily_loss_stop: 3000
      enabled: true
```

- **Inputs added:** the NIFTY spot's last price (md bus) and the chain's strikes and expiries
  (catalog); quotes and depth of the chosen option (subscribed already: feedd keeps ATM ± 20
  strikes of the next three NIFTY expiries).
- **Recovery** as §6: the held option comes from the run's tagged orders in paper.db.
- **Metrics** as §8, per run: the two cards on the Auto tab, side by side.

### Units

| Unit | Scope | Done when |
|---|---|---|
| AT5 | `libs/autotrade`: the options run (contract choice from chain and spot, limit entry with timeout, premium SL, index-based exits, re-priced limit exits); config `strategy: follow-signal-options` | Every rule tested on scripted signals, spot and option quotes, including an unfilled entry and an exit that needs re-pricing |
| AT6 | mda-autotrader: chain lookup and spot input, both strategies in one process; the Auto tab shows the option held | A replayed morning trades both runs on the same signals; numbers checked |

**Status (2026-10-25):** AT5 and AT6 done.

- `OptionsRun` and `choose_atm` are in `libs/autotrade/options_run.hpp`. mda-autotrader
  holds either strategy per run. It reads the option's bid and ask from depth snapshots,
  and the index from its snapshots. While flat it picks the ATM pair again every 200 ms.
- Re-priced exits are Modify requests. Each one gets its own client id, because the
  broker dedupes only new orders.
- `GET /paper/auto` adds `held` (instrument and quantity). The Auto tab shows the contract
  held (e.g. "Long 65 NIFTY26OCT24000PE") and the options settings.
- **The SL needed one rms change:** a stop-limit's limit is now banded around its
  **trigger**, not the LTP (paper-trading.md §9). Otherwise the auto 1% band rejected
  every premium stop, which rests 30% below the market.
- **Replay of 2026-10-23, 09:15–11:15 at 10×,** both runs on user 1:
  - 5 option entries on the same signals as the futures run. Every entry filled at the ask
    or better, and every stop was accepted and then cancelled on exit.
  - Exits: target, signal change, and index stop twice.
  - The four closed option trades made ₹1,868.75 gross (checked by hand against the fills)
    for ₹303 of charges. The futures run made ₹1,131 gross for ₹4,295 of charges.
  - The replay used a target of 80 points; the live config uses 40.
  - When a replay ends, the clock falls back to the wall clock, so the broker expires a
    still-open stop as "placed on an earlier day". That is the replay ending; on a live
    day the dates match.

## 11. Open questions

- Which accounts run it: only the owner's at first, or the friend's too?
- MIS (flat every day, v1) or NRML for multi-day signals later?
- Futures only, or option buying? Decided 2026-10-25: both, side by side (§10).

### Exit hysteresis (2026-10-26)

The first live afternoon (26 Oct, 14:35–15:05) closed all 8 positions on "signal changed" within
5–15 minutes, several at a Neutral score just under the entry threshold (+23 against +25) after one
factor (futures buildup, ±20) flipped for a minute. Gross −₹8,449, charges ₹3,686. An entry still
needs the label (±25), but a position is now held until the label turns opposite or the score
falls through `exit_score` (default 10). Turning it up to the entry threshold restores the old rule.
