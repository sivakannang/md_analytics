import { useCallback, useEffect, useState } from 'react';

import { errorMessage, isSessionLost } from '../api/client';
import { useAuth } from '../auth/AuthContext';

/**
 * Loads `load()` now and every `intervalMs` while the tab is visible. Returns the latest
 * data, the last error, and a function to reload at once (after an action).
 */
export function usePolling<T>(load: () => Promise<T>, intervalMs: number) {
  const { sessionEnded } = useAuth();
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setData(await load());
      setError(null);
    } catch (e) {
      if (isSessionLost(e)) sessionEnded();
      else setError(errorMessage(e));
    }
  }, [load, sessionEnded]);

  useEffect(() => {
    void refresh();
    const timer = setInterval(() => {
      if (document.visibilityState === 'visible') void refresh();
    }, intervalMs);
    return () => clearInterval(timer);
  }, [refresh, intervalMs]);

  return { data, error, refresh };
}
