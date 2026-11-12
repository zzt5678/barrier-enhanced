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
