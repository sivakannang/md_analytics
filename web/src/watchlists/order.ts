// Pure helpers for the arrangement of a watchlist: sections on screen, and the complete
// order sent to PUT /watchlists/{id}/order after a drag or a sort.

import type { Watchlist, WatchlistGroup, WatchlistItem, WatchlistOrder } from '../api/client';
import type { Quote } from '../stream/quoteStream';

export interface Section {
  group: WatchlistGroup | null; // null = ungrouped (shown first, without a header)
  items: WatchlistItem[];
}

/** Ungrouped items first, then each group in order; items keep their list order. */
export function sectionsOf(list: Watchlist): Section[] {
  const sections: Section[] = [{ group: null, items: [] }];
  const byGroup = new Map<number, Section>();
  for (const group of list.groups) {
    const section = { group, items: [] };
    byGroup.set(group.id, section);
    sections.push(section);
  }
  for (const item of list.items) {
    (item.group_id === null ? sections[0] : (byGroup.get(item.group_id) ?? sections[0]))?.items.push(
      item,
    );
  }
  return sections;
}

function orderOf(sections: Section[]): WatchlistOrder {
  return {
    groups: sections.flatMap((s) => (s.group ? [s.group.id] : [])),
    items: sections.flatMap((s) =>
      s.items.map((item) => ({ instrument_id: item.instrument_id, group_id: s.group?.id ?? null })),
    ),
  };
}

/**
 * Moves an item into `groupId` (null = ungrouped), before `beforeId` or at the group's
 * end when `beforeId` is null.
 */
export function moveItem(
  list: Watchlist,
  instrumentId: number,
  groupId: number | null,
  beforeId: number | null,
): WatchlistOrder {
  const sections = sectionsOf(list);
  let moving: WatchlistItem | undefined;
  for (const section of sections) {
    const at = section.items.findIndex((i) => i.instrument_id === instrumentId);
    if (at >= 0) [moving] = section.items.splice(at, 1);
  }
  const target = sections.find((s) => (s.group?.id ?? null) === groupId);
  if (moving && target) {
    const at = beforeId === null ? -1 : target.items.findIndex((i) => i.instrument_id === beforeId);
    target.items.splice(at < 0 ? target.items.length : at, 0, moving);
  }
  return orderOf(sections);
}

/** Moves a group before `beforeGroupId` (or to the end). */
export function moveGroup(
  list: Watchlist,
  groupId: number,
  beforeGroupId: number | null,
): WatchlistOrder {
  const sections = sectionsOf(list);
  const from = sections.findIndex((s) => s.group?.id === groupId);
  if (from > 0) {
    const [moving] = sections.splice(from, 1);
    const at = beforeGroupId === null ? -1 : sections.findIndex((s) => s.group?.id === beforeGroupId);
    if (moving) sections.splice(at < 1 ? sections.length : at, 0, moving);
  }
  return orderOf(sections);
}

export type SortKey = 'pct' | 'ltp' | 'name' | 'exchange';

/** Sorts items within every section (like Kite: "sort items within a group"). */
export function sortItems(
  list: Watchlist,
  key: SortKey,
  descending: boolean,
  quoteOf: (id: number) => Quote | undefined,
  pctOf: (quote: Quote | undefined) => number | undefined,
): WatchlistOrder {
  const value = (item: WatchlistItem): number | string => {
    switch (key) {
      case 'pct':
        return pctOf(quoteOf(item.instrument_id)) ?? Number.NEGATIVE_INFINITY;
      case 'ltp':
        return quoteOf(item.instrument_id)?.ltp ?? Number.NEGATIVE_INFINITY;
      case 'name':
        return item.trading_symbol;
      case 'exchange':
        return `${item.exchange} ${item.trading_symbol}`;
    }
  };
  const sections = sectionsOf(list).map((section) => ({
    ...section,
    items: [...section.items].sort((a, b) => {
      const x = value(a);
      const y = value(b);
      const cmp = typeof x === 'string' ? x.localeCompare(String(y)) : x - (y as number);
      return descending ? -cmp : cmp;
    }),
  }));
  return orderOf(sections);
}

/** The list as it will look once the server accepts `order` (for instant feedback). */
export function applyOrder(list: Watchlist, order: WatchlistOrder): Watchlist {
  const groups = new Map(list.groups.map((g) => [g.id, g]));
  const items = new Map(list.items.map((i) => [i.instrument_id, i]));
  return {
    ...list,
    groups: order.groups.flatMap((id) => groups.get(id) ?? []),
    items: order.items.flatMap(({ instrument_id, group_id }) => {
      const item = items.get(instrument_id);
      return item ? [{ ...item, group_id }] : [];
    }),
  };
}
