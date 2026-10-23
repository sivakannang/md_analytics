# Runbook: measure OI update frequency

Purpose: collect one trading day of OI change timings to set the OI chart defaults
([design §7](../design/oi-buildup-chart.md#7-measuring-oi-update-frequency)).

## One-time setup

```bash
python3 -m venv .venv
.venv/bin/pip install -r tools/research/requirements.txt
mkdir -p secrets && chmod 700 secrets
```

Create `secrets/kite_session.json` (git-ignored) with today's token, then restrict it:

```json
{"api_key": "your_api_key", "access_token": "todays_access_token"}
```

```bash
chmod 600 secrets/kite_session.json
```

Alternatively export `MDA_KITE_API_KEY` and `MDA_KITE_ACCESS_TOKEN`.

## On a trading day

1. Generate today's access token (as with your existing test app) and update the session file.
2. Before 09:15 IST, start recording (runs until 15:30 IST, or Ctrl+C):

   ```bash
   .venv/bin/python tools/research/oi_probe.py record
   ```

   Options: `--strikes 5` (ATM ± N), `--stocks RELIANCE INFY …`, `--until 12:00`.
3. Output: `data/research/oi_probe/<date>_<time>.csv` (git-ignored — market data stays local).

## Analyze

```bash
.venv/bin/python tools/research/oi_probe.py analyze data/research/oi_probe/<file>.csv
```

Reports, per segment (index/stock × futures/options): median and p90 seconds between OI
changes; whether updates arrive in synchronized bursts; and update counts per
30-minute slot. Paste the output into the design note's §7.
