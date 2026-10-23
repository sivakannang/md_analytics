#!/usr/bin/env python3
"""Capture what Kite sends during pre-open and the closing auction (ADR-0023 verification).

Throwaway research tool. Not production code. For each window (IST, default 08:58-09:17
and 15:10-15:42) it:

  - polls Kite's quote API every --every seconds for --keys and appends one JSON line per
    instrument (price, volume, circuits, reference / indicative price, imbalance, best
    depth rows) to quotes.jsonl;
  - runs mda-tail for each key's symbol, so the raw tick stream (volume field, exchange
    time) lands in ticks-<SYMBOL>.txt. Needs mda-feedd running with the keys subscribed
    (e.g. in a watchlist).

Output: data/research/phase_capture/<YYYY-MM-DD>/ (git-ignored). Start it any time after
mda-login; it waits for each window and exits after the last.

Credentials (never logged or written), first match wins:
  1. env vars MDA_KITE_API_KEY and MDA_KITE_ACCESS_TOKEN
  2. secrets/kite_session.json  {"api_key": "...", "access_token": "..."}
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IST = dt.timezone(dt.timedelta(hours=5, minutes=30))
DEFAULT_KEYS = "NSE:HDFCBANK,NSE:INFY,NSE:RELIANCE,NSE:HDBFS"  # HDBFS: non-CAS control
QUOTE_FIELDS = (
    "timestamp", "last_trade_time", "last_price", "last_quantity", "volume", "average_price",
    "buy_quantity", "sell_quantity", "lower_circuit_limit", "upper_circuit_limit",
    "reference_limit_price", "indicative_close_price", "total_imbalance_qty",
    "low_limit_price_protection", "high_limit_price_protection", "ohlc",
)


def credentials() -> tuple[str, str]:
    key, token = os.environ.get("MDA_KITE_API_KEY"), os.environ.get("MDA_KITE_ACCESS_TOKEN")
    if key and token:
        return key, token
    session = json.loads((ROOT / "secrets" / "kite_session.json").read_text())
    return session["api_key"], session["access_token"]


def quotes(keys: list[str], auth: tuple[str, str]) -> dict:
    query = "&".join("i=" + urllib.parse.quote(k) for k in keys)
    request = urllib.request.Request(
        "https://api.kite.trade/quote?" + query,
        headers={"X-Kite-Version": "3", "Authorization": f"token {auth[0]}:{auth[1]}"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)["data"]


def window(text: str, day: dt.date) -> tuple[dt.datetime, dt.datetime]:
    start, end = (dt.datetime.combine(day, dt.time.fromisoformat(t), IST) for t in text.split("-"))
    return start, end


def capture(start: dt.datetime, end: dt.datetime, keys: list[str], every: float, out: Path,
            auth: tuple[str, str]) -> None:
    now = dt.datetime.now(IST)
    if now >= end:
        print(f"window {start:%H:%M}-{end:%H:%M} is over; skipped")
        return
    if now < start:
        print(f"waiting for {start:%H:%M} IST")
        time.sleep((start - now).total_seconds())
    print(f"capturing {start:%H:%M}-{end:%H:%M} IST into {out.relative_to(ROOT)}")

    tails = []
    tail = ROOT / "build/release/engine/apps/tools/mda-tail"
    for key in keys:
        exchange, symbol = key.split(":", 1)
        log = open(out / f"ticks-{symbol}.txt", "a")
        tails.append((subprocess.Popen(
            [str(tail), "--depth", "--filter", f"{exchange}|CASH|EQ|{symbol}"], cwd=ROOT, stdout=log,
            stderr=subprocess.STDOUT), log))

    polls = errors = 0
    try:
        with open(out / "quotes.jsonl", "a") as sink:
            while dt.datetime.now(IST) < end:
                began = time.monotonic()
                try:
                    data = quotes(keys, auth)
                    at = dt.datetime.now(IST).isoformat(timespec="milliseconds")
                    for key, q in data.items():
                        row = {"at": at, "key": key, **{f: q.get(f) for f in QUOTE_FIELDS}}
                        depth = q.get("depth", {})
                        row["bids"] = depth.get("buy", [])
                        row["asks"] = depth.get("sell", [])
                        sink.write(json.dumps(row) + "\n")
                    sink.flush()
                    polls += 1
                except (urllib.error.URLError, TimeoutError, KeyError, ValueError) as error:
                    errors += 1
                    print(f"poll failed: {type(error).__name__}")  # no URL: it has no secrets, but stay terse
                time.sleep(max(0.0, every - (time.monotonic() - began)))
    finally:
        for process, log in tails:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
            log.close()
    print(f"window done: {polls} polls, {errors} failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keys", default=DEFAULT_KEYS, help="comma-separated Kite quote keys")
    parser.add_argument("--window", action="append", help="HH:MM-HH:MM IST (repeatable)")
    parser.add_argument("--every", type=float, default=3.0, help="seconds between quote polls")
    args = parser.parse_args()

    today = dt.datetime.now(IST).date()
    windows = [window(w, today) for w in (args.window or ["08:58-09:17", "15:10-15:42"])]
    keys = [k.strip() for k in args.keys.split(",") if k.strip()]
    out = ROOT / "data" / "research" / "phase_capture" / today.isoformat()
    out.mkdir(parents=True, exist_ok=True)
    auth = credentials()
    for start, end in windows:
        capture(start, end, keys, args.every, out, auth)
    return 0


if __name__ == "__main__":
    sys.exit(main())
