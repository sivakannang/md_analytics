# ADR-0006: Multi-process architecture from the start

- **Status:** Accepted
- **Date:** 2026-10-20
- **Deciders:** Sivakannan, Claude

## Context

The [architecture overview](../architecture/overview.md) splits the system into
processes (`mda-feedd`, `mda-recorder`, `mda-engine`, `mda-gateway`, …) connected by a
market data bus. We had to choose the build order for the first milestone.

## Options considered

### Option A — Single process first, split later
Modules as libraries in one process, connected by in-process queues; introduce the
shared-memory bus and split into processes after an IPC benchmark.
- Pros: live option chain on screen sooner; fewer moving parts initially.
- Cons: process-isolation, crash-recovery and IPC problems are postponed; in-process
  shortcuts (shared pointers, shared state) tend to creep in and make the later split harder.

### Option B — Multi-process bus first
Build the shared-memory bus first, then build each component as its own process on top.
- Pros: the real architecture from day one; crash isolation and replay-based recovery
  work immediately; no hidden in-process coupling; the bus — a core HFT topic — is
  learned and benchmarked first.
- Cons: longer before anything is visible; more up-front design.

## Decision

**Option B.** Build the market data bus first, then the processes on top of it.

## Rationale

The learning goal places IPC, lock-free structures and process isolation at the centre
of the project, and the recovery design (journal replay, engine restart without feed
loss) depends on separate processes. Building them first avoids a costly retrofit.

## Consequences

- Positive: production-shaped system early; crash recovery testable from the first milestone.
- Negative: the first visible result (live data on screen) comes later.
- Mitigation: components remain **libraries with thin process wrappers**, so tests can
  still run them in-process and the IPC benchmark can compare transports fairly.
- The IPC comparison (shared memory vs Unix sockets vs in-process queues) still happens —
  as a benchmark of the bus we build, not as a prerequisite for choosing it.

## Learning notes

- Process isolation vs threads: failure domains, address spaces, restart semantics.
- Interview: "Why would a trading system split feed handling and strategy into separate processes?"
