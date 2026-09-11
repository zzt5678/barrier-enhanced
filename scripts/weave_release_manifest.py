#!/usr/bin/env python3
"""Inspect a trusted candidate directory; emit JSON without installing anything."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re

from weave_doctor import assess, binary_architecture, inspect_binary

SOURCE_ROOT = Path(__file__).resolve().parents[1]
WINDOWS_RULE = SOURCE_ROOT / "src/gui/DeployWindowsGui.cmake"
PLATFORMS = ("Linux", "Darwin", "Windows")


def required_files(system):
    if system != "Windows":
        return ["weave", "weavec", "weaves"]
    # The installer stage is the authority; fail closed if its syntax changes.
    text = WINDOWS_RULE.read_text(encoding="utf-8")
    match = re.search(r"set\(release_runtime_files\s+([^)]*)\)", text)
    if not match:
        raise ValueError("WindowsStageRuleUnavailable")
    names = match.group(1).split()
    if not {"weave.exe", "weavec.exe", "weaves.exe", "weaved.exe"}.issubset(names):
        raise ValueError("WindowsStageRuleMissingCore")
    if any(not re.fullmatch(r"[A-Za-z0-9_./-]+", name)
           or ".." in Path(name).parts or Path(name).is_absolute() for name in names):
        raise ValueError("WindowsStageRuleUnsupported")
    return sorted(set(names))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def binary_platform(path):
    with path.open("rb") as source:
        magic = source.read(4)
    if magic == b"\x7fELF":
        return "Linux"
    if magic[:2] == b"MZ":
        return "Windows"
    if magic in (b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
                 b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                 b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
        return "Darwin"
    return "unknown"


def manifest(bin_dir, source_commit, system):
    if not re.fullmatch(r"[0-9a-fA-F]{40}", source_commit):
        raise ValueError("SourceCommitMustBeFull40Hex")
    if system not in PLATFORMS:
        raise ValueError("UnsupportedPlatform")
    source_commit = source_commit.lower()
    root = Path(bin_dir).resolve()
    records, errors = [], []
    names = required_files(system)
    for name in names:
        path = root / name
        record = {"file": name, "role": "component" if Path(name).stem in
                  {"weave", "weavec", "weaves", "weaved"} else "runtime"}
        records.append(record)
        try:
            path.resolve().relative_to(root)
            if not path.is_file():
                raise FileNotFoundError(name)
            initial_hash = sha256(path)
            record.update(sha256=initial_hash, size_bytes=path.stat().st_size,
                          platform=binary_platform(path), architecture=binary_architecture(path))
            if record["platform"] != system:
                errors.append("BinaryPlatformMismatch:" + name)
            if record["architecture"] == "unknown" or record["architecture"].startswith("universal"):
                errors.append("ArchitectureUnverified:" + name)
            if record["role"] == "component":
                identity = inspect_binary(path)
                record.update({key: identity[key] for key in ("build_id", "protocol", "error") if key in identity})
                revision = re.search(r"\+([0-9a-fA-F]{8})\.\d{8}$", record.get("build_id", ""))
                if not revision or revision.group(1).lower() != source_commit[:8]:
                    errors.append("SourceRevisionMismatch:" + name)
            if sha256(path) != initial_hash:
                errors.append("FileChangedDuringInspection:" + name)
        except FileNotFoundError:
            record["error"] = "ComponentMissing"
            errors.append("RequiredFileMissing:" + name)
        except ValueError:
            record["error"] = "PathEscapesCandidate"
            errors.append("PathEscapesCandidate:" + name)
        except OSError as exc:
            record["error"] = type(exc).__name__
            errors.append("FileInspectionFailed:" + name)
    components = [record for record in records if record["role"] == "component"]
    errors.extend(assess(components))
    architectures = {r["architecture"] for r in records if "architecture" in r}
    if len(architectures) > 1:
        errors.append("ArchitectureMismatch")
    return {"schema_version": 1, "kind": "candidate-component-identity-manifest",
            "source_commit": source_commit, "bin_dir": str(root), "platform": system,
            "required_files": names, "files": records,
            "errors": sorted(set(errors)), "ok": not errors,
            "windows_hook": "statically linked into core; no separate hook DLL in current stage rule" if system == "Windows" else None,
            "limitations": ["Unsigned identity inventory, not an installer, signature, SBOM or deployment verification.",
                            "Build IDs attest only an embedded eight-character revision; they do not prove source provenance or a clean build.",
                            "Only required candidate files are inventoried; extra files and full dependency closure are not validated.",
                            "Version probes execute trusted candidate components with --version on the native host.",
                            "Universal Mach-O slices are not yet validated and therefore fail closed.",
                            "Permissions, hooks at runtime, TLS and live handshakes are not tested."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-commit", required=True, help="Full candidate source SHA-1 commit (40 hex)")
    parser.add_argument("--bin-dir", required=True, help="Candidate bin directory, macOS Contents/MacOS, or Windows stage/payload")
    parser.add_argument("--platform", choices=PLATFORMS, default=platform.system())
    args = parser.parse_args()
    try:
        result = manifest(args.bin_dir, args.source_commit, args.platform)
    except (ValueError, OSError) as exc:
        result = {"schema_version": 1, "ok": False, "errors": [str(exc)]}
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
