# Kite adapter: binary tick decoder

> Status: **Draft** · Source: Kite Connect v3 WebSocket documentation
> (<https://kite.trade/docs/connect/v3/websocket/>) and the official `pykiteconnect`
> implementation. Related: [domain model](domain-model.md).

The only module that knows Kite's wire format. It turns Kite binary WebSocket frames into
`MarketSnapshot` / `DepthSnapshot` events. Kite instrument tokens are translated to our
`InstrumentId` here and never leave the module.

## 1. Frame structure

All integers **big-endian**.

| Frame | Meaning |
|---|---|
| 1 byte | Heartbeat (keep-alive); no packets |
| Text frame | JSON (errors, order updates) — not market data; handled by the connection layer |
| Binary frame | `u16` packet count, then for each packet: `u16` length + packet bytes |

## 2. Packet layouts

Segment = `instrument_token & 0xFF`. Segment 9 is **indices** (non-tradable), which use a
different layout.

### Tradable instruments

| Bytes | Field | Mode |
|---|---|---|
| 0–4 | instrument_token | all |
| 4–8 | last traded price | all (**LTP mode = 8 bytes**) |
| 8–12 | last traded quantity | quote, full |
| 12–16 | average traded price | quote, full |
| 16–20 | volume traded today | quote, full |
| 20–24 | total buy quantity | quote, full |
| 24–28 | total sell quantity | quote, full |
| 28–32 | open | quote, full |
| 32–36 | high | quote, full |
| 36–40 | low | quote, full |
| 40–44 | close (previous day) | quote, full (**quote mode = 44 bytes**) |
| 44–48 | last trade time (Unix s) | full |
| 48–52 | open interest | full |
| 52–56 | OI day high | full |
| 56–60 | OI day low | full |
| 60–64 | exchange timestamp (Unix s) | full |
| 64–184 | depth: 10 × 12 bytes, 5 bids then 5 asks | full (**full mode = 184 bytes**) |

Depth entry (12 bytes): `u32` quantity, `u32` price, `u16` orders, 2 bytes padding.

### Indices (segment 9)

| Bytes | Field | Mode |
|---|---|---|
| 0–4 | instrument_token | all |
| 4–8 | last price | all (8 bytes) |
| 8–12 | high | quote, full |
| 12–16 | low | quote, full |
| 16–20 | open | quote, full |
| 20–24 | close | quote, full |
| 24–28 | price change (signed) | quote, full (**28 bytes**) |
| 28–32 | exchange timestamp | full (**32 bytes**) |

Note the index OHLC order (high, low, open, close) differs from tradable packets.

## 3. Price scaling

| Segment | Kite divisor | To our 1/10,000 scale |
|---|---|---|
| CDS (3) | 10,000,000 | value ÷ 1,000 |
| BCD (6) | 10,000 | value as is |
| everything else | 100 (paise) | value × 100 |

## 4. Mapping to events

| Kite packet | `MarketSnapshot.fields` | Depth |
|---|---|---|
| LTP (8) | `Ltp` | — |
| Quote (44) | `Ltp, Ohlc, Volume` | — |
| Full (184) | `Ltp, Ohlc, Volume, ExchangeTime` + `OpenInterest` for derivative segments | `DepthSnapshot` |
| Index quote (28) | `Ltp, Ohlc` | — |
| Index full (32) | `Ltp, Ohlc, ExchangeTime` | — |

`header.sequence` is left 0 by the decoder; `mda-feedd` assigns the feed sequence when
publishing. Packets for tokens not in today's token map are skipped and counted.

## 5. Robustness

The decoder never trusts lengths: every packet and field read is bounds-checked against
the frame. A malformed frame stops decoding at the bad packet (earlier packets are kept)
and is counted. A randomized-input test feeds millions of random frames through the
decoder under ASan/UBSan; proper coverage-guided fuzzing (libFuzzer) is planned once we
build with Homebrew LLVM on Linux.

## 6. Open items

- Golden tests from **real recorded frames** once the recorder runs (handcrafted packets
  follow the documentation; real data confirms it).
- Confirm OI units against NSE (see domain model open questions).
