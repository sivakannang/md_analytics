# ADR-0022: Option analytics in mda-engine: Black-76 on a parity-implied forward

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

The option chain shows IV and Greeks, OI change and summaries (PCR, max pain, ATM IV).
The same numbers will drive the OI charts, alerts and signals, so they shouldn't live in
the browser or be tied to one gateway request.

## Options considered

- **Model:**
  - **Black-Scholes on spot** needs a rate and a dividend yield, and is wrong for index
    options whose futures trade at a basis.
  - **Black-76 on the future** works only when a future exists for the expiry; weeklies
    have none.
  - **Black-76 on the put–call-parity forward (chosen):** F = K + C − P near ATM, from live
    prices. It has no rate or dividend assumption and works for every expiry. It falls
    back to the future, then to spot.
- **Where:** in the gateway, it's per request and would be recomputed by every consumer.
  **In mda-engine (chosen)** it's computed once a second per chain, as ADR-0006 and the
  architecture overview always planned.

## Decision

- Per underlying and expiry, once a second:
  - forward (parity, median of 3 strikes near ATM);
  - T = calendar time to 15:30 IST on expiry, ACT/365, floored at 1 hour.
- **IV** by Newton–Raphson (start 20%) with a bisection fallback. None when the price is
  below intrinsic value or the option hasn't traded today. Greeks in closed form: delta
  and gamma per point, vega per vol point, theta per calendar day.
- **OI change** against the previous day's closing OI, taken from bars.db (ADR-0021).
  **Buildup** per strike uses the same classes as the OI chart.
- **PCR, max pain** over the subscribed window, labelled as such. **ATM IV** is the call/put
  average at ATM, and is recorded daily for a future IV percentile.
- Published on a second bus `an` (analytics) as fixed-layout messages (`OptionAnalytics`,
  `ChainSummary`). The `md` bus keeps its single writer (mda-feedd). The gateway merges
  them into its quote cache.

## Consequences

- One implementation serves the chain, charts, alerts and signals.
- Pricing code gets its own unit tests against published Black-76 values and parity
  identities.
- Theta by calendar day differs slightly from trading-day conventions some platforms use.
  The UI will say "per calendar day".
