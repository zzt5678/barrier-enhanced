# Weave experience and transfer center implementation

Goal: implement the user-approved spacious, coherent bilingual desktop experience and useful real file transfer center.
Architecture: retain Qt Widgets, existing authenticated Weave transport and isolated candidate deployment. Adapt layouts to actual viewport sizes; centralize translation loading and display text. Transfer tasks must carry stable identity, explicit destination and final publication acknowledgement.
Tech stack: C++17, Qt 5, CMake, GoogleTest, existing TLS/bulk transfer.
Spec: user screenshots and ../weave-ui-redesign-2026-09-15/audit/FINDINGS.md (relative to repository parent); user approved implementation on 2026-09-15.

## Global Constraints

- Preserve working keyboard/mouse, security settings, configuration and unrelated edits. Do not deploy until candidate gates pass.
- Retain warm neutral/teal visual direction. Do not disable HiDPI or invent online status, progress or completion.
- Chinese and English are display languages; protocol identifiers and stored identifiers stay stable.
- A transfer is saved only after destination publication succeeds. A queued payload or CommitAck is not proof of a saved file.
- Use isolated test data and displays; never overwrite user files in tests.

### Task 1: Responsive shell and settings

Ownership: src/gui/src/WorkspaceDashboard.{h,cpp}, MainWindow.{h,cpp}, shared GUI stylesheet if identified, src/gui/test/WorkspaceDashboardTests.cpp and dedicated layout tests. CMake only if registering new tests. Do not change transfer engine or translation resources yet.

1. Add regression tests showing the actual settings viewport, not outer window width, determines one/two column arrangement. Cover sidebar widths and logical windows 560, 760, 810, 1100.
2. Implement proportionate typography, restrained sidebar, consistent spacing and responsive content/actions. Preserve physical device arrangement and accessible full names. At narrow widths prefer useful reflow over hidden controls.
3. Fix settings right-column clipping using actual viewport geometry and resize handling; controls and bottom action bar remain reachable.
4. Test targeted GUI cases, compile GUI, render isolated actual MainWindow at 1x/2x and inspect both home/settings. Keep evidence outside source under ../weave-ui-redesign-2026-09-15/.
5. Self-review, commit scoped changes, report exact tests and screenshots; no production mutation.

### Task 2: Bilingual application and subordinate dialogs

Ownership: QBarrierApplication, BarrierLocale, SettingsDialog, ServerConfigDialog, ScreenSetupView/Model and retained ScreenSettings/Hotkey/Action/CommandPalette/WorkflowHub dialogs, relevant UI forms, translation resources and focused tests. Integrate Task 1 shared styles without reverting them.

1. Test locale normalization, English fallback, unknown language and repeated live switching.
2. Normalize locale names and check translator load; translate all new visible strings and display-only states. Support open-dialog LanguageChange without losing edits, selection or actions.
3. Redesign subordinate dialogs consistently; clamp initial geometry to available logical screen and reflow narrow content. Modernize server layout controls using existing icons, adaptive grid space and clear instructions.
4. Verify actual dialogs in Chinese/English, both scaling factors; tests must check behavior and reachability, not only construction.
5. Review and commit scoped changes.

Specific verified integration risks: SettingsDialog selects language using exact itemData comparison (normalize this too), previews the translator before save and must restore the original language on cancel. GUI test main uses QApplication, not QBarrierApplication; put normalization/load logic in a testable helper or use a dedicated executable rather than constructing a second application. WorkflowTypes contextKindText and sensitivityText are also used in API summaries and clipboard signatures; retain those stable functions and add separate translated display labels. ServerConfigDialog has a fixed 273-pixel canvas height and showEvent calls QDialog::show() instead of base showEvent; preserve edits while fixing lifecycle and adaptive geometry. Avoid whole-resource translation churn unrelated to supported Chinese/English surfaces.

### Task 3: Explicit file task transport and final-save semantics

Ownership: focused GUI/core command bridge, transfer task data and protocol/core integration with tests. Before implementation record concrete interfaces after reading existing IPC and file receive lifecycle.

1. Establish bounded same-user GUI/core command channel, live peer snapshot and explicit target delivery without moving pointer/clipboard ownership.
2. Introduce stable task/attempt identity and typed events; negotiated protocol addition reports final destination publication, failure and cancellation. Preserve older peer compatibility and report unsupported operations truthfully.
3. Implement server/client and client/client relay over existing authenticated transport. Use existing archive safety, limits and no-overwrite publication.
4. Persist task metadata/final paths safely and reconcile interrupted tasks on restart. No false resume; retry creates a new attempt. Unit and integration tests cover disconnects, publication failure, corruption, target mismatch and final hashes.

### Task 4: Transfer center integration

Ownership: transfer center widget, WorkflowStore/ActionBus integration and shell navigation.

1. Make Transfers an embedded useful page with send files, connected destination selection, active tasks, received files and clear empty/error states.
2. Wire real progress, cancel, retry, open and reveal to Task 3. Clipboard history and diagnostics are secondary. Local save is explicitly local.
3. Verify both languages, narrow widths, keyboard accessibility and real two-endpoint send outcomes; no fabricated telemetry.

### Task 5: Integrated acceptance and staged deployment

1. Review complete candidate diff; fix regressions and run relevant complete test suites.
2. Build/test Windows and macOS using previously authorized device paths; retain pinned Mac signing identity and launch policies.
3. Back up and deploy one platform at a time, verify exact executable version and working connections. Verify transfer files and hashes across requested platforms, including explicit destination and restart persistence.
4. Report actual evidence, recovery paths and any remaining user-visible limitations without claiming subjective beauty is objectively proven.
