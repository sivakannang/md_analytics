// Right-pane home: live index cards and what is coming (docs/design/ui.md §5).

import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';

import { DayRange } from '../components/DayRange';
import { resolveIndices } from '../components/IndexStrip';
import type { Resolved } from '../components/IndexStrip';
import { useAuth } from '../auth/AuthContext';
import { useQuote } from '../stream/useQuotes';
import { formatChange, formatPrice } from '../watchlists/format';

function IndexCard({ instrument, label }: Resolved) {
  const quote = useQuote(instrument.instrument_id);
  const pct = quote?.change_pct;
  const direction = pct === undefined || pct === 0 ? '' : pct > 0 ? 'up' : 'down';
  return (
    <Link to={`/instrument/${instrument.instrument_id}`} className="index-card">
      <span className="muted">{label}</span>
      <span className={`big-price ${direction}`}>{formatPrice(quote?.ltp)}</span>
      <span className={direction}>{formatChange(quote) || ' '}</span>
      <DayRange quote={quote} />
    </Link>
  );
}

export function Dashboard() {
  const { state } = useAuth();
  const [indices, setIndices] = useState<Resolved[]>([]);
  useEffect(() => {
    let active = true;
    resolveIndices()
      .then((list) => active && setIndices(list))
      .catch(() => undefined);
    return () => {
      active = false;
    };
  }, []);
  // The header strip already streams these instruments.

  return (
    <div className="page">
      <h2>Hi, {state.status === 'signed-in' ? state.user.username : ''}</h2>
      <section className="index-cards">
        {indices.map((r) => (
          <IndexCard key={r.instrument.instrument_id} {...r} />
        ))}
      </section>
      <section className="card wide">
        <h2>Coming next</h2>
        <ul className="roadmap">
          <li>Market depth and day stats inside the watchlist</li>
          <li>Charts with price, volume and open interest panes</li>
          <li>Option chain with OI, Greeks, PCR and max pain</li>
          <li>OI buildup analytics and a multi-panel Analyst workspace</li>
        </ul>
      </section>
    </div>
  );
}
