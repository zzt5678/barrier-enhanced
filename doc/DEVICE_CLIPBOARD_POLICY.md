# Clipboard sharing per device

The global `clipboardSharing` option remains the master switch. A device can
opt out in its screen entry while other devices continue sharing:

```text
section: screens
    mac-mini:
    macbook-air:
        clipboardSharing = false
    ubuntu:
end

section: options
    clipboardSharing = true
end
```

Keep the existing links and other options in a real configuration. This option
is currently supported by the external configuration file; it is not a new
checkbox in the graphical screen editor. Select that external file so GUI
layout regeneration does not discard the override.

The effective value is sent once to each device, including the primary. A
global disable cannot be overridden by a screen setting. Aliases resolve to
the canonical screen name. The server excludes disabled devices from clipboard
ownership, replay and file clipboard prefetch; the client rejects late payloads
and invalidates pending file clipboard publications when disabled. Input and
explicit manual file transfers remain available.

For Mac mini + MacBook + Ubuntu, the intended setup is to opt the MacBook out
of Weave sharing while retaining the Mac mini–Ubuntu path. Apple's Universal
Clipboard can provide the Mac-to-Mac path independently; Weave does not enable
or validate Apple's service. The Mac mini must still read its own clipboard to
share with Ubuntu, so excluding the MacBook does not eliminate every possible
native clipboard stall on the Mac mini. The bounded macOS reader remains in use.

Regression coverage includes configuration serialization, master-switch
precedence, option delivery, excluded-device traffic and ownership rejection,
and preserving the enabled device's path. `scripts/test_xvfb_smoke.py
--clipboard-policy --bin-dir ...` checks opt-out, input continuity and opt-in
using private X displays and a loopback TLS connection.

## Validation and deployment — 2026-11-12

- The main policy change passed 1,245 macOS core tests. The subsequent bulk
  retry correction passed 170 related macOS tests; the final Ubuntu build
  passed 503 related tests. Each final platform build passed six identity and
  release-policy checks.
- The isolated TLS smoke passed text, PNG and file clipboard checks; device
  opt-out blocked both directions while pointer movement and screen return
  continued. A disabled client was restarted and produced neither clipboard
  publication nor rejected snapshots after its fresh bulk connection. Opt-in
  restored exact text on the existing connection. Modifier release and three
  additional crossing/return cycles passed.
- Live inspection of the first policy build caught an initial dirty-cache
  retry bypassing Server routing on bulk attachment. The client rejected it,
  but the sender now checks policy for cached retries, file metadata and queued
  chunks too. The final live MacBook reconnect showed its disabled option and
  no attempted snapshot publication/rejection in that observed startup.
- All three machines were updated to `2.4.0-release+69045bef.20261112` and
  reconnected. The Mac mini keeps its external configuration, with global
  sharing enabled and only the MacBook's screen entry opting out. Control-to-
  cross remains off. No native Apple sharing settings were changed.
- User acceptance: **Ubuntu copying works; MacBook has not frozen again so
  far.** This is short-term real-use feedback. The proposed interaction with
  Apple's Universal Clipboard remains unproven, and long-run freeze freedom
  has not been established. The separate intermittent dual-cursor report also
  remains unresolved.
