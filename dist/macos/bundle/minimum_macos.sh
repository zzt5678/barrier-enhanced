#!/bin/bash
# Read the deployment minimum from every packaged Mach-O, never the host OS.
set -euo pipefail
metadata=$(mktemp)
trap 'rm -f "$metadata"' EXIT
while IFS= read -r -d '' image; do
    case "$("${FILE_TOOL:-file}" -b "$image")" in
        *Mach-O*) "${OTOOL:-otool}" -l "$image" >> "$metadata" ;;
    esac
done < <(find "$1" -type f -print0)
awk '
    $1 == "cmd" { mode = $2 }
    (mode == "LC_BUILD_VERSION" && $1 == "minos") ||
    (mode == "LC_VERSION_MIN_MACOSX" && $1 == "version") {
        n = split($2, parts, ".")
        value = parts[1] * 1000000 + parts[2] * 1000 + parts[3]
        if (value > highest) { highest = value; version = $2 }
    }
    END { if (highest > 0) print version; else exit 1 }
' "$metadata"
