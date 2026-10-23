#!/usr/bin/env bash
# The live stack in one command (docs/runbooks/daily-start-stop.md).
#
#   tools/stack.sh start [name...]    start (all, or the named ones) if not running
#   tools/stack.sh stop [name...]     Ctrl+C them (then TERM after 15 s)
#   tools/stack.sh restart name...    stop, then start
#   tools/stack.sh status             what runs, since when, and each log's last line
#
# Names: feedd engine gateway notifier recorder web. Each process runs in its own session,
# detached from the terminal, so closing a terminal or the Claude app doesn't stop it;
# its pid is in data/run/<name>.pid and its log in data/logs/. While mda-feedd runs, the
# Mac is kept awake (caffeinate): sleep would stop the feed. Prints statuses only, never
# secrets. Replay processes (config/replay.yaml) are never touched.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

ALL=(feedd engine gateway notifier recorder web)
RUN=data/run
LOGS=data/logs
APPS=build/release/engine/apps
mkdir -p "$RUN" "$LOGS"

command_of() {
    case "$1" in
        web) echo "npm --prefix web run dev" ;;
        *) echo "$APPS/mda-$1/mda-$1" ;;
    esac
}

# What `ps` shows for a live one, to check a pid file still points at it.
signature_of() {
    case "$1" in
        web) echo "npm run dev" ;;
        *) echo "$APPS/mda-$1/mda-$1" ;;
    esac
}

log_of() {
    case "$1" in
        web) echo "$LOGS/web.console" ;;
        *) echo "$LOGS/mda-$1.log" ;;
    esac
}

pid_of() {
    local file="$RUN/$1.pid" pid
    [[ -f $file ]] || return 1
    pid=$(<"$file")
    if [[ $pid =~ ^[0-9]+$ ]] && ps -o command= -p "$pid" 2>/dev/null | grep -qF "$(signature_of "$1")"; then
        echo "$pid"
        return 0
    fi
    rm -f "$file"  # stale
    return 1
}

check_name() {
    for known in "${ALL[@]}"; do
        [[ $1 == "$known" ]] && return 0
    done
    echo "unknown name '$1' (one of: ${ALL[*]})" >&2
    exit 2
}

# The gateway serves the built website (ADR-0027): rebuild it when a source is newer than
# the build, so a restart picks up web changes. Prints one line; the build log goes to
# data/logs/web-build.log.
build_web_if_needed() {
    local built=web/dist/index.html
    if [[ -f $built ]] && [[ -z $(find web/src web/index.html web/package.json web/vite.config.ts -newer "$built" -print -quit 2>/dev/null) ]]; then
        return 0
    fi
    echo "web: building the site (npm --prefix web run build)"
    if ! npm --prefix web run build >"$LOGS/web-build.log" 2>&1; then
        echo "web: build failed; see $LOGS/web-build.log (the gateway serves the previous build)"
        return 1
    fi
}

session_is_today() {
    local file=secrets/kite_session.json
    [[ -f $file ]] && [[ $(TZ=Asia/Kolkata date -r "$file" +%F) == $(TZ=Asia/Kolkata date +%F) ]]
}

start_one() {
    local name=$1 pid
    if pid=$(pid_of "$name"); then
        echo "$name: already running (pid $pid)"
        return 0
    fi
    if [[ $name == gateway ]]; then
        build_web_if_needed || true
    fi
    if [[ $name == feedd ]] && ! session_is_today; then
        echo "feedd: Kite session isn't from today; run build/release/engine/apps/mda-login/mda-login first"
        return 1
    fi
    # A new session (setsid), stdin closed, startup output kept beside the log.
    # shellcheck disable=SC2046
    perl -e 'use POSIX qw(setsid); setsid(); exec @ARGV' $(command_of "$name") \
        </dev/null >>"$LOGS/$name.console" 2>&1 &
    echo $! >"$RUN/$name.pid"
    echo "$name: started (pid $!)"
    if [[ $name == feedd ]]; then
        # Keep the Mac awake while the feed runs; caffeinate ends with it.
        perl -e 'use POSIX qw(setsid); setsid(); exec @ARGV' caffeinate -s -i -w $! </dev/null >/dev/null 2>&1 &
        sleep 3  # let it create the md bus before the readers attach
    fi
}

stop_one() {
    local name=$1 pid
    if ! pid=$(pid_of "$name"); then
        echo "$name: not running"
        return 0
    fi
    kill -INT "$pid" 2>/dev/null
    for _ in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null
        echo "$name: stopped (TERM after 15 s)"
    else
        echo "$name: stopped"
    fi
    if [[ $name == web ]]; then
        pkill -f "$PWD/web/node_modules/.bin/vite" 2>/dev/null  # npm doesn't always pass it on
    fi
    rm -f "$RUN/$name.pid"
}

status() {
    printf '%-9s %-8s %-10s %s\n' NAME PID SINCE "LAST LOG LINE"
    for name in "${ALL[@]}"; do
        local pid since line log
        log=$(log_of "$name")
        line=$([[ -f $log ]] && tail -n 1 "$log" | cut -c 1-110 || true)
        if pid=$(pid_of "$name"); then
            since=$(ps -o lstart= -p "$pid" | awk '{print $4}')
            printf '%-9s %-8s %-10s %s\n' "$name" "$pid" "$since" "$line"
        else
            printf '%-9s %-8s %-10s %s\n' "$name" "-" "stopped" ""
        fi
    done
    if pgrep -q caffeinate; then
        echo "Mac kept awake while feedd runs"
    fi
}

names_or_all() {
    if (($# == 0)); then
        echo "${ALL[@]}"
    else
        for name in "$@"; do check_name "$name"; done
        echo "$@"
    fi
}

action=${1:-status}
shift || true
case "$action" in
    start)
        for name in $(names_or_all "$@"); do start_one "$name"; done
        ;;
    stop)
        # Readers first, the feed last (reverse of start).
        read -r -a names <<<"$(names_or_all "$@")"
        for ((i = ${#names[@]} - 1; i >= 0; i--)); do stop_one "${names[i]}"; done
        ;;
    restart)
        (($# > 0)) || { echo "restart needs names (e.g. tools/stack.sh restart gateway)" >&2; exit 2; }
        for name in "$@"; do check_name "$name"; stop_one "$name"; start_one "$name"; done
        ;;
    status)
        status
        ;;
    *)
        echo "usage: tools/stack.sh start|stop|restart|status [name...]" >&2
        exit 2
        ;;
esac
