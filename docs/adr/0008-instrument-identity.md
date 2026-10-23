# ADR-0008: Instrument identity — ContractKey, permanent InstrumentId, daily broker mapping

- **Status:** Accepted
- **Date:** 2026-10-21
- **Deciders:** Sivakannan, Claude

## Context

Every event, bar and chart refers to an instrument. Kite identifies instruments with its
`instrument_token`, derived from the exchange token. Two facts make that unusable as our
identity:

- **NSE reuses F&O contract tokens after expiry** (confirmed from NSE experience), so a
  token does not identify a contract across days — history keyed by token would mix
  different contracts.
- Broker tokens are **broker-specific**; adding a second broker must not change identities.

The engine also needs per-instrument state lookups on every tick (thousands per second,
millions during replay), so the internal identifier should be cheap to index by.

## Options considered

### Option A — Use Kite's instrument_token everywhere
- Pros: zero mapping work.
- Cons: breaks history on token reuse; ties the whole system to Kite.

### Option B — Use a string/struct ContractKey everywhere
- Pros: permanent and readable.
- Cons: large keys on the hot path; hashing on every lookup.

### Option C — Three layers: ContractKey (meaning) → InstrumentId (dense `uint32`) → per-day broker mapping
- Pros: permanent, broker-independent, and `InstrumentId` indexes arrays directly.
- Cons: a registry to maintain and persist; a daily mapping rebuild.

## Decision

Option C (details in [domain model §1](../design/domain-model.md#1-instrument-identity)):

- **ContractKey** = exchange, segment, underlying, kind, expiry, strike, option type.
- **InstrumentId** = dense `uint32`, assigned once per ContractKey, persisted in the app
  DB, **never reused**; 0 is invalid.
- **Broker mapping** = Kite token ↔ InstrumentId, rebuilt each trading day from the
  instrument list; visible only inside `kite/`.

## Rationale

Correct history under token reuse and broker independence are non-negotiable; the dense
id keeps the hot path as cheap as using the broker token directly.

## Consequences

- Positive: history and research join cleanly across years; a second broker only adds a mapping.
- Negative: the instrument registry becomes critical state — losing it would orphan
  history. It is small; back it up with the bar store.
- The daily import must run before the feed starts.

## Learning notes

- Surrogate keys vs natural keys; why exchanges themselves use dense tokens.
- Interview: "How do you key per-instrument state for millions of updates per second?"
