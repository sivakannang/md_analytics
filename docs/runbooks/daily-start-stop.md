# Runbook: start and stop md_analytics each day

Six processes and the web dev server, started and stopped with **`tools/stack.sh`**. Run
everything from the repository root.

| Process | What it does |
|---|---|
| `mda-feedd` | Kite connection: subscriptions (static + watchlists + option windows), publishes the `md` bus |
| `mda-engine` | Builds 1-minute bars from the `md` bus into `data/bars.db`, publishes the `an` bus |
| `mda-gateway` | REST API, live stream, admin; also serves the **built** website at http://localhost:8090 (ADR-0027; `tools/stack.sh start gateway` rebuilds it when `web/` changed) |
| `mda-notifier` | Sends notifications (email, Telegram) and answers the Telegram bot |
| `mda-recorder` | Writes every Kite frame to the tick journal `data/journal/` (ADR-0026) |
| web dev server | The website with hot reload at http://localhost:5173 (development; never exposed remotely) |

## The short version

```bash
build/release/engine/apps/mda-login/mda-login
```

```bash
tools/stack.sh start
```

```bash
tools/stack.sh status
```

After 15:41:

```bash
tools/stack.sh stop
```

`tools/stack.sh` starts each process **detached** in its own session (pid in `data/run/`,
log in `data/logs/`), so closing a terminal, the Claude app, or locking the screen doesn't
stop it. While mda-feedd runs, the Mac is kept awake (`caffeinate`): **sleep would stop
the feed**. `start` refuses to start mda-feedd if today's Kite session is missing.
`restart <name>` bounces one process (e.g. `tools/stack.sh restart gateway` after a
gateway build); `start`/`stop` also take names. Replay processes are never touched.

The sections below are the same steps one by one, with what each log should say.

## Morning (start between 08:30 and 09:00 IST)

The pre-open session starts at 09:00 and bars start at 09:15. Kite publishes the day's
instrument list early in the morning, and mda-feedd downloads it at start (when the cached
copy predates 08:30). So start after 08:30 to pick up new contracts and expiries.

1. **Log in to Kite** (the token expires about 06:00 every day). See
   [daily-login.md](daily-login.md).

   ```bash
   build/release/engine/apps/mda-login/mda-login
   ```

2. **Feed**: wait for `kite ws: connected (subscribed … instruments)` and the `chains: …
   centred at …` lines.

   ```bash
   build/release/engine/apps/mda-feedd/mda-feedd
   ```

3. **Engine**: it logs `bus: reading 'md'`, then a summary each minute.

   ```bash
   build/release/engine/apps/mda-engine/mda-engine
   ```

4. **Gateway**: it logs `market data: mda-feedd is publishing`.

   ```bash
   build/release/engine/apps/mda-gateway/mda-gateway
   ```

   **Recorder** (before 09:00, so the pre-open is recorded): it logs `journal: writing
   data/journal/<date>/<HH>.mdj` and a summary each minute.

   ```bash
   build/release/engine/apps/mda-recorder/mda-recorder
   ```

   **Notifier** (any time; it needs no market data): it logs `email: via …` and
   `telegram: bot @… ready`. Leave it running overnight: on weekdays it reminds you at 08:45
   if `mda-login` hasn't run, and it delivers mda-feedd's alerts (feed down for 60 s or
   back, Kite session rejected) to admins by Telegram and verified email.

   ```bash
   build/release/engine/apps/mda-notifier/mda-notifier
   ```

5. **Website**

   ```bash
   npm --prefix web run dev
   ```

   Open http://localhost:5173.

6. **Optional: fill gaps.** New instruments need nothing: mda-feedd fills the history of
   anything it subscribes that has no bars (today's minutes at once, past days after
   15:40; ADR-0025; log lines `backfill: <symbol>: N bars`). Gaps in instruments that
   already have bars still need a manual run, e.g. if the engine wasn't running for part
   of the day, or after a few days away:

   ```bash
   build/release/engine/apps/mda-backfill/mda-backfill --days 7
   ```

   It fetches Kite's 1-minute candles (with OI) for everything we record and fills only
   the minutes we don't have. It never overwrites our own live bars, so it's safe to run
   at any time, even while the engine is running. 30 days for ~640 instruments takes about
   4 minutes. Use `--instrument "NSE|CASH|IDX|NIFTY 50"` for just one.

7. **Temporary (ADR-0023 checks): capture pre-open and the closing auction.** Start it
   right after mda-login, before 08:58, and leave it running until 15:42. It needs mda-feedd running with the
   captured stocks subscribed (HDFCBANK, INFY, RELIANCE and HDBFS are in the default
   watchlist).

   ```bash
   python3 -I tools/research/phase_capture.py
   ```

   It writes Kite quotes every 3 s and the raw ticks of those stocks to
   `data/research/phase_capture/<date>/` for 08:58–09:17 and 15:10–15:42.

The order matters only a little. The engine and gateway wait for the feed's bus if they
start first. The feed must have today's Kite token, or it waits (logging "access token
rejected; run mda-login") and connects once you log in.

## Evening (after 15:40 IST)

F&O trades until 15:40 (cash until 15:30, with the closing auction for F&O stocks
15:15–15:35), so stop after 15:41 to keep the last derivatives minute.


Run `tools/stack.sh stop` (or Ctrl+C in each tab, if started by hand). It stops the
readers first and the feed last; each process logs `stopped (exit code 0)`, and the Mac
may sleep again once mda-feedd has stopped.
- The engine writes its closed bars before exiting. The minute in progress is dropped,
  which is fine after the close.
- The gateway saves the last minute of usage counters.

Shutting the machine down is safe afterwards. The buses (`md`, `an`) live in shared
memory and are recreated at the next start. Everything that matters is on disk:
- `data/app.db`: users, watchlists, admin data;
- `data/bars.db`: bars;
- `data/journal/`: the tick journal, one folder per day (raw frames + that day's instrument list; ~0.5 GB/day, 90 days kept);
- `data/last_quotes.db`: the last quote, depth and circuit limits per instrument, shown after a restart or before the open (ADR-0024);
- `data/instruments/`: instrument lists and the permanent instrument ids.

## Quick health checks

```bash
sqlite3 data/bars.db "SELECT COUNT(*), datetime(MAX(start)+19800,'unixepoch') FROM bars_1m"
```

That shows the bar count and the last bar time (IST).

```bash
build/release/engine/apps/tools/mda-busctl stat md an
```

That shows both buses, their writers and readers.

## Notifier setup (once)

1. **Telegram bot:** in Telegram, message @BotFather → `/newbot` → pick a name and a
   username ending in `bot`. Keep the token it replies with.
2. **Gmail app password:** Google Account → Security → 2-Step Verification (must be on) →
   App passwords → create one named "md_analytics". Keep the 16 letters.
3. **Secrets file** (never committed; `secrets/` is git-ignored):

   ```bash
   cp config/notifier.example.json secrets/notifier.json
   ```

   ```bash
   chmod 600 secrets/notifier.json
   ```

   Edit it: the bot token, your Gmail address (as `username` and `from_address`) and the
   app password. Leave out a section to turn that channel off.
4. **Config:** set `notifier.telegram_bot` in `config/mda.yaml` to the bot's username
   (without `@`), so the dashboard can show the connect link.
5. Start `mda-notifier` and check its log. A wrong app password shows as
   `smtp AUTH: reply 535`; mails wait and go out once it's fixed (up to 8 tries, ~4 h).


## Replaying a recorded session (ADR-0026)

The recorder keeps every session in `data/journal/<date>/`. To watch one again, after
hours or at the weekend, run the replay stack. It uses its own buses (`md-replay`,
`an-replay`), its own `data/replay/bars.db` and port 8093, so it never touches live data
and can run next to the live stack. Start the engine and gateway first, then the replayer:

```bash
build/release/engine/apps/mda-engine/mda-engine --config config/replay.yaml
```

```bash
build/release/engine/apps/mda-gateway/mda-gateway --config config/replay.yaml
```

```bash
build/release/engine/apps/mda-replayer/mda-replayer --config config/replay.yaml --date 2026-10-23 --from 09:15 --to 15:40 --speed 10
```

Then open the replay website: `npm --prefix web run dev` with `MDA_GATEWAY_URL=http://127.0.0.1:8093 PORT=5175`
(the `web-replay` launch entry), at http://localhost:5175. `--speed max` replays as fast as
possible. To replay again from scratch, stop the three processes and delete `data/replay/`.
