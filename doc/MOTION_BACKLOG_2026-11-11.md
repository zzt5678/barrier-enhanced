# Pointer backlog recovery — 2026-11-11

Runtime code commit: `c3547559`; build ID
`2.4.0-release+c3547559.20261111`, protocol 1.13.

## Behavior

Previously low-latency/nested-remote modes injected every received position even
when a transport stall delivered a backlog. Server-side rate limiting does not
prevent this new backlog at the receiver.

The client now keeps the latest already-queued ordinary absolute position,
including in those modes. It adds no delay/timer and does not wait for more data:
a lone position is still delivered immediately. The pending position is flushed
at input boundaries and at the end of the existing bounded parser batch.

- Button, keyboard and wheel operations retain their order and preceding position.
- Held-button drag/drawing paths retain every coordinate.
- Immediate-mode relative motion retains every delta, including direction changes.
- Stale/replayed input cannot replace accepted pending motion. Revocation drops
  pending motion. Display-info gating also drops pending coordinates rather than
  replaying positions from an invalid geometry.
- Opt-in timing records retain the sequence of the position actually posted.
  `R` records every accepted absolute position; `P` identifies actual native posts.

This is receiver backlog recovery, not a change to transport/protocol reliability.
It cannot eliminate a period during which the network delivers no fresh input.

## Verification

- The old implementation failed the new queued-hover behavior checks.
- The display-info gate test also failed before its corresponding protection.
- macOS and Ubuntu: 175 related client/parser tests passed on each platform.
- Six component/version/release-policy checks passed on each platform.
- An isolated Xvfb/TLS test paused only its own private-display client, queued
  fourteen 3-pixel movements, then resumed it. The original build injected all
  14 positions; the final build injected 1. Both had the same exact final X
  pointer position, 42 pixels beyond the initial position.
- The final native run also passed ten cross-screen/return cycles, text/image/file
  clipboard checks, and emergency modifier release. The test never uses the
  user's live DISPLAY, listener, profiles or certificates.
- GUI, server and client were installed together on Mac mini, MacBook and Ubuntu.
  Both client control and bulk channels reconnected; the Mac bundles' component
  hashes match. The existing wired MacBook configuration and layout are retained.

Reproduce the native queue test on Linux with Xvfb, xdotool, xclip, OpenSSL and
Python Pillow installed:

```sh
python3 scripts/test_xvfb_smoke.py --bin-dir build/bin --cycles 10 --motion-burst
```

Native Windows execution, subjective wireless usability and long-duration
physical acceptance were not newly verified by this change. No claim is made
that the earlier MacBook wireless-path stalls have been repaired.
