// Admin → System (docs/design/system-monitor.md §2, SM3): the stack's health from
// GET /admin/system, refreshed every 5 s. Each card takes the worst level of its checks
// (green / amber / red); trend lines show the last hour.

import { useCallback } from 'react';
import type { ReactNode } from 'react';

import { api } from '../api/client';
import type { CheckLevel, LatencySummary, SystemCheck, SystemReport } from '../api/client';
import { AdminTabs } from './AdminTabs';
import { formatBytes } from './format';
import { usePolling } from './usePolling';

const LEVEL_TEXT: Record<CheckLevel, string> = { ok: 'All OK', warn: 'Needs a look', bad: 'Problem' };

const clock = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  hour12: false,
});
const hhmm = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  hour12: false,
});

function level(checks: SystemCheck[], card: string): CheckLevel {
  const mine = checks.filter((c) => c.card === card);
  return mine.some((c) => c.level === 'bad') ? 'bad' : mine.some((c) => c.level === 'warn') ? 'warn' : 'ok';
}

/** 850 -> "850 µs", 1500 -> "1.5 ms". */
function micros(us: number): string {
  return us < 1000 ? `${us} µs` : `${(us / 1000).toFixed(us < 10_000 ? 1 : 0)} ms`;
}

function age(ms: number | null | undefined): string {
  if (ms === null || ms === undefined) return '—';
  return ms < 1000 ? `${ms} ms` : ms < 120_000 ? `${(ms / 1000).toFixed(1)} s` : `${Math.round(ms / 60_000)} min`;
}

function duration(seconds: number | null | undefined): string {
  if (seconds === null || seconds === undefined) return '—';
  const d = Math.floor(seconds / 86_400);
  const h = Math.floor((seconds % 86_400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return d > 0 ? `${d} d ${h} h` : h > 0 ? `${h} h ${m} min` : `${m} min`;
}

const num = (v: number | null | undefined, digits = 0) => (v === null || v === undefined ? '—' : v.toLocaleString('en-IN', { maximumFractionDigits: digits }));

/** A small trend line over the history; the last value is labelled by the caller. */
function Sparkline({ values, label }: { values: number[]; label: string }) {
  if (values.length < 2) return null;
  const width = 140;
  const height = 28;
  const max = Math.max(...values, 1e-9);
  const min = Math.min(...values, 0);
  const span = max - min || 1;
  const points = values
    .map((v, i) => `${((i / (values.length - 1)) * width).toFixed(1)},${(height - ((v - min) / span) * (height - 2) - 1).toFixed(1)}`)
    .join(' ');
  return (
    <svg className="sparkline" width={width} height={height} role="img" aria-label={label}>
      <polyline points={points} />
    </svg>
  );
}

function Card({ title, state, children }: { title: string; state: CheckLevel; children: ReactNode }) {
  return (
    <section className={`card sys-card ${state}`}>
      <h3>
        <i className={`sys-dot ${state}`} aria-hidden="true" />
        {title}
      </h3>
      {children}
    </section>
  );
}

function Row({ label, children, title }: { label: string; children: ReactNode; title?: string }) {
  return (
    <div className="sys-row" title={title}>
      <span className="muted">{label}</span>
      <span>{children}</span>
    </div>
  );
}

function Meter({ used, total }: { used: number; total: number }) {
  const pct = total > 0 ? Math.min(100, (used / total) * 100) : 0;
  return (
    <div className="sys-meter" aria-hidden="true">
      <div style={{ width: `${pct}%` }} className={pct > 90 ? 'bad' : pct > 75 ? 'warn' : ''} />
    </div>
  );
}

function Latency({ s, unit }: { s: LatencySummary; unit: 'us' | 's' }) {
  if (s.count === 0) return <span className="muted">no ticks in the last 5 s</span>;
  const f = unit === 'us' ? micros : (v: number) => `${v} s`;
  return (
    <>
      p50 <b>{f(s.p50)}</b> · p99 <b>{f(s.p99)}</b> · max {f(s.max)}
    </>
  );
}

export function AdminSystem() {
  const load = useCallback(() => api.adminSystem(), []);
  const { data, error } = usePolling(load, 5000);

  return (
    <div className="page admin">
      <AdminTabs />
      <header className="admin-header">
        <h2>System</h2>
        {data && (
          <>
            <span className={`sys-pill ${data.status}`}>{LEVEL_TEXT[data.status]}</span>
            <span className="muted">
              updated {clock.format(data.time * 1000)} · {data.market_hours ? 'market open' : 'market closed'}
            </span>
          </>
        )}
      </header>
      {error && <p className="form-error">{error}</p>}
      {!data && !error && <p className="loading">Loading…</p>}
      {data && <Report r={data} />}
    </div>
  );
}

function Report({ r }: { r: SystemReport }) {
  const c = r.checks;
  const m = r.machine;
  const h = r.history;
  return (
    <>
      {c.length > 0 && (
        <ul className="sys-checks" aria-label="Checks">
          {c.map((check, i) => (
            <li key={i} className={check.level}>
              <i className={`sys-dot ${check.level}`} aria-hidden="true" />
              {check.text}
            </li>
          ))}
        </ul>
      )}
      <div className="sys-grid">
        <Card title="Processes" state={level(c, 'processes')}>
          <table className="sys-table">
            <thead>
              <tr>
                <th>Name</th>
                <th>Since</th>
                <th className="num">CPU</th>
                <th className="num" title="Resident memory; includes the mapped ring buffers">
                  Memory*
                </th>
              </tr>
            </thead>
            <tbody>
              {r.processes.map((p) => (
                <tr key={p.name} className={p.running ? '' : 'down'}>
                  <td>
                    <i className={`sys-dot ${p.running ? 'ok' : 'bad'}`} aria-hidden="true" />
                    {p.name === 'web' ? 'web (dev server)' : p.name}
                  </td>
                  <td>{p.running && p.started ? hhmm.format(p.started * 1000) : 'stopped'}</td>
                  <td className="num">{p.cpu_percent != null ? `${p.cpu_percent}%` : '—'}</td>
                  <td className="num">{p.rss_bytes !== undefined ? formatBytes(p.rss_bytes) : '—'}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p className="sys-note muted">* includes the mapped ring buffers</p>
        </Card>

        <Card title="Machine" state={level(c, 'machine')}>
          <Row label="CPU load">
            <b>{num(m.load1, 2)}</b> / {m.cores} cores
          </Row>
          <Sparkline values={h.load1} label="CPU load, last hour" />
          <Row label="Memory">
            {m.memory_used !== null && m.memory_total !== null ? (
              <>
                <b>{formatBytes(m.memory_used)}</b> of {formatBytes(m.memory_total)}
              </>
            ) : (
              '—'
            )}
          </Row>
          {m.memory_used !== null && m.memory_total !== null && <Meter used={m.memory_used} total={m.memory_total} />}
          <Row label="Power">
            <b>{m.power === 'battery' ? 'Battery' : m.power === 'ac' ? 'AC' : '—'}</b>
            {m.battery_percent !== null && ` (${m.battery_percent}%)`}
          </Row>
          <Row label="Uptime">{duration(m.uptime_s)}</Row>
        </Card>

        <Card title="Disk" state={level(c, 'disk')}>
          {r.disk.free !== undefined && r.disk.total !== undefined && (
            <>
              <Row label="Free">
                <b>{formatBytes(r.disk.free)}</b> of {formatBytes(r.disk.total)}
              </Row>
              <Meter used={r.disk.total - r.disk.free} total={r.disk.total} />
            </>
          )}
          <Row label="bars.db">{formatBytes(r.disk.files.bars_db)}</Row>
          <Row label="app.db">{formatBytes(r.disk.files.app_db)}</Row>
          <Row label="last_quotes.db">{formatBytes(r.disk.files.last_quotes_db)}</Row>
          <Row label="Journal">
            {formatBytes(r.disk.journal_today)} today · {formatBytes(r.disk.journal_all)} in all
          </Row>
        </Card>

        <Card title="Feed" state={level(c, 'feed')}>
          <Row label="State">
            <b>{r.feed.state ?? '—'}</b>
          </Row>
          <Row label="Ticks">
            <b>{num(r.feed.ticks_per_s)}</b>/s · {num(r.feed.frames_per_s, 1)} frames/s
          </Row>
          <Sparkline values={h.ticks_per_s} label="Ticks per second, last hour" />
          <Row label="Instruments">{num(r.feed.subscribed)}</Row>
          <Row label="Last tick">{age(r.feed.last_tick_age_ms)} ago</Row>
          <Row label="Reconnects" title="Since mda-feedd started">
            {r.feed.reconnects}
          </Row>
        </Card>

        <Card title="Latency" state={level(c, 'latency')}>
          <Row label="Feed → gateway" title="From mda-feedd reading the frame to the gateway reading the tick">
            <Latency s={r.latency.feed_to_gateway_us} unit="us" />
          </Row>
          <Sparkline values={h.feed_to_gateway_p99_us} label="Feed to gateway p99, last hour" />
          <Row label="Exchange → feed" title="Kite's exchange time is whole seconds">
            <Latency s={r.latency.exchange_to_feed_s} unit="s" />
          </Row>
        </Card>

        <Card title="Ring buffers" state={level(c, 'buses')}>
          {r.buses.map((b) =>
            !b.available ? (
              <Row key={b.name} label={b.name}>
                <span className="muted">not available</span>
              </Row>
            ) : (
              <div key={b.name} className="sys-bus">
                <Row label={b.name}>
                  <i className={`sys-dot ${b.writer_alive ? 'ok' : 'bad'}`} aria-hidden="true" />
                  {formatBytes(b.capacity ?? 0)} · {formatBytes(b.bytes_per_s ?? 0)}/s · {num(b.messages_per_s)} msg/s
                </Row>
                {(b.readers ?? []).map((reader, i) => (
                  <Row key={i} label={`  ${reader.name}`}>
                    lag {formatBytes(reader.lag_bytes)} ({reader.lag_percent}%) · laps {reader.laps}
                  </Row>
                ))}
                {(b.stale_slots ?? 0) > 0 && (
                  <p className="sys-note muted">{b.stale_slots} slots of stopped readers</p>
                )}
              </div>
            ),
          )}
          <Sparkline values={h.md_max_lag_pct} label="md: largest reader lag %, last hour" />
        </Card>

        <Card title="Engine" state={level(c, 'engine')}>
          <Row label="Bars">
            <b>{num(r.engine.bars_per_min)}</b> per minute
          </Row>
          <Row label="Last bar">{r.engine.last_bar ? hhmm.format(r.engine.last_bar * 1000) : '—'}</Row>
          <Row label="Analytics">{num(r.engine.analytics_per_s)}/s</Row>
        </Card>

        <Card title="Notifier" state={level(c, 'notifier')}>
          <Row label="Queued">
            <b>{num(r.notifier.queued)}</b>
            {r.notifier.oldest_queued_s !== null && ` · oldest ${duration(r.notifier.oldest_queued_s)}`}
          </Row>
          <Row label="Sent today">{num(r.notifier.sent_today)}</Row>
          <Row label="Failed today">{num(r.notifier.failed_today)}</Row>
        </Card>

        <Card title="Recorder" state={level(c, 'recorder')}>
          <Row label="State">
            <b>{!r.recorder.enabled ? 'off' : r.recorder.running ? 'recording' : 'enabled, not running'}</b>
          </Row>
          <Row label="Today">{formatBytes(r.recorder.journal_today)}</Row>
          <Row label="Gaps today">{r.recorder.gaps_today ?? '—'}</Row>
          <Row label="Records">
            {r.recorder.records_last_minute != null ? `${num(r.recorder.records_last_minute)}/min` : '—'}
          </Row>
        </Card>
      </div>
    </>
  );
}
