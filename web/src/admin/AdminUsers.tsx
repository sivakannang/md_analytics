// Admin: every user with status, sign-in history and today's traffic (ADR-0019).
// Refreshes every 5 s while the tab is visible.

import { useCallback } from 'react';
import { Link } from 'react-router-dom';

import { api } from '../api/client';
import type { AdminUser } from '../api/client';
import { AdminTabs } from './AdminTabs';
import { ago, formatBytes, istTime } from './format';
import { usePolling } from './usePolling';

function Status({ user }: { user: AdminUser }) {
  if (user.online > 0) {
    return (
      <span className="status online" title={`${user.streaming_instruments} instruments streaming`}>
        Online{user.online > 1 ? ` · ${user.online} streams` : ''}
      </span>
    );
  }
  if (user.active_sessions > 0) {
    return <span className="status idle">Signed in</span>;
  }
  return <span className="status offline">Offline</span>;
}

export function AdminUsers() {
  const load = useCallback(() => api.adminUsers(), []);
  const { data: users, error } = usePolling(load, 5000);

  const online = users?.filter((u) => u.online > 0).length ?? 0;
  const today = users?.reduce((sum, u) => sum + u.today.total_bytes, 0) ?? 0;

  return (
    <div className="page admin">
      <AdminTabs />
      <header className="admin-header">
        <h2>Users</h2>
        {users && (
          <span className="muted">
            {users.length} users · {online} online · {formatBytes(today)} sent today
          </span>
        )}
      </header>
      {error && <p className="form-error">{error}</p>}
      {!users && !error && <p className="loading">Loading…</p>}
      {users && (
        <div className="table-scroll">
          <table className="admin-table">
            <thead>
              <tr>
                <th>User</th>
                <th>Status</th>
                <th>Last sign-in</th>
                <th>Last sign-out</th>
                <th>Last seen</th>
                <th className="num">Sessions</th>
                <th className="num">Data today</th>
                <th className="num">Messages</th>
              </tr>
            </thead>
            <tbody>
              {users.map((user) => (
                <tr key={user.user_id}>
                  <td>
                    <Link to={`/admin/users/${user.user_id}`} className="admin-user">
                      <strong>{user.username}</strong>
                      {user.role === 'admin' && <span className="badge ok">admin</span>}
                    </Link>
                    <div className="muted small">{user.email || '—'}</div>
                  </td>
                  <td>
                    <Status user={user} />
                  </td>
                  <td title={istTime(user.last_sign_in_at)}>{ago(user.last_sign_in_at)}</td>
                  <td title={istTime(user.last_sign_out_at)}>{ago(user.last_sign_out_at)}</td>
                  <td title={istTime(user.last_seen_at)}>{ago(user.last_seen_at)}</td>
                  <td className="num">{user.active_sessions}</td>
                  <td className="num" title={`Stream ${formatBytes(user.today.ws_bytes)} · REST ${formatBytes(user.today.http_bytes)}`}>
                    {formatBytes(user.today.total_bytes)}
                  </td>
                  <td className="num">{user.today.ws_messages.toLocaleString('en-IN')}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <p className="muted small">
        Data is what the gateway sent (live stream + API responses, excluding network
        overhead), counted per Indian trading day. Sign-in history starts when this dashboard was
        deployed.
      </p>
    </div>
  );
}
