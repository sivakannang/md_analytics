// OI over time (docs/design/multistrike-oi.md, MS1): a few chosen contracts' OI as lines
// through the session (left scale) with the spot (right scale). Data: the session's step
// series from GET /chains/{u}/oi-timeline with one expiry ticked, i.e. per contract; the
// "most active" list and every line are computed here.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ColorType, LineSeries, LineStyle, createChart } from 'lightweight-charts';
import type { IChartApi, ISeriesApi, MouseEventParams, UTCTimestamp } from 'lightweight-charts';

import { api, errorMessage, isSessionLost } from '../api/client';
import type { OiTimeline, StepSeries } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { toChartTime } from '../chart/buckets';
import { valueAt } from '../chart/steps';
import { formatExpiry, formatLakhs, formatPrice } from '../watchlists/format';

const INTERVALS = [1, 5, 15] as const;
const MAX_LINES = 8;
const ACTIVE_PER_SIDE = 3;
/** Distinct in light and dark; a contract keeps its colour while it stays selected. */
const COLORS = ['#3b82f6', '#f59e0b', '#14b8a6', '#e11d48', '#8b5cf6', '#84cc16', '#ec4899', '#64748b'];
const REFRESH_DELAY_MS = 2_000;

type Side = 'call' | 'put';
interface Contract {
  strike: number;
  side: Side;
}
const keyOf = (c: Contract) => `${c.strike}-${c.side}`;
const labelOf = (c: Contract) => `${c.strike} ${c.side === 'call' ? 'CE' : 'PE'}`;

const istTime = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  hour12: false,
});
const hhmm = (t: number) => istTime.format(t * 1000);

function signedLakhs(value: number | null): string {
  if (value === null) return '—';
  return (value > 0 ? '+' : value < 0 ? '−' : '') + formatLakhs(Math.abs(value));
}

function seriesOf(timeline: OiTimeline, c: Contract): StepSeries {
  return timeline.rows.find((r) => r.strike === c.strike)?.[c.side] ?? [];
}

/** OI change since the open: last value minus the value before the open (or the first one). */
function changeOf(series: StepSeries, minute: number): number | null {
  const last = valueAt(series, minute);
  const first = valueAt(series, 0) ?? series[0]?.[1] ?? null;
  return last === null || first === null ? null : last - first;
}

/** The 3 calls and 3 puts with the largest OI change since the open (either way). */
function mostActive(timeline: OiTimeline): Contract[] {
  const minute = (timeline.latest - timeline.session_open) / 60;
  const pick = (side: Side) =>
    timeline.rows
      .map((r) => ({ c: { strike: r.strike, side }, change: changeOf(r[side], minute) }))
      .filter((x) => x.change !== null)
      .sort((a, b) => Math.abs(b.change!) - Math.abs(a.change!))
      .slice(0, ACTIVE_PER_SIDE)
      .map((x) => x.c);
  return [...pick('call'), ...pick('put')];
}

interface Props {
  underlying: string;
  expiries: string[];
  initialExpiry: string;
}

export function OiOverTime({ underlying, expiries, initialExpiry }: Props) {
  const { sessionEnded } = useAuth();
  const [expiry, setExpiry] = useState(initialExpiry);
  const [interval, setInterval_] = useState<number>(5);
  const [showSpot, setShowSpot] = useState(true);
  const [timeline, setTimeline] = useState<OiTimeline | null>(null);
  const [error, setError] = useState<string | null>(null);
  // Selected contracts in order (their index is their colour); null = not chosen yet.
  const [selected, setSelected] = useState<Contract[] | null>(null);
  const [hovered, setHovered] = useState<number | null>(null);  // Unix s under the cursor
  const [tick, setTick] = useState(0);

  useEffect(() => {
    setExpiry(initialExpiry);
  }, [underlying, initialExpiry]);
  useEffect(() => {
    setSelected(null);
    setTimeline(null);
  }, [underlying, expiry]);

  useEffect(() => {
    let active = true;
    api
      .oiTimeline(underlying, { expiries: [expiry], strikes: 20 })
      .then((found) => {
        if (!active) return;
        setError(null);
        setTimeline(found);
      })
      .catch((e: unknown) => {
        if (!active) return;
        if (isSessionLost(e)) sessionEnded();
        else setError(errorMessage(e));
      });
    return () => {
      active = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [underlying, expiry, tick]);

  // Reload 2 s after each minute closes (the session's series grow by a minute).
  useEffect(() => {
    const now = Date.now();
    const next = (Math.floor(now / 60_000) + 1) * 60_000 + REFRESH_DELAY_MS;
    const timer = setTimeout(() => setTick((n) => n + 1), next - now);
    return () => clearTimeout(timer);
  }, [tick]);

  const active = useMemo(() => (timeline ? mostActive(timeline) : []), [timeline]);
  // First load of an expiry: tick the most active call and put.
  useEffect(() => {
    if (!timeline || selected !== null) return;
    const call = active.find((c) => c.side === 'call');
    const put = active.find((c) => c.side === 'put');
    setSelected([call, put].filter((c): c is Contract => c !== undefined));
  }, [timeline, active, selected]);

  const chosen = useMemo(() => selected ?? [], [selected]);
  const isChosen = (c: Contract) => chosen.some((s) => keyOf(s) === keyOf(c));
  const toggle = (c: Contract) =>
    setSelected((current) => {
      const list = current ?? [];
      if (list.some((s) => keyOf(s) === keyOf(c))) return list.filter((s) => keyOf(s) !== keyOf(c));
      return list.length >= MAX_LINES ? list : [...list, c];
    });
  // The list: the most active, then any other chosen contract.
  const listed = [...active, ...chosen.filter((c) => !active.some((a) => keyOf(a) === keyOf(c)))];
  const minuteNow = timeline ? (timeline.latest - timeline.session_open) / 60 : 0;
  // Stable while the selection is, so hovering (a re-render) doesn't rebuild the lines.
  const colorOf = useCallback(
    (c: Contract) => {
      const index = chosen.findIndex((s) => keyOf(s) === keyOf(c));
      return index >= 0 ? COLORS[index % COLORS.length]! : undefined;
    },
    [chosen],
  );

  // The legend's time: the hovered one, else the latest minute.
  const legendTime = hovered ?? timeline?.latest ?? 0;
  const legendMinute = timeline ? Math.floor((legendTime - timeline.session_open) / 60) : 0;

  return (
    <div className="oot">
      <div className="chain-toolbar">
        <div className="chart-intervals" role="group" aria-label="Expiry">
          {expiries.map((e) => (
            <button key={e} type="button" className={e === expiry ? 'active' : ''} onClick={() => setExpiry(e)}>
              {formatExpiry(e)}
            </button>
          ))}
        </div>
        <div className="chart-intervals" role="group" aria-label="Interval">
          {INTERVALS.map((m) => (
            <button
              key={m}
              type="button"
              className={interval === m ? 'active' : ''}
              aria-pressed={interval === m}
              onClick={() => setInterval_(m)}
            >
              {m}m
            </button>
          ))}
        </div>
        <label className="obs-check">
          <input type="checkbox" checked={showSpot} onChange={(e) => setShowSpot(e.target.checked)} />
          Spot
        </label>
        {timeline?.oi_as_of != null && (
          <span className="chart-oi-asof muted">
            <i className="obs-live" title="Refreshes after each minute" />
            OI as of {hhmm(timeline.oi_as_of)}
          </span>
        )}
      </div>

      {error ? (
        <p className="form-error">{error}</p>
      ) : !timeline ? (
        <p className="loading">Loading…</p>
      ) : (
        <div className="oot-body">
          <section className="card oot-list" aria-label="Contracts">
            <h3>Most active today</h3>
            <ul>
              {listed.map((c, i) => {
                const series = seriesOf(timeline, c);
                const color = colorOf(c);
                return (
                  <li key={keyOf(c)} className={i === active.length ? 'oot-added' : ''}>
                    <label className="obs-check">
                      <input
                        type="checkbox"
                        checked={isChosen(c)}
                        disabled={!isChosen(c) && chosen.length >= MAX_LINES}
                        onChange={() => toggle(c)}
                      />
                      <i
                        className={`oot-swatch ${c.side}`}
                        style={color ? { borderColor: color, background: c.side === 'call' ? color : undefined } : undefined}
                      />
                      <span className="oot-name">{labelOf(c)}</span>
                    </label>
                    <span className="oot-change">{signedLakhs(changeOf(series, minuteNow))}</span>
                  </li>
                );
              })}
            </ul>
            <label className="chain-strikes oot-add">
              Add
              <select
                value=""
                disabled={chosen.length >= MAX_LINES}
                onChange={(e) => {
                  const [strike, side] = e.target.value.split('-');
                  if (strike && side) toggle({ strike: Number(strike), side: side as Side });
                }}
              >
                <option value="">strike…</option>
                {timeline.rows.flatMap((r) =>
                  (['call', 'put'] as const)
                    .map((side) => ({ strike: r.strike, side }))
                    .filter((c) => !isChosen(c) && seriesOf(timeline, c).length > 0)
                    .map((c) => (
                      <option key={keyOf(c)} value={keyOf(c)}>
                        {labelOf(c)}
                      </option>
                    )),
                )}
              </select>
            </label>
            <p className="muted oot-note">Calls solid, puts dashed · up to {MAX_LINES} lines</p>
          </section>

          <section className="card wide oot-chart-card">
            <div className="chart-legend" aria-live="off">
              <span className="muted">{hhmm(legendTime)}</span>
              {chosen.map((c) => {
                const series = seriesOf(timeline, c);
                return (
                  <span key={keyOf(c)}>
                    <i className={`oot-swatch ${c.side}`} style={{ borderColor: colorOf(c), background: c.side === 'call' ? colorOf(c) : undefined }} />
                    {labelOf(c)} <b>{formatLakhs(valueAt(series, legendMinute) ?? undefined)}</b> (
                    {signedLakhs(changeOf(series, legendMinute))})
                  </span>
                );
              })}
              {showSpot && (
                <span>
                  {underlying} <b>{formatPrice(valueAt(timeline.spot, legendMinute) ?? undefined)}</b>
                </span>
              )}
            </div>
            <Lines
              timeline={timeline}
              contracts={chosen}
              colorOf={colorOf}
              interval={interval}
              showSpot={showSpot}
              fitKey={`${underlying} ${expiry} ${interval}`}
              onHover={setHovered}
            />
          </section>
        </div>
      )}
    </div>
  );
}

interface LinesProps {
  timeline: OiTimeline;
  contracts: Contract[];
  colorOf: (c: Contract) => string | undefined;
  interval: number; // minutes
  showSpot: boolean;
  fitKey: string; // fit the time axis when this changes, not on each minute's refresh
  onHover: (time: number | null) => void;
}

/** The chart: created once; its series are rebuilt when the data or the selection changes. */
function Lines({ timeline, contracts, colorOf, interval, showSpot, fitKey, onHover }: LinesProps) {
  const container = useRef<HTMLDivElement>(null);
  const fitted = useRef('');
  const chart = useRef<IChartApi | null>(null);
  const lines = useRef<ISeriesApi<'Line'>[]>([]);

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
    const onCrosshair = (param: MouseEventParams) => {
      // Chart times are shifted to show IST; undo it for the legend.
      onHover(param.time === undefined ? null : Number(param.time) - toChartTime(0));
    };
    created.subscribeCrosshairMove(onCrosshair);
    chart.current = created;
    return () => {
      created.unsubscribeCrosshairMove(onCrosshair);
      created.remove();
      chart.current = null;
      lines.current = [];
    };
  }, [onHover]);

  useEffect(() => {
    const c = chart.current;
    if (!c) return;
    for (const line of lines.current) c.removeSeries(line);
    lines.current = [];
    // Sample each step series at the end of every `interval` minutes (and the open).
    const minutes: number[] = [];
    const last = (timeline.latest - timeline.session_open) / 60;
    for (let m = 0; m <= last; m += interval) minutes.push(m);
    if (minutes[minutes.length - 1] !== last) minutes.push(last);
    const points = (series: StepSeries) =>
      minutes.flatMap((m) => {
        const value = valueAt(series, m);
        return value === null ? [] : [{ time: toChartTime(timeline.session_open + m * 60) as UTCTimestamp, value }];
      });
    const muted = getComputedStyle(document.documentElement).getPropertyValue('--muted').trim() || '#5f6b7a';
    if (showSpot) {
      const spot = c.addSeries(LineSeries, {
        color: muted,
        lineWidth: 1,
        priceScaleId: 'right',
        priceLineVisible: false,
        lastValueVisible: true,
        crosshairMarkerVisible: false,
      });
      spot.setData(points(timeline.spot));
      lines.current.push(spot);
    }
    for (const contract of contracts) {
      const line = c.addSeries(LineSeries, {
        color: colorOf(contract),
        lineWidth: 2,
        lineStyle: contract.side === 'put' ? LineStyle.Dashed : LineStyle.Solid,
        priceScaleId: 'left',
        priceFormat: { type: 'custom', formatter: (v: number) => formatLakhs(v).replace(/\.00(?=L|Cr)/, ''), minMove: 1 },
        priceLineVisible: false,
        lastValueVisible: true,
      });
      line.setData(points(seriesOf(timeline, contract)));
      lines.current.push(line);
    }
    if (fitted.current !== fitKey) {
      fitted.current = fitKey;
      c.timeScale().fitContent();
    }
  }, [timeline, contracts, colorOf, interval, showSpot, fitKey]);

  return <div ref={container} className="oot-chart" />;
}
