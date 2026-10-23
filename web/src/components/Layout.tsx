// Signed-in shell, Kite's "Default" workspace (docs/design/ui.md §3):
//   header: brand, live index strip, navigation, feed state, user
//   body:   watchlist panel (left, resizable, collapsible) | routed page (right)
// The panel lives here, not in a page, so it keeps its state and live stream while the
// right pane changes. Ctrl/⌘+K (or /) focuses the instrument search, like Kite.

import { useCallback, useEffect, useRef, useState } from 'react';
import type { PointerEvent as ReactPointerEvent } from 'react';
import { Link, NavLink, Outlet, useNavigate } from 'react-router-dom';

import { errorMessage } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { IndexStrip } from './IndexStrip';
import { FeedBadge } from './FeedBadge';
import { WatchlistPanel } from './WatchlistPanel';

const MIN_WIDTH = 300;
const MAX_WIDTH = 640;
const DEFAULT_WIDTH = 440; // Kite's marketwatch column is ~455 px
const WIDTH_KEY = 'mda.panel.width';
const COLLAPSED_KEY = 'mda.panel.collapsed';

function load(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function save(key: string, value: string) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // remembering the layout is only a convenience
  }
}

function clampWidth(width: number): number {
  return Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, Math.round(width)));
}

export function Layout() {
  const { state, signOut } = useAuth();
  const navigate = useNavigate();
  const [error, setError] = useState<string | null>(null);
  const [width, setWidth] = useState(() => clampWidth(Number(load(WIDTH_KEY)) || DEFAULT_WIDTH));
  const [collapsed, setCollapsed] = useState(() => load(COLLAPSED_KEY) === '1');
  const dragging = useRef<{ startX: number; startWidth: number } | null>(null);

  const handleSignOut = async () => {
    try {
      await signOut();
      navigate('/');
    } catch (e) {
      setError(errorMessage(e));
    }
  };

  const setPanelCollapsed = useCallback((value: boolean) => {
    setCollapsed(value);
    save(COLLAPSED_KEY, value ? '1' : '0');
  }, []);

  // Ctrl/⌘+K or "/" (outside text fields) focuses the instrument search.
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      const typing = event.target instanceof HTMLElement && event.target.closest('input, textarea');
      const shortcut =
        ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') ||
        (event.key === '/' && !typing);
      if (!shortcut) return;
      event.preventDefault();
      setPanelCollapsed(false);
      // after the panel is shown
      requestAnimationFrame(() =>
        document.querySelector<HTMLInputElement>('input[aria-label="Search instruments"]')?.focus(),
      );
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [setPanelCollapsed]);

  // Drag the divider to resize; double-click resets.
  const onDragStart = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.currentTarget.setPointerCapture(event.pointerId);
    dragging.current = { startX: event.clientX, startWidth: width };
  };
  const onDragMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (dragging.current) {
      setWidth(clampWidth(dragging.current.startWidth + event.clientX - dragging.current.startX));
    }
  };
  const onDragEnd = () => {
    if (dragging.current) {
      dragging.current = null;
      save(WIDTH_KEY, String(width));
    }
  };

  return (
    <div className="app">
      <header className="appbar">
        <Link to="/" className="brand">
          md_analytics
        </Link>
        <IndexStrip />
        <nav>
          <NavLink to="/" end>
            Dashboard
          </NavLink>
          <NavLink to="/chain">Option chain</NavLink>
          {state.status === 'signed-in' && state.user.role === 'admin' && (
            <NavLink to="/admin">Admin</NavLink>
          )}
          <NavLink to="/account">Account</NavLink>
        </nav>
        <FeedBadge />
        <div className="topbar-user">
          {state.status === 'signed-in' && <span>{state.user.username}</span>}
          <button type="button" className="secondary" onClick={handleSignOut}>
            Sign out
          </button>
        </div>
      </header>
      {error && (
        <p className="form-error banner" role="alert">
          {error}
        </p>
      )}
      <div className="workspace">
        {collapsed && (
          <button
            type="button"
            className="panel-expand"
            title="Show watchlist (Ctrl+K)"
            aria-label="Show watchlist"
            onClick={() => setPanelCollapsed(false)}
          >
            ›
          </button>
        )}
        {/* Hidden, not unmounted, when collapsed: it keeps its lists and live stream. */}
        <aside className="panel" style={{ width }} aria-label="Watchlist" hidden={collapsed}>
          <button
            type="button"
            className="panel-collapse"
            title="Hide watchlist"
            aria-label="Hide watchlist"
            onClick={() => setPanelCollapsed(true)}
          >
            ‹
          </button>
          <WatchlistPanel />
        </aside>
        {!collapsed && (
          <div
            className="panel-resizer"
            role="separator"
            aria-orientation="vertical"
            aria-label="Resize watchlist"
            title="Drag to resize, double-click to reset"
            onPointerDown={onDragStart}
            onPointerMove={onDragMove}
            onPointerUp={onDragEnd}
            onPointerCancel={onDragEnd}
            onDoubleClick={() => {
              setWidth(DEFAULT_WIDTH);
              save(WIDTH_KEY, String(DEFAULT_WIDTH));
            }}
          />
        )}
        <main className="pane">
          <Outlet />
        </main>
      </div>
    </div>
  );
}
