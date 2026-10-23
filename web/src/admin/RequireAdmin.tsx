import type { ReactNode } from 'react';

import { useAuth } from '../auth/AuthContext';
import { RequireAuth } from '../components/RequireAuth';

/** Signed-in admins only. The server enforces this too (403); this just explains it. */
export function RequireAdmin({ children }: { children: ReactNode }) {
  const { state } = useAuth();
  return (
    <RequireAuth>
      {state.status === 'signed-in' && state.user.role !== 'admin' ? (
        <div className="page">
          <p className="form-error">This page is for admins.</p>
        </div>
      ) : (
        children
      )}
    </RequireAuth>
  );
}
