# Review follow-up — 2026-11-11

The supplied static review used `04d4d61a`. This first follow-up checks its
failure paths against the current implementation. Runtime/test commit:
`b5c6379040530c013424c11868c71dafca70eebf`; protocol remains 1.13.

## Implemented and verified in this batch

| Review item | Change and evidence | Boundary |
| --- | --- | --- |
| A1 | Failed leave and malformed options return a parser failure before further acknowledgements or handshake state changes. Connection cleanup runs after parsing has stopped. Tests include deletion of the owned proxy during `handleData`. | This fixes these confirmed paths; it does not claim every reentrant callback has been audited. |
| A2 | File opening uses the existing UTF-8 path helper; file-size lookup uses the error-code API and missing files are omitted without throwing. File-transfer fixtures now include Chinese and accented characters. | Verified on macOS/Linux; native Windows execution remains pending. |
| A3 | Drag metadata is validated as a complete list before publication: field count, basename, integer size/overflow, and entry type. Both typed and legacy metadata are supported, including legacy filenames `F`/`D`. | The legacy format still cannot encode commas. Such source paths are omitted with a warning; malformed received lists are discarded. Escaped/length-prefixed encoding remains future protocol work. |
| A4 | Failed spool removal no longer throws during error cleanup; receive state is cleared and repeated cleanup is safe. Test uses a nonempty directory to force a real filesystem removal error. | An undeletable file may remain; this is not a claim that disk cleanup always succeeds. |
| B1 | Replacement recomputes whether the old source still owns input. If timeout/disconnect has restored the primary, the new connection is adopted without entering it. | Existing test incorrectly expected a new enter after primary recovery; that expectation was corrected. |
| B5 | Replacement transfers saved screensaver ownership to the new connection instead of retaining the retiring proxy. | This addresses the dangling reference; the broader handoff/replacement/screensaver transaction design remains open. |
| D5 | Input registration resumes on crossing the low watermark, including successive 4 KiB reads. A full-buffer-to-half-buffer single read is no longer required. | A separate explicit socket pause API is not implemented here. |
| G3 | Current README now names protocol 1.13 and the modernization branch. | Historical design-review baseline metadata is retained as historical evidence. |

The new failure tests were first exercised against the old paths. The original
options handler continued after failure and crashed in the handshake case.
Seven additional cases failed before the corresponding fixes, including the
4 KiB read progression, replacement timeout, screensaver ownership and spool
removal failure. A real ownership/deletion test was then added and passed.

## Validation

- macOS: 1,223 core tests passed.
- Ubuntu: 1,124 core tests passed.
- Ubuntu AddressSanitizer: 274 relevant tests passed, including owned-proxy
  destruction, reconnect, TCP flow control, drag metadata and receive buffers.
- macOS AddressSanitizer could not start: its runtime deadlocked while
  initializing shadow memory through the system allocator. That process was
  stopped; this is **not** a successful macOS sanitizer run.
- Six component/version/release-policy checks passed on each platform.
- Isolated Xvfb/TLS smoke passed: ten crossings/returns, clipboard, emergency
  modifier release, and a 1 MiB synthetic file verified by digest.
- GUI, server and client were built and installed together on Mac mini,
  MacBook and Ubuntu, and both client control/bulk channels reconnected.
- Windows native/service/installer acceptance and long-duration physical
  acceptance are not claimed by these checks.

## MacBook latency investigation remains open

The earlier `63702e99` change makes secondary macOS input observation passive.
The user still reported noticeable latency after deployment. A passive native
sample's 95th-percentile event age improved from roughly 2.65 ms to 0.61 ms at
the annotated-session observer, but this excludes capture, transport, receive
queue time and display scanout. It is not an end-to-end latency measurement.
Separate LAN ping samples also had approximately 70 ms RTT spikes.

Opt-in diagnostics now correlate actual protocol motion sequence/epoch across
server send (`S`), client dispatch (`R`) and native-call completion (`P`). Set
`WEAVE_INPUT_TRACE_PREFIX` to a local filename prefix before launching the core.
Each process creates a new mode-0600 `prefix.PID.csv`, records only motion
coordinates/identifiers/timestamps, and stops after ten minutes from first
motion or 300,000 rows. It is disabled by default and never generates input.
Files are buffered, so an incomplete final block can remain until flush/exit.
Cross-host wall-clock offsets must be measured separately; raw timestamps
must not be interpreted as one-way latency without that correction. Native
call completion still does not establish visible display scanout time.

Other findings in the supplied review remain unverified or unaddressed in this
batch, including B2–B4, C1–C4, D1–D4/D6–D14, GUI/UX items E/F, CI/packaging G1/G2,
and the remaining cleanup/architecture suggestions. They are not marked closed
merely because this batch passes tests.
