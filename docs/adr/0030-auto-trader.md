# ADR-0030: An automatic paper trader (mda-autotrader) that acts on market signals

- **Status:** Accepted
- **Date:** 2026-10-24
- **Deciders:** owner, Claude

## Context

mda-signald (ADR-0028) gives a direction signal per index each minute; mda-broker
(ADR-0029) fills paper orders against the live book, with Zerodha's charges and pre-trade
risk. The owner wants a trader that acts on the signals by itself, to learn whether a
strategy makes money **after costs**, not just whether its calls are right.

Facts that shape the choice:

- The 10-day backtest (docs/design/signals.md §12) found no edge yet: oi-bias-v1 ties
  momentum on NIFTY and SENSEX and loses on the bank indices. One NIFTY futures round trip
  costs about ₹1,000, roughly 6 bp. An auto trader will most likely lose money at first;
  measuring that honestly is the point.
- The broker already treats `ord-auto` as the automatic source: it sets the source from the
  bus, applies the stricter auto limits (2 lots per order and per instrument, ₹40 lakh
  gross, 1% price band, 30 orders a minute (6 until 2026-10-25, when four runs started), 2% daily loss, trading 09:20–15:10), and the
  account kill switch stops it.
- More strategies will follow (ADR-0028 expects several): the trader must be one engine
  running configured strategies, like signald.
- Each person has one paper account. Trades the robot makes must be visible as its own
  (source `auto`) next to the person's.
- Real orders stay out of scope (ADR-0029): nothing here may reach a broker's order API.

## Options considered

### Option A: inside mda-signald
- Pros: signals are in memory; no new process.
- Cons: mixes forecasting with execution; a signald restart (common while tuning) would
  also restart trading; signald would need the order and report buses.

### Option B: inside mda-broker
- Pros: positions and orders are right there.
- Cons: the broker is the neutral risk gate and the record of truth for every source;
  strategy code inside it could bypass its own checks. The ETT roadmap keeps participants
  outside the broker.

### Option C: a new process, mda-autotrader, as one more client of the broker
- Pros: it is exactly a participant: reads `sig` and `md`, sends on `ord-auto`, reads its
  reports on `exec`; the broker's risk applies to it like to anyone; it can be stopped,
  replayed (journal → signald → autotrader → broker on replay buses) and replaced.
- Cons: one more process; it must rebuild its view of positions after a restart (from
  paper.db, read-only).

## Decision

Option C. **mda-autotrader** runs the strategies configured in `config/mda.yaml`
(`autotrader.runs`), each with its user, its index, its contract and its limits:

- **Inputs:** MarketSignal on `sig`, quotes on `md` (for prices and stops), its own
  ExecutionReports on `exec`; at start, its open positions and orders from `paper.db`
  (read-only), so a restart resumes.
- **Output:** OrderRequests on **`ord-auto`** only (its single writer), with client order
  ids that make retries safe, resent until the broker answers (as the gateway does).
- **Accounts:** a run trades its user's own paper account; orders show as source `auto`.
  The account's kill switch stops it (the broker rejects its opening orders), and the
  person can still close what it opened.
- **Strategy v1, `follow-signal`:** go long or short one lot of the index's near-month
  future when the signal turns Bullish or Bearish; exit when it turns Neutral or opposite,
  after a maximum holding time, on a stop loss or target (points), and in any case by
  15:10. Guards: trades per day, a cooldown after an exit, its own daily loss stop, no new
  entries on a stale signal (older than 2 minutes) or with the feed down.
- **Decisions are recorded:** every evaluation that leads to an action (or is blocked by a
  guard) is written with its reason to **`data/autotrader.db`** (single writer), so a day
  can be reviewed: "entered long at 10:42 on Bullish +38; stop hit at 11:05".
- **The strategy logic is a library**, `libs/autotrade`: pure functions of (signal, price,
  position, time, config), tested without the process; new strategies plug into it.
- **Still paper only.** mda-autotrader has no broker client; real trading would need its
  own ADR and would not reuse this path without one.

## Rationale

The trader is a participant, so it belongs outside both the forecaster and the broker; the
broker's risk then constrains it exactly as it constrains a person, which is the property
that matters most for unattended code. Keeping strategies pure and their decisions
recorded makes them testable, replayable on the tick journal and reviewable, which is how
we'll learn whether any strategy has an edge after costs.

## Consequences

- Positive: hands-off paper trading on signals with a full audit trail; a template for more
  strategies; the first ETT "participant" talking to the broker over its own order bus.
- Negative / costs we accept: one more process and a database; it rebuilds positions after
  a restart; v1 has one strategy and one contract type (index futures).
- Follow-ups:
  - The Paper page's Auto tab (runs, positions, decisions, P&L after charges) and a pause
    per run.
  - Options: decided 2026-10-25 to build option buying beside the futures run
    (`follow-signal-options`, docs/design/auto-trader.md §10): ATM calls / puts on the same
    signal, limit entries and exits, a resting SL on the premium, exits measured on the index.
  - A replay backtest over recorded days (journal → signald → autotrader → broker).
  - Revisit if a strategy beats both baselines after costs for several weeks.

## Learning notes

- Strategy vs execution vs risk as separate components, and why unattended code must go
  through the same risk gate as people.
- Idempotent order entry and state recovery for an automated client (client order ids,
  resend, reconcile from the system of record).
- Position-holding rules: entry, exit on signal change, max holding time, stops; why
  cooldowns and trade caps matter with noisy signals.
- Measuring a strategy: P&L after charges, hit rate, average win / loss, drawdown, trades
  per day; transaction costs as the hurdle.
- Interview questions: "How would you build a simple algo trading system safely?" "How do
  you stop an algo that misbehaves?"
