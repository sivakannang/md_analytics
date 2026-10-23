// Price chart for one instrument, chart first (docs/design/instrument-page.md): candles with
// volume drawn over the bottom of the price pane, and for futures/options a ΔOI pane below on
// the same time axis (bars coloured by buildup state, plus the OI level as a line). An
// always-visible legend, interval buttons, range presets, and an OI pane control
// (collapse · normal · maximise) remembered per browser. (ADR-0021, oi-buildup-chart.md.)
//
// History comes from GET /instruments/{id}/bars; live 1-minute bars from the stream's "bars"
// mode. The current candle is rebuilt from its minutes (fetched as 1m history, then live),
// so volume is never double-counted. Buildup states and ΔOI are the gateway's: when a
// minute closes, the last two candles are re-read so the one that closed gets its final
// values and the current one provisional ones. Times are shown in IST.

import { useEffect, useRef, useState } from 'react';
import { CandlestickSeries, ColorType, HistogramSeries, LineSeries, createChart } from 'lightweight-charts';
import type { IChartApi, ISeriesApi, MouseEventParams, UTCTimestamp } from 'lightweight-charts';

import { api, errorMessage } from '../api/client';
import type { BarInterval, BarRow, Buildup, Instrument } from '../api/client';
import { quoteStream } from '../stream/quoteStream';
import { formatPrice, formatQuantity } from '../watchlists/format';
import { BUILDUP_LABEL, BUILDUP_ORDER, buildupChange, buildupColors, oiChangeBar, oiRange, reference } from './buildup';
import { INTERVALS, bucketStart, liveRow, mergeRows, toChartTime } from './buckets';

const INTERVAL_KEY = 'mda.chart.interval';
const OI_PANE_KEY = 'mda.chart.oiPane';
const VISIBLE_BARS = 150;
/** The engine writes closed minutes to bars.db once a second; re-read after that. */
const REFRESH_DELAY_MS = 2_000;

type OiPane = 'collapsed' | 'normal' | 'max';
/** Price : OI pane heights per mode. */
const OI_STRETCH: Record<OiPane, [number, number]> = { collapsed: [24, 1], normal: [3, 2], max: [1, 3] };

/** Range presets: an interval, how far back to fetch, and how many bars to show. */
const PRESETS = [
  { label: '1D', interval: '1m', days: 4, bars: 375 },
  { label: '5D', interval: '5m', days: 9, bars: 375 },
  { label: '1M', interval: '30m', days: 35, bars: 286 },
  { label: '3M', interval: '1h', days: 95, bars: 455 },
  { label: '6M', interval: '1d', days: 190, bars: 125 },
  { label: 'All', interval: '1d', days: 400, bars: 0 },  // 0 = fit everything
] as const satisfies readonly { label: string; interval: BarInterval; days: number; bars: number }[];
type Preset = (typeof PRESETS)[number];

function load<T extends string>(key: string, allowed: readonly T[], fallback: T): T {
  try {
    const saved = localStorage.getItem(key);
    if (saved !== null && (allowed as readonly string[]).includes(saved)) return saved as T;
  } catch {
    // a convenience only
  }
  return fallback;
}

function save(key: string, value: string) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // a convenience only
  }
}

/** Theme colours from the app's CSS tokens (works in light and dark). */
function palette() {
  const css = getComputedStyle(document.documentElement);
  const get = (name: string, fallback: string) => css.getPropertyValue(name).trim() || fallback;
  return {
    background: get('--surface', '#ffffff'),
    text: get('--muted', '#5f6b7a'),
    grid: get('--border', '#dfe3e8'),
    up: get('--up', '#1b7f3b'),
    down: get('--down', '#c62828'),
  };
}

/** #rrggbb + alpha; other colour forms are returned as they are. */
function withAlpha(color: string, alpha: number): string {
  if (!/^#[0-9a-f]{6}$/i.test(color)) return color;
  return color + Math.round(alpha * 255).toString(16).padStart(2, '0');
}

const istTime = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  hour12: false,
});

function signed(value: number, format: (v: number) => string): string {
  return (value > 0 ? '+' : value < 0 ? '−' : '') + format(Math.abs(value));
}

interface Series {
  chart: IChartApi;
  candles: ISeriesApi<'Candlestick'>;
  volume: ISeriesApi<'Histogram'> | null;
  oiChange: ISeriesApi<'Candlestick'> | null;
  oiLevel: ISeriesApi<'Line'> | null;
}

export function PriceChart({ instrument }: { instrument: Instrument }) {
  const container = useRef<HTMLDivElement>(null);
  const series = useRef<Series | null>(null);
  // The interval's rows, oldest first; the last may be the live current candle.
  const rows = useRef<BarRow[]>([]);
  const [interval, setInterval_] = useState<BarInterval>(() =>
    load(INTERVAL_KEY, INTERVALS.map((i) => i.value), '5m'),
  );
  const [preset, setPreset] = useState<Preset | null>(null);
  const [oiPane, setOiPane] = useState<OiPane>(() => load(OI_PANE_KEY, ['collapsed', 'normal', 'max'], 'normal'));
  const [hovered, setHovered] = useState<number | null>(null);  // index into rows
  const [, setVersion] = useState(0);  // re-render the legend when rows change
  const [oiAsOf, setOiAsOf] = useState<number | null>(null);
  const [status, setStatus] = useState<'loading' | 'ready' | 'empty' | string>('loading');
  const hasVolume = instrument.kind !== 'IDX';
  const hasOi = instrument.kind === 'FUT' || instrument.kind === 'OPT';

  // Create the chart once per instrument (its panes depend on the kind).
  useEffect(() => {
    const element = container.current;
    if (!element) return;
    const colors = palette();
    const chart = createChart(element, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: colors.background },
        textColor: colors.text,
        fontSize: 11,
        panes: { separatorColor: colors.grid, enableResize: true },
      },
      grid: { vertLines: { color: colors.grid }, horzLines: { color: colors.grid } },
      timeScale: { timeVisible: true, secondsVisible: false, borderColor: colors.grid },
      rightPriceScale: { borderColor: colors.grid },
      crosshair: { mode: 0 },
    });
    const candles = chart.addSeries(CandlestickSeries, {
      upColor: colors.up,
      downColor: colors.down,
      borderUpColor: colors.up,
      borderDownColor: colors.down,
      wickUpColor: colors.up,
      wickDownColor: colors.down,
      priceLineVisible: true,
    });
    // Volume over the bottom quarter of the price pane, on its own hidden scale (as Kite).
    const volume = hasVolume
      ? chart.addSeries(HistogramSeries, {
          priceFormat: { type: 'volume' },
          priceScaleId: 'volume',
          priceLineVisible: false,
          lastValueVisible: false,
        })
      : null;
    if (volume) {
      candles.priceScale().applyOptions({ scaleMargins: { top: 0.08, bottom: 0.25 } });
      volume.priceScale().applyOptions({ scaleMargins: { top: 0.78, bottom: 0 } });
    }
    const oiChange = hasOi
      ? chart.addSeries(
          CandlestickSeries,
          { priceFormat: { type: 'volume' }, priceLineVisible: false, lastValueVisible: false },
          1,
        )
      : null;
    const oiLevel = hasOi
      ? chart.addSeries(
          LineSeries,
          {
            color: colors.text,
            lineWidth: 1,
            priceScaleId: 'oi-level',
            priceFormat: { type: 'volume' },
            priceLineVisible: false,
            lastValueVisible: false,
            crosshairMarkerVisible: false,
          },
          1,
        )
      : null;
    oiLevel?.priceScale().applyOptions({ scaleMargins: { top: 0.1, bottom: 0.1 } });

    const onCrosshair = (param: MouseEventParams) => {
      if (param.time === undefined) {
        setHovered(null);
        return;
      }
      const time = Number(param.time);
      const index = rows.current.findIndex((r) => toChartTime(r[0]) === time);
      setHovered(index >= 0 ? index : null);
    };
    chart.subscribeCrosshairMove(onCrosshair);
    series.current = { chart, candles, volume, oiChange, oiLevel };
    return () => {
      chart.unsubscribeCrosshairMove(onCrosshair);
      chart.remove();
      series.current = null;
    };
  }, [instrument.instrument_id, hasVolume, hasOi]);

  // Price : OI heights for the pane mode (the divider stays draggable in between).
  useEffect(() => {
    const panes = series.current?.chart.panes();
    if (!hasOi || !panes || panes.length < 2) return;
    const [price, oi] = OI_STRETCH[oiPane];
    panes[0]?.setStretchFactor(price);
    panes[1]?.setStretchFactor(oi);
  }, [oiPane, hasOi, instrument.instrument_id]);

  // Load history for the interval (or range preset), then follow live minutes.
  useEffect(() => {
    let active = true;
    let stopLive: (() => void) | undefined;
    let refreshTimer: ReturnType<typeof setTimeout> | undefined;
    const colors = palette();
    const oiColors = buildupColors();
    const volumeUp = withAlpha(colors.up, 0.35);
    const volumeDown = withAlpha(colors.down, 0.35);
    const bump = () => setVersion((v) => v + 1);
    rows.current = [];
    setStatus('loading');
    setHovered(null);
    setOiAsOf(null);

    const noteOiTime = (row: BarRow) => {
      const t = row[11];
      if (t !== null) setOiAsOf((previous) => (previous === null || t > previous ? t : previous));
    };

    const drawOi = (s: Series) => {
      if (!s.oiChange || !s.oiLevel) return;
      const list = rows.current;
      s.oiChange.setData(
        list.flatMap((row) => {
          const bar = oiChangeBar(row, oiColors, toChartTime(row[0]));
          return bar ? [{ ...bar, time: bar.time as UTCTimestamp }] : [];
        }),
      );
      s.oiLevel.setData(
        list.filter((r) => r[6] > 0).map((r) => ({ time: toChartTime(r[0]) as UTCTimestamp, value: r[6] })),
      );
    };

    // The live current candle: price and volume as merged; state and ΔOI from the last REST read.
    const drawLive = (merged: BarRow, s: Series) => {
      const list = rows.current;
      const last = list[list.length - 1];
      if (last && last[0] === merged[0]) {
        // Keep the state, and measure ΔOI from the same OI the REST read did (body and wick
        // then follow the live OI; the colour is fixed at the minute-close refresh).
        merged[12] = last[12];
        merged[13] = last[13] === null || merged[6] <= 0 ? null : merged[6] - (last[6] - last[13]);
        list[list.length - 1] = merged;
      } else {
        list.push(merged);
      }
      const time = toChartTime(merged[0]) as UTCTimestamp;
      s.candles.update({ time, open: merged[1], high: merged[2], low: merged[3], close: merged[4] });
      s.volume?.update({ time, value: merged[5], color: merged[4] >= merged[1] ? volumeUp : volumeDown });
      if (s.oiLevel && merged[6] > 0) s.oiLevel.update({ time, value: merged[6] });
      const bar = oiChangeBar(merged, oiColors, time);
      if (s.oiChange && bar) s.oiChange.update({ ...bar, time });
      noteOiTime(merged);
      bump();
    };

    // Re-reads the candles closed since the last refresh (at least the last two): each gets its
    // final state and ΔOI. Live that is one candle a minute; a fast replay closes several.
    let closedSince: number | undefined;
    const refresh = async () => {
      const list = rows.current;
      const lastTwo = list[Math.max(0, list.length - 2)]?.[0];
      const from = closedSince !== undefined && lastTwo !== undefined ? Math.min(closedSince, lastTwo) : lastTwo;
      closedSince = undefined;
      if (from === undefined) return;
      const fresh = await api.bars(instrument.instrument_id, interval, from);
      if (!active || !series.current) return;
      const byTime = new Map(fresh.bars.map((r) => [r[0], r] as const));
      for (const row of rows.current) {
        const update = row[0] >= from ? byTime.get(row[0]) : undefined;
        if (update) {
          row[12] = update[12];
          row[13] = update[13];
        }
      }
      drawOi(series.current);
      bump();
    };

    void (async () => {
      try {
        const now = Math.floor(Date.now() / 1000);
        const history = await api.bars(
          instrument.instrument_id,
          interval,
          preset ? now - preset.days * 86_400 : undefined,
        );
        if (!active || !series.current) return;
        const s = series.current;
        rows.current = history.bars;
        const list = rows.current;
        s.candles.setData(
          list.map(([t, o, h, l, c]) => ({ time: toChartTime(t) as UTCTimestamp, open: o, high: h, low: l, close: c })),
        );
        s.volume?.setData(
          list.map(([t, o, , , c, v]) => ({
            time: toChartTime(t) as UTCTimestamp,
            value: v,
            color: c >= o ? volumeUp : volumeDown,
          })),
        );
        drawOi(s);
        for (const row of list) noteOiTime(row);
        const visible = preset ? preset.bars : VISIBLE_BARS;
        if (visible > 0 && list.length > visible) {
          s.chart.timeScale().setVisibleLogicalRange({ from: list.length - visible, to: list.length + 3 });
        } else {
          s.chart.timeScale().fitContent();
        }
        setStatus(list.length === 0 ? 'empty' : 'ready');
        bump();

        // The current candle = its minutes: 1m history since the bucket began, then live. It
        // starts from the newest bar loaded, not the wall clock, so a replay (whose bars are
        // hours behind the clock) is followed too; live, the two are the same minute.
        // With no history yet, the first live bar starts it.
        const newest = list[list.length - 1]?.[0];
        let bucket = newest === undefined ? 0 : bucketStart(newest, interval);
        const minutes = new Map<number, BarRow>();
        if (interval !== '1m' && bucket > 0) {
          const recent = await api.bars(instrument.instrument_id, '1m', bucket);
          if (!active) return;
          for (const row of recent.bars) minutes.set(row[0], row);
        }
        let lastClosed = 0;
        stopLive = quoteStream.watchBars(instrument.instrument_id, (live) => {
          const current = series.current;
          if (!current) return;
          const start = bucketStart(live.t, interval);
          if (start < bucket) return;  // an older minute: already in history
          if (start > bucket) {
            bucket = start;
            minutes.clear();
          }
          minutes.set(live.t, liveRow(live));
          const merged = mergeRows(bucket, [...minutes.values()].sort((a, b) => a[0] - b[0]));
          if (merged) {
            drawLive(merged, current);
            setStatus('ready');
          }
          if (hasOi && !live.forming && live.t > lastClosed) {
            lastClosed = live.t;
            closedSince = Math.min(closedSince ?? start, start);
            // A throttle, not a debounce: during a fast replay minutes close every second, and
            // restarting the timer on each would never let it fire.
            refreshTimer ??= setTimeout(() => {
              refreshTimer = undefined;
              void refresh().catch(() => undefined);
            }, REFRESH_DELAY_MS);
          }
        });
      } catch (e) {
        if (active) setStatus(errorMessage(e));
      }
    })();

    return () => {
      active = false;
      clearTimeout(refreshTimer);
      stopLive?.();
    };
  }, [instrument.instrument_id, interval, preset, hasOi]);

  const chooseInterval = (value: BarInterval) => {
    setPreset(null);
    setInterval_(value);
    save(INTERVAL_KEY, value);
  };

  const choosePreset = (value: Preset) => {
    setPreset(value);
    setInterval_(value.interval);
  };

  const chooseOiPane = (value: OiPane) => {
    setOiPane(value);
    save(OI_PANE_KEY, value);
  };

  // The legend shows the hovered candle, or the latest one.
  const list = rows.current;
  const index = hovered ?? (list.length > 0 ? list.length - 1 : null);
  const shown = index !== null ? list[index] : undefined;
  const previous = index !== null && index > 0 ? list[index - 1] : undefined;
  const up = shown ? shown[4] >= shown[1] : true;
  const change = shown && previous ? shown[4] - previous[4] : null;
  const state: Buildup | null = shown?.[12] ?? null;
  const oiChange = shown && index !== null ? buildupChange(shown, reference(list, index)) : null;
  // The wick, when OI went beyond where it closed (e.g. +500K reached, +300K kept).
  const range = shown && oiChange ? oiRange(shown) : null;
  const wick =
    range && (range.high > Math.max(oiChange!.oi, 0) || range.low < Math.min(oiChange!.oi, 0)) ? range : null;
  const intervalLabel = INTERVALS.find((i) => i.value === interval)?.label ?? interval;

  return (
    <section className="card wide chart-card">
      <div className="chart-toolbar">
        <div className="chart-intervals" role="group" aria-label="Interval">
          {INTERVALS.map((i) => (
            <button
              key={i.value}
              type="button"
              className={i.value === interval && !preset ? 'active' : ''}
              aria-pressed={i.value === interval && !preset}
              onClick={() => chooseInterval(i.value)}
            >
              {i.label}
            </button>
          ))}
        </div>
        {hasOi && (
          <div className="chart-intervals oi-pane-control" role="group" aria-label="OI pane">
            {(
              [
                ['collapsed', '–', 'Collapse the OI pane'],
                ['normal', '▢', 'Price and OI'],
                ['max', '⤢', 'Maximise the OI pane'],
              ] as const
            ).map(([value, label, title]) => (
              <button
                key={value}
                type="button"
                title={title}
                aria-label={title}
                className={oiPane === value ? 'active' : ''}
                aria-pressed={oiPane === value}
                onClick={() => chooseOiPane(value)}
              >
                {label}
              </button>
            ))}
          </div>
        )}
        {hasOi && oiAsOf !== null && (
          <span className="chart-oi-asof muted">OI as of {istTime.format(oiAsOf * 1000)}</span>
        )}
      </div>
      <div className="chart-legend" aria-live="off">
        {shown ? (
          <>
            <span className="muted">{preset ? preset.label : intervalLabel}</span>
            <span>O <b className={up ? 'up' : 'down'}>{formatPrice(shown[1])}</b></span>
            <span>H <b className={up ? 'up' : 'down'}>{formatPrice(shown[2])}</b></span>
            <span>L <b className={up ? 'up' : 'down'}>{formatPrice(shown[3])}</b></span>
            <span>C <b className={up ? 'up' : 'down'}>{formatPrice(shown[4])}</b></span>
            {change !== null && previous && (
              <span className={change >= 0 ? 'up' : 'down'}>
                {signed(change, formatPrice)} ({signed((change / previous[4]) * 100, (v) => v.toFixed(2))}%)
              </span>
            )}
            {hasVolume && <span>Vol <b>{formatQuantity(shown[5])}</b></span>}
            {hasOi && shown[6] > 0 && <span>OI <b>{formatQuantity(shown[6])}</b></span>}
            {hasOi && oiChange && (
              <span>
                ΔOI <b>{signed(oiChange.oi, formatQuantity)}</b> ({signed(oiChange.oiPercent, (v) => v.toFixed(2))}%)
              </span>
            )}
            {hasOi && wick && (
              <span className="muted" title="OI range inside the bar, as changes from where ΔOI is measured">
                range {signed(wick.low, formatQuantity)} … {signed(wick.high, formatQuantity)}
              </span>
            )}
            {hasOi && state && (
              <span className={`oi-state oi-${state}`}>
                <i aria-hidden="true" />
                {BUILDUP_LABEL[state]}
              </span>
            )}
          </>
        ) : (
          <span className="muted">
            {hasOi ? 'Price · Volume · ΔOI buildup' : hasVolume ? 'Price · Volume' : 'Price'} · IST
          </span>
        )}
      </div>
      <div className="chart-box">
        <div ref={container} className="chart-canvas" />
        {status !== 'ready' && (
          <div className="chart-overlay">
            {status === 'loading'
              ? 'Loading chart…'
              : status === 'empty'
                ? 'No bars recorded for this instrument yet.'
                : status}
          </div>
        )}
      </div>
      <div className="chart-footer">
        <div className="chart-intervals" role="group" aria-label="Range">
          {PRESETS.map((p) => (
            <button
              key={p.label}
              type="button"
              className={preset?.label === p.label ? 'active' : ''}
              aria-pressed={preset?.label === p.label}
              onClick={() => choosePreset(p)}
            >
              {p.label}
            </button>
          ))}
        </div>
        {hasOi && (
          <ul className="oi-key" aria-label="OI buildup colours">
            {BUILDUP_ORDER.filter((s) => s !== 'no_oi_update').map((s) => (
              <li key={s} className={`oi-state oi-${s}`}>
                <i aria-hidden="true" />
                {BUILDUP_LABEL[s]}
              </li>
            ))}
            <li className="muted">gap = no OI update</li>
          </ul>
        )}
      </div>
    </section>
  );
}
