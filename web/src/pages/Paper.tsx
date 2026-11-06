// Paper trading (ADR-0029, docs/design/paper-trading.md §12): positions with live P&L,
// the day's orders and trades, funds and the kill switch. Everything is simulated by
// mda-broker; nothing reaches a real broker. Re-read whenever the "orders" stream reports a
// change, and funds every few seconds for the live P&L. With `viewUser` (an admin, from
// Admin → Users) it shows that user's account read-only: no order, exit or kill-switch
// controls, and polled, since the orders stream carries only one's own reports.

import { createContext, useCallback, useContext, useEffect, useMemo, useState, useSyncExternalStore } from 'react';
import type { FormEvent, ReactNode } from 'react';
import { useSearchParams } from 'react-router-dom';

import { api, errorMessage, isSessionLost } from '../api/client';
import type { Instrument, PaperFunds, PaperOrder, PaperPosition, PaperTrade } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { AutoRuns } from '../paper/AutoRuns';
import { useInstruments } from '../paper/instruments';
import { useOrderTicket } from '../paper/OrderTicket';
import { quoteStream } from '../stream/quoteStream';
import { useQuote, useWatch } from '../stream/useQuotes';
import { formatPrice, instrumentTitle } from '../watchlists/format';

const TABS = ['positions', 'orders', 'trades', 'funds', 'auto'] as const;
type Tab = (typeof TABS)[number];

const istTime = new Intl.DateTimeFormat('en-IN', {
  timeZone: 'Asia/Kolkata',
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  hour12: false,
});

const money = (value: number | null | undefined) =>
  value === null || value === undefined
    ? '—'
    : value.toLocaleString('en-IN', { minimumFractionDigits: 2, maximumFractionDigits: 2 });

const tone = (value: number | null | undefined) => (!value ? '' : value > 0 ? 'up' : 'down');

/** Today in IST as YYYY-MM-DD. */
function today() {
  return new Intl.DateTimeFormat('en-CA', { timeZone: 'Asia/Kolkata' }).format(new Date());
}

/** Whose account the page shows: undefined for one's own (with controls), else read-only. */
const ViewUser = createContext<number | undefined>(undefined);
const useReadOnly = () => useContext(ViewUser) !== undefined;

function usePaperData(date: string, user: number | undefined) {
  const { sessionEnded } = useAuth();
  const [positions, setPositions] = useState<PaperPosition[] | null>(null);
  const [orders, setOrders] = useState<PaperOrder[] | null>(null);
  const [trades, setTrades] = useState<PaperTrade[] | null>(null);
  const [funds, setFunds] = useState<PaperFunds | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(() => {
    Promise.all([api.paperPositions(user), api.paperOrders(date, user), api.paperTrades(date, user), api.paperFunds(user)])
      .then(([p, o, t, f]) => {
        setPositions(p);
        setOrders(o);
        setTrades(t);
        setFunds(f);
        setError(null);
      })
      .catch((e: unknown) => {
        if (isSessionLost(e)) sessionEnded();
        else setError(errorMessage(e));
      });
  }, [date, user, sessionEnded]);

  useEffect(() => {
    load();
    if (user !== undefined) {
      const every = setInterval(load, 5000);
      return () => clearInterval(every);
    }
    let timer: ReturnType<typeof setTimeout> | undefined;
    const stop = quoteStream.watchOrders(() => {
      clearTimeout(timer);
      timer = setTimeout(load, 250);  // a burst of reports → one re-read
    });
    const funds = setInterval(() => {
      api.paperFunds().then(setFunds).catch(() => undefined);
    }, 5000);
    return () => {
      stop();
      clearTimeout(timer);
      clearInterval(funds);
    };
  }, [load]);

  return { positions, orders, trades, funds, error, reload: load };
}

// --------------------------------------------------------------------- positions

/**
 * The trader's view, ignoring the daily mark that settles cash: (LTP − the price traded at) ×
 * Qty, at `last` (the live LTP, else the broker's last price), plus what closed today.
 */
export function positionPnl(position: PaperPosition, last: number | undefined): number {
  const open =
    position.quantity !== 0 && last !== undefined && position.entry_price !== null
      ? (last - position.entry_price) * position.quantity
      : 0;
  return (position.closed_pnl ?? 0) + open;
}

/** The sum over all positions at live prices; re-renders only when the total changes. */
function useTotalPnl(positions: PaperPosition[]): number {
  return useSyncExternalStore(quoteStream.subscribe, () =>
    positions.reduce(
      (sum, p) => sum + positionPnl(p, quoteStream.quote(p.instrument_id)?.ltp ?? p.last_price ?? undefined),
      0,
    ),
  );
}

/**
 * Converts lots of a position to the other product (MIS ↔ NRML), as Zerodha's position
 * conversion: same average, no charges.
 */
function ConvertForm({ position, instrument, onDone }: { position: PaperPosition; instrument: Instrument; onDone: () => void }) {
  const lot = Math.max(1, instrument.lot_size);
  const held = Math.abs(position.quantity) / lot;
  const to = position.product === 'MIS' ? 'NRML' : 'MIS';
  const [lots, setLots] = useState(String(held));
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const n = Number(lots);
    if (!(n > 0) || !Number.isInteger(n) || n > held) {
      setError(`1–${held} lots`);
      return;
    }
    setBusy(true);
    try {
      await api.convertPosition(position.instrument_id, n * lot, to);
      onDone();
    } catch (err) {
      setError(errorMessage(err));
      setBusy(false);
    }
  };
  return (
    <form className="convert-form" onSubmit={submit}>
      <input
        type="number"
        min={1}
        max={held}
        step={1}
        value={lots}
        onChange={(e) => setLots(e.target.value)}
        aria-label="Lots to convert"
      />
      <span className="muted">of {held} lots</span>
      <button type="submit" disabled={busy}>
        To {to}
      </button>
      <button type="button" className="link" onClick={onDone}>
        Cancel
      </button>
      {error && <span className="form-error">{error}</span>}
    </form>
  );
}

function PositionRow({ position, instrument }: { position: PaperPosition; instrument: Instrument | undefined }) {
  const openTicket = useOrderTicket();
  const readOnly = useReadOnly();
  const quote = useQuote(position.instrument_id);
  const [converting, setConverting] = useState(false);
  const last = quote?.ltp ?? position.last_price ?? undefined;
  const open = position.quantity !== 0;
  const pnl = positionPnl(position, last);
  if (converting && open && instrument) {
    return (
      <tr>
        <td>{position.product}</td>
        <td>{instrumentTitle(instrument)}</td>
        <td colSpan={5}>
          <ConvertForm position={position} instrument={instrument} onDone={() => setConverting(false)} />
        </td>
      </tr>
    );
  }
  return (
    <tr className={open ? '' : 'muted'}>
      <td>{position.product}</td>
      <td>{instrument ? instrumentTitle(instrument) : `#${position.instrument_id}`}</td>
      <td className={tone(position.quantity)}>{position.quantity}</td>
      <td>{formatPrice(position.entry_price ?? undefined)}</td>
      <td>{formatPrice(last)}</td>
      <td className={tone(pnl)}>{money(pnl)}</td>
      <td>
        {open && instrument && !readOnly && (
          <div className="row-actions">
            <button
              type="button"
              className="secondary"
              onClick={() =>
                openTicket({
                  instrument,
                  side: position.quantity > 0 ? 'SELL' : 'BUY',
                  quantity: Math.abs(position.quantity),
                  product: position.product,
                  type: 'MARKET',
                })
              }
            >
              Exit
            </button>
            <button
              type="button"
              className="secondary"
              title={`Convert to ${position.product === 'MIS' ? 'NRML (overnight)' : 'MIS (intraday)'}`}
              onClick={() => setConverting(true)}
            >
              Convert
            </button>
          </div>
        )}
      </td>
    </tr>
  );
}

function Positions({ positions: all, instruments }: { positions: PaperPosition[]; instruments: Map<number, Instrument> }) {
  // A flat row with nothing realised (left behind by a conversion) says nothing: hidden. A
  // closed trade stays, greyed, with its P&L, as Kite shows it.
  const positions = all.filter((p) => p.quantity !== 0 || (p.closed_pnl ?? 0) !== 0);
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<string | null>(null);
  const readOnly = useReadOnly();
  useWatch(positions.map((p) => p.instrument_id));
  const total = useTotalPnl(positions);
  const open = positions.filter((p) => p.quantity !== 0);
  if (positions.length === 0) {
    return (
      <p className="muted">
        {readOnly ? 'No positions.' : 'No positions yet. Use B / S on a futures or options row in your watchlist.'}
      </p>
    );
  }
  const exitAll = async () => {
    if (!window.confirm(`Exit all ${open.length} open positions at market?`)) return;
    setBusy(true);
    const results = await Promise.allSettled(
      open.map((p) =>
        api.placeOrder({
          instrument_id: p.instrument_id,
          side: p.quantity > 0 ? 'SELL' : 'BUY',
          quantity: Math.abs(p.quantity),
          type: 'MARKET',
          product: p.product,
          tag: 'exit all',
        }),
      ),
    );
    const failed = results.filter((r) => r.status === 'rejected').length;
    setNote(failed ? `${failed} exit order(s) failed; see Orders` : 'Exit orders sent');
    setBusy(false);
  };
  return (
    <>
      <div className="table-scroll">
        <table className="items paper-table">
        <thead>
          <tr>
            <th>Product</th>
            <th>Instrument</th>
            <th>Qty</th>
            <th title="The average price you traded at.">Avg.</th>
            <th>LTP</th>
            <th title="(LTP − Avg.) × Qty since you entered, before charges.">P&amp;L</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {positions.map((p) => (
            <PositionRow key={`${p.instrument_id}|${p.product}`} position={p} instrument={instruments.get(p.instrument_id)} />
          ))}
        </tbody>
        <tfoot>
          <tr className="paper-total">
            <td colSpan={5}>Total P&amp;L</td>
            <td className={tone(total)}>{money(total)}</td>
            <td />
          </tr>
        </tfoot>
      </table>
        </div>
      {open.length > 0 && !readOnly && (
        <p>
          <button type="button" className="secondary" disabled={busy} onClick={() => void exitAll()}>
            Exit all
          </button>{' '}
          {note && <span className="muted">{note}</span>}
        </p>
      )}
    </>
  );
}

// ------------------------------------------------------------------------ orders

function Orders({ orders, instruments, onChange }: { orders: PaperOrder[]; instruments: Map<number, Instrument>; onChange: () => void }) {
  const openTicket = useOrderTicket();
  const readOnly = useReadOnly();
  const [error, setError] = useState<string | null>(null);
  const live = orders.filter((o) => o.status === 'OPEN' || o.status === 'TRIGGER_PENDING');
  const done = orders.filter((o) => !live.includes(o)).reverse();
  const cancel = async (id: number) => {
    try {
      await api.cancelOrder(id);
      onChange();
    } catch (e: unknown) {
      setError(errorMessage(e));
    }
  };
  const row = (o: PaperOrder, actions: boolean) => {
    const instrument = instruments.get(o.instrument_id);
    return (
      <tr key={o.order_id}>
        <td>{istTime.format(o.created_at)}</td>
        <td>{o.placed_type === o.type ? o.type : `${o.placed_type} → ${o.type}`}</td>
        <td>
          {instrument ? instrumentTitle(instrument) : `#${o.instrument_id}`}
          {o.source !== 'ui' && (
            <span className="source-pill" title={o.source === 'auto' ? `Auto trader (${o.tag})` : 'Placed by the broker'}>
              {o.source === 'auto' ? 'AUTO' : 'SYSTEM'}
            </span>
          )}
        </td>
        <td className={o.side === 'BUY' ? 'buy-text' : 'sell-text'}>{o.side}</td>
        <td>{o.product}</td>
        <td>
          {o.filled} / {o.quantity}
        </td>
        <td>
          {o.price !== null ? formatPrice(o.price) : 'MKT'}
          {o.trigger_price !== null && <span className="muted"> trg {formatPrice(o.trigger_price)}</span>}
        </td>
        <td>{formatPrice(o.average_price ?? undefined)}</td>
        <td title={o.reason || undefined}>
          <span className={`order-status ${o.status.toLowerCase()}`}>{o.status.replace('_', ' ')}</span>
          {o.reason && o.status !== 'COMPLETE' && <span className="muted small"> {o.reason}</span>}
        </td>
        <td>
          {actions && instrument && !readOnly && (
            <>
              <button type="button" className="link" onClick={() => openTicket({ instrument, side: o.side, modify: o })}>
                Modify
              </button>{' '}
              <button type="button" className="link danger" onClick={() => void cancel(o.order_id)}>
                Cancel
              </button>
            </>
          )}
        </td>
      </tr>
    );
  };
  const head = (
    <thead>
      <tr>
        <th>Time</th>
        <th>Type</th>
        <th>Instrument</th>
        <th>Side</th>
        <th>Product</th>
        <th>Qty</th>
        <th>Price</th>
        <th>Avg.</th>
        <th>Status</th>
        <th />
      </tr>
    </thead>
  );
  return (
    <>
      {error && <p className="form-error">{error}</p>}
      <h3>Open orders ({live.length})</h3>
      {live.length > 0 ? (
        <div className="table-scroll">
        <table className="items paper-table">
          {head}
          <tbody>{live.map((o) => row(o, true))}</tbody>
        </table>
        </div>
      ) : (
        <p className="muted">None.</p>
      )}
      <h3>Executed and closed ({done.length})</h3>
      {done.length > 0 ? (
        <div className="table-scroll">
        <table className="items paper-table">
          {head}
          <tbody>{done.map((o) => row(o, false))}</tbody>
        </table>
        </div>
      ) : (
        <p className="muted">None on this day.</p>
      )}
    </>
  );
}

function Trades({ trades, instruments }: { trades: PaperTrade[]; instruments: Map<number, Instrument> }) {
  if (trades.length === 0) return <p className="muted">No trades on this day.</p>;
  return (
    <div className="table-scroll">
        <table className="items paper-table">
      <thead>
        <tr>
          <th>Time</th>
          <th>Instrument</th>
          <th>Side</th>
          <th>Product</th>
          <th>Qty</th>
          <th>Price</th>
          <th>Charges</th>
          <th>Realised</th>
        </tr>
      </thead>
      <tbody>
        {[...trades].reverse().map((t, i) => {
          const instrument = instruments.get(t.instrument_id);
          const c = t.charges;
          return (
            <tr key={`${t.order_id}-${i}`}>
              <td>{istTime.format(t.at)}</td>
              <td>{instrument ? instrumentTitle(instrument) : `#${t.instrument_id}`}</td>
              <td className={t.side === 'BUY' ? 'buy-text' : 'sell-text'}>{t.side}</td>
              <td>{t.product}</td>
              <td>{t.quantity}</td>
              <td>{formatPrice(t.price)}</td>
              <td
                title={`Brokerage ${money(c.brokerage)} · STT ${money(c.stt)} · Exchange ${money(c.exchange)} · SEBI ${money(c.sebi)} · Stamp ${money(c.stamp)} · GST ${money(c.gst)}`}
              >
                {money(c.total)}
              </td>
              <td className={tone(t.realised)}>{t.realised ? money(t.realised) : ''}</td>
            </tr>
          );
        })}
      </tbody>
    </table>
        </div>
  );
}

function Funds({ funds, onChange }: { funds: PaperFunds; onChange: () => void }) {
  const readOnly = useReadOnly();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const toggle = async () => {
    const on = !funds.kill_switch;
    if (on && !window.confirm('Turn the kill switch on? Open orders are cancelled and only exits are allowed until you turn it off.')) {
      return;
    }
    setBusy(true);
    try {
      await api.killSwitch(on);
      onChange();
    } catch (e: unknown) {
      setError(errorMessage(e));
    } finally {
      setBusy(false);
    }
  };
  const card = (label: string, value: number, className = '') => (
    <div className="funds-card">
      <span className="muted">{label}</span>
      <b className={className}>₹{money(value)}</b>
    </div>
  );
  return (
    <>
      <div className="funds-cards">
        {card('Available', funds.available)}
        {card('Margin blocked', funds.blocked)}
        {card('Cash', funds.cash)}
        {card('Opening cash', funds.opening_cash)}
        {card('Realised today', funds.realised_today, tone(funds.realised_today))}
        {card('Unrealised', funds.unrealised, tone(funds.unrealised))}
        {card('Charges today', funds.charges_today)}
      </div>
      <div className={`kill-switch ${funds.kill_switch ? 'on' : ''}`}>
        <div>
          <b>Kill switch {funds.kill_switch ? 'ON' : 'off'}</b>
          <p className="muted small">
            {funds.kill_switch
              ? 'Only orders that reduce a position are accepted.'
              : readOnly
                ? "The user's own switch: it cancels their open orders and allows only exits."
                : 'Cancels open orders and allows only exits until you turn it off.'}
          </p>
        </div>
        {!readOnly && (
          <button type="button" className={funds.kill_switch ? 'secondary' : 'danger'} disabled={busy} onClick={() => void toggle()}>
            {funds.kill_switch ? 'Turn off' : 'Turn on'}
          </button>
        )}
      </div>
      {error && <p className="form-error">{error}</p>}
    </>
  );
}

/** One's own paper account, or with `viewUser` (admins) another user's, read-only. */
export function Paper({ viewUser, title }: { viewUser?: number; title?: ReactNode } = {}) {
  const [search, setSearch] = useSearchParams();
  const tab: Tab = (TABS as readonly string[]).includes(search.get('tab') ?? '') ? (search.get('tab') as Tab) : 'positions';
  const [date, setDate] = useState(today());
  const { positions, orders, trades, funds, error, reload } = usePaperData(date, viewUser);
  const ids = useMemo(
    () => [
      ...(positions ?? []).map((p) => p.instrument_id),
      ...(orders ?? []).map((o) => o.instrument_id),
      ...(trades ?? []).map((t) => t.instrument_id),
    ],
    [positions, orders, trades],
  );
  const instruments = useInstruments(ids);

  return (
    <ViewUser.Provider value={viewUser}>
    <div className="page paper-page">
      {title}
      <header className="paper-header">
        <h2>
          Paper trading <span className="paper-pill">PAPER</span>
          {viewUser !== undefined && <> <span className="paper-pill">READ-ONLY</span></>}
        </h2>
        {funds?.kill_switch && <span className="order-status rejected">Kill switch on</span>}
        <span className="muted small">Simulated against the live market by mda-broker. No real orders.</span>
      </header>
      <nav className="instrument-tabs" aria-label="Paper trading">
        {TABS.map((t) => (
          <button
            key={t}
            type="button"
            className={`tab-link ${tab === t ? 'active' : ''}`}
            aria-current={tab === t ? 'page' : undefined}
            onClick={() => setSearch({ tab: t })}
          >
            {t.charAt(0).toUpperCase() + t.slice(1)}
          </button>
        ))}
        {(tab === 'orders' || tab === 'trades') && (
          <input type="date" className="paper-date" value={date} onChange={(e) => setDate(e.target.value || today())} />
        )}
      </nav>
      {error && <p className="form-error">{error}</p>}
      {!positions || !orders || !trades || !funds ? (
        <p className="loading">Loading…</p>
      ) : tab === 'positions' ? (
        <Positions positions={positions} instruments={instruments} />
      ) : tab === 'orders' ? (
        <Orders orders={orders} instruments={instruments} onChange={reload} />
      ) : tab === 'trades' ? (
        <Trades trades={trades} instruments={instruments} />
      ) : tab === 'auto' ? (
        <AutoRuns user={viewUser} />
      ) : (
        <Funds funds={funds} onChange={reload} />
      )}
    </div>
    </ViewUser.Provider>
  );
}
