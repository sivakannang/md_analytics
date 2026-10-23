// Option chain (docs/design/option-chain-and-bars.md §5, UI5): calls | strike | puts around
// ATM for one underlying and expiry, with live LTP / OI / OI change (OI view) or IV and Greeks
// (Greeks view), and the engine's summary (forward, ATM IV, PCR, max pain) in the footer.
//
// The strike rows come from GET /chains/{underlying}/{expiry}; values stream as quotes for the
// rows' instruments, and the summary through the stream's "chain" mode.

import { memo, useEffect, useMemo, useRef, useState } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';

import { api, errorMessage, isSessionLost } from '../api/client';
import type { Chain, ChainInfo, ChainRow } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { OiByStrike } from '../components/OiByStrike';
import { OiOverTime } from '../components/OiOverTime';
import type { Quote } from '../stream/quoteStream';
import { useChainSummary, useMaxOi, useQuote, useWatch } from '../stream/useQuotes';
import { formatExpiry, formatLakhs, formatPrice } from '../watchlists/format';

type View = 'oi' | 'greeks' | 'strikes' | 'time';
const STRIKE_CHOICES = [10, 20, 30] as const;

const BUILDUP: Record<NonNullable<Quote['buildup']>, { tag: string; label: string; up: boolean }> = {
  long_buildup: { tag: 'LB', label: 'Long buildup: price up, OI up', up: true },
  short_buildup: { tag: 'SB', label: 'Short buildup: price down, OI up', up: false },
  short_covering: { tag: 'SC', label: 'Short covering: price up, OI down', up: true },
  long_unwinding: { tag: 'LU', label: 'Long unwinding: price down, OI down', up: false },
};

function signed(text: string, value: number | undefined): string {
  return value !== undefined && value > 0 ? `+${text}` : text;
}

function fixed(value: number | undefined, digits: number): string {
  return value === undefined ? '—' : value.toFixed(digits);
}

function directionOf(value: number | undefined): string {
  return value === undefined || value === 0 ? '' : value > 0 ? 'up' : 'down';
}

export function OptionChain() {
  const { sessionEnded } = useAuth();
  const [params, setParams] = useSearchParams();
  const [chains, setChains] = useState<ChainInfo[] | null>(null);
  const [chain, setChain] = useState<Chain | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [view, setView] = useState<View>('oi');
  const [strikes, setStrikes] = useState<number>(20);
  const [reload, setReload] = useState(0);

  const info = chains?.find((c) => c.underlying === params.get('u')) ?? chains?.[0];
  const expiry = info?.expiries.find((e) => e === params.get('e')) ?? info?.expiries[0];

  const fail = (e: unknown) => {
    if (isSessionLost(e)) sessionEnded();
    else setError(errorMessage(e));
  };

  useEffect(() => {
    let active = true;
    api
      .chains()
      .then((found) => active && setChains(found))
      .catch((e: unknown) => active && fail(e));
    return () => {
      active = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    if (!info || !expiry) return;
    let active = true;
    setError(null);
    api
      .chain(info.underlying, expiry, strikes)
      .then((found) => active && setChain(found))
      .catch((e: unknown) => active && fail(e));
    return () => {
      active = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [info?.underlying, expiry, strikes, reload]);

  const shown = chain && info && chain.underlying === info.underlying && chain.expiry === expiry ? chain : null;
  const ids = useMemo(
    () => (shown ? shown.rows.flatMap((r) => [r.call, r.put]).filter((id): id is number => id !== null) : []),
    [shown],
  );
  const spotId = shown?.spot_id ?? 0;
  useWatch(spotId > 0 ? [...ids, spotId] : ids);
  const spot = useQuote(spotId);
  const summary = useChainSummary(spotId, expiry ?? '') ?? shown?.summary ?? undefined;
  const maxOi = useMaxOi(ids);

  // Bring the ATM row to the middle of the table when a chain loads.
  const card = useRef<HTMLElement>(null);
  useEffect(() => {
    const box = card.current;
    const row = box?.querySelector<HTMLElement>('tr.atm');
    if (box && row) box.scrollTop = row.offsetTop - box.clientHeight / 2 + row.clientHeight / 2;
  }, [shown]);

  // Re-centre when the engine's ATM drifts away from the middle of the loaded rows.
  const atm = summary?.atm_strike ?? shown?.atm_strike;
  useEffect(() => {
    if (!shown || atm === undefined || atm === null) return;
    const index = shown.rows.findIndex((r) => r.strike === atm);
    const edge = Math.max(2, Math.floor(shown.rows.length / 4));
    if (index < 0 || index < edge || index >= shown.rows.length - edge) {
      if (atm !== shown.atm_strike) setReload((n) => n + 1);
    }
  }, [atm, shown]);

  const select = (underlying: string, nextExpiry?: string) => {
    const next: Record<string, string> = { u: underlying };
    if (nextExpiry) next.e = nextExpiry;
    setParams(next, { replace: true });
  };

  if (error) {
    return (
      <div className="page">
        <p className="form-error">{error}</p>
      </div>
    );
  }
  if (!chains) return <p className="loading">Loading…</p>;
  if (chains.length === 0 || !info) {
    return (
      <div className="page">
        <p className="muted">No option chains are configured (feed.chains in config/mda.yaml).</p>
      </div>
    );
  }

  // In the money: calls below the forward, puts above it (the spot until the engine knows).
  const pivot = summary?.forward ?? spot?.ltp;

  return (
    <div className="page chain-page">
      <div className="chain-toolbar">
        <div className="chart-intervals" role="group" aria-label="Underlying">
          {chains.map((c) => (
            <button
              key={c.underlying}
              type="button"
              className={c.underlying === info.underlying ? 'active' : ''}
              onClick={() => select(c.underlying)}
            >
              {c.underlying}
            </button>
          ))}
        </div>
        {view !== 'strikes' && view !== 'time' && (
          <div className="chart-intervals" role="group" aria-label="Expiry">
            {info.expiries.map((e) => (
              <button
                key={e}
                type="button"
                className={e === expiry ? 'active' : ''}
                onClick={() => select(info.underlying, e)}
              >
                {formatExpiry(e)}
              </button>
            ))}
          </div>
        )}
        <div className="chart-intervals" role="group" aria-label="View">
          <button type="button" className={view === 'oi' ? 'active' : ''} onClick={() => setView('oi')}>
            OI
          </button>
          <button
            type="button"
            className={view === 'greeks' ? 'active' : ''}
            onClick={() => setView('greeks')}
          >
            Greeks
          </button>
          <button
            type="button"
            className={view === 'strikes' ? 'active' : ''}
            onClick={() => setView('strikes')}
          >
            OI by strike
          </button>
          <button type="button" className={view === 'time' ? 'active' : ''} onClick={() => setView('time')}>
            OI over time
          </button>
        </div>
        {view !== 'strikes' && view !== 'time' && (
          <label className="chain-strikes">
            Strikes ±
            <select value={strikes} onChange={(e) => setStrikes(Number(e.target.value))}>
              {STRIKE_CHOICES.map((n) => (
                <option key={n} value={n}>
                  {n}
                </option>
              ))}
            </select>
          </label>
        )}
        <span className="chain-spot">
          {info.underlying} <b className={directionOf(spot?.change)}>{formatPrice(spot?.ltp)}</b>
        </span>
      </div>

      {view === 'strikes' && expiry ? (
        <OiByStrike underlying={info.underlying} expiries={info.expiries} initialExpiry={expiry} />
      ) : view === 'time' && expiry ? (
        <OiOverTime underlying={info.underlying} expiries={info.expiries} initialExpiry={expiry} />
      ) : (
        <>
          <section ref={card} className="card wide chain-card">
            {!shown ? (
              <p className="loading">Loading…</p>
            ) : (
              <table className={`chain-table ${view}`}>
                <thead>
                  <tr className="chain-sides">
                    <th colSpan={view === 'oi' ? 4 : 6}>Calls</th>
                    <th />
                    <th colSpan={view === 'oi' ? 4 : 6}>Puts</th>
                  </tr>
                  <tr>
                    {view === 'oi' ? (
                      <>
                        <th>OI</th>
                        <th>OI chg</th>
                        <th>Chg %</th>
                        <th>LTP</th>
                        <th className="chain-strike">Strike</th>
                        <th>LTP</th>
                        <th>Chg %</th>
                        <th>OI chg</th>
                        <th>OI</th>
                      </>
                    ) : (
                      <>
                        <th>Vega</th>
                        <th>Theta</th>
                        <th>Gamma</th>
                        <th>Delta</th>
                        <th>IV</th>
                        <th>LTP</th>
                        <th className="chain-strike">Strike</th>
                        <th>LTP</th>
                        <th>IV</th>
                        <th>Delta</th>
                        <th>Gamma</th>
                        <th>Theta</th>
                        <th>Vega</th>
                      </>
                    )}
                  </tr>
                </thead>
                <tbody>
                  {shown.rows.map((row) => (
                    <StrikeRow
                      key={row.strike}
                      row={row}
                      view={view}
                      atm={row.strike === atm}
                      callItm={pivot !== undefined && row.strike < pivot}
                      putItm={pivot !== undefined && row.strike > pivot}
                      maxOi={maxOi}
                    />
                  ))}
                </tbody>
              </table>
            )}
          </section>

          <footer className="chain-summary" aria-label="Chain summary">
            <span>
              Forward <b>{formatPrice(summary?.forward ?? undefined)}</b>
            </span>
            <span>
              ATM <b>{atm ?? '—'}</b>
            </span>
            <span>
              ATM IV <b>{summary?.atm_iv != null ? `${summary.atm_iv.toFixed(2)}%` : '—'}</b>
            </span>
            <span title="Put OI / call OI over the subscribed strikes">
              PCR <b>{summary ? summary.pcr.toFixed(2) : '—'}</b>
            </span>
            <span title="Strike where option writers pay out least (over the subscribed strikes)">
              Max pain <b>{summary?.max_pain || '—'}</b>
            </span>
            <span>
              Days to expiry <b>{summary ? summary.days_to_expiry.toFixed(1) : '—'}</b>
            </span>
            {shown && (
              <span>
                Lot <b>{shown.lot_size}</b>
              </span>
            )}
          </footer>
        </>
      )}
    </div>
  );
}

interface StrikeRowProps {
  row: ChainRow;
  view: View;
  atm: boolean;
  callItm: boolean;
  putItm: boolean;
  maxOi: number;
}

const StrikeRow = memo(function StrikeRow({ row, view, atm, callItm, putItm, maxOi }: StrikeRowProps) {
  const call = useQuote(row.call ?? 0);
  const put = useQuote(row.put ?? 0);
  const navigate = useNavigate();
  const open = (id: number | null) => id !== null && navigate(`/instrument/${id}`);

  const side = (quote: Quote | undefined, id: number | null, itm: boolean, mirror: boolean) => {
    const cls = `chain-cell${itm ? ' itm' : ''}`;
    const ltp = (
      <td key="ltp" className={`${cls} chain-ltp`} onClick={() => open(id)} title="Open chart">
        <span className={directionOf(quote?.change)}>{formatPrice(quote?.ltp)}</span>
        {quote?.buildup && (
          <span
            className={`chain-buildup ${BUILDUP[quote.buildup].up ? 'up' : 'down'}`}
            title={BUILDUP[quote.buildup].label}
          >
            {BUILDUP[quote.buildup].tag}
          </span>
        )}
      </td>
    );
    let cells: React.ReactNode[];
    if (view === 'oi') {
      const width = maxOi > 0 && quote?.oi ? Math.round((quote.oi / maxOi) * 100) : 0;
      cells = [
        <td key="oi" className={`${cls} chain-oi`}>
          <span className={`chain-oi-bar ${mirror ? 'put' : 'call'}`} style={{ width: `${width}%` }} />
          <span className="chain-oi-value">{formatLakhs(quote?.oi)}</span>
        </td>,
        <td key="oichg" className={`${cls} ${directionOf(quote?.oi_change)}`}>
          {signed(formatLakhs(quote?.oi_change), quote?.oi_change)}
        </td>,
        <td key="chg" className={`${cls} ${directionOf(quote?.change_pct)}`}>
          {quote?.change_pct === undefined ? '—' : signed(quote.change_pct.toFixed(2), quote.change_pct)}
        </td>,
        ltp,
      ];
    } else {
      cells = [
        <td key="vega" className={cls}>{fixed(quote?.vega, 2)}</td>,
        <td key="theta" className={cls}>{fixed(quote?.theta, 2)}</td>,
        <td key="gamma" className={cls}>{fixed(quote?.gamma, 4)}</td>,
        <td key="delta" className={cls}>{fixed(quote?.delta, 2)}</td>,
        <td key="iv" className={cls}>{fixed(quote?.iv, 2)}</td>,
        ltp,
      ];
    }
    return mirror ? cells.reverse() : cells;
  };

  return (
    <tr className={atm ? 'atm' : undefined}>
      {side(call, row.call, callItm, false)}
      <td className="chain-strike">{row.strike}</td>
      {side(put, row.put, putItm, true)}
    </tr>
  );
});
