#!/usr/bin/env bash
set -u

SERVICE="${WEAVE_SERVICE:-weave-enhanced.service}"
PORT="${WEAVE_PORT:-24800}"
LOG_FILE="${WEAVE_LOG:-/tmp/weave-clipboard-debug.log}"
DISCONNECT_WARN_COUNT="${WEAVE_DISCONNECT_WARN_COUNT:-3}"

uid="$(id -u)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/${uid}}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR}/bus}"

failures=0
warnings=0

ok() {
    printf 'OK   %s\n' "$*"
}

warn() {
    warnings=$((warnings + 1))
    printf 'WARN %s\n' "$*"
}

fail() {
    failures=$((failures + 1))
    printf 'FAIL %s\n' "$*"
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

check_service() {
    if have_cmd systemctl && systemctl --user show "$SERVICE" >/dev/null 2>&1; then
        local active
        active="$(systemctl --user is-active "$SERVICE" 2>/dev/null || true)"
        if [ "$active" = "active" ]; then
            ok "$SERVICE is active"
        else
            fail "$SERVICE is not active (state=${active:-unknown})"
        fi
        return
    fi

    warn "systemd user service is not readable here; falling back to listener and connection checks"
}

check_connection() {
    if ! have_cmd ss; then
        warn "ss is not installed; cannot verify TCP client connection"
        return
    fi

    local established
    established="$(ss -H -tnp "sport = :$PORT" 2>/dev/null | awk '$1 == "ESTAB" {print; found=1} END {exit found ? 0 : 1}' || true)"
    if [ -n "$established" ]; then
        ok "client connection is established on port $PORT"
        printf '%s\n' "$established" | sed 's/^/     /'
    else
        fail "no established client connection on port $PORT"
    fi
}

check_listener() {
    if ! have_cmd ss; then
        return
    fi

    local listener
    listener="$(ss -H -ltnp "sport = :$PORT" 2>/dev/null | awk 'NR == 1 {print}')"
    if [ -n "$listener" ]; then
        ok "server is listening on port $PORT"
        printf '%s\n' "$listener" | sed 's/^/     /'
    else
        fail "server is not listening on port $PORT"
    fi
}

check_xrandr() {
    if ! have_cmd xrandr; then
        warn "xrandr is not installed; cannot verify local screen topology"
        return
    fi

    local summary
    summary="$(xrandr --query 2>/dev/null | awk '
        /^Screen / { screen=$0 }
        / connected / && /[0-9]+x[0-9]+\+[0-9]+\+[0-9]+/ {
            geometry="";
            for (i = 1; i <= NF; ++i) {
                if ($i ~ /^[0-9]+x[0-9]+\+[0-9]+\+[0-9]+$/) {
                    geometry=$i;
                    break;
                }
            }
            if (geometry == "") {
                next;
            }
            active++;
            outputs = outputs sprintf("%s%s", outputs ? ", " : "", $1 " " geometry);
        }
        END {
            if (screen != "") {
                print screen;
            }
            if (active > 0) {
                print active "|" outputs;
                exit 0;
            }
            exit 1;
        }' || true)"

    if [ -n "$summary" ] && printf '%s\n' "$summary" | tail -1 | grep -q '|'; then
        local screen outputs active
        screen="$(printf '%s\n' "$summary" | head -1)"
        active="$(printf '%s\n' "$summary" | tail -1 | cut -d'|' -f1)"
        outputs="$(printf '%s\n' "$summary" | tail -1 | cut -d'|' -f2-)"
        ok "xrandr has $active active output(s): $outputs"
        printf '     %s\n' "$screen"
    else
        fail "xrandr reports no active connected output"
    fi
}

check_recent_log() {
    if [ ! -r "$LOG_FILE" ]; then
        warn "log file is not readable: $LOG_FILE"
        return
    fi

    local recent disconnects connects switches
    recent="$(tail -500 "$LOG_FILE" 2>/dev/null)"
    disconnects="$(printf '%s\n' "$recent" | awk '/started server/ {count=0} /disconnecting client/ {count++} END {print count + 0}')"
    connects="$(printf '%s\n' "$recent" | awk '/started server/ {count=0} /client ".*" has connected/ {count++} END {print count + 0}')"
    switches="$(printf '%s\n' "$recent" | awk '/started server/ {count=0} /switch from / {count++} END {print count + 0}')"

    if [ "$disconnects" -ge "$DISCONNECT_WARN_COUNT" ]; then
        warn "recent log has $disconnects disconnect(s), $connects reconnect(s), $switches switch event(s)"
    else
        ok "recent log has $disconnects disconnect(s), $connects reconnect(s), $switches switch event(s)"
    fi
}

check_service
check_listener
check_connection
check_xrandr
check_recent_log

if [ "$failures" -gt 0 ]; then
    printf 'RESULT unhealthy: %d failure(s), %d warning(s)\n' "$failures" "$warnings"
    exit 1
fi

printf 'RESULT healthy: %d warning(s)\n' "$warnings"
