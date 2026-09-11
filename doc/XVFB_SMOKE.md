# Isolated Linux end-to-end smoke test

Run `python3 scripts/test_xvfb_smoke.py --bin-dir build/bin` after building the
candidate components. It requires Xvfb, xdotool, xclip, openssl, and Python Pillow. A sandbox
that prohibits X11/localhost sockets requires the normal execution approval.

The script creates two independent Xvfb displays with TCP disabled, a random
127.0.0.1 listener (never 24800), temporary profiles, separate one-day test
certificates, and SHA256 fingerprint trust databases. It never uses the user's
DISPLAY, production profile, clipboard, trust database, or running Weave process.
Neither encryption nor client-certificate verification is disabled.

Checks include rejection of an untrusted client certificate, trusted TLS and
application handshake, crossing to the client, actual client X pointer movement,
exact synthetic text clipboard contents, decoded PNG pixel equality, and a
synthetic text file materialized under the remote profile with exact contents.
The file is copied on the server then the pointer reenters the client, matching
the current lazy file-transfer trigger; staying on the client while changing the
server clipboard does not by itself demonstrate that transfer.

The test also checks returning across the left edge and Ctrl+Alt+Shift+F12
emergency return while modifiers are held. Both displays' XQueryKeymap must
show all keys released afterward; no corrective client keyup is injected to
hide a stuck-key defect. Clipboard publication
is polled with a deadline rather than treating queue acceptance as completion.

Use `--cycles 1000` for 1000 additional verified crossings and returns. Progress
is printed every 100 cycles, and the final JSON records completed cycles. A
failed content or modifier stage prevents pressure-test success.
Use `--file-mib 16 --cycles 1000` to validate a 16 MiB file before the input
pressure stage. The result records source and materialized SHA256 values as
well as byte count. After the file assertion, the clipboard is reset to the
synthetic text fixture before repeated input cycles, so the test does not
unintentionally repeat a 16 MiB transfer on every crossing.

Only processes started by the script are terminated. Temporary profile trees
(including private keys) are deleted; logs and result.json remain under the
printed `/tmp/weave-xvfb-smoke-*` directory. A failed assertion exits nonzero and
preserves diagnostic logs. This is Linux X11 virtual-display acceptance, not
macOS/Windows input-hook or physical multi-machine acceptance.
