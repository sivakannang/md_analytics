import { useState } from 'react';
import type { FormEvent } from 'react';

import { ApiError, api, errorMessage, isSessionLost } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { PASSWORD_HINT, passwordError } from '../auth/validation';
import { Field, FormError } from '../components/Field';
import { NotificationSettings } from '../components/NotificationSettings';

/** +919876543210 -> +91 98765 43210 */
function formatPhone(e164: string): string {
  const match = /^\+91(\d{5})(\d{5})$/.exec(e164);
  return match ? `+91 ${match[1]} ${match[2]}` : e164;
}

function Verified({ verified }: { verified: boolean }) {
  return verified ? (
    <span className="badge ok">Verified</span>
  ) : (
    <span className="badge" title="Verify it under Notifications below">
      Not verified
    </span>
  );
}

function formatDate(iso: string): string {
  return new Date(iso).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
}

/** Deletes the account after the password is re-entered and the user confirms. */
function DeleteAccount({ onDeleted }: { onDeleted: () => void }) {
  const [open, setOpen] = useState(false);
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    if (!window.confirm('Delete your account and all your watchlists? This cannot be undone.')) {
      return;
    }
    setError(null);
    setBusy(true);
    try {
      await api.deleteAccount(password);
      onDeleted();
    } catch (e) {
      if (isSessionLost(e)) {
        onDeleted();
        return;
      }
      setError(errorMessage(e));
      setBusy(false);
    }
  };

  if (!open) {
    return (
      <section className="card wide danger-zone">
        <h2>Delete account</h2>
        <p className="subtitle">
          Removes your profile, sessions and watchlists permanently. Signals you set up will stop.
        </p>
        <div>
          <button type="button" className="secondary" onClick={() => setOpen(true)}>
            Delete my account…
          </button>
        </div>
      </section>
    );
  }

  return (
    <form className="card wide danger-zone" onSubmit={submit} noValidate>
      <h2>Delete account</h2>
      <p className="subtitle">Enter your password to confirm. This cannot be undone.</p>
      <Field
        label="Password"
        name="delete-password"
        type="password"
        autoComplete="current-password"
        value={password}
        onChange={(e) => setPassword(e.target.value)}
      />
      <FormError message={error} />
      <div className="actions">
        <button type="submit" className="destructive" disabled={busy || !password}>
          {busy ? 'Deleting…' : 'Delete account permanently'}
        </button>
        <button
          type="button"
          className="secondary"
          disabled={busy}
          onClick={() => {
            setOpen(false);
            setPassword('');
            setError(null);
          }}
        >
          Cancel
        </button>
      </div>
    </form>
  );
}

export function Account() {
  const { state, sessionEnded } = useAuth();
  const [current, setCurrent] = useState('');
  const [next, setNext] = useState('');
  const [confirm, setConfirm] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const [busy, setBusy] = useState(false);

  if (state.status !== 'signed-in') {
    return null; // RequireAuth handles the other states
  }
  const { user } = state;

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setSaved(false);
    const problem =
      passwordError(next) ??
      (next !== confirm ? 'New passwords do not match' : null) ??
      (next === current ? 'New password must differ from the current one' : null);
    if (problem) {
      setError(problem);
      return;
    }
    setError(null);
    setBusy(true);
    try {
      await api.changePassword(current, next);
      setSaved(true);
      setCurrent('');
      setNext('');
      setConfirm('');
    } catch (e) {
      // A wrong current password is also an AuthException; only a lost session signs out.
      if (e instanceof ApiError && e.errorType === 'AuthException' && e.message === 'not signed in') {
        sessionEnded();
        return;
      }
      setError(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="stack">
      <section className="card wide">
        <h2>Profile</h2>
        <dl className="details">
          <dt>Username</dt>
          <dd>{user.username}</dd>
          <dt>Email</dt>
          <dd>
            {user.email || '—'} {user.email && <Verified verified={user.email_verified} />}
          </dd>
          <dt>Mobile</dt>
          <dd>
            {user.phone ? formatPhone(user.phone) : '—'}{' '}
            {user.phone && <Verified verified={user.phone_verified} />}
          </dd>
          <dt>User ID</dt>
          <dd>{user.user_id}</dd>
          <dt>Member since</dt>
          <dd>{formatDate(user.created_at)}</dd>
        </dl>
      </section>

      <NotificationSettings />

      <form className="card wide" onSubmit={submit} noValidate>
        <h2>Change password</h2>
        <p className="subtitle">Other devices will be signed out; this one stays signed in.</p>
        <Field
          label="Current password"
          name="current"
          type="password"
          autoComplete="current-password"
          value={current}
          onChange={(e) => setCurrent(e.target.value)}
        />
        <Field
          label="New password"
          name="next"
          type="password"
          autoComplete="new-password"
          hint={PASSWORD_HINT}
          value={next}
          onChange={(e) => setNext(e.target.value)}
        />
        <Field
          label="Confirm new password"
          name="confirm"
          type="password"
          autoComplete="new-password"
          value={confirm}
          onChange={(e) => setConfirm(e.target.value)}
        />
        <FormError message={error} />
        {saved && (
          <p className="notice" role="status">
            Password changed.
          </p>
        )}
        <button type="submit" disabled={busy || !current || !next || !confirm}>
          {busy ? 'Saving…' : 'Change password'}
        </button>
      </form>

      <DeleteAccount
        onDeleted={() => {
          // A full reload to the public home page drops every bit of client state from the
          // deleted account (and avoids RequireAuth racing us to /signin).
          window.location.replace('/');
        }}
      />
    </div>
  );
}
