// The Paper page's Auto tab (docs/design/auto-trader.md §8): the signed-in user's
// auto-trader runs (ADR-0030) with their settings, position, metrics over a period and
// today's decisions. Runs are configured in config/mda.yaml (autotrader.runs); this is a
// read-only view. Re-read when an order update arrives and every 15 s.

import { useCallback, useEffect, useState } from 'react';

import { api, errorMessage } from '../api/client';
import type { AutoRun, AutoRuns as AutoRunsData } from '../api/client';
import { quoteStream } from '../stream/quoteStream';
import { useInstruments } from './instruments';
import { formatPrice } from '../watchlists/format';

const PERIODS = [5, 30, 90] as const;

const istTime = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  hour12: false,
});

const money = (value: number) =>
  `₹${value.toLocaleString('en-IN', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`;

const tone = (value: number) => (value > 0 ? 'up' : value < 0 ? 'down' : '');

function Run({ run }: { run: AutoRun }) {
  const m = run.metrics;
  const options = run.strategy === 'follow-signal-options';
  const instruments = useInstruments(run.held.map((h) => h.instrument_id));
  const held = run.held
    .map((h) => {
      const side = h.quantity > 0 ? 'Long' : 'Short';
      return `${side} ${Math.abs(h.quantity)} ${instruments.get(h.instrument_id)?.trading_symbol ?? ''}`.trim();
    })
    .join(' · ');
  const s = run.settings;
  const exits = Object.entries(m.exits)
    .map(([reason, count]) => `${reason} ${count}`)
    .join(' · ');
  return (
    <section className="auto-run">
      <header className="auto-run-head">
        <h3>
          {run.id} <span className="muted small">{run.underlying} {options ? 'ATM options (bought)' : 'near-month future'} · {run.strategy} on {run.signal}</span>
        </h3>
        <span className={`order-status ${run.enabled ? 'complete' : 'cancelled'}`}>{run.enabled ? 'ON' : 'OFF'}</span>
        <span className={tone(run.position)}>
          {held === '' ? 'Flat' : held}
        </span>
        <span className="muted small">{run.entries_today} entries today</span>
      </header>
      <p className="muted small">
        {s.lots} lot {s.product} ·{' '}
        {options
          ? `index stop ${s.index_stop_points} pts · premium SL ${s.premium_stop_percent}%`
          : `stop ${s.stop_points} pts`}{' '}
        · target {s.target_points || '—'} pts · max hold{' '}
        {s.max_hold_min} min · {s.max_trades_per_day} trades/day · cooldown {s.cooldown_min} min ·{s.exit_score !== undefined && ` exit below ${s.exit_score} ·`} daily loss stop{' '}
        {money(s.daily_loss_stop)}
      </p>
      <div className="funds-cards">
        <div className="funds-card">
          <span className="muted">Net (after charges)</span>
          <b className={tone(m.net)}>{money(m.net)}</b>
        </div>
        <div className="funds-card">
          <span className="muted">Gross / charges</span>
          <b>
            <span className={tone(m.gross)}>{money(m.gross)}</span> / {money(m.charges)}
          </b>
        </div>
        <div className="funds-card">
          <span className="muted">Trades · win rate</span>
          <b>
            {m.round_trips} · {m.win_rate === null ? '—' : `${(m.win_rate * 100).toFixed(0)}%`}
          </b>
        </div>
        <div className="funds-card">
          <span className="muted">Avg win / avg loss</span>
          <b>
            <span className="up">{money(m.average_win)}</span> / <span className="down">{money(m.average_loss)}</span>
          </b>
        </div>
        <div className="funds-card">
          <span className="muted">Largest loss · max drawdown</span>
          <b className="down">
            {money(m.largest_loss)} · {money(m.max_drawdown > 0 ? -m.max_drawdown : 0)}
          </b>
        </div>
      </div>
      {exits && <p className="muted small">Exits: {exits}</p>}
      <h4>Today's decisions</h4>
      {run.decisions_today.length === 0 ? (
        <p className="muted">None yet{run.enabled ? '' : ': the run is off'}.</p>
      ) : (
        <div className="table-scroll">
          <table className="items paper-table">
            <thead>
              <tr>
                <th>Time</th>
                <th>State</th>
                <th>Action</th>
                <th>Why</th>
                <th>Price</th>
              </tr>
            </thead>
            <tbody>
              {run.decisions_today.map((d, i) => (
                <tr key={`${d.at}-${i}`}>
                  <td>{istTime.format(d.at)}</td>
                  <td className="muted">{d.state}</td>
                  <td>{d.action}</td>
                  <td>{d.reason}</td>
                  <td>{formatPrice(d.price ?? undefined)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}

/** `user`: another user's runs, read-only (an admin, from Admin → Users). */
export function AutoRuns({ user }: { user?: number } = {}) {
  const [days, setDays] = useState<number>(30);
  const [data, setData] = useState<AutoRunsData | null>(null);
  const [error, setError] = useState<string | null>(null);
  const load = useCallback(() => {
    api
      .paperAuto(days, user)
      .then((d) => {
        setData(d);
        setError(null);
      })
      .catch((e: unknown) => setError(errorMessage(e)));
  }, [days, user]);
  useEffect(() => {
    load();
    let timer: ReturnType<typeof setTimeout> | undefined;
    const stop = quoteStream.watchOrders(() => {
      clearTimeout(timer);
      timer = setTimeout(load, 500);
    });
    const every = setInterval(load, 15_000);
    return () => {
      stop();
      clearTimeout(timer);
      clearInterval(every);
    };
  }, [load]);

  if (error) return <p className="form-error">{error}</p>;
  if (!data) return <p className="loading">Loading…</p>;
  return (
    <>
      <div className="chart-toolbar">
        <span className="muted small">
          {user === undefined
            ? "The auto trader trades your paper account on market signals, with the broker's stricter auto limits. Your kill switch stops it. Runs are set up in config."
            : "This user's auto-trader runs (read-only). Their kill switch stops them; runs are set up in config."}
        </span>
        <div className="chart-intervals" role="group" aria-label="Period">
          {PERIODS.map((d) => (
            <button key={d} type="button" className={d === days ? 'active' : ''} onClick={() => setDays(d)}>
              {d} days
            </button>
          ))}
        </div>
      </div>
      {data.runs.length === 0 ? (
        <p className="muted">No auto-trader runs for your account.</p>
      ) : (
        data.runs.map((run) => <Run key={run.id} run={run} />)
      )}
    </>
  );
}
