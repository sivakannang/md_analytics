// Instrument details for paper trading screens (symbol, lot, tick), fetched once per id and
// shared: positions and orders carry only instrument ids.

import { useEffect, useState } from 'react';

import { api } from '../api/client';
import type { Instrument } from '../api/client';

const cache = new Map<number, Promise<Instrument>>();

export function loadInstrument(id: number): Promise<Instrument> {
  let found = cache.get(id);
  if (!found) {
    found = api.instrument(id);
    found.catch(() => cache.delete(id)); // retry next time
    cache.set(id, found);
  }
  return found;
}

/** The instruments of `ids` as they load (missing until then). */
export function useInstruments(ids: readonly number[]): Map<number, Instrument> {
  const key = [...new Set(ids)].sort((a, b) => a - b).join(',');
  const [known, setKnown] = useState(new Map<number, Instrument>());
  useEffect(() => {
    let active = true;
    const list = key === '' ? [] : key.split(',').map(Number);
    Promise.allSettled(list.map(loadInstrument)).then((results) => {
      if (!active) return;
      const next = new Map<number, Instrument>();
      results.forEach((r) => {
        if (r.status === 'fulfilled') next.set(r.value.instrument_id, r.value);
      });
      setKnown(next);
    });
    return () => {
      active = false;
    };
  }, [key]);
  return known;
}
