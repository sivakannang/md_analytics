// Step series from GET /chains/{u}/oi-timeline (docs/api/rest.md "OI timeline"): [m, value]
// = from 09:15 + m minutes the value is this, until the next point.

import type { StepSeries } from '../api/client';

/** A step series' value at minute index `m` (its last point at or before m); null before the first. */
export function valueAt(series: StepSeries, m: number): number | null {
  let lo = 0;
  let hi = series.length - 1;
  let found: number | null = null;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const point = series[mid]!;
    if (point[0] <= m) {
      found = point[1];
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return found;
}
