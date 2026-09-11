# macOS validation, 2026-09-11

## Evidence and scope

The reachable `mac-mini` is an Apple Silicon host (`arm64`), macOS 26.6.2
(25G83), Apple Clang 17.0.0, SDK 26.0, CMake 4.3.1 and Ninja 1.13.2.
Qt 5.15.18, OpenSSL 3.6.3 and Google Test 1.17.0 are already installed.
No application was deployed, restarted, or granted permissions.

Initially the candidate commit `7eed3dc8` was not macOS-build-verified. An attempted
`git archive HEAD | ssh mac-mini ...` transfer was rejected by automatic
approval review because sending the complete source to that host had not
been explicitly authorized. That rejection was respected; no candidate
source or patches were transferred before the subsequent explicit user authorization.
The candidate and `1ff621d6` were both
absent from the remote Git object database.

## Independent existing-source baseline check

A pre-existing checkout was found at
`/Volumes/data/codex-projects/mllm-hallucination-lab/external/barrier-enhanced`,
origin `https://github.com/zzt5678/barrier-enhanced.git`, HEAD `a5a5c6b6`.
Its uncommitted changes were left untouched. Only its already-existing
committed tree was archived locally on the Mac into the isolated directory
`/tmp/weave-modernization.VTHvwU`.

Configuration succeeded without dependency installation:

```sh
/opt/homebrew/bin/cmake -S /tmp/weave-modernization.VTHvwU \
  -B /tmp/weave-modernization.VTHvwU/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5;/opt/homebrew/opt/openssl@3;/opt/homebrew/opt/googletest' \
  -DBARRIER_BUILD_INSTALLER=OFF -DBARRIER_BUILD_TESTS=ON \
  -DBARRIER_REVISION=a5a5c6b6
/opt/homebrew/bin/cmake --build /tmp/weave-modernization.VTHvwU/build --parallel 4
```

The baseline build failed at `OSXClipboard.cpp:139`:

```text
error: cannot cast from type 'size_t' (aka 'unsigned long')
to pointer type 'PasteboardItemID' (aka 'void *')
static_cast<PasteboardItemID>(i + 1)
```

The candidate already uses
`reinterpret_cast<PasteboardItemID>(static_cast<std::uintptr_t>(i + 1))`
at that site. This source comparison explains the baseline error; it does
not prove that the complete candidate compiles or runs on macOS.

## Remaining acceptance

Local follow-up source corrections: `OSXClipboard::has()` and `get()` now
return safely when the first pasteboard item cannot be obtained, rather
than passing an uninitialized item identifier to Carbon. `get()` takes
ownership of copied CFData immediately, handles failure/null data before
conversion, and releases it on every exit including conversion exceptions
and thread cancellation. Zero-length CFData avoids constructing a string
from a potentially null byte pointer. `git diff --check` passed. These
additional edits were subsequently compiled on macOS as described below.

With authorized candidate-source availability on a Mac, configure and build
the exact final candidate, run its tests, compare all component build IDs
and protocol versions, then separately perform pasteboard ownership/leak
and interactive sharing checks. Baseline compiler progress or Linux tests
must not be reported as candidate macOS acceptance.

## Authorized candidate build and tests

The user subsequently explicitly authorized copying and testing on Mac mini.
The tracked working-tree snapshot was transferred to the isolated directory
`/tmp/weave-candidate.O2I1AB`; SHA-256 verified identically on both hosts:
`89348073401f14b6201074b344a17cb17a5c6bf33e8ef9764c011600c608b639`.
New `src/lib/common/CoreRestartPolicy.h` was additionally transferred
(SHA-256 `431815796be950700af50fa5c3a94d176d98470dbd9ad501dfe0633a2a89408d`).
This is an intermediate snapshot, not the final integrated source state.

The build exposed one actual Apple SDK compatibility error in GUI `main.cpp`:
`sigemptyset` is a macro on macOS, so `::sigemptyset(...)` expands to invalid
C++. Removing the global namespace prefix fixed the build. The updated
`main.cpp` and corrected `ServerReconnectTests.cpp` fixture were overlaid.
The full Release build then succeeded, including the modified OSXClipboard.

All three binaries (`build/bin/weave`, `weavec`, `weaves`) report build ID
`2.4.0-release+7eed3dc8.20260911` and protocol `1.12`. The revision was passed
explicitly to CMake because the source snapshot excludes `.git`; it labels
the baseline plus working changes, not an immutable final commit.

Core tests: **923/923 passed** after updating the stale intermediate test
fixture. The initial fixture crashed in an emergency-return test; the
corrected fixture passes. Interactive integration tests were not run.

GUI tests with the default Mac temporary path: 130/132 passed. Two
GuiInstanceCoordinator fixtures use a long UUID socket name which exceeds
the Unix socket path budget with macOS's long `/var/folders/.../T/` prefix.
The same three coordinator tests pass with `TMPDIR=/tmp`. This is a test
name/path failure; it must not be silently counted as a default-environment
pass. The production socket name is shorter.

The fixture was then corrected to use `wv-` plus 16 hexadecimal UUID
characters (64 random bits). After rebuilding, **132/132 GUI tests passed
with the unmodified default macOS TMPDIR**, still using the real local
socket activation behavior and `QT_QPA_PLATFORM=offscreen`.

No running application, login item, permissions, or pre-existing checkout
was modified. End-to-end sharing and pasteboard leak testing remain separate
acceptance work; compilation and noninteractive tests do not establish them.

## Bounded clipboard lifecycle leak check

Added `platform/OSXClipboardTests.cpp`, automatically included only under
CMake's `APPLE` platform branch. It creates/destroys 1000 OSXClipboard
objects and asserts each reference was obtained. Construction calls
PasteboardCreate/PasteboardSynchronize; open() checks the reference and
updates the local timestamp. The test never calls get/has/add/empty and
therefore neither reads clipboard contents nor writes/clears the clipboard.

After rebuilding the test target on Mac, the following completed with exit
status zero, one test run and one passed:

```sh
/usr/bin/leaks --noContent --atExit -- build/bin/unittests \
  --gtest_filter=OSXClipboardTests.repeatedLifecycleAcquiresAndReleasesPasteboard
/usr/bin/leaks --noContent --atExit -- build/bin/unittests \
  '--gtest_filter=OSXKeyStateTests.*'
```

The lifecycle process reported **0 leaks / 0 leaked bytes** after 1000
iterations (1474 allocated nodes, 200 KB; physical footprint 5456 KB).
The existing key-mask mapping baseline also reported **0 leaks / 0 leaked
bytes** (1404 nodes, 192 KB; footprint 5392 KB). Logs are
`/tmp/weave-candidate.O2I1AB/clipboard-leaks.log` and `baseline-leaks.log`
on Mac mini. The retained-node/footprint difference is not a measured leak.
This verifies repeated constructor/destructor ownership on this host;
read/write conversion paths and long-running sharing remain untested by
this deliberately content-free probe.

## Private pasteboard read/write ownership check

The installed Apple SDK `HIServices/Headers/Pasteboard.h` documents that
`kPasteboardUniqueName` (NULL) guarantees a new resource. OSXClipboard now
accepts an optional pasteboard name; the default remains
`kPasteboardClipboard`, preserving application behavior. A second test
explicitly selects the unique resource and never addresses the system
copy/paste or find pasteboards.

`OSXClipboardTests.privatePasteboardRoundTripsTextAndPngRepeatedly` creates
a valid 1x1 PNG with lodepng, then performs 1000 iterations of private-board
clear, empty has/get, text add/has/get equality, and PNG add/has/get byte
equality. A final clear releases the last private payload. It passed on Mac
under `leaks --noContent --atExit`, reporting **0 leaks / 0 bytes** (one test
actually executed; 4315 allocated nodes, 466 KB; footprint 6784 KB).
Evidence: `/tmp/weave-candidate.O2I1AB/private-leaks.log` on Mac mini.

This additionally exercises CFData copy/publish ownership and normal text/
PNG conversions using real Carbon APIs. It does not simulate allocator
failure or prove every exceptional conversion path.

## Bundle script validation

The existing `build_dist.sh.in` now copies only weave/weavec/weaves,
accepts explicit DEPLOYQT or resolves an installed tool, reports missing
tools clearly, and tolerates terminals without color support. The latter
was exposed by a real TERM=dumb test where successful packaging previously
returned tput's failure status. `scripts/test_macos_bundle.py` passes two
isolated checks: exact component set and invalid-tool diagnostics.
