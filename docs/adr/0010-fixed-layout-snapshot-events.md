# ADR-0010: Normalized events are fixed-layout snapshot structs

- **Status:** Accepted
- **Date:** 2026-10-21
- **Deciders:** Sivakannan, Claude

## Context

`mda-feedd` turns Kite's binary ticks into broker-agnostic events on the bus
([ADR-0007](0007-shared-memory-broadcast-bus.md)). We must choose the events' semantics
(snapshot vs delta) and encoding. Writer and readers run on the same machine, built from
the same source.

## Options considered

### Semantics
- **Deltas** (only changed fields) — smaller, but a lost event corrupts state until a
  resync; consumers must track sequence carefully.
- **Snapshots** (full current state per instrument) — Kite already sends snapshots;
  every event is self-contained, so a lapped reader is correct again at the next event.

### Encoding
- **Fixed-layout plain structs** — zero encode/decode cost (copy in, read fields),
  no dependency; only valid between identical builds on the same architecture.
- **SBE (Simple Binary Encoding)** — exchange-grade, schema-driven, cross-language; code
  generation toolchain.
- **FlatBuffers / Cap'n Proto** — zero-copy, schema evolution; larger, pointer-offset tables.
- **Protobuf / JSON** — far too slow for the hot path.

## Decision

- Snapshot semantics: `MarketSnapshot`, `DepthSnapshot` (separate, only for full-mode
  instruments), `FeedStatus`, and `RawFrame` for the recorder.
- Fixed-layout, trivially copyable, standard-layout structs with explicit padding and
  `static_assert`s on size and every field offset; native little-endian.
- A 16-byte common header with a per-type `schema_version`, `instrument_id` and feed
  `sequence`; any layout change bumps the version and readers reject unknown versions.

Field lists: [domain model §3](../design/domain-model.md#3-events-on-the-bus).

## Rationale

Snapshots match the source and make lossy broadcast safe. Fixed structs are the fastest
possible encoding and the simplest to reason about; cross-language or cross-machine needs
(where SBE shines) don't exist yet.

## Consequences

- Positive: no serialization cost or dependency; events are self-healing.
- Negative: snapshots are larger than deltas; structs can't be read by other languages
  directly (Python research reads the journal through a converter or the bar store).
- Revisit when events leave the machine (gateway to other hosts, Phase 3–4): consider SBE.

## Learning notes

- Snapshot vs incremental feeds — the same trade-off exchanges make (e.g. NSE/CME
  snapshot + incremental channels).
- Struct layout, padding, `offsetof`, trivially copyable types and implicit-lifetime
  rules for reading structs out of shared memory.
- Interview: "Design the message format between a feed handler and a strategy engine."
