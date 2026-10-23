# md_analytics — Vision & Goals

> Status: **Living document.** The product will evolve; this file records *what* we are
> building and *why*. *How* we build it is recorded in [ADRs](adr/README.md) and
> [design notes](design/).

md_analytics has three goals, kept deliberately separate:

| Goal | Question it answers |
|---|---|
| [Product](#1-product-goal) | What are we building? |
| [Learning](#2-learning-goal) | What do we want to master by building it? |
| [Technology](#3-technology-goal) | How do we intend to engineer it? |

**Guiding principle:** build a useful product, develop deep engineering expertise, and
lay a foundation for electronic trading infrastructure — without letting any one goal
silently sacrifice the others.

---

## 1. Product goal

A **real-time, broker-agnostic market-data and derivatives analytics platform**,
growing over time into a personal **algorithmic trading platform**.

### Users

| Stage | Users | Notes |
|---|---|---|
| Now | The author | Primary user; uses it on live trading days |
| Soon | Author + one friend | Feedback user; exercises multi-user paths |
| Maybe | External users | Only if it becomes a real product — requires market-data licensing review first |

Multi-user support exists from early on so the design stays honest, but it is
**not** a commercial redistribution service.

### Core capabilities

| Area | Description |
|---|---|
| Broker connectivity | Zerodha Kite Connect first; more brokers and direct exchange feeds later |
| Session management | Daily Kite login (manual, one click), token storage, expiry handling |
| Real-time processing | Receive, decode, normalize and fan out live market data |
| Subscription management | Derive instrument lists from config; roll ATM strike windows; respect broker limits |
| Market snapshots | Latest price, volume, OI and market depth per instrument |
| Option chain | Live option chains, starting with NIFTY, then stocks and other indices |
| OI analytics | Long/short buildup, short covering, long unwinding from price/OI change |
| Options analytics | Implied volatility and Greeks (computed by us — Kite does not provide them) |
| Charts | Price, volume and OI candles across timeframes |
| Historical data | Store data for charts, comparisons and research |
| Watchlists & signals | User watchlists; signals derived from analytics |
| Publishing | Push live data to each user for the instruments they subscribe to |
| Recording & replay | Record live sessions; replay them (real-time or accelerated) for testing and research |

### Initial instrument universe (Phase 1)

| Segment | Scope (configurable) |
|---|---|
| NIFTY 50 index | Spot, futures, options **ATM ± 20 strikes** |
| NSE F&O stocks (~220) | Cash price, **all 3 futures**, options **ATM ± N strikes** (N = 5 or 10) |
| Later | BANKNIFTY, SENSEX (BSE), BANKEX (BSE), other indices |

**Known constraint:** Kite allows ~3,000 instruments per WebSocket connection and
3 connections per API key (~9,000 total). Stock options at ±10 strikes exceed this,
so a subscription budget with priorities and a rolling ATM window is a Phase 1
requirement, not an optimization.

### Roadmap

| Phase | Theme | Highlights |
|---|---|---|
| 0 | Foundations | Repo, toolchain, CI, domain model, Kite tick decoder, recorder/replayer, load generator |
| 1 | Market-data analytics MVP | Live NIFTY + stock F&O feed, snapshots, option chain, OI analytics, price/OI charts, storage, web dashboard, REST/WS API |
| 2 | Advanced analytics | More indices, IV/Greeks, configurable indicators, OI trends, historical comparison, alerts, signals |
| 3 | Platform | Second user, more brokers, retention, multi-process deployment, monitoring, Linux server |
| 4 | Trading | Strategies, paper trading, pre-trade risk checks, OMS, order execution, fund allocation |

**Product principle:** ship a focused, working product early; avoid decisions that
needlessly block later phases — but don't build later phases early.

---

## 2. Learning goal

Use md_analytics to gain hands-on, **interview-grade** experience in the engineering of
professional electronic trading and low-latency systems — and be able to explain every
design decision and trade-off.

Career target: roles in HFT / low-latency trading infrastructure, using modern C++.

| Area | Topics |
|---|---|
| Architecture | Modularity, interfaces, dependency direction, trade-offs, ADRs |
| Modern C++ | C++20/23, templates, concepts, RAII, move semantics, `std::span`, `std::expected`, allocators |
| Concurrency | Threads, atomics, memory ordering, lock-free queues (SPSC/MPSC), false sharing |
| IPC | In-process queues vs Unix sockets vs shared memory — benchmarked |
| Networking | TCP, TLS, WebSocket, binary protocols, epoll/kqueue, reconnect & recovery |
| Performance | Cache behaviour, allocation, latency histograms, profiling, CPU pinning, NUMA |
| Linux | Scheduling, isolcpus, IRQ affinity, kernel/network tuning, `perf` |
| Data engineering | Time-series storage, compression, retention, historical queries |
| Reliability | Watchdogs, health checks, logging, metrics, graceful shutdown/restart |
| Testing | Unit, integration, replay-based, load, stress, performance regression |
| DevOps | Git, CI/CD, reproducible builds, containers, deployment, monitoring |
| Frontend | React, TypeScript, financial charting, efficient real-time UI |
| Quant basics | Option pricing (Black-76), implied volatility, Greeks |

### Learning cycle (applied to every major component)

1. **Understand** — requirements, constraints, expected behaviour.
2. **Explore** — alternatives and their trade-offs.
3. **Decide** — record the choice in an ADR.
4. **Implement** — clean interfaces, tested code.
5. **Measure** — correctness, latency, throughput, failure recovery.
6. **Reflect** — write lessons learned and likely interview questions in [`docs/learning/`](learning/).

Example: before choosing an IPC mechanism, benchmark in-process queues, Unix domain
sockets and shared memory on the same workload, then decide.

### Working rhythm

- Small, reviewable units of work, each with a test or benchmark.
- Daily habit of 1–2 hours; each unit sized to fit one session.
- Every decision is explained and reviewed before it is committed.

---

## 3. Technology goal

Engineer the system with modern, portable, measurable technology choices.

| Layer | Choice | Status |
|---|---|---|
| Core engine | C++20, selective C++23 | Decided |
| Dev OS | macOS ARM64 | Decided |
| Target OS | Linux x86-64 | Decided |
| Build | CMake (+ Ninja, presets) | Decided |
| Dependencies | vcpkg (manifest mode) | Proposed |
| Kite WebSocket client | Boost.Beast (Asio) | Proposed |
| Frontend gateway (REST + WS) | Beast / uWebSockets / Drogon | Open |
| Frontend | React + TypeScript + Vite | Proposed |
| API docs | Markdown + OpenAPI (REST) + AsyncAPI (WS), modeled on Kite's API style | Proposed |
| Config | YAML | Proposed |
| Logging | spdlog (or async alternative) | To evaluate |
| Testing | GoogleTest, Google Benchmark | To evaluate |
| Charting | TradingView Lightweight Charts or alternatives | To evaluate |
| IPC | In-process / Unix sockets / shared memory | Open — benchmark first |
| Storage | SQLite / DuckDB / Parquet / hybrid | Open |
| Tooling & research | Python (daily login helper, notebooks) — never on the hot path | Proposed |
| CI | GitHub Actions: macOS Clang + Linux GCC, sanitizers, secret scanning | Proposed |
| Containers / cloud | Docker, AWS (Mumbai) or similar | Later |

### Engineering principles

- **Measure, don't assume.** Optimize from profiles and benchmarks.
- **Portable core.** Develop on macOS; CI on Linux from day one.
- **Broker isolation.** Broker-specific code never leaks into core analytics.
- **Data correctness.** Preserve exchange vs receive timestamps, contract identity, OI observations and data-quality flags.
- **Reliability is a feature.** Disconnects, stale data, expired tokens and crashes are normal states, not exceptions.
- **Simplicity.** No framework, service or dependency without a reason recorded in an ADR.
- **Architecture first.** Document decisions and interfaces before major implementation.
- **Secrets never in git.** The repository is public.

### Deployment path

Single laptop → single Linux machine (multi-process) → dedicated/cloud server.
Designed for the later steps, built for the first.
