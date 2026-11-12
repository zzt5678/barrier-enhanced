# macOS clipboard provider deadline — 2026-11-12

## Observed failure

The MacBook client recorded native clipboard reads taking 55,003.9 ms and
120,084.3 ms (120,086.3 ms total snapshot time). These are actual native data
read stalls, not evidence that the input event loop itself was blocked for
that entire duration. The user reports recurring clipboard-related freezes;
the scope of the affected applications still needs real-use confirmation.

Disabling clipboard sharing stopped the protocol exchange but did not stop
the macOS worker from polling the native clipboard. Its native data read also
had no interruptible deadline: cancelling a generation could invalidate the
result but could not release the blocked worker.

## Repair

- A read-only sibling `weavec --clipboard-snapshot` process reads and converts
  native clipboard data. It starts before input, screen, GUI or network setup.
  A private pipe carries a bounded, validated snapshot; clipboard contents are
  not placed in command arguments or logs.
- The worker stops and reaps its own reader after approximately two seconds,
  or when its generation is cancelled. A three-second child alarm is a backup
  if the parent disappears. A failed or truncated read is not an empty copy.
- An unchanged clipboard that timed out is not read again for 30 seconds.
  A newly observed native change clears that backoff. Other read failures
  receive a shorter three-second backoff.
- Disabling sharing now cancels the worker's outstanding read and prevents
  further polling, snapshot delivery and publication requests.
- Existing publication staging, native commit, and local-copy-wins checks
  remain in place. Publication is not killed halfway through its native commit.

Native metadata synchronization and publication remain in the worker process.
This is a bound on foreign data reads, not a claim that every macOS pasteboard
API or the external application providing the data can no longer hang.

## Validation

- macOS: 1,238 core tests passed; six product/version policy checks passed.
- Ubuntu: GUI, server and client compiled; six product/version policy checks
  passed. The new native helper implementation is macOS-only.
- A real Carbon promised-data provider was deliberately stalled on a unique
  private pasteboard. The reader stopped in approximately 2,013 ms and returned
  no snapshot. The user's general clipboard was untouched.
- Cancellation, worker destruction, child reaping, timeout backoff, disabled
  polling, truncated replies and successful process-based reads are covered.
- Text, HTML, PNG and materialized file lists retain their expected contents
  through the private-board checks.
- The blocked-provider fixture runs in a fresh test process: cancelling its
  native IPC request disturbed later Carbon pasteboard operations when the
  fault-injection provider shared the rest of the suite's process. The full
  suite passed with that fixture isolated.

## Deployment

The Mac mini, MacBook and Ubuntu GUI/server/client components were installed
as `2.4.0-release+8e3f2119.20261112`. Both Mac bundles passed signature checks
with the existing identity, and their three executable hashes match. Both
clients reconnected with their control and bulk channels. Ubuntu's user service
is active. The signed MacBook reader passed an additional unique-pasteboard
UTF-8 check in 371.2 ms, without touching the general clipboard.

Clipboard sharing was already disabled before this repair and remained
disabled at deployment; Control-to-cross remained disabled. Live clipboard
sharing acceptance is therefore pending, not implied by the connection checks.

The intermittent source Mac mini cursor remaining visible/moving is a
separate unresolved report; these clipboard changes do not claim to fix it.
