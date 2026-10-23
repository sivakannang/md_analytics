# Architecture Decision Records

Why we chose what we chose. See [ADR-0001](0001-record-architecture-decisions.md)
for the process and [template.md](template.md) to write a new one.

## Index

| # | Title | Status |
|---|---|---|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions as ADRs | Accepted |
| [0002](0002-cpp20-cmake-ninja.md) | C++20 (with selective C++23), CMake, Ninja and presets | Proposed |
| [0003](0003-vcpkg-manifest-dependencies.md) | Manage C++ dependencies with vcpkg in manifest mode | Proposed |
| [0004](0004-compilers-and-ci-matrix.md) | Compilers, warnings, sanitizers and CI matrix | Proposed |
| [0005](0005-secrets-handling.md) | Keep secrets out of the public repository | Proposed |
| [0006](0006-multi-process-from-the-start.md) | Multi-process architecture from the start | Accepted |
| [0007](0007-shared-memory-broadcast-bus.md) | Lossy single-writer broadcast ring in shared memory for the market data bus | Accepted |
| [0008](0008-instrument-identity.md) | Instrument identity: ContractKey, permanent InstrumentId, daily broker mapping | Accepted |
| [0009](0009-fixed-point-prices-and-nanosecond-time.md) | Fixed-point prices and nanosecond UTC timestamps | Accepted |
| [0010](0010-fixed-layout-snapshot-events.md) | Normalized events are fixed-layout snapshot structs | Accepted |
| [0011](0011-boost-beast-for-kite-connectivity.md) | Boost.Beast (Asio + OpenSSL) for Kite WebSocket and REST clients | Accepted |
| [0012](0012-sqlite-app-database.md) | SQLite for the app database | Accepted |
| [0013](0013-gateway-http-server-and-api-conventions.md) | mda-gateway HTTP server on Boost.Beast, Kite-style API conventions | Accepted |
| [0014](0014-authentication.md) | App authentication: Argon2id, server-side sessions, HttpOnly cookies | Accepted |
| [0015](0015-frontend-stack.md) | Frontend: React + TypeScript + Vite, minimal dependencies | Accepted |
| [0016](0016-notification-service.md) | Notification service: separate process, Telegram and email first | Accepted |
| [0017](0017-configuration-and-logging.md) | YAML configuration and spdlog logging for every process | Accepted |
| [0018](0018-live-prices-to-browsers.md) | Live prices to browsers: conflated JSON snapshots over a WebSocket | Proposed |
| [0019](0019-admin-dashboard.md) | Admin dashboard: roles, auth event log and per-user usage | Accepted |
| [0020](0020-dynamic-option-subscriptions.md) | Dynamic option subscriptions: ATM windows with hysteresis in mda-feedd | Accepted |
| [0021](0021-bar-storage.md) | 1-minute bars in SQLite (bars.db), built by mda-engine, backfilled from Kite | Accepted |
| [0022](0022-option-analytics-in-engine.md) | Option analytics in mda-engine: Black-76 on a parity-implied forward | Accepted |
| [0023](0023-session-phases.md) | Session phases: pre-open and the closing auction (auction fields, band, bar sessions) | Accepted |
| [0024](0024-last-known-quotes.md) | Last-known quotes survive restarts: the engine saves the latest snapshot per instrument | Accepted |
| [0025](0025-auto-backfill.md) | Automatic bar backfill: mda-feedd fills history for subscribed instruments with none | Accepted |
| [0026](0026-tick-journal.md) | Tick journal: mda-recorder writes raw frames to hourly files; mda-replayer replays them on separate buses | Accepted |
| [0027](0027-remote-access-tailscale.md) | Remote access over Tailscale: the gateway serves the built site, `tailscale serve` adds HTTPS inside the tailnet | Accepted |

## Backlog (decisions to make soon)

| Topic | Notes |
|---|---|
| Repository layout | Monorepo: `engine/`, `web/`, `tools/`, `docs/` |
| Recording format | Decided in ADR-0026 (raw frames, hourly files, compression later) |
| Threading model | feedd v1: single thread (kite-connectivity §5); engine/gateway threading TBD |
