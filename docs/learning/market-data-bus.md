# Learning notes: market data bus

Companion to the [design](../design/market-data-bus.md) and
[ADR-0007](../adr/0007-shared-memory-broadcast-bus.md). What we measured, what it means,
and the questions it prepares for.

## 1. Benchmark results (Unit 4)

Tool: [`engine/bench/bus_bench.cpp`](../../engine/bench/bus_bench.cpp) — run with
`build/release/engine/bench/mda-bus-bench` (release builds only).

Machine: Apple M4 (4 performance + 6 efficiency cores), macOS, laptop on battery/AC as
found. **No core pinning** (macOS doesn't support it); processes request the
`USER_INTERACTIVE` QoS class to prefer performance cores. Date: 2026-10-21.

### One-way latency, 64-byte messages (µs), paced at 200k msg/s

| Transport / wait strategy | min | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| Same ring, two **threads**, busy-spin | 0.04 | **0.12** | 0.12 | 0.7–1.0 | 14–22 | 55–201 |
| **shm**, two **processes**, busy-spin | 0.04 | **0.12** | 0.12 | 0.4–1.1 | 14–18 | 62–67 |
| shm, processes, backoff | 0.04 | 0.17 | 0.21 | 3.6–3.8 | 24–25 | 92–167 |
| Unix datagram socket, busy-poll (`MSG_DONTWAIT`) | 0.46 | 0.62–0.88 | 0.7–1.0 | 1.0–1.2 | 29–33 | 72–85 |
| Unix datagram socket, blocking `recv` | 0.46 | 4.4–5.1 | 5.9–6.4 | 10–109 | 49–2463 | 98–3074 |

At a Kite-like live rate (5k msg/s, a message every 200 µs):

| Wait strategy | p50 | p90 | p99 |
|---|---|---|---|
| shm, backoff (default config) | 35 | 140 | 148 |
| shm, fixed sleep 100 µs | 58 | 105 | 115 |

Ranges are across three runs; medians were stable, tails were not.

### Throughput, writer publishing flat out, one reader process

| Payload | shm (M msg/s) | shm GB/s | Unix socket (M msg/s) | Unix socket GB/s |
|---|---|---|---|---|
| 16 B | 29.8 | 0.47 | 2.4 | 0.04 |
| 64 B | 26.4 | 1.69 | 1.8 | 0.11 |
| 256 B | 22.5 | 5.73 | 1.9 | 0.48 |
| 1024 B | 21.3 | 21.6 | 2.0 | 2.08 |

Reader received 100% in every shm run (no laps): it keeps up with a writer at ~30M msg/s.

### `publish()` alone (no readers)

| 16 B | 64 B | 256 B | 1024 B |
|---|---|---|---|
| 4.3 ns | 5.6 ns | 11.7 ns | 40 ns |

## 2. What the numbers mean

1. **Shared memory between processes is as fast as between threads** (~120 ns p50 both).
   The cost is not "IPC" — it is moving cache lines from the writer's core to the reader's
   core. Process isolation is free on the data path.
2. **Unix sockets: ~5× slower when busy-polling, ~40× when blocking**, and ~10–15× lower
   throughput. Every message is a system call on each side plus a kernel copy; blocking
   adds a scheduler wake-up (µs) and terrible tails (up to 3 ms).
3. **The wait strategy dominates latency more than the transport.** Busy-spin 0.12 µs vs
   backoff 0.17 µs under load, but 35–140 µs when messages are sparse and backoff has
   fallen asleep. At live Kite rates this is irrelevant (Kite's own latency is
   milliseconds), so **backoff is the right default on the laptop**; busy-spin on a
   pinned, isolated core is for the Linux box.
4. **Tails come from the OS, not the ring.** p99.9 of 14–25 µs and maxima up to 200 µs
   are scheduler preemption, interrupts and migrations between performance/efficiency
   cores. Fixing tails needs Linux: `isolcpus`, CPU pinning, IRQ affinity — the next
   learning step.
5. **`publish()` costs 4 ns for small messages**; cost grows with size because of the
   word-by-word copy (40 ns at 1 KB ≈ 25 GB/s).
6. **Clock resolution matters.** Apple Silicon's monotonic clock ticks every 41.67 ns
   (24 MHz), so "min 0.04" is one tick and "p50 0.12" is three ticks: sub-100 ns figures
   are quantized. On x86 Linux, `clock_gettime`/TSC resolve ~1 ns.

## 3. Methodology notes

- **Latency is measured at a fixed send rate**, below capacity, so it reflects delivery,
  not queueing. A flat-out writer would measure how long messages wait in the ring.
- **Warm-up samples are discarded** (page faults, cold caches, CPU frequency ramp-up).
- **One run is not a result.** Medians repeat; tails vary run to run. Report ranges.
- **Same machine, same clock:** the monotonic clock is system-wide, so writer and reader
  timestamps are comparable across processes. Across machines this would need PTP.
- **Release build only** — the tool warns if `NDEBUG` is not defined.
- What's not measured yet: pinned cores, isolated CPUs, multiple readers' effect on the
  writer, huge pages. Linux machine.

## 4. Interview questions this unit prepares for

1. *Why is shared memory faster than a Unix socket?* — No system calls or kernel copy on
   the data path; one user-space copy; readers spin on a cache line instead of being woken
   by the scheduler. Measured: 0.12 vs 0.6 (busy) vs 4.8 µs (blocking) p50.
2. *Your p50 is 120 ns but p99.9 is 20 µs. Why, and how do you fix it?* — OS jitter:
   preemption, interrupts, core migration, frequency changes. Isolate and pin cores,
   move IRQs, disable frequency scaling/deep C-states, avoid page faults (pre-fault,
   `mlock`), avoid allocations and syscalls on the hot path.
3. *Busy-spin vs blocking: trade-offs?* — latency vs CPU and power; spin needs a
   dedicated core or it hurts everything else; hybrid back-off is a common compromise.
4. *How do you measure one-way latency between two processes?* — same-machine monotonic
   clock, timestamp at send, subtract at receive, paced load, warm-up, percentiles not
   averages, multiple runs, know your clock's resolution.
5. *What happens when a consumer is slow in your design?* — the writer never waits;
   the consumer detects being lapped and recovers (journal / snapshot), like multicast.
6. *Why 128-byte padding between counters?* — Apple Silicon's 128-byte lines and x86's
   adjacent-line prefetch; avoids false sharing between writer- and reader-owned lines.
