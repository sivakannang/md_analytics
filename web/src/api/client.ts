// Typed client for the mda-gateway REST API (docs/api/rest.md).
//
// Every response uses the Kite-style envelope:
//   {"status": "success", "data": ...}
//   {"status": "error", "error_type": "...", "message": "..."}
// Success returns `data`; errors become an ApiError. Components never call fetch directly.

export type ErrorType =
  | 'InputException'
  | 'AuthException'
  | 'PermissionException'
  | 'NotFoundException'
  | 'ConflictException'
  | 'RateLimitException'
  | 'GeneralException'
  | 'NetworkException';

export class ApiError extends Error {
  readonly status: number;
  readonly errorType: ErrorType;

  constructor(status: number, errorType: ErrorType, message: string) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.errorType = errorType;
  }
}

export interface User {
  user_id: number;
  username: string;
  email: string;
  phone: string; // E.164, e.g. +919876543210
  email_verified: boolean;
  phone_verified: boolean;
  role: 'user' | 'admin';
  created_at: string; // ISO-8601 UTC
}

// ---------- option chains (ADR-0022)
export interface ChainInfo {
  underlying: string;
  spot_id: number;
  expiries: string[]; // YYYY-MM-DD, nearest first
}

/** Engine summary for one expiry; null fields are not known yet. */
export interface ChainSummary {
  underlying: string;
  spot_id: number;
  expiry: string;
  forward: number | null;
  atm_strike: number | null;
  atm_iv: number | null; // percent
  pcr: number;
  max_pain: number;
  call_oi: number;
  put_oi: number;
  days_to_expiry: number;
}

// ---------- market signals (ADR-0028, docs/api/rest.md "Market signals")
export type SignalLabel = 'bullish' | 'neutral' | 'bearish';
export type FactorStatus = 'present' | 'absent' | 'not_applicable';

export interface SignalFactor {
  factor: string;
  status: FactorStatus;
  value: number; // -1..1
  points: number; // share of the score
}

export interface SignalOutcome {
  spot_after: number | null;
  return_bp: number | null;
  hit: boolean | null; // null: neutral or flat
}

/** One stored signal (GET /signals/{underlying}). */
export interface SignalRow {
  minute: number; // Unix seconds, the minute's start
  strategy: string;
  horizon_min: number;
  score: number; // -100..100
  label: SignalLabel;
  prob: number | null;
  spot: number | null;
  factors: SignalFactor[];
  outcome: SignalOutcome | null; // null until the horizon has passed
}

export interface SignalIndex {
  underlying: string;
  spot: string;
  spot_id: number;
  latest: SignalRow[]; // the newest minute's, one per strategy
}

export interface SignalsInfo {
  indices: SignalIndex[];
  strategies: { id: string; horizon_min: number; bullish: number; bearish: number }[];
  factors: string[];
}

export interface SignalDay {
  underlying: string;
  date: string | null; // null: no signals yet
  signals: SignalRow[];
}

export interface TrackRecord {
  strategy: string;
  horizon_min: number;
  settled: number;
  bullish: number;
  bearish: number;
  neutral: number;
  hits: number;
  misses: number;
  flats: number;
  hit_rate: number | null;
  always_up_rate: number | null;
  momentum_rate: number | null;
  momentum_calls: number;
}

export interface SignalRecord {
  underlying: string;
  from: string;
  days: number;
  strategies: TrackRecord[];
}

/** A live signal from the stream (mode "signals"): the four largest factors only. */
export interface LiveSignal {
  spot_id: number;
  minute: number;
  strategy: string;
  horizon_min: number;
  score: number;
  label: SignalLabel;
  prob: number | null;
  top: { factor: string; points: number }[];
}

// ---------- paper trading (ADR-0029, docs/api/rest.md "Paper trading")

/** "?a=1&b=x" from the defined values; "" when there are none. */
function query(params: Record<string, string | number | undefined>): string {
  const pairs = Object.entries(params).filter((e): e is [string, string | number] => e[1] !== undefined);
  return pairs.length === 0 ? '' : `?${new URLSearchParams(pairs.map(([k, v]) => [k, String(v)])).toString()}`;
}
export type OrderSide = 'BUY' | 'SELL';
export type OrderType = 'MARKET' | 'LIMIT' | 'SL' | 'SL-M';
export type OrderProduct = 'MIS' | 'NRML';
export type OrderStatus = 'TRIGGER_PENDING' | 'OPEN' | 'COMPLETE' | 'CANCELLED' | 'REJECTED';

export interface PlaceOrder {
  instrument_id: number;
  side: OrderSide;
  quantity: number; // units, a multiple of the lot size
  type: OrderType;
  product: OrderProduct;
  price?: number;
  trigger_price?: number;
  tag?: string;
  client_order_id?: number; // the same id again returns the same order (safe retries)
}

/** An execution report (POST answers and the stream's "orders" mode). */
export interface OrderReport {
  order_id: number;
  client_order_id: number;
  instrument_id: number;
  event:
    | 'accepted' | 'rejected' | 'triggered' | 'fill' | 'cancelled' | 'modified' | 'status'
    | 'kill_switch_on' | 'kill_switch_off' | 'settled' | 'converted';
  status: OrderStatus;
  side: OrderSide;
  type: OrderType;
  product: OrderProduct;
  source: 'ui' | 'auto' | 'system';
  quantity: number;
  filled: number;
  last_quantity: number;
  last_price: number | null;
  average_price: number | null;
  price: number | null;
  trigger_price: number | null;
  charges: number; // this fill's, rupees
  reason: string;
  reject_code: string | null;
  at: number; // Unix ms
}

export interface PaperOrder {
  order_id: number;
  client_order_id: number;
  instrument_id: number;
  side: OrderSide;
  type: OrderType; // now (an SL becomes LIMIT once triggered)
  placed_type: OrderType;
  product: OrderProduct;
  quantity: number;
  filled: number;
  price: number | null;
  trigger_price: number | null;
  average_price: number | null;
  status: OrderStatus;
  reason: string;
  tag: string;
  source: 'ui' | 'auto' | 'system';
  charges: number;
  created_at: number; // Unix ms
  updated_at: number;
}

export interface PaperTrade {
  order_id: number;
  instrument_id: number;
  side: OrderSide;
  product: OrderProduct;
  quantity: number;
  price: number;
  charges: { brokerage: number; stt: number; exchange: number; sebi: number; stamp: number; gst: number; total: number };
  realised: number;
  at: number;
}

export interface PaperPosition {
  instrument_id: number;
  product: OrderProduct;
  quantity: number; // + long, - short, 0 closed
  average_price: number | null; // the last daily mark for a carried future
  entry_price: number | null; // what the fills paid, untouched by the marks
  last_price: number | null;
  realised: number;
  closed_pnl?: number; // today's closing fills from their entry price
  unrealised: number | null;
}

export interface PaperFunds {
  opening_cash: number;
  cash: number;
  blocked: number;
  available: number;
  realised_today: number;
  charges_today: number;
  unrealised: number;
  kill_switch: boolean;
}

/** One auto-trader run (GET /paper/auto, ADR-0030). Money in rupees. */
export interface AutoRun {
  id: string;
  strategy: string;
  signal: string;
  underlying: string;
  enabled: boolean;
  settings: {
    lots: number;
    product: OrderProduct;
    stop_points?: number;
    target_points: number;
    max_hold_min: number;
    max_trades_per_day: number;
    cooldown_min: number;
    exit_score?: number;
    daily_loss_stop: number;
    // follow-signal-options only (no stop_points there)
    index_stop_points?: number;
    premium_stop_percent?: number;
    entry_slip_ticks?: number;
    entry_timeout_s?: number;
  };
  position: number;
  /** What the run holds now, per instrument (an options run trades many contracts). */
  held: { instrument_id: number; quantity: number }[];
  entries_today: number;
  metrics: {
    round_trips: number;
    wins: number;
    losses: number;
    win_rate: number | null;
    gross: number;
    charges: number;
    net: number;
    average_win: number;
    average_loss: number;
    largest_loss: number;
    max_drawdown: number;
    exits: Record<string, number>;
  };
  decisions_today: { at: number; state: string; action: string; reason: string; price: number | null }[];
}

export interface AutoRuns {
  from: string;
  days: number;
  runs: AutoRun[];
}

/** POST answers: the order, or PENDING when mda-broker didn't answer in time. */
export type OrderAnswer = { order: OrderReport } | { client_order_id: number; status: 'PENDING' };

export interface ChainRow {
  strike: number;
  call: number | null; // instrument ids
  put: number | null;
}

export interface Chain {
  underlying: string;
  expiry: string;
  spot_id: number;
  lot_size: number;
  atm_strike: number;
  rows: ChainRow[];
  summary: ChainSummary | null;
}

/** One strike of GET /chains/{u}/oi-change: OI (shares) at `from` and `to`; null = no data. */
export interface OiChangeRow {
  strike: number;
  call_from: number | null;
  call_to: number | null;
  put_from: number | null;
  put_to: number | null;
}

/** OI by strike at two times (docs/design/oi-change-by-strike.md §4). Times are Unix seconds. */
export interface OiChange {
  underlying: string;
  expiries: string[];
  from: number;
  to: number;
  session_open: number;
  latest: number;
  oi_as_of: number | null;
  spot_from: number | null;
  spot_to: number;
  atm_strike: number;
  totals: { call_from: number; call_to: number; put_from: number; put_to: number };
  pcr: number | null;
  rows: OiChangeRow[];
}

/** A step series: [m, value] = from the session open + m minutes the value is this (rest.md "OI timeline"). */
export type StepSeries = [number, number][];

/** The session's OI per strike as step series (design oi-change-by-strike.md §8). */
export interface OiTimeline {
  underlying: string;
  expiries: string[];
  session_open: number;
  latest: number;
  oi_as_of: number | null;
  atm_strike: number;
  spot: StepSeries;
  rows: { strike: number; call: StepSeries; put: StepSeries }[];
}

// ---------- system monitor (docs/design/system-monitor.md; rest.md "System monitor")
export type CheckLevel = 'ok' | 'warn' | 'bad';
export interface SystemCheck {
  card: string;
  level: CheckLevel;
  text: string;
}
export interface LatencySummary {
  count: number;
  p50: number;
  p99: number;
  max: number;
}
export interface SystemReport {
  time: number;
  market_hours: boolean;
  status: CheckLevel;
  checks: SystemCheck[];
  processes: {
    name: string;
    running: boolean;
    pid?: number;
    started?: number | null;
    cpu_percent?: number | null;
    rss_bytes?: number;
  }[];
  machine: {
    load1: number | null;
    cores: number;
    memory_used: number | null;
    memory_total: number | null;
    power: 'ac' | 'battery' | null;
    battery_percent: number | null;
    uptime_s: number | null;
  };
  disk: {
    free?: number;
    total?: number;
    files: { bars_db: number; app_db: number; last_quotes_db: number };
    journal_today: number;
    journal_all: number;
  };
  buses: {
    name: string;
    available: boolean;
    capacity?: number;
    writer_alive?: boolean;
    writer_pid?: number;
    heartbeat_age_ms?: number | null;
    bytes_per_s?: number;
    messages_per_s?: number;
    stale_slots?: number;
    readers?: {
      name: string;
      pid: number;
      lag_bytes: number;
      lag_percent: number;
      laps: number;
      lost_bytes: number;
      heartbeat_age_ms: number | null;
    }[];
  }[];
  feed: {
    state: string | null;
    subscribed: number;
    reconnects: number;
    ticks_per_s: number;
    frames_per_s: number;
    last_tick_age_ms: number | null;
  };
  latency: { feed_to_gateway_us: LatencySummary; exchange_to_feed_s: LatencySummary };
  engine: { bars_per_min: number | null; last_bar: number | null; analytics_per_s: number };
  recorder: {
    enabled: boolean;
    running: boolean;
    journal_today: number;
    journal_all: number;
    gaps_today?: number;
    records_last_minute?: number | null;
    status_age_s?: number;
  };
  notifier: {
    queued: number | null;
    sent_today: number | null;
    failed_today: number | null;
    oldest_queued_s: number | null;
  };
  history: {
    step_s: number;
    start: number;
    load1: number[];
    ticks_per_s: number[];
    md_max_lag_pct: number[];
    feed_to_gateway_p99_us: number[];
  };
}

// ---------- chart history (ADR-0021)
export type BarInterval = '1m' | '3m' | '5m' | '15m' | '30m' | '1h' | '1d';
/** OI buildup state of a bar (docs/design/oi-buildup-chart.md §1); null when unknown. */
export type Buildup =
  | 'long_buildup'
  | 'short_buildup'
  | 'short_covering'
  | 'long_unwinding'
  | 'neutral'
  | 'no_oi_update';
/**
 * [time (Unix s), open, high, low, close, volume, oi (close), oi_open, oi_high, oi_low,
 *  oi_updates, oi_time (Unix s) | null, buildup | null, oi_change | null] — docs/api/rest.md.
 */
export type BarRow = [
  number, number, number, number, number, number, number,
  number, number, number, number, number | null, Buildup | null, number | null,
];
export interface BarHistory {
  instrument_id: number;
  interval: BarInterval;
  bars: BarRow[];
}

// ---------- admin (ADR-0019)
export interface AdminUsage {
  ws_bytes: number;
  ws_messages: number;
  http_bytes: number;
  http_requests: number;
  total_bytes: number;
}

export interface AdminUser {
  user_id: number;
  username: string;
  email: string; // masked
  phone: string; // masked
  role: 'user' | 'admin';
  created_at: string;
  last_sign_in_at: string | null;
  last_sign_out_at: string | null;
  last_seen_at: string | null;
  active_sessions: number;
  online: number; // open live streams
  streaming_instruments: number;
  today: AdminUsage;
}

export interface AdminUserDetail {
  user: AdminUser;
  sessions: { id: string; created_at: string; last_seen_at: string; user_agent: string; ip: string }[];
  events: { type: string; at: string; ip: string; user_agent: string }[];
  usage: (AdminUsage & { day: string })[];
  connections: { since: string; instruments: number }[];
}

export interface SignupRequest {
  username: string;
  email: string;
  phone: string;
  password: string;
}

/** A tradable or index instrument (GET /instruments/...). */
export interface Instrument {
  instrument_id: number; // md_analytics' permanent id
  exchange: string; // NSE, BSE, MCX
  market: string; // CASH, FO (MCX futures and options are FO)
  kind: 'IDX' | 'EQ' | 'FUT' | 'OPT';
  underlying: string;
  trading_symbol: string;
  name: string;
  expiry: string | null; // YYYY-MM-DD
  strike: number | null;
  option_type: 'CE' | 'PE' | null;
  lot_size: number;
  tick_size: number;
  listed: boolean; // false once expired or delisted
}

export interface WatchlistItem extends Instrument {
  added_at: string;
  group_id: number | null; // null = ungrouped
}

export interface WatchlistGroup {
  id: number;
  name: string;
}

export interface Watchlist {
  id: number;
  name: string;
  created_at: string;
  groups: WatchlistGroup[]; // display order
  items: WatchlistItem[]; // display order (grouped by group_id on screen)
}

/** A complete arrangement for PUT /watchlists/{id}/order. */
export interface WatchlistOrder {
  groups: number[];
  items: { instrument_id: number; group_id: number | null }[];
}

type Envelope<T> =
  | { status: 'success'; data: T }
  | { status: 'error'; error_type: ErrorType; message: string };

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  let response: Response;
  try {
    response = await fetch(`/api/v1${path}`, {
      method,
      credentials: 'same-origin', // send the HttpOnly session cookie
      headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    throw new ApiError(0, 'NetworkException', 'Cannot reach the server. Is mda-gateway running?');
  }

  let envelope: Envelope<T>;
  try {
    envelope = (await response.json()) as Envelope<T>;
  } catch {
    throw new ApiError(
      response.status,
      'GeneralException',
      response.status === 502 || response.status === 504
        ? 'Cannot reach the server. Is mda-gateway running?'
        : `Unexpected response (HTTP ${response.status})`,
    );
  }
  if (envelope.status === 'success') {
    return envelope.data;
  }
  throw new ApiError(response.status, envelope.error_type, envelope.message);
}

/** The signed-in user's notification channels (GET /notifications). */
export interface NotificationChannels {
  email: { address: string; verified: boolean; enabled: boolean };
  telegram: { available: boolean; linked: boolean; enabled: boolean };
  phone_verified: boolean;
}

export const api = {
  signup: (fields: SignupRequest) => request<User>('POST', '/user/signup', fields),

  signin: (username: string, password: string) =>
    request<Pick<User, 'user_id' | 'username'>>('POST', '/session', { username, password }),

  signout: () => request<boolean>('DELETE', '/session'),

  profile: () => request<User>('GET', '/user/profile'),

  changePassword: (currentPassword: string, newPassword: string) =>
    request<boolean>('PUT', '/user/password', {
      current_password: currentPassword,
      new_password: newPassword,
    }),

  deleteAccount: (password: string) => request<boolean>('DELETE', '/user', { password }),

  // Account → Notifications (docs/design/notifications.md, N5)
  notifications: () => request<NotificationChannels>('GET', '/notifications'),
  sendEmailCode: () => request<{ expires_in: number }>('POST', '/notifications/email/code', {}),
  verifyEmail: (code: string) => request<boolean>('POST', '/notifications/email/verify', { code }),
  telegramLink: () => request<{ url: string; expires_in: number }>('POST', '/notifications/telegram/link', {}),
  telegramUnlink: () => request<boolean>('DELETE', '/notifications/telegram'),
  setChannel: (channel: 'email' | 'telegram', enabled: boolean) =>
    request<boolean>('PUT', `/notifications/${channel}`, { enabled }),

  searchInstruments: (query: string, limit = 20) =>
    request<Instrument[]>(
      'GET',
      `/instruments/search?q=${encodeURIComponent(query)}&limit=${limit}`,
    ),

  instrument: (id: number) => request<Instrument>('GET', `/instruments/${id}`),

  watchlists: () => request<Watchlist[]>('GET', '/watchlists'),

  createWatchlist: (name: string) => request<Watchlist>('POST', '/watchlists', { name }),

  renameWatchlist: (id: number, name: string) =>
    request<Watchlist>('PUT', `/watchlists/${id}`, { name }),

  deleteWatchlist: (id: number) => request<boolean>('DELETE', `/watchlists/${id}`),

  addToWatchlist: (id: number, instrumentId: number, groupId: number | null = null) =>
    request<Watchlist>('POST', `/watchlists/${id}/items`, {
      instrument_id: instrumentId,
      ...(groupId === null ? {} : { group_id: groupId }),
    }),

  createGroup: (id: number, name: string) =>
    request<Watchlist>('POST', `/watchlists/${id}/groups`, { name }),

  renameGroup: (id: number, groupId: number, name: string) =>
    request<Watchlist>('PUT', `/watchlists/${id}/groups/${groupId}`, { name }),

  deleteGroup: (id: number, groupId: number) =>
    request<Watchlist>('DELETE', `/watchlists/${id}/groups/${groupId}`),

  setOrder: (id: number, order: WatchlistOrder) =>
    request<Watchlist>('PUT', `/watchlists/${id}/order`, order),

  bars: (id: number, interval: BarInterval, from?: number, to?: number) =>
    request<BarHistory>(
      'GET',
      `/instruments/${id}/bars?interval=${interval}` +
        (from === undefined ? '' : `&from=${from}`) +
        (to === undefined ? '' : `&to=${to}`),
    ),

  chains: () => request<ChainInfo[]>('GET', '/chains'),

  placeOrder: (order: PlaceOrder) => request<OrderAnswer>('POST', '/paper/orders', order),

  modifyOrder: (orderId: number, change: { quantity: number; price?: number; trigger_price?: number }) =>
    request<OrderAnswer>('PUT', `/paper/orders/${orderId}`, change),

  cancelOrder: (orderId: number) => request<OrderAnswer>('DELETE', `/paper/orders/${orderId}`),

  killSwitch: (on: boolean) => request<{ kill_switch: boolean }>('POST', '/paper/kill-switch', { on }),
  /** Moves `quantity` (units) of a position into `to` from the other product, as Zerodha's conversion. */
  convertPosition: (instrument_id: number, quantity: number, to: OrderProduct) =>
    request<{ order: OrderReport }>('POST', '/paper/positions/convert', { instrument_id, quantity, to }),

  // `user`: an admin reading another user's paper account (rest.md "Paper trading").
  paperOrders: (date?: string, user?: number) => request<PaperOrder[]>('GET', `/paper/orders${query({ date, user })}`),

  paperTrades: (date?: string, user?: number) => request<PaperTrade[]>('GET', `/paper/trades${query({ date, user })}`),

  paperPositions: (user?: number) => request<PaperPosition[]>('GET', `/paper/positions${query({ user })}`),

  paperFunds: (user?: number) => request<PaperFunds>('GET', `/paper/funds${query({ user })}`),

  paperAuto: (days: number, user?: number) => request<AutoRuns>('GET', `/paper/auto${query({ days, user })}`),

  signals: () => request<SignalsInfo>('GET', '/signals'),

  signalDay: (underlying: string, date?: string) =>
    request<SignalDay>(
      'GET',
      `/signals/${encodeURIComponent(underlying)}${date ? `?date=${date}` : ''}`,
    ),

  signalRecord: (underlying: string, days: number) =>
    request<SignalRecord>('GET', `/signals/${encodeURIComponent(underlying)}/record?days=${days}`),

  chain: (underlying: string, expiry: string, strikes: number) =>
    request<Chain>(
      'GET',
      `/chains/${encodeURIComponent(underlying)}/${expiry}?strikes=${strikes}`,
    ),

  oiChange: (
    underlying: string,
    query: { expiries: string[]; strikes: number; from?: number; to?: number; minutes?: number },
  ) =>
    request<OiChange>(
      'GET',
      `/chains/${encodeURIComponent(underlying)}/oi-change?expiries=${query.expiries.join(',')}` +
        `&strikes=${query.strikes}` +
        (query.from === undefined ? '' : `&from=${query.from}`) +
        (query.minutes === undefined ? '' : `&minutes=${query.minutes}`) +
        (query.to === undefined ? '' : `&to=${query.to}`),
    ),

  oiTimeline: (underlying: string, query: { expiries: string[]; strikes: number }) =>
    request<OiTimeline>(
      'GET',
      `/chains/${encodeURIComponent(underlying)}/oi-timeline?expiries=${query.expiries.join(',')}` +
        `&strikes=${query.strikes}`,
    ),

  adminUsers: () => request<AdminUser[]>('GET', '/admin/users'),

  adminUser: (id: number) => request<AdminUserDetail>('GET', `/admin/users/${id}`),

  revokeSession: (sessionId: string) => request<boolean>('DELETE', `/admin/sessions/${sessionId}`),

  adminSystem: () => request<SystemReport>('GET', '/admin/system'),

  removeFromWatchlist: (id: number, instrumentId: number) =>
    request<Watchlist>('DELETE', `/watchlists/${id}/items/${instrumentId}`),
};

/** True when the session is gone (expired, signed out elsewhere, account deleted). */
export function isSessionLost(error: unknown): boolean {
  return error instanceof ApiError && error.status === 401 && error.message === 'not signed in';
}

/** Message for showing an error to the user. */
export function errorMessage(error: unknown): string {
  if (error instanceof ApiError) {
    return error.message;
  }
  return 'Something went wrong. Please try again.';
}
