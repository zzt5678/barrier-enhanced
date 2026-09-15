# Transfer engine implementation plan

Goal: implement Task 3 of the Weave experience plan with real selected-peer delivery and final-save evidence.
Spec: docs/superpowers/plans/2026-09-15-transfer-contract.md.
Architecture: core-owned tasks; bounded typed control messages; existing authenticated bulk payloads; atomic selection-folder publication; GUI consumes snapshots/events.
Tech stack: existing C++17, GoogleTest, native platform IPC, existing TLS and filesystem helpers. No new cloud or package dependency.

## Global Constraints

- Preserve existing input/clipboard behavior and old-peer compatibility. Do not route by changing pointer or clipboard ownership.
- Publication success requires final destination no-replace publication, not 1.12 CommitAck.
- Keep all filesystem work within the interactive user's authority; reject elevated/SYSTEM file commands and incoming publication.
- No production mutation in implementation tasks; use isolated fixtures and displays. Source files and published user output are never cleanup targets.
- Bound messages, tasks, archive/spool/staging ownership and retries. No implicit resend/resume.

### Task 1: Typed task contract, state and journal

Own new focused shared files under src/lib/barrier/TransferTask* and tests under src/test/unittests/barrier/TransferTask*; no GUI/core integration yet.

1. Define stable task/attempt IDs, phases, peer handles with generation, explicit request/result/progress records and capability reasons. Stable IDs use SecureRandom; invalid/unknown fields are rejected.
2. Implement bounded versioned codec suitable for local/wire records and journal snapshots. Maximum frame is 64 KiB; validate lengths/counts before allocation, no trailing bytes or uncontrolled enum casts. Do not use unbounded ProtocolUtil strings directly as an outer frame parser.
3. Core task state validates transitions, deduplicates requests and ignores stale generation/attempt/sequence. Distinguish hop accepted, publishing, completed, failed, cancelled, interrupted and publication-unknown. Capture real counter meaning.
4. Implement atomic private metadata journal with restart reconciliation and bounded terminal retention. Validate referenced task-owned paths, never delete original or published files. Keep journal operations and in-memory state separate enough to test failures.
5. Account reservations across all task-owned copies within stated limits. Tests cover malformed/truncated/oversized codec, state transitions, stale replies, cancellation race rules, dedupe, quota release and restart with corrupt/interrupted journals.
6. Reconfigure/build unittests, run targeted tests, self-review and commit.

### Task 2: Safe publication and cancellation

Own focused shared publisher, archive cancellation context, reusable filesystem helper if needed, and corresponding tests. Preserve RemoteFileClipboard wrappers and existing behavior.

1. Reuse io/filesystem.h rename_no_replace and secure temp-file helpers; do not duplicate native no-replace implementations. Extract/generalize RemoteFileClipboard.cpp extractPackageAtomically rather than duplicating its staging algorithm.
2. Stage an entire selection beneath receiver inbox on its filesystem, in a verified private directory. Write recoverable task marker, then atomic no-replace publish of the enclosing folder. Typed result carries full paths and exact completion/error/cancel state.
3. Add optional cooperative cancel/progress context at existing TransferArchive traversal and 64-KiB copy checkpoints, preserving old overloads and Thread::testCancel behavior. Ensure exceptions release only owned temporary paths through RAII.
4. Test valid multifile/folder/Unicode fixtures, collision/no-overwrite, traversal/symlink rejection, cancellation before/after publication boundary, injected extraction/publication failure and marker-based recovery. Verify resulting bytes/digests.
5. Run affected archive/clipboard/drop/filesystem tests plus new publisher tests; self-review and commit.

### Task 3: Authenticated local command adapters

Own new local control adapters plus existing IPC typed relay extension and App/ClientApp/ServerApp lifecycle integration. No arbitrary path commands on unauthenticated localhost.

1. Unix uses owner-only AF_UNIX endpoint with kernel peer identity and matching unprivileged UID. Validate endpoint ownership and fail closed; strict bounded frame handling and connection limits.
2. Windows service mode reuses authenticated daemon relay to exact active interactive-user core with matching SID/session/PID/generation. Reject elevated/input-discovery nodes; enforce in both relay and core. Windows desktop mode uses same-user authenticated adapter.
3. Support task command dispatch and snapshot/event subscription, bounded/coalesced output and resnapshot on reconnect. Command acceptance is not completion. Clean endpoint ownership/lifetime on shutdown.
4. Test unauthorized identity, malformed frames, stale core generation, standby/elevated nodes, reconnect, bounded queues and cleanup; native Windows/macOS checks remain acceptance gates.

### Task 4: Direct chosen-peer delivery and receipts

Own protocol negotiation/proxies, task routing into existing bulk engine and direct core integration tests.

1. Negotiate protocol 1.13 and explicit-task capabilities; older peers retain 1.12 behavior and do not receive new message codes. Define bounded offers, offer ACK, progress, cancel, publication result and status query/result.
2. Add explicit target entry without m_active mutation. Offer reserves exact bound hop before bulk Start. Explicit package receiver bypasses drag metadata and sends payload to publisher.
3. Coordinate with existing singleton senders and backpressure. Use immutable task IDs for event-thread completion, not retained raw peer pointers. Handle final publication after hop CommitAck and reconcile lost result by authenticated receipt query.
4. Test server→client/client→server, different active pointer peer, old peer, failure/disconnect/stale reply, hash integrity and delayed publication. Sender success follows final saved result only.

### Task 5: Relay and integrated task service

Own server peer roster/relay mappings and integrated test fixtures.

1. Publish trustworthy connected-peer snapshots with generations and capabilities, including server as destination. No configured-name-only online claim.
2. Client A→server→B stores verified payload privately, forwards in independent bound hop, and relays final B receipt to A. Do not extract into server inbox.
3. Fence routes on peer replacement, bound reservations and cleanup, forward cancel/status outcomes. Persist owner-bound final results and expose truthful interrupted/unknown states after restart.
4. Verify three-endpoint fixture routes and hashes, active-pointer independence, relay failure/quota/cancel, GUI reconnect snapshots and core restart semantics. Review entire backend before GUI integration and native deployment.
