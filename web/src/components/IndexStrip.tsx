// Live index values in the header (NIFTY 50, NIFTY BANK, SENSEX), like Kite's top strip.
// The instruments are looked up once by exact symbol; mda-feedd always subscribes them
// (config/mda.yaml feed.instruments), so they tick even when no watchlist holds them.

import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';

import { api } from '../api/client';
import type { Instrument } from '../api/client';
import { useQuote, useWatch } from '../stream/useQuotes';
import { formatChange, formatPrice } from '../watchlists/format';

const INDICES = [
  { exchange: 'NSE', symbol: 'NIFTY 50', label: 'NIFTY 50' },
  { exchange: 'NSE', symbol: 'NIFTY BANK', label: 'BANKNIFTY' },
  { exchange: 'BSE', symbol: 'SENSEX', label: 'SENSEX' },
] as const;

export type Resolved = { instrument: Instrument; label: string };

let cache: Promise<Resolved[]> | null = null;

/** Finds each index by exchange + exact symbol (search ranks the exact match first). */
export function resolveIndices(): Promise<Resolved[]> {
  if (cache === null) {
    const lookups = INDICES.map(async ({ exchange, symbol, label }): Promise<Resolved | null> => {
      const found = await api.searchInstruments(`${symbol} ${exchange}`, 5);
      const instrument = found.find(
        (i) => i.kind === 'IDX' && i.exchange === exchange && i.trading_symbol === symbol,
      );
      return instrument ? { instrument, label } : null;
    });
    cache = Promise.all(lookups)
      .then((list) => list.filter((r): r is Resolved => r !== null))
      .catch((error: unknown) => {
        cache = null; // try again next time
        throw error;
      });
  }
  return cache;
}

function IndexQuote({ instrument, label }: Resolved) {
  const quote = useQuote(instrument.instrument_id);
  const pct = quote?.change_pct;
  const direction = pct === undefined || pct === 0 ? '' : pct > 0 ? 'up' : 'down';
  return (
    <Link to={`/instrument/${instrument.instrument_id}`} className="index-quote">
      <span className="index-name">{label}</span>
      <span className={`index-ltp ${direction}`}>{formatPrice(quote?.ltp)}</span>
      <span className={`index-change ${direction}`}>{formatChange(quote)}</span>
    </Link>
  );
}

export function IndexStrip() {
  const [indices, setIndices] = useState<Resolved[]>([]);
  useEffect(() => {
    let active = true;
    resolveIndices()
      .then((list) => active && setIndices(list))
      .catch(() => undefined); // the strip just stays empty; the page shows API errors
    return () => {
      active = false;
    };
  }, []);
  useWatch(indices.map((r) => r.instrument.instrument_id));

  return (
    <div className="index-strip" aria-label="Indices">
      {indices.map((r) => (
        <IndexQuote key={r.instrument.instrument_id} {...r} />
      ))}
    </div>
  );
}
