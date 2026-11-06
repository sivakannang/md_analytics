# ADR-0029: Paper trading: a paper broker process (mda-broker) with OMS, pre-trade risk and a venue interface

- **Status:** Accepted
- **Date:** 2026-10-24
- **Deciders:** owner, Claude

## Context

The owner and one friend want to place paper (simulated) trades from the web app, and later
let an automatic trader act on signals (ADR-0028), to learn whether a strategy makes money
**after costs and slippage** without risking money. The ETT roadmap
(`docs/architecture/ett-platform.md`) has a broker (OMS, pre-trade RMS, back office,
terminal) and a sim exchange; this is the first piece of the broker.

Facts that shape the choice:

- Live prices and the 5-level book already arrive on the `md` bus (mda-feedd); the catalog
  has lot and tick sizes; users live in `app.db`.
- Buses are single-writer, lossy broadcast rings (ADR-0007): a reader that falls a full
  ring behind loses messages. Fine for prices; orders must not be lost silently.
- Order rates are tiny (a few per minute per user), but correctness matters: an order is
  acknowledged, filled, cancelled or rejected exactly once.
- Orders will come from more than one source: the web UI now, an auto trader next (ADR to
  follow), strategies in backtest later.
- No real orders: the project rules forbid order actions on Kite, and the roadmap keeps
  real-money paths last, behind an explicit ADR.
- Indian charges (brokerage, STT, exchange fees, SEBI fee, stamp duty, GST) are a few basis
  points per round trip on index futures: the size of a 30-minute signal's edge, so they
  must be modelled from the first trade.

## Options considered

### Option A: paper trading inside mda-gateway
- Pros: no new process or IPC; REST handlers call the OMS directly.
- Cons: the gateway serves browsers and restarts often; an auto trader would have to call
  REST with a service account; OMS state shares a process with HTTP. Doesn't grow into the
  roadmap's broker.

### Option B: a broker process, requests over local HTTP or a Unix socket
- Pros: reliable request/response with immediate acks; simple clients.
- Cons: a second protocol next to the buses; fills and order updates still need a push
  channel; less like an exchange order gateway.

### Option C: a broker process, orders and execution reports on buses
- Pros: the same transport as everything else; one input log per source and one output
  stream, which is how exchange gateways and OMSs work (sequence numbers, acks,
  idempotent client order ids); the broker is a single-threaded state machine whose inputs
  can be replayed.
- Cons: lossy rings need an end-to-end guard (client order ids, ack timeouts, resend,
  reconciliation from the broker's database).

## Decision

Option C, with persistence as the source of truth:

- A new process **mda-broker** owns paper accounts, orders, trades, positions and the
  ledger in its own **`data/paper.db`** (single writer). It is one thread: commands are
  applied in order, written to `paper.db` (event log plus state tables, one transaction),
  then published.
- **Order requests** arrive on one bus per source, each with its single writer:
  **`ord-gw`** (mda-gateway, for the web app) and later **`ord-auto`** (the auto trader).
  **Execution reports** (ack, reject, trigger, fill, cancel, expire) go out on **`exec`**,
  written by mda-broker. New message types: **OrderRequest (10)** and
  **ExecutionReport (11)**.
- Every request carries a **client order id** chosen by the source; the broker ignores a
  repeat, so a source that sees no ack within a timeout resends safely. A reader that is
  lapped reconciles from `paper.db` (orders and trades are the truth; the bus is the fast
  path).
- The OMS sends orders to a **venue** through an interface. **v1: the paper venue** fills
  against the live book from the `md` bus (depth walk, a queue rule for resting limits,
  staleness checks, optional latency). Later: the sim exchange's matching engine
  (roadmap phase 1) behind the same interface.
- **Pre-trade risk** (a library, `rms`) runs inside the broker on every order from every
  source: quantity and position limits, notional and margin, price bands, order rate, daily
  loss limit, trading hours, and a kill switch per account and global. Sources can't
  bypass it.
- **Charges** come from a dated table in config (`paper.charges`), applied per fill and
  shown per trade.
- **Scope of v1:** NFO/BFO index futures and option **buying**; products MIS (squared off near the
  close at a configured time, as the broker does) and NRML; order types MARKET, LIMIT, SL, SL-M; validity DAY. Option writing waits
  for a margin model (SPAN-like) and equities for a later unit.
- mda-broker has **no Kite order client**: it can't reach a real order API. Real trading,
  if ever, needs its own ADR.

## Rationale

The deciding forces are more than one order source, an auto trader that must go through
the same risk checks as a person, and the roadmap's broker and exchange. A separate,
single-threaded broker with an input log per source is the exchange-gateway shape the
owner wants to learn, and it lets the sim exchange replace the paper venue later without
touching the UI or the auto trader. Persisting before publishing, plus idempotent client
order ids, makes the lossy bus safe for orders at these rates.

## Consequences

- Positive: one place for orders, risk and P&L whatever the source; deterministic,
  replayable broker state; realistic fills and costs from day one; the first broker
  component of the ETT platform.
- Negative / costs we accept: one more process and three buses; clients need ack
  timeouts and reconciliation; paper fills are a model (no queue position, no market
  impact beyond the visible 5 levels).
- Follow-ups:
  - ADR-0030: the auto trader (signals → orders on `ord-auto`, paper only).
  - A margin model for option writing; equities.
  - Roadmap: the broker's paper half comes before the matching engine; recorded in
    `docs/architecture/ett-platform.md` §5.
  - Revisit the transport if order rates ever rise enough for lapping to be likely.

## Learning notes

- OMS order state machine (new → acked → partially filled → filled / cancelled /
  rejected), idempotency with client order ids, and why exchanges sequence everything.
- Pre-trade risk as a gate every order passes; kill switches.
- Fill simulation: crossing the spread, walking the book, queue position for resting limit
  orders, and why paper P&L flatters a strategy (no impact, no queue).
- Indian charges and why a few basis points decide a short-horizon strategy.
- Interview questions: "Design an OMS." "How do you make order entry over an unreliable
  channel safe?" "How would you backtest a strategy so paper results are believable?"
