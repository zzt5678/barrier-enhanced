# Weave modernization: initial defect fixes

Source baseline: `1ff621d6a0cdd45e9d4fe2ee9ccd4ff99b573e1d`.
Review input: `WEAVE_AUDIT_AND_MODERNIZATION_2026-09-11.md` supplied by the user.
Implementation branch: `codex/weave-modernization`.

## Implemented

- F02: release the macOS pasteboard on destruction and error; own converters
  with `unique_ptr`; own non-file CFData writes with RAII; check allocation and
  publication errors. Interpret PasteboardSynchronize as flags rather than an
  OSStatus. This code requires a macOS build and Instruments validation before
  claiming platform correctness or a measured memory improvement.
- F03: one 5000-entry/512-MiB copy budget per save, counting directories too;
  maximum nesting 64; incremental enumeration; refuse special files, symlinks
  and existing destinations; preserve colliding basenames with numbered names
  while retaining extensions. Copy to a same-filesystem temporary directory and
  publish after the whole batch succeeds. Ordinary failures remove staging via
  QTemporaryDir. Size/mtime checks reject observable source changes during copy.
- F08: link CURL::libcurl instead of a host-specific library directory and a
  hard-coded list of dependencies. Full Linux build verified with curl enabled.
- PR01 diagnostic foundation: `python3 scripts/weave_doctor.py --bin-dir PATH`
  emits JSON component paths, build IDs and protocols, rejects missing/unknown
  identities, mismatched builds and legacy protocols, and returns nonzero on
  detected errors. Handles the existing GUI/core version output differences.

The diagnostic reports **installed executables** only. It does not attest
running services, hooks, OS permissions, TLS trust, or an application handshake.
It does not collect clipboard content, credentials or certificate stores.

## Verification

Two new regression tests failed on the original ActionBus implementation:
existing destination preservation and empty-directory budget. After repair all
10 ActionBus tests passed. These include shared-byte-budget rejection using
small equivalent inputs, colliding contents, missing source cleanup, existing
target preservation, depth, symlinks and the existing managed-pruning tests.

Release build: CMake with GUI/tests and libcurl enabled; GCC 11.4 on Linux x86_64.
The eight registered CTest suites passed outside the sandbox with isolated XDG
config/data/cache paths and `QT_QPA_PLATFORM=offscreen`. Inside the sandbox,
socket-based tests failed at local listen/create operations; the GUI tests also
crashed after failed listen assumptions. The host run passed those same tests.
The diagnostic has five Python unit tests and was exercised against all three
newly built Linux executables.

Commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBARRIER_BUILD_GUI=ON \
  -DBARRIER_BUILD_TESTS=ON -DBARRIER_USE_LIBCURL=ON
cmake --build build -j 8
# Supply isolated XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_CACHE_HOME for tests.
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure --timeout 90
python3 -m unittest discover -s scripts -p test_weave_doctor.py
python3 scripts/weave_doctor.py --bin-dir build/bin
```

## Remaining review work and acceptance boundaries

This is the initial defect-fix batch, not completion of the report's twelve-PR
roadmap or a three-platform release. The running Ubuntu installation was not
replaced. Windows/macOS builds, interactive input/clipboard tests, signing,
permissions, relay scenarios and long-duration performance were not verified.
No protocol minimum, TLS requirement or content size ceiling was lowered.

F03 still needs crash-restart reclamation (a killed process can leave its
temporary directory), cancellation/async workers, durable management metadata,
disk-full injection and stronger source snapshot semantics. Size/mtime checks
cannot detect every same-size concurrent rewrite. Publication is a same-volume
rename, not a claim of power-loss durability or resistance to a hostile process
with write access to the user's inbox. Existing pruning and marker semantics
remain, including the distinction between managed cache and user-owned data.

Next dependencies: macOS same-source compilation and resource tests; complete
running-component diagnostics and permanent incompatibility UX; then common
release manifests, bounded asynchronous content jobs, crash recovery and
structured transfer events. Global byte budgets, event-driven backpressure,
Tailscale pairing, emergency-hotkey configuration, privacy changes, Qt6/Wayland,
64-bit transfers and optional research workflows remain separate changes.
The report's proposed RAM/CPU targets are not measured results.

Artifacts are in this isolated checkout. To discard the candidate, stop using
its build outputs; the original checkout and running service are unaffected.
