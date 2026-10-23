// Candle buckets, the same rule as the gateway (engine/libs/bars aggregate.hpp): intraday
// buckets aligned to the 09:15 IST session start, daily buckets = IST days.

import type { BarInterval, BarRow } from '../api/client';
import type { LiveBar } from '../stream/quoteStream';

const IST = 5 * 3600 + 30 * 60;
const SESSION = 9 * 3600 + 15 * 60;

export const INTERVALS: { value: BarInterval; label: string; seconds: number }[] = [
  { value: '1m', label: '1m', seconds: 60 },
  { value: '3m', label: '3m', seconds: 180 },
  { value: '5m', label: '5m', seconds: 300 },
  { value: '15m', label: '15m', seconds: 900 },
  { value: '30m', label: '30m', seconds: 1800 },
  { value: '1h', label: '1h', seconds: 3600 },
  { value: '1d', label: '1D', seconds: 86_400 },
];

export function istMidnight(t: number): number {
  const local = t + IST;
  return local - (((local % 86_400) + 86_400) % 86_400) - IST;
}

export function bucketStart(t: number, interval: BarInterval): number {
  const midnight = istMidnight(t);
  if (interval === '1d') return midnight;
  const length = INTERVALS.find((i) => i.value === interval)?.seconds ?? 60;
  const session = midnight + SESSION;
  return session + Math.floor((t - session) / length) * length;
}

/** A live minute as a history row; its buildup state and ΔOI come from the REST bars. */
export function liveRow(b: LiveBar): BarRow {
  return [b.t, b.o, b.h, b.l, b.c, b.v, b.oi, b.oi_o, b.oi_h, b.oi_l, b.oi_n, b.oi_t, null, null];
}

/**
 * Merges 1-minute rows (oldest first) into one candle stamped `start`, like the gateway's
 * aggregate(): OI open/close = first/last known OI, high/low over minutes with OI, updates
 * summed, OI time = the last known one. State and ΔOI stay null (they are the server's).
 */
export function mergeRows(start: number, minutes: BarRow[]): BarRow | null {
  const first = minutes[0];
  if (!first) return null;
  let [, open, high, low, close, volume] = first;
  let oi = 0, oiOpen = 0, oiHigh = 0, oiLow = 0, updates = 0;
  let oiTime: number | null = null;
  minutes.forEach(([, , h, l, c, v, o, oo, oh, ol, n, t], i) => {
    if (i > 0) {
      high = Math.max(high, h);
      low = Math.min(low, l);
      close = c;
      volume += v;
    }
    updates += n;
    if (t !== null) oiTime = t;
    if (o <= 0) return;
    if (oi === 0) {
      [oiOpen, oiHigh, oiLow] = [oo, oh, ol];
    } else {
      oiHigh = Math.max(oiHigh, oh);
      oiLow = Math.min(oiLow, ol);
    }
    oi = o;
  });
  return [start, open, high, low, close, volume, oi, oiOpen, oiHigh, oiLow, updates, oiTime, null, null];
}

/** Shifts Unix seconds so the chart (which draws UTC) shows Indian time. */
export function toChartTime(t: number): number {
  return t + IST;
}
