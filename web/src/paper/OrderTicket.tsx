// The paper order ticket (docs/design/paper-trading.md §12): opened from B / S on a watchlist
// row, the instrument page or a position, Kite's layout with a PAPER label. It places (or
// modifies) through the gateway; the estimates come from paper/estimate.ts. The provider also
// shows short notes for fills and for orders the broker cancels, from the "orders" stream.

import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import type { FormEvent, ReactNode } from 'react';

import { ApiError, api, errorMessage, isSessionLost } from '../api/client';
import type { Instrument, OrderProduct, OrderReport, OrderSide, OrderType, PaperOrder } from '../api/client';
import { useAuth } from '../auth/AuthContext';
import { quoteStream } from '../stream/quoteStream';
import { useQuote, useWatch } from '../stream/useQuotes';
import { formatPrice, instrumentTitle } from '../watchlists/format';
import { estimateCharges, estimateMargin, onTick } from './estimate';
import { loadInstrument } from './instruments';

export interface TicketRequest {
  instrument: Instrument;
  side: OrderSide;
  quantity?: number; // units; default one lot
  product?: OrderProduct;
  type?: OrderType;
  modify?: PaperOrder; // modify this open order instead of placing one
}

type Notify = (text: string, tone?: 'ok' | 'warn') => void;

const TicketContext = createContext<(request: TicketRequest) => void>(() => undefined);

/** Opens the order ticket. */
export function useOrderTicket() {
  return useContext(TicketContext);
}

const TYPES: OrderType[] = ['MARKET', 'LIMIT', 'SL', 'SL-M'];

/** A client order id for one submission: retries of it are the same order. */
function freshClientId() {
  return Date.now() * 1000 + Math.floor(Math.random() * 1000);
}

function rupees(value: number) {
  return `₹${value.toLocaleString('en-IN', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`;
}

function OrderTicket({ request, onClose, notify }: { request: TicketRequest; onClose: () => void; notify: Notify }) {
  const { instrument, modify } = request;
  const { sessionEnded } = useAuth();
  const lot = Math.max(1, instrument.lot_size);
  const [side, setSide] = useState<OrderSide>(modify?.side ?? request.side);
  const [product, setProduct] = useState<OrderProduct>(modify?.product ?? request.product ?? 'NRML');
  const [type, setType] = useState<OrderType>(modify?.type ?? request.type ?? 'MARKET');
  const [lots, setLots] = useState(String(Math.max(1, Math.round((modify?.quantity ?? request.quantity ?? lot) / lot))));
  const [price, setPrice] = useState(modify?.price != null ? String(modify.price) : '');
  const [trigger, setTrigger] = useState(modify?.trigger_price != null ? String(modify.trigger_price) : '');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const clientId = useRef(freshClientId());
  useWatch([instrument.instrument_id]);
  const quote = useQuote(instrument.instrument_id);
  const ltp = quote?.ltp;

  // Start a LIMIT at the last price, once it is known.
  useEffect(() => {
    if (price === '' && ltp !== undefined && !modify) setPrice(String(onTick(ltp, instrument.tick_size)));
  }, [ltp, price, modify, instrument.tick_size]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);

  const quantity = (Number(lots) || 0) * lot;
  const needsPrice = type === 'LIMIT' || type === 'SL';
  const needsTrigger = type === 'SL' || type === 'SL-M';
  const reference = needsPrice ? Number(price) || 0 : (ltp ?? 0);  // MARKET: at the last price
  const margin = estimateMargin(instrument, side, quantity, reference);
  const charges = estimateCharges(instrument, side, quantity, reference);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    if (!(quantity > 0)) {
      setError('Enter the number of lots');
      return;
    }
    setBusy(true);
    setError(null);
    const priceValue = needsPrice ? Number(price) : undefined;
    const triggerValue = needsTrigger ? Number(trigger) : undefined;
    try {
      const answer = modify
        ? await api.modifyOrder(modify.order_id, { quantity, price: priceValue, trigger_price: triggerValue })
        : await api.placeOrder({
            instrument_id: instrument.instrument_id,
            side,
            quantity,
            type,
            product,
            price: priceValue,
            trigger_price: triggerValue,
            client_order_id: clientId.current,
          });
      if ('order' in answer) {
        notify(modify ? `Order #${answer.order.order_id} modified` : `Paper order #${answer.order.order_id} placed`);
      } else {
        notify('Order sent; waiting for the paper broker', 'warn');
      }
      onClose();
    } catch (err: unknown) {
      if (isSessionLost(err)) {
        sessionEnded();
        return;
      }
      setError(errorMessage(err));
      // A definite answer (rejected): the next submit is a new order. A network failure
      // keeps the id, so trying again can't place it twice.
      if (!(err instanceof ApiError && err.status === 0)) clientId.current = freshClientId();
    } finally {
      setBusy(false);
    }
  };

  const title = instrumentTitle(instrument);
  return (
    <div className="ticket-backdrop" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <form className={`ticket ${side === 'BUY' ? 'buy' : 'sell'}`} onSubmit={submit} role="dialog" aria-label={`${side} ${title}`}>
        <header className="ticket-head">
          <div>
            <b>
              {side === 'BUY' ? 'Buy' : 'Sell'} {title}
            </b>{' '}
            <span className="ticket-exchange">{instrument.exchange}</span>
            <div className="ticket-ltp">LTP {formatPrice(ltp)}</div>
          </div>
          <span className="paper-pill" title="Simulated: no real order is placed">
            PAPER
          </span>
          {!modify && (
            <button
              type="button"
              className="ticket-side"
              onClick={() => setSide(side === 'BUY' ? 'SELL' : 'BUY')}
              title="Switch between buy and sell"
            >
              {side === 'BUY' ? 'B' : 'S'} ⇄
            </button>
          )}
        </header>

        <div className="ticket-body">
          <div className="ticket-products" role="radiogroup" aria-label="Product">
            {(['MIS', 'NRML'] as const).map((p) => (
              <label key={p} className={product === p ? 'active' : ''}>
                <input type="radio" name="product" checked={product === p} disabled={!!modify} onChange={() => setProduct(p)} />
                {p === 'MIS' ? 'Intraday' : 'Overnight'} <span className="muted">{p}</span>
              </label>
            ))}
          </div>

          <div className="ticket-fields">
            <label>
              Lots
              <input
                type="number"
                min={1}
                step={1}
                value={lots}
                onChange={(e) => setLots(e.target.value)}
                autoFocus
              />
              <span className="muted small">
                {quantity} qty · lot {lot}
              </span>
            </label>
            <label>
              Price
              <input
                type="number"
                step={instrument.tick_size || 0.05}
                value={needsPrice ? price : ''}
                placeholder={needsPrice ? '' : 'Market'}
                disabled={!needsPrice}
                onChange={(e) => setPrice(e.target.value)}
              />
            </label>
            <label>
              Trigger
              <input
                type="number"
                step={instrument.tick_size || 0.05}
                value={needsTrigger ? trigger : ''}
                disabled={!needsTrigger}
                onChange={(e) => setTrigger(e.target.value)}
              />
            </label>
          </div>

          <div className="chart-intervals ticket-types" role="group" aria-label="Order type">
            {TYPES.map((t) => (
              <button
                key={t}
                type="button"
                className={type === t ? 'active' : ''}
                disabled={!!modify && t !== type}
                onClick={() => setType(t)}
              >
                {t}
              </button>
            ))}
          </div>

          {error && (
            <p className="form-error" role="alert">
              {error}
            </p>
          )}
        </div>

        <footer className="ticket-foot">
          <span className="muted small" title="Estimates: the paper broker's own figures are what count">
            Margin ≈ {rupees(margin)} · Charges ≈ {rupees(charges)}
          </span>
          <span className="ticket-buttons">
            <button type="submit" className="ticket-submit" disabled={busy}>
              {busy ? 'Sending…' : modify ? 'Modify' : side === 'BUY' ? 'Buy' : 'Sell'}
            </button>
            <button type="button" className="secondary" onClick={onClose}>
              Cancel
            </button>
          </span>
        </footer>
      </form>
    </div>
  );
}

interface Toast {
  id: number;
  text: string;
  tone: 'ok' | 'warn';
}

/** Tells the user what happened to their paper orders after placing them. */
async function describe(report: OrderReport): Promise<{ text: string; tone: 'ok' | 'warn' } | null> {
  const symbol = await loadInstrument(report.instrument_id)
    .then((i) => instrumentTitle(i))
    .catch(() => `#${report.instrument_id}`);
  if (report.event === 'fill') {
    return {
      text: `${report.side === 'BUY' ? 'Bought' : 'Sold'} ${report.last_quantity} ${symbol} at ${formatPrice(report.last_price ?? undefined)} (paper)`,
      tone: 'ok',
    };
  }
  if (report.event === 'triggered') return { text: `Stop triggered: ${symbol}`, tone: 'ok' };
  if (report.event === 'converted') {
    return { text: `Converted ${report.last_quantity} ${symbol} to ${report.product} (paper)`, tone: 'ok' };
  }
  if (report.event === 'cancelled' && report.reason !== 'cancelled by the user') {
    return { text: `Order #${report.order_id} cancelled: ${report.reason}`, tone: 'warn' };
  }
  return null;
}

export function OrderTicketProvider({ children }: { children: ReactNode }) {
  const [ticket, setTicket] = useState<TicketRequest | null>(null);
  const [toasts, setToasts] = useState<Toast[]>([]);
  const nextId = useRef(1);

  const notify = useCallback<Notify>((text, tone = 'ok') => {
    const id = nextId.current++;
    setToasts((list) => [...list.slice(-3), { id, text, tone }]);
    setTimeout(() => setToasts((list) => list.filter((t) => t.id !== id)), 6000);
  }, []);

  useEffect(
    () =>
      quoteStream.watchOrders((report) => {
        void describe(report).then((d) => d && notify(d.text, d.tone));
      }),
    [notify],
  );

  const close = useCallback(() => setTicket(null), []);
  const open = useMemo(() => (request: TicketRequest) => setTicket(request), []);

  return (
    <TicketContext.Provider value={open}>
      {children}
      {ticket && <OrderTicket key={ticket.instrument.instrument_id} request={ticket} onClose={close} notify={notify} />}
      <div className="toasts" aria-live="polite">
        {toasts.map((t) => (
          <div key={t.id} className={`toast ${t.tone}`}>
            {t.text}
          </div>
        ))}
      </div>
    </TicketContext.Provider>
  );
}
