# ADR-0009: Fixed-point prices and nanosecond UTC timestamps

- **Status:** Accepted
- **Date:** 2026-10-21
- **Deciders:** Sivakannan, Claude

## Context

Prices are compared (did price go up?), bucketed (bar high/low), checked against tick
sizes and, later, used in order logic. Time decides which bar an event belongs to, when
OI updated, and how fast replay runs. Kite sends prices as integer paise and timestamps
with 1-second resolution; our own receive time is available in nanoseconds.

## Options considered

### Prices
- **`double`** — convenient, but `0.1 + 0.2 != 0.3`; equality and tick checks become
  approximate; error accumulates in sums.
- **Decimal library** — exact, slower, an extra dependency.
- **Fixed-point `int64`** — exact, fast, trivially copyable; needs a chosen scale.

### Scale
- **Paise (1/100)** — matches Kite for NSE/BSE today; can't represent currency
  derivative ticks (0.0025).
- **1/10,000** — covers equity, F&O and currency ticks with ±9.2×10¹⁴ rupees of range.

### Timestamps
- `std::chrono::system_clock::time_point` in structs (implementation-defined
  representation) vs **plain `int64` nanoseconds since the Unix epoch, UTC**.

## Decision

- `Price`: strong type over `int64`, scale **1/10,000**.
- `Quantity`: strong type over `int64`, in shares/units (lots only for display).
- `Timestamp`: strong type over `int64` nanoseconds since the Unix epoch, **UTC**.
- `double` only for derived analytics (IV, Greeks, indicators).
- Events carry `receive_ts` (ns, ours) plus `exchange_ts` and `last_trade_ts` (1 s, Kite's).

## Rationale

Exact integer prices remove a whole class of comparison bugs and are as fast as it gets.
The 1/10,000 scale avoids a format change when currency derivatives arrive. A plain
`int64` timestamp has a fixed, documented layout — essential for shared memory and files.
Strong types prevent mixing prices, quantities and times by accident at zero runtime cost.

## Consequences

- Positive: exact, fast, layout-stable; type errors caught at compile time.
- Negative: conversion at the edges (Kite paise ×100; display ÷10,000); multiplication
  (price × quantity) needs care about overflow and scale.
- `receive_ts` becomes the basis for bars and replay; `exchange_ts` is advisory.

## Learning notes

- Why trading systems avoid floating point for prices; fixed-point overflow analysis.
- Strong typedefs in C++ (zero-cost wrappers, deleted mixed operators).
- Interview: "How would you represent prices in a matching engine and why?"
