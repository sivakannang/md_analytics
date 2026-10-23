import { useState } from 'react';
import type { FormEvent } from 'react';
import { Link, Navigate, useLocation, useNavigate } from 'react-router-dom';

import { errorMessage } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { Field, FormError } from '../components/Field';

export function SignIn() {
  const { state, signIn } = useAuth();
  const navigate = useNavigate();
  const location = useLocation();
  const from = (location.state as { from?: string; justSignedUp?: boolean } | null) ?? {};
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  if (state.status === 'signed-in') {
    return <Navigate to={from.from ?? '/'} replace />;
  }

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setError(null);
    setBusy(true);
    try {
      await signIn(username.trim(), password);
      navigate(from.from ?? '/', { replace: true });
    } catch (e) {
      setError(errorMessage(e));
      setPassword('');
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="auth-page">
      <form className="card" onSubmit={submit} noValidate>
        <h1>Sign in</h1>
        <p className="subtitle">md_analytics market dashboard</p>
        {from.justSignedUp && <p className="notice">Account created. Sign in to continue.</p>}
        <Field
          label="Username"
          name="username"
          autoComplete="username"
          autoFocus
          value={username}
          onChange={(e) => setUsername(e.target.value)}
        />
        <Field
          label="Password"
          name="password"
          type="password"
          autoComplete="current-password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
        />
        <FormError message={error} />
        <button type="submit" disabled={busy || !username || !password}>
          {busy ? 'Signing in…' : 'Sign in'}
        </button>
        <p className="switch">
          New here? <Link to="/signup">Create an account</Link>
        </p>
      </form>
    </div>
  );
}
