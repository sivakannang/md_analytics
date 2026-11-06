// The instrument page's Signals tab for an index (docs/design/signals.md §8): the current
// signal and its factors, the day's score against the spot, the strategy's track record
// and its recent calls with what happened after them.

import { useEffect, useRef, useState } from 'react';
import { ColorType, LineSeries, LineStyle, createChart } from 'lightweight-charts';
import type { IChartApi, UTCTimestamp } from 'lightweight-charts';

import { api, errorMessage, isSessionLost } from '../api/client';
import type { SignalDay, SignalIndex, SignalRecord, SignalRow, SignalsInfo, TrackRecord } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { toChartTime } from '../chart/buckets';
import { useLiveSignal } from '../stream/useQuotes';
import { formatPrice } from '../watchlists/format';
import { SignalBadge, factorName, fromRow, hhmm, labelClass, labelText, signedScore } from './SignalBadge';

const RECORD_DAYS = [5, 20, 60] as const;
const RECENT_CALLS = 15;
/** The largest weight a factor has in oi-bias-v1: a full bar. */
const FULL_BAR_POINTS = 20;

function percent(rate: number | null): string {
  return rate === null ? '—' : `${(rate * 100).toFixed(1)}%`;
}

/** Each factor's share of the score, as a bar left (bearish) or right (bullish) of the middle. */
function FactorBars({ row }: { row: SignalRow }) {
  return (
    <ul className="factor-bars">
      {row.factors.map((f) => {
        const width = Math.min(50, (Math.abs(f.points) / FULL_BAR_POINTS) * 50);
        const side = f.points >= 0 ? 'right' : 'left';
        return (
          <li key={f.factor} className={f.status === 'present' ? '' : 'muted'}>
            <span className="factor-name">{factorName(f.factor)}</span>
            <span className="factor-track">
              {f.status === 'present' && (
                <span
                  className={`factor-fill ${f.points >= 0 ? 'up-bg' : 'down-bg'}`}
                  style={{ width: `${width}%`, [side === 'right' ? 'left' : 'right']: '50%' }}
                />
              )}
            </span>
            <span className="factor-points">
              {f.status === 'present' ? signedScore(Math.round(f.points)) : f.status === 'absent' ? 'no data' : 'n/a'}
            </span>
          </li>
        );
      })}
    </ul>
  );
}

/** The day's score (left scale, -100..100, threshold lines) and the spot (right scale). */
function ScoreChart({ rows, bullish, bearish }: { rows: SignalRow[]; bullish: number; bearish: number }) {
  const container = useRef<HTMLDivElement>(null);
  const chart = useRef<IChartApi | null>(null);

  useEffect(() => {
    const element = container.current;
    if (!element) return;
    const css = getComputedStyle(document.documentElement);
    const get = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
    const grid = get('--border', '#dfe3e8');
    const created = createChart(element, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: get('--surface', '#ffffff') },
        textColor: get('--muted', '#5f6b7a'),
        fontSize: 11,
      },
      grid: { vertLines: { color: grid }, horzLines: { color: grid } },
      timeScale: { timeVisible: true, secondsVisible: false, borderColor: grid },
      leftPriceScale: { visible: true, borderColor: grid },
      rightPriceScale: { visible: true, borderColor: grid },
      crosshair: { mode: 0 },
    });
    const score = created.addSeries(LineSeries, {
      color: get('--accent', '#1f6feb'),
      lineWidth: 2,
      priceScaleId: 'left',
      priceLineVisible: false,
      autoscaleInfoProvider: () => ({ priceRange: { minValue: -100, maxValue: 100 } }),
    });
    const line = (price: number, color: string, title: string) =>
      score.createPriceLine({ price, color, lineWidth: 1, lineStyle: LineStyle.Dashed, axisLabelVisible: true, title });
    line(bullish, get('--up', '#1b7f3b'), 'Bullish');
    line(bearish, get('--down', '#c62828'), 'Bearish');
    line(0, grid, '');
    const spot = created.addSeries(LineSeries, {
      color: get('--muted', '#5f6b7a'),
      lineWidth: 1,
      priceScaleId: 'right',
      priceLineVisible: false,
    });
    score.setData(rows.map((r) => ({ time: toChartTime(r.minute) as UTCTimestamp, value: r.score })));
    spot.setData(
      rows.filter((r) => r.spot !== null).map((r) => ({ time: toChartTime(r.minute) as UTCTimestamp, value: r.spot as number })),
    );
    created.timeScale().fitContent();
    chart.current = created;
    return () => {
      created.remove();
      chart.current = null;
    };
  }, [rows, bullish, bearish]);

  return <div className="signal-chart" ref={container} />;
}

function RecordTable({ record }: { record: TrackRecord }) {
  const decided = record.hits + record.misses;
  return (
    <table className="items signal-record">
      <tbody>
        <tr>
          <th>Hit rate</th>
          <td>
            <b>{percent(record.hit_rate)}</b>
            <span className="muted"> · {record.hits} of {decided} calls right</span>
          </td>
        </tr>
        <tr>
          <th title="Of the same moves, the share that went up: what always saying Bullish would score">Always up</th>
          <td>{percent(record.always_up_rate)}</td>
        </tr>
        <tr>
          <th title="Calling the direction of the previous horizon again">Momentum</th>
          <td>
            {percent(record.momentum_rate)}
            <span className="muted"> · {record.momentum_calls} calls</span>
          </td>
        </tr>
        <tr>
          <th>Calls</th>
          <td className="muted">
            {record.bullish} bullish · {record.bearish} bearish · {record.neutral} neutral · {record.flats} flat
          </td>
        </tr>
      </tbody>
    </table>
  );
}

function outcomeText(row: SignalRow): { text: string; className: string } {
  if (!row.outcome) return { text: 'pending', className: 'muted' };
  const bp = row.outcome.return_bp;
  const move = bp === null ? '—' : `${bp > 0 ? '+' : ''}${bp} bp`;
  if (row.outcome.hit === true) return { text: `✓ ${move}`, className: 'up' };
  if (row.outcome.hit === false) return { text: `✗ ${move}`, className: 'down' };
  return { text: `flat ${move}`, className: 'muted' };
}

export function SignalsView({ index, info }: { index: SignalIndex; info: SignalsInfo }) {
  const { sessionEnded } = useAuth();
  const strategy = info.strategies[0];
  const live = useLiveSignal(index.spot_id, strategy?.id ?? '');
  const [date, setDate] = useState<string | undefined>(undefined); // undefined: the latest day
  const [day, setDay] = useState<SignalDay | null>(null);
  const [days, setDays] = useState<number>(20);
  const [record, setRecord] = useState<SignalRecord | null>(null);
  const [error, setError] = useState<string | null>(null);
  // Following the latest day: re-read it when a new minute's signal arrives.
  const liveMinute = date === undefined ? (live?.minute ?? 0) : 0;

  useEffect(() => {
    let active = true;
    api
      .signalDay(index.underlying, date)
      .then((found) => active && setDay(found))
      .catch((e: unknown) => {
        if (!active) return;
        if (isSessionLost(e)) sessionEnded();
        else setError(errorMessage(e));
      });
    return () => {
      active = false;
    };
  }, [index.underlying, date, liveMinute, sessionEnded]);

  useEffect(() => {
    let active = true;
    api
      .signalRecord(index.underlying, days)
      .then((found) => active && setRecord(found))
      .catch(() => undefined);
    return () => {
      active = false;
    };
  }, [index.underlying, days, liveMinute]);

  if (!strategy) return <p className="muted">No strategies are configured.</p>;
  if (error) return <p className="form-error">{error}</p>;
  if (!day) return <p className="loading">Loading…</p>;

  const rows = day.signals.filter((r) => r.strategy === strategy.id);
  const latest = rows.at(-1);
  const current = latest && (!live || live.minute <= latest.minute) ? fromRow(latest) : live;
  const strategyRecord = record?.strategies.find((r) => r.strategy === strategy.id);
  const recent = rows
    .filter((r) => r.label !== 'neutral')
    .slice(-RECENT_CALLS)
    .reverse();

  return (
    <div className="signals-view">
      <section className="signal-now">
        <div>
          <SignalBadge signal={current} />
          <p className="muted">
            {strategy.id}: direction of {index.underlying} over the next {strategy.horizon_min} minutes
            {latest && ` · ${day.date} ${hhmm(latest.minute + 60)} IST`}. Bullish at {signedScore(strategy.bullish)} or
            more, bearish at {strategy.bearish} or less.
          </p>
        </div>
        <label className="signal-date">
          Day{' '}
          <input
            type="date"
            value={date ?? day.date ?? ''}
            onChange={(e) => setDate(e.target.value || undefined)}
          />
          {date !== undefined && (
            <button type="button" className="secondary" onClick={() => setDate(undefined)}>
              Latest
            </button>
          )}
        </label>
      </section>

      {rows.length === 0 ? (
        <p className="muted">
          No signals {day.date ? `on ${day.date}` : 'yet'}. mda-signald gives one a minute while the market is open.
        </p>
      ) : (
        <>
          <ScoreChart rows={rows} bullish={strategy.bullish} bearish={strategy.bearish} />
          <p className="muted chart-note">Score (left, −100 to +100) · {index.underlying} (right) · IST</p>
          {latest && (
            <section>
              <h3>Factors at {hhmm(latest.minute + 60)}</h3>
              <FactorBars row={latest} />
            </section>
          )}
        </>
      )}

      <section>
        <div className="chart-toolbar">
          <h3>Track record</h3>
          <div className="chart-intervals" role="group" aria-label="Days">
            {RECORD_DAYS.map((d) => (
              <button key={d} type="button" className={d === days ? 'active' : ''} onClick={() => setDays(d)}>
                {d} days
              </button>
            ))}
          </div>
        </div>
        {strategyRecord ? (
          <RecordTable record={strategyRecord} />
        ) : (
          <p className="muted">No settled signals in the last {days} days.</p>
        )}
        <p className="muted small">
          A call is right when the index moved its way by more than the flat band over the horizon. A strategy that
          doesn't beat both baselines has no edge; judge it over weeks, not days. Not investment advice.
        </p>
      </section>

      {recent.length > 0 && (
        <section>
          <h3>Recent calls</h3>
          <table className="items signal-calls">
            <thead>
              <tr>
                <th>Time</th>
                <th>Call</th>
                <th>{index.underlying}</th>
                <th>After {strategy.horizon_min}m</th>
                <th>Result</th>
              </tr>
            </thead>
            <tbody>
              {recent.map((r) => {
                const outcome = outcomeText(r);
                return (
                  <tr key={r.minute}>
                    <td>{hhmm(r.minute + 60)}</td>
                    <td className={labelClass(r.label)}>
                      {labelText(r.label)} {signedScore(r.score)}
                    </td>
                    <td>{formatPrice(r.spot ?? undefined)}</td>
                    <td>{formatPrice(r.outcome?.spot_after ?? undefined)}</td>
                    <td className={outcome.className}>{outcome.text}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </section>
      )}
    </div>
  );
}
