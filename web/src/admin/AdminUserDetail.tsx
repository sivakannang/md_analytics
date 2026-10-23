// Admin: one user — summary, sessions (with force sign-out), live streams, the sign-in /
// sign-out timeline and 30 days of traffic (ADR-0019).

import { useCallback, useState } from 'react';
import { Link, useParams } from 'react-router-dom';

import { api, errorMessage } from '../api/client';
import { ago, device, eventLabel, formatBytes, istTime } from './format';
import { usePolling } from './usePolling';

export function AdminUserDetail() {
  const id = Number(useParams().userId);
  const load = useCallback(() => api.adminUser(id), [id]);
  const { data, error, refresh } = usePolling(load, 5000);
  const [actionError, setActionError] = useState<string | null>(null);

  const revoke = async (sessionId: string, label: string) => {
    if (!window.confirm(`Sign out ${label}? The user will have to sign in again on that device.`)) return;
    setActionError(null);
    try {
      await api.revokeSession(sessionId);
      await refresh();
    } catch (e) {
      setActionError(errorMessage(e));
    }
  };

  if (error) {
    return (
      <div className="page admin">
        <Link to="/admin">← Users</Link>
        <p className="form-error">{error}</p>
      </div>
    );
  }
  if (!data) return <p className="loading">Loading…</p>;

  const { user, sessions, events, usage, connections } = data;
  const peak = Math.max(1, ...usage.map((d) => d.total_bytes));

  return (
    <div className="page admin">
      <Link to="/admin" className="small">
        ← Users
      </Link>
      <header className="admin-header">
        <h2>
          {user.username} {user.role === 'admin' && <span className="badge ok">admin</span>}
        </h2>
        <span className="muted">
          {user.email || '—'} · {user.phone || '—'} · member since {istTime(user.created_at)}
        </span>
      </header>

      <section className="admin-cards">
        <div className="admin-card">
          <span className="muted small">Status</span>
          <strong className={user.online > 0 ? 'up' : ''}>
            {user.online > 0 ? `Online (${user.online} stream${user.online > 1 ? 's' : ''})` : user.active_sessions > 0 ? 'Signed in' : 'Offline'}
          </strong>
          <span className="muted small">last seen {ago(user.last_seen_at)}</span>
        </div>
        <div className="admin-card">
          <span className="muted small">Last sign-in / sign-out</span>
          <strong>{istTime(user.last_sign_in_at)}</strong>
          <span className="muted small">out: {istTime(user.last_sign_out_at)}</span>
        </div>
        <div className="admin-card">
          <span className="muted small">Data today</span>
          <strong>{formatBytes(user.today.total_bytes)}</strong>
          <span className="muted small">
            {user.today.ws_messages.toLocaleString('en-IN')} stream msgs · {user.today.http_requests} API calls
          </span>
        </div>
      </section>

      {actionError && <p className="form-error">{actionError}</p>}

      <section className="card wide">
        <h3>Active sessions ({sessions.length})</h3>
        {sessions.length === 0 ? (
          <p className="muted">No active sessions.</p>
        ) : (
          <table className="admin-table">
            <thead>
              <tr>
                <th>Device</th>
                <th>IP</th>
                <th>Signed in</th>
                <th>Last seen</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {sessions.map((s) => (
                <tr key={s.id}>
                  <td title={s.user_agent}>{device(s.user_agent)}</td>
                  <td>{s.ip || '—'}</td>
                  <td>{istTime(s.created_at)}</td>
                  <td title={istTime(s.last_seen_at)}>{ago(s.last_seen_at)}</td>
                  <td className="num">
                    <button type="button" className="secondary small" onClick={() => void revoke(s.id, device(s.user_agent))}>
                      Sign out
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
        {connections.length > 0 && (
          <p className="muted small">
            Live streams:{' '}
            {connections.map((c) => `since ${istTime(c.since)} (${c.instruments} instruments)`).join(' · ')}
          </p>
        )}
      </section>

      <section className="card wide">
        <h3>Data over the last 30 days</h3>
        {usage.length === 0 ? (
          <p className="muted">Nothing recorded yet.</p>
        ) : (
          <div className="usage-bars" role="img" aria-label="Daily data sent">
            {usage.map((d) => (
              <div
                key={d.day}
                className="usage-bar"
                title={`${d.day}: ${formatBytes(d.total_bytes)} (${d.ws_messages.toLocaleString('en-IN')} msgs, ${d.http_requests} API calls)`}
              >
                <span style={{ height: `${Math.max(2, (d.total_bytes / peak) * 100)}%` }} />
                <small>{d.day.slice(8)}</small>
              </div>
            ))}
          </div>
        )}
      </section>

      <section className="card wide">
        <h3>Sign-in activity</h3>
        {events.length === 0 ? (
          <p className="muted">No events recorded yet.</p>
        ) : (
          <ol className="timeline">
            {events.map((e, i) => (
              <li key={i} className={`event-${e.type}`}>
                <span className="event-type">{eventLabel(e.type)}</span>
                <span className="muted small">
                  {istTime(e.at)} · {ago(e.at)}
                  {e.ip && ` · ${e.ip}`}
                  {e.user_agent && ` · ${e.type === 'session_revoked' ? e.user_agent : device(e.user_agent)}`}
                </span>
              </li>
            ))}
          </ol>
        )}
      </section>
    </div>
  );
}
