# Local component manifest

`python3 scripts/weave_doctor.py --bin-dir /path/to/bin` emits a JSON manifest
of the installed GUI, client and server (plus daemon on Windows). Each component
includes its absolute and resolved path, header architecture, Build ID, protocol,
or a specific error. Missing components, failed identity queries, version timeouts,
mixed builds and incompatible protocols produce a nonzero exit code.

Add `--running` to compare exact local `weave`, `weavec`, `weaves`, and `weaved`
process names against the installed manifest. No process arguments, environment,
clipboard, trust store, or credentials are read. No remote connection is opened.
The diagnostic executes component images with `--version`, with a five-second
timeout per image; use only your trusted installation directory.

On Linux it executes `/proc/PID/exe`, including a deleted/replaced original image,
rather than trusting the replacement file at its old path. A process exiting
during inspection produces a missing/unavailable error. No matching processes
produces `NoRunningComponents` when `--running` is requested.

On macOS and Windows it can only inspect the process's reported disk path;
replacement after startup can therefore hide a loaded-version mismatch. These
platform enumeration branches have mocked tests, not native-host acceptance.
Executable headers cover ELF, PE, and thin Mach-O. Universal Mach-O is identified
but its slices are not enumerated, and architecture compatibility/emulation is
not asserted. The manifest does not certify input hooks, permissions, TLS, live
handshakes, or end-to-end clipboard operation.

Run tests with `python3 -m unittest discover -s scripts -p test_weave_doctor.py -v`.
Linux tests include a real temporary process whose executable is deleted while
running and a real version-query timeout; neither touches an existing service.
