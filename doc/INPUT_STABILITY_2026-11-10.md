# Native input stability candidate — 2026-11-10

The code commit is `6b21605cf69bbee20c5f5903039f44330039ca52`.
Both native builds use `2.4.0-release+6b21605c.20261110`, protocol `1.13`.
The GUI, server and client are built and deployed together on each host. Changing
roles must not silently select an older executable.

## Changes included

This candidate preserves the previously uncommitted native-input, handoff,
clipboard-worker, monotonic-clock and socket-readiness changes from the working
implementation, together with the following reproduced cursor corrections:

- Mac local input uses a passive observer. The consuming tap is enabled for
  remote ownership, so local desktop input does not wait for the sharing helper.
- Enable that consuming tap **before** the initial parking warp. Previously, the
  event containing the warp could be queued to the passive tap and then dropped
  at the ownership transition. Its compensation remained and turned the next
  genuine `+1` pixel event into approximately `+1280` pixels on the client.
  Failed leave paths restore passive local observation.
- Normalize a return to the primary edge once, before source revocation. Keep
  movement accumulated during revocation instead of warping to the edge again
  and then replaying the accumulated position.
- Read the primary modifier snapshot without waiting for the input owner, and
  skip modifier queries for edge switches that have no modifier requirement.
- A failed prepare preserves local control and suppresses retries at that edge
  until the pointer retreats. Native input failure is reported to the GUI and
  stops automatic restart rather than repeatedly recapturing the pointer.

The snapshot also retains the existing Windows motion/deadline work; this run
has not newly validated Windows native execution.

## Verified results

| Check | Result |
| --- | --- |
| macOS core unit tests | 1,213 passed |
| macOS GUI unit tests | 172 passed |
| Ubuntu core unit tests | 1,115 passed |
| Ubuntu GUI unit tests | 172 passed |
| Ubuntu native X11 tests on a separate Xvfb display | 26 passed |
| Version/product policy checks | All eight registered CTest groups passed on each host |
| Cocoa helper lifecycle | Background helper, no key/visible window, clean stop without input |
| Isolated two-display TLS/input/clipboard/file smoke | Passed, including rejection of an untrusted certificate |
| Isolated repeated crossing and return | 100 cycles completed |
| Emergency return | Held modifiers released on both virtual displays |
| Component identity manifests | GUI, client and server agree on revision, protocol and native architecture |

The isolated smoke test uses temporary profiles and certificates, two Xvfb
servers and a random loopback port. It does not use the live desktop, clipboard,
production trust database or production listener. Text, PNG pixels and a
synthetic 114,688-byte transferred file were checked for equality. Temporary
keys and test processes are removed by the harness.

### Physical evidence and limits

For Mac server → Ubuntu client, the captured baseline contained three large
initial jumps in five entries. The corrected input implementation was then
checked with physical mouse input: 15 observed entries, no recurrence of the
large initial jump, and the user reported that the center jump was gone.
This observation precedes the final rebuild that assigned the common commit
identity; the final build uses the same input source and passed the complete
checks above, and the installed pair reconnected.

For Ubuntu server → Mac client, the same-version components established trusted
TLS and a separate bulk channel. A directly connected Ubuntu mouse was not
available. The user deferred the physical-input retest. Reverse connectivity
is verified; reverse physical pointer/click/keyboard acceptance remains open.
The original Mac-server configuration was restored after the temporary test.

Native Windows execution, cold boot and long-duration physical use are not
certified by this record. No public stable-release or notarization claim is
made by pushing this branch.

## Reproducing the checks

Check out the code commit above. Use a C++17 compiler, CMake, Qt 5, OpenSSL, the
platform development libraries, and **both** Google Test and Google Mock.
Ubuntu X11 checks additionally require Xvfb, xdotool, xclip and Python Pillow.
A system with only Google Test installed is insufficient for the test build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DBARRIER_BUILD_GUI=ON -DBARRIER_BUILD_TESTS=ON \
  -DBARRIER_BUILD_INSTALLER=OFF -DBARRIER_REVISION=6b21605c
cmake --build build --parallel 4 --target \
  barrier barriers barrierc unittests guiunittests topologysnapshotprobe
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

On macOS, set `CMAKE_PREFIX_PATH` to the installed Qt 5/OpenSSL prefixes when
needed. The separate Cocoa lifecycle probe requires a logged-in desktop, but
creates no input tap and neither presents nor activates a window:

```sh
cmake --build build --target osxcocoalifecycleprobe
build/bin/osxcocoalifecycleprobe
```

On Linux, run the native tests only on a separate display:

```sh
cmake --build build --target integtests
xvfb-run -a -s '-screen 0 2560x1440x24 -nolisten tcp' \
  build/bin/integtests --gtest_filter='*XWindows*'
python3 scripts/test_xvfb_smoke.py --bin-dir build/bin --cycles 100
```

Use the existing read-only manifest command on each native host:

```sh
python3 scripts/weave_release_manifest.py --bin-dir build/bin \
  --source-commit 6b21605cf69bbee20c5f5903039f44330039ca52
```

The sanitized installed-component inventory is in
[`validation/input-stability-20261110.json`](validation/input-stability-20261110.json).
Do not publish live profiles, private keys, raw user input or machine-specific
connection details with validation artifacts.
