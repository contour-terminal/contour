# Pre-flight scan — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-1c.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.4 (binds), with what it cites (§1,
§2.1 incl. the acceptance at `:107-110`, §2.3, §2.5, §2.6). Scanned against HEAD 16273d5d on
`feature/native-redesign-phase0`. The plan's numbers are at 9811812d. Since then, 4a's Tasks 1–4 have landed (56d36049,
41b99e0b, bcfac1f0, 483e5e82, 98193c60, 16273d5d), along with the 1b cleanup 97e661ac and the 1d and Step 2 plan docs.
Of the files 1c cites, these moved: `ChromeTokenGroups.hpp` (+_test), `ChromeTokenObjects.hpp`, `TabStrip.qml`,
`Main.qml`, `WindowController.{hpp,cpp}`, `TerminalSessionManager.{hpp,cpp}` (+_test), `TerminalSession.cpp`,
`MockMainController.hpp`, `QmlComponents_test.cpp`, `MainWindowQml_test.cpp`, `ChromeSnapshotPlan.hpp`,
`ChromeSnapshotScene.hpp`, `ChromeSnapshot_test.cpp`, `CMakeLists.txt` and the size baseline. These did not move:
`TabItem.qml`, `TitleBar.qml`, `DaemonDriftBadge.qml`, `TabColorFlyout.qml`, `UiStyle.hpp`, `UiStyleProvider.cpp`,
`ChromeTokenObjects.cpp`, `TabListModel_test.cpp` and `Accessibility_qml_test.cpp`.

The rulings in `progress.md` (D1–D7, the spec-gap rulings) and 1d's S1–S4 are settled, and this scan does not reopen
them. **4a's Task 5 (Finish, `/simplify` over the step's src diff) is running now at BASE 16273d5d**, so 1c's Task 1
BASE is that commit. Re-check any cite below in a file it touches.

## Pre-flight scan

### 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → 4 | `TabBadgeKind`, `TabBadgeLooks`/`tabBadgeLook`, `BadgeMark`/`badgeMarkName`, the look's `StatusColorRole ink` | `UiStyleProvider::badgeLook(int)` → `{ glyph, mark, ink = statusInk(look.ink), name }` | Names and types match. The ink is a role, and `statusInk(int role)` resolves it per row, so the Terminal row's `schemeOnBar` reaches the glyph. |
| 1 → 5 | `resolveTabBadge`, `TabActivity`, `FinishedBlockBadge`, `finishedBlockBadgeOf`, `strongerFinishedBlock` | `TabAttention` (marks), `WindowController::tabBadge(tab, row)` | Consistent. `ProgramStatusBadge::progress` is `optional<uint8_t>` (`ProgramStatusBadge.hpp:50`), and `tabBadge()` widens it, as the spec-gap ruling says. `BadgeKind` order is `None, Working, Blocked, Done, Error` (`:30-37`), and the rule rows name kinds, not numbers. **F12** (Interrupted), **F13** (pins). |
| 2 → 4 / 5 / 7 | `HostChipState`, `hostChipStateOf`, `HostChipLooks`, `hostChipPresence` | `hostChipLook(int)` (4), `WindowController::hostChipState` (5), `HostChip.qml` (7) | Consistent. `ConnectionState` is `Connecting=0 … Stopped=5` (`vthost/client/ConnectionState.hpp:15-23`), as the table assumes. **F11**: `HostChip.state`. **F13**: the row-table pin. |
| 3 → 4 | `config::TabShape` (Task 3 adds the enum only), `TitleBandInputs`/`resolveTitleBand` and its name functions, `layoutTabWell`/`TabWellMetrics` | `LayoutTokens::tabShape` (4); `titleBand(int frame, int mode, int strip, int position)`; `tabWellLayout(qreal, int)` | Consistent. The int encodings line up with the enums: `titleBarVisible` false/true → `ClientBand=0`/`NativeFrame`; `tabBarShouldShow` false/true → `Hidden=0`/`Shown`; `tabBarPosition` 0/1 → `Top`/`Bottom` (`TabBarMode.hpp:16-20`). The metrics are `minTabWidth`/`maxTabWidth` (120/240, `UiStyle.hpp:320-321`), `2·s5 + size.tabIcon` = 34, and `s1` = 2. All six `tabWell.widths` rows recompute exactly. **F20**: the rule key. |
| 4 → 5 | `badgeLook`, `motion.spinner/pulse/full`, `ProgressStatusRoles` | `TabItem.qml`'s badge and progress (`:34-59`, `:199`, `:201-210`, `:370-397`) | Consistent. TabItem's size baseline is 5 (`:180`, `:194`, `:208`, `:209`, `:377`). Three of the five are durations, not two, so Task 5 takes it to 2. Regenerate the baseline, as the plan says. |
| 4 → 6 | the extents, radii, `tint.tab`, `layout.tabShape/pill`, `colors.tab*`, `tabWellLayout`, `tabColorGlyph` | the pill, the well, `TabBadgeSlot`, the `▌` cell | Consistent in names. **F9**: the Terminal Flat strip does not draw from `colors.tab*`. |
| 4 → 7 | `titleBand`, `hostChipLook`, `statusInk`, `snapToCells`, `chip*Glyph`, `size.toolbar*`, `space.dragReserve/revealEdge` | `TitleBar.qml`, `HostChip.qml`, `ToolbarButton.qml`, `DaemonDriftBadge.qml`, Main.qml | Consistent. **F5**: no task says what the band's *height* binds to. |
| 5 → 6 | `TabItem.tabBadge/tabBadgeText/tabBadgeProgress`, the objectNames `tabBadgeSlot/Glyph/Spinner` | Task 6 moves the badge into `TabBadgeSlot` (`tabBadgeMark/Glyph/Spinner` inside) | Consistent. Keep `tabBadgeSpinner` on the `RotationAnimation`, where `programStatusSpinner` is today (`TabItem.qml:371`), so Task 5's spinner case survives the move. |
| 5 → 7 | `WindowController::hostChipState` + `hostChipStateChanged`; the mock's `hostChipState` with a setter | `HostChip.qml` via `win.hostChipState`; the scene's `hostChipState` | Consistent. `win.hostLabel` already exists (`WindowController.hpp:131`, `.cpp:287`), and so does the mock's (`MainWindowControllerMock.hpp:78`). |
| 6 → 7 | `TabStrip.availableWidth`/`wellLayout`; `.decor` and three snapshot rows; the Flat-gated `+`/`▾` | `TitleBar` assigns `availableWidth`; `.drift/.frame/.mode` and five rows; the band's `ToolbarButton` `newTabButton` | **F4** (blocking): the gate lands a task before its replacement. **F8**: a binding loop until Task 7. Two `newTabButton`s (**F6**, **F11**). The snapshot enums append in order, and the `static_assert`s (`ChromeSnapshotPlan.hpp:222-228`) derive the counts. |
| 5 ↔ 7 (`MainWindowQml_test`) | — | Task 7 rewrites the remote chrome case | It is now `:359-401` (`hostBadge` at `:372`, `:389`). Until Task 7 the default row (ContourLinux, Flat, `QmlChromeStyle.hpp:78-84`) keeps `hostBadge`, so the case passes through Tasks 5–6. |
| 7 → existing `ChromeSnapshot_test` | the always-shown macOS band (D1) | `ChromeSnapshot_test.cpp:79` pins `titleBar.isVisible() == isTabBarShown(...)` for every script on MacOS | **F3** (blocking). |
| 1–7 → 8 | each task's filter | Step 2's union; the snapshot count | The union covers every tag. `[contour][gui][qml]` covers `[mainwindow]`, `[a11y]`, `[snapshot]` and `[tabbadge]`. The count is derived: 20 scripts × 16 = 320. **F17**: the `/simplify` base, the staging globs and clang-tidy. `metainfo.xml` 0.9.0 is at `:107` (template `:97-105`), which is right. |
| 1c ↔ 1d spike (S1–S4) | R-q (horizontal snap only), R-h (band fill `systemPalette.window`), R-k (overlay) | S1 Hint, S2 compact toolbar (inset 84, bar 40), S3 GrowBandToCells, S4 Glass + Tint | No contradiction. 1c pins 38 and one cell only through the mock, where `titleBandHeight` is 0, and 1d grows the band from there. The manual check "note where it sits relative to the menu bar" is moot, because the spike measured `fullScreenTop` = 29 pt (**F19**). |

### 2. Per-task self-consistency

| Task | Agrees with itself? | Detail |
|---|---|---|
| 1 | Yes | The priority rows recompute (Failed outranks Working, `{Done, 40}` drops progress). The file goes after `window/RemoteChrome_test.cpp`, now `CMakeLists.txt:1198` (`ProgramStatusBadge_test` is `:1197`). The filter `[contour][window][programstatus]` exists in contour_test. **F12**, **F13**. |
| 2 | Yes | The table matches `ConnectionState`. `[contour][hosts]` exists (`RemoteChrome_test.cpp`, in contour_test). Step 5 skips `editorconfig-checker src/contour/CMakeLists.txt` (**F19**). **F13**. |
| 3 | Yes | Every `tabWell.widths` row is arithmetically right: 728, 147.5, 97.67, 34/Scrolls, 0/Scrolls, and `{}`. The `overflow` pair is 40.9 ≥ 34 (Fits) and 26.6 < 34 (Scrolls). The zsh globs `TitleBandLayout*.{hpp,cpp}` match (header + test), so zsh's `nomatch` is not tripped. The test uses a literal 40 for the drag reserve; take it from `ChromeTokenRows[MacOS].space.dragReserve` once Task 4 lands, or say why it is a literal. **F20**. |
| 4 | Mostly | Every field has a home. No `ChromeColorSource{`/`SizeTokens{` literal exists outside `ChromeTokenGroups.hpp` (grep), so naming `.level = 0` is confined to the header. The contrast claims hold: contour-light's blue `#1F5FBF` on bar `#2B2E35` is about 2.2, which gives `BarText`; contour-dark's `#6FA8F5` on `#2A2D35` is 5.6, which gives `normalColor(4)`. `UiStyle.nativeRowsKeepTodaysGeometry` (`UiStyle_test.cpp:204`) pins `chromeHeightUnits == 34` on MacOS too, so the Pill band must come from `size.titleBar` (**F5**). `chromeTokens.reducedMotion`/`allOverridesTogether` (`ChromeTokenGroups_test.cpp:212-261`) should also compare the new `tint` (which needs `operator==`) and `statusInk`. `errorColor()` becomes `statusInk(Danger)` through `progressColor` (`UiStyleProvider.cpp:298-301`), which moves its Terminal-row value off the fixed `0xD13438` (**F19**). |
| 5 | **No** | **F1** (blocking): no output hook exists. **F2** (blocking): the restore and the unzoom never mark a session seen. **F14**: the name `SessionPlacement` is taken. **F15**: the resync grace. **F16**: the label fallback. The test helpers exist: `TestApp`, `MockPtySessionFactory`, `ScopedControllerWithOsWindow` (`GuiTestFixtures.hpp:866`), the visibility case at `TerminalSessionManager_test.cpp:799`. `TabLabel_test` is in contour_gui_test (`CMakeLists.txt:826`), which matches Step 4's binary. |
| 6 | **No** | **F4** (blocking): the `+` gate. **F8**: the binding loop; the drag-reserve assertion runs before the reserve exists; the pointer cases need a shown window. **F9**: the mock gaps, and the Terminal contrast pair. **F10**: the flyout tokens. The Fluent/Linux acceptance numbers are right: 34 (`chromeHeightUnits`), 32 (`tabHeightUnits`), `+56` (10+4+22+20), and 32 (`stripButtonUnits`). |
| 7 | **No** | **F3** (blocking): the snapshot visibility pin. **F6**: the new-tab menu. **F7**: the drift chip's home and its dialog. **F9**: "show_title_bar" asserts nothing on the mock. **F11**: `state`, `clicked`, the `tabsBar` duplicates. Step 4 leaves out Main.qml's baseline: 1 → 0, because `anchors.margins: 8` leaves with the badge (`Main.qml:227`). The full-screen case needs `bannerStack.y` to follow the effective height, not `titleBar.height` (`Main.qml:153`). |
| 8 | Mostly | **F17**: the `/simplify` base, the staging globs and clang-tidy. The release-note target is right. The index's Plan column is shifted one row (1d → `-1c.md`, 2 → `-1d.md`, 3a–3e → `-2.md`); fix it in Step 6 (**F19**). |

### 3. References verified against HEAD 16273d5d

Present and correct:
- `ProgramStatusBadge.hpp:30-37` (`BadgeKind`), `:50` (`optional<uint8_t>`), `BadgeRows` `:75-88`
- `UiStyle.hpp`: `:202-209`, `:347-353`, `:422-428` (`progressColors`); `:218-225`/`:357-362`/`:431-437` (glyphs); `:232-238` (`StatusColorRole`)
- `vtbackend/shell/CommandBlock.hpp:266-274` (`CommandEnding`); `isFailure` `:311-323`
- `vthost/client/ConnectionState.hpp:15-23`
- `ChromeTokenGroups.hpp`: `:43-47` (`MenuPresentation`), `:50-58`, `:61-68`, `:71-83`, `:92-101`
- `ChromeTokenObjects.cpp:101-121` (`ChromeColorTokens::color`)
- `UiStyleProvider.cpp:231-241` (`progressColor`), `:243-259` (`statusColor`'s light decision)
- `TabItem.qml`:
  - `:34-59`, `:61-81`, `:104-116`/`:104-123`;
  - `:174-212`, `:199`, `:208-209`;
  - `:353-399`, `:377`, `:389`, `:504-534`
- `TitleBar.qml`: `:60-63` (fill), `:77-161`, `:103`
- `DaemonDriftBadge.qml:1-61` (baseline 4)
- `TabColorFlyout.qml` (12 literals: `:27`, `:88`, `:121`, `:123`, `:194`, `:196`, `:201`, `:217`, `:237-240`); keyboard comment `:7-9`
- `Config.hpp:1188` (`showTitleBar`), `:1200` (`fullscreen`)
- `TerminalSession.cpp`:
  - `:442-450` (`setHostConnection`);
  - `:1247-1261` (the foreground poll, `dynamic_cast<vtpty::Process const*>` at `:1252`);
  - `:2266-2286` (`handleCommandBlockFinished`)
- `vtpty/Process.hpp:160` (`std::optional<std::string> foregroundProcessName() const`)
- `ChromeSnapshotPlan.hpp`: `:35-89`, `:77-81`, `:97-108`, `:111-220`, `:222-228`
- `ChromeSnapshotScene.hpp`: `subject()` `:195`
- `DaemonDriftModel_test.cpp:13-22`; `TabListModel_test.cpp:61-84`, `:108-115`
- `QmlComponents_test.cpp:190-239`, `:1542-1700`; `TerminalSessionManager_test.cpp:799-813`
- `UiStyle_test.cpp:204`
- `metainfo.xml:97-105`, `:107`
- Tools: `/opt/homebrew/opt/llvm@22/bin/clang-format`, `editorconfig-checker`, `scripts/check-qml-sizes.py --print-baseline`
- Tags present: `[chrometokens]`, `[uistyle]`, `[contour][uistyle]`, `[config]`, `[snapshot]`, `[contour][window][programstatus]`, `[contour][hosts]`, `[contour][drift]`, `[contour][gui][model]`, `[contour][multiwindow]`, `[manager]`, `[contour][tablabel]`, `[contour][gui][qml][a11y|mainwindow|snapshot]`, `[display][snapshot]`
- Tags created: `[tabbadge]`, `[hostchip]`, `[titleband]`, `[tabwell]`, `[tabattention]`, `[manager][tabbadge|hostchip]`, `[contour][gui][qml][tabbadge]`, `[…][mainwindow][tabwell|band]`
- 4a's removals are done:
  - no `settingsActive`/`settingsController` anywhere;
  - `SnapshotSurface` replaced `page`;
  - Main.qml's root is `ThemedWindow`;
  - the settings tab and its literal are gone (`TabStrip.qml` baseline 2).

Stale (locate by name; the line is the current one):
- `ChromeTokenGroups.hpp`:
  - `SizeTokens` `:114-131` (4a's four `settings*` fields end it);
  - `SpaceTokens` `:134-142`, `RadiusTokens` `:145-154`, `MotionTokens` `:169-177`;
  - `LayoutTokens` `:227-233`, `ChromeTokenRow` `:236-250`;
  - `schemeColor` `:315-322`, the rows `:324-439`;
  - `resolveChromeTokens` `:457-485`, `chromeSchemeColorsOf` `:490-520`, the `BarText` pick `:500-504`.
- `ChromeTokenGroups_test.cpp`: `numbersOf` `:47-80`, `everyRowMatchesTheDesignKit` `:97`, `barTextReadsOnTheBar` `:349-377`.
- `ChromeTokenObjects.hpp` (+18 lines since 9811812d):
  - `ChromeSizeTokens` `:35-94`, `ChromeSpaceTokens` `:97-146`, `ChromeMotionTokens` `:216-242`;
  - `ChromeLayoutTokens` `:390-…`, `ChromeColorTokens` `:424-…`.
- `TabStrip.qml`:
  - `hostBadge` `:54-81`, the list `:83-208`, the delegate's required properties `:103-135`;
  - reorder `:137-140` (the duration at `:139`);
  - the new-tab buttons `:210-259` (the plan's `:272-321` predates 4a's removal of the settings tab);
  - `newTabMenu` (inside `newTabMenuButton`) `:253-258`, its popup `:287-289`.
- `Main.qml`:
  - `chromeHeight` binding `:96`;
  - `titleBar` `:110-142`, `bannerStack` `:148-186`, `content` `:193-229`;
  - the drift badge `:222-228`.
- `WindowController.hpp`: `Roles` `:140-160`, the remote chrome block `:716-734`, `cachedProgramStatusBadge` `:800`.
- `WindowController.cpp`:
  - `roleNames` `:442-459`, `data` `:461-515` (the badge cases `:500-512`);
  - `programStatusBadge` `:536-549`, `resolvedTabLabel` `:556-581`;
  - `bindWindow` `:1144` (`visibilityChanged` connect `:1206-1210`), `onWindowStateChanged` `:1248-1251`;
  - `refreshActiveTabHighlight` `:1991-2002`.
- `TerminalSessionManager.hpp`: `setHostConnection` `:813-818`, `refreshTabProgramStatusForSession` `:629`.
- `TerminalSessionManager.cpp`:
  - `windowStateChanged` `:428-432`, `sessionVisibility` `:471-490`;
  - `removeSession` `:1138`, `activeTabChanged` `:1286-1296`;
  - `refreshTabProgramStatusForSession` `:1500-1511`.
- `TerminalSession.cpp:893-898` is **not an output post**. `screenUpdated` is `:852-901`; its only post is the find bar's tally, made while the bar is open, and it returns at `:855-856` when there is no display (**F1**).
- `MockMainController.hpp`: `Roles` `:79-95`, `data` `:243-257`, `roleNames` `:258-276`, `titleBarVisible` `:127`.
- `MainWindowQml_test.cpp`: the remote chrome case `:359-401`.
- `Accessibility_qml_test.cpp`: the kit table `:179-208`.
- `CMakeLists.txt`: `ProgramStatusBadge_test`/`RemoteChrome_test` `:1197`/`:1198`; `qml/TabItem.qml` `:393` (`_qml_files` `:383-…`); the heavy list `:865-…` (`TerminalSessionManager_test` `:875`, `QmlComponents_test` `:879`, `UiStyleProvider_test` `:887`, `TabListModel_test` `:886`).
- The spec's own cites are stale, not the plan's: `Config.hpp:1056`/`:1066` → `:1188`/`:1200` (the plan already says so); "the existing roles (`WindowController.cpp:320-336`)" → `:442-459`.

### 4. Interfaces 1c produces for Step 2 and Step 1d

Step 2 (its plan, and its scan's G1–G3 gates: Tasks 3, 6 and 8 wait on 1c Task 4; Tasks 4 and 5 on Task 5; Tasks 9 and 11
on Task 7):

| Step 2 expects | 1c's plan gives | Status |
|---|---|---|
| `refreshTabBadgeForSession(SessionId, OutputOrigin)`, which Step 2 Task 4 extends with `refreshPaneProgramStatus()` | the same signature | **Mismatch in meaning (F1)**: one entry point serves output and OSC 7501 alike. Step 2's F12 already wants the pane-status hook on the program-status path only. Split it into two calls in 1c, and tell Step 2 the names. |
| `TerminalSession::foregroundProcessName()` via `dynamic_cast<vtpty::Process const*>`, a `std::string` | the same | Matches. Step 2 Task 5 rewrites it over `Pty`. **F16**. |
| `ColorSourceKind::{Fill,Label,Surface,SchemeOnBar}`, `ChromeColorSource::level`, `detail::fill/label/schemeOnBar(slot)`, roles through `ChipTextStrong`, `MotionStyle`, `statusInk`, `legibleOnBar`, `badgeLook`, four glyphs beside `tabSeparator` | all of these | Matches (Step 2 uses `fill(1)`, `label(1|2)` and `schemeOnBar(Secondary)`, which are all in range). |
| `TitleBandInputs`/`TitleBandRules`/`resolveTitleBand`/`BarCentre::Empty`; rows keyed on the gate ("ahead of 1c's `Pill`/hidden-gate rows") | rules keyed on `(shape-or-any, frame, position)`, with the gate read inside | **Mismatch (F20)**. Step 2's scan F16 proposes a post-step instead. Settle it in 1c Task 3. |
| `UiStyleProvider::titleBand(...)` (the density is added later); Main.qml's `band`; `TitleBar.qml`'s `bandTitle` | the same | Matches. |
| `ToolbarButton.qml` (Step 2 adds an `extent` property) | `glyph`, `accessibleName`, `clicked()`, `secondaryClicked()`; its size is `size.toolbarButton` | Matches, if the root is not an `AbstractButton` (**F11**). |
| `win.hostLabel`, `chipOpenGlyph`/`chipCloseGlyph` | the glyph tokens; `hostLabel` predates 1c | Matches. |
| snapshot rows naming `.drift/.frame/.mode`; `HostConnection {Local, Connected, Reconnecting, Disconnected}` | the same, plus `.decor` | Matches. Step 2 Task 12's dependency note omits `.decor`, which costs nothing. |
| `motion.crossFade` (1c R-j) | `motion.style/full/crossFade` ints | Matches. |

Step 1d (its plan, and S1–S4):

| 1d expects | 1c's plan gives | Status |
|---|---|---|
| `window::WindowFrame`, `window::WindowMode` in `TitleBandLayout.hpp` | `ClientBand=0, NativeFrame`; `Windowed=0, FullScreen` | Matches. |
| "the band's height … `<1c's token binding>`"; `titleBandTokens().height` = "1c's band token" (`size.titleBar` on native rows, one cell on Terminal) | Pill reads `size.titleBar` (38). Flat keeps `chromeHeight` (34), while `size.titleBar` is 40/46 on Fluent/Linux, "recorded but unread". TitleBar's `implicitHeight` change is never stated | **Mismatch (F5)**. If 1d reads `size.titleBar` on Fluent/Linux, the bar grows to 40/46 and breaks 1c's acceptance test. |
| `TitleBar.qml` `presence`/`centre`/`revealed`/`slideOffset`; Main.qml's `band`, `bandRevealEdge`, `tabsBar`; `chromeHeight` excludes the revealed band | the same | Matches. 1d's revealed `y = titleBandRevealTop` composes with 1c's slide if the slide is an offset from the resting `y`: write `y: restingTop + slideOffset`, not `y: slideOffset`. |
| the band fill left as `systemPalette.window` (1c R-h) | the same (`TitleBar.qml:60-63`; 1d cites `:58-61`) | Matches. |
| `chromeStyle.snapToCells(nativeControlsInset)`, and Task 7's Terminal-row inset case | the same | Matches. S2's 84-pt inset comes from the controller, and the mock's 70 is arbitrary. |
| 1c `bindWindow`'s `visibilityChanged` → `markVisibleSessionsSeen` sits beside 1d Task 6's hooks | the same | **Changes under F2**: the seen trigger moves to `onWindowStateChanged` → the manager's `windowStateChanged`, which 1d Task 6 also edits. The edits are additive; tell 1d. |

## Findings needing a ruling

**Blocking** (a task's own steps, the build or CI fail as written): **F1–F4**. **Non-blocking:** **F5–F20**.

1. **F1 (blocking): there is no output hook for the sessions the dot is for (R-b, Task 5).**
   - **Plan text:** "output arrives through the post that `TerminalSession::screenUpdated` already makes (`TerminalSession.cpp:893-898`)". Also the Risk line: "The attention ledger is fed from the parser thread's existing post."
   - **Why it fails:**
     - `screenUpdated` (`:852-901`) makes no such post. Its only post is the find bar's tally timer, and only while the bar is open (`:871-891`).
     - It also returns at once when the session has no display (`:855-856`). A background tab's panes are unloaded (Main.qml renders only `activeTabRootPane`), so its session has none.
     - The sessions in the `[manager][tabbadge]` TestApp cases have no display either. Every "shows the dot" section, and the resync sequence, read `None`.
   - **Proposed:**
     - Add a coalesced post in `screenUpdated`, *before* the `_display` check, guarded by an atomic flag. This copies `programStatusChanged`'s pattern (`:2180-2195`, `_isProgramStatusRefreshPending`, `.hpp:2028`).
     - The posted lambda clears the flag and calls `_manager->noteSessionOutput(modelSessionId(), origin)`. The origin and `ResyncGrace` are read on the GUI thread.
     - Split the manager API in two:
       - `noteSessionOutput(SessionId, OutputOrigin)` is the output path. It emits the three badge roles only when `noteOutput` returns `Changed`.
       - `refreshTabBadgeForSession(SessionId)` (the renamed `refreshTabProgramStatusForSession`) is the OSC 7501 path, with no origin. An OSC 7501 report must not mark "new output".
     - Tell Step 2 (its Task 4 and F12) the two names.
     - Performance: one queued call per GUI-loop turn per busy session. Optionally skip the post while the session is already marked (a session-side atomic, cleared on `noteSeen`). Correct the Risk line.

2. **F2 (blocking): the restore, and the unzoom, never mark a session seen (Task 5).**
   - **Plan text:** "`bindWindow`, where the QWindow's `visibilityChanged` now calls the manager's `markVisibleSessionsSeen`". The test: "with the window minimised, output on the active tab gives `Output`. Restoring the window (`setWindowStates(Qt::WindowNoState)`) gives `None`."
   - **Why it fails:**
     - `ScopedControllerWithOsWindow`'s window is "Never shown" (`GuiTestFixtures.hpp:858-861`). On an unshown `QWindow`, `setWindowStates` emits `windowStateChanged` but no `visibilityChanged`, because visibility stays `Hidden`. The "Restoring … gives `None`" check fails.
     - Visibility is also derived from `windowStates()` (`WindowController.cpp:1660-1663`), not from `visibility`.
     - A sibling covered by a zoomed pane is `Hidden` (`TerminalSessionManager.cpp:475-477`), so its output marks the active tab. Unzooming fires `paneZoomChanged`, not `activeTabChanged`, so the dot stays until the user leaves the tab and comes back.
   - **Proposed:**
     - Call `markVisibleSessionsSeen(window)` from the existing seam `TerminalSessionManager::windowStateChanged` (`:428`, reached from `WindowController::onWindowStateChanged`, `:1248`), and from `paneZoomChanged`, beside `activeTabChanged`.
     - Have the test `bindWindow(window.osWindow.get())` first, as the minimise section does (`TerminalSessionManager_test.cpp:855-858`).
     - Add a section: "unzooming clears a covered sibling's dot".

3. **F3 (blocking): the snapshot structure case pins "a one-tab strip hides" on the macOS row (Task 7).**
   - **Plan text:** Task 7 "makes the band always shown on `Pill` `Top`" (R-e, D1). It does not list `ChromeSnapshot_test.cpp`'s existing case.
   - **Why it fails:** `ChromeSnapshot_test.cpp:79` checks `titleBar->isVisible() == isTabBarShown(look.tabBarVisibility, row.tabCount)` for every script on MacOS and Terminal (`:61-66`). Native2026 is `Multiple` (`Config.hpp:1120`). Every one-tab script on MacOS under Native2026 now has a visible band, so `[contour][gui][qml][snapshot]`, which is in Task 7's own filter, fails.
   - **Proposed:** expect `band != Hidden`, where `band` is `resolveTitleBand({ .shape = ChromeTokenRows[row].layout.tabShape, .strip = isTabBarShown(…) ? Shown : Hidden, … })`. The pure call then decides, and the comment "Native2026 hides a one-tab strip" becomes "… except on the macOS band (1c D1)".

4. **F4 (blocking): Task 6 removes the macOS `+` a task before its replacement exists.**
   - **Plan text:** Task 6: "the new-tab buttons … are gated. The gates are `Flat`-only for the buttons". Its own case: "twenty tabs … `newTabButton` (Task 7's band button, or the Flat one) is visible" on the MacOS row.
   - **Why it fails:**
     - After Task 6, the MacOS row has no visible `newTabButton`: Task 7's `ToolbarButton` does not exist yet. The twenty-tabs case fails.
     - For one commit, the macOS window has no way to open a tab by pointer.
   - **Proposed:** Task 6 leaves the `+`/`▾` on every shape. Task 7 gates them in the same commit that adds the band's `ToolbarButton`, and relocates the menu (**F6**).

5. **F5: the band's height has no single source, and 1d reads the wrong token on Flat rows (Tasks 4 and 7, and 1d).**
   - **Plan text:** the token table records `size.titleBar` as 40/46 on Fluent/Linux, "unread by the `Flat` shape". Task 7 asserts `titleBar.height == 38` on MacOS but never changes `TitleBar.qml:47` (`implicitHeight: chromeStyle.chromeHeight`, which is 34 on every native row).
   - **Why:**
     - The MacOS row keeps `chromeHeightUnits == 34`, pinned by `UiStyle_test.cpp:210`.
     - 1d binds `titleBar.height` to `win.titleBandHeight || <1c's token binding>`. Its `titleBandTokens().height` is "1c's `size.titleBar` on native rows", which gives Fluent 40 and Linux 46 and breaks 1c's acceptance test.
   - **Proposed:**
     - Task 4 adds `Q_PROPERTY(qreal bandHeight)`: `size.titleBar` under `Pill`, else `chromeHeight`.
     - Task 7 binds `TitleBar.implicitHeight` to it.
     - Tell 1d that `bandHeight` is the band token.

6. **F6: on macOS the new-tab menu has no owner or anchor (Task 7, D1 + D5).**
   - **Plan text:** "on the MacOS row, `newTabMenuButton` is absent; a right-click on `newTabButton` makes the mock record `openNewTabMenu`".
   - **Why:**
     - `newTabMenu` is a child of `newTabMenuButton`, and `onNewTabMenuRequested` pops it at that button (`TabStrip.qml:253-258`, `:287-289`). With the button absent, the request has no menu or raises a ReferenceError, which the suite's QML gate turns into a failure.
     - A one-tab macOS band (centre `title`) has no TabStrip at all, so a right-click on `+` opens nothing. That is the default macOS case.
     - The mock's `openNewTabMenu` records nothing (`MainWindowControllerMock.hpp:134-140`).
   - **Proposed:**
     - Move `newTabMenu` and its handler into `TitleBar`, and pop it at the button that asked (`newTabButton` on Pill, `newTabMenuButton` on Flat).
     - Have the mock record `calls << "openNewTabMenu"`.
     - Test the one-tab macOS case too.

7. **F7: the drift chip has no home without a bar, and its dialog centres on the chip (Task 7, R-m).**
   - **Why:**
     - On Fluent and Linux, a one-tab window under Native2026's `Multiple` shows no bar. The drift chip, always visible over the content today, would vanish. The same happens under `show_title_bar` with the gate hidden.
     - `KitDialog` centres on its `parent` (`KitDialog.qml:37-38`), which here is the 22-pt chip.
   - **Ruling needed. Default:**
     - While the bar is hidden, the chip stays at today's content corner, parented by state, so it never disappears. The alternative is that a visible drift forces the bar.
     - The dialog takes `parent: Overlay.overlay`.
   - **Cost if wrong:** one placement row.

8. **F8: three Task 6 tests depend on Task 7 or on a shown window.**
   - **The binding loop:** "`availableWidth` … Until then it is bound to the strip's own width." TabStrip is a `Row` sized by its delegates, whose width comes from `wellLayout` of that same width. That is a binding loop, a QML warning, and so a gate failure.
     - Bind it in Task 6 to the TitleBar's width less the reserve and the leading slot.
   - **The drag reserve:** "`dragRegion.width ≥ chromeStyle.space.dragReserve`" is asserted in Task 6, but `Layout.minimumWidth: dragReserve` is Task 7's.
     - Move the clause to Task 7, or add the minimum in Task 6.
   - **A shown window:** Main.qml loads with `visible: false`, and the mock's `showInitial` maps nothing. The hover-close and right-click cases (`QTest::mouseMove`, `mouseClick`) therefore need `root->show()` and `QTest::qWaitForWindowExposed`. The full-screen case shows the window already.

9. **F9: mock gaps, and tests that assert nothing (Tasks 6 and 7).**
   - **`tabBackgroundColor`:** `MockMainController` has no `tabBackgroundColor`/`tabTextColor`, which TabItem calls for a coloured tab (`TabItem.qml:108-121`). `TabDecor::EveryBadge`'s coloured row 0 then raises a TypeError.
     - Add both to the mock, through `window/TabColorScheme`.
   - **"show_title_bar":** the mock exposes no `needsClientFrameAffordances`/`nativeControlsInset`, so `leadingWindowControls.width == 0` and "no window controls" hold before and after `setTitleBarVisible(true)`.
     - Give the mock both, following `titleBarVisible` as the frame policy does.
   - **The band title:** "`bandTitle` … the active session's title". The mock's `activeSession` is null (`MockMainController.hpp:161`).
     - Assert the active row's `TitleRole` ("Tab 1"), as Task 7 Step 3 says.
   - **The Terminal row's contrast pair (needs a ruling):** "the active label's colour on `colors.tabActiveFill` ≥ 4.5:1" measures a pair Flat does not draw. Flat's uncoloured active tab is `systemPalette.highlight`/`highlightedText` (`TabItem.qml:111-122`).
     - Default: the test measures what is drawn (`effectiveBackground` and `foreground`).
     - R-i's `SelectionText` then applies only if the Terminal row's Flat strip reads `colors.tabActiveFill/tabTextActive/tabText`. Make that data, for example `layout.tabColors ∈ { Palette = 0, Roles }`: `Roles` on Terminal and Pill, `Palette` on native Flat.
     - Cost if wrong: one layout token.

10. **F10: the flyout's literal-to-token mapping is not stated, and it is lossy (Task 6).**
    - **Plan text:** "its 12 size literals become tokens (`radii.popover`, `size.swatchWidth`/`swatchHeight`, `radii.control`, `colors.swatchRing`, `space.s4`)".
    - **Why:**
      - The 12 are padding/spacing 8 (×3), radius 9/5/5, border 2/1/1, `duration: 80`, and swatches of 30×30.
      - `size.swatchWidth/Height` is 16×16 on native rows (the menu swatch), so taking it would visibly halve the flyout's swatches on every row.
      - `border.width: 2`, the ring radius 9 and `80` have no token.
    - **Proposed:** the mapping, stated in the plan:
      - spacing/padding → `space.s4`; radius 5 → `radii.control`; border 1 → `chromeStyle.borderWidth`; `80` → `motion.fast`;
      - add `size.flyoutSwatch` (30/30/30/2) rather than reuse the menu swatch;
      - accept a non-zero remaining baseline (the 2-px ring) rather than promise 0.

11. **F11: QML names that clash, and duplicates from the second bar (Task 7).**
    - **`HostChip.state`:** `required property int state` shadows `Item.state`, the `QString` that drives States. Rename it `connection`; the a11y kit case then passes it by that name.
    - **`ToolbarButton.clicked`:** "the signals `clicked()` and `secondaryClicked()`" duplicate `AbstractButton.clicked` if ToolbarButton's root is a button, which is a load error. Give it an `Item` root with `TapHandler`s (`Accessible.Button`, `Accessible.onPressAction`), or declare only `secondaryClicked`.
    - **The `tabsBar`:** it instantiates a second TitleBar. It must hold no HostChip, drift chip, toolbar or window controls, or it duplicates `hostChip`, `daemonDriftBadge` and `newTabButton`, and `findChild` picks the wrong one. Make its slots `Loader`s off a `role: "tabs"` property.
    - **`titleBand(int, int, int, int)`:** four adjacent, silently swappable ints. This is the documented QML-facing carve-out; note it at the declaration, and keep Main.qml the only caller.

12. **F12: `Interrupted → None` disagrees with the codebase's one classification (Task 1).**
    - **Plan text:** "`Unknown` and `Interrupted` → `None` (no verdict, or the user's own ⌃C)".
    - **Why:** `vtbackend::isFailure` (`CommandBlock.hpp:311-323`), "the one classification the gutter's colour and the finish notification both read", counts `Interrupted` as a failure. A ⌃C'd command would show a red gutter and a failure notice, but a clean tab.
    - **Ruling needed. Default:** derive `finishedBlockBadgeOf` from `isFailure`: `Failed` when it says so, `Succeeded` for `Succeeded`, else `None`. Then the tab cannot disagree with the gutter. The alternative keeps the plan's row and documents the divergence at the table.
    - **Cost if wrong:** one row.

13. **F13: row tables over another module's enum are pinned to its last enumerator (Tasks 1 and 2).**
    - **Plan text:** "`HostChipRows.size()` equals `std::to_underlying(ConnectionState::Stopped) + 1`"; "`static_assert` that the rows … are in enumerator order"; `FinishedBlockRows`, "one row per `CommandEnding` enumerator".
    - **Why:** a seventh `ConnectionState`, or a new `CommandEnding`, passes the order assert and the tests, which iterate the table's own size. `hostChipStateOf` would then index past the table. This is 4a's F2 and 1b's F2 again.
    - **Proposed:** add `ConnectionStateCount` beside the enum (`vthost/client/ConnectionState.hpp`, first-party) and `CommandEndingCount` beside `CommandEnding`, each in its one place, as 4a did with `HostPlatformCount`. Pin the tables to them.

14. **F14: the name `SessionPlacement` is taken (Task 5).**
    - **Plan text:** "`enum class SessionPlacement : uint8_t { OnScreen = 0, OffScreen };`".
    - **Why:** `platform::SessionPlacement` already exists, a struct (`platform/SessionVisibility.hpp:49-57`), and `TerminalSessionManager.cpp:479` builds one. The manager's `.cpp`, where both meet, then reads ambiguously.
    - **Proposed:** take `platform::SessionVisibility` directly. The header is std-only and Qt-free, so `TabAttention.hpp` may include it, and `Hidden` is the mark condition. This saves an enum and a conversion. Otherwise rename the enum `AttentionPlacement`.

15. **F15: the resync grace covers one output burst; a resync is several (Task 5, Review Focus 3).**
    - **Plan text:** "`ResyncGrace { None = 0, NextOutput }` … armed when the host connection returns to `Connected`". The test: "write output again … `Output`".
    - **Why:** a mirror's resync repaints every pane in as many chunks as the stream has. Under F1's coalescing, only the first drained burst is excused. A slow resync still dots every background tab, and the test passes only because it writes a single burst.
    - **Ruling needed. Default:** a time-based grace through the injected steady clock (`_app.terminalClocks().steady`, as `recordProgress` uses). Output within, say, 500 ms of the return to `Connected` is `Resync`. Use a fake clock in the test.
    - **Cost if wrong:** a stray dot after a reconnect.

16. **F16: the label fallback is uncached, unrefreshed and untested end to end (Task 5, R-o).**
    - **Why:**
      - `resolvedTabLabel` runs on every `TitleRole` read. For an empty label, each read makes a `Process::foregroundProcessName()` call (proc/sysctl), and the strip reads that role often.
      - Nothing refreshes the role when the foreground process changes.
      - `MockPty` is not a `vtpty::Process`, so only the pure `tabLabelWithFallback` is tested.
    - **Proposed:**
      - Cache the name per session, refreshed only on the triggers that already refresh `TitleRole`.
      - Record the staleness, and the controller-level test, as Step 2 Task 5's (its `Pty::foregroundProcessName()` and poll). Do not move that `Pty` work into 1c: Step 2 owns it in the parallel worktree.

17. **F17: workflow items (Tasks 1–8).**
    - **clang-tidy:** no step runs it. Add the 4a ruling's invocation to every Step 5 that touches C++, on the new and changed files: `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path)`.
    - **The `/simplify` base:** "`git diff <the plan commit>..HEAD -- src scripts`". The plan commit 8a8aa716 predates all of 4a's src commits. Base it on Task 1's parent.
    - **Staging:** `git add src/contour/config src/contour/window` (Task 4) and `src/contour/qml src/contour/test` (Tasks 5–7) sweep up anything else in the tree. Stage the listed files after `git status`.
    - **Builds:** the "every core" ruling rested on "the only job on the machine". Step 2 now builds in `contour-step2`, so coordinate the two builds, or give each half the cores.
    - **The BASE:** 1c starts on 4a Task 5's Finish commit.

18. **F18: Reduce Motion and palette re-evaluation gaps (Tasks 4–7).**
    - **The reorder:** it still slides `x` over `motion.standard` (100 ms) under `CrossFade`. The kit says every motion becomes a cross-fade. Run `moveDisplaced` only under `motion.full`.
    - **Palette changes:** `statusInk`, `badgeLook` and `hostChipLook` are `Q_INVOKABLE`s. A QML binding over them does not re-evaluate on a light/dark switch or an accent change, and Running is the accent.
      - Make the bindings depend on `chromeStyle.paletteRevision`, as `KitDialog.qml:116-118` does.
      - Add a case: "a palette change recolours the badge".

19. **F19: wording, baselines and small drift.**
    - **Baselines:** Task 7's Step 4 should name `Main.qml` 1 → 0 (the gate fails on a lower count too) and `TabStrip.qml` → 0. Task 5's "the two durations" is three literals.
    - **Task 2:** Step 5 runs no `editorconfig-checker` on `CMakeLists.txt`.
    - **`errorColor`:** it changes on the Terminal row via `progressColor` (Task 4). Say so, or keep `errorColor` on `statusColor(Danger)`.
    - **D1's text:** it says "Native2026 macOS windows … grow by 38 pt", but `TabShape` is per row. Classic on macOS gets the pill band too, and the snapshots render both editions. State it.
    - **The menu-bar check:** Task 8 Step 4's "note where it sits relative to the system menu bar" is answered by the spike (29 pt). Drop it, or keep it only as a visual check.
    - **The plans index:** its Plan column is shifted one row from 1d down. Fix it at Task 8 Step 6.
    - **Line numbers:** take the current ones from §3.

20. **F20: settle `TitleBandRules`' key for Step 2 now (Task 3).**
    - **Plan text:** "matched on `(shape-or-any, frame, position)`".
    - **Why:**
      - Step 2's R-f and Task 7 add a row "ahead of 1c's `Pill`/hidden-gate rows", which needs the gate as a key column.
      - Step 2's scan F16 proposes a post-step instead.
      - Either works. Deciding late means rewriting 1c's table under Step 2.
    - **Ruling needed. Default:** key the rows on `(shape-or-any, frame, gate-or-any, position)`. R-e's Pill/Top rule becomes two rows, Shown → `Tabs` and Hidden → `Title`. Step 2 then adds one row with a density column, and no rule logic is hidden in code. Record the choice for Step 2's Task 7.
    - **Cost if wrong:** one table column.
