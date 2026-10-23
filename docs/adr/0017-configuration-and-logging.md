# ADR-0017: YAML configuration and spdlog logging for every process

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

With `mda-feedd` joining `mda-gateway`, processes have real tuning knobs (bus size, feed
mode, subscriptions, reconnect timing, how often prices are pushed to browsers) and need
clear startup and audit logs (what was loaded, which port, which user signed in).
Hard-coded values and `printf` don't scale.

## Options considered

### Configuration
- **Command-line flags only** — fine for 3 options, unreadable for 30.
- **JSON** (Boost.JSON already linked) — no comments; awkward to hand-edit.
- **TOML** — good, smaller ecosystem in C++.
- **YAML (yaml-cpp)** — comments, nesting, the format the vision named; widely used.

### Logging
- **printf/iostream** — no levels, no files, blocking.
- **spdlog** — fast, levels, rotating files, async mode, fmt formatting; the de-facto C++ choice.
- **Quill / NanoLog / fmtlog** — lower hot-path latency (binary logging); worth it if we ever
  log on the hot path — we don't.

## Decision

- One **YAML** file, default `config/mda.yaml` (committed, no secrets), `--config PATH` to
  override. Each process reads its own section plus shared ones (`logging`, `bus`, `kite`).
  Unknown keys are errors (catches typos); every setting has a documented default.
- **spdlog** via the `runtime` module: console (coloured) + rotating file
  `data/logs/<process>.log` (10 MB × 5). Async logger with a bounded queue that **drops
  rather than blocks** when full, so logging can never stall the feed.
- Startup logs: version, config path, every effective setting that matters, listen
  addresses, bus names/sizes. Audit logs: sign-up, sign-in (success/failure), sign-out,
  password change, account deletion — by user id/username only.
- **Never logged:** passwords, tokens, API keys/secrets, email addresses, phone numbers,
  message bodies.
- The hot path (per-tick work) does not log; it counts (bus stats, decoder stats) and a
  periodic summary line reports the counters.

## Consequences

- Positive: one place to tune everything; reproducible runs (log the effective config).
- Negative: two more dependencies (spdlog with fmt, yaml-cpp).
- Revisit if logging ever enters the hot path (binary loggers).

## Learning notes

- Async logging and back-pressure policy (drop vs block) in latency-sensitive systems.
- Interview: "How do you log from a trading process without hurting latency?"
