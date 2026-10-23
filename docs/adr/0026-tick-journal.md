# ADR-0026: Tick journal (recorder and replayer)

- **Status:** Accepted
- **Date:** 2026-10-23
- **Deciders:** owner, Claude

## Context

Kite's WebSocket never resends missed ticks, so a session not recorded is gone. We want
sessions on disk for after-hours and weekend development, regression tests (replay an
hour, compare bars and analytics), research (OI update frequency, the closing auction for
ADR-0023) and recovery. mda-feedd already publishes every WebSocket frame as received
(`RawFrame`, type 1, with receive time and sequence); nothing reads it yet. The domain
model (§4) settled on storing raw frames only and decoding them again on replay.

Measured on 2026-10-22 (644 instruments, full mode): 117 ticks/s on average, 607 at peak,
~4 frames/s: about 21 KB/s, ~0.5 GB per session uncompressed.

## Decision

**`mda-recorder`** reads the `md` bus (it never slows the writer: the bus doesn't wait for
readers) and appends `RawFrame` and `FeedStatus` messages to a journal:

- **Layout:** `data/journal/<IST date>/<HH>.mdj`, one file per IST hour, plus
  `instruments.csv`, a copy of that day's Kite instrument list (feedd keeps only today's),
  which replay needs to map Kite tokens to our instrument ids.
- **File:** a 16-byte header (`MDAJRNL1`, format version, reserved), then records:
  `u32 length · u32 bus type · payload` (the bus payload unchanged, so the raw frame's
  own header keeps receive time, sequence and connection). A journal-only `Gap` record
  (type `0x7FFF0001`: time, lost bytes) marks where the recorder was lapped.
- **Durability:** buffered writes flushed every second; a file is fsynced when closed. A
  torn last record after a crash is ignored by readers and cut off when the writer
  reopens the file.
- **No index or compression in v1:** an hour file is ~70 MB and scans in well under a
  second. Compressing finished files (zstd) is a later step; readers will accept both.
- **Retention:** 90 days; older day folders are deleted by the recorder.
- **Switch:** `recorder.enabled` in `config/mda.yaml`; with `false` the recorder exits at
  once, so the daily start steps don't change.

**`mda-replayer`** (next unit) reads a day's journal, decodes frames with the same
`kite::TickDecoder` and publishes `MarketSnapshot`/`DepthSnapshot`/`FeedStatus` at real
time, ×N or full speed, from/to a time. It runs on **separate buses** (`md-replay`,
`an-replay`) with its own config (scratch `bars.db`, gateway on another port), so a replay
never touches live data and can run while the live stack does.

## Rationale

Raw frames are the smallest complete record: decoded events can always be regenerated,
and a decoder fix applies to old recordings. Hour files keep any one file small, make
"replay 10:00–11:00" a file choice, and bound the damage of a corrupt file. A dedicated
process keeps disk stalls away from the feed. Separate replay buses make replay safe by
construction rather than by care.

## First full replay (2026-10-23)

Today's session (09:00–15:45, 105,155 frames, 13.1 M ticks) replayed at 120× in 202 s with
no laps. Compared with the live `bars.db`, **all 271,571 bars (09:15–15:30) are identical**
(OHLC, volume, OI open/high/low/close, OI updates) after two fixes the replay exposed:

- **mda-engine closed bars by the wall clock** (`close_due(unix_now())`): replaying 09:16 at
  17:40 closed every bar mid-minute and the rest of the minute overwrote it (25 % of bars lost
  their first ticks). The engine now keeps time by the data (`bars::EventClock`: the newest
  tick's receive time plus wall time since); live it equals the wall clock. Option analytics
  use the same clock, so replayed time-to-expiry is right too.
- **The web chart started its live candle from the wall clock** and dropped replayed bars as
  "older than now". It now starts from the newest bar it loaded (or the first live bar).

Lesson: anything that asks the wall clock for "now" breaks replay; event-driven code should
take time from its events.
