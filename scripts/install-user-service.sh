#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
unit_src="${project_dir}/scripts/weave-enhanced-user.service.in"
unit_dir="${HOME}/.config/systemd/user"
unit_dst="${unit_dir}/weave-enhanced.service"
uid="$(id -u)"
build_dir="${BUILD_DIR:-${project_dir}/build}"
weave_bin="${WEAVE_BIN:-${build_dir}/bin/weave}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/${uid}}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR}/bus}"

if [[ ! -x "${weave_bin}" ]]; then
    echo "missing executable: ${weave_bin}" >&2
    echo "build the project before installing the user service" >&2
    exit 1
fi

mkdir -p "${unit_dir}"
sed \
    -e "s#@PROJECT_DIR@#${project_dir}#g" \
    -e "s#@WEAVE_BIN@#${weave_bin}#g" \
    "${unit_src}" > "${unit_dst}"

systemctl --user import-environment DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS \
    XDG_CURRENT_DESKTOP XDG_SESSION_TYPE WAYLAND_DISPLAY
systemctl --user daemon-reload
systemctl --user enable weave-enhanced.service
systemctl --user restart weave-enhanced.service
systemctl --user --no-pager --full status weave-enhanced.service
