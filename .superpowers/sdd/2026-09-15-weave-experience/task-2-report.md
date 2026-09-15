# Task 2 — bilingual application and subordinate dialogs

## Delivered

- Normalized saved locales (`zh_CN.UTF-8`, `zh-Hans-CN`, regional English, traditional-Chinese aliases), checked translator resource loading, and English fallback. Settings previews use the same production helper under both QApplication and QBarrierApplication; Cancel restores the saved language and does not save edited fields.
- Live retranslation of Settings, Server Configuration, Screen Settings, Hotkey, Action, command palette and retained workflow/history inspection. Palette command IDs, receipt selections, screen edits and wire/storage tokens remain unchanged. Workflow runtime, capabilities, kind, sensitivity and receipt status are translated only in presentation; raw capability IDs remain in the diagnostic tooltip.
- Added missing simplified-Chinese main/settings/server/palette strings and inspector metadata labels; the compiled resource contains 450 finished translations. Existing unrelated languages/resources were preserved.
- Scrollable editors with fixed, explicitly bilingual OK/Cancel buttons; removed old colored native confirmation icons. Initial size and frame position clamp to available logical screen geometry, once on first show. Screen settings stacks group panels at narrow widths; settings forms wrap long rows. User resize/move remains usable afterward.
- Server canvas grows with the dialog, measures its viewport, uses bundled device symbols, and exposes Add/Edit and Remove controls. Corrected showEvent lifecycle. Cancelling creation of a new screen no longer inserts an unnamed screen into the working model.
- Retained history inspector uses a scrollable body and responsive 4/2-column actions; all actions remain reachable on a 960×540 logical desktop. This is still the history/inspection surface, not Task 4's transfer center.
- Dashboard illustration and brand pixmaps now carry their device pixel ratio. Trash artwork is the unmodified GNOME Adwaita symbol with attribution; its runtime resource is a 96 px PNG, avoiding a new Qt SVG plugin dependency.

## RED → GREEN evidence

Production integration harness: `src/gui/test/MainWindowLayoutTests.cpp`, launched only through the isolation guard in `src/gui/test/run-layout-tests.sh`. It uses Xvfb plus fresh XDG config/data/cache/runtime and TMPDIR. No running Weave core, live desktop clipboard, TCC prompt or production service was used.

Observed RED failures before fixes:

1. `zh_CN.UTF-8` selected English; server view max height was exactly 273; open Screen Settings did not translate. `/tmp/task2-red.log`.
2. Palette/history titles did not change on live switching. `/tmp/task2-red2.log`.
3. Cancelling Add/Edit on an empty cell left a new screen; 2x dashboard images still had DPR 1. `/tmp/task2-scale2-red.log`.
4. Narrow Screen Settings content measured 426 px inside a 376 px viewport. `/tmp/task2-narrow-red.log`.
5. A 1920×1080 physical screen at 2x exposed retained history's 586 px minimum height on a 540 px logical screen. Fixed by scrolling the inspector body while retaining actions outside it.

Commands (run from repository root):

```sh
lrelease src/gui/res/lang/gui_zh-CN.ts
cmake --build build --target guilayouttests guiunittests -j4
bash src/gui/test/run-layout-tests.sh "$PWD/build/bin/guilayouttests" "$PWD/../weave-ui-redesign-2026-09-15/task-2/scale-1"
QT_SCALE_FACTOR=2 bash src/gui/test/run-layout-tests.sh "$PWD/build/bin/guilayouttests" "$PWD/../weave-ui-redesign-2026-09-15/task-2/scale-2"
WEAVE_TEST_SCREEN=1920x1080x24 QT_SCALE_FACTOR=2 GTEST_FILTER=DialogLayoutTests.* bash src/gui/test/run-layout-tests.sh "$PWD/build/bin/guilayouttests" "$PWD/../weave-ui-redesign-2026-09-15/task-2/laptop-2x"
GTEST_FILTER='SettingsDialogTests.*:WorkflowHubDialogTests.*:WorkspaceDashboardTests.*:AppConfigTests.*:HotkeyTests.*:KeySequenceTests.*' bash src/gui/test/run-layout-tests.sh "$PWD/build/bin/guiunittests"
git diff --check
```

GREEN: 5/5 actual GUI integration tests at each normal scale; 1/1 laptop logical-screen matrix; 23/23 matching affected GUI unit tests. Checks cover locale normalization/fallback, Cancel restoration, repeated live retranslation, retained command/receipt IDs and edited screen name, cancel-new-screen, translated receipt metadata without stored status mutation, frame placement, each confirmation/action control, narrow reflow, all three server tabs, main status retranslation, main viewport behavior and DPR-aware illustrations.

## Rendered evidence

`../weave-ui-redesign-2026-09-15/task-2/{scale-1,scale-2,laptop-2x}/`

- `en-*` and `zh-CN-*`: preferences, layout, layout-tab-1 (Automation), layout-tab-2 (Advanced), screen, screen-narrow, hotkey, action, palette, history and history-narrow.
- Main actual home/settings captures: `zh-CN-home.png`, `zh-CN-settings.png`, plus the preserved English width matrix.
- Visually inspected actual rendered Chinese layout/history/preferences/palette/home, English screen/action and narrow stacked editor. These are real widgets, not mockups.

## Limits and review notes

- Native macOS/Windows packaging and live keyboard/mouse/cross-device acceptance are outside this task and were not run. Core input, storage identities, security policy and transfer engine were not changed. Qt file pickers may follow the operating system's language.
- Xvfb prints one `BadMatch/SetInputFocus` warning when the modal cancellation test closes; the cancellation and geometry assertions pass. Existing build deprecations occur in NewScreenWidget, Screen, WorkflowStore and ServerLaunchProfile; no new compiler error remains. `lrelease` still reports two pre-existing unfinished unrelated entries.
- The helper's screen-size tests include physical 1920×1080 at 2x (960×540 logical). Multi-monitor native window-manager positioning and moving an already-open window between different-DPR monitors require native acceptance later; initial placement and resize on the isolated screen are verified.
- No deployment. Changes can be reverted by reverting the task commit. All unrelated controller changes were preserved.
