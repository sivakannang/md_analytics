// Right pane for one instrument, chart first (docs/design/instrument-page.md): live price and
// change, one line of the day's stats, and the chart filling the height. Market depth lives
// in the watchlist (UI2), as on Kite. Options and futures link to their option chain;
// indices with market signals get a Signals tab (?view=signals, docs/design/signals.md §8).

import { useEffect, useState } from 'react';
import { Link, useParams, useSearchParams } from 'react-router-dom';

import { api, errorMessage, isSessionLost } from '../api/client';
import type { ChainInfo, Instrument, SignalsInfo } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { PriceChart } from '../chart/PriceChart';
import { SignalsView } from '../components/SignalsView';
import { useOrderTicket } from '../paper/OrderTicket';
import type { Quote } from '../stream/quoteStream';
import { useQuote, useWatch } from '../stream/useQuotes';
import {
  formatChange,
  formatExpiry,
  formatPrice,
  formatQuantity,
  instrumentDetail,
  instrumentTitle,
} from '../watchlists/format';

/** The day's numbers that depth used to push below the fold, on one line. */
function StatsLine({ instrument, quote }: { instrument: Instrument; quote: Quote | undefined }) {
  const hasOi = instrument.kind === 'FUT' || instrument.kind === 'OPT';
  const item = (label: string, value: string, title?: string) => (
    <span title={title}>
      {label} <b>{value}</b>
    </span>
  );
  return (
    <p className="stats-line">
      {item('O', formatPrice(quote?.open))}
      {item('H', formatPrice(quote?.high))}
      {item('L', formatPrice(quote?.low))}
      {item('Prev', formatPrice(quote?.close), 'Previous close')}
      {instrument.kind !== 'IDX' && item('Vol', formatQuantity(quote?.volume))}
      {hasOi &&
        item(
          'OI',
          formatQuantity(quote?.oi),
          `Day high ${formatQuantity(quote?.oi_day_high)} · low ${formatQuantity(quote?.oi_day_low)}`,
        )}
      {quote?.lower_circuit !== undefined &&
        quote.upper_circuit !== undefined &&
        item('Circuit', `${formatPrice(quote.lower_circuit)}–${formatPrice(quote.upper_circuit)}`)}
      {instrument.kind !== 'IDX' && item('Lot', String(instrument.lot_size))}
    </p>
  );
}

/** The option chain this instrument belongs to (configured underlyings only). */
function chainLink(instrument: Instrument, chains: ChainInfo[] | null): string | null {
  if (instrument.kind !== 'FUT' && instrument.kind !== 'OPT') return null;
  const chain = chains?.find((c) => c.underlying === instrument.underlying);
  if (!chain) return null;
  const expiry = instrument.expiry && chain.expiries.includes(instrument.expiry) ? `&e=${instrument.expiry}` : '';
  return `/chain?u=${encodeURIComponent(chain.underlying)}${expiry}`;
}

export function InstrumentPage() {
  const id = Number(useParams().instrumentId);
  const { sessionEnded } = useAuth();
  const [instrument, setInstrument] = useState<Instrument | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [chains, setChains] = useState<ChainInfo[] | null>(null);
  const [signals, setSignals] = useState<SignalsInfo | null>(null);
  const [search] = useSearchParams();
  const openTicket = useOrderTicket();
  const quote = useQuote(id);
  useWatch(Number.isInteger(id) && id > 0 ? [id] : []);

  useEffect(() => {
    let active = true;
    setInstrument(null);
    setError(null);
    api
      .instrument(id)
      .then((found) => active && setInstrument(found))
      .catch((e: unknown) => {
        if (!active) return;
        if (isSessionLost(e)) sessionEnded();
        else setError(errorMessage(e));
      });
    return () => {
      active = false;
    };
  }, [id, sessionEnded]);

  useEffect(() => {
    let active = true;
    api
      .chains()
      .then((found) => active && setChains(found))
      .catch(() => undefined); // only the Option chain tab depends on it
    api
      .signals()
      .then((found) => active && setSignals(found))
      .catch(() => undefined); // only the Signals tab depends on it
    return () => {
      active = false;
    };
  }, []);

  if (error) {
    return (
      <div className="page">
        <p className="form-error">{error}</p>
      </div>
    );
  }
  if (!instrument) {
    return <p className="loading">Loading…</p>;
  }

  const chain = chainLink(instrument, chains);
  const signalIndex = signals?.indices.find((i) => i.spot_id === instrument.instrument_id);
  const showSignals = signals !== null && signalIndex !== undefined && search.get('view') === 'signals';
  const pct = quote?.change_pct;
  const direction = pct === undefined || pct === 0 ? '' : pct > 0 ? 'up' : 'down';

  return (
    <div className="page instrument-page">
      <header className="instrument-header">
        <div>
          <h2>
            {instrumentTitle(instrument)} <span className="exchange">{instrument.exchange}</span>
          </h2>
          <p className="muted">
            {instrumentDetail(instrument)}
            {instrument.expiry && ` · expires ${formatExpiry(instrument.expiry)}`}
            {!instrument.listed && ' · expired or delisted'}
          </p>
        </div>
        <div className="instrument-price">
          <span className={`big-price ${direction}`}>
            {formatPrice(quote?.ltp)}
          </span>
          <span className={direction}>{formatChange(quote) || ' '}</span>
          {(instrument.kind === 'FUT' || instrument.kind === 'OPT') && instrument.listed && (
            <span className="instrument-trade">
              <button type="button" className="trade-btn buy" onClick={() => openTicket({ instrument, side: 'BUY' })}>
                Buy
              </button>
              <button type="button" className="trade-btn sell" onClick={() => openTicket({ instrument, side: 'SELL' })}>
                Sell
              </button>
              <span className="paper-pill" title="Simulated: no real order is placed">
                PAPER
              </span>
            </span>
          )}
        </div>
      </header>

      <StatsLine instrument={instrument} quote={quote} />

      <nav className="instrument-tabs" aria-label="Views">
        {showSignals ? (
          <Link to={`/instrument/${instrument.instrument_id}`}>Chart</Link>
        ) : (
          <span className="active" aria-current="page">
            Chart
          </span>
        )}
        {signalIndex &&
          (showSignals ? (
            <span className="active" aria-current="page">
              Signals
            </span>
          ) : (
            <Link to={`/instrument/${instrument.instrument_id}?view=signals`}>Signals</Link>
          ))}
        {chain && <Link to={chain}>Option chain</Link>}
      </nav>

      {showSignals && signals && signalIndex ? (
        <SignalsView index={signalIndex} info={signals} />
      ) : (
        <PriceChart instrument={instrument} />
      )}

      {!quote && (
        <p className="muted">
          No ticks received yet. Live data flows for instruments on a watchlist (it starts within a few
          seconds of adding one) and for the indices in the header.
        </p>
      )}
    </div>
  );
}
