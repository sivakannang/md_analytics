// Right-pane home: live index cards (docs/design/ui.md §5), with the market signal of the
// indices that have one (docs/design/signals.md §8).

import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';

import { api } from '../api/client';
import type { SignalIndex, SignalsInfo } from '../api/client';
import { DayRange } from '../components/DayRange';
import { SignalBadge, useCurrentSignal } from '../components/SignalBadge';
import { resolveIndices } from '../components/IndexStrip';
import type { Resolved } from '../components/IndexStrip';
import { useAuth } from '../auth/AuthContext';
import { useQuote, useWatch } from '../stream/useQuotes';
import { formatChange, formatPrice } from '../watchlists/format';

function IndexCard({
  instrument,
  label,
  signals,
  strategy,
}: Resolved & { signals: SignalIndex | undefined; strategy: string | undefined }) {
  const quote = useQuote(instrument.instrument_id);
  const signal = useCurrentSignal(signals, strategy);
  const pct = quote?.change_pct;
  const direction = pct === undefined || pct === 0 ? '' : pct > 0 ? 'up' : 'down';
  return (
    <Link to={`/instrument/${instrument.instrument_id}`} className="index-card">
      <span className="muted">{label}</span>
      <span className={`big-price ${direction}`}>{formatPrice(quote?.ltp)}</span>
      <span className={direction}>{formatChange(quote) || ' '}</span>
      <DayRange quote={quote} />
      {signals && <SignalBadge signal={signal} />}
    </Link>
  );
}

export function Dashboard() {
  const { state } = useAuth();
  const [indices, setIndices] = useState<Resolved[]>([]);
  const [signals, setSignals] = useState<SignalsInfo | null>(null);
  useEffect(() => {
    let active = true;
    resolveIndices()
      .then((list) => active && setIndices(list))
      .catch(() => undefined);
    api
      .signals()
      .then((info) => active && setSignals(info))
      .catch(() => undefined); // the cards work without signals
    return () => {
      active = false;
    };
  }, []);
  // The header streams its own indices; this covers the dashboard-only ones too.
  useWatch(indices.map((r) => r.instrument.instrument_id));

  return (
    <div className="page">
      <h2>Hi, {state.status === 'signed-in' ? state.user.username : ''}</h2>
      <section className="index-cards">
        {indices.map((r) => (
          <IndexCard
            key={r.instrument.instrument_id}
            {...r}
            signals={signals?.indices.find((i) => i.spot_id === r.instrument.instrument_id)}
            strategy={signals?.strategies[0]?.id}
          />
        ))}
      </section>
    </div>
  );
}
