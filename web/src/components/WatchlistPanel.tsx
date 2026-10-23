// The left panel of the signed-in shell: the user's watchlists (docs/design/ui.md §3).
// - tabs switch lists; "+ New list" / Rename / Delete manage them
// - a list has named groups (collapsible, renamable, draggable) plus ungrouped items
// - search adds to the chosen group; rows show live prices, hover actions and inline depth
// - drag a row (by its ⋮⋮ handle) or a group header: a ghost follows the pointer, the list
//   rearranges live (the dragged row shows faded where it will land), releasing saves,
//   Esc cancels. Pointer events, not HTML5 drag-and-drop: works with mouse and touch and
//   has none of the native API's quirks (layout changes or DOM moves aborting a drag).
// - display settings: change from close/open, columns, sort within groups
// Arrangement changes apply at once and are saved with PUT /order; on failure we reload.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { FormEvent, PointerEvent as ReactPointerEvent, ReactNode } from 'react';
import { useMatch, useNavigate } from 'react-router-dom';

import { api, errorMessage, isSessionLost } from '../api/client';
import type { Instrument, Watchlist, WatchlistOrder } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { quoteStream } from '../stream/quoteStream';
import { useWatch } from '../stream/useQuotes';
import { instrumentTitle } from '../watchlists/format';
import { applyOrder, moveGroup, moveItem, sectionsOf, sortItems } from '../watchlists/order';
import type { SortKey } from '../watchlists/order';
import { changeOf, setDisplaySettings, useDisplaySettings } from '../watchlists/settings';
import { InstrumentSearch } from './InstrumentSearch';
import { WatchlistRow } from './WatchlistRow';

const MAX_LISTS = 10; // server limits (docs/api/rest.md)
const MAX_ITEMS = 250;
const MAX_NAME = 40;
const SELECTED_KEY = 'mda.watchlist.selected';
const COLLAPSED_KEY = 'mda.watchlist.collapsedGroups';
const UNGROUPED = 'ungrouped';

function load<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key);
    return raw === null ? fallback : (JSON.parse(raw) as T);
  } catch {
    return fallback;
  }
}

function save(key: string, value: unknown) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // remembering view state is only a convenience
  }
}

/** Inline name editor used for new list / rename list / new group / rename group. */
function NameForm({
  initial,
  submitLabel,
  placeholder,
  onSubmit,
  onCancel,
}: {
  initial: string;
  submitLabel: string;
  placeholder?: string;
  onSubmit: (name: string) => void;
  onCancel: () => void;
}) {
  const [name, setName] = useState(initial);
  const submit = (event: FormEvent) => {
    event.preventDefault();
    if (name.trim()) onSubmit(name.trim());
  };
  return (
    <form className="name-form" onSubmit={submit}>
      <input
        aria-label="Name"
        placeholder={placeholder}
        maxLength={MAX_NAME}
        value={name}
        autoFocus
        onFocus={(e) => e.target.select()}
        onChange={(e) => setName(e.target.value)}
        onKeyDown={(e) => e.key === 'Escape' && onCancel()}
      />
      <button type="submit" disabled={!name.trim()}>
        {submitLabel}
      </button>
      <button type="button" className="secondary" onClick={onCancel}>
        Cancel
      </button>
    </form>
  );
}

const SORTS: { key: SortKey; label: string }[] = [
  { key: 'pct', label: '%' },
  { key: 'ltp', label: 'LTP' },
  { key: 'name', label: 'A-Z' },
  { key: 'exchange', label: 'EXCH' },
];

function SettingsPopover({ onSort, onClose }: { onSort: (key: SortKey) => void; onClose: () => void }) {
  const settings = useDisplaySettings();
  const toggle = (key: 'showChange' | 'showChangePct' | 'showDirection', label: string) => (
    <label className="check">
      <input
        type="checkbox"
        checked={settings[key]}
        onChange={(e) => setDisplaySettings({ [key]: e.target.checked })}
      />
      {label}
    </label>
  );
  return (
    <div className="popover" role="dialog" aria-label="Watchlist settings">
      <div className="popover-row">
        <span className="popover-label">Change from</span>
        {(['close', 'open'] as const).map((from) => (
          <label className="check" key={from}>
            <input
              type="radio"
              name="change-from"
              checked={settings.changeFrom === from}
              onChange={() => setDisplaySettings({ changeFrom: from })}
            />
            {from === 'close' ? 'Prev. close' : "Today's open"}
          </label>
        ))}
      </div>
      <div className="popover-row">
        <span className="popover-label">Show</span>
        {toggle('showChange', 'Change')}
        {toggle('showChangePct', 'Change %')}
        {toggle('showDirection', 'Direction')}
      </div>
      <div className="popover-row">
        <span className="popover-label">Sort</span>
        {SORTS.map((s) => (
          <button key={s.key} type="button" className="secondary small" onClick={() => onSort(s.key)}>
            {s.label}
          </button>
        ))}
      </div>
      <p className="muted small">Sorts items within each group; click again to reverse.</p>
      <button type="button" className="link" onClick={onClose}>
        Close
      </button>
    </div>
  );
}

type Editing =
  | { kind: 'new-list' }
  | { kind: 'rename-list' }
  | { kind: 'new-group' }
  | { kind: 'rename-group'; groupId: number }
  | null;

type Dragging = { kind: 'item'; id: number } | { kind: 'group'; id: number } | null;

/** A drag from pointerdown on a handle until release; `active` once moved a few pixels. */
interface PointerDrag {
  what: NonNullable<Dragging>;
  startX: number;
  startY: number;
  active: boolean;
  label: string;
  width: number;
  cleanup: () => void;
}

/** The floating copy of the dragged row that follows the pointer. */
interface Ghost {
  x: number;
  y: number;
  label: string;
  width: number;
}

const DRAG_THRESHOLD = 4; // px before a press on a handle becomes a drag
const SCROLL_EDGE = 40; // px from the panel's edge where dragging auto-scrolls

export function WatchlistPanel() {
  const { sessionEnded } = useAuth();
  const navigate = useNavigate();
  const openId = Number(useMatch('/instrument/:instrumentId')?.params.instrumentId ?? 0);
  const settings = useDisplaySettings();
  const [lists, setLists] = useState<Watchlist[] | null>(null);
  const [selectedId, setSelectedId] = useState<number | null>(() => load(SELECTED_KEY, null));
  const [editing, setEditing] = useState<Editing>(null);
  const [error, setError] = useState<string | null>(null);
  const [expanded, setExpanded] = useState<Set<number>>(new Set());
  const [collapsed, setCollapsed] = useState<Set<number>>(() => new Set(load<number[]>(COLLAPSED_KEY, [])));
  const [targetGroup, setTargetGroup] = useState<number | null>(null);
  const [showSettings, setShowSettings] = useState(false);
  const [lastSort, setLastSort] = useState<{ key: SortKey; descending: boolean } | null>(null);
  const [dragging, setDragging] = useState<Dragging>(null);
  const [preview, setPreview] = useState<WatchlistOrder | null>(null); // while dragging
  const [ghost, setGhost] = useState<Ghost | null>(null);
  // The pointer drag in progress. Window listeners read these refs, not stale state.
  const drag = useRef<PointerDrag | null>(null);
  const previewRef = useRef<WatchlistOrder | null>(null);
  const selectedRef = useRef<Watchlist | null>(null);

  /** Runs one server call; a lost session signs out, other errors show above the list. */
  const run = useCallback(
    async <T,>(call: () => Promise<T>): Promise<T | undefined> => {
      setError(null);
      try {
        return await call();
      } catch (e) {
        if (isSessionLost(e)) sessionEnded();
        else setError(errorMessage(e));
        return undefined;
      }
    },
    [sessionEnded],
  );

  const reload = useCallback(async () => {
    const loaded = await run(api.watchlists);
    if (loaded) setLists(loaded);
  }, [run]);

  useEffect(() => {
    void reload();
  }, [reload]);


  const selected = useMemo(
    () => lists?.find((l) => l.id === selectedId) ?? lists?.[0] ?? null,
    [lists, selectedId],
  );
  // While dragging, show the arrangement the drop would produce.
  const shown = useMemo(
    () => (selected && preview ? applyOrder(selected, preview) : selected),
    [selected, preview],
  );
  const sections = useMemo(() => (shown ? sectionsOf(shown) : []), [shown]);
  selectedRef.current = selected;
  const added = useMemo(() => new Set(selected?.items.map((i) => i.instrument_id) ?? []), [selected]);
  useWatch(useMemo(() => [...added], [added]));

  // The "add to" group must exist in the open list.
  useEffect(() => {
    if (targetGroup !== null && !selected?.groups.some((g) => g.id === targetGroup)) setTargetGroup(null);
  }, [selected, targetGroup]);

  const select = (id: number) => {
    setSelectedId(id);
    save(SELECTED_KEY, id);
    setEditing(null);
  };

  /** Puts the server's copy of one list into state. */
  const replace = (list: Watchlist) =>
    setLists((current) => current?.map((l) => (l.id === list.id ? list : l)) ?? [list]);

  /** Shows the new arrangement at once, then saves it; reloads if the server refuses. */
  const reorder = async (order: WatchlistOrder) => {
    if (!selected) return;
    replace(applyOrder(selected, order));
    const saved = await run(() => api.setOrder(selected.id, order));
    if (saved) replace(saved);
    else await reload();
  };

  // ---------- list actions
  const createList = async (name: string) => {
    const list = await run(() => api.createWatchlist(name));
    if (list) {
      setLists((current) => [...(current ?? []), list]);
      select(list.id);
    }
  };
  const renameList = async (name: string) => {
    if (!selected) return;
    const list = await run(() => api.renameWatchlist(selected.id, name));
    if (list) {
      replace(list);
      setEditing(null);
    }
  };
  const deleteList = async () => {
    if (!selected || !lists) return;
    const count = selected.items.length;
    if (!window.confirm(`Delete "${selected.name}"${count ? ` and its ${count} instruments` : ''}?`)) return;
    if ((await run(() => api.deleteWatchlist(selected.id))) !== undefined) {
      const rest = lists.filter((l) => l.id !== selected.id);
      setLists(rest);
      if (rest[0]) select(rest[0].id);
    }
  };

  // ---------- group actions
  const createGroup = async (name: string) => {
    if (!selected) return;
    const list = await run(() => api.createGroup(selected.id, name));
    if (list) {
      replace(list);
      setEditing(null);
      setTargetGroup(list.groups.at(-1)?.id ?? null);
    }
  };
  const renameGroup = async (groupId: number, name: string) => {
    if (!selected) return;
    const list = await run(() => api.renameGroup(selected.id, groupId, name));
    if (list) {
      replace(list);
      setEditing(null);
    }
  };
  const deleteGroup = async (groupId: number, name: string) => {
    if (!selected) return;
    if (!window.confirm(`Delete group "${name}"? Its instruments stay in the list, ungrouped.`)) return;
    const list = await run(() => api.deleteGroup(selected.id, groupId));
    if (list) replace(list);
  };
  const toggleCollapsed = (groupId: number) => {
    setCollapsed((current) => {
      const next = new Set(current);
      if (next.has(groupId)) next.delete(groupId);
      else next.add(groupId);
      save(COLLAPSED_KEY, [...next]);
      return next;
    });
  };

  // ---------- item actions
  const add = async (instrument: Instrument) => {
    if (!selected) return;
    const list = await run(() => api.addToWatchlist(selected.id, instrument.instrument_id, targetGroup));
    if (list) replace(list);
  };
  const removeItem = async (instrumentId: number) => {
    if (!selected) return;
    const list = await run(() => api.removeFromWatchlist(selected.id, instrumentId));
    if (list) replace(list);
    setExpanded((current) => {
      const next = new Set(current);
      next.delete(instrumentId);
      return next;
    });
  };
  const toggleDepth = (instrumentId: number) =>
    setExpanded((current) => {
      const next = new Set(current);
      if (next.has(instrumentId)) next.delete(instrumentId);
      else next.add(instrumentId);
      return next;
    });

  const sort = (key: SortKey) => {
    if (!selected) return;
    const descending = lastSort?.key === key ? !lastSort.descending : key === 'pct' || key === 'ltp';
    setLastSort({ key, descending });
    void reorder(
      sortItems(selected, key, descending, quoteStream.quote, (q) => changeOf(q, settings.changeFrom)?.pct),
    );
  };

  // ---------- drag and drop (pointer events) with a live preview
  /** Shows `order` as the preview (skipping identical ones: moves fire constantly). */
  const showPreview = (order: WatchlistOrder) => {
    if (JSON.stringify(previewRef.current) === JSON.stringify(order)) return;
    previewRef.current = order;
    setPreview(order);
  };

  const endDrag = () => {
    drag.current?.cleanup();
    drag.current = null;
    previewRef.current = null;
    setDragging(null);
    setPreview(null);
    setGhost(null);
  };

  /** Ids of a section's items in the saved list, without the one being dragged. */
  const othersIn = (list: Watchlist, groupId: number | null, draggedId: number) =>
    sectionsOf(list)
      .find((s) => (s.group?.id ?? null) === groupId)
      ?.items.map((i) => i.instrument_id)
      .filter((id) => id !== draggedId) ?? [];

  /** Works out where the pointer would drop and previews it. Targets carry data-drop. */
  const previewAt = (x: number, y: number) => {
    const current = drag.current;
    const list = selectedRef.current;
    if (!current || !list) return;
    const target = document.elementFromPoint(x, y)?.closest<HTMLElement>('[data-drop]');
    if (!target) return; // e.g. between rows: keep the last preview
    const rect = target.getBoundingClientRect();
    const lower = y > rect.top + rect.height / 2;
    const kind = target.dataset.drop; // item | header | end
    const groupId = target.dataset.group ? Number(target.dataset.group) : null;
    const { what } = current;

    if (what.kind === 'item') {
      if (kind === 'item') {
        const targetId = Number(target.dataset.id);
        if (targetId === what.id) return; // over its own preview position
        const others = othersIn(list, groupId, what.id);
        const before = lower ? (others[others.indexOf(targetId) + 1] ?? null) : targetId;
        showPreview(moveItem(list, what.id, groupId, before));
      } else if (kind === 'header' && groupId !== null) {
        showPreview(moveItem(list, what.id, groupId, othersIn(list, groupId, what.id)[0] ?? null));
      } else if (kind === 'end') {
        showPreview(moveItem(list, what.id, groupId, null));
      }
    } else if (kind === 'header' && groupId !== null && groupId !== what.id) {
      const ids = list.groups.map((g) => g.id).filter((id) => id !== what.id);
      const before = lower ? (ids[ids.indexOf(groupId) + 1] ?? null) : groupId;
      showPreview(moveGroup(list, what.id, before));
    }
  };

  /** Scrolls the watchlist panel while the pointer is near its top or bottom edge. */
  const autoScroll = (y: number) => {
    const panel = document.querySelector<HTMLElement>('aside.panel');
    if (!panel) return;
    const rect = panel.getBoundingClientRect();
    if (y < rect.top + SCROLL_EDGE) panel.scrollTop -= 12;
    else if (y > rect.bottom - SCROLL_EDGE) panel.scrollTop += 12;
  };

  /** pointerdown on a ⋮⋮ handle: track the pointer on the window until release. */
  const startDrag =
    (what: NonNullable<Dragging>, label: string, rowSelector: string) =>
    (event: ReactPointerEvent<HTMLElement>) => {
      if (event.button !== 0 || drag.current) return;
      event.preventDefault(); // no text selection, no native drag
      event.stopPropagation(); // don't open the row
      const row = event.currentTarget.closest<HTMLElement>(rowSelector);

      const onMove = (e: PointerEvent) => {
        const current = drag.current;
        if (!current) return;
        if (!current.active) {
          if (Math.hypot(e.clientX - current.startX, e.clientY - current.startY) < DRAG_THRESHOLD) return;
          current.active = true;
          setDragging(current.what);
          document.body.classList.add('dragging-wl');
        }
        e.preventDefault();
        setGhost({ x: e.clientX, y: e.clientY, label: current.label, width: current.width });
        autoScroll(e.clientY);
        previewAt(e.clientX, e.clientY);
      };
      const onUp = () => {
        const order = drag.current?.active ? previewRef.current : null;
        endDrag();
        if (order) void reorder(order);
      };
      const onKey = (e: KeyboardEvent) => {
        if (e.key === 'Escape') endDrag(); // cancel: nothing saved
      };
      window.addEventListener('pointermove', onMove);
      window.addEventListener('pointerup', onUp);
      window.addEventListener('pointercancel', endDrag);
      window.addEventListener('keydown', onKey);
      drag.current = {
        what,
        startX: event.clientX,
        startY: event.clientY,
        active: false,
        label,
        width: row?.getBoundingClientRect().width ?? 300,
        cleanup: () => {
          window.removeEventListener('pointermove', onMove);
          window.removeEventListener('pointerup', onUp);
          window.removeEventListener('pointercancel', endDrag);
          window.removeEventListener('keydown', onKey);
          document.body.classList.remove('dragging-wl');
        },
      };
    };

  // Never leave window listeners behind.
  useEffect(() => () => drag.current?.cleanup(), []);

  /**
   * The list as one flat run of siblings (group header, its rows, its drop zone, next
   * header...). Drop targets are marked with data-drop / data-group / data-id.
   */
  const rows = (): ReactNode[] => {
    const out: ReactNode[] = [];
    for (const section of sections) {
      const group = section.group;
      const groupId = group?.id ?? null;
      const isCollapsed = groupId !== null && collapsed.has(groupId);
      if (group) {
        const renaming = editing?.kind === 'rename-group' && editing.groupId === group.id;
        out.push(
          <div
            key={`group-${group.id}`}
            className={`wl-group-header ${dragging?.kind === 'group' && dragging.id === group.id ? 'dragging' : ''}`}
            data-drop="header"
            data-group={group.id}
          >
            {renaming ? (
              <NameForm
                initial={group.name}
                submitLabel="Save"
                onSubmit={(name) => void renameGroup(group.id, name)}
                onCancel={() => setEditing(null)}
              />
            ) : (
              <>
                <span
                  className="wl-handle"
                  title="Drag to reorder groups"
                  aria-hidden
                  onPointerDown={startDrag({ kind: 'group', id: group.id }, group.name, '.wl-group-header')}
                >
                  ⋮⋮
                </span>
                <button
                  type="button"
                  className="wl-group-toggle"
                  aria-expanded={!isCollapsed}
                  onClick={() => toggleCollapsed(group.id)}
                >
                  <span aria-hidden>{isCollapsed ? '▸' : '▾'}</span> {group.name}{' '}
                  <span className="muted">({section.items.length})</span>
                </button>
                <span className="spacer" />
                <span className="wl-group-actions">
                  <button
                    type="button"
                    className="icon-btn"
                    title="Rename group"
                    aria-label={`Rename group ${group.name}`}
                    onClick={() => setEditing({ kind: 'rename-group', groupId: group.id })}
                  >
                    ✎
                  </button>
                  <button
                    type="button"
                    className="icon-btn danger"
                    title="Delete group"
                    aria-label={`Delete group ${group.name}`}
                    onClick={() => void deleteGroup(group.id, group.name)}
                  >
                    ×
                  </button>
                </span>
              </>
            )}
          </div>,
        );
      }
      if (isCollapsed) continue;
      for (const item of section.items) {
        out.push(
          <WatchlistRow
            key={`item-${item.instrument_id}`}
            item={item}
            open={item.instrument_id === openId}
            expanded={expanded.has(item.instrument_id)}
            dragging={dragging?.kind === 'item' && dragging.id === item.instrument_id}
            onOpen={() => navigate(`/instrument/${item.instrument_id}`)}
            onToggleDepth={() => toggleDepth(item.instrument_id)}
            onRemove={() => void removeItem(item.instrument_id)}
            groupId={groupId}
            onHandlePointerDown={startDrag(
              { kind: 'item', id: item.instrument_id },
              instrumentTitle(item),
              '.wl-item',
            )}
          />,
        );
      }
      // While dragging a row: a zone to drop at the end of each group (and to ungroup).
      if (dragging?.kind === 'item') {
        out.push(
          <div
            key={`end-${groupId ?? UNGROUPED}`}
            className="wl-drop-end"
            data-drop="end"
            data-group={groupId ?? undefined}
          >
            {section.items.length === 0 ? (group ? 'Drop here' : 'Drop here to ungroup') : ''}
          </div>,
        );
      }
    }
    return out;
  };

  if (lists === null) {
    return error ? (
      <p className="form-error banner" role="alert">
        {error}
      </p>
    ) : (
      <p className="loading">Loading watchlists…</p>
    );
  }

  return (
    <section className="watchlists">
      {ghost && (
        <div
          className="wl-ghost"
          style={{ left: ghost.x - 14, top: ghost.y - 16, width: ghost.width }}
          aria-hidden
        >
          <span className="wl-handle">⋮⋮</span> {ghost.label}
        </div>
      )}
      <div className="list-tabs" role="tablist" aria-label="Watchlists">
        {lists.map((list) => (
          <button
            key={list.id}
            type="button"
            role="tab"
            aria-selected={list.id === selected?.id}
            className={list.id === selected?.id ? 'tab active' : 'tab'}
            onClick={() => select(list.id)}
          >
            {list.name}
            <span className="count">{list.items.length}</span>
          </button>
        ))}
        {lists.length < MAX_LISTS && editing?.kind !== 'new-list' && (
          <button type="button" className="tab add-tab" onClick={() => setEditing({ kind: 'new-list' })}>
            + New list
          </button>
        )}
      </div>

      {editing?.kind === 'new-list' && (
        <NameForm initial="" submitLabel="Create" placeholder="List name" onSubmit={createList} onCancel={() => setEditing(null)} />
      )}

      {selected && (
        <div className="list-panel">
          {editing?.kind === 'rename-list' ? (
            <NameForm initial={selected.name} submitLabel="Save" onSubmit={renameList} onCancel={() => setEditing(null)} />
          ) : (
            <div className="list-header">
              <h2>{selected.name}</h2>
              <span className="muted small">
                {selected.items.length} / {MAX_ITEMS}
              </span>
              <span className="spacer" />
              <button type="button" className="link" onClick={() => setEditing({ kind: 'new-group' })}>
                + Group
              </button>
              <button type="button" className="link" onClick={() => setEditing({ kind: 'rename-list' })}>
                Rename
              </button>
              {lists.length > 1 && (
                <button type="button" className="link danger" onClick={deleteList}>
                  Delete
                </button>
              )}
            </div>
          )}

          {editing?.kind === 'new-group' && (
            <NameForm initial="" submitLabel="Add group" placeholder="Group name, e.g. Indices" onSubmit={createGroup} onCancel={() => setEditing(null)} />
          )}

          <div className="search-row">
            <InstrumentSearch added={added} onAdd={add} disabled={selected.items.length >= MAX_ITEMS} />
            <button
              type="button"
              className={`icon-btn settings-btn ${showSettings ? 'active' : ''}`}
              title="Display settings"
              aria-label="Display settings"
              aria-expanded={showSettings}
              onClick={() => setShowSettings((v) => !v)}
            >
              ⚙
            </button>
            {showSettings && <SettingsPopover onSort={sort} onClose={() => setShowSettings(false)} />}
          </div>
          {selected.groups.length > 0 && (
            <label className="add-to muted small">
              Add to
              <select
                value={targetGroup ?? ''}
                onChange={(e) => setTargetGroup(e.target.value === '' ? null : Number(e.target.value))}
              >
                <option value="">Ungrouped</option>
                {selected.groups.map((g) => (
                  <option key={g.id} value={g.id}>
                    {g.name}
                  </option>
                ))}
              </select>
            </label>
          )}

          {error && (
            <p className="form-error" role="alert">
              {error}
            </p>
          )}

          {selected.items.length === 0 && selected.groups.length === 0 ? (
            <p className="empty">
              This list is empty. Search above to add stocks, indices, futures or options. Use
              “+ Group” to organise them, like Kite.
            </p>
          ) : (
            <div className="wl-items">{rows()}</div>
          )}
        </div>
      )}
    </section>
  );
}
