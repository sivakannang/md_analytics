# Domain model

> Status: **Accepted** · Decisions: [ADR-0008](../adr/0008-instrument-identity.md),
> [ADR-0009](../adr/0009-fixed-point-prices-and-nanosecond-time.md),
> [ADR-0010](../adr/0010-fixed-layout-snapshot-events.md)

The vocabulary shared by every component: how instruments are identified, how prices,
quantities and times are represented, and what events travel on the bus. Broker-specific
details stay inside `kite/`; everything else sees only these types.

## 1. Instrument identity

| Layer | Example | Lifetime | Used by |
|---|---|---|---|
| **ContractKey** | `NSE·FO·NIFTY·OPT·2026-10-27·25000·CE` | Permanent meaning of a contract | Instrument master, history, research |
| **InstrumentId** | `uint32` `48213` | Assigned once per ContractKey, **never reused** | Everything inside the system |
| **Broker mapping** | Kite token `13874690` → `48213` | One trading day | `kite/` only |

Why: exchange F&O tokens are **reused after expiry** (confirmed from NSE experience) and
broker tokens are broker-specific. Neither can identify a contract across days or brokers.

### ContractKey fields

| Field | Values |
|---|---|
| exchange | NSE, BSE (later MCX, …) |
| segment | Cash (equity/index), FO |
| underlying | `NIFTY`, `BANKNIFTY`, `RELIANCE`, … |
| kind | Index, Equity, Future, Option |
| expiry | date (derivatives only) |
| strike | Price (options only) |
| option type | CE / PE (options only) |

Contract attributes (not part of identity, may change): lot size, tick size, trading
symbol, underlying's InstrumentId.

### InstrumentId properties

- Dense, starting at 1 (0 = invalid). Per-instrument engine state lives in **arrays indexed
  by InstrumentId** — no hash lookups on the hot path.
- Registry persisted in the app DB: `instrument(id, contract_key, first_seen, last_seen, …)`.
- Expired contracts keep their IDs forever, so history stays joinable.

### Daily instrument master

1. Download Kite's instrument list (CSV, ~100k rows) after login.
2. For each row: build the ContractKey → look up or assign InstrumentId.
3. Rebuild today's Kite-token ↔ InstrumentId mapping.
4. Update attributes (lot size can change on NSE revisions); link derivatives to their underlying.
5. Derive the expiry calendar and strike ladders per underlying.

## 2. Prices, quantities, time

| Type | Representation | Notes |
|---|---|---|
| `Price` | `int64`, scale **1/10,000** (₹123.45 → `1234500`) | Strong type; exact arithmetic; covers 0.05/0.01 equity and F&O ticks and 0.0025 currency ticks |
| `Quantity` | `int64`, in **shares/units** | Volume, OI, depth quantities. Lots = quantity / lot size of that day (display only) |
| `Timestamp` | `int64` ns since Unix epoch, **UTC** | IST only for display and trading-day logic |
| Analytics values | `double` | IV, Greeks, indicators — not prices on the data path |

Kite → our scale: Kite sends NSE/BSE prices as integer paise (÷100) → multiply by 100.
(Currency derivatives use ÷10⁷; handled in `kite/` when we add them.)

### Two clocks per event

| Field | Source | Resolution |
|---|---|---|
| `exchange_ts` | Kite's exchange timestamp | 1 s |
| `last_trade_ts` | Kite's last trade time | 1 s |
| `receive_ts` | Our wall clock when `mda-feedd` read the frame | ns (tick ~1 s apart) |

`receive_ts` is what we *knew when*: it drives bar assignment, OI-update detection and
replay timing. `exchange_ts` is kept for analysis and sanity checks (e.g. clock drift).

## 3. Events on the bus

Kite sends **snapshots** (current state of an instrument), not deltas, so normalized
events are snapshots too. A lost or skipped event never corrupts state — the next one
is complete.

| MsgType | Event | Producer → consumers |
|---|---|---|
| 1 | `RawFrame` — one WebSocket frame exactly as received, plus `receive_ts`, connection id, sequence | feedd → recorder |
| 2 | `MarketSnapshot` — prices, volumes, OI, timestamps | feedd → engine |
| 3 | `DepthSnapshot` — best 5 bid/ask levels | feedd → engine |
| 4 | `FeedStatus` — connected / disconnected / stale / subscription changed | feedd → all |

Depth is a separate message because many consumers don't need it and it is the largest
part; feedd publishes it only for instruments subscribed in full mode.

### Common event header (16 bytes)

| Field | Type | Notes |
|---|---|---|
| `schema_version` | u16 | Per message type; readers reject unknown versions |
| `flags` | u16 | Message-specific |
| `instrument_id` | u32 | 0 for non-instrument events |
| `sequence` | u64 | Per-feed sequence from feedd; gaps are visible in journal/replay |

### `MarketSnapshot` (152 bytes)

| Field | Type | From Kite |
|---|---|---|
| header | 16 B | |
| `fields` | u32 | bitmask of present groups: LTP, OHLC, volume, OI, exchange time, last trade time |
| `mode` | u8 + 3 pad | ltp / quote / full |
| `receive_ts`, `exchange_ts`, `last_trade_ts` | Timestamp ×3 | |
| `ltp`, `avg_price`, `open`, `high`, `low`, `close` | Price ×6 | `close` = previous day close |
| `last_qty`, `volume` | Quantity ×2 | `volume` is cumulative for the day |
| `total_buy_qty`, `total_sell_qty` | Quantity ×2 | pending order totals |
| `oi`, `oi_day_high`, `oi_day_low` | Quantity ×3 | full mode, F&O only |

### `DepthSnapshot` (184 bytes)

Header + `receive_ts` + 5 bid and 5 ask levels × {`price` Price, `quantity` u32,
`orders` u32} (16 B each).

### Encoding rules

- Plain structs: trivially copyable, standard layout, **explicit padding**, no pointers or
  `std::string`; `static_assert` on `sizeof` and every field offset.
- Native little-endian (both target CPUs); the bus never leaves the machine.
- Any layout change → bump that message's `schema_version`.

## 4. Sizes and rates (sanity check)

| Item | Estimate |
|---|---|
| Live tick rate | ~2–5k instruments/s update (illiquid strikes tick rarely) |
| Bus bytes/s | ~5k × (152 + 184 + raw ~190) ≈ 2.6 MB/s → 256 MB ring ≈ 100 s of slack |
| Raw frames per day | ~5k × 190 B × 22,500 s ≈ 21 GB before compression |

Implication for the journal (next design): **store raw frames only** (plus receive time and
sequence) and re-decode on replay — normalized events can always be regenerated from raw,
so storing both doubles disk use for no information. Compression (e.g. zstd) to be measured.

## 5. Module placement

| Module | Contents |
|---|---|
| `core/` | `Price`, `Quantity`, `Timestamp`, `InstrumentId` strong types |
| `messages/` | `MarketSnapshot`, `DepthSnapshot`, `FeedStatus`, `RawFrame` layouts + MsgType ids |
| `instruments/` | `ContractKey`, registry, daily master import, expiry calendar |
| `kite/` | Kite-token mapping, binary decoder → `messages/` types |

## 6. Open questions

- **OI units:** we assume Kite sends OI in shares. Verify against NSE's website for one
  contract using the OI probe data.
- **Circuit limits, market status, trading halts:** not in Kite's tick feed. Add from the
  REST quote API (periodic) as an `InstrumentStatus` event later.
- **Index instruments** have no volume/OI/depth — `fields` mask marks them.
- Kite delivers ~1 snapshot/s per instrument; fields like `last_qty` describe only the latest
  trade, not every trade in between. Accepted (see recovery design, known limitation).
