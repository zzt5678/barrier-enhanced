# Weave native workspace redesign QA

final result: passed

## Visual target and evidence

Selected target: first displayed concept,
`/home/zyt/.codex/generated_images/01a08fd3-feac-7a62-9689-a04dd5678eed/exec-e7e3396e-3dc4-4d31-b8aa-0dfd3098fa72.png`.

Evidence directory: `/home/zyt/文档/ChatGPT/日常/weave-ui-redesign-2026-09-14`.
The target and `desktop-v5.png` are 1488×1056 pixels, density 1; both were
opened together for comparison. `small-v5.png` covers 560×420. This is a Qt
native app, not a website: no browser/CSS viewport is involved.

The device and receipt preview uses synthetic data, explicitly not proof of
online peers. Native `main-window-home.png`, `main-window-settings.png` and
`main-window-transfers.png` were captured on an isolated Xvfb desktop using
the real application, temporary configuration/data/clipboard, no core start.
Navigation Devices → Settings → Devices → Transfers was exercised.

## Findings and fixes

- Initial desktop capture cropped the monitor images. Fixed explicit image
  bounds and sizing; regression test first failed, then passed.
- Initial narrow capture overlapped names and images. Reused ElidingLabel,
  retained full name in tooltip/accessibility, reserved viewport space before
  image scaling. `small-v5.png` shows all three images without cropping.
- Muted 13px text was too pale. Changed to #66716e on #faf9f6: 4.80:1 contrast.
- Repeated receipts refresh cleared selection. Added a failing test and
  preserved unchanged list contents.
- Transfer rows lacked an operation cue. Added double-click/Enter tooltip;
  keyboard activation and the dedicated transfer navigation remain available.
- Actual transfer dialog squeezed eight buttons into one row and a long path
  expanded it. Changed to two action rows and elided the path with full tooltip.
  Two tests reproduced the failure and passed after the fix; real dialog
  capture confirms full labels.
- Live Ubuntu capture then exposed a status-source bug: the homepage copied
  already-elided text from the hidden settings footer. It now reads the
  ElidingLabel fullText contract; existing elision tests cover that source.

An independent reviewer inspected target, desktop and narrow captures and
confirmed no remaining P0/P1/P2 homepage findings. The actual transfer dialog
was subsequently inspected after its separate repair.

## Fidelity surfaces

- Typography: native system font fallback, stronger heading hierarchy,
  readable Chinese labels; long device names elide rather than overlap.
- Spacing/layout: left navigation, open device canvas, centered sharing actions,
  separated recent-transfer list; native menus retained. Small window scrolls
  vertically; navigation and sharing controls remain reachable.
- Colors: warm neutral surfaces, teal primary/selected states; settings and
  transfer surfaces share the palette. Keyboard focus styles retained.
- Images: generated woven Weave mark and generic monitor raster asset; real
  GNOME symbolic navigation icons with retained source and attribution.
  Actual small and large rendered images were inspected for clipping.
- Copy: real connection/configuration/receipt data, Chinese home/settings and
  transfer labels. No invented online count, latency, OS type or shortcut.

## Intentional differences and remaining P3 polish

No OS logos are guessed from device names. Client mode identifies the server
without pretending to know its full topology. Unobserved peer status remains
unknown. The decorative connection lines, concept subtitle and dummy footer
are omitted. The generic monitor is more substantial than the mock's outline.
Navigation icons are slightly heavier, and receipt filename/time hierarchy can
be refined later. These do not block use. Aesthetic preference still belongs
to the user; this report is not a claim of subjective perfection.

## Verification boundary

151 native GUI tests passed on Ubuntu before the final translation-only
addition. Cross-platform frozen build gates and live rollout are recorded
separately in the evidence directory; screenshots alone do not certify them.
