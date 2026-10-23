# ADR-0007: Lossy single-writer broadcast ring in shared memory for the market data bus

- **Status:** Accepted
- **Date:** 2026-10-20
- **Deciders:** Sivakannan, Claude

## Context

[ADR-0006](0006-multi-process-from-the-start.md) makes the system multi-process from
the start. Processes on one machine need a transport that moves market data from one
writer to several readers with low latency, where a slow or stuck reader must never
affect the feed. Full design: [market data bus](../design/market-data-bus.md).

## Options considered

### Transport
- **Unix domain sockets / pipes** — simple, kernel-managed, but a syscall and copy per
  message per reader, and the kernel buffer applies back-pressure to the writer.
- **Message broker / library (Aeron IPC, ZeroMQ, LBM)** — proven, but hides exactly
  what this project sets out to learn and adds a large dependency.
- **Shared-memory ring buffer (own implementation)** — no syscalls on the data path,
  one copy per reader, full control of semantics. More work and subtle correctness.

### Slow-reader policy
- **Writer waits for the slowest reader** (Disruptor-style gating) — no loss, but one
  stuck reader halts the feed.
- **Writer never waits; lapped readers detect loss** (Agrona broadcast-style) — the feed
  is isolated from readers; readers recover via journal replay or snapshot, as with
  exchange multicast.

### Message layout
- **Fixed-size slots** — simplest torn-read detection, but cannot carry raw frames.
- **Variable-length records** — carries raw Kite frames and any future message type.

## Decision

A **single-writer, lossy, variable-length broadcast ring buffer in POSIX shared
memory**, modeled on Agrona's broadcast buffer:

- writer never blocks; readers keep private cursors and detect being lapped via a
  *tail intent* counter;
- 8-byte-aligned records (`length`, `type`, payload), padding records at wrap-around;
- payload copied with relaxed 64-bit atomic accesses so the design is data-race-free
  under the C++ memory model;
- pluggable wait strategy (busy-spin / back-off / sleep);
- ring logic independent of shared memory, so it can also run in-process.

## Rationale

Isolating the feed from slow consumers is the most important property for a market
data system; loss is rare at our rates given a large ring, and the recovery paths
(journal replay, snapshot + incremental) already exist in the design. Building it
ourselves is a deliberate learning investment; following Agrona's proven algorithm
limits the correctness risk.

## Consequences

- Positive: no syscalls or locks on the data path; readers can join, leave and crash
  freely; writer restart is transparent to readers.
- Negative: readers must handle `Lapped`; every reader copies each message once;
  correctness depends on careful memory ordering (mitigated by TSan, multi-process
  stress tests on ARM64 and x86-64).
- Revisit if: we need cross-machine distribution (consider Aeron/UDP multicast), or
  benchmarks show the copy-out dominates (consider validated zero-copy reads).

## Learning notes

- Single-writer principle; why it enables lock-freedom.
- Release/acquire fences, fence-to-fence synchronization, and why ARM64 exposes ordering bugs x86 hides.
- Seqlocks and the C++ memory model (Boehm), `std::atomic_ref`, P1478 `atomic_memcpy`.
- Comparison with LBM, Aeron IPC and the LMAX Disruptor.
- Interview: "Design a shared-memory market data distribution between processes. What happens when a consumer is slow?"
