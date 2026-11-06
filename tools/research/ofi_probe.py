#!/usr/bin/env python3
"""Probe: do order flow imbalance and queue imbalance predict index-future returns?

Throwaway research (signals.md §13 candidates), not production. Reads the tick journal
(ADR-0026) read-only and, for each index future, measures the correlation between
microstructure features and the future's mid-price return over the next 10 s, 1 min and
5 min. Also prints the overlap between the live factors in signals.db.

Features, on a 1-second grid from the depth snapshots (Kite: 5 levels, ~1 update a second):
  qi1      queue imbalance at the best level  (bid - ask) / (bid + ask)
  qi5      the same over 5 levels
  micro    micro-price offset from mid, in ticks of half-spread: shows qi1 redundancy
  ofi_10s  order flow imbalance (Cont, Kukanov, Stoikov 2014), best level, last 10 s,
  ofi_60s  divided by the day's mean best-level depth
  flow_60s signed traded volume over the last 60 s (trade above mid = buy), / mean depth

Usage: python3 -I tools/research/ofi_probe.py data/journal 2026-10-23 2026-10-26 2026-10-27
Stdlib only.
"""

import json
import math
import mmap
import os
import sqlite3
import struct
import sys
from bisect import bisect_left
from multiprocessing import Pool

# Near-month futures: (Kite instrument token, last day it is the near month), from the
# journal's instruments.csv.
CONTRACTS = {
    "NIFTY": [(12468226, "2026-10-27"), (15736578, "9999-12-31")],
    "SENSEX": [(221330181, "2026-10-29"), (281837829, "9999-12-31")],
}
IST = 19800
SESSION = (9 * 3600 + 20 * 60, 15 * 3600 + 25 * 60)  # skip the open and close auctions
HORIZONS = (10, 60, 300)
FILE_HEADER = 16
T_RAW = 1


def ids_for(day):
    out = {}
    for name, rows in CONTRACTS.items():
        for iid, last in rows:
            if day <= last:
                out[iid] = name
                break
    return out


FULL_PACKET = 184  # Kite full-mode packet for F&O: 16 u32 fields, then 10 depth entries
PACKET = struct.Struct(">16I" + "IIHH" * 10)


def scan(args):
    """Depth and trade rows for the target Kite tokens in one journal file.

    The journal keeps Kite's WebSocket frames as received (RawFrame, 32-byte header), so
    this decodes Kite's binary format: u16 packet count, then u16 length + packet each,
    big-endian, prices in paise.
    """
    path, ids = args
    depth, trades = [], []
    with open(path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        if size <= FILE_HEADER:
            return depth, trades
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    pos, unpack = FILE_HEADER, struct.unpack_from
    while pos + 8 <= size:
        length, kind = unpack("<II", m, pos)
        body = pos + 8
        if body + length > size:
            break  # torn tail
        if kind == T_RAW and length > 34:
            frame, end = body + 32, body + length
            if m[frame] != 0x7B:  # skip text (JSON) frames
                count, = unpack(">H", m, frame)
                p = frame + 2
                for _ in range(count):
                    if p + 2 > end:
                        break
                    plen, = unpack(">H", m, p)
                    p += 2
                    if plen == FULL_PACKET and unpack(">I", m, p)[0] in ids:
                        v = PACKET.unpack_from(m, p)
                        token, ts = v[0], v[15]
                        bids = [(v[16 + i * 4 + 1] / 100, v[16 + i * 4]) for i in range(5)]
                        asks = [(v[36 + i * 4 + 1] / 100, v[36 + i * 4]) for i in range(5)]
                        depth.append((token, ts, bids, asks))
                        trades.append((token, ts, v[1] / 100, v[4]))
                    p += plen
        pos = body + length
    m.close()
    return depth, trades


def corr(xs, ys):
    n = len(xs)
    if n < 30:
        return float("nan"), n
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return float("nan"), n
    return sxy / math.sqrt(sxx * syy), n


def features(depth, trades):
    """Per-second feature rows {t, mid, qi1, qi5, micro, ofi_10s, ofi_60s, flow_60s}."""
    depth.sort(key=lambda r: r[1])
    trades.sort(key=lambda r: r[1])
    # One depth row per second (the last), dropping crossed or empty books.
    book = {}
    for _, t, bids, asks in depth:
        if bids[0][1] and asks[0][1] and asks[0][0] > bids[0][0]:
            book[t] = (bids, asks)
    secs = sorted(book)
    if not secs:
        return []
    mean_depth = sum(book[t][0][0][1] + book[t][1][0][1] for t in secs) / (2 * len(secs))

    # OFI per second, best level.
    ofi, prev = {}, None
    for t in secs:
        (bp, bq), (ap, aq) = book[t][0][0], book[t][1][0]
        if prev:
            (pbp, pbq), (pap, paq) = prev
            e = (bq if bp >= pbp else 0) - (pbq if bp <= pbp else 0)
            e += (paq if ap >= pap else 0) - (aq if ap <= pap else 0)
            ofi[t] = e
        prev = ((bp, bq), (ap, aq))

    # Signed volume per second: the volume increment, signed by trade price vs the mid.
    flow, last_vol = {}, None
    mids = {t: (book[t][0][0][0] + book[t][1][0][0]) / 2 for t in secs}
    for _, t, ltp, vol in trades:
        if last_vol is not None and vol > last_vol:
            i = bisect_left(secs, t)
            ref = mids[secs[min(i, len(secs) - 1)]]
            sign = 1 if ltp > ref else -1 if ltp < ref else 0
            flow[t] = flow.get(t, 0) + sign * (vol - last_vol)
        last_vol = vol if vol else last_vol

    def window(src, t, w):
        return sum(src.get(s, 0) for s in range(t - w + 1, t + 1))

    rows = []
    for t in secs:
        bids, asks = book[t]
        b1, a1 = bids[0][1], asks[0][1]
        b5, a5 = sum(q for _, q in bids), sum(q for _, q in asks)
        mid = mids[t]
        half = (asks[0][0] - bids[0][0]) / 2
        micro = (asks[0][0] * b1 + bids[0][0] * a1) / (b1 + a1)
        rows.append({
            "t": t, "mid": mid,
            "qi1": (b1 - a1) / (b1 + a1),
            "qi5": (b5 - a5) / (b5 + a5) if b5 + a5 else 0.0,
            "micro": (micro - mid) / half if half else 0.0,
            "ofi_10s": window(ofi, t, 10) / mean_depth,
            "ofi_60s": window(ofi, t, 60) / mean_depth,
            "flow_60s": window(flow, t, 60) / mean_depth,
        })
    return rows


FEATS = ("qi1", "qi5", "micro", "ofi_10s", "ofi_60s", "flow_60s")


def evaluate(rows, sample_every=1):
    """corr(feature at t, mid return t → t+h) for each horizon, in session hours only."""
    by_t = {r["t"]: r for r in rows}
    out = {}
    for h in HORIZONS:
        xs = {f: [] for f in FEATS}
        ys = []
        for r in rows[::sample_every]:
            sod = (r["t"] + IST) % 86400
            if not SESSION[0] <= sod <= SESSION[1] - h:
                continue
            fut = by_t.get(r["t"] + h)
            if fut is None:
                continue
            ys.append((fut["mid"] / r["mid"] - 1) * 1e4)
            for f in FEATS:
                xs[f].append(r[f])
        out[h] = {f: corr(xs[f], ys) for f in FEATS}
    return out


def overlap(rows):
    """Correlation matrix of the features themselves."""
    return {(a, b): corr([r[a] for r in rows], [r[b] for r in rows])[0]
            for i, a in enumerate(FEATS) for b in FEATS[i + 1:]}


def factor_overlap(db_path):
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    series = {}
    for underlying, factors in con.execute("SELECT underlying, factors FROM signal"):
        vals = {x["factor"]: x["value"] for x in json.loads(factors) if x["status"] == "present"}
        series.setdefault(underlying, []).append(vals)
    con.close()
    names = sorted({k for rows in series.values() for r in rows for k in r})
    print("\nLive factor overlap (signals.db, all underlyings pooled; |r| ≥ 0.5 flagged)")
    rows = [r for rs in series.values() for r in rs]
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            both = [(r[a], r[b]) for r in rows if a in r and b in r]
            if len(both) < 30:
                continue
            c, n = corr([x for x, _ in both], [y for _, y in both])
            flag = "  <-- overlap" if abs(c) >= 0.5 else ""
            print(f"  {a:16s} {b:16s} r={c:+.2f}  n={n}{flag}")


def main():
    root, days = sys.argv[1], sys.argv[2:]
    for day in days:
        ids = ids_for(day)
        files = sorted(os.path.join(root, day, f) for f in os.listdir(os.path.join(root, day))
                       if f.endswith(".mdj") and "09" <= f[:2] <= "15")
        with Pool(2) as pool:
            parts = pool.map(scan, [(p, ids) for p in files])
        for iid, name in ids.items():
            depth = [r for d, _ in parts for r in d if r[0] == iid]
            trades = [r for _, t in parts for r in t if r[0] == iid]
            rows = features(depth, trades)
            print(f"\n== {day} {name} (id {iid}): {len(depth)} depth, {len(trades)} trades, "
                  f"{len(rows)} seconds")
            if not rows:
                continue
            res = evaluate(rows)
            print("  corr(feature, forward return)   " + "  ".join(f"{h:>6d}s" for h in HORIZONS))
            for f in FEATS:
                cells = "  ".join(f"{res[h][f][0]:+7.3f}" for h in HORIZONS)
                print(f"  {f:31s} {cells}   n={res[HORIZONS[0]][f][1]}")
            print("  feature overlap: " + ", ".join(
                f"{a}~{b} {c:+.2f}" for (a, b), c in overlap(rows).items() if abs(c) >= 0.5))
            # Machine-readable line for pooling across days.
            print("JSON " + json.dumps({"day": day, "name": name, "res": {
                h: {f: res[h][f] for f in FEATS} for h in HORIZONS}}))
    factor_overlap(os.path.join(os.path.dirname(os.path.abspath(root)), "signals.db"))


if __name__ == "__main__":
    main()
