#!/usr/bin/env python3
"""Read-only component identity check. Does not read clipboard or trust stores."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess


def inspect_binary(path):
    path = Path(path).absolute()
    result = {"path": str(path), "resolved_path": str(path.resolve())}
    if not path.is_file():
        return dict(result, error="ComponentMissing")
    try:
        output = subprocess.run([str(path), "--version"], capture_output=True,
                                text=True, timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return dict(result, error="VersionUnavailable")
    text = output.stdout + output.stderr
    build = re.search(r"^(?:Build ID |Weave )(\S+)", text, re.MULTILINE)
    protocol = re.search(r"^Protocol version (\d+\.\d+)", text, re.MULTILINE)
    if output.returncode or not build or not protocol:
        return dict(result, error="IdentityUnavailable")
    result.update(build_id=build.group(1), protocol=protocol.group(1))
    return result


def assess(components):
    errors = sorted({c["error"] for c in components if "error" in c})
    builds = {c["build_id"] for c in components if "build_id" in c}
    protocols = {c["protocol"] for c in components if "protocol" in c}
    if len(builds) > 1:
        errors.append("ComponentBuildMismatch")
    if len(protocols) > 1 or any(tuple(map(int, p.split("."))) < (1, 12)
                                 or int(p.split(".")[0]) != 1 for p in protocols):
        errors.append("ProtocolMismatch")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", required=True,
                        help="Installed bin folder or Weave.app/Contents/MacOS")
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    names = ["weave", "weavec", "weaves"]
    if os.name == "nt":
        names.append("weaved")
    components = [dict(inspect_binary(Path(args.bin_dir) / (name + suffix)), component=name)
                  for name in names]
    errors = assess(components)
    print(json.dumps({"schema_version": 1, "os": platform.system(),
                      "host_architecture": platform.machine(), "components": components,
                      "errors": errors, "ok": not errors,
                      "limitations": ["Installed executable identities only; running processes, "
                                       "hooks, permissions, TLS and live handshakes are not checked."]},
                     indent=2))
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
