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
  15:40 IST.
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
| `engine/libs/*` | C++ libraries: core, messages, bus, kite, instruments, catalog, storage, appdb, auth, admin, watchlists, subscriptions, bars, options, quotestore, notify, journal, sysmon, gateway, runtime |
| `engine/apps/*` | Processes: mda-feedd (Kite WS → `md` bus), mda-engine (bars + option analytics → `an` bus, `data/bars.db`), mda-gateway (REST + WS stream, :8090), mda-notifier (email + Telegram), mda-recorder (tick journal), mda-login, mda-backfill; `tools/` holds mda-tail, mda-busctl, mda-useradm and mda-kite-check |
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
- `md` has a single writer (mda-feedd); `an` is written by mda-engine. Message types:
  RawFrame 1, MarketSnapshot 2, DepthSnapshot 3, FeedStatus 4, CircuitLimits 5, Bar 6,
  OptionAnalytics 7, ChainSummary 8.

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
- **Admin:** dashboard.
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
- **OI views:** past days (OC4), several days in OI over time (MS2); Sensibull's other OI
  tabs and other OI chart ideas are in memory (not started).
- **Notifications N7:** alert rules (OI buildup, price/OI thresholds), quiet hours.
- **Trading terminal:** ADR first (read-only account views, then paper trading).
- **Session phases S1–S3** (ADR-0023). Run `tools/research/phase_capture.py` on a trading
  day first.
