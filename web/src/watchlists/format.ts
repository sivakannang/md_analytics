import type { Instrument } from '../api/client';
import type { Quote } from '../stream/quoteStream';

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/** 2026-10-27 -> 27 Oct (the year only when it isn't this year). */
export function formatExpiry(expiry: string, today = new Date()): string {
  const [year = 0, month = 1, day = 1] = expiry.split('-').map(Number);
  const label = `${day} ${MONTHS[month - 1] ?? ''}`;
  return year === today.getFullYear() ? label : `${label} ${year}`;
}

/** 25000 -> "25000", 112.5 -> "112.5" */
function formatStrike(strike: number): string {
  return Number.isInteger(strike) ? String(strike) : strike.toFixed(2).replace(/0$/, '');
}

/**
 * How an instrument reads in a list, like Kite: options and futures by contract
 * ("NIFTY 27 Oct 25000 CE", "BANKNIFTY 28 Oct FUT"), cash instruments by symbol.
 */
export function instrumentTitle(i: Instrument): string {
  if (i.kind === 'OPT' && i.expiry && i.strike !== null && i.option_type) {
    return `${i.underlying} ${formatExpiry(i.expiry)} ${formatStrike(i.strike)} ${i.option_type}`;
  }
  if (i.kind === 'FUT' && i.expiry) {
    return `${i.underlying} ${formatExpiry(i.expiry)} FUT`;
  }
  return i.trading_symbol;
}

const KIND_LABEL: Record<Instrument['kind'], string> = {
  IDX: 'Index',
  EQ: 'Equity',
  FUT: 'Future',
  OPT: 'Option',
};

/** Second line: company name for equities, kind and lot size for derivatives. */
export function instrumentDetail(i: Instrument): string {
  if (i.kind === 'EQ') {
    return i.name && i.name !== i.trading_symbol ? i.name : KIND_LABEL[i.kind];
  }
  if (i.kind === 'FUT' || i.kind === 'OPT') {
    return `${KIND_LABEL[i.kind]} · lot ${i.lot_size}`;
  }
  return KIND_LABEL[i.kind];
}

const priceFormat = new Intl.NumberFormat('en-IN', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2,
});
const quantityFormat = new Intl.NumberFormat('en-IN');

/** 22811.35 -> "22,811.35" (Indian grouping); "—" when unknown. */
export function formatPrice(value: number | undefined): string {
  return value === undefined ? '—' : priceFormat.format(value);
}

/** 1834567 -> "18,34,567"; "—" when unknown. */
export function formatQuantity(value: number | undefined): string {
  return value === undefined ? '—' : quantityFormat.format(value);
}

/** "+12.10 (+0.86%)" from a quote; "" until the previous close is known. */
export function formatChange(quote: Quote | undefined): string {
  if (quote?.change === undefined || quote.change_pct === undefined) return '';
  const sign = quote.change > 0 ? '+' : '';
  return `${sign}${priceFormat.format(quote.change)} (${sign}${quote.change_pct.toFixed(2)}%)`;
}

/** 1234567 -> "12.35L", 12345678 -> "1.23Cr" (Indian units, like Kite's chain). */
export function formatLakhs(value: number | undefined): string {
  if (value === undefined) return '—';
  const abs = Math.abs(value);
  if (abs >= 1e7) return `${(value / 1e7).toFixed(2)}Cr`;
  if (abs >= 1e5) return `${(value / 1e5).toFixed(2)}L`;
  return formatQuantity(value);
}
