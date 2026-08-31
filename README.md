# Weave

Weave shares one keyboard, mouse, and clipboard across trusted computers. Move
the pointer across a configured screen edge to control the other machine while
each computer keeps using its own display.

This repository is a stability-focused fork of Barrier. The current work
prioritizes deterministic input ownership, resilient Windows desktop changes,
bounded memory use, and isolation of file and clipboard traffic from control
input.

## Current capabilities

- Cross-screen mouse, keyboard, buttons, and wheel input.
- Text, image, and file clipboard exchange.
- A separate authenticated bulk channel for large clipboard and file payloads
  between current Weave peers.
- TLS transport with peer certificate trust.
- Windows service mode with desktop and session recovery.
- Linux/X11 raw-motion handling and multi-monitor geometry support.
- A GUI that may be hidden or minimized without stopping the data plane;
  explicit **Quit** stops it.

Use the same current Weave build on every peer. Protocol 1.12 is required so
input epochs, transactional handoff, and the connection-bound bulk channel
cannot be bypassed by a legacy peer.

## Release status

The `codex/weave-stability-optimization` branch is under active hardening. A
successful local build is not, by itself, a public-release qualification.
Windows UAC/secure-desktop, sleep/resume, RDP/session switching, installer
upgrade, long-running soak, and cross-machine transfer tests must pass for the
exact release commit and signed package.

Primary regression targets for this branch are Windows 10/11 and Linux/X11.
The inherited macOS code remains in the tree, but macOS packages should not be
described as verified until they are built and tested from the same commit.

## Security model

Run Weave only between machines you trust, preferably on a private LAN or
tailnet. Do not expose its ports directly to the public Internet.

Keep TLS enabled. Verify peer fingerprints before trusting a new device. The
Windows service accepts only authenticated local IPC peers and owns a bounded
replacement transaction for the foreground node. Large payloads have explicit
size, parser, queue, and archive extraction limits.

Security reports and reproducible defects belong in the
[issue tracker](https://github.com/zzt5678/barrier-enhanced/issues). Do not put
passwords, private keys, clipboard contents, or personal files in reports.

## Basic use

1. Install and start Weave on both computers.
2. Select **Server** on the computer whose keyboard and mouse you use.
3. Open **Configure server** and place the client screen on the correct edge.
4. Select **Client** on the other computer and enter the server address.
5. Confirm that the configured screen name exactly matches the client's name.
6. Start both sides and verify the TLS fingerprint prompt before trusting it.

Scroll Lock can intentionally prevent screen switching. If input does not
cross an edge, check topology, screen names, connection state, and Scroll Lock
before restarting either node.

## Build and test

Weave uses CMake, C++17, Qt 5, OpenSSL, and platform input libraries. A typical
Linux release build is:

```sh
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBARRIER_BUILD_GUI=ON \
  -DBARRIER_BUILD_TESTS=ON
cmake --build build-release -j2
ctest --test-dir build-release --output-on-failure
```

The executables are emitted under `build-release/bin` as `weave`, `weaves`,
`weavec`, and `weaved` where supported. Windows release evidence must come from
an MSVC build of the exact same source tree; copying binaries from a different
commit invalidates the result.

## Diagnostics

When reporting a fault, include:

- the full build ID from both peers;
- operating system, session type, and screen topology;
- whether the GUI, desktop mode, or Windows service mode was used;
- timestamps and the smallest reproducible sequence;
- sanitized logs from both sides.

Do not claim a disconnect or input failure is fixed from a single restart.
Repeat the relevant switch, clipboard, transfer, desktop-change, and idle/rejoin
scenario against the exact candidate build.

## Project links

- Repository: <https://github.com/zzt5678/barrier-enhanced>
- Issues: <https://github.com/zzt5678/barrier-enhanced/issues>
- Releases: <https://github.com/zzt5678/barrier-enhanced/releases>
- Architecture review: [`doc/WEAVE_SYSTEM_DESIGN_REVIEW.md`](doc/WEAVE_SYSTEM_DESIGN_REVIEW.md)

## Upstream and license

Weave is derived from Barrier, which was derived from Synergy. The existing
copyright notices and contributor history remain authoritative. The project is
distributed under GPL-2.0; see [`LICENSE`](LICENSE).
