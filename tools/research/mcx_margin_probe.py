#!/usr/bin/env python3
"""MCX probe (ADR-0031): Kite's required margin for one lot of each near MCX future.

Checks the units-per-lot table (price × units = one lot's value) against Kite's margin
calculator, and gives the margin as a share of that value. Read-only: quotes and the
margin calculator, no orders. Prints market data and margins only.

  python3 tools/research/mcx_margin_probe.py
"""

from __future__ import annotations

import csv
import datetime as dt
import json
import os
import urllib.parse
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SESSION_FILE = REPO_ROOT / "secrets" / "kite_session.json"

# Units (of the quoted price) per lot, as assumed in instruments/mcx.cpp.
UNITS = {
    "CRUDEOIL": 100, "CRUDEOILM": 10, "NATURALGAS": 1250, "NATGASMINI": 250,
    "GOLD": 100, "GOLDM": 10, "GOLDPETAL": 1, "GOLDGUINEA": 1, "GOLDTEN": 1,
    "SILVER": 30, "SILVERM": 5, "SILVERMIC": 1,
    "COPPER": 2500, "ZINC": 5000, "ZINCMINI": 1000, "LEAD": 5000, "LEADMINI": 1000,
    "ALUMINIUM": 5000, "ALUMINI": 1000, "NICKEL": 250,
    "MENTHAOIL": 360, "CARDAMOM": 100, "MCXBULLDEX": 30, "MCXMETLDEX": 40,
}


def load_credentials() -> tuple[str, str]:
    api_key = os.environ.get("MDA_KITE_API_KEY")
    token = os.environ.get("MDA_KITE_ACCESS_TOKEN")
    if api_key and token:
        return api_key, token
    data = json.loads(SESSION_FILE.read_text())
    return data["api_key"], data["access_token"]


def near_futures() -> dict[str, str]:
    files = sorted((REPO_ROOT / "data" / "instruments").glob("kite_instruments_*.csv"))
    today = dt.date.today().isoformat()
    best: dict[str, tuple[str, str]] = {}
    with files[-1].open() as f:
        for row in csv.DictReader(f):
            if row["exchange"] != "MCX" or row["instrument_type"] != "FUT" or row["expiry"] <= today:
                continue
            name = row["name"]
            if name in UNITS and (name not in best or row["expiry"] < best[name][0]):
                best[name] = (row["expiry"], row["tradingsymbol"])
    return {name: symbol for name, (_, symbol) in best.items()}


def main() -> None:
    api_key, token = load_credentials()
    headers = {"X-Kite-Version": "3", "Authorization": f"token {api_key}:{token}"}

    def call(path: str, body: list | None = None):
        request = urllib.request.Request(
            "https://api.kite.trade" + path, headers=headers,
            data=json.dumps(body).encode() if body is not None else None)
        if body is not None:
            request.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(request, timeout=20) as response:
            return json.load(response)["data"]

    futures = near_futures()
    query = urllib.parse.urlencode([("i", f"MCX:{s}") for s in futures.values()])
    quotes = call("/quote?" + query)
    orders = [{"exchange": "MCX", "tradingsymbol": s, "transaction_type": "BUY",
               "variety": "regular", "product": "NRML", "order_type": "MARKET",
               "quantity": 1, "price": 0, "trigger_price": 0} for s in futures.values()]
    margins = {m["tradingsymbol"]: m for m in call("/margins/orders", orders)}

    print(f"{'symbol':<22}{'ltp':>12}{'units':>7}{'lot value':>14}{'margin':>12}{'%':>7}"
          f"{'volume':>10}{'oi':>9}")
    for name, symbol in sorted(futures.items()):
        q = quotes.get(f"MCX:{symbol}", {})
        m = margins.get(symbol, {})
        ltp = q.get("last_price") or 0
        value = ltp * UNITS[name]
        total = m.get("total") or 0
        pct = 100 * total / value if value else 0
        print(f"{symbol:<22}{ltp:>12.2f}{UNITS[name]:>7}{value:>14,.0f}{total:>12,.0f}{pct:>7.1f}"
              f"{q.get('volume', 0):>10}{q.get('oi', 0):>9}")


if __name__ == "__main__":
    main()
