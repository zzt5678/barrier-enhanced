#!/usr/bin/env bash
set -euo pipefail
# Never attach the real MainWindow harness to a user's desktop/clipboard/config.
binary=${1:?Usage: run-layout-tests.sh /absolute/path/to/guilayouttests [output-directory]}
task_tmp=$(mktemp -d /tmp/weave-layout-test.XXXXXX)
export XDG_CONFIG_HOME="$task_tmp/config" XDG_DATA_HOME="$task_tmp/data"
export XDG_CACHE_HOME="$task_tmp/cache" XDG_RUNTIME_DIR="$task_tmp/runtime"
export TMPDIR="$task_tmp" WEAVE_ISOLATED_UI_TEST=1
mkdir -m 700 "$XDG_RUNTIME_DIR"
unset QT_QPA_PLATFORM
if [[ -n ${2:-} ]]; then
    mkdir -p "$2"
    export WEAVE_LAYOUT_PREVIEW="$2"
fi
xvfb-run -a -s '-screen 0 2800x2200x24' "$binary"
