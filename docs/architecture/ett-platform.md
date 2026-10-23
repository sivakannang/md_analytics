# ETT platform: the long-term map

> Status: **Living roadmap** (2026-10-23) · Today's system: [overview.md](overview.md) ·
> Each module gets its own ADR and design before code, as everything so far.

## 1. Goal

Grow md_analytics into a small but complete **electronic trading technology (ETT)** platform:
every role of a real market built once, end to end, on the same engine. It is a learning
vehicle for HFT and trading-systems work, and a useful product for two users along the way.
Nothing here trades real money unless a later ADR says so explicitly.

## 2. The actors and their systems

```
   Participants                  Broker                      Exchange                 Clearing / depository
 ┌──────────────┐ orders ┌───────────────────┐ orders ┌──────────────────┐ trades ┌───────────────────┐
 │ Retail UI    │───────▶│ OMS + pre-trade   │───────▶│ Order gateway    │───────▶│ Novation, margin, │
 │ Algo / HFT   │◀───────│ RMS, client API,  │◀───────│ MATCHING ENGINE  │        │ MTM, settlement,  │
 │ Sim agents   │ fills  │ back office       │ fills  │ Risk controls    │        │ holdings          │
 └──────────────┘        └───────────────────┘        │ MD publisher ────┼──┐     └───────────────────┘
        ▲                                             └──────────────────┘  │
        │      market data (ticks, depth, trades)                           │
        └────────────── Market data platform + analytics ◀──────────────────┘
```

## 3. Modules

Status: **done** (exists today), **partial**, **new**.

### 3.1 Exchange (a simulated NSE)

| Module | Scope | Status |
|---|---|---|
| Matching engine | Book per instrument, price-time priority; limit, market, IOC/FOK, stop; modify, cancel; trades; deterministic | new |
| Order gateway | Sessions, binary order protocol (NNF-like), throttles, acks, sequence numbers | new |
| Exchange risk | Price bands and circuits, freeze quantity, self-trade prevention, kill switch | new (circuits are read from Kite today) |
| Session manager | Pre-open call auction, continuous, closing auction, halts (ADR-0023 research) | new |
| Market data publisher | Tick-by-tick orders and trades plus snapshots, sequence numbers, gap recovery; publishes **our bus messages** | new |
| Reference data | Contract master, lots and ticks, expiries, calendar and holidays | partial (`instruments`, `catalog`) |

### 3.2 Clearing and depository (a simulated NSE Clearing)

| Module | Scope | Status |
|---|---|---|
| Novation and positions | Central counterparty; net positions per member and client | new |
| Margin | SPAN-like scenario margin plus exposure; intraday calls | new (uses `options` Greeks) |
| MTM and settlement | Daily MTM, expiry settlement (cash / physical), T+1 equity | new |
| Depository | Demat holdings, pledges | new |

### 3.3 Broker (a simulated Zerodha)

| Module | Scope | Status |
|---|---|---|
| Client API | REST + WebSocket like Kite Connect: orders, positions, holdings, market data | partial (gateway: auth, REST, stream) |
| OMS | Order state machines, routing, idempotent order tags, reconciliation | new (trading terminal T2) |
| Pre-trade RMS | Margin and limit checks, fat-finger guards, auto square-off | new |
| Back office | Ledger, contract notes, Indian charges (brokerage, STT, stamp duty, GST, SEBI, exchange fees), P&L and tax reports | new |
| Onboarding | Users, simulated KYC, segments, risk profiles | partial (auth, roles) |
| Trading terminal | Watchlists, charts, chain, OI views, order ticket, positions and P&L | partial (the analysis half exists) |

### 3.4 Participants

| Module | Scope | Status |
|---|---|---|
| Retail users | People on the terminal | done |
| Simulated liquidity | Agents that make the sim market realistic: market makers quoting around live Kite prices, noise and momentum traders | new |
| Strategy SDK | Write strategies once; run them in backtest, paper and (later) live | new |

### 3.5 Market data and analytics

| Module | Status |
|---|---|
| Broker feed handler (Kite) — mda-feedd | done |
| Shared-memory bus, fixed-layout messages (ADR-0010) | done |
| Tick journal, recorder, replayer (ADR-0026) | done |
| Bars, OI buildup, option analytics, OI by strike, OI over time | done |
| Feed handler for the sim exchange's feed | new |
| Long-term historical store (columnar) | partial (`bars.db`, journal) |

### 3.6 HFT lab

| Module | Scope | Status |
|---|---|---|
| Backtester | Journal replay into the sim exchange with queue position and latency, so fills are believable | new (builds on the replayer) |
| Latency measurement | Timestamps at every hop, tick-to-trade histograms | partial (system monitor histogram) |
| Low-latency techniques | Lock-free structures, cache-friendly books, core pinning, kernel bypass (study) | partial (the bus) |
| Colocation model | Per-participant network delay so speed matters | new |

### 3.7 Cross-cutting

| Module | Status |
|---|---|
| Surveillance and compliance (spoofing, layering, wash trades, OTR, audit trail) | new |
| FIX gateway and drop copy | new |
| Time synchronisation (one clock; PTP in real life) | partial (ns timestamps) |
| Identity and entitlements | partial (auth, roles) |
| Observability and ops (monitor, alerts, runbooks) | done for today's stack |
| Determinism and recovery (state rebuilt from an event log) | pattern exists (journal) |

## 4. Principles

1. **Venue-agnostic.** Analytics, terminal and strategies consume the same bus messages
   whether the venue is Kite (real NSE) or the sim exchange. A venue id in the instrument key
   tells them apart; nothing downstream special-cases the source.
2. **Deterministic cores.** The matching engine, OMS and clearing are single-threaded state
   machines driven by an ordered event log, so a replay reproduces every state exactly
   (testing, recovery, debugging).
3. **One strategy, three modes.** The same strategy code runs in backtest (journal → sim
   exchange), paper (live data → sim exchange) and, only if ever decided, live (→ broker).
4. **Read-only until proven.** Real-money order paths come last, behind paper trading, RMS
   and an explicit ADR.
5. **Measure.** Latency and throughput are recorded per component from the first version.

## 5. Order of work

| Phase | Deliverable | Why this order |
|---|---|---|
| 0 | md_analytics: market data + analytics (mostly done) | The foundation |
| 1 | Matching engine for one instrument → many; deterministic; tests and benchmarks | The core of an exchange and the best interview topic |
| 2 | Order gateway, market data publisher onto the bus, sessions and auctions | The sim market becomes visible in our existing UI |
| 3 | Simulated liquidity agents anchored to live Kite prices | A book worth trading against |
| 4 | Broker: OMS, pre-trade RMS, client API, paper accounts, terminal screens (T1 read-only views first) | Trading the sim market end to end |
| 5 | Clearing and back office: positions, margin, MTM, settlement, ledger, charges | Real P&L |
| 6 | HFT lab: strategy SDK, backtester, market-making strategy, latency | Strategies on the full stack |
| 7 | Surveillance, FIX gateway, drop copy, recovery drills | Completeness |

Revisit this order when a phase finishes; record changes here.
