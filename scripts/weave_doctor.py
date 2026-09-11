#!/usr/bin/env python3
"""Read-only component identity check. Does not read clipboard or trust stores."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import struct

COMPONENT_NAMES = {"weave", "weavec", "weaves", "weaved"}


def binary_architecture(path):
    """Read executable headers only; unknown formats are reported, not guessed."""
    try:
        with open(path, "rb") as binary:
            header = binary.read(64)
            if header[:4] == b"\x7fELF" and len(header) >= 20:
                endian = "<" if header[5] == 1 else ">" if header[5] == 2 else None
                if endian:
                    machine = struct.unpack(endian + "H", header[18:20])[0]
                    return {3: "x86", 62: "x86_64", 183: "arm64", 40: "arm"}.get(machine, "unknown")
            if header[:2] == b"MZ" and len(header) == 64:
                binary.seek(struct.unpack("<I", header[60:64])[0])
                pe = binary.read(6)
                if pe[:4] == b"PE\0\0" and len(pe) == 6:
                    return {0x14c: "x86", 0x8664: "x86_64", 0xaa64: "arm64"}.get(
                        struct.unpack("<H", pe[4:])[0], "unknown")
            magic = header[:4]
            if magic in (b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf") and len(header) >= 8:
                cpu = struct.unpack(("<" if magic[0] in (0xce, 0xcf) else ">") + "I", header[4:8])[0]
                return {7: "x86", 0x1000007: "x86_64", 12: "arm", 0x100000c: "arm64"}.get(cpu, "unknown")
            if magic in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
                return "universal (slices not inspected)"
    except (OSError, ValueError):
        pass
    return "unknown"


def running_components(system=None, proc_root=Path("/proc")):
    """Enumerate only exact component names and pid/path, never command lines."""
    system = system or platform.system()
    found = []
    if system == "Linux":
        try:
            entries = list(proc_root.iterdir())
        except OSError:
            return [], ["RunningProcessScanUnavailable"]
        for entry in entries:
            if not entry.name.isdigit():
                continue
            name = None
            try:
                name = (entry / "comm").read_text().strip()
                if name not in COMPONENT_NAMES:
                    continue
                image = entry / "exe"
                path = os.readlink(image)
                found.append({"pid": int(entry.name), "component": name,
                              "executable_path": path, "inspection_path": str(image),
                              "identity_source": "running-image"})
            except OSError:
                # A matching process may have exited or its image may be protected.
                if name in COMPONENT_NAMES:
                    found.append({"pid": int(entry.name), "component": name,
                                  "error": "RunningImageUnavailable"})
        return sorted(found, key=lambda item: item["pid"]), []
    try:
        if system == "Darwin":
            output = subprocess.run(["ps", "-axo", "pid=,comm="], capture_output=True,
                                    text=True, timeout=5, check=True)
            rows = [line.strip().split(None, 1) for line in output.stdout.splitlines()]
            for row in rows:
                if len(row) != 2:
                    continue
                pid, path = row
                if Path(path).name in COMPONENT_NAMES:
                    found.append({"pid": int(pid), "component": Path(path).name,
                                  "executable_path": path, "inspection_path": path,
                                  "identity_source": "process-disk-path"})
        elif system == "Windows":
            command = ("Get-Process | Where-Object { $_.ProcessName -cin @('weave','weavec','weaves','weaved') } | "
                       "Select-Object Id,ProcessName,Path | ConvertTo-Json -Compress")
            output = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                                    capture_output=True, text=True, timeout=5, check=True)
            rows = json.loads(output.stdout) if output.stdout.strip() else []
            for row in rows if isinstance(rows, list) else [rows]:
                item = {"pid": row["Id"], "component": row["ProcessName"],
                        "identity_source": "process-disk-path"}
                if row.get("Path"):
                    item.update(executable_path=row["Path"], inspection_path=row["Path"])
                else:
                    item["error"] = "RunningImageUnavailable"
                found.append(item)
        else:
            return [], ["RunningProcessScanUnsupported"]
    except (OSError, subprocess.SubprocessError, ValueError, KeyError):
        return [], ["RunningProcessScanUnavailable"]
    return found, []


def inspect_binary(path):
    path = Path(path).absolute()
    result = {"path": str(path), "resolved_path": str(path.resolve())}
    if not path.is_file():
        return dict(result, error="ComponentMissing")
    result["binary_architecture"] = binary_architecture(path)
    try:
        output = subprocess.run([str(path), "--version"], capture_output=True,
                                text=True, timeout=5, check=False)
    except subprocess.TimeoutExpired:
        return dict(result, error="VersionTimeout")
    except (OSError, UnicodeError):
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
    parser.add_argument("--running", action="store_true",
                        help="Compare local running component images; reads only names, pids and executable paths")
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    names = ["weave", "weavec", "weaves"]
    if os.name == "nt":
        names.append("weaved")
    components = [dict(inspect_binary(Path(args.bin_dir) / (name + suffix)), component=name)
                  for name in names]
    running = []
    scan_errors = []
    if args.running:
        running, scan_errors = running_components()
        for item in running:
            if "inspection_path" in item:
                item.update(inspect_binary(item["inspection_path"]))
        if not running and not scan_errors:
            scan_errors.append("NoRunningComponents")
    errors = sorted(set(assess(components + running) + scan_errors))
    print(json.dumps({"schema_version": 2, "os": platform.system(),
                      "host_architecture": platform.machine(), "components": components,
                      "running_components": running, "running_requested": args.running,
                      "errors": errors, "ok": not errors,
                      "limitations": ["Hooks, permissions, TLS and live handshakes are not checked.",
                                       "Linux running identities use /proc/PID/exe; process exit can race inspection.",
                                       "macOS/Windows inspect the process disk path, not the loaded image; replacement can hide mixed versions.",
                                       "Unknown or universal executable architectures are not validated against host compatibility."]},
                     indent=2))
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
