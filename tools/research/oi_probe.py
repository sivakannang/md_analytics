#!/usr/bin/env python3
"""Measure how often open interest (OI) actually changes in the Kite feed.

Throwaway research tool (see docs/design/oi-buildup-chart.md §7). Not production code.

  record   subscribe to a sample of NIFTY and stock F&O instruments in full mode and
           log every OI change (plus tick counts) to a CSV under data/research/oi_probe/
  analyze  summarize a recorded CSV: OI update intervals per segment, batching across
           instruments, and variation through the session

Credentials (never logged or written to the CSV), first match wins:
  1. env vars MDA_KITE_API_KEY and MDA_KITE_ACCESS_TOKEN
  2. secrets/kite_session.json  {"api_key": "...", "access_token": "..."}
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import signal
import statistics
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT_DIR = REPO_ROOT / "data" / "research" / "oi_probe"
SESSION_FILE = REPO_ROOT / "secrets" / "kite_session.json"
IST = dt.timezone(dt.timedelta(hours=5, minutes=30))

INDEX_SPOT_SYMBOL = {"NIFTY": "NSE:NIFTY 50", "BANKNIFTY": "NSE:NIFTY BANK"}
DEFAULT_STOCKS = ["RELIANCE", "HDFCBANK", "ICICIBANK", "INFY", "SBIN", "TCS"]

CSV_FIELDS = [
    "recv_ns", "event", "token", "symbol", "segment", "oi", "prev_oi",
    "last_price", "volume", "exchange_ts", "last_trade_ts",
]


# --------------------------------------------------------------------------- credentials

def load_credentials() -> tuple[str, str]:
    api_key = os.environ.get("MDA_KITE_API_KEY")
    token = os.environ.get("MDA_KITE_ACCESS_TOKEN")
    if api_key and token:
        return api_key, token
    if SESSION_FILE.exists():
        data = json.loads(SESSION_FILE.read_text())
        return data["api_key"], data["access_token"]
    sys.exit(f"No credentials: set MDA_KITE_API_KEY/MDA_KITE_ACCESS_TOKEN or create {SESSION_FILE}")


# --------------------------------------------------------------------------- instrument selection

def select_instruments(kite, underlyings: list[str], strikes: int) -> dict[int, dict]:
    """Returns {token: {"symbol", "segment"}} for futures (all expiries) and nearest-expiry
    options at ATM ± `strikes` for each underlying."""
    today = dt.date.today()
    nfo = kite.instruments("NFO")

    spot_keys = [INDEX_SPOT_SYMBOL.get(u, f"NSE:{u}") for u in underlyings]
    spots = kite.ltp(spot_keys)

    selected: dict[int, dict] = {}
    for underlying, spot_key in zip(underlyings, spot_keys):
        if spot_key not in spots:
            print(f"warning: no spot price for {underlying}, skipped", file=sys.stderr)
            continue
        spot = spots[spot_key]["last_price"]
        kind = "index" if underlying in INDEX_SPOT_SYMBOL else "stock"
        contracts = [i for i in nfo if i["name"] == underlying and i["expiry"] and i["expiry"] >= today]

        for fut in (i for i in contracts if i["instrument_type"] == "FUT"):
            selected[fut["instrument_token"]] = {"symbol": fut["tradingsymbol"], "segment": f"{kind}_fut"}

        options = [i for i in contracts if i["instrument_type"] in ("CE", "PE")]
        if not options:
            continue
        nearest = min(i["expiry"] for i in options)
        chain = [i for i in options if i["expiry"] == nearest]
        all_strikes = sorted({i["strike"] for i in chain})
        atm_index = min(range(len(all_strikes)), key=lambda k: abs(all_strikes[k] - spot))
        window = set(all_strikes[max(0, atm_index - strikes): atm_index + strikes + 1])
        for opt in (i for i in chain if i["strike"] in window):
            selected[opt["instrument_token"]] = {"symbol": opt["tradingsymbol"], "segment": f"{kind}_opt"}

    return selected


# --------------------------------------------------------------------------- record

def record(args: argparse.Namespace) -> None:
    from kiteconnect import KiteConnect, KiteTicker  # only needed for recording

    api_key, access_token = load_credentials()
    kite = KiteConnect(api_key=api_key)
    kite.set_access_token(access_token)

    underlyings = ["NIFTY"] + args.stocks
    instruments = select_instruments(kite, underlyings, args.strikes)
    if not instruments:
        sys.exit("No instruments selected")
    print(f"Subscribing to {len(instruments)} instruments", file=sys.stderr)

    args.out_dir.mkdir(parents=True, exist_ok=True)
    out_path = args.out_dir / f"{dt.datetime.now(IST):%Y-%m-%d_%H%M%S}.csv"
    out_file = out_path.open("w", newline="")
    writer = csv.DictWriter(out_file, fieldnames=CSV_FIELDS)
    writer.writeheader()

    lock = threading.Lock()
    last_oi: dict[int, int] = {}
    tick_counts: dict[int, int] = defaultdict(int)
    stop = threading.Event()

    def on_ticks(_ws, ticks):
        recv_ns = time.time_ns()
        with lock:
            for t in ticks:
                token = t["instrument_token"]
                tick_counts[token] += 1
                oi = t.get("oi")
                if oi is None or last_oi.get(token) == oi:
                    continue
                meta = instruments.get(token, {"symbol": "?", "segment": "?"})
                writer.writerow({
                    "recv_ns": recv_ns,
                    "event": "oi_first" if token not in last_oi else "oi_change",
                    "token": token,
                    "symbol": meta["symbol"],
                    "segment": meta["segment"],
                    "oi": oi,
                    "prev_oi": last_oi.get(token, ""),
                    "last_price": t.get("last_price", ""),
                    "volume": t.get("volume_traded", ""),
                    "exchange_ts": t.get("exchange_timestamp") or "",
                    "last_trade_ts": t.get("last_trade_time") or "",
                })
                last_oi[token] = oi

    def on_connect(ws, _response):
        tokens = list(instruments)
        ws.subscribe(tokens)
        ws.set_mode(ws.MODE_FULL, tokens)
        print("Connected; recording OI changes (Ctrl+C to stop)", file=sys.stderr)

    def on_close(_ws, code, reason):
        print(f"Connection closed: {code} {reason}", file=sys.stderr)

    ticker = KiteTicker(api_key, access_token)
    ticker.on_ticks = on_ticks
    ticker.on_connect = on_connect
    ticker.on_close = on_close
    ticker.connect(threaded=True)

    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    until = dt.datetime.combine(dt.date.today(), args.until, tzinfo=IST)
    while not stop.is_set() and dt.datetime.now(IST) < until:
        stop.wait(5)
        with lock:
            out_file.flush()

    ticker.close()
    with lock:
        for token, count in tick_counts.items():
            meta = instruments.get(token, {"symbol": "?", "segment": "?"})
            writer.writerow({"recv_ns": time.time_ns(), "event": "tick_count", "token": token,
                             "symbol": meta["symbol"], "segment": meta["segment"], "volume": count})
        out_file.close()
    print(f"Wrote {out_path}", file=sys.stderr)


# --------------------------------------------------------------------------- analyze

def percentile(values: list[float], p: float) -> float:
    ordered = sorted(values)
    k = (len(ordered) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def analyze(args: argparse.Namespace) -> None:
    changes: dict[int, list[int]] = defaultdict(list)   # token -> recv_ns of OI changes
    segment_of: dict[int, str] = {}
    ticks: dict[int, int] = {}

    with args.csv.open() as f:
        for row in csv.DictReader(f):
            token = int(row["token"])
            segment_of[token] = row["segment"]
            if row["event"] == "oi_change":
                changes[token].append(int(row["recv_ns"]))
            elif row["event"] == "tick_count":
                ticks[token] = int(row["volume"])

    # 1. Interval between consecutive OI changes, per segment
    intervals: dict[str, list[float]] = defaultdict(list)
    for token, times in changes.items():
        times.sort()
        intervals[segment_of[token]] += [(b - a) / 1e9 for a, b in zip(times, times[1:])]

    print("OI update interval per segment (seconds)")
    print(f"{'segment':<12}{'instr':>7}{'updates':>9}{'p10':>8}{'median':>8}{'p90':>8}{'max':>8}")
    for segment in sorted(set(segment_of.values())):
        tokens = [t for t, s in segment_of.items() if s == segment]
        values = intervals.get(segment, [])
        n_updates = sum(len(changes.get(t, [])) for t in tokens)
        if values:
            print(f"{segment:<12}{len(tokens):>7}{n_updates:>9}{percentile(values, 0.1):>8.1f}"
                  f"{statistics.median(values):>8.1f}{percentile(values, 0.9):>8.1f}{max(values):>8.1f}")
        else:
            print(f"{segment:<12}{len(tokens):>7}{n_updates:>9}{'-':>8}{'-':>8}{'-':>8}{'-':>8}")

    # 2. Batching: how many instruments change OI within the same window
    window_ns = int(args.batch_window * 1e9)
    all_events = sorted((ts, token) for token, times in changes.items() for ts in times)
    batches: list[set[int]] = []
    start = None
    for ts, token in all_events:
        if start is None or ts - start > window_ns:
            batches.append(set())
            start = ts
        batches[-1].add(token)
    if batches:
        sizes = [len(b) for b in batches]
        n_tokens = len(changes)
        print(f"\nBatching ({args.batch_window:.0f}s windows): {len(batches)} bursts; "
              f"instruments per burst median {statistics.median(sizes):.0f}, max {max(sizes)} of {n_tokens}")

    # 3. Variation through the session: OI updates per 30-minute slot (IST)
    slots: dict[str, int] = defaultdict(int)
    for ts, _ in all_events:
        t = dt.datetime.fromtimestamp(ts / 1e9, IST)
        slots[f"{t.hour:02d}:{0 if t.minute < 30 else 30:02d}"] += 1
    if slots:
        print("\nOI updates per 30-minute slot")
        for slot in sorted(slots):
            print(f"  {slot}  {slots[slot]}")

    if ticks:
        total_ticks = sum(ticks.values())
        total_changes = sum(len(v) for v in changes.values())
        print(f"\nTicks received: {total_ticks}; OI changes: {total_changes} "
              f"({100 * total_changes / max(total_ticks, 1):.2f}% of ticks)")


# --------------------------------------------------------------------------- main

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    rec = sub.add_parser("record", help="record OI changes from the live feed")
    rec.add_argument("--strikes", type=int, default=5, help="option strikes each side of ATM (default 5)")
    rec.add_argument("--stocks", nargs="*", default=DEFAULT_STOCKS, help="stock underlyings to include")
    rec.add_argument("--until", type=dt.time.fromisoformat, default=dt.time(15, 30), help="stop time IST (default 15:30)")
    rec.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    rec.set_defaults(func=record)

    ana = sub.add_parser("analyze", help="summarize a recorded CSV")
    ana.add_argument("csv", type=Path)
    ana.add_argument("--batch-window", type=float, default=2.0, help="seconds grouping one burst (default 2)")
    ana.set_defaults(func=analyze)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
