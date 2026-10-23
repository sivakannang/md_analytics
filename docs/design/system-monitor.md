# System monitor (admin)

> Status: **Done 2026-10-23: SM1–SM3** (threshold alerts to Telegram later) · Builds on the admin dashboard
> ([ADR-0019](../adr/0019-admin-dashboard.md)), the bus monitoring API
> ([market-data-bus.md](market-data-bus.md) §8a, `bus::inspect_bus`) and system alerts
> ([notifications.md](notifications.md), N6)

## 1. Why

Today, "is the recorder working, how big is it, is anything falling behind" meant a terminal,
`tools/stack.sh status`, `du` and log greps. The admin should see the health of the whole
stack on one page: processes, machine, disk, ring buffers, feed, latency, and the pieces
around them (engine, notifier, recorder). Read-only: nothing on the page changes how the
system runs.

## 2. What the admin sees

A **System** tab on the Admin page, next to Users; refreshed every 5 s. Cards, each with a
status colour (green / amber / red by the thresholds in §5) and, where it helps, a small
trend line over the last hour.

```
Admin   [Users] [System]                                   updated 13:42:05 · ● all OK
┌ Processes ─────────────────────────────────────┐ ┌ Machine ───────────────────────────┐
│ feedd     ● 08:53  CPU 4.1%  RSS  62 MB        │ │ CPU load 1.9 / 8 cores   ▁▂▂▃▂▂  │
│ engine    ● 08:53  CPU 6.8%  RSS 180 MB        │ │ Memory 11.2 / 16 GB              │
│ gateway   ● 12:45  CPU 1.2%  RSS  48 MB        │ │ Power  AC (battery 100%)          │
│ notifier  ● 08:53  CPU 0.0%  RSS  12 MB        │ │ Uptime 3 d 4 h                    │
│ recorder  ● 08:53  CPU 1.0%  RSS  20 MB        │ └───────────────────────────────────┘
│ web       ● 08:53  CPU 0.3%  RSS 110 MB        │ ┌ Disk ──────────────────────────────┐
└────────────────────────────────────────────────┘ │ Free 91 GB of 228 GB (40%)        │
┌ Ring buffers ──────────────────────────────────┐ │ bars.db 1.2 GB · app.db 2 MB       │
│ md  64 MB · 1.6 MB/s · 2,410 msg/s · writer ●  │ │ journal 1.6 GB today, 1.6 GB all   │
│   engine    lag  12 KB (0.02%)  laps 0          │ └───────────────────────────────────┘
│   gateway   lag   3 KB (0.00%)  laps 0          │ ┌ Feed ──────────────────────────────┐
│   recorder  lag 140 KB (0.21%)  laps 0          │ │ ● connected · 0 reconnects today  │
│ an  32 MB · 0.2 MB/s · 610 msg/s · writer ●    │ │ 543 ticks/s ▃▅▇▅▃ · 4 frames/s     │
│   gateway   lag   1 KB          laps 0          │ │ 749 instruments · last tick 0.2 s  │
└────────────────────────────────────────────────┘ │ Latency feed→gateway p50 38 µs     │
┌ Engine ──────────┐┌ Notifier ─────────┐┌ Recorder ┐ │                p99 410 µs ▁▁▃▁    │
│ 687 bars/min      ││ 0 queued · 0 fail ││ on · 0 gaps│└───────────────────────────────────┘
│ last bar 13:41    ││ 14 sent today     ││ 1.6 GB     │
└───────────────────┘└───────────────────┘└────────────┘
```

## 3. Sources

| Card | Values | Where they come from |
|---|---|---|
| Processes | Running, start time, CPU % (over the last 5 s), resident memory | `data/run/<name>.pid` (tools/stack.sh) checked against the process's command line, as `stack.sh` does; per-process CPU time and RSS from the OS |
| Machine | Load average and cores, memory used / total, power source and battery %, uptime | OS calls (macOS: `sysctl`, `host_statistics64`, IOKit power sources; Linux: `/proc`) |
| Disk | Free / total of the `data/` volume; sizes of `bars.db`, `app.db`, `last_quotes.db`; journal size today and in all | `statvfs`; file sizes (journal: summed per day, cached for 60 s) |
| Ring buffers | Per bus (`md`, `an`): capacity, write rate (bytes/s, messages/s from the counters' change over 5 s), writer alive and heartbeat age; per reader: name, lag (bytes and % of capacity), laps, lost bytes, heartbeat age | `bus::inspect_bus` (maps read-only; what `mda-busctl stat` shows) |
| Feed | Connected, reconnects today, ticks/s, frames/s, subscribed instruments, age of the last tick | FeedStatus on `md` (already read by the gateway) and the gateway's snapshot count |
| Latency | **feed → gateway**: gateway's read time − the snapshot's `receive_ts` (when mda-feedd read the frame), p50 / p99 / max per 5 s window; **exchange → feed**: `receive_ts − exchange_ts`, whole seconds only (Kite's exchange time is per second) | Measured in the gateway's bus reader on every MarketSnapshot (a clock read and a histogram bump: no allocation) |
| Engine | Bars written in the last minute, newest bar time, analytics messages/s | Bars and OptionAnalytics on `an` (counted in the gateway) |
| Notifier | Deliveries queued, sent today, failed today, oldest queued age | `deliveries` in `app.db` |
| Recorder | Enabled (config), today's journal size, gaps written today | Config, journal folder; gaps by counting Gap records is costly, so the recorder's log is not read: **the journal's Gap count comes from the recorder writing a small status file** (`data/run/recorder.status`, JSON, each minute) |

**Platforms:** the gateway runs on macOS; CI builds on Linux. Machine and process stats sit
behind one small interface (`sysmon::Host`) with a macOS and a Linux implementation; a value
a platform can't give is `null` and the page shows "—".

## 4. API

`GET /api/v1/admin/system` (admin only, like the other admin routes): the current values plus
**history**: the gateway samples every 5 s and keeps the last hour (720 samples) in memory for
the trend lines (CPU load, ticks/s, the `md` readers' lag, feed → gateway p99). No storage:
history starts again when the gateway restarts.

```json
{"status": "success", "data": {
  "time": 1792742525, "status": "ok",
  "processes": [{"name": "feedd", "running": true, "pid": 12379, "started": 1792725780,
                 "cpu_percent": 4.1, "rss_bytes": 65011712}, …],
  "machine": {"load1": 1.9, "cores": 8, "memory_used": 12025908224, "memory_total": 17179869184,
              "power": "ac", "battery_percent": 100, "uptime_s": 274000},
  "disk": {"free": 97710505984, "total": 244813135872,
           "files": {"bars_db": 1288490188, "app_db": 2097152, "last_quotes_db": 524288},
           "journal_today": 1717986918, "journal_all": 1717986918},
  "buses": [{"name": "md", "capacity": 67108864, "writer_alive": true, "heartbeat_age_ms": 40,
             "bytes_per_s": 1677721, "messages_per_s": 2410,
             "readers": [{"name": "mda-engine", "lag_bytes": 12288, "laps": 0, "lost_bytes": 0,
                          "heartbeat_age_ms": 90}, …]}, …],
  "feed": {"connected": true, "reconnects_today": 0, "ticks_per_s": 543, "frames_per_s": 4,
           "instruments": 749, "last_tick_age_ms": 210},
  "latency": {"feed_to_gateway_us": {"p50": 38, "p99": 410, "max": 1900},
              "exchange_to_feed_s": {"p50": 0, "p99": 1}},
  "engine": {"bars_last_minute": 687, "last_bar": 1792742460, "analytics_per_s": 120},
  "notifier": {"queued": 0, "sent_today": 14, "failed_today": 0, "oldest_queued_s": null},
  "recorder": {"enabled": true, "running": true, "journal_today": 1717986918, "gaps_today": 0},
  "history": {"step_s": 5, "load1": […], "ticks_per_s": […], "md_max_lag_pct": […],
              "feed_to_gateway_p99_us": […]},
  "checks": [{"card": "disk", "level": "ok", "text": "91 GB free"}, …]}}
```

## 5. Status levels

Each card gets **ok / warn / bad** from fixed thresholds (config later if needed); the page
header shows the worst.

| Check | Warn | Bad |
|---|---|---|
| A stack process not running | — | not running (market hours: feedd, engine, gateway; any time: gateway) |
| Disk free | < 20 GB | < 5 GB |
| A reader's lag | > 25 % of capacity | > 60 % (it is about to be lapped) or laps today > 0 |
| Writer heartbeat age | > 2 s | > 10 s |
| Feed | reconnects today > 0 | disconnected in market hours |
| Last tick age (market hours) | > 5 s | > 30 s |
| feed → gateway p99 | > 5 ms | > 50 ms |
| Power | on battery | battery < 20 % |
| Notifier | failed today > 0 | oldest queued > 10 min |

**Later (not in these units):** the same checks as Telegram system alerts (N6 already sends
feed down / back), so a "bad" also reaches the phone.

## 6. Units

| Unit | Scope |
|---|---|
| **SM1** | `sysmon` library: `Host` (macOS + Linux: load, memory, uptime, power, per-pid CPU and RSS), disk (`statvfs`, file and folder sizes); tests on what can be tested portably |
| **SM2** | Gateway: collector (5 s sampling, 1 h history), bus snapshots (`inspect_bus`), feed and engine counters, latency histogram in the bus reader, notifier counts, recorder status file (+ the recorder writing it); `GET /admin/system` with the checks; gateway tests; `rest.md` |
| **SM3** | Web: Admin → System tab: cards, status colours, trend sparklines, 5 s refresh |
| Later | Threshold alerts to Telegram; thresholds in config |

All read-only and outside mda-engine, so each unit can be built and deployed during market
hours (restarting the gateway; SM2 also restarts the recorder after the close).

## 7. Open questions (defaults chosen)

1. **History length:** one hour at 5 s in memory (default), or longer with a file? Default:
   one hour, nothing stored.
2. **The web process** (Vite dev server) is shown like the others, but it is a Node process:
   its CPU and memory include Vite's. Default: shown, labelled "web (dev server)".
   *Found in SM1:* the pid file names `npm`, whose own CPU is near zero; the work is in its
   child (Vite's node). SM2 adds the process's children for "web".
3. *Found in SM1:* resident memory counts the mapped buses (64 MB `md`, 32 MB `an`) for each
   process that maps them, so feedd, engine, gateway and recorder each show ~200–300 MB.
   SM2 labels it "incl. buses" rather than splitting it.
4. **Who sees it:** admins only (default), as the rest of the Admin page.

## 8. Found in the first live run (SM2, 2026-10-23)

- **feed → gateway latency is ~0.8 ms p50, ~1.5 ms p99**, not tens of µs: the gateway's bus
  reader backs off (sleeps briefly) when the bus is empty instead of spinning, so a tick
  waits up to about a millisecond. Fine for a browser UI; a spinning reader would cost a core.
- **Stale reader slots:** a stopped reader's slot keeps its old position (lag in the
  thousands of %). Only running readers are listed; the rest are counted as `stale_slots`.
- **Engine bars arrive in one burst per minute** (~750), so the rate is counted over 60 s.
