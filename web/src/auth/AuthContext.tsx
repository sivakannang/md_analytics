// Who is signed in, shared across the app. On load we ask the gateway for the profile:
// the session cookie is HttpOnly, so JavaScript can't read it — the server is the only
// source of truth for "am I signed in?".

import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react';
import type { ReactNode } from 'react';

import { ApiError, api } from '../api/client';
import { quoteStream } from '../stream/quoteStream';
import type { User } from '../api/client';

type AuthState =
  | { status: 'loading' }
  | { status: 'signed-out' }
  | { status: 'signed-in'; user: User };

interface AuthContextValue {
  state: AuthState;
  signIn: (username: string, password: string) => Promise<void>;
  signOut: () => Promise<void>;
  /** Marks the session as gone (e.g. an API call returned AuthException). */
  sessionEnded: () => void;
  /** Re-reads the profile (e.g. after the email or phone was verified). */
  refreshProfile: () => Promise<void>;
}

const AuthContext = createContext<AuthContextValue | null>(null);

export function AuthProvider({ children }: { children: ReactNode }) {
  const [state, setState] = useState<AuthState>({ status: 'loading' });

  useEffect(() => {
    let cancelled = false;
    api
      .profile()
      .then((user) => !cancelled && setState({ status: 'signed-in', user }))
      .catch(() => !cancelled && setState({ status: 'signed-out' }));
    return () => {
      cancelled = true;
    };
  }, []);

  const signIn = useCallback(async (username: string, password: string) => {
    await api.signin(username, password);
    const user = await api.profile();
    setState({ status: 'signed-in', user });
  }, []);

  const signOut = useCallback(async () => {
    try {
      await api.signout();
    } catch (error) {
      // Already signed out on the server is fine; anything else is surfaced.
      if (!(error instanceof ApiError && error.errorType === 'AuthException')) {
        throw error;
      }
    }
    quoteStream.close();
    setState({ status: 'signed-out' });
  }, []);

  const sessionEnded = useCallback(() => {
    quoteStream.close();
    setState({ status: 'signed-out' });
  }, []);

  const refreshProfile = useCallback(async () => {
    const user = await api.profile();
    setState({ status: 'signed-in', user });
  }, []);

  const value = useMemo(
    () => ({ state, signIn, signOut, sessionEnded, refreshProfile }),
    [state, signIn, signOut, sessionEnded, refreshProfile],
  );
  return <AuthContext.Provider value={value}>{children}</AuthContext.Provider>;
}

export function useAuth(): AuthContextValue {
  const context = useContext(AuthContext);
  if (context === null) {
    throw new Error('useAuth must be used inside <AuthProvider>');
  }
  return context;
}
