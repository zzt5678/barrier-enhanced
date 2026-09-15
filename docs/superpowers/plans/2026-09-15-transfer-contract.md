# Explicit transfer implementation contract

This is the concrete implementation boundary for Task 3 of the approved
Weave experience plan. Source investigation is recorded externally in
../weave-ui-redesign-2026-09-15/transfer-contract.md relative to repository.

## Product behavior

Transfers are explicitly targeted, independent of pointer position and
clipboard ownership. The user chooses one or more local files and a currently
connected destination. The receiver uses its local inbox; a remote sender
cannot specify an arbitrary output path. Each selection is received in a
single uniquely named transfer folder, making final publication one operation
and preventing partially published selections from appearing complete.
Stage beneath the receiver inbox on the destination filesystem, use atomic
no-replace publication, and write a task/attempt recovery marker before publish.
A generic overwrite-capable rename is not sufficient.

Transport remains existing Weave TLS/bulk, with a trusted server relay for
client-to-client routes. This is not end-to-end encryption across the relay.
No cloud, SSH data path, additional network service, automatic resend or fake
resume is introduced. Retry creates a new attempt after explicit user action.

## Core interfaces and lifecycle

- A core-owned task service accepts send, cancel, query tasks and query peers.
  Requests carry IDs and core instance identity; replay does not duplicate work.
- Immutable route identity binds source, target and connection generation.
  Reconnection does not silently replace the intended recipient.
- Bounded typed task events identify task/attempt, phase, byte counter meaning,
  timestamp and final result. GUI is a consumer, never authoritative executor.
- Existing 1.12 CommitAck stays next-hop verified-payload acceptance. A new
  negotiated 1.13 task result means final destination publication, with saved
  roots, digest and receipt identity. Never send new frames to older peers.
- Explicit offers reserve a 1.12 hop transaction before bulk Start. A relayed
  payload remains in private spool, not the server's inbox, and uses a separate
  bound transaction on the second hop.
  Wait for TaskOfferAck before Start; match binding, generation and hop ID.
  Explicit packages bypass drag-metadata-based manual receive. Logical task
  state remains alive after CommitAck retires the hop transaction.
- Worker completion is delivered to the core event thread by stable IDs;
  workers do not access network streams or retain disconnectable proxy pointers.
  Archive, hashing and journal disk I/O run outside the input event thread;
  serialize journal writes and acknowledge state only after the matching write
  completes. Transfer persistence must not stall keyboard/mouse delivery.
- Initial limits: one active explicit send, eight queued tasks, 128 selected
  roots, 512 MiB archive limit, and 2 GiB aggregate owned spool/staging budget.
  Existing archive safety limits and transfer backpressure remain binding.
  Coordinate admission with existing singleton clipboard/drag senders. Count
  all incoming, outgoing, relay, staging and quarantined ownership; expire
  offer reservations rather than holding resources indefinitely.
- Metadata is atomically persisted, bounded to 200 terminal tasks/30 days.
  Core restart marks unfinished tasks interrupted or publication-unknown;
  it never guesses success or resends automatically. GUI reconnect snapshots
  running-core state. Published output is never deleted by history cleanup.
  Authenticated TaskStatusQuery/TaskStatusResult reconcile a remote receipt by
  task/attempt and owner, with an explicit unknown result after receipt expiry.
  Preserve up to4096 compact current-core request replay tombstones separately
  from visible history. At capacity reject new sends explicitly while allowing
  duplicates, queries and cancellation; never auto-restart input or silently
  evict replay protection. Reject unknown old-core requests after restart.

## Local control security

Keep the existing Windows daemon authentication. A typed relay must target
only the verified active interactive-user core and exact launch generation;
do not broadcast file commands to input helpers. File commands fail closed on
SYSTEM/elevated secure-desktop cores. Temporary transfer unavailability during
such desktop transitions is an explicit capability state, not a hidden retry.
This gates incoming offers/publication as well as source reads. Check actual
token identity, fence workers during replacement, and bind owner SID/session,
active PID and generation. Windows non-service mode needs a same-user local
adapter with the same policy, not an unverified localhost shortcut.

Unix local control must use owner-only endpoint storage and kernel peer UID
validation (SO_PEERCRED/getpeereid), with matching unprivileged core identity.
Messages are length-delimited and at most 64 KiB, bounded before allocation;
file bytes do not travel through GUI IPC. No shell commands or bearer secrets
in command arguments/logs. Authenticate before revealing paths or task data.

Use a shared typed codec so platform adapters do not invent different task
semantics. Inspect the existing IPC codecs before choosing the encoding; no
new unbounded parser or dependency is needed merely for convenience.

## Success, cancellation and failure

Preparation/upload/relay/verification/publication are distinct phases. Byte
counters must be described accurately; a sender queued-byte count is not
receiver-written progress. A terminal success follows final receiver publication.
Cancellation before publication cleans only owned staging; cancellation losing
the publication race returns too-late with saved receipt. Unknown acknowledgement
outcome remains unknown until receipt reconciliation, never false failure/success.

Tests must cover correct chosen target while pointer changes, stale generation,
malformed commands, unauthorized endpoint identity, old peers, unavailable
secure desktop, destination failure, lost acknowledgement, cancellation race,
no-overwrite safety, quotas, restart reconciliation and actual destination hashes.
