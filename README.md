# md_analytics

A real-time, broker-agnostic market-data and derivatives analytics platform for Indian
markets — written in modern C++ with a React/TypeScript dashboard — and a deliberate
learning project in low-latency trading infrastructure.

> **Status:** Phase 0 — foundations and design. Nothing runnable yet.

## Documentation

| Document | Purpose |
|---|---|
| [Vision & goals](docs/vision.md) | What we build, what we learn, how we engineer it |
| [Architecture overview](docs/architecture/overview.md) | Processes, data flows, memory vs storage |
| [Market data bus](docs/design/market-data-bus.md) | Shared-memory broadcast ring between processes |
| [Domain model](docs/design/domain-model.md) | Instrument identity, prices, time, bus events |
| [Kite adapter](docs/design/kite-adapter.md) | Kite binary tick format and decoder |
| [Instruments](docs/design/instruments.md) | Instrument registry, daily Kite import, token reuse |
| [Kite connectivity](docs/design/kite-connectivity.md) | Session, REST, WebSocket, mda-feedd lifecycle |
| [User management](docs/design/user-management.md) | App sign-up/sign-in, sessions, SQLite schema |
| [REST API](docs/api/rest.md) | Endpoints for the dashboard (Kite-style) |
| [Notifications](docs/design/notifications.md) | Signals and alerts via Telegram and email (planned) |
| [OI buildup chart](docs/design/oi-buildup-chart.md) | Price and OI candles, buildup classification |
| [Recovery & gap filling](docs/design/recovery-and-gap-filling.md) | Crash recovery and intraday bar backfill |
| [Architecture decisions](docs/adr/README.md) | Why each significant choice was made |
| [Learning: market data bus](docs/learning/market-data-bus.md) | Benchmark results, lessons, interview questions |

## Repository layout

Directories are created as each component is built.

```
md_analytics/
├── docs/
│   ├── vision.md          # product, learning and technology goals
│   ├── adr/               # architecture decision records
│   ├── architecture/      # system overview, data flow, threading
│   ├── design/            # one design note per component
│   ├── api/               # REST + WebSocket spec for the frontend
│   ├── learning/          # lessons learned and interview Q&A per module
│   └── runbooks/          # operational how-tos (daily login, recording)
├── engine/                # C++ backend
│   ├── libs/              # all logic, as libraries (unit tests in libs/<name>/tests/)
│   │   ├── core/          # ids, prices, time, byte order, errors
│   │   ├── bus/           # shared-memory broadcast ring
│   │   ├── messages/      # event types carried on the buses
│   │   ├── runtime/       # process skeleton: config, logging, signals, heartbeat
│   │   ├── instruments/   # instrument master, contract identity, expiries
│   │   ├── feed/          # broker-agnostic adapter interface + normalizer
│   │   ├── kite/          # Zerodha Kite client (the only Kite-aware module)
│   │   ├── subscription/  # instrument budget, rolling ATM window
│   │   ├── journal/       # tick journal: record, replay, recovery
│   │   ├── market/        # snapshots, bars, option chain
│   │   ├── analytics/     # OI buildup, IV/Greeks, signals
│   │   ├── storage/       # bar store, app DB
│   │   └── gateway/       # REST/WS server, sessions, fan-out
│   ├── apps/              # thin process wrappers: mda-feedd, mda-replayer, mda-recorder,
│   │                      # mda-engine, mda-gateway, mda-supervisor, tools/
│   ├── tests/             # cross-process integration and crash tests
│   └── bench/             # latency/throughput benchmarks
├── web/                   # React + TypeScript dashboard
├── tools/                 # Python: mda_login, research — never on the hot path
├── config/                # example configuration (no secrets)
├── scripts/               # git hooks, dev setup
├── deploy/                # later: systemd / Docker
├── secrets/               # git-ignored credentials and session token
├── data/                  # git-ignored journals, bars, research output
└── .github/workflows/     # CI
```

Dependency rules: `kite/` is the only module that knows about Zerodha; libraries never
depend on `apps/`; `core/` depends on nothing; `bus/` depends only on `core/`.

## Building

Prerequisites (macOS): Xcode command-line tools, CMake ≥ 3.28, Ninja, and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```bash
cmake --preset dev-debug           # configure (vcpkg fetches dependencies on first run)
cmake --build --preset dev-debug   # build
ctest --preset dev-debug           # run tests
```

Other presets: `dev-asan` (AddressSanitizer + UBSan), `dev-tsan` (ThreadSanitizer),
`release` (optimized with debug info). See [ADR-0004](docs/adr/0004-compilers-and-ci-matrix.md).

### Web dashboard

Needs Node.js. Start `mda-gateway` first (it serves the API on `127.0.0.1:8090`), then:

```bash
npm --prefix web install
```

```bash
npm --prefix web run dev
```

Open <http://127.0.0.1:5173>. The dev server proxies `/api` to the gateway
([ADR-0015](docs/adr/0015-frontend-stack.md)).

## Security

This repository is public. API keys, secrets and access tokens must never be
committed — they live in the git-ignored `secrets/` directory or environment variables.
