# md_analytics: notes for Claude

A market-data and derivatives analytics platform on Zerodha Kite: multi-process C++23
engine plus a React/TS web app. It's a personal product (the owner and one friend) and HFT
interview practice. Claude writes the code and the owner reviews it. Design and ADRs come
first, then small tested units.

## Rules (always)

**Secrets**
- The repo is **public**. Never commit secrets.
- `secrets/` (Kite session, keys) is git-ignored: directory 700, files 600.
- Never log or print passwords, tokens, API keys or secrets, emails or phones. Admin
  shows contact details masked.
- Scripts that use the Kite session print only statuses, counts and market data, never
  the key or token.

**Kite**
- Never enter the owner's Zerodha password or TOTP; the owner runs `mda-login`.
- Kite web in Chrome may be examined for UI reference. Never click Buy/Sell or anything
  order-related.

**Testing**
- Never test against the real `data/app.db`. Use the test stack below.

**Markets**
- Past-day backfills only after market hours: NSE/BSE cash 09:15–15:30, F&O until
  15:40 IST; MCX 09:00 to 23:30, or 23:55 while the US is off daylight saving (ADR-0031).
  The market processes run until 00:05.
- Don't restart `mda-engine` during market hours without saying so: its first minute is
  skipped and its analytics state is rebuilt.

**Working style**
- Don't remind about committing, git identity or `gh` login; the owner handles them.
- Keep tokens low:
  - read only the files a task needs;
  - check pages with text reads, and take one screenshot at the end of a unit;
  - run long jobs in the background and check once.

## Layout

| Path | What |
|---|---|
| `engine/libs/*` | C++ libraries: core, messages, bus, kite, instruments, catalog, storage, appdb, auth, admin, watchlists, subscriptions, bars, options, signals, quotestore, notify, journal, sysmon, gateway, runtime |
| `engine/apps/*` | Processes: mda-feedd (Kite WS → `md` bus), mda-engine (bars + option analytics → `an` bus, `data/bars.db`), mda-signald (index signals → `sig` bus, `data/signals.db`), mda-broker (paper trading: `ord-*` → `exec` buses, `data/paper.db`), mda-autotrader (paper trading on signals → `ord-auto`, `data/autotrader.db`), mda-gateway (REST + WS stream, :8090), mda-notifier (email + Telegram), mda-recorder (tick journal), mda-login, mda-backfill; `tools/` holds mda-tail, mda-busctl, mda-useradm, mda-kite-check, mda-backtest and mda-paperctl |
| `web/` | React 19 + Vite + TS. `src/api/client.ts` (REST), `src/stream/quoteStream.ts` (one WS; modes quote, depth, bars, chain), `src/pages`, `src/components`, `src/chart` (lightweight-charts 5.1) |
| `config/mda.yaml` | All runtime config (buses, feed chains, engine, gateway) |
| `data/` | Git-ignored: `app.db`, `bars.db`, `last_quotes.db` (ADR-0024), `journal/` (ADR-0026), `instruments/`, `research/` |
| `docs/adr/` | Decisions (index in `README.md`, template in `template.md`) |
| `docs/design/` | Unit designs (e.g. `oi-buildup-chart.md`, `notifications.md`, `ui.md`) |
| `docs/api/` | `rest.md`, `websocket.md`: keep them in step with gateway changes |
| `docs/runbooks/` | `daily-start-stop.md`, `daily-login.md` |
| `tools/research/` | Throwaway Python probes (not production) |

## Build and test

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
npm --prefix web run typecheck
```

- **Before finishing a C++ unit**, also run `ci-asan` and `ci-tsan`: same commands with
  that preset. They use `-Werror` with `-Wshadow` and `-Wsign-conversion`, which release
  doesn't catch.
- **clang-format:** CI runs `clang-format-19` with the repo's `.clang-format`. It isn't
  installed locally.
- **Tests:** gtest, in each library's `tests/` directory.
- **Gateway tests** use an in-memory DB fixture: `engine/libs/gateway/tests/gateway_test.cpp`
  and `stream_test.cpp`.

## Conventions

**C++**
- C++23: `std::expected` for errors, designated initialisers in declaration order.
- Fixed-point `core::Price` (`from_raw`, `kScale`) and nanosecond `core::Timestamp`.
- Bus messages are fixed-layout with `static_assert` sizes, registered in
  `MessageTraits` (ADR-0010).
- `md` has a single writer (mda-feedd); `an` is written by mda-engine; `sig` by mda-signald; `exec` by mda-broker; `ord-gw` by mda-gateway; `ord-auto` by mda-autotrader. Message types:
  RawFrame 1, MarketSnapshot 2, DepthSnapshot 3, FeedStatus 4, CircuitLimits 5, Bar 6,
  OptionAnalytics 7, ChainSummary 8, MarketSignal 9, OrderRequest 10, ExecutionReport 11.

**Time and SQLite**
- Exchange time is UTC on the wire. IST is +19800 s.
- `bars.db` minute starts are Unix seconds. Live bars (source 1) beat Kite backfill
  (source 2).
- SQLite migrations live in `appdb` and are numbered: add a new one, never edit an old one.

**Gateway API**
- Shapes are `{"status":"success","data":…}` and `{"status":"error","error_type",
  "message"}`.
- Routes under `/api/v1` need a session cookie.
- Admin routes check the role; roles are set only with `mda-useradm`.

**Web**
- Plain CSS with tokens in `styles.css` (`--up`, `--down`, `--accent` …; dark mode via
  media query).
- Kite's UI is the minimum standard.
- Quotes reach components through `useSyncExternalStore` hooks in `stream/useQuotes.ts`.

**Comments and docs**
- Comments explain *why*, briefly. Each header starts with a short purpose comment and
  links its ADR or design section.

## Running and test stack

- **Real stack:** see `docs/runbooks/daily-start-stop.md`. After mda-login,
  `tools/stack.sh start|stop|restart <name>|status` runs feedd, engine, gateway (:8090),
  notifier, recorder and the website (:5173) detached (pids in `data/run/`, logs in
  `data/logs/`); they survive the Claude app restarting. Restart only the one you changed
  (`tools/stack.sh restart gateway`).
- **Test gateway:**
  - copy `config/mda.yaml` to the scratchpad;
  - point `database.path` at a scratch copy of `data/app.db`;
  - set `gateway.listen` to `127.0.0.1:8092`;
  - run `mda-gateway --config <that file>`.

  It reads the live buses read-only. Create a throwaway user with the signup API.
- **Test web:** `.claude/launch.json` entry `web-test` (Vite on :5174, proxied to :8092).
- **Cleanup:** stop the test gateway and the preview, then delete the scratch DB and any
  test credentials.

## Status (2026-10-22)

**Done**
- **Accounts and watchlists:** auth, watchlists with groups and drag reorder, live quotes
  with depth and circuits.
- **Admin:** dashboard; each user's paper trading, read-only (Admin → Users → user → Paper trading).
- **Market data:** dynamic option subscriptions; 1-minute bars (live + backfill); charts
  (price, volume, OI).
- **Options:** analytics (IV, Greeks, OI change, buildup, PCR, max pain); the option chain
  page.
- **Auto-backfill** of bar history for newly subscribed instruments, in mda-feedd (ADR-0025).
- **Last-known quotes** after a restart or before the open (ADR-0024, `data/last_quotes.db`).
- **OI buildup chart** (futures and options): ΔOI bars coloured by buildup under the price
  pane, classified in the gateway (`bars/buildup.hpp`); chart-first instrument page
  (`docs/design/instrument-page.md`). Live refresh, "OI as of" and the OI-range wicks checked
  live on 2026-10-23.
- **OI by strike** (Sensibull's OI Change, `docs/design/oi-change-by-strike.md`): a view
  on the option chain page, with expiries, strikes ±N, presets, a time slider that redraws
  while dragged, Replay and live refresh. **OI over time** (Multistrike OI,
  `docs/design/multistrike-oi.md` MS1): chosen contracts' OI lines with spot.
- **System monitor** (`docs/design/system-monitor.md`): Admin → System: processes, machine,
  disk, ring buffers, feed, latency, engine, notifier, recorder, with checks and trends
  (`GET /admin/system`, library `sysmon`).
- **Notifications** N1–N6: `mda-notifier`, email + Telegram, account and system alerts
  (ADR-0016, `docs/design/notifications.md`).

**Next**
- **Market signals** (ADR-0028, `docs/design/signals.md`): direction bias for the indices
  in a new process, mda-signald; units SG1–SG6; SG1 (`libs/signals`) and SG2 (mda-signald, `sig` bus,
  `signals.db`), SG3 (outcomes, track record) and SG4 (REST `/signals`, stream mode
  `signals`) and SG5 (dashboard badges, instrument page Signals tab) done, checked by
  replaying 2026-10-23. A 10-day `mda-backtest` (§12) found
  no broken factor and no edge yet. Candidate factors at weight 0 (`signals.md` §13): F8 India VIX change
  done (SG7; 20-day backtest corr +0.12 NIFTY, +0.11 SENSEX) and F9 signed volume of the
  future (SG8; signald reads `md`; 23 Oct replay corr +0.16 NIFTY) done. SG6 (calibration, a second strategy) after ≥ 20 live
  trading days; the recorder is on (30 days kept, ~2.3 GB a day).
- **OI views:** past days (OC4), several days in OI over time (MS2); Sensibull's other OI
  tabs and other OI chart ideas are in memory (not started).
- **Notifications N7:** alert rules (OI buildup, price/OI thresholds), quiet hours.
- **Paper trading** (ADR-0029, `docs/design/paper-trading.md`): mda-broker (OMS, pre-trade
  risk, paper venue on the live book), `paper.db`, order ticket and positions; units PT1–PT7;
  PT1 (`libs/oms`: orders, positions, ledger, Zerodha's charges) and PT2 (paper
  venue fill model, `libs/rms`) and PT3 (`libs/broker`, mda-broker, `paper.db`, buses
  `ord-gw`/`ord-auto`/`exec`, mda-paperctl) and PT4 (REST `/paper/*`, stream mode `orders`) and PT5 (web: order ticket from B / S,
  Paper page with positions, orders, trades, funds, kill switch) done; PT6 (close of day: MIS
  square-off 15:25, DAY orders expire 15:30, expiry settlement, daily P&L summary 15:35)
  done. 
- **Auto trader** (ADR-0030, `docs/design/auto-trader.md`): mda-autotrader, strategy
  follow-signal on the near-month future, resting stop at the broker, decisions in
  `data/autotrader.db`; units AT1–AT4; AT1 (`libs/autotrade`) and AT2 (mda-autotrader, `autotrader.db`, recovery; replayed
  with a restart) done.
  AT3 (run metrics,
  `GET /paper/auto`) and AT4 (Auto tab, AUTO / SYSTEM order labels, summary section) done:
  the auto trader is complete. Option buying beside it (`follow-signal-options`, design §10):
  AT5 (`OptionsRun`, `choose_atm`) and AT6 (process wiring, `held` on `/paper/auto`, Auto tab)
  done, replayed on 2026-10-23. Live runs (user 1, all on since 2026-10-25):
  `nifty-follow`, `nifty-options`, `sensex-follow`, `sensex-options` (SENSEX stops and
  targets scaled ×3.2); the auto order rate is 30 a minute. Hardening done 2026-10-27: SQLite's busy timeout is set before WAL (fresh files opened together no
  longer fail), and replays score the same at any speed (closing chain summaries, `signals.md` §14).
- **MCX commodities** (ADR-0031): MX1 (import, quantities in units, contract keys,
  watchlists and charts), MX2 (hours per exchange in `instruments/session.hpp`; watchdog to
  00:05), MX3 (chains on each expiry's future: CRUDEOIL, NATURALGAS, GOLD, SILVER) and MX4
  (paper trading: per-commodity margins, MCX charges, depth in units, close of day at the
  MCX close) done 2026-10-27.
- **Session phases S1–S3** (ADR-0023). Run `tools/research/phase_capture.py` on a trading
  day first.
