# ADR-0028: Market signals in a separate process (mda-signald, `sig` bus)

- **Status:** Accepted
- **Date:** 2026-10-24
- **Deciders:** owner, Claude

## Context

We want to estimate the direction of NIFTY, SENSEX, BANKNIFTY and BANKEX and show it in
the web app, and we expect to run **several strategies** side by side (a rule-based score
first, learned models later, each with its own horizon). Every strategy needs a track
record before anyone trusts it.

Facts that shape the choice:

- Every input is already on the `an` bus: 1-minute `Bar`s with OI for spots, futures and
  options (type 6), `OptionAnalyticsMessage` with IV, Greeks and previous-day OI (7) and
  `ChainSummaryMessage` with PCR, max pain, forward and ATM IV per expiry (8).
- Strategies will change often: weights, thresholds, new factors. A restart of mda-engine
  during market hours skips its first minute and rebuilds its analytics state.
- Each bus has one writer (ADR-0010); `an` is written only by mda-engine.
- Signals are minute-level: a millisecond of extra latency doesn't matter.

## Options considered

### Option A: strategies inside mda-engine, published on `an`
- Pros: no new process or bus; inputs are in memory, no extra hop.
- Cons: every strategy change restarts the engine, which hurts bars and option analytics
  during market hours; a strategy bug can crash or stall the engine; the engine grows a
  second job.

### Option B: new process `mda-signald`, reads `an`, writes a new `sig` bus
- Pros: restart freely while the market is open; failures stay contained; strategies are
  developed against a stable input; the process can replay recorded `an` data later.
- Cons: one more process to run and monitor, one more bus, and the gateway reads three
  buses.

### Option C: strategies in the gateway
- Pros: no new process.
- Cons: the gateway serves users; computation there would scale with clients, not
  instruments, and ties signal restarts to the web app.

## Decision

Option B. A new process **mda-signald** reads `an` (and the instrument catalog), runs the
enabled strategies, publishes **`MarketSignal` (message type 9)** on a new bus **`sig`**
(single writer: mda-signald) and stores every signal and its outcome in its own
**`data/signals.db`**. Strategy code lives in a library, **`libs/signals`**, so the
strategies are testable without the process. The gateway reads `sig` for live signals and
`signals.db` (read-only) for history and track records.

Strategies are compiled in and enabled per underlying in `config/mda.yaml`; no plugins.

## Rationale

The deciding forces are the plan to run several strategies and the cost of restarting the
engine during market hours. Keeping the engine the single source of analytics and adding
a consumer follows the pattern already used for the gateway, recorder and notifier. A
separate SQLite file keeps one writer per database, as `bars.db` and `last_quotes.db` do.

## Consequences

- Positive: strategies change and restart without touching market data or analytics; each
  strategy's record is measured in one place; the bus pattern stays one writer per bus.
- Negative / costs we accept: another process in `tools/stack.sh` and Admin → System; a
  third bus for the gateway; warm-up logic after a restart (read today's bars from
  `bars.db`).
- Follow-ups: compiled-in strategies mean a rebuild per change; revisit (config-driven
  weights, or a Python research loop that exports coefficients) once there are several
  strategies. Revisit the process split if signals ever need tick-level latency.

## Learning notes

- Single-writer buses and fan-out readers; why a consumer process beats in-process hooks
  for code that changes daily.
- Look-ahead bias and leakage: a signal at minute *t* may use only bars closed by *t*.
- Evaluating a direction signal: hit rate against a naive baseline, calibration (does
  "70%" come true 70% of the time), walk-forward rather than one train/test split.
- Interview questions: "How would you add a new consumer to a market-data pipeline without
  risking the producer?" "How do you know a signal has edge?"
