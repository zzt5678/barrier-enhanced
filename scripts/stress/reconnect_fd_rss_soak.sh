#!/usr/bin/env bash
set -Eeuo pipefail

build_dir="${BUILD_DIR:-build}"
server="${WEAVES_BIN:-${build_dir}/bin/weaves}"
client="${WEAVEC_BIN:-${build_dir}/bin/weavec}"
display="${DISPLAY:-:0}"
port="24837"
cycles="300"
client_hold_seconds="0.2"
sample_every="10"
max_rss_delta_kb="20480"
max_fd_delta="8"
max_thread_delta="4"
tmp_root=""

usage() {
    cat <<'USAGE'
Usage: reconnect_fd_rss_soak.sh [options]

Repeatedly connects and terminates a Weave client while keeping one server
alive, then fails if server RSS, FD count, or thread count grows past limits.

Options:
  --server <path>              Server executable (default: build/bin/weaves, or $BUILD_DIR/bin/weaves)
  --client <path>              Client executable (default: build/bin/weavec, or $BUILD_DIR/bin/weavec)
  --display <display>          X display for both processes (default: $DISPLAY or :0)
  --port <port>                Test listen port (default: 24837)
  --cycles <n>                 Reconnect cycles (default: 300)
  --client-hold-seconds <sec>  Seconds to keep each client alive (default: 0.2)
  --sample-every <n>           Sample server resources every n cycles (default: 10)
  --max-rss-delta-kb <kb>      Max allowed VmRSS growth (default: 20480)
  --max-fd-delta <n>           Max allowed FD growth (default: 8)
  --max-thread-delta <n>       Max allowed thread growth (default: 4)
  -h, --help                   Show this help
USAGE
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --server) server="$2"; shift 2 ;;
        --client) client="$2"; shift 2 ;;
        --display) display="$2"; shift 2 ;;
        --port) port="$2"; shift 2 ;;
        --cycles) cycles="$2"; shift 2 ;;
        --client-hold-seconds) client_hold_seconds="$2"; shift 2 ;;
        --sample-every) sample_every="$2"; shift 2 ;;
        --max-rss-delta-kb) max_rss_delta_kb="$2"; shift 2 ;;
        --max-fd-delta) max_fd_delta="$2"; shift 2 ;;
        --max-thread-delta) max_thread_delta="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[ -x "$server" ] || die "server is not executable: $server"
[ -x "$client" ] || die "client is not executable: $client"
[[ "$cycles" =~ ^[0-9]+$ ]] || die "--cycles must be an integer"
[[ "$sample_every" =~ ^[0-9]+$ ]] || die "--sample-every must be an integer"
[ "$cycles" -gt 0 ] || die "--cycles must be > 0"
[ "$sample_every" -gt 0 ] || die "--sample-every must be > 0"

tmp_root="$(mktemp -d /tmp/weave-reconnect-soak.XXXXXX)"
server_pid=""
client_pid=""

cleanup() {
    if [ -n "${client_pid:-}" ] && kill -0 "$client_pid" 2>/dev/null; then
        kill "$client_pid" 2>/dev/null || true
        wait "$client_pid" 2>/dev/null || true
    fi
    if [ -n "${server_pid:-}" ] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    echo "logs: $tmp_root"
}
trap cleanup EXIT

config="$tmp_root/weave.conf"
server_log="$tmp_root/server.log"
server_name="soak-server"
client_name="soak-client"

cat > "$config" <<CONFIG
section: screens
    $server_name:
    $client_name:
end

section: links
    $server_name:
        right = $client_name
    $client_name:
        left = $server_name
end
CONFIG

mkdir -p "$tmp_root/server-profile" "$tmp_root/client-profile" \
    "$tmp_root/server-drop" "$tmp_root/client-drop"

rss_kb() {
    awk '/^VmRSS:/ { print $2; found = 1 } END { if (!found) print 0 }' "/proc/$1/status"
}

thread_count() {
    awk '/^Threads:/ { print $2; found = 1 } END { if (!found) print 0 }' "/proc/$1/status"
}

fd_count() {
    find "/proc/$1/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l
}

sample_resources() {
    local pid="$1"
    local rss fds threads
    rss="$(rss_kb "$pid")"
    fds="$(fd_count "$pid")"
    threads="$(thread_count "$pid")"
    echo "$rss $fds $threads"
}

wait_for_server() {
    local waited=0
    while [ "$waited" -lt 50 ]; do
        kill -0 "$server_pid" 2>/dev/null || return 1
        if command -v ss >/dev/null 2>&1 &&
            ss -ltn "sport = :$port" 2>/dev/null | grep -q ":$port"; then
            return 0
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    kill -0 "$server_pid" 2>/dev/null
}

echo "starting server on :$port using display $display"
env DISPLAY="$display" "$server" -f --no-tray --debug WARNING \
    --disable-crypto --name "$server_name" -c "$config" --address ":$port" \
    --profile-dir "$tmp_root/server-profile" --drop-dir "$tmp_root/server-drop" \
    --log "$server_log" >> "$server_log" 2>&1 &
server_pid="$!"

wait_for_server || die "server did not start; see $server_log"
sleep 1

read -r base_rss base_fd base_threads < <(sample_resources "$server_pid")
max_rss="$base_rss"
max_fd="$base_fd"
max_threads="$base_threads"

echo "baseline: rss=${base_rss}KB fd=$base_fd threads=$base_threads pid=$server_pid"

for ((i = 1; i <= cycles; ++i)); do
    client_log="$tmp_root/client-$i.log"
    env DISPLAY="$display" "$client" -f --no-tray --debug ERROR \
        --disable-crypto --name "$client_name" \
        --profile-dir "$tmp_root/client-profile" \
        --drop-dir "$tmp_root/client-drop" \
        --log "$client_log" "127.0.0.1:$port" >> "$client_log" 2>&1 &
    client_pid="$!"

    sleep "$client_hold_seconds"
    if kill -0 "$client_pid" 2>/dev/null; then
        kill "$client_pid" 2>/dev/null || true
    fi
    wait "$client_pid" 2>/dev/null || true
    client_pid=""

    if (( i % sample_every == 0 || i == cycles )); then
        kill -0 "$server_pid" 2>/dev/null || die "server exited at cycle $i; see $server_log"
        read -r rss fds threads < <(sample_resources "$server_pid")
        (( rss > max_rss )) && max_rss="$rss"
        (( fds > max_fd )) && max_fd="$fds"
        (( threads > max_threads )) && max_threads="$threads"

        rss_delta=$((rss - base_rss))
        fd_delta=$((fds - base_fd))
        thread_delta=$((threads - base_threads))
        echo "cycle=$i rss=${rss}KB delta=${rss_delta}KB fd=$fds delta=$fd_delta threads=$threads delta=$thread_delta"

        if (( rss_delta > max_rss_delta_kb )); then
            die "RSS delta ${rss_delta}KB exceeded ${max_rss_delta_kb}KB at cycle $i"
        fi
        if (( fd_delta > max_fd_delta )); then
            die "FD delta $fd_delta exceeded $max_fd_delta at cycle $i"
        fi
        if (( thread_delta > max_thread_delta )); then
            die "thread delta $thread_delta exceeded $max_thread_delta at cycle $i"
        fi
    fi
done

echo "passed: cycles=$cycles max_rss=${max_rss}KB max_fd=$max_fd max_threads=$max_threads"
