import { useEffect, useSyncExternalStore } from 'react';

import { quoteStream } from './quoteStream';
import type { ChainSummary } from '../api/client';
import type { Depth, FeedState, Quote } from './quoteStream';

/** Streams live quotes for `ids` while the calling component is mounted. */
export function useWatch(ids: readonly number[]) {
  const key = [...ids].sort((a, b) => a - b).join(',');
  useEffect(() => {
    const list = key === '' ? [] : key.split(',').map(Number);
    return quoteStream.watch(list);
  }, [key]);
}

/** The latest quote of one instrument (re-renders only when it changes). */
export function useQuote(id: number): Quote | undefined {
  return useSyncExternalStore(quoteStream.subscribe, () => quoteStream.quote(id));
}

/** Streams the 5-level book of `id` while mounted (pass 0 for none). */
export function useDepth(id: number): Depth | undefined {
  useEffect(() => (id > 0 ? quoteStream.watchDepth([id]) : undefined), [id]);
  return useSyncExternalStore(quoteStream.subscribe, () => quoteStream.depth(id));
}

export function useFeedState(): FeedState {
  return useSyncExternalStore(quoteStream.subscribe, quoteStream.feedState);
}

/** Streams the engine's summaries of the chain on `spotId` (0 for none); returns one expiry's. */
export function useChainSummary(spotId: number, expiry: string): ChainSummary | undefined {
  useEffect(() => (spotId > 0 ? quoteStream.watchChain(spotId) : undefined), [spotId]);
  return useSyncExternalStore(quoteStream.subscribe, () => quoteStream.chainSummary(spotId, expiry));
}

/** The largest open interest among `ids` (0 until known); re-renders only when it changes. */
export function useMaxOi(ids: readonly number[]): number {
  return useSyncExternalStore(quoteStream.subscribe, () => {
    let max = 0;
    for (const id of ids) max = Math.max(max, quoteStream.quote(id)?.oi ?? 0);
    return max;
  });
}
