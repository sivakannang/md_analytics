# Market data bus

> Status: **Draft for review** · Decision record: [ADR-0007](../adr/0007-shared-memory-broadcast-bus.md)
> · Context: [architecture overview](../architecture/overview.md)

## 1. Purpose

Move messages from **one writer process** to **any number of reader processes** on the
same machine, with low latency, without the writer ever waiting for a reader.

| Bus instance | Writer | Readers |
|---|---|---|
| `md` (market data) | `mda-feedd` or `mda-replayer` | `mda-recorder`, `mda-engine` |
| `analytics` | `mda-engine` | `mda-gateway` |

Same library, different segments.

## 2. Requirements

| # | Requirement |
|---|---|
| R1 | Single writer, multiple independent readers; every reader can see every message (broadcast) |
| R2 | **The writer never blocks.** A slow reader can never slow the writer or other readers |
| R3 | A reader that falls a full buffer behind (**lapped**) detects it reliably and is told how much it lost |
| R4 | Variable-length messages (normalized ticks ~100 B, raw Kite frames up to several KB) |
| R5 | Readers can join and leave at any time; the writer does not know who is reading |
| R6 | Survives process crashes: a restarted writer continues; readers keep their position |
| R7 | Free of data races under the C++ memory model (clean under ThreadSanitizer) |
| R8 | macOS ARM64 (weak memory model) and Linux x86-64 |
| R9 | Throughput ≥ several million messages/s for high-speed replay; live load is a few thousand/s |

Non-goals: multiple writers per bus, cross-machine transport, reliable (no-loss) delivery
to slow readers. Loss is detected, and recovery is the reader's job (journal replay,
snapshot) — as with exchange multicast.

## 3. Design overview

A **lossy broadcast ring buffer** in shared memory, modeled on Agrona's
`BroadcastTransmitter`/`BroadcastReceiver` (used by Aeron).

- The writer appends records at a monotonically increasing 64-bit byte **position**.
  Position `p` lives at offset `p & (capacity - 1)` in the ring.
- Readers keep their **own** cursor in their own memory. The writer keeps no reader state.
- Before overwriting a region, the writer publishes **tail intent** — "I am about to
  write up to position X". A reader that copied a record checks afterwards that tail
  intent has not reached its record + capacity. If it has, the copy may be torn: the
  reader was lapped.

```
              ┌─────────────── ring: capacity bytes (power of 2) ───────────────┐
 segment:  [ header ][ rec ][ rec ][ rec ][ pad ]... [ rec ][ rec ]  free/old  ][
              ▲                                         ▲          ▲
              metadata + counters                       latest     tail
```

## 4. Shared memory layout

```
offset 0      SegmentHeader          (1 page; read-mostly)
              ├─ magic            u64   "MDABUS\0\1"
              ├─ layout_version   u32
              ├─ capacity         u64   ring size, power of 2
              ├─ max_msg_length   u32   capacity / 8
              ├─ writer_pid       u32
              ├─ writer_epoch     u64   incremented on each writer (re)start
              ├─ created_ns       u64
              └─ bus_name         char[32]
next lines    Counters — each on its own 128-byte line (Apple M-series line size;
              avoids false sharing on both ARM64 and x86-64)
              ├─ tail_intent      atomic u64   writer → readers: "writing up to here"
              ├─ tail             atomic u64   writer → readers: "published up to here"
              ├─ latest           atomic u64   position of the most recent record
              └─ heartbeat_ns     atomic u64   writer liveness
next line     WriterStats          (written only by the writer, relaxed)
              ├─ messages         atomic u64
              ├─ bytes            atomic u64
              └─ rejected         atomic u64   oversized / reserved-type publishes
next          ReaderSlot[16]       (one 128-byte line each; see §8a)
              ├─ pid              atomic u32   owner; 0 = free (ownership is the pid)
              ├─ active           atomic u32   1 once the fields below are valid
              ├─ name             atomic u64[3] 24 chars, atomic words (tools read it concurrently)
              ├─ position         atomic u64   reader cursor
              ├─ messages         atomic u64
              ├─ laps             atomic u64
              ├─ lost_bytes       atomic u64
              └─ heartbeat_ns     atomic u64
page-aligned  Ring               capacity bytes
```

The writer **never reads** the reader slots — it stays independent of readers. Only
monitoring tools and the supervisor read them.

`static_assert(std::atomic<std::uint64_t>::is_always_lock_free)` — atomics in shared
memory must be lock-free (and therefore address-free) to work across processes.

### Record format

```
 0        4        8                      8 + length, padded to 8
 ┌────────┬────────┬────────────────────┬─────┐
 │ length │  type  │      payload       │ pad │
 │  u32   │  u32   │   length bytes     │     │
 └────────┴────────┴────────────────────┴─────┘
```

- `length` = payload length; the record occupies `align8(8 + length)` bytes.
- `type` = message type id; `0` is reserved for **padding** records.
- Records never wrap: if a record does not fit before the end of the ring, the writer
  fills the remainder with a padding record and starts the record at offset 0.

Ordering/continuity is carried by the byte position. Message-level sequence numbers
(per feed, per instrument) belong in the message schemas, not the bus.

## 5. Writer algorithm

```
publish(type, payload):
    assert payload.size() <= max_msg_length
    tail       = tail.load(relaxed)                  // only the writer modifies tail
    offset     = tail & mask
    rec_len    = align8(8 + payload.size())
    to_end     = capacity - offset

    if to_end < rec_len:                             // wrap with padding
        tail_intent.store(tail + to_end + rec_len, release)
        write_header(offset, length = to_end - 8, type = PADDING)
        tail  += to_end
        offset = 0
    else:
        tail_intent.store(tail + rec_len, release)

    atomic_thread_fence(release)                     // intent visible before data
    write_header(offset, payload.size(), type)
    write_payload(offset + 8, payload)
    latest.store(tail, release)
    tail.store(tail + rec_len, release)              // publish
```

## 6. Reader algorithm

```
poll(handler):
    tail = tail.load(acquire)
    if cursor >= tail: return Empty

    if lapped(cursor):                               // writer already overwriting us
        lost = latest - cursor; cursor = latest.load(acquire)
        report Lapped(lost)

    offset = cursor & mask
    hdr    = read_header(offset)
    if hdr.type == PADDING:
        cursor += align8(8 + hdr.length); offset = 0; hdr = read_header(0)

    // hdr may itself be torn: never trust length before validation
    len = min(hdr.length, max_msg_length)
    copy len payload bytes into the reader's private scratch buffer
    atomic_thread_fence(acquire)
    if lapped(cursor):                               // copy may be torn — discard it
        report Lapped; resync to latest; return

    cursor += align8(8 + hdr.length)                 // hdr now known to be intact
    handler(hdr.type, scratch[0 .. hdr.length])      // safe: private copy
    return Message

lapped(c) := tail_intent.load(acquire) > c + capacity
```

The handler always receives a **private copy**, because in a lossy broadcast the ring
bytes can be overwritten at any moment. (A validated zero-copy "peek" can be added later
if benchmarks justify it.)

## 7. Memory model and torn reads

Readers copy bytes that the writer may be overwriting concurrently. With plain
`memcpy` this is a **data race — undefined behaviour** in C++, and TSan reports it,
even though the validation step discards torn data. This is the classic seqlock problem
(see H. Boehm, *Can Seqlocks Get Along With Programming Language Memory Models?*, and
the `atomic_memcpy` proposal P1478).

**Approach:** records are 8-byte aligned, so header and payload are copied as 64-bit
words with `std::atomic_ref<std::uint64_t>` **relaxed** loads (reader) and stores
(writer). Relaxed word-sized atomic accesses compile to ordinary loads/stores on
ARM64 and x86-64 — no performance cost — but are race-free by the standard. Ordering
comes from the release/acquire operations and fences on the counters.

## 8. Lifecycle and crash behaviour

| Event | Behaviour |
|---|---|
| Writer first start | `shm_open(O_CREAT \| O_EXCL)`, size, `mmap`, **write-touch every page** (pre-fault), initialize header, `writer_pid` = self, `writer_epoch = 1`; **`magic` stored last** (release) so nobody trusts a half-initialized header |
| Writer restart (segment exists, layout matches) | Attach, claim ownership, `writer_epoch++`, continue from `tail`. A record half-written at the crash was never published (`tail` not advanced), so it is simply overwritten |
| Second writer while one is alive | **Refused** (`WriterAlreadyActive`): ownership is claimed with a CAS on `writer_pid`, and a non-zero pid that is still alive (`kill(pid, 0)`) blocks the claim. Prevents e.g. `mda-replayer` and `mda-feedd` corrupting the same bus |
| Writer crashed (pid dead) | Next writer's CAS takes over, `writer_epoch++`. (PID reuse could in theory make a dead writer look alive — acceptable; the supervisor and heartbeat catch it) |
| Clean writer shutdown | Releases ownership (`writer_pid` = 0); segment and data remain |
| Segment exists, `magic` = 0 | `NotReady` — creator still initializing, or crashed mid-way; operator removes it |
| Existing capacity ≠ configured | `CapacityMismatch` — refuse rather than silently use a different size |
| Layout/version mismatch | `LayoutMismatch` — refuse to attach; operator removes the segment |
| Reader start | Attach and start at the current `tail`: only messages published after attaching are delivered. History comes from the journal, not the ring — "oldest still available" (`tail - capacity`) is not supported because that position can fall inside a record and records cannot be walked backwards |
| Writer dead | Readers see `heartbeat_ns` stale beyond threshold → report "writer down" (supervisor restarts it) |
| Reader crash | No effect on the writer or other readers. Its reader slot stays `Active` until reclaimed (§8a) |
| Machine reboot | Segment is gone (RAM-backed); journal is the durable record |

Segment names: `/mda.<bus>` — kept short because macOS limits POSIX shm names to 31 characters.

## 8a. Monitoring: stats and reader slots

Same approach as Aeron's counters file read by `AeronStat`: processes write plain
counters into shared memory; tools map it **read-only** and print them. Monitoring
never adds work to the data path beyond a few relaxed stores to the process's own
cache line, and a tool can never block or slow a process.

| Item | Decision |
|---|---|
| Writer stats | `WriterStats` in the segment (messages, bytes, rejected), updated on every publish |
| Reader slots | **16** slots in the segment, one cache line each |
| Claiming a slot | On attach, a reader CASes a slot's `pid` from 0 to its own pid (ownership *is* the pid), fills in name and counters, then sets `active` = 1 (release) |
| Updating | Reader updates position/messages/laps/lost/heartbeat in its own slot (relaxed stores) |
| Opt-out | `register_slot = false` attaches without a slot — for debug tools; invisible to monitoring |
| Slots exhausted | Reader still attaches, unregistered; the attach result says so. Monitoring never blocks data |
| Clean detach | Reader sets `active` = 0, then CASes `pid` back to 0 |
| Crashed reader | Slot keeps the dead pid; tools show it as `DEAD`. When no slot is free, a new reader reclaims a dead one by CASing `pid` from the dead pid to its own |
| Writer | **Never reads reader slots** |

Derived metric: **lag = writer `tail` − reader `position`**, also as a percentage of
capacity — the distance to being lapped.

### Tool: `mda-busctl`

```
$ mda-busctl stat md
bus md   capacity 256 MB   epoch 3   writer pid 4412   heartbeat 0.2 s ago   41,230 msg/s
reader      pid    lag        lag %   laps   lost
recorder    4418   12.4 KB    0.00%   0      0
engine      4420   1.1 MB     0.43%   0      0
```

Also `mda-busctl rm <bus>`, `mda-busctl list` (Linux only — macOS cannot enumerate POSIX
shared memory) and `--watch <seconds>`. Lag is shown as `LAPPED` once it reaches the ring
size, and as `-` for dead readers (their position is frozen). Rates come from sampling
counters twice.

Later, in the `runtime/` unit, a generic per-process counters segment (all metrics,
not just the bus) and a `mda-stat` tool follow the same pattern; an exporter can later
publish the same counters to Prometheus without touching any process.

## 9. Waiting for data

Pluggable wait strategy (compile-time policy, no virtual call on the hot path):

| Strategy | Use |
|---|---|
| `BusySpin` | Lowest latency; dedicated, pinned core (Linux) |
| `Backoff` (spin → `yield` → sleep 1 µs…1 ms) | Default on the laptop |
| `Sleep` (fixed interval) | Tests, low-priority tools |

Kernel notification (futex/eventfd) is deferred: not portable to macOS and not needed
at our rates.

## 10. Public API

**Ring layer (Unit 1, implemented)** — works on any memory region:
[`ring.hpp`](../../engine/libs/bus/include/mda/bus/ring.hpp),
[`ring_writer.hpp`](../../engine/libs/bus/include/mda/bus/ring_writer.hpp),
[`ring_reader.hpp`](../../engine/libs/bus/include/mda/bus/ring_reader.hpp).

```cpp
namespace mda::bus {

struct RingCounters;  // tail_intent, tail, latest — each on its own 128-byte line

class RingWriter {
public:
    static std::expected<RingWriter, RingError> create(RingCounters&, std::span<std::byte> ring) noexcept;
    // Never blocks. Fails only for payload > max_message_length() or type 0.
    std::expected<void, PublishError> publish(MsgType, std::span<const std::byte> payload) noexcept;
};

class RingReader {
public:
    // Starts at the current tail.
    static std::expected<RingReader, RingError> create(RingCounters&, std::span<std::byte> ring);
    // Delivers at most one message to handler(type, payload); payload is a private copy.
    template <MessageHandler H>
    PollResult poll(H&& handler);   // Message | Empty | Lapped{lost_bytes}
};

}  // namespace mda::bus
```

**Shared-memory layer** — [`bus.hpp`](../../engine/libs/bus/include/mda/bus/bus.hpp)
(Unit 2a, implemented): `Writer::create_or_attach(BusConfig{name, capacity})`,
`Writer::publish/heartbeat/epoch`, `Reader::attach(name)`, `Reader::poll/writer_status`,
`remove_bus(name)`; errors as `BusError{BusErrc, errno}`. Unit 2b adds
`ReaderOptions{name, register_slot}`, writer stats, reader slots and `stats()`.

Keeping the ring layer independent of shared memory lets unit tests run on a heap buffer
and lets benchmarks compare in-process vs cross-process fairly.

## 11. Testing and measurement

| Test | Checks |
|---|---|
| Unit | Empty ring; single record; exact-fit at ring end; padding/wrap; max-length message; oversized message rejected |
| Lapping | Reader paused while writer laps it → `Lapped` reported with correct loss; reader resumes at latest |
| Torn-read | Writer and reader threads hammering a tiny ring; every delivered message has a valid checksum (never torn) |
| Multi-process stress | 1 writer, 3 reader processes, millions of messages; sequence and checksum verified (TSan cannot see across processes, so correctness is checked end-to-end; the same ring code is TSan-checked between threads) |
| Monitoring | Slot claim/release; slots exhausted → unregistered attach; stale slot reclaimed after reader crash; stats match delivered counts |
| Crash | Kill writer (SIGKILL) mid-stream; replacement writer takes over (epoch 2); readers continue with no torn or out-of-order messages. Kill a reader; others unaffected; its slot shows `DEAD` |
| Benchmark | Latency p50/p99/p99.9/max and throughput vs message size; compare **in-process ring vs shared memory vs Unix domain socket**; per wait strategy |

## 12. Implementation units

| Unit | Scope |
|---|---|
| 1 ✅ | `RingWriter`/`RingReader` on plain memory + unit tests (wrap, padding, lapping, concurrent torn-read stress) |
| 2a ✅ | Shared-memory segment (RAII `shm_open`/`mmap`, pre-faulting), header, attach/validate, writer ownership, `Writer`/`Reader` |
| 2b ✅ | Writer stats, reader slots, `inspect_bus`, `mda-busctl` (stat / rm / list) |
| 3 ✅ | Multi-process tests ([`multiprocess_test.cpp`](../../engine/libs/bus/tests/multiprocess_test.cpp)): 1 writer + 3 reader processes, 1M messages verified end-to-end, a stalled reader forced to lap; writer killed with SIGKILL mid-publish and replaced; reader killed with SIGKILL. A standalone long-running soak tool is deferred to the Linux machine |
| 4 ✅ | Wait strategies ([`wait_strategy.hpp`](../../engine/libs/bus/include/mda/bus/wait_strategy.hpp)) + benchmark ([`bus_bench.cpp`](../../engine/bench/bus_bench.cpp)); results in [learning notes](../learning/market-data-bus.md) |

## 13. Open questions

- Ring size default: 64 MB vs 256 MB for `md` (minutes of slack at live rates vs RAM).
- ~~Should readers publish their positions into the segment for lag monitoring?~~ —
  decided: yes, 16 reader slots with opt-out (§8a).
- Huge pages (`MAP_HUGETLB` on Linux) — benchmark when on Linux hardware.
