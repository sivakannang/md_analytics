// Market signal badge and helpers (ADR-0028, docs/design/signals.md §8): an index's latest
// direction signal as "▲ Bullish +42 · 30m", from GET /signals until the stream sends a
// newer minute.

import type { LiveSignal, SignalIndex, SignalLabel, SignalRow } from '../api/client';
import { useLiveSignal } from '../stream/useQuotes';

/** What a badge shows: a live signal, or the stored one shaped like it. */
export type CurrentSignal = Pick<LiveSignal, 'minute' | 'strategy' | 'horizon_min' | 'score' | 'label' | 'top'>;

export const FACTOR_NAMES: Record<string, string> = {
  futures_buildup: 'Futures buildup (15 min)',
  atm_oi_shift: 'OI shift near ATM (30 min)',
  pcr_change: 'PCR change (30 min)',
  future_vs_vwap: 'Futures vs VWAP',
  atm_iv_change: 'ATM IV change (30 min)',
  max_pain_pull: 'Max-pain pull (near expiry)',
  sector_breadth: 'Sector breadth',
  vix_change: 'India VIX change (30 min)',
  volume_delta: 'Futures volume delta (15 min)',
};

export const factorName = (factor: string) => FACTOR_NAMES[factor] ?? factor;

const istTime = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  hour12: false,
});

/** "14:05" (IST) for a Unix-seconds time. */
export const hhmm = (t: number) => istTime.format(t * 1000);

export const labelClass = (label: SignalLabel) => (label === 'bullish' ? 'up' : label === 'bearish' ? 'down' : 'muted');

const ARROWS: Record<SignalLabel, string> = { bullish: '▲', bearish: '▼', neutral: '●' };

export const labelText = (label: SignalLabel) => label.charAt(0).toUpperCase() + label.slice(1);

export const signedScore = (score: number) => (score > 0 ? `+${score}` : String(score));

/** A stored row as a badge value: its four largest factors. */
export function fromRow(row: SignalRow): CurrentSignal {
  const top = row.factors
    .filter((f) => f.status === 'present' && Math.round(f.points) !== 0)
    .sort((a, b) => Math.abs(b.points) - Math.abs(a.points))
    .slice(0, 4)
    .map((f) => ({ factor: f.factor, points: Math.round(f.points) }));
  return { ...row, top };
}

/** The index's newest signal of `strategy`: live when the stream has a newer minute. */
export function useCurrentSignal(index: SignalIndex | undefined, strategy: string | undefined): CurrentSignal | undefined {
  const live = useLiveSignal(index?.spot_id ?? 0, strategy ?? '');
  const stored = index?.latest.find((r) => r.strategy === strategy);
  if (live && (!stored || live.minute >= stored.minute)) return live;
  return stored ? fromRow(stored) : undefined;
}

/** A signal older than this many seconds shows its time (the market is closed or signald is down). */
const STALE_AFTER = 180;

export function SignalBadge({ signal }: { signal: CurrentSignal | undefined }) {
  if (!signal) return null;
  const stale = Date.now() / 1000 - (signal.minute + 60) > STALE_AFTER;
  const title = [
    `${signal.strategy}: direction over the next ${signal.horizon_min} minutes, as of ${hhmm(signal.minute + 60)} IST`,
    ...signal.top.map((f) => `${factorName(f.factor)}: ${signedScore(f.points)}`),
    'Experimental: see its track record before relying on it.',
  ].join('\n');
  return (
    <span className={`signal-badge ${labelClass(signal.label)}${stale ? ' stale' : ''}`} title={title}>
      {ARROWS[signal.label]} {labelText(signal.label)} {signedScore(signal.score)}
      <span className="signal-badge-meta">
        · {signal.horizon_min}m{stale && ` · ${hhmm(signal.minute + 60)}`}
      </span>
    </span>
  );
}
