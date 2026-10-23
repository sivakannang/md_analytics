// OI by strike (docs/design/oi-change-by-strike.md §2, OC2): call and put OI per strike at
// two times. The view loads the session's OI as step series (GET /chains/{u}/oi-timeline) and
// computes each window itself, so the bars follow the two-handle time slider while it is
// dragged (OC3, OC5: design §8). Per strike a put bar (green) and a call bar
// (red); with "Show OI" the solid part is the OI at the start, striped the increase on top,
// a hollow outline the part removed; without it, only the change from a zero line. Drawn in
// SVG: lightweight-charts has no category (strike) axis.

import { useEffect, useId, useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { MouseEvent } from 'react';

import { api, errorMessage, isSessionLost } from '../api/client';
import type { OiChange, OiChangeRow, OiTimeline } from '../api/client';
import { valueAt } from '../chart/steps';
import { useAuth } from '../auth/AuthContext';
import { formatExpiry, formatLakhs, formatPrice } from '../watchlists/format';

const STRIKE_CHOICES = [5, 10, 15, 20] as const;
/** Presets: minutes back from the latest minute; 0 = since the open (Sensibull's "Full day"). */
const PRESETS = [
  { label: '5m', minutes: 5 },
  { label: '15m', minutes: 15 },
  { label: '30m', minutes: 30 },
  { label: '1h', minutes: 60 },
  { label: '2h', minutes: 120 },
  { label: '3h', minutes: 180 },
  { label: 'Full day', minutes: 0 },
] as const;

const HEIGHT = 380;
const MARGIN = { top: 28, right: 12, bottom: 44, left: 56 };

const istTime = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  hour12: false,
});
const hhmm = (t: number) => istTime.format(t * 1000);

type Side = 'put' | 'call';

/**
 * The time window: a preset (minutes back from the latest minute; 0 = since the open), or a
 * slider range. With a preset, or a range whose end is at the latest minute (`live`), the
 * end follows the market: the view refreshes after each minute closes.
 */
type Range = { preset: number } | { from: number; to: number; live: boolean };

/** Replay speeds, in session minutes per second of playback (a full day at 5 min/s: 75 s). */
const REPLAY_SPEEDS = [5, 15, 30] as const;
const REPLAY_TICK_MS = 100;

/** After a minute closes, give the engine time to write the bar (as the price chart does). */
const REFRESH_DELAY_MS = 2_000;

/**
 * The window [from, to] of a timeline, in the shape of GET /oi-change (same rules: totals over
 * sides known at both times; PCR = put / call OI at `to`).
 */
function windowOf(t: OiTimeline, from: number, to: number): OiChange {
  const mFrom = Math.floor((from - t.session_open) / 60);
  const mTo = Math.floor((to - t.session_open) / 60);
  const totals = { call_from: 0, call_to: 0, put_from: 0, put_to: 0 };
  const rows: OiChangeRow[] = t.rows.map((r) => {
    const row = {
      strike: r.strike,
      call_from: valueAt(r.call, mFrom),
      call_to: valueAt(r.call, mTo),
      put_from: valueAt(r.put, mFrom),
      put_to: valueAt(r.put, mTo),
    };
    if (row.call_from !== null && row.call_to !== null) {
      totals.call_from += row.call_from;
      totals.call_to += row.call_to;
    }
    if (row.put_from !== null && row.put_to !== null) {
      totals.put_from += row.put_from;
      totals.put_to += row.put_to;
    }
    return row;
  });
  return {
    underlying: t.underlying,
    expiries: t.expiries,
    from,
    to,
    session_open: t.session_open,
    latest: t.latest,
    oi_as_of: to >= t.latest ? t.oi_as_of : null,
    spot_from: valueAt(t.spot, mFrom),
    spot_to: valueAt(t.spot, mTo) ?? 0,
    atm_strike: t.atm_strike,
    totals,
    pcr: totals.call_to > 0 ? Math.round((totals.put_to / totals.call_to) * 100) / 100 : null,
    rows,
  };
}

/** Round tick steps (1, 2, 5 × 10^n) for about `count` ticks over [low, high]. */
function ticks(low: number, high: number, count = 5): number[] {
  const span = high - low || 1;
  const raw = span / count;
  const power = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 5, 10].map((m) => m * power).find((s) => s >= raw) ?? raw;
  const out: number[] = [];
  for (let v = Math.ceil(low / step) * step; v <= high + step / 1e6; v += step) out.push(Math.round(v));
  return out;
}

function change(row: OiChangeRow, side: Side): number | null {
  const from = row[`${side}_from`];
  const to = row[`${side}_to`];
  return from === null || to === null ? null : to - from;
}

function signedLakhs(value: number | null): string {
  if (value === null) return '—';
  return (value > 0 ? '+' : value < 0 ? '−' : '') + formatLakhs(Math.abs(value));
}

interface Props {
  underlying: string;
  expiries: string[]; // the chain's listed expiries
  initialExpiry: string;
}

export function OiByStrike({ underlying, expiries, initialExpiry }: Props) {
  const { sessionEnded } = useAuth();
  const [ticked, setTicked] = useState<string[]>([initialExpiry]);
  const [strikes, setStrikes] = useState<number>(10);
  const [range, setRange] = useState<Range>({ preset: 0 });
  const [showOi, setShowOi] = useState(true);
  const [timeline, setTimeline] = useState<OiTimeline | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [hovered, setHovered] = useState<number | null>(null);
  const [tick, setTick] = useState(0);  // bumps after each minute close while live
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState<number>(REPLAY_SPEEDS[0]);

  // A new underlying starts from its nearest expiry; a slider range belongs to the old one.
  useEffect(() => {
    setTicked([initialExpiry]);
    setRange((r) => ('preset' in r ? r : { preset: 0 }));
  }, [underlying, initialExpiry]);

  const live = 'preset' in range || range.live;

  // The session's series; reloaded when the selection changes, and each minute while live.
  useEffect(() => {
    let active = true;
    api
      .oiTimeline(underlying, { expiries: ticked, strikes })
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
  }, [underlying, ticked.join(','), strikes, tick]);

  // The window: computed here, so a slider move redraws at once without a request.
  const data = useMemo(() => {
    if (!timeline) return null;
    const { session_open: open, latest } = timeline;
    if ('preset' in range) {
      const from = range.preset > 0 ? Math.max(open, latest - range.preset * 60) : open;
      return windowOf(timeline, from, latest);
    }
    const to = range.live ? latest : Math.min(range.to, latest);
    return windowOf(timeline, Math.min(range.from, to - 60), to);
  }, [timeline, range]);

  // Replay: the end handle moves forward from the start; at the latest minute it goes live.
  const latest = timeline?.latest;
  useEffect(() => {
    if (!playing || latest === undefined) return;
    const step = (speed * 60 * REPLAY_TICK_MS) / 1000;  // seconds of session per tick
    const timer = setInterval(() => {
      setRange((r) => {
        if ('preset' in r) return r;
        const to = r.to + step;
        if (to < latest) return { ...r, to };
        setPlaying(false);
        return { ...r, to: latest, live: true };
      });
    }, REPLAY_TICK_MS);
    return () => clearInterval(timer);
  }, [playing, speed, latest]);

  const replay = () => {
    if (playing) {
      setPlaying(false);
      return;
    }
    if (!data) return;
    // From the start handle; resume where the end handle is unless it is at the latest minute.
    const to = data.to >= data.latest ? data.from + 60 : data.to;
    setRange({ from: data.from, to, live: false });
    setPlaying(true);
  };

  // While live, refresh 2 s after each minute closes.
  useEffect(() => {
    if (!live) return;
    const now = Date.now();
    const next = (Math.floor(now / 60_000) + 1) * 60_000 + REFRESH_DELAY_MS;
    const timer = setTimeout(() => setTick((n) => n + 1), next - now);
    return () => clearTimeout(timer);
  }, [live, tick]);

  const toggleExpiry = (expiry: string) => {
    setTicked((current) => {
      const next = current.includes(expiry) ? current.filter((e) => e !== expiry) : [...current, expiry];
      return next.length === 0 ? current : expiries.filter((e) => next.includes(e));
    });
  };

  const atmIndex = data ? data.rows.findIndex((r) => r.strike === data.atm_strike) : -1;
  const shownRow = data ? data.rows[hovered ?? atmIndex] : undefined;
  const callChange = data ? data.totals.call_to - data.totals.call_from : null;
  const putChange = data ? data.totals.put_to - data.totals.put_from : null;

  return (
    <div className="obs">
      <div className="chain-toolbar obs-controls">
        <div className="obs-expiries" role="group" aria-label="Expiries included">
          {expiries.map((e) => (
            <label key={e} className="obs-check">
              <input type="checkbox" checked={ticked.includes(e)} onChange={() => toggleExpiry(e)} />
              {formatExpiry(e)}
            </label>
          ))}
        </div>
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
        <div className="chart-intervals" role="group" aria-label="Time range">
          {PRESETS.map((p) => (
            <button
              key={p.label}
              type="button"
              className={'preset' in range && range.preset === p.minutes ? 'active' : ''}
              aria-pressed={'preset' in range && range.preset === p.minutes}
              onClick={() => {
                setPlaying(false);
                setRange({ preset: p.minutes });
              }}
            >
              {p.label}
            </button>
          ))}
        </div>
        <label className="obs-check">
          <input type="checkbox" checked={showOi} onChange={(e) => setShowOi(e.target.checked)} />
          Show OI
        </label>
        {data?.oi_as_of != null && (
          <span className="chart-oi-asof muted">
            {live && <i className="obs-live" title="Refreshes after each minute" />}
            OI as of {hhmm(data.oi_as_of)}
          </span>
        )}
      </div>

      <section className="card wide obs-card">
        {error ? (
          <p className="form-error">{error}</p>
        ) : !data ? (
          <p className="loading">Loading…</p>
        ) : (
          <>
            <div className="chart-legend obs-legend" aria-live="off">
              {shownRow && (
                <>
                  <span>
                    Strike <b>{shownRow.strike}</b>
                  </span>
                  <span className="obs-put-text">
                    Put OI <b>{formatLakhs(shownRow.put_to ?? undefined)}</b> ({signedLakhs(change(shownRow, 'put'))})
                  </span>
                  <span className="obs-call-text">
                    Call OI <b>{formatLakhs(shownRow.call_to ?? undefined)}</b> ({signedLakhs(change(shownRow, 'call'))})
                  </span>
                </>
              )}
              <span className="muted">
                {hhmm(data.from)} → {hhmm(data.to)}
              </span>
            </div>
            <StrikeBars data={data} showOi={showOi} hovered={hovered} onHover={setHovered} />
            <div className="obs-replay">
              <button
                type="button"
                className={`obs-play ${playing ? 'active' : ''}`}
                aria-pressed={playing}
                title={playing ? 'Pause' : 'Replay from the start handle'}
                onClick={replay}
              >
                {playing ? '❚❚ Pause' : '▶ Replay'}
              </button>
              <div className="chart-intervals" role="group" aria-label="Replay speed">
                {REPLAY_SPEEDS.map((s) => (
                  <button
                    key={s}
                    type="button"
                    className={speed === s ? 'active' : ''}
                    aria-pressed={speed === s}
                    title={`${s} minutes of the session per second`}
                    onClick={() => setSpeed(s)}
                  >
                    {s}m/s
                  </button>
                ))}
              </div>
              <TimeSlider
                min={data.session_open}
                max={data.latest}
                from={data.from}
                to={data.to}
                onChange={(from, to) => {
                  setPlaying(false);
                  setRange({ from, to, live: to >= data.latest });
                }}
              />
            </div>
            <ul className="obs-key" aria-label="Colours">
              <li className="obs-put-text">
                <i className="solid" /> Put OI <i className="striped" /> Increase <i className="hollow" /> Decrease
              </li>
              <li className="obs-call-text">
                <i className="solid" /> Call OI <i className="striped" /> Increase <i className="hollow" /> Decrease
              </li>
              <li className="muted">
                <i className="nodata" /> no data at {hhmm(data.from)}
              </li>
            </ul>
          </>
        )}
      </section>

      {data && (
        <footer className="chain-summary" aria-label="OI change summary">
          <span>
            {hhmm(data.from)} → {hhmm(data.to)}
          </span>
          <span className="obs-call-text">
            Call OI <b>{signedLakhs(callChange)}</b>
          </span>
          <span className="obs-put-text">
            Put OI <b>{signedLakhs(putChange)}</b>
          </span>
          <span title="Put OI / call OI at the end time, over the strikes shown">
            PCR <b>{data.pcr?.toFixed(2) ?? '—'}</b>
          </span>
          <span>
            {underlying} <b>{formatPrice(data.spot_from ?? undefined)}</b> → <b>{formatPrice(data.spot_to)}</b>
          </span>
        </footer>
      )}
    </div>
  );
}

interface SliderProps {
  min: number; // session open (Unix s)
  max: number; // latest minute
  from: number;
  to: number;
  onChange: (from: number, to: number) => void;
}

/**
 * Two handles over the session's minutes (two overlaid range inputs, so keyboard and touch
 * work as usual). Every move redraws the bars: the window is computed in the browser.
 */
function TimeSlider({ min, max, from, to, onChange }: SliderProps) {
  const span = Math.max(60, max - min);
  const percent = (t: number) => ((Math.min(max, Math.max(min, t)) - min) / span) * 100;
  const input = (which: 'from' | 'to') => (
    <input
      type="range"
      min={min}
      max={max}
      step={60}
      value={which === 'from' ? from : to}
      aria-label={which === 'from' ? 'Start time' : 'End time'}
      aria-valuetext={hhmm(which === 'from' ? from : to)}
      onChange={(e) => {
        const value = Number(e.target.value);
        // Keep at least a minute between the handles.
        if (which === 'from') onChange(Math.min(value, to - 60), to);
        else onChange(from, Math.max(value, from + 60));
      }}
    />
  );
  return (
    <div className="obs-slider">
      <span className="muted">{hhmm(min)}</span>
      <div className="obs-slider-track">
        <div className="obs-slider-fill" style={{ left: `${percent(from)}%`, width: `${percent(to) - percent(from)}%` }} />
        {input('from')}
        {input('to')}
      </div>
      <span className="muted">{hhmm(max)}</span>
      <span className="obs-slider-value">
        {hhmm(from)} → {hhmm(to)}
      </span>
    </div>
  );
}

interface BarsProps {
  data: OiChange;
  showOi: boolean;
  hovered: number | null;
  onHover: (index: number | null) => void;
}

function StrikeBars({ data, showOi, hovered, onHover }: BarsProps) {
  const box = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(800);
  const id = useId().replace(/:/g, '');

  useLayoutEffect(() => {
    const element = box.current;
    if (!element) return;
    const observer = new ResizeObserver(([entry]) => entry && setWidth(entry.contentRect.width));
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const rows = data.rows;
  const plotW = Math.max(100, width - MARGIN.left - MARGIN.right);
  const plotH = HEIGHT - MARGIN.top - MARGIN.bottom;
  const slot = plotW / Math.max(1, rows.length);
  const barW = Math.max(3, Math.min(16, slot * 0.36));

  // The y range: OI levels from 0 with "Show OI", else the changes around a zero line.
  const [low, high] = useMemo(() => {
    let lo = 0;
    let hi = 0;
    for (const row of rows) {
      for (const side of ['put', 'call'] as const) {
        if (showOi) {
          hi = Math.max(hi, row[`${side}_from`] ?? 0, row[`${side}_to`] ?? 0);
        } else {
          const c = change(row, side) ?? 0;
          lo = Math.min(lo, c);
          hi = Math.max(hi, c);
        }
      }
    }
    return [lo, hi === lo ? lo + 1 : hi];
  }, [rows, showOi]);
  const yTicks = ticks(low, high);
  const yLow = Math.min(low, yTicks[0] ?? low);
  const yHigh = Math.max(high, yTicks[yTicks.length - 1] ?? high);
  const y = (v: number) => MARGIN.top + plotH - ((v - yLow) / (yHigh - yLow)) * plotH;

  // Spot between strikes, by value.
  const strikeX = (i: number) => MARGIN.left + slot * (i + 0.5);
  let spotX: number | null = null;
  for (let i = 0; i + 1 < rows.length; i++) {
    const a = rows[i]!.strike;
    const b = rows[i + 1]!.strike;
    if (data.spot_to >= a && data.spot_to <= b) {
      spotX = strikeX(i) + ((data.spot_to - a) / (b - a)) * slot;
    }
  }
  const labelEvery = Math.max(1, Math.ceil(44 / slot));

  const bar = (row: OiChangeRow, side: Side, x: number) => {
    const from = row[`${side}_from`];
    const to = row[`${side}_to`];
    const cls = `obs-${side}`;
    if (to === null) return null;
    if (from === null) {
      // Subscribed later in the day: no start value, shown greyed and left out of totals.
      return showOi ? (
        <rect className="obs-nodata" x={x} y={y(to)} width={barW} height={y(0) - y(to)} />
      ) : null;
    }
    if (!showOi) {
      const c = to - from;
      const top = y(Math.max(c, 0));
      const h = Math.abs(y(c) - y(0));
      return c >= 0 ? (
        <rect className={cls} x={x} y={top} width={barW} height={h} style={{ fill: `url(#${id}-${side})` }} />
      ) : (
        <rect className={`${cls} hollow`} x={x + 0.5} y={top} width={barW - 1} height={h} />
      );
    }
    const base = Math.min(from, to);
    return (
      <>
        <rect className={cls} x={x} y={y(base)} width={barW} height={y(0) - y(base)} />
        {to > from && (
          <rect className={cls} x={x} y={y(to)} width={barW} height={y(from) - y(to)} style={{ fill: `url(#${id}-${side})` }} />
        )}
        {from > to && <rect className={`${cls} hollow`} x={x + 0.5} y={y(from)} width={barW - 1} height={y(to) - y(from)} />}
      </>
    );
  };

  const onMove = (event: MouseEvent<SVGSVGElement>) => {
    const rect = event.currentTarget.getBoundingClientRect();
    const index = Math.floor((event.clientX - rect.left - MARGIN.left) / slot);
    onHover(index >= 0 && index < rows.length ? index : null);
  };

  return (
    <div ref={box} className="obs-chart">
      <svg width={width} height={HEIGHT} role="img" aria-label="OI by strike" onMouseMove={onMove} onMouseLeave={() => onHover(null)}>
        <defs>
          {(['put', 'call'] as const).map((side) => (
            <pattern key={side} id={`${id}-${side}`} width="5" height="5" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
              <rect className={`obs-${side} obs-stripe-bg`} width="5" height="5" />
              <line className={`obs-${side}-stroke`} x1="0" y1="0" x2="0" y2="5" strokeWidth="3" />
            </pattern>
          ))}
        </defs>
        {yTicks.map((t) => (
          <g key={t}>
            <line className="obs-grid" x1={MARGIN.left} x2={MARGIN.left + plotW} y1={y(t)} y2={y(t)} />
            <text className="obs-axis" x={MARGIN.left - 6} y={y(t) + 4} textAnchor="end">
              {t < 0 ? '−' : ''}
              {formatLakhs(Math.abs(t)).replace(/\.00(?=L|Cr)/, '')}
            </text>
          </g>
        ))}
        <line className="obs-zero" x1={MARGIN.left} x2={MARGIN.left + plotW} y1={y(0)} y2={y(0)} />
        {hovered !== null && (
          <rect className="obs-hover" x={MARGIN.left + slot * hovered} y={MARGIN.top} width={slot} height={plotH} />
        )}
        {rows.map((row, i) => {
          const centre = strikeX(i);
          return (
            <g key={row.strike}>
              {bar(row, 'put', centre - barW - 1)}
              {bar(row, 'call', centre + 1)}
              {i % labelEvery === 0 && (
                <text
                  className={`obs-axis ${row.strike === data.atm_strike ? 'obs-atm' : ''}`}
                  x={centre}
                  y={MARGIN.top + plotH + 16}
                  textAnchor="middle"
                >
                  {row.strike}
                </text>
              )}
            </g>
          );
        })}
        {spotX !== null && (
          <g>
            <line className="obs-spot" x1={spotX} x2={spotX} y1={MARGIN.top} y2={MARGIN.top + plotH} />
            <text className="obs-spot-label" x={spotX} y={MARGIN.top - 8} textAnchor="middle">
              {data.underlying} {formatPrice(data.spot_to)}
            </text>
          </g>
        )}
      </svg>
    </div>
  );
}
