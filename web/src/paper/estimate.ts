// The order ticket's estimates (docs/design/paper-trading.md §7, §8): charges per order the
// way libs/oms/charges.hpp computes them (Zerodha's rates of 2026-10-24; MCX's of 2026-10-27,
// ADR-0031) and the margin risk blocks. Shown before placing; the broker's own figures are the
// ones that count.

import type { Instrument, OrderSide } from '../api/client';

const round2 = (x: number) => Math.round(x * 100) / 100;
const roundRupee = (x: number) => Math.round(round2(x));

const FUTURES = { brokerage: 0.0003, cap: 20, stt: 0.0005, nse: 0.0000173, ipft: 0.000001, bse: 0, stamp: 0.00002 };
const OPTIONS = { flat: 20, stt: 0.0015, nse: 0.0003503, ipft: 0.000005, bse: 0.000325, stamp: 0.00003 };
// MCX: CTT in place of STT, MCX's transaction charges.
const MCX_FUTURES = { brokerage: 0.0003, cap: 20, stt: 0.0001, mcx: 0.000021, stamp: 0.00002 };
const MCX_OPTIONS = { flat: 20, stt: 0.0005, mcx: 0.000418, stamp: 0.00003 };
const SEBI = 0.000001;
const GST = 0.18;
export const FUTURES_MARGIN = 0.12;  // index futures (config paper.futures_margin_percent)
export const STOCK_FUTURES_MARGIN = 0.2;  // stock futures (paper.stock_futures_margin_percent)
/** MCX futures margin per commodity (config paper.mcx_margin_percent); 10% otherwise. */
const MCX_MARGIN: Record<string, number> = {
  CRUDEOIL: 0.32, CRUDEOILM: 0.32, NATURALGAS: 0.16, NATGASMINI: 0.16,
  SILVER: 0.14, SILVERM: 0.14, SILVERMIC: 0.14, CARDAMOM: 0.14, MENTHAOIL: 0.12,
};
const MCX_DEFAULT_MARGIN = 0.1;
/** Underlyings that are indices: their futures block the lower margin. */
const INDICES = new Set(['NIFTY', 'BANKNIFTY', 'FINNIFTY', 'MIDCPNIFTY', 'NIFTYNXT50', 'SENSEX', 'BANKEX', 'SENSEX50']);

/** Charges of one order executed in full at `price` (rupees). */
export function estimateCharges(instrument: Instrument, side: OrderSide, quantity: number, price: number): number {
  const turnover = quantity * price;
  if (turnover <= 0) return 0;
  const future = instrument.kind === 'FUT';
  if (instrument.exchange === 'MCX') {
    const r = future ? MCX_FUTURES : MCX_OPTIONS;
    const brokerage = future ? Math.min(round2(turnover * MCX_FUTURES.brokerage), MCX_FUTURES.cap) : MCX_OPTIONS.flat;
    const ctt = side === 'SELL' ? roundRupee(turnover * r.stt) : 0;
    const stamp = side === 'BUY' ? roundRupee(turnover * r.stamp) : 0;
    const exchange = round2(turnover * r.mcx);
    const sebi = round2(turnover * SEBI);
    return round2(brokerage + ctt + exchange + sebi + stamp + round2(GST * (brokerage + exchange + sebi)));
  }
  const nse = instrument.exchange !== 'BSE';
  const brokerage = future ? Math.min(round2(turnover * FUTURES.brokerage), FUTURES.cap) : OPTIONS.flat;
  const rates = future ? FUTURES : OPTIONS;
  const stt = side === 'SELL' ? roundRupee(turnover * rates.stt) : 0;
  const stamp = side === 'BUY' ? roundRupee(turnover * rates.stamp) : 0;
  const exchange = nse ? round2(turnover * rates.nse) + round2(turnover * rates.ipft) : round2(turnover * rates.bse);
  const sebi = round2(turnover * SEBI);
  const gst = round2(GST * (brokerage + exchange + sebi));
  return round2(brokerage + stt + exchange + sebi + stamp + gst);
}

/**
 * Margin an opening order blocks (rupees): index futures 12%, stock futures 20%, MCX futures
 * the commodity's share of notional; a bought option its premium.
 */
export function estimateMargin(instrument: Instrument, side: OrderSide, quantity: number, price: number): number {
  const value = quantity * price;
  if (instrument.kind === 'FUT' && instrument.exchange === 'MCX') {
    return round2(value * (MCX_MARGIN[instrument.underlying] ?? MCX_DEFAULT_MARGIN));
  }
  if (instrument.kind === 'FUT') {
    return round2(value * (INDICES.has(instrument.underlying) ? FUTURES_MARGIN : STOCK_FUTURES_MARGIN));
  }
  return side === 'BUY' ? round2(value) : 0;
}

/** `price` on the instrument's tick (nearest). */
export function onTick(price: number, tick: number): number {
  if (!(tick > 0)) return round2(price);
  return round2(Math.round(price / tick) * tick);
}
