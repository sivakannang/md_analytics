// OI buildup (docs/design/oi-buildup-chart.md §1, instrument-page.md §2): ΔOI bars coloured
// by state, grouped by direction (greens lean bullish, red/amber bearish; strong = new
// positions, light = closing); no bar when no OI was published in the bar. The state and
// the change come from the gateway; this file only draws and explains them.

import type { BarRow, Buildup } from '../api/client';

export const BUILDUP_LABEL: Record<Buildup, string> = {
  long_buildup: 'Long buildup',
  short_covering: 'Short covering',
  short_buildup: 'Short buildup',
  long_unwinding: 'Long unwinding',
  neutral: 'Neutral',
  no_oi_update: 'No OI update',
};

/** Legend order. */
export const BUILDUP_ORDER: Buildup[] = [
  'long_buildup',
  'short_covering',
  'short_buildup',
  'long_unwinding',
  'neutral',
  'no_oi_update',
];

export type BuildupColors = Record<Buildup, string>;

/** Candle colours from the CSS tokens (light and dark). */
export function buildupColors(): BuildupColors {
  const css = getComputedStyle(document.documentElement);
  const get = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
  return {
    long_buildup: get('--oi-long-buildup', '#1b7f3b'),
    short_covering: get('--oi-short-covering', '#7cc995'),
    short_buildup: get('--oi-short-buildup', '#c62828'),
    long_unwinding: get('--oi-long-unwinding', '#e0a100'),
    neutral: get('--oi-neutral', '#9aa7b6'),
    no_oi_update: get('--oi-neutral', '#9aa7b6'),
  };
}

/**
 * How far OI moved inside a bar, against the OI the state was measured from: `high`/`low`
 * are the bar's OI extremes as changes (always spanning 0 and the close), so a +500K
 * addition that ends at +300K shows high +500K. Null without a change.
 */
export function oiRange(row: BarRow): { high: number; low: number } | null {
  const [, , , , , , oi, , oiHigh, oiLow, , , , change] = row;
  if (oi <= 0 || change === null) return null;
  const before = oi - change;
  const high = oiHigh > 0 ? oiHigh - before : change;
  const low = oiLow > 0 ? oiLow - before : change;
  return { high: Math.max(high, change, 0), low: Math.min(low, change, 0) };
}

/**
 * The ΔOI candle for a row (docs/design/instrument-page.md §2): body from 0 to the OI change
 * the state was measured on, wick over the OI range inside the bar, colour = the state. Null
 * without a change (no OI, no update yet, or the live bar before its minute-close refresh):
 * the gap reads as "no OI update".
 */
export function oiChangeBar(row: BarRow, colors: BuildupColors, time: number) {
  const range = oiRange(row);
  const change = row[13];
  if (!range || change === null) return null;
  const color = colors[row[12] ?? 'neutral'];
  return { time, open: 0, close: change, ...range, color, borderColor: color, wickColor: color };
}

/** What a bar's state was measured against: the last earlier row with an OI update. */
export function reference(rows: readonly BarRow[], index: number): BarRow | null {
  for (let i = index - 1; i >= 0; i--) {
    const row = rows[i]!;
    if (row[6] > 0 && row[10] > 0) return row;
  }
  return null;
}

export interface BuildupChange {
  oi: number;
  oiPercent: number;
  price: number;
}

/**
 * OI and price change of `row` from its reference: the OI change is the gateway's (the one
 * the state was measured on); the price change uses the same reference bar.
 */
export function buildupChange(row: BarRow, ref: BarRow | null): BuildupChange | null {
  const change = row[13];
  if (!ref || row[6] <= 0 || change === null) return null;
  const before = row[6] - change;
  return { oi: change, oiPercent: before > 0 ? (change / before) * 100 : 0, price: row[4] - ref[4] };
}
