# ADR-0023: Session phases: pre-open and the closing auction

- **Status:** Accepted
- **Date:** 2026-10-22
- **Deciders:** Sivakannan, Claude

## Context

Since 2026-08-03 NSE and BSE close F&O stocks with a **closing auction session (CAS)**
instead of the 15:00–15:30 VWAP. The rules below come from NSE's product page and circular
CMTR73362, through secondary sources. The circular itself didn't load, so the sub-phase
times need checking against it.

**Scope and band**
- CAS covers only cash stocks with F&O contracts. Other stocks trade continuously until 15:30.
- Continuous trading for CAS stocks ends at 15:15.
- The **reference price** is the VWAP of 15:00–15:15. If there were no trades then, it's the
  last price; if none all day, the previous close.
- Orders must lie within **±3 % of the reference price**: the auction band.

**Sub-phases**

| Time | Phase |
|---|---|
| 15:15–15:20 | Reference price is calculated |
| 15:20–15:25 | Order entry |
| 15:25–15:30 | Limit orders only, closing at a random moment |
| 15:30–15:35 | Matching |

**Close and derivatives**
- The official close is the equilibrium price, falling back to the reference price.
- **Equity derivatives trade until 15:40.** Both exchanges confirmed this on 2026-10-22:
  NIFTY and SENSEX futures and SENSEX options have Kite candles up to 15:39.

**Pre-open**
Pre-open (09:00–09:15) is an auction too, ending in an equilibrium "indicative open".

### What we saw on 2026-10-22 (HDFCBANK, Kite and our own data)

**Kite web and the quote API during CAS**
- Kite web shows Ref. price 693.35, Indicative close 694.60–694.70, Total imbalance
  (13 → −20,829 → −19,043), and circuits 672.55 / 714.15. The normal circuits are
  636.30 / 777.60.
- The depth is crossed (bid 714.15 > offer 672.55). A `0.00` row holds market orders
  (382 orders, 2,78,556 shares).
- LTP and last trade time don't move: the last trade stays at 15:14:59.
- **Kite's quote API** has the same fields: `reference_limit_price`,
  `indicative_close_price` and `total_imbalance_qty`, and its
  `lower/upper_circuit_limit` switch to the auction band.
- **The WebSocket ticks don't carry the auction fields.**

**After the auction**
- At 15:34 the quote API showed a last trade at 15:28:54 at 694.05, the indicative close.
- Volume rose by 15,20,312 shares.
- **The circuits were still the auction band.**
- Kite's minute candles have nothing after 15:14 for HDFCBANK, so the auction print is in
  no Kite candle.

**Our bars**
- **F&O minutes 15:30–15:39 were missing.** The engine kept receiving about 32,000
  snapshots a minute and wrote no bars, because the session ended at 15:30.
- **CAS stocks got day-sized auction bars.** HDFCBANK 15:28 had volume 5,87,34,972 (the
  whole day). INFY, RELIANCE, NESTLEIND, VBL and KALYANKJIL are similar.
- The pattern fits Kite's tick volume dropping to the auction's matched quantity during
  CAS and returning to the day total after it. Our builder counted the return as volume.
  The engine had also restarted at 15:20, which put the baseline at the dropped value.

## Options considered

- **Data source for the auction fields:**
  - **WebSocket only** isn't possible: the ticks don't carry them.
  - **Kite's quote API (chosen):** poll during the phase windows only. It takes 500
    instruments a call, and Kite limits quotes to about 1 request a second.
- **Who polls:**
  - **The gateway** would need to hold a Kite session, which ADR-0006 keeps in mda-feedd.
  - **mda-feedd (chosen):** it already polls circuits from the same API and is the
    single writer of `md`.
- **Phase times:**
  - **Hard-coded** times are wrong on special sessions such as Muhurat trading.
  - **Config (chosen):** with the NSE defaults.
  - Exchange-sent phase codes would be best, but Kite doesn't send them.

## Decision

1. **Bars (done with this ADR):**
   - Derivatives' bar session runs 09:15–15:40 and cash runs 09:15–15:30.
   - The engine tells them apart from the permanent registry (`market == FO`).
   - Cumulative volume only counts upward: a drop isn't volume, and the baseline stays at
     the highest value.
   - After a restart the baseline is the larger of the first tick's volume and today's
     stored volume.
   - The auction print lands in its own minute (15:28 on 22 Oct). Kite has no candle for
     it, so live bars are its only source and backfill can't repair it.
2. **Phase clock** in `config/mda.yaml` (`market.phases`), in IST:
   - `pre_open` 09:00–09:15;
   - `closing_auction` 15:15–15:35, for CAS stocks;
   - `derivatives_end` 15:40.

   A stock is a CAS stock if listed today with F&O contracts; the catalog already links
   underlyings.
3. **mda-feedd polls the quote API during a phase:**
   - every 3 s;
   - for the subscribed NSE and BSE equities the phase applies to: all subscribed
     equities in pre-open, CAS stocks in the auction;
   - up to 500 keys a call, keeping within the rate limit together with the circuit
     refresh.

   It publishes an **`AuctionStatus`** message (type 9, fixed layout) on `md` with:
   - the phase;
   - reference price;
   - indicative price (open or close by phase);
   - signed imbalance quantity;
   - auction band;
   - quote time.

   When the phase ends it publishes a final status with phase `none`.
4. **Circuits:**
   - `CircuitLimits` keeps meaning the **normal** price band.
   - During a phase, the band from the quote API goes into `AuctionStatus`, never into
     `CircuitLimits`.
   - For CAS stocks the circuit refresh skips 15:15 to midnight, so the morning's normal
     band stays.
   - Pre-open circuits are taken as normal until checked (verification 2 below).
5. **Gateway:** the quote JSON gains these fields while a phase is active, and drops them
   when it ends:
   - `phase` (`pre_open` or `closing_auction`);
   - `ref_price`;
   - `indicative_price`;
   - `imbalance_qty`;
   - `auction_lower` and `auction_upper`.

   No new stream mode: the fields ride on `quote`.
6. **Web:**
   - A phase badge on the watchlist row and the instrument header ("Pre-open",
     "Closing auction").
   - Kite's extra depth stats: Ref. price, Indicative open or close, Total imbalance.
   - In depth, the auction band replaces the circuits, labelled as such.
   - Depth rows with price 0 read **Market**.
   - After 15:35 the badge goes, and the close shown is the auction price, which is the
     last trade.

### Verification before S1 (first trading day after this ADR)

1. **Auction volume:** capture the raw ticks of three CAS stocks from 15:10 to 15:41 with
   `mda-tail`, to confirm how the volume field behaves during CAS. The volume fix in
   decision 1 assumes the drop-and-return pattern.
2. **Pre-open:** capture the quote API every 3 s from 09:00 to 09:16 for the same stocks
   to see:
   - whether Kite sends an indicative open price, and in which field;
   - pre-open circuits;
   - when the pre-open match prints, and whether Kite's 09:15 candle includes its volume.

   Our 09:15 bar currently doesn't include it.
3. **CAS sub-phase times:** check them against NSE's circular.

## Consequences

- F&O bars for 15:30–15:39 are recorded from the next session. Today's can come from the
  evening backfill: Kite has those candles.
- Today's day-sized auction bars stay wrong until corrected by hand. The backfill can't
  replace them, because Kite has no candles for those minutes and live bars win.
- Quote polling during phases adds about 6 calls a minute for 40 minutes a day: it fits
  the rate limit.
- The phase times live in config. A special session is a config change, not a release.
- **Not covered:**
  - The post-close session (15:50–16:00): we ignore it.
  - Kite's `low/high_limit_price_protection` fields: zero today; we'll add them when seen.
  - Whether option expiry time is still 15:30 now that F&O closes at 15:40. ADR-0022's
    time to expiry uses 15:30 until confirmed.

## Units

| Unit | What |
|---|---|
| **S0** ✅ | Bars: per-market session end and the volume baseline (decision 1) |
| **S1** | Phase clock, feedd quote polling, `AuctionStatus` on `md`; circuit refresh rules |
| **S2** | Gateway quote fields and tests |
| **S3** | Web: phase badge, depth rows, auction band and Market rows |
