# ADR-0020: Dynamic option subscriptions: ATM windows with hysteresis in mda-feedd

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

The option chain and OI analytics need live data for a band of strikes around the money,
for several expiries, and that band must follow the market. Kite allows 3,000 instruments
per WebSocket and 3 connections, so "subscribe every listed option" (tens of thousands)
is impossible. Details: [option-chain-and-bars.md §2](../design/option-chain-and-bars.md).

## Options considered

- **Static strike lists in config.** Simple, but goes stale within hours on a trending day.
- **A separate subscription-manager process telling feedd what to subscribe.** Cleaner
  separation, but it needs a control channel and a new failure mode. Unnecessary while
  the spot price is visible inside feedd itself.
- **Inside mda-feedd, driven by the spot it decodes (chosen).** feedd already owns the
  Kite connection, the token map and the live subscription diff. The spot price is in the
  ticks it decodes.

## Decision

- Configured chains: underlying, spot instrument, number of expiries, ATM ± N strikes.
- The strike grid comes from today's instrument list (never an assumed step).
- **Hysteresis:**
  - the window is ATM ± N plus 5 buffer strikes per side;
  - it is re-centred when ATM drifts more than 5 strikes;
  - strikes that leave are unsubscribed only after a 10-minute grace period.
- The futures of each chain underlying are always subscribed.
- Subscriptions are the union of static + watchlists + chain windows, applied by the
  existing live diff (no reconnects). A warning at 80% of the per-connection budget.
- F&O stock chains (which need several connections) are a later decision.

## Consequences

- feedd becomes a little smarter, but remains the only process talking to Kite's stream.
- The chain covers ±N strikes, not the full chain. PCR and max pain are labelled "window".
