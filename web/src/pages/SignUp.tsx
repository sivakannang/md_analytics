import { useState } from 'react';
import type { FormEvent } from 'react';
import { Link, Navigate, useNavigate } from 'react-router-dom';

import { api, errorMessage } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import {
  EMAIL_HINT,
  PASSWORD_HINT,
  PHONE_HINT,
  USERNAME_HINT,
  emailError,
  passwordError,
  phoneError,
  usernameError,
} from '../auth/validation';
import { Field, FormError } from '../components/Field';

export function SignUp() {
  const { state, signIn } = useAuth();
  const navigate = useNavigate();
  const [username, setUsername] = useState('');
  const [email, setEmail] = useState('');
  const [phone, setPhone] = useState('');
  const [password, setPassword] = useState('');
  const [confirm, setConfirm] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  if (state.status === 'signed-in') {
    return <Navigate to="/" replace />;
  }

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    const name = username.trim();
    const problem =
      usernameError(name) ??
      emailError(email) ??
      phoneError(phone) ??
      passwordError(password) ??
      (password !== confirm ? 'Passwords do not match' : null);
    if (problem) {
      setError(problem);
      return;
    }
    setError(null);
    setBusy(true);
    try {
      await api.signup({ username: name, email: email.trim(), phone, password });
    } catch (e) {
      setError(errorMessage(e));
      setBusy(false);
      return;
    }
    // Signing up doesn't sign in on the server; do it now for a smooth start.
    try {
      await signIn(name, password);
      navigate('/', { replace: true });
    } catch {
      navigate('/signin', { replace: true, state: { justSignedUp: true } });
    }
  };

  return (
    <div className="auth-page">
      <form className="card" onSubmit={submit} noValidate>
        <h1>Create account</h1>
        <p className="subtitle">md_analytics market dashboard</p>
        <Field
          label="Username"
          name="username"
          autoComplete="username"
          autoFocus
          hint={USERNAME_HINT}
          value={username}
          onChange={(e) => setUsername(e.target.value)}
        />
        <Field
          label="Email"
          name="email"
          type="email"
          autoComplete="email"
          inputMode="email"
          hint={EMAIL_HINT}
          value={email}
          onChange={(e) => setEmail(e.target.value)}
        />
        <Field
          label="Mobile number"
          name="phone"
          type="tel"
          autoComplete="tel-national"
          inputMode="tel"
          placeholder="98765 43210"
          hint={PHONE_HINT}
          value={phone}
          onChange={(e) => setPhone(e.target.value)}
        />
        <Field
          label="Password"
          name="password"
          type="password"
          autoComplete="new-password"
          hint={PASSWORD_HINT}
          value={password}
          onChange={(e) => setPassword(e.target.value)}
        />
        <Field
          label="Confirm password"
          name="confirm"
          type="password"
          autoComplete="new-password"
          value={confirm}
          onChange={(e) => setConfirm(e.target.value)}
        />
        <FormError message={error} />
        <button
          type="submit"
          disabled={busy || !username || !email || !phone || !password || !confirm}
        >
          {busy ? 'Creating account…' : 'Create account'}
        </button>
        <p className="switch">
          Already have an account? <Link to="/signin">Sign in</Link>
        </p>
      </form>
    </div>
  );
}
