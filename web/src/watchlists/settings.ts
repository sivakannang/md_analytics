// Watchlist display settings (Kite's marketwatch settings): per browser, in localStorage.

import { useSyncExternalStore } from 'react';

import type { Quote } from '../stream/quoteStream';

export interface DisplaySettings {
  /** Change measured from the previous close (default) or today's open. */
  changeFrom: 'close' | 'open';
  showChange: boolean;
  showChangePct: boolean;
  showDirection: boolean;
}

const KEY = 'mda.watchlist.display';
const DEFAULTS: DisplaySettings = {
  changeFrom: 'close',
  showChange: true,
  showChangePct: true,
  showDirection: true,
};

function read(): DisplaySettings {
  try {
    const stored = JSON.parse(localStorage.getItem(KEY) ?? '{}') as Partial<DisplaySettings>;
    return { ...DEFAULTS, ...stored };
  } catch {
    return DEFAULTS;
  }
}

let current = read();
const listeners = new Set<() => void>();

export function setDisplaySettings(change: Partial<DisplaySettings>) {
  current = { ...current, ...change };
  try {
    localStorage.setItem(KEY, JSON.stringify(current));
  } catch {
    // a convenience only
  }
  for (const listener of listeners) listener();
}

export function useDisplaySettings(): DisplaySettings {
  return useSyncExternalStore(
    (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    () => current,
  );
}

/** Change and % from the chosen base; undefined until both prices are known. */
export function changeOf(
  quote: Quote | undefined,
  from: DisplaySettings['changeFrom'],
): { change: number; pct: number } | undefined {
  const base = from === 'open' ? quote?.open : quote?.close;
  if (quote?.ltp === undefined || base === undefined || base === 0) return undefined;
  const change = quote.ltp - base;
  return { change, pct: (change / base) * 100 };
}
