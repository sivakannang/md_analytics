// 5-level market depth with the day's stats, laid out like Kite's depth panel:
//   one 6-column book (Bid | Orders | Qty | Offer | Orders | Qty) with a Total row,
//   an Open / Prev. close / Low / High box with the day range bar,
//   then two columns of "label  value" pairs.
// Subscribes to the book only while mounted (depth is a separate, smaller subscription).

import type { Instrument } from '../api/client';
import { useDepth, useQuote } from '../stream/useQuotes';
import { formatPrice, formatQuantity } from '../watchlists/format';

const LEVELS = 5;

/** 2026-10-22 10:51:05 in IST, like Kite. */
function dateTime(iso: string | undefined): string {
  if (!iso) return '—';
  return new Date(iso)
    .toLocaleString('sv-SE', { timeZone: 'Asia/Kolkata', hour12: false })
    .replace('T', ' ');
}

/** One "label  value" pair (label left, value right-aligned). */
function Pair({ label, value }: { label: string; value: string }) {
  return (
    <div className="pair">
      <span className="muted">{label}</span>
      <span>{value}</span>
    </div>
  );
}

function RangeBar({ low, high, open, ltp }: { low?: number; high?: number; open?: number; ltp?: number }) {
  if (low === undefined || high === undefined || high <= low) return <div className="range-track" />;
  const at = (v: number | undefined) =>
    v === undefined ? undefined : Math.min(100, Math.max(0, ((v - low) / (high - low)) * 100));
  const o = at(open);
  const l = at(ltp);
  const from = Math.min(o ?? 0, l ?? 0);
  const to = Math.max(o ?? 0, l ?? 0);
  const up = (ltp ?? 0) >= (open ?? 0);
  return (
    <div className="range-track" aria-hidden>
      {o !== undefined && l !== undefined && (
        <div className={`range-span ${up ? 'up' : 'down'}`} style={{ left: `${from}%`, width: `${to - from}%` }} />
      )}
      {o !== undefined && <div className="range-open" style={{ left: `${o}%` }} title="Open" />}
      {l !== undefined && <div className="range-ltp" style={{ left: `${l}%` }} title="LTP" />}
    </div>
  );
}

export function MarketDepth({ instrument }: { instrument: Instrument }) {
  const id = instrument.instrument_id;
  const depth = useDepth(instrument.kind === 'IDX' ? 0 : id);
  const quote = useQuote(id);
  const derivative = instrument.kind === 'FUT' || instrument.kind === 'OPT';
  const bids = depth?.bids ?? [];
  const asks = depth?.asks ?? [];
  const max = Math.max(1, ...bids.map((l) => l.qty), ...asks.map((l) => l.qty));
  const bar = (qty: number) => ({ width: `${(qty / max) * 100}%` });

  return (
    <div className="market-depth">
      {instrument.kind !== 'IDX' && (
        <table className="depth">
          <thead>
            <tr>
              <th>Bid</th>
              <th className="num">Orders</th>
              <th className="num">Qty.</th>
              <th className="offer">Offer</th>
              <th className="num">Orders</th>
              <th className="num">Qty.</th>
            </tr>
          </thead>
          <tbody>
            {Array.from({ length: LEVELS }, (_, i) => {
              const b = bids[i];
              const a = asks[i];
              return (
                <tr key={i}>
                  <td className="bid">{b ? formatPrice(b.price) : ''}</td>
                  <td className="num bid">{b?.orders ?? ''}</td>
                  <td className="num bid qty">
                    {b && <span className="qty-bar" style={bar(b.qty)} />}
                    {b ? formatQuantity(b.qty) : ''}
                  </td>
                  <td className="ask offer">{a ? formatPrice(a.price) : ''}</td>
                  <td className="num ask">{a?.orders ?? ''}</td>
                  <td className="num ask qty">
                    {a && <span className="qty-bar" style={bar(a.qty)} />}
                    {a ? formatQuantity(a.qty) : ''}
                  </td>
                </tr>
              );
            })}
            <tr className="total">
              <td className="bid">Total</td>
              <td />
              <td className="num bid">{formatQuantity(quote?.buy_qty)}</td>
              <td className="ask offer">Total</td>
              <td />
              <td className="num ask">{formatQuantity(quote?.sell_qty)}</td>
            </tr>
          </tbody>
        </table>
      )}

      <div className="ohlc-box">
        <div className="pairs">
          <Pair label="Open" value={formatPrice(quote?.open)} />
          <Pair label="Prev. close" value={formatPrice(quote?.close)} />
          <Pair label="Low" value={formatPrice(quote?.low)} />
          <Pair label="High" value={formatPrice(quote?.high)} />
        </div>
        <RangeBar low={quote?.low} high={quote?.high} open={quote?.open} ltp={quote?.ltp} />
      </div>

      {instrument.kind !== 'IDX' && (
        <div className="pairs stats-pairs">
          <Pair label="Volume" value={formatQuantity(quote?.volume)} />
          {/* 0 = not known (a last-known quote rebuilt from bars, ADR-0024) */}
          <Pair label="Avg. price" value={formatPrice(quote?.avg_price || undefined)} />
          <Pair label="Lower circuit" value={formatPrice(quote?.lower_circuit)} />
          <Pair label="Upper circuit" value={formatPrice(quote?.upper_circuit)} />
          <Pair label="LTQ" value={formatQuantity(quote?.last_qty || undefined)} />
          <Pair label="LTT" value={dateTime(quote?.last_trade_time)} />
          {derivative && (
            <>
              <Pair label="Expiry" value={instrument.expiry ?? '—'} />
              <Pair label="OI" value={formatQuantity(quote?.oi)} />
              <Pair label="OI day high" value={formatQuantity(quote?.oi_day_high)} />
              <Pair label="OI day low" value={formatQuantity(quote?.oi_day_low)} />
              <Pair label="Lot size" value={formatQuantity(instrument.lot_size)} />
            </>
          )}
        </div>
      )}
    </div>
  );
}
