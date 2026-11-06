#!/usr/bin/env bash
# Unattended days (docs/runbooks/daily-start-stop.md, "Leaving the Mac at home"): launchd
# runs this every 2 minutes (tools/launchd/com.mda.watchdog.plist).
#
#   - always: the gateway, notifier and web are running (restarted if they died);
#   - weekdays 08:30–23:50 IST, once today's Kite session exists: mda-feedd started after
#     the login (so it has today's instruments and chains; only before 08:59, never in the
#     market: MCX opens at 09:00), then anything else that isn't running;
#   - 00:05–00:11 IST: the market processes stopped, after the MCX close (23:30, or 23:55
#     while the US is off daylight saving; ADR-0031) and the broker's close of day.
#
# Prints statuses only (stack.sh never prints secrets); output goes to data/logs/watchdog.log.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

SESSION=secrets/kite_session.json
stack() { tools/stack.sh "$@" | grep -v 'already running' | sed "s/^/$(date '+%F %T') /"; }

stack start gateway notifier web

now=$(TZ=Asia/Kolkata date +%H%M)
if ((10#$now >= 5 && 10#$now <= 11)); then
    stack stop feedd engine signald broker autotrader recorder | grep -v 'not running'
    exit 0
fi
[[ $(TZ=Asia/Kolkata date +%u) -le 5 ]] || exit 0
((10#$now >= 830 && 10#$now < 2350)) || exit 0
[[ -f $SESSION && $(TZ=Asia/Kolkata date -r "$SESSION" +%F) == $(TZ=Asia/Kolkata date +%F) ]] || exit 0

# A feed from before the login has yesterday's instrument list: restart it pre-open only.
if [[ -f data/run/feedd.pid && data/run/feedd.pid -ot $SESSION ]] && ((10#$now < 859)); then
    stack restart feedd
fi
stack start
