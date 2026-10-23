// Account → Notifications (docs/design/notifications.md §3, unit N5): verify the email with
// a mailed code, connect the Telegram bot with a one-time t.me link, switch each channel
// on or off. Phone numbers are verified by sharing the contact with the bot.

import { useCallback, useEffect, useRef, useState } from 'react';
import type { FormEvent } from 'react';

import { api, errorMessage } from '../api/client';
import type { NotificationChannels } from '../api/client';
import { useAuth } from '../auth/AuthContext';

/** While waiting for "Start" in Telegram: how often, and for how long, to look. */
const LINK_POLL_MS = 3_000;
const LINK_WAIT_MS = 10 * 60_000;

function Toggle({
  checked,
  disabled,
  label,
  onChange,
}: {
  checked: boolean;
  disabled?: boolean;
  label: string;
  onChange: (on: boolean) => void;
}) {
  return (
    <label className="toggle">
      <input
        type="checkbox"
        checked={checked}
        disabled={disabled}
        aria-label={label}
        onChange={(e) => onChange(e.target.checked)}
      />
      {checked ? 'On' : 'Off'}
    </label>
  );
}

export function NotificationSettings() {
  const { refreshProfile } = useAuth();
  const [channels, setChannels] = useState<NotificationChannels | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [codeSent, setCodeSent] = useState(false);
  const [code, setCode] = useState('');
  const [busy, setBusy] = useState(false);
  const [waitingForTelegram, setWaitingForTelegram] = useState(false);
  const waitUntil = useRef(0);
  const shown = useRef<NotificationChannels | null>(null);

  const load = useCallback(async () => {
    try {
      const next = await api.notifications();
      const previous = shown.current;
      // The Profile card shows the same verified badges: keep it in step.
      if (previous && (previous.email.verified !== next.email.verified || previous.phone_verified !== next.phone_verified)) {
        void refreshProfile().catch(() => undefined);
      }
      shown.current = next;
      setChannels(next);
      return next;
    } catch (e) {
      setError(errorMessage(e));
      return null;
    }
  }, [refreshProfile]);

  useEffect(() => {
    void load();
  }, [load]);

  // After "Connect Telegram", look for the link until it appears (or the code expires).
  useEffect(() => {
    if (!waitingForTelegram) return;
    const timer = window.setInterval(() => {
      void load().then((next) => {
        if (next?.telegram.linked || Date.now() > waitUntil.current) setWaitingForTelegram(false);
      });
    }, LINK_POLL_MS);
    return () => window.clearInterval(timer);
  }, [waitingForTelegram, load]);

  const act = async (work: () => Promise<unknown>) => {
    setError(null);
    setBusy(true);
    try {
      await work();
      await load();
    } catch (e) {
      setError(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };

  const sendCode = () =>
    act(async () => {
      await api.sendEmailCode();
      setCodeSent(true);
    });

  const verify = (event: FormEvent) => {
    event.preventDefault();
    void act(async () => {
      await api.verifyEmail(code.trim());
      setCodeSent(false);
      setCode('');
    });
  };

  const connectTelegram = () =>
    act(async () => {
      const { url, expires_in } = await api.telegramLink();
      window.open(url, '_blank', 'noopener');
      waitUntil.current = Date.now() + Math.min(expires_in * 1000, LINK_WAIT_MS);
      setWaitingForTelegram(true);
    });

  if (!channels) {
    return (
      <section className="card wide">
        <h2>Notifications</h2>
        {error ? <p className="form-error">{error}</p> : <p className="muted">Loading…</p>}
      </section>
    );
  }
  const { email, telegram } = channels;

  return (
    <section className="card wide notifications">
      <h2>Notifications</h2>
      <p className="subtitle">
        Account messages always go to your email. System alerts (admins) and, later, market
        signals go to the verified channels you switch on.
      </p>

      <div className="channel-row">
        <div className="channel-name">
          <strong>Email</strong>
          <span className="muted">{email.address || '—'}</span>
          {email.verified ? <span className="badge ok">Verified</span> : <span className="badge">Not verified</span>}
        </div>
        <div className="channel-actions">
          {!email.verified && email.address && !codeSent && (
            <button type="button" className="secondary small" disabled={busy} onClick={() => void sendCode()}>
              Send code
            </button>
          )}
          <Toggle
            label="Email notifications"
            checked={email.enabled}
            disabled={busy}
            onChange={(on) => void act(() => api.setChannel('email', on))}
          />
        </div>
      </div>
      {codeSent && (
        <form className="code-form" onSubmit={verify} noValidate>
          <label>
            Code from the email
            <input
              inputMode="numeric"
              autoComplete="one-time-code"
              maxLength={6}
              pattern="\d{6}"
              value={code}
              onChange={(e) => setCode(e.target.value.replace(/\D/g, ''))}
            />
          </label>
          <button type="submit" className="small" disabled={busy || code.length !== 6}>
            Verify
          </button>
          <span className="muted">
            It expires in 15 minutes. Not there? Check Spam, or{' '}
            <button type="button" className="link-button" disabled={busy} onClick={() => void sendCode()}>
              send again
            </button>
            .
          </span>
        </form>
      )}

      <div className="channel-row">
        <div className="channel-name">
          <strong>Telegram</strong>
          {!telegram.available ? (
            <span className="muted">Not set up on this server</span>
          ) : telegram.linked ? (
            <span className="badge ok">Connected</span>
          ) : (
            <span className="muted">Not connected</span>
          )}
        </div>
        {telegram.available && (
          <div className="channel-actions">
            {telegram.linked ? (
              <>
                <button
                  type="button"
                  className="secondary small"
                  disabled={busy}
                  onClick={() => void act(() => api.telegramUnlink())}
                >
                  Disconnect
                </button>
                <Toggle
                  label="Telegram notifications"
                  checked={telegram.enabled}
                  disabled={busy}
                  onChange={(on) => void act(() => api.setChannel('telegram', on))}
                />
              </>
            ) : (
              <button type="button" className="small" disabled={busy} onClick={() => void connectTelegram()}>
                {waitingForTelegram ? 'Open link again' : 'Connect Telegram'}
              </button>
            )}
          </div>
        )}
      </div>
      {waitingForTelegram && !telegram.linked && (
        <p className="notice" role="status">
          Tap <b>Start</b> in Telegram, then come back here. Waiting for the bot…
        </p>
      )}

      <div className="channel-row">
        <div className="channel-name">
          <strong>Mobile</strong>
          {channels.phone_verified ? (
            <span className="badge ok">Verified</span>
          ) : (
            <span className="muted">
              {telegram.linked
                ? 'Tap "Share phone number" in the bot to verify it'
                : 'Connect Telegram, then share your phone number with the bot to verify it'}
            </span>
          )}
        </div>
      </div>

      {error && <p className="form-error">{error}</p>}
    </section>
  );
}
