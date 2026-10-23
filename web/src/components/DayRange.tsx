import type { Quote } from '../stream/quoteStream';
import { formatPrice } from '../watchlists/format';

/** Low ─●─ High bar with the LTP marker, like Kite's day range. */
export function DayRange({ quote }: { quote: Quote | undefined }) {
  const { low, high, ltp } = quote ?? {};
  if (low === undefined || high === undefined || ltp === undefined || high <= 0) {
    return null;
  }
  const span = high - low;
  const at = span > 0 ? Math.min(100, Math.max(0, ((ltp - low) / span) * 100)) : 50;
  return (
    <div className="day-range" aria-label={`Day range ${formatPrice(low)} to ${formatPrice(high)}`}>
      <span className="muted">L {formatPrice(low)}</span>
      <div className="range-track">
        <div className="range-fill" style={{ width: `${at}%` }} />
        <div className="range-marker" style={{ left: `${at}%` }} />
      </div>
      <span className="muted">H {formatPrice(high)}</span>
    </div>
  );
}
