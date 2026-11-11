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

### Follow-up live observations

A subsequent 3,328 matched MacBook motion records showed up to 72.5 ms of
additional send-to-dispatch delay above the fastest event in the same epoch
(p95 55.3 ms, p99 67.1 ms). Dispatch-to-native-call-return was p95 0.082 ms,
maximum 3.02 ms. Server sends continued approximately every 5 ms during observed
70 ms client delivery gaps, followed by batches of old coordinates.

The host wall-clock offset changed during collection. Consequently these are
**relative delays from per-host monotonic timestamps**, not absolute one-way
latency; subtracting a later wall-clock calibration from older records produced
invalid negative estimates and was discarded. The physical capture and display
scanout portions remain unmeasured.

An independent TCP echo probe, separate from Weave, reproduced approximately
73 ms p99 RTT spikes. Four short runs covered the overlay path twice, direct
LAN, and direct LAN with macOS Responsive Data service class. Direct LAN and
service class did not remove the spikes. This reproduces a network-path delay
outside Weave; it does not identify whether the remaining cause is Wi-Fi/AP
scheduling, peer-to-peer wireless activity, or OS networking. A subsequent wired comparison was performed after the MacBook route changed
to its USB gigabit Ethernet adapter. The same independent TCP probe over the
existing overlay connection measured approximately 1.6 ms p99 and 2.2 ms maximum
(two runs, 1,351 round trips), versus approximately 73 ms p99 on the earlier
wireless path. Direct wired LAN measured below 1 ms in the sampled runs.

Live interface checks also confirmed that Ubuntu uses Wi-Fi, not Ethernet.
Its concurrent 100-packet ping sample had median 1.79 ms and p95 3.22 ms, with
one 66.6 ms outlier. Thus the evidence concerns the MacBook's particular wireless
path and frequency of stalls, not a blanket claim that wireless cannot work.
Both Weave peers use direct LAN Tailscale paths. Whether the user's visible
cursor catch-up disappeared on Ethernet still awaits physical confirmation.

Other findings in the supplied review remain unverified or unaddressed in this
batch, including B2–B4, C1–C4, D1–D4/D6–D14, GUI/UX items E/F, CI/packaging G1/G2,
and the remaining cleanup/architecture suggestions. They are not marked closed
merely because this batch passes tests.
