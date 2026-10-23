// One instrument in the watchlist: symbol, change, % and LTP (Kite's row), hover actions
// (depth, open, remove), drag handle, and the inline depth panel when expanded.
// The row is a drop target for the panel's pointer drag (data-drop / data-group / data-id).

import type { PointerEvent } from 'react';

import type { WatchlistItem } from '../api/client';
import { useQuote } from '../stream/useQuotes';
import { formatPrice, instrumentTitle } from '../watchlists/format';
import { changeOf, useDisplaySettings } from '../watchlists/settings';
import { MarketDepth } from './MarketDepth';

const KIND_TAG: Record<WatchlistItem['kind'], string> = { IDX: 'INDEX', EQ: '', FUT: '', OPT: '' };

interface Props {
  item: WatchlistItem;
  open: boolean; // shown in the right pane
  expanded: boolean; // depth visible
  dragging: boolean; // this row is being dragged (shown faded where it would land)
  groupId: number | null;
  onOpen: () => void;
  onToggleDepth: () => void;
  onRemove: () => void;
  onHandlePointerDown: (event: PointerEvent<HTMLElement>) => void;
}

export function WatchlistRow(props: Props) {
  const { item, open, expanded, dragging } = props;
  const quote = useQuote(item.instrument_id);
  const settings = useDisplaySettings();
  const change = changeOf(quote, settings.changeFrom);
  const direction = change === undefined || change.change === 0 ? '' : change.change > 0 ? 'up' : 'down';
  const title = instrumentTitle(item);
  const tag = KIND_TAG[item.kind] || (item.exchange === 'NSE' ? '' : item.exchange);

  return (
    <div
      className={['wl-item', open ? 'open' : '', expanded ? 'expanded' : '', dragging ? 'dragging' : '']
        .join(' ')
        .trim()}
      data-drop="item"
      data-id={item.instrument_id}
      data-group={props.groupId ?? undefined}
    >
      <div
        className={`wl-row ${item.listed ? '' : 'expired'}`}
        role="button"
        tabIndex={0}
        aria-label={`Open ${title}`}
        onClick={props.onOpen}
        onKeyDown={(e) => {
          if (e.key === 'Enter') props.onOpen();
          if (e.key === 'd' || e.key === 'D') props.onToggleDepth();
        }}
      >
        <span
          className="wl-handle"
          title="Drag to reorder"
          aria-hidden
          onPointerDown={props.onHandlePointerDown}
          onClick={(e) => e.stopPropagation()}
        >
          ⋮⋮
        </span>
        <span className={`wl-symbol ${direction}`} title={title}>
          {title}
          {tag && <span className="wl-tag">{tag}</span>}
          {!item.listed && <span className="wl-tag">EXPIRED</span>}
        </span>
        <span className="wl-numbers">
          {settings.showChange && (
            <span className="wl-change muted">
              {change === undefined ? '' : `${change.change > 0 ? '+' : ''}${formatPrice(change.change)}`}
            </span>
          )}
          {settings.showChangePct && (
            <span className="wl-pct">
              {change === undefined ? '' : `${change.pct > 0 ? '+' : ''}${change.pct.toFixed(2)}%`}
            </span>
          )}
          {settings.showDirection && (
            <span className={`wl-arrow ${direction}`} aria-hidden>
              {direction === 'up' ? '▲' : direction === 'down' ? '▼' : ''}
            </span>
          )}
          <span className={`wl-ltp ${direction}`}>{formatPrice(quote?.ltp)}
          </span>
        </span>
        <span className="wl-actions" onClick={(e) => e.stopPropagation()}>
          <button
            type="button"
            className={`icon-btn ${expanded ? 'active' : ''}`}
            title="Market depth (D)"
            aria-label={`Market depth for ${title}`}
            aria-expanded={expanded}
            onClick={props.onToggleDepth}
          >
            ≡
          </button>
          <button
            type="button"
            className="icon-btn"
            title="Open"
            aria-label={`Open ${title}`}
            onClick={props.onOpen}
          >
            ↗
          </button>
          <button
            type="button"
            className="icon-btn danger"
            title="Remove"
            aria-label={`Remove ${title}`}
            onClick={props.onRemove}
          >
            ×
          </button>
        </span>
      </div>
      {expanded && <MarketDepth instrument={item} />}
    </div>
  );
}
