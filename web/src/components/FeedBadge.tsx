import { useFeedState } from '../stream/useQuotes';
import type { FeedState } from '../stream/quoteStream';

const FEED_LABEL: Record<FeedState, string> = {
  connecting: 'Connecting…',
  connected: 'Live',
  disconnected: 'Reconnecting to broker…',
  login_required: 'Broker login needed',
  unavailable: 'Feed offline',
};

export function FeedBadge() {
  const feed = useFeedState();
  return (
    <span className={`feed feed-${feed}`} role="status" title="Market data feed">
      {FEED_LABEL[feed]}
    </span>
  );
}

