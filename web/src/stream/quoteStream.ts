import type { ChainSummary } from '../api/client';

// One WebSocket to /api/v1/stream for the whole app (docs/api/websocket.md).
//
// Components ask for instruments with `watch(ids)` and read quotes from the store; the
// stream subscribes the union of what is being watched, reconnects with back-off and
// re-subscribes after a reconnect. Quotes live outside React so a tick re-renders only
// the rows that read the changed instrument (useSyncExternalStore).

export interface Quote {
  id: number;
  ltp?: number;
  change?: number;
  change_pct?: number;
  open?: number;
  high?: number;
  low?: number;
  close?: number;
  volume?: number;
  avg_price?: number;
  last_qty?: number;
  buy_qty?: number; // whole-book totals
  sell_qty?: number;
  oi?: number;
  oi_day_high?: number;
  oi_day_low?: number;
  exchange_time?: string;
  last_trade_time?: string;
  lower_circuit?: number; // today's price band (not for indices)
  upper_circuit?: number;
  // Options (ADR-0022): Greeks per point / per day / per 1% vol, IV in percent.
  iv?: number;
  delta?: number;
  gamma?: number;
  theta?: number;
  vega?: number;
  prev_oi?: number; // previous day's closing OI
  oi_change?: number;
  buildup?: 'long_buildup' | 'short_buildup' | 'short_covering' | 'long_unwinding';
}

export interface DepthLevel {
  price: number;
  qty: number;
  orders: number;
}

export interface Depth {
  id: number;
  bids: DepthLevel[]; // best first
  asks: DepthLevel[];
}

/** A live 1-minute bar from mda-engine (times in Unix seconds). */
export interface LiveBar {
  id: number;
  t: number;
  o: number;
  h: number;
  l: number;
  c: number;
  v: number;
  oi: number;
  oi_o: number;
  oi_h: number;
  oi_l: number;
  oi_n: number; // OI changes in the minute
  oi_t: number | null; // when OI last changed (receive time)
  forming: boolean;
}

export type FeedState = 'connecting' | 'connected' | 'disconnected' | 'login_required' | 'unavailable';

type ServerMessage =
  | { type: 'quotes'; data: Quote[] }
  | { type: 'depth'; data: Depth[] }
  | { type: 'bars'; data: LiveBar[] }
  | { type: 'chain'; data: ChainSummary[] }
  | { type: 'status'; data: { feed: Exclude<FeedState, 'connecting'> } }
  | { type: 'error'; message: string };

type StreamMode = 'quote' | 'depth' | 'bars' | 'chain';

const MAX_BACKOFF_MS = 15_000;
const SESSION_ENDED = 4001;

class QuoteStream {
  private socket: WebSocket | null = null;
  private quotes = new Map<number, Quote>();
  private depths = new Map<number, Depth>();
  private watchers = new Map<number, number>(); // id -> how many components watch it
  private depthWatchers = new Map<number, number>();
  private barWatchers = new Map<number, number>();
  private chainWatchers = new Map<number, number>(); // by spot id
  private summaries = new Map<string, ChainSummary>(); // `${spot_id}|${expiry}`
  private barListeners = new Map<number, Set<(bar: LiveBar) => void>>();
  private listeners = new Set<() => void>();
  private feed: FeedState = 'connecting';
  private backoff = 1000;
  private retryTimer: ReturnType<typeof setTimeout> | undefined;
  private closedForGood = false;

  // ---- store API (useSyncExternalStore)
  subscribe = (listener: () => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };
  quote = (id: number): Quote | undefined => this.quotes.get(id);
  feedState = (): FeedState => this.feed;
  depth = (id: number): Depth | undefined => this.depths.get(id);
  chainSummary = (spotId: number, expiry: string): ChainSummary | undefined =>
    this.summaries.get(`${spotId}|${expiry}`);

  /** Starts streaming `ids`; the returned function stops. Counts overlapping watchers. */
  watch(ids: readonly number[]): () => void {
    return this.track(this.watchers, ids, 'quote');
  }

  /** Calls `listener` with each live 1-minute bar of `id` (forming and closed) while subscribed. */
  watchBars(id: number, listener: (bar: LiveBar) => void): () => void {
    let listeners = this.barListeners.get(id);
    if (!listeners) {
      listeners = new Set();
      this.barListeners.set(id, listeners);
    }
    listeners.add(listener);
    const stop = this.track(this.barWatchers, [id], 'bars');
    return () => {
      listeners.delete(listener);
      if (listeners.size === 0) this.barListeners.delete(id);
      stop();
    };
  }

  /** Same as watch(), for the 5-level book (a separate, smaller subscription). */
  watchDepth(ids: readonly number[]): () => void {
    return this.track(this.depthWatchers, ids, 'depth');
  }

  /** Streams the engine's chain summaries (every expiry) of the chain whose spot is `spotId`. */
  watchChain(spotId: number): () => void {
    return this.track(this.chainWatchers, [spotId], 'chain');
  }

  private track(counts: Map<number, number>, ids: readonly number[], mode: StreamMode) {
    const fresh: number[] = [];
    for (const id of ids) {
      const count = counts.get(id) ?? 0;
      counts.set(id, count + 1);
      if (count === 0) fresh.push(id);
    }
    this.ensureOpen();
    if (fresh.length > 0) this.send({ a: 'subscribe', v: fresh, mode });

    return () => {
      const gone: number[] = [];
      for (const id of ids) {
        const count = (counts.get(id) ?? 1) - 1;
        if (count <= 0) {
          counts.delete(id);
          gone.push(id);
        } else {
          counts.set(id, count);
        }
      }
      if (gone.length > 0) {
        this.send({ a: 'unsubscribe', v: gone, mode });
        if (mode === 'depth') for (const id of gone) this.depths.delete(id);
      }
    };
  }

  /** Closes for good (sign-out). A later watch() opens a new connection. */
  close() {
    this.closedForGood = true;
    clearTimeout(this.retryTimer);
    this.socket?.close(1000);
    this.socket = null;
    this.quotes.clear();
    this.depths.clear();
    this.summaries.clear();
    this.setFeed('connecting');
  }

  // ---- connection
  private ensureOpen() {
    if (this.socket || this.retryTimer) return;
    this.closedForGood = false;
    const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const socket = new WebSocket(`${scheme}://${window.location.host}/api/v1/stream`);
    this.socket = socket;

    socket.onopen = () => {
      this.backoff = 1000;
      const ids = [...this.watchers.keys()];
      if (ids.length > 0) this.send({ a: 'subscribe', v: ids, mode: 'quote' });
      const books = [...this.depthWatchers.keys()];
      if (books.length > 0) this.send({ a: 'subscribe', v: books, mode: 'depth' });
      const charts = [...this.barWatchers.keys()];
      if (charts.length > 0) this.send({ a: 'subscribe', v: charts, mode: 'bars' });
      const chains = [...this.chainWatchers.keys()];
      if (chains.length > 0) this.send({ a: 'subscribe', v: chains, mode: 'chain' });
    };
    socket.onmessage = (event) => this.onMessage(event.data as string);
    socket.onclose = (event) => {
      this.socket = null;
      if (this.closedForGood) return;
      if (event.code === SESSION_ENDED) {
        // Signed out elsewhere: the next API call will notice and show the sign-in page.
        this.setFeed('unavailable');
        return;
      }
      this.setFeed('connecting');
      this.retryTimer = setTimeout(() => {
        this.retryTimer = undefined;
        if (this.watchers.size + this.depthWatchers.size + this.barWatchers.size + this.chainWatchers.size > 0) this.ensureOpen();
      }, this.backoff * (0.8 + Math.random() * 0.4));
      this.backoff = Math.min(this.backoff * 2, MAX_BACKOFF_MS);
    };
  }

  private send(message: {
    a: 'subscribe' | 'unsubscribe';
    v: number[];
    mode: StreamMode;
  }) {
    // Before open the subscription is sent by onopen (from `watchers`), so dropping is fine.
    if (this.socket?.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(message));
    }
  }

  private onMessage(text: string) {
    let message: ServerMessage;
    try {
      message = JSON.parse(text) as ServerMessage;
    } catch {
      return;
    }
    if (message.type === 'status') {
      this.setFeed(message.data.feed);
    } else if (message.type === 'quotes') {
      for (const update of message.data) {
        this.quotes.set(update.id, { ...this.quotes.get(update.id), ...update });
      }
      this.emit();
    } else if (message.type === 'bars') {
      for (const bar of message.data) {
        for (const listener of this.barListeners.get(bar.id) ?? []) listener(bar);
      }
    } else if (message.type === 'chain') {
      for (const summary of message.data) {
        this.summaries.set(`${summary.spot_id}|${summary.expiry}`, summary);
      }
      this.emit();
    } else if (message.type === 'depth') {
      for (const book of message.data) this.depths.set(book.id, book);
      this.emit();
    } else if (message.type === 'error') {
      console.warn('stream:', message.message);
    }
  }

  private setFeed(feed: FeedState) {
    if (feed !== this.feed) {
      this.feed = feed;
      this.emit();
    }
  }

  private emit() {
    for (const listener of this.listeners) listener();
  }
}

export const quoteStream = new QuoteStream();
