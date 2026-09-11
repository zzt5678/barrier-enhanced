# Candidate component identity inventory

Run on the native build host after the candidate build/stage has stopped changing:

```sh
python3 scripts/weave_release_manifest.py --source-commit FULL_40_HEX_COMMIT --bin-dir /absolute/candidate/bin --platform Linux
```

For macOS pass `--platform Darwin` and the candidate app's `Contents/MacOS` directory. For Windows pass `--platform Windows` and `windows-release/Release/payload`. Both inputs identify the candidate explicitly; the script never picks an installed or running service automatically. JSON goes to stdout; exit 0 means the required component identity checks passed, exit 1 reports a failed check. Redirect stdout to a new artifact only when desired.

The script reuses `weave_doctor.inspect_binary` for GUI/core version parsing. It requires matching Build IDs, protocol major 1 at least 1.12, and an eight-character embedded revision matching the supplied full commit's prefix. Each required file has a SHA256, byte size, platform header and architecture. Hashes are checked before and after the version probe to detect changes during inspection. Unknown and uninspected universal architectures fail closed. Use a native thin macOS candidate until universal slice validation is available.

Linux/macOS require `weave`, `weavec`, `weaves`. Windows requirements are read directly from `src/gui/DeployWindowsGui.cmake`'s `release_runtime_files`, including `weaved.exe` and the listed Qt/OpenSSL runtime DLLs/plugins. Missing required files fail. Executables receive version probes; DLLs receive hashes and architecture checks only.

There is **no separate required Windows hook DLL in this source tree**: `src/lib/platform/CMakeLists.txt` builds the platform library STATIC from `MSWindows*.cpp`, and `synwinhk.h` documents migration out of a DLL. Hook code is covered by the core file hashes, but its runtime behavior and separate identity cannot be established by `--version`. If the stage policy later introduces a hook DLL, the script inventories it as a required runtime file without executing it.

This is an unsigned candidate inventory, not a signed release, installer, complete dependency inventory, reproducible-build attestation or three-machine deployment acceptance. Matching a self-reported eight-character revision does not prove that binaries were built from clean source or that the full commit is their cryptographic provenance. Only listed required files are included; extra files are not checked. Existing package staging and installation validation remain separate requirements.

The script itself only reads the candidate and source staging rule, writes JSON to stdout, and does not inspect clipboard, credentials, trust stores or process lists. It executes candidate components with `--version`; use trusted candidates, since this is not a sandbox for arbitrary binaries. It neither installs nor changes a running service. Native permissions, hook functionality, TLS and real cross-machine handshakes require subsequent deployment tests.

Tests:

```sh
python3 -m unittest discover -s scripts -p test_weave_release_manifest.py -v
```
