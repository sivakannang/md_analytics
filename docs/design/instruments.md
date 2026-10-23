# Instrument registry and daily import

> Status: **Implemented (v1)** · Decision: [ADR-0008](../adr/0008-instrument-identity.md)
> · Code: [`engine/libs/instruments`](../../engine/libs/instruments),
> [`kite/instrument_import`](../../engine/libs/kite/src/instrument_import.cpp)

## 1. Pieces

| Piece | Module | Role |
|---|---|---|
| `ContractKey` | `instruments` | Permanent identity; canonical text `NSE\|FO\|OPT\|NIFTY\|2026-10-27\|25000.00\|CE` |
| `InstrumentRegistry` | `instruments` | ContractKey ↔ InstrumentId; dense ids from 1, never reused; daily attributes |
| Registry file | `instruments` | Durable id ↔ key pairs (append-only) |
| `import_instruments` | `kite` | Today's Kite CSV → registry + `TokenMap` |

## 2. Daily import (`import_instruments`)

1. `registry.begin_day()` clears yesterday's attributes (`listed_today = false`).
2. Columns are located **by header name**, so Kite adding or reordering columns is harmless.
3. Each row becomes a ContractKey:

| Kite `segment` | Kind | `underlying` from |
|---|---|---|
| `INDICES` (exchange NSE/BSE) | Index | `tradingsymbol` (`NIFTY 50`) |
| `NSE` / `BSE`, type `EQ` | Equity | `tradingsymbol` (`RELIANCE`) |
| `NFO-FUT` / `BFO-FUT` | Future | `name` (`NIFTY`) + expiry |
| `NFO-OPT` / `BFO-OPT` | Option | `name` + expiry + strike + CE/PE |
| anything else (CDS, MCX, …) | skipped, counted | — |

4. `assign(key)` returns the existing id or a new one; attributes (symbol, lot size,
   tick size) are refreshed; `token → id` goes into today's `TokenMap`.
5. Derivatives are linked to their underlying: index first (F&O names that differ from
   the index symbol use an alias table — `NIFTY → NIFTY 50`, `BANKNIFTY → NIFTY BANK`,
   `FINNIFTY`, `MIDCPNIFTY`, `NIFTYNXT50`, `NIFTYFPI → NIFTY FPI 150`, `SENSEX`,
   `SENSEX50 → SNSX50`, `BANKEX`, `FOCIT → BSE FOCUSED IT`; verified on the real list), else the equity of the
   same name. Unresolved links are counted (`unlinked`).
6. The caller persists new ids with `registry.save_new(path)` **before** the feed starts.

Strikes, tick sizes and dates are parsed exactly (`parse_price`, ISO dates) — no
floating point anywhere in the import.

## 3. Token reuse (why this exists)

| Day | Token `13369090` means | InstrumentId |
|---|---|---|
| Day 1 | NIFTY 27-Oct-2026 25000 CE | 5 |
| Day 2 (after expiry) | NIFTY 03-Nov-2026 25000 CE | 10 (new) |

Day 2's `TokenMap` maps the token to 10; contract 5 keeps id 5 forever (history stays
correct) with `listed_today = false`. Covered by
`ReusedTokenMapsToTheNewContractWithoutDisturbingHistory`.

## 4. Registry file (v1)

```
# md_analytics instrument registry v1: id,contract_key
1,NSE|CASH|IDX|NIFTY 50
2,NSE|FO|OPT|NIFTY|2026-10-27|25000.00|CE
```

- **Append-only**, written with `write` + `fsync` — ids handed out survive a crash or
  power loss.
- Loading checks ids are consecutive and keys unique; a corrupt complete line is an
  error (operator must look). A final line **without a newline** — an interrupted append
  — is ignored and reported.
- Only id ↔ key is stored; daily attributes come from each day's import.

**Interim choice:** a plain text file until the app DB (SQLite) is decided in the
storage ADR. Migration is a one-off load-and-insert. Human-readable and diffable meanwhile.

Size: NSE+BSE F&O lists ~100k contracts; a year of expiries adds a few million lines
(~60 bytes each, ~200 MB/year) — fine for a text file; a reason to move to SQLite later.

## 5. Open items

- Download the CSV from Kite (`GET /instruments`) — part of the Kite REST client unit.
- Alias table is hard-coded; move to config if Kite adds index derivatives.
- Expiry calendar / strike ladder queries for the option chain — with the engine.
