# ADR-0031: MCX commodities: market data, option chains and paper trading

- **Status:** Accepted
- **Date:** 2026-10-27
- **Deciders:** owner, Claude

## Context

The platform covers NSE and BSE only. The instrument import skips every MCX row, so crude
oil, gold, silver and natural gas can't be watched, charted or paper traded. The owner wants
MCX on the same footing as NSE F&O: watchlists, charts, the option chain and its OI views,
and paper trading.

Facts that shape the choice (Kite's instrument list of 2026-10-27, Kite's margin calculator
via `tools/research/mcx_margin_probe.py`, zerodha.com/charges, MCX and Zerodha notices):

- Kite lists about 16,500 MCX rows: 174 futures, 16,374 options, and 11 MCX indices.
  Options trade on 11 underlyings: CRUDEOIL, CRUDEOILM, GOLD, GOLDM, SILVER, SILVERM,
  NATURALGAS, NATGASMINI, COPPER, ZINC and MCXBULLDEX.
- **Every MCX row has `lot_size` 1.** Kite counts MCX quantities in lots: order quantity,
  depth, volume and OI are all in lots. Prices are per quotation unit (₹ per barrel, per
  10 g of gold, per kg of silver). One lot holds a fixed number of those units, which Kite
  doesn't publish: CRUDEOIL 100, GOLD 100 (1 kg / 10 g), SILVER 30, NATURALGAS 1,250.
  The probe confirmed them: each mini and full contract comes out at the same margin
  percentage.
- **MCX options are options on a future.** Each option series has one underlying future:
  the nearest future of the same name expiring on or after the option. GOLD options of
  30 Oct are on the 4 Dec future, and CRUDEOIL options of 17 Nov on the 19 Nov future.
  The options expire a few days before that future, and in-the-money options devolve into
  futures positions.
- **Trading hours** are 09:00–23:30 IST while the US is on daylight saving time (second
  Sunday of March to first Sunday of November), 09:00–23:55 otherwise. Agri contracts close
  earlier (17:00 or 21:00). The market processes stop at 16:05 today.
- **Margins** are SPAN plus exposure and differ a lot by commodity. On 2026-10-27 one lot
  needed: crude oil 31% of its value, natural gas 16%, silver 13%, cardamom 13%, mentha 11%,
  gold and base metals 9%, lead 7%.
- **Charges** (Zerodha): brokerage as on F&O; CTT 0.01% (futures) or 0.05% (options) on the
  sell side; MCX transaction charges 0.0021% (futures) or 0.0418% (options premium); SEBI
  ₹10 per crore; stamp duty on the buy side as on F&O; GST 18%. No tax on exercise: options
  devolve into futures.
- Zerodha squares off MCX intraday (MIS) positions about 10 minutes before the close.
- MCX's maximum order size per contract isn't in Kite's data. An unofficial table gives crude
  100 lots, gold 10, silver 10 and natural gas 16.

## Options considered

### Quantities: in lots (as Kite) with a contract multiplier, or in units
- **Lots, multiplier on the side.** Matches Kite's order ticket, depth, volume and OI.
  But every value calculation (P&L, margin, notional, charges, the ticket estimate, the
  web's live P&L) would need the multiplier, in about ten places in C++ and TypeScript.
  Missing one silently gives a wrong P&L by 100×.
- **Units of the quotation (chosen).** At import, an MCX contract's `lot_size` becomes the
  units per lot, so one CRUDEOIL lot is quantity 100 at a price per barrel. Every
  price × quantity in the OMS, risk, charges and web stays right unchanged, exactly like a
  NIFTY lot of 65. Only Kite's own quantities need converting, and they come in at two
  places: market depth for the paper venue (lots × units) and the OI buildup threshold
  (OI is already in lots).

### Hours: per exchange, or a second "evening" stack
- A separate evening process set would duplicate the stack. Instead the session is a
  property of the instrument's exchange, and the market processes run until after the MCX
  close.

### Option chain underlying: the spot or the future
- MCX has no spot price for options. The chain's "spot" becomes the option series'
  underlying future. The engine already prices on a forward with Black-76 (ADR-0022), and a
  future is a forward, so IV and Greeks need no new model.

## Decision

1. **Exchange MCX** (`MCX|FO|FUT|CRUDEOIL|2026-11-19`, `MCX|FO|OPT|…`, and MCX indices as
   `MCX|CASH|IDX|MCXCOMDEX`). The import keeps MCX futures, options and indices.
2. **Quantities in units.** A table in `libs/instruments` (`mcx.hpp`) gives the units per lot
   for each commodity. The import sets that as `lot_size`. A commodity not in the table
   keeps lot size 1 and can be watched but not paper traded.
3. **Kite's lot-based quantities** stay as Kite sends them on the `md` bus and in
   `bars.db` (volume, OI and depth for MCX are in lots). They are converted where they meet
   unit quantities: the paper venue's depth, and the OI buildup threshold.
4. **Underlyings.** An MCX option's underlying is its future, as above. An MCX index future
   (MCXBULLDEX) links to its index. Other futures have none.
5. **Sessions per exchange** in `libs/instruments` (`session.hpp`): NSE and BSE 09:15–15:30
   (F&O to 15:40), MCX 09:00 to 23:30 or 23:55 by US daylight saving time. Bars, intraday
   bucket alignment, the option expiry time (years to expiry), the OI views, risk's market
   hours and the backfill window all use it.
6. **Process hours.** The watchdog starts the market processes at 08:30 as before and keeps
   them running to the MCX close. It stops them at 00:05–00:11 instead of 16:05.
7. **Option chains** for MCX are configured like the indices (`feed.chains`), with the
   underlying name and no spot: each expiry's chain uses its own future.
8. **Paper trading** accepts MCX futures and options with a units table entry:
   - margin per commodity (`paper.mcx_margin_percent`, with a default), from the probe;
   - MCX charges as above;
   - maximum order size per commodity in `paper.freeze_quantity` (in units), sliced like
     NSE;
   - the close of day at the same distance from the MCX close as from 15:30 on NSE: opening
     orders stop and MIS is squared off 5 minutes before it (Zerodha: about 10), DAY orders
     expire at it, and expiries settle a minute after it;
   - expiry: futures settle at their last price, like NSE. **Options settle in cash at
     intrinsic value against the underlying future's last price.** Zerodha devolves them
     into futures instead; the paper broker doesn't open that future.
9. Market signals and the auto trader stay on the indices.

## Rationale

Units keep a single meaning for quantity across the OMS, risk, charges, ledger and web, and
the existing tests cover that arithmetic. Lots would have spread a multiplier through every
value calculation. The cost is that MCX quantities in our order book differ from Kite's
(100 instead of 1 for a crude oil lot). The ticket shows lots beside quantity, as it already
does for NSE.

Per-exchange sessions in one place replace seven copies of 09:15–15:30. Options on futures
fit the forward-based pricing already in place.

## Consequences

- Positive:
  - Commodities can be watched, charted, analysed and paper traded in the evening, when
    NSE is shut.
  - Market-hours rules now live in one place.
- Negative / costs we accept:
  - The market processes and the recorder run until about midnight, and the recorder keeps
    more data each day.
  - The units table and margins are hand-maintained. MCX changes lot sizes now and then, and
    margins move with volatility; rerun the probe after a contract change.
  - Option expiry is cash-settled in paper. Devolvement into futures isn't modelled.
  - MCX volume, OI and depth are shown in lots while paper quantities are in units.
- Follow-ups:
  - Agri contracts' earlier closes; Zerodha's rule that MIS is allowed only for energy
    options.
  - Commodity signals or strategies, if wanted.

## Learning notes

- Options on futures vs options on spot: Black-76 with the future as the forward, and why
  put–call parity then needs no rate or dividend.
- Contract multipliers and why exchanges quote per unit rather than per lot. CME does the
  same: crude is quoted per barrel, 1,000 barrels a lot.
- SPAN margin: scenario-based worst loss, which is why the margin share differs per
  commodity.
- Session handling across venues with different hours and daylight saving rules: the kind of
  bug where a strategy trades into a closed market.

## Units

| Unit | What |
|---|---|
| MX1 | Exchange MCX, units table, import, contract keys, Kite's REST and circuits for MCX, the feed config: MCX futures and indices on watchlists, quotes, depth and charts |
| MX2 | Sessions per exchange: bars, bucket alignment, backfill window, risk hours, monitor and alerts, the watchdog until the MCX close |
| MX3 | MCX option chains: underlying futures, chain and OI views on the future, years to expiry at the MCX close |
| MX4 | Paper trading: margins, charges, maximum order size, depth in units, MIS square-off and DAY expiry at the MCX close, options settled at expiry |
