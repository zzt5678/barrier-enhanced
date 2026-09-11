# Integrated modernization progress

This continues the initial `7eed3dc8` batch. The supplied audit is a roadmap,
not evidence that every proposed feature already works. Source changes are
kept in the isolated `codex/weave-modernization` branch. No production service
has been replaced by this work.

## Implemented after the initial batch

- **Permanent protocol failure:** both client handshake rejection paths stop
  retrying. Exit code 6 propagates the condition to the GUI and Windows
  supervisor. The GUI stops automatic restart and offers explicit Start after
  correction. Windows blocks the failed command generation, not future user
  Start commands. Ordinary network recovery remains enabled.
- **Emergency return:** Ctrl+Alt+Shift+F12 works alongside the existing Pause
  combination; tests verify actual return to primary and reject missing Shift.
  The real Xvfb held-modifier gate exposed a pre-existing recovery defect:
  forced local return did not notify the still-connected peer to release keys.
  Emergency return now sends leave first; transport failure still cannot
  block local recovery. The unchanged test now observes empty keymaps on
  both displays, rather than leaving Ctrl/Shift/Alt down on the client.
- **Inbox background tasks:** one worker, no unbounded queue, bounded 256-KiB
  copy buffer, cooperative cancellation, GUI-thread completion. Acceptance is
  reported as Queued; Completed appears only after successful publication and
  metadata registration. A second save is rejected while busy. A Cancel Save
  button is available in the workflow hub.
- **Crash recovery and ownership:** an inbox lock prevents concurrent writers
  reclaiming one another's staging. New staging has an explicit versioned
  ownership marker and is reclaimed at the next save after a crash. Unmarked
  directories and symlinks are not reclaimed. Management metadata is now an
  external top-level sidecar; copied `.weave-managed` files are preserved and
  nested payload markers cannot authorize pruning. Old in-folder legacy
  markers are intentionally not migrated or pruned automatically. File
  permissions are retained, and copied directories are owner-only.
- **Image bounds and atomic writes:** dimensions, pixels, decoded bytes and
  encoded PNG bytes have explicit admission limits. Atomic file writes check
  length and commit. Failed images never become available history entries.
- **Image background tasks:** production clipboard and screenshot paths use
  one encoding worker and at most one latest pending clipboard image, sharing
  a decoded-byte admission budget. Worker-owned QImage uses
  implicit sharing. Screenshot acceptance and completion are separate;
  actual history publication runs on the GUI thread. SHA-256 replaces a
  dimensions/encoded-size-only signature for asynchronous clipboard images.
  A newer clipboard revision invalidates a delayed older image completion.
  Shutdown cancels or reclaims assets whose completion was not adopted.
  Independent review found that initially rejecting a busy second image also
  invalidated the first; this was repaired with the bounded latest slot. The
  rapid-image, newer-text and shutdown cases pass 50 consecutive repetitions.
- **Action/receipt accuracy:** the misleading launch_allowed_app path now
  refuses execution until an actual allowlist exists. Materialization is
  distinguished from clipboard publication. A generic transfer-finished log
  is an observation, not proof that destination publication succeeded.
- **Backpressure:** steady-clock no-progress deadlines replace loop-count
  approximations; 1-to-10-ms bounded backoff reduces stalled polling, resets
  on progress and still checks cancellation. Byte budgets are unchanged.
  This is explicitly an intermediate polling implementation, not a new
  event-driven low-watermark API.
- **Diagnostics/release identity:** optional running-process checks and
  executable architecture inspection extend the JSON doctor. Linux uses the
  actual `/proc/PID/exe` image. Release manifests verify embedded revision,
  protocol and required component hashes; Windows follows the existing stage
  component/runtime list instead of inventing a separate hook DLL.
- **macOS follow-up:** first-item Pasteboard failures are checked and copied
  CFData is immediately owned across conversion exceptions. The Apple SDK
  sigemptyset macro call is fixed. Local-socket test names fit normal macOS
  temporary-directory paths.
- **Bundle gate:** reuse the existing macdeployqt workflow, shipping only
  GUI/client/server, refusing missing tools or failed stale-binary removal.
  Bundle assembly tests include a stale test executable and no-color terminal.

## Verification evidence

The Linux integrated Release build and registered CTest suites pass with
isolated XDG state and real localhost sockets. As of iteration 4: 1004 core
tests and 136 GUI tests pass; subsequent additions are recorded in the final
test log. Doctor: 13 tests, including real timeout and deleted running image.
Release manifest: 10 tests. The source change and final build must be tied by
the candidate commit and generated manifest, not the date alone.

`scripts/test_xvfb_smoke.py` uses two private Xvfb displays, random localhost
ports and temporary mutually trusted TLS profiles. Seven observed checks pass:
untrusted client rejection, trusted application handshake, cross-screen entry,
actual client X pointer movement, identical text clipboard, normal edge return,
and emergency F12 return. It never connects to the user's display or live
port 24800; owned processes and temporary private keys are cleaned up.
The expanded scenario also verifies PNG pixels and independent file
materialization with byte-identical content; modifier-release and repeated
switching were then run as separate gates and passed. The 16-MiB file's source
and materialized SHA-256 both equal
`e31a1f08ca3188e843466597bb18f20f2a3fc418c9f01e08980e9babd3eee382`.
After the modifier fix both X keymaps are empty. All 1000 cross-screen round
trips complete in 87.924 seconds in this isolated environment (not a physical
network latency benchmark). The full eleven-check report is
`/tmp/weave-xvfb-smoke-v6x7r3zf/result.json`; all checks are true.

The Mac intermediate snapshot builds with AppleClang/Qt5 and passes 923 core
and 132 GUI tests. A new test repeats 1000 OSXClipboard construction/destruction
cycles; `leaks --atExit` reports zero leaks, with a framework baseline also
zero. An additional 1000-loop text/PNG add/has/get roundtrip on a uniquely
created private Pasteboard also passes with zero reported leaks. No user
clipboard content is read or written by either resource test.
See `MACOS_VALIDATION_2026-09-11.md` for exact snapshot, overlays, commands and
the subsequent final-candidate evidence.

## Remaining boundaries

- Windows candidate native build/testing needs the separately requested
  source-transfer approval. Read-only checks confirmed the existing Windows
  compiler, cached dependencies and baseline source.
- MacBook installation, system permissions and interactive physical-device
  acceptance are distinct from Mac mini compilation and isolated tests.
- PNG write/read conversion loops and 8/24/72-hour memory/latency tests are
  not covered by the constructor/destructor leak measurement.
- The process-wide combined content budget, cross-peer fairness and truly
  event-driven drain notifications are still architectural follow-up work.
  Image limits include only admitted images, not allocations already made by
  Qt or the OS before admission. Synchronous legacy image APIs still return
  actual completion; production capture/clipboard paths use the worker.
- Crash reclamation runs on the next inbox save. Source size/mtime checks
  are not filesystem snapshots; rename is not a power-loss durability claim.
  Very slow filesystem syscalls can still delay cooperative cancellation.
- Structured transfer IPC, Tailscale device pairing UX, per-source privacy,
  Qt6/Wayland and 64-bit/resumable transfers remain separate roadmap items.
  No TLS downgrade, minimum-protocol reduction or raised file limit was used.
- Local artifacts/manifests are not signed/notarized releases and are not
  automatically deployed. Installing a candidate is a separate deliberate
  operation after this evidence is reviewed.
