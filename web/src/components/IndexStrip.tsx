// Live index values in the header (NIFTY 50, SENSEX, NIFTY BANK), like Kite's top strip;
// the dashboard shows every index here, the header only those marked `header`.
// The instruments are looked up once by exact symbol; mda-feedd always subscribes them
// (config/mda.yaml feed.instruments, or as a chain's spot), so they tick even when no
// watchlist holds them.

import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';

import { api } from '../api/client';
import type { Instrument } from '../api/client';
import { useQuote, useWatch } from '../stream/useQuotes';
import { formatChange, formatPrice } from '../watchlists/format';

const INDICES = [
  { exchange: 'NSE', symbol: 'NIFTY 50', label: 'NIFTY 50', header: true },
  { exchange: 'BSE', symbol: 'SENSEX', label: 'SENSEX', header: true },
  { exchange: 'NSE', symbol: 'NIFTY BANK', label: 'BANKNIFTY', header: true },
  { exchange: 'BSE', symbol: 'BANKEX', label: 'BANKEX', header: false },
  { exchange: 'NSE', symbol: 'NIFTY FIN SERVICE', label: 'NIFTY FIN', header: false },
  { exchange: 'NSE', symbol: 'NIFTY AUTO', label: 'NIFTY AUTO', header: false },
  { exchange: 'NSE', symbol: 'NIFTY IT', label: 'NIFTY IT', header: false },
  { exchange: 'NSE', symbol: 'NIFTY PHARMA', label: 'NIFTY PHARMA', header: false },
  { exchange: 'NSE', symbol: 'NIFTY FMCG', label: 'NIFTY FMCG', header: false },
  { exchange: 'NSE', symbol: 'NIFTY CAPITAL MKT', label: 'NIFTY CAPITAL MKT', header: false },
] as const;

export type Resolved = { instrument: Instrument; label: string; header: boolean };

let cache: Promise<Resolved[]> | null = null;

/** Finds each index by exchange + exact symbol. Search ranks an exact symbol first only when
 *  the query is just the symbol ("NIFTY IT NSE" ranked NIFTY IT below 5 other indices). */
export function resolveIndices(): Promise<Resolved[]> {
  if (cache === null) {
    const lookups = INDICES.map(async ({ exchange, symbol, label, header }): Promise<Resolved | null> => {
      const found = await api.searchInstruments(symbol, 5);
      const instrument = found.find(
        (i) => i.kind === 'IDX' && i.exchange === exchange && i.trading_symbol === symbol,
      );
      return instrument ? { instrument, label, header } : null;
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
      .then((list) => active && setIndices(list.filter((r) => r.header)))
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
