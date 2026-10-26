# Pre-flight scan — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-2.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.6 (`:469-519`, binds), with what
it cites (§1, §2.1 `:68-126`, §2.3, §2.6, §2.7, §3.16 row 6.3 `:820`, §4 `:835`). Scanned against HEAD 00ba0eed44ce6781907d7f0b3367b137bf013cc8 on
`feature/native-redesign-phase0`. The plan's numbers are stated at d8053e4f. Between that commit and HEAD sit
41b99e0b (4a Task 2) and the plan commit itself, and neither moves a line the plan cites in session/, config/ or
vtworkspace/. Uncommitted 4a Task 3 edits in ContourGuiApp, WindowController, TerminalSessionManager,
TerminalSession(_test), Config(.cpp/_test), Actions and the menu fixtures were set aside: every line number below is
taken from `git show HEAD:<file>`.

The worktree `/Users/christianparpart/projects/contour-step2` exists (branch `work/native-redesign-step-2` at
41b99e0b). It has its own configured tree, `out/appleclang-debug` (20 GB, `CONTOUR_EXTENSIONS=ON`,
`CONTOUR_EXTENSIONS_BUNDLED=OFF`, `compile_commands.json` present). The disk has **11 GiB free (99 % used)**.

Owner decisions D1–D5 and the writer's rulings in `progress.md` are settled and not reopened. The canvas values in
the plan's token table were checked against today's rows: `paneHeader` 22/28/28/1, `paneGap` 6/4/6/1, `splitHit`
10/10/10/1, `paneInset` 6/4/6/0 and `paneCard` 10/4/6/0 all match `ChromeTokenGroups.hpp:320-431`.

## Pre-flight scan

### 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → 2 | `config::ChromeDensity`, `detail::ChromeDensityTable`, `configEnumValues<ChromeDensity>()` | `ConfigEntry<ChromeDensity, …>`, the `EditionDefaults` column, `globalEnumFieldFromTable<ChromeDensity>` | Consistent. The `ConfigEnumConcept` reader, writer and formatter (`ConfigEnumInfo.hpp:72`, `:114`; `Config.hpp:2097`) give load, YAML write-back and the Settings options with no extra code. **F17**: `chromeDensityName` re-spells the table. |
| 1 → 3 | `PaneActionsReveal`, `resolvePaneHeader`, `paneHeaderName`, the name functions | `LayoutTokens::paneActions`; `UiStyleProvider::paneHeader/paneHeaderName` | Consistent. `PaneActionsReveal` sits in `config/` so that the rows can name it. |
| 1 → 5 | `resolvePaneHeader` (rule 3 reads `headerHeight + minimumContentHeight`) | `refreshHeaderFollowers` with `.height = <large>` and both left 0 | Consistent: with both at 0, rule 3 never fires. |
| 1 → 9 | `HeldModifierReveal`, `PaneHeaderAction`, `PaneHeaderActions` | The controller's filter and timer; the header buttons' `accessibleName` "via a provider list" | **F8**: no task creates that provider list, and Task 9 does not touch `UiStyleProvider`. **F11**: auto-repeat. |
| 2 → 3 | `Config::chromeDensity` | `ContourGuiApp::chromeInputs()` (`ContourGuiApp.cpp:1796-1806`) | Consistent. `onConfigurationReloaded` (`:1884`) → `refreshChromeStyle` (`:1808-1814`) → `ChromeStyleHost::update`, so a reload rebuilds the provider. |
| 2 → 5 | `Config::chromeDensity` | `_manager.app().config().chromeDensity.value()` | Consistent. |
| 3 → 6 | `space.paneGapRows`, the Minimal sub-row, the density parameter of `resolveChromeTokens` | `splitChromeOf`; `ContourGuiApp::splitChrome()` "from the published provider's resolved tokens" | **F15**: the provider publishes no `ChromeTokenRow` (`UiStyleProvider` holds `UiStyleTokens` and group objects), so an accessor is needed. 1d's `titleBandTokens()` needs the same one. **F17**: the sub-row has no `paneGapRows` field. |
| 3 → 7 | `size.bareBand`, `BandFill::Background`, `ChromeInputs::density` | `resolveTitleBandSurface`, `bandFill`, `refreshTitleBand` | **F4**, **F16**. |
| 3 → 8 | The gap, hit, inset and grabber tokens, `paneGapFill/paneSplitLine/paneGrabber`, `splitLineGlyphs`, `density` | `PaneNode.qml`'s handle | Consistent in names. **F9**: the grabber's "radius half its thickness" is a size literal. |
| 3 → 9 | `paneHeader()`, `colorOn()`, `hairline()`, `unfocusedDim()`, `type.paneHeader*`, `tui.inactivePaneOpacity` (exists, `ChromeTokenObjects.hpp:360`) | `PaneCard.qml`, `PaneHeader.qml`, `TerminalPane.qml` | **F8**: QML cannot name a `ChromeColorRole` int for `colorOn(role, …)`. **F7**: `colorOn` and `unfocusedDim` read the palette, and nothing notifies their callers when it changes. |
| 3 → 11 | `paneDimColor` (re-tokenized); `paneDimTextColor` "removed" (Task 3 Interfaces) | `TerminalPane.qml:220` reads `paneDimTextColor` until Task 11 removes the overlay | **F2** (blocking). |
| 4 → 5 | `MockSession.foregroundProcessName` (the mock, in Task 4) | `TerminalSession::foregroundProcessName` Q_PROPERTY (Task 5) | Consistent: the mock leads, and QML first reads it in Task 9. |
| 4 → 9 | `PaneProxy.focus`, `WindowController.visiblePaneCount`, the session's cwd and status properties | `PaneCard.node.focus`, `win.visiblePaneCount`, `PaneHeader` | **F12**: `focus` is notified only through `activeChanged`. |
| 5 → 9 | `refreshHeaderFollowers()`, `_headerReveal` | The reveal drives `_headerReveal` and refreshes | Consistent. |
| 6 ↔ 8 | The solver's token gap, header and inset (Task 6) | The rendered handle and inset (Task 8) | Consistent. Between the two tasks the solver overshoots, which its contract allows. Task 6 removes the solver's use of `DefaultSplitHandleThickness` (`WindowController.cpp:1835-1839`) before Task 8 removes `splitHandleThickness`. |
| 8 → 9 | `PaneNode.qml` (handle, root inset) | `PaneNode.qml` (the leaf `Loader` `:55-64` → `PaneCard`; `inSplit` `:41-43` → `visiblePanes`) | Disjoint parts. Task 9 must also rewrite the two `onLoaded` assignments (`item.inSplit = true` at `:124`, `:156`) so that `visiblePanes` and `headerReveal` reach nested nodes. The plan only implies it. |
| 9 → 11 | `PaneCard.qml` | Task 11 adds the footer and the reconnecting layer to `PaneCard.qml` and removes `TerminalPane.qml`'s overlay (`:205-223`) | Consistent. The object name `reconnectingOverlay` is kept, so `ChromeSnapshot_test.cpp:103-108` passes. |
| 10 → 11 | `sessionEnd`, `endMessage`, `restartOffer`, `restart()`, `dismiss()` | `ExitedFooter.qml`; `MockSession` mirrors | Consistent: `restartOffer === 1` is `FooterRestart::Offered`, and `sessionEnd !== 0` is `SessionEnd::None`. |
| 10 (alone) | `restartSession` → `createBackingSession` + `replaceLeafSession` | The manager's registries | **F5** (blocking). |
| 4, 9, 10, 11 → 12 | Mock properties, the hover, the end | `ChromeSnapshotScene` builds `density`, the session end and `HoverPaneHeader` | Consistent. Task 12 depends on every earlier task. |
| all → 13 | Each task's filter | Step 2's union | Covers every tag the tasks create. `[windowmenus]` matches the `windowmenus` tag of `[contour][windowmenus]` cases. **F21**: the `/simplify` base. |

### 2. Per-task self-consistency

| Task | Agrees with itself? | Detail |
|---|---|---|
| 1 | Yes | Three headers and two tests are created, listed for CMake, formatted and staged. The filters `[paneheader]` and `[heldmodifier]` are created. The test `base` values (22 + 17, so 38.9 → `{}` and 39 → shown) agree with rule 3. The placement "beside 1c's `window/TabBadge_test.cpp`" names a file that will not exist if 1c Task 1 has not landed (**F1**). Put the files at their own anchor instead (see Part 4). **F17**. |
| 2 | Mostly | The entry, the column, the load (`Config.cpp:1601`), the override row (`GlobalOverrides.cpp:63`), the documentation (`ConfigDocumentation.hpp:306-318`, `:2477`, `:3404`) and the Settings row (`SettingsController.cpp:1653-1658`) are all present. The round trip already pins `window_control_style` the same way (`Config_test.cpp:2860-2895`). **F6**: the Config_test cases run in a binary that the task's filter does not run. The doc text "a bare 28 pt band" depends on **F4**. |
| 3 | No | **F2** (blocking): `UiStyleProvider_test.cpp:348-349` pins the 60 % dim (alpha 153) and `:373` pins `paneDimTextColor`, and the plan edits neither. `TerminalPane.qml:220` still reads the removed property. **F7**, **F8**, **F17**. Every listed file exists. `numbersOf` is `ChromeTokenGroups_test.cpp:46-77`. 1c Task 4 owns the factories this task consumes (`fill`, `label`, `schemeOnBar`, `level`), and 1d Task 2 owns `toneOf` and `BandFill`, so this task cannot compile before both land (**F1**). |
| 4 | Mostly | The two terminal events are `Terminal.hpp:595` (`contextChanged`) and `:602` (`workingDirectoryChanged`), raised on the parser thread under the lock. The plan posts through `_postTarget`, which is correct. The program status arrives on the GUI thread already (`TerminalSession.cpp:2181-2195` posts to `refreshTabProgramStatusForSession`, `TerminalSessionManager.cpp:1521`). The tests sit in the heavy `TerminalSessionManager_test.cpp`. **F12**, **F13**. The manual check "the cwd follows `cd`" assumes OSC 7 (**F23**). |
| 5 | Mostly | `Process::foregroundProcessName()` is `Process.hpp:160`, and `Pty` is `Pty.hpp:100-162`. Five other `Pty` subclasses exist (`UnixPty`, `ConPty`, `ChannelPty`, `SshSession`, `MockViewPty`) plus vthost's `TappingPty`, and all of them take the `nullopt` default. **F10**: the poll lives under `#ifdef CONTOUR_EXTENSIONS` (`TerminalSession.cpp:1078-1084`, `:1162-1261`); `ProcessNameFollower` has no zero; the remote case asserts nothing. `vtpty_test` (`src/vtpty/CMakeLists.txt:153`) gains `MockPty_test.cpp`, which is staged. |
| 6 | Mostly | I checked the test arithmetic: `{6,6,22,6}` and `{400,300}` give `{812, 334}`, and the second leaf gives `{824, 334}`. `SplitChrome_test`'s Terminal row `{8,17,17,0}` follows from `splitChromeOf`. `WindowSizing_test.cpp` pins no split solver (its cases are about chrome height, hints, DPR settlement and positions), and `TabSizeInheritance_test.cpp` pins spawn sizes only, so neither changes. **F15**: the accessor, and the display case that the task never runs. **F9**: the "#2040" case. |
| 7 | No | **F4** (blocking, needs a ruling). **F16**: `TitleBandRules` does not match on the gate; `bandFill(frame, mode)` takes no centre; `refreshTitleBand` would have to recompute the band per window. The task depends on 1c Tasks 3, 4 and 7 and 1d Tasks 2, 6 and 7 (**F1**). |
| 8 | Mostly | `PaneNode.qml:99-117` (the handle), `TerminalSessionManager.hpp:126` and `:158-164`, `MockMainController.hpp:63` and `:154-157`, `QmlComponents_test.cpp:102-120`, `KeyboardRouting_test.cpp:210` and `:248-251` are all present. Nothing else reads `splitHandleThickness` (grep). `SessionChrome.qml:137` is `verticalScrollBar`. **F9**: a size literal, a baseline that cannot go down, and a test that does not exist. |
| 9 | Mostly | The focus-frame cases are `QmlComponents_test.cpp:3456` and `:6719-6760`; `focusFrame` is read nowhere else. `TerminalPane.qml:41-42` and `:225-235` are present. The test file choice ("MultiWindow_test or TerminalSessionManager_test") is staged as the latter, and both are heavy. **F8**, **F11**, **F19**, **F20**. `ToolbarButton.qml` is 1c Task 7's (**F1**). |
| 10 | No | **F3** (blocking): two `[contour][session][start]` cases break. **F5** (blocking): the registries. The plan's "pre-flight check" has an answer: `removeSession` (`TerminalSessionManager.cpp:1159-1214`) tolerates a session that no leaf carries. `findLeaf(old)` returns null, so `closePane` is skipped, and the registries are erased and `deleteLater` runs. No factoring is needed. `Pane.hpp` has no `swapSession`: the neighbour is `swapLeafPayload` (`:109-115`) (**F22**). `SessionModel.hpp:198` and `ModelEvents.hpp:129` are present. `onClosed()` is always posted to the GUI thread (`TerminalSession.cpp:250`, `TerminalDisplay.cpp:841`), so `sessionEndChanged` needs no queued hop. **F13**, **F14**. |
| 11 | Mostly | `win.hostLabel` already exists (`WindowController.hpp:132`, `.cpp:337`): it is not 1c's (**F22**). `command::chordIndex` and `platformShortcutStyle()` are used today at `WindowController.cpp:226`. **F14**: `ClosePane` defaults to Ctrl+Shift+X (`Config.hpp:1498-1501`), not ⌘W. The Step 4 claim "`TerminalPane.qml`'s baselines go down" is wrong for this task: the overlay holds no size literal. The drop from 3 to 2 is Task 9's `border.width: 1`. |
| 12 | Yes | The new fields have zero defaults, and the counts are derived. `ChromeSnapshot_test.cpp:103-108` is the `RemoteReconnecting` check (a `SingleLeaf` row, which `PaneCard` still wraps). The enums and the row are at `ChromeSnapshotPlan.hpp:35-49`, `:66-74` and `:97-108`, not `:28-42`, `:57-65` and `:86-98` (**F22**). |
| 13 | Mostly | **F21**: after the rebase, the `/simplify` base `<plan commit>..HEAD` would include 1c's and 1d's commits. The snapshot count is derived. Metainfo `0.9.0` is right. |

### 3. References verified against HEAD

Present and correct:
- Spec: §3.6 `:469-519`; §2.1 `:68-126` (contrast overrides `:120-126`); §2.3 `:139`; §2.6 `:206`; §2.7 `:228`.
  The cites `:821` and `:836` are `:820` and `:835` (off by one).
- `vtworkspace/PaneLayout.hpp:37` (`DefaultSplitHandleThickness`) and `:47-100` (`contentSizeForLeaf`).
  Callers: `WindowController.cpp:1835` (the plan says `:1830`) and `PaneLayout_test.cpp:56-293`.
- `WindowController.cpp:1826-1839` (the solver site); `WindowController.hpp:111`, `:449` and `:711`; `:116`
  (`needsFramelessHint`); `:132` (`hostLabel`); `:189` (`eventFilter`).
- `WindowController.cpp:1202-1258` (`bindWindow` already installs the event filter at `osWindow->installEventFilter(this)`);
  `:1435-1489` (`eventFilter`).
- `TerminalPane.qml`: `:41-42`, `:184-189` (`dimOverlayColor`), `:194-203` (`dimOverlay`), `:205-223`
  (`reconnectingOverlay`), `:213` and `:220` (the two `paneDim*` readers), `:225-235` (`focusFrame`).
- `PaneNode.qml`: `:41-43`, `:55-64`, `:99-117`, `:124`, `:156`.
- `TerminalSession.cpp`:
  - `:342-353` (`ExtensionState`'s timer and name);
  - `:709-728` (`reportDeviceStartFailure`);
  - `:762-795` (`terminate`);
  - `:1078-1084`, `:1162-1177`, `:1188-1204`, `:1247-1261` (the poll, all under `#ifdef CONTOUR_EXTENSIONS`);
  - `:1686-1785` (`onClosed`; transport lost `:1720-1734`; early exit `:1737-1750`);
  - `:2181-2195`, `:2831-2849`, `:2886-2910`, `:5776-5800`.
- `TerminalSession.hpp`: `:200-252`, `:208` and `:213`, `:250`, `:634`, `:641`, `:738`, `:759`, `:786`, `:954`, `:1303`,
  `:1822`, `:1843`.
- `vtbackend/screen/Terminal.hpp:595` and `:602`; `vtbackend/core/WorkingDirectory.hpp:109`.
- `TerminalSessionManager.cpp`: `:97-103` (the allocator), `:280-339` (`createBackingSession`), `:1159-1214`,
  `:1307`, `:1521`, `:1852-1881`. `.hpp`: `:126`, `:155`, `:158-164`, `:462`.
- `PaneProxy.hpp:25-103`; `Pane.hpp:109-115`; `SessionModel.hpp:198`; `ModelEvents.hpp:129`.
- `vtpty/Pty.hpp:100-162`, `Process.hpp:160`, `MockPty.hpp:24-79`, `vtpty/CMakeLists.txt:153-193`.
- `ChromeTokenGroups.hpp`: `:114-131` (`SizeTokens`), `:134-142` (`SpaceTokens`, the `paneInset` comment at `:138`),
  `:208-213` (`ContrastTokens`), `:257-263` (`nativeSpace`), `:329`, `:457-487` (`resolveChromeTokens`).
- `UiStyle.hpp:270` (`PaneDimPercent`); `UiStyleProvider.hpp:88-97` (`ChromeInputs`), `:143-144` (both `CONSTANT`);
  `UiStyleProvider.cpp:282-290`.
- `ChromeTokenObjects.hpp:35-371`; `test/QmlChromeStyle.hpp:28-37`, `:77-84`.
- `ContourGuiApp.cpp:1796-1806`, `:1808-1814`, `:1884-1898`, `:2443`.
- `Config.hpp:1035-1056`, `:1085-1121`, `:1138`, `:1498-1501`, `:1810-1816`; `DefaultsEdition.hpp:29-43`;
  `Config.cpp:546-626`, `:1601`; `GlobalOverrides.cpp:63`; `ConfigDocumentation.hpp:306-318`, `:2477`, `:3404`;
  `SettingsController.cpp:1650-1658`; `Actions.hpp:112-128`, `:423`, `:953`.
- Tests:
  - `Config_test.cpp:3651` (the plan says `:3650`);
  - `GlobalOverrides_test.cpp:12`;
  - `SettingsController_test.cpp:812-831`;
  - `TerminalSession_test.cpp:336-399`, `:3666-3718`;
  - `QmlComponents_test.cpp:102-120`, `:3038`, `:3175`, `:3456`, `:3586-3663`, `:6719-6760`;
  - `KeyboardRouting_test.cpp:210`, `:248-251`;
  - `ChromeSnapshot_test.cpp:103-108`;
  - `DisplayRendering_test.cpp:2223`;
  - `UiStyleProvider_test.cpp:343-353`, `:373`;
  - `UiStyle_test.cpp:204` (`nativeRowsKeepTodaysGeometry`, `[config]`);
  - `TerminalSessionExtensions_test.cpp:282-354`.
- Fixtures: `GuiTestFixtures.hpp:86-148` (`createdPtys`, `requestedCwds`, `requestedProfileNames`,
  `reportedHosting`) and `:864` (`ScopedControllerWithOsWindow`); `QmlMocks.hpp:22-217` (`MockSession`) and
  `:225-314` (`MockPaneProxy`); `MockMainController.hpp:63`, `:154-157`; `MainWindowControllerMock.hpp`.
- `SessionChrome.qml:137`; `Toast.qml` (`text`, `glyph`, `shown`, `AlertMessage`).
- `src/contour/CMakeLists.txt`:
  - `_qml_files` `:383-425`;
  - `contour_gui_test` light list `:770-820`;
  - heavy list `:861-886`;
  - `contour_test` `:1151-1199`.
  - `Config_test.cpp` is heavy GUI (`:866`, comment `:1147`). `GlobalOverrides_test`, `UiStyle_test`,
    `ChromeTokenGroups_test` and `ChromeSnapshotPlan_test` are `contour_test`. `UiStyleProvider_test` is heavy
    GUI (`:885`).
- `scripts/check-qml-sizes.py:36-49` (the size properties include `radius`, `border.width` and `duration`) and
  `qml-size-literal-baseline.txt` (`PaneNode.qml` is unlisted, so 0; `TerminalPane.qml 3`).
- `check-qml-colors.py` counts hex and all-numeric `Qt.rgba` only.
- `.clang-tidy:190` (`HeaderFilterRegex` covers `src/contour` and `src/vtworkspace`). `ENABLE_TIDY=OFF` in both trees.
- Tools: `/opt/homebrew/opt/llvm@22/bin/clang-format` and `clang-tidy`, `/opt/homebrew/bin/editorconfig-checker`.
- Tags present: `[vtworkspace][layout]`, `[vtworkspace][model]`, `[config]`, `[settings]`, `[contour][uistyle]`,
  `[uistyle]`, `[chrometokens]`, `[snapshot]`, `[contour][sizing]`, `[contour][tablabel]`, `[contour][multiwindow]`,
  `[keyboard]`, `[extensions]`, `[vtpty]`, `[contour][session]`, `[manager]`, `[contour][gui][qml][a11y]`,
  `[contour][gui][qml][split]`, `[display]`.
- Tags the plan creates: `[paneheader]`, `[heldmodifier]`, `[splitchrome]`, `[panestate]`, `[vtpty][mockpty]`,
  `[manager][panes]`, `[manager][processname]`, `[manager][paneend]`, `[contour][gui][qml][split][handle]`,
  `[…][panecard]`, `[…][paneend]`, `[contour][panes][reveal]`, `[…][mainwindow][band][density]`.
- Tags from 1c and 1d that the plan's filters name (absent until they land): `[titleband]`, `[titlebandsurface]`,
  `[titlebandgeometry]`, `[contour][titleband][controller]`.

Stale or imprecise (no build impact; see **F22**):
- `Pane::swapSession` → `swapLeafPayload` (`Pane.hpp:109-115`).
- `contentSizeForLeaf`'s caller is `WindowController.cpp:1835`, not `:1830`.
- `ChromeSnapshotPlan.hpp` enum and row lines (above). These move again with 4a Task 4, 1c and 1d.
- `win.hostLabel` "(1c)": it already exists.
- `Config_test.cpp:3650` → `:3651`.
- "The #2040 snapping case" is `WindowGeometry_test.cpp:1005-1030` (`[contour][geometry]`, a literal 6), not a QML case.
- At HEAD, every 1c and 1d symbol the plan names by name is absent, as expected:
  - `TitleBandLayout.hpp`, `TitleBandSurface.hpp`, `ToolbarButton.qml`;
  - `refreshTabBadgeForSession`, `badgeLook`, `ColorSourceKind::{Fill,Label,Surface,SchemeOnBar}`, `toneOf`;
  - `refreshTitleBand`, `titleBandTokens`, `chipOpenGlyph`.

  Their locations are 1c's and 1d's plans, as cited.

### 4. Parallel-run conflicts (Step 2 beside 1c and 1d)

**The plan is written as if 4a, 1c and 1d had landed** ("Steps 4a, 1c and 1d … are assumed done", `:50`; the
Depends-on notes of Tasks 3, 4, 5, 7, 9, 11 and 12). Run in parallel, only Tasks 1, 2 and 10 (and the vtpty half of
Task 5) can start on 4a alone. Every other task consumes a 1c or 1d symbol. The table lists each file that Step 2
changes and that 1c's or 1d's plan also changes, with the gate that keeps the rebase clean. "Gate" means: rebase
`work/native-redesign-step-2` onto the main line after the named task has landed there, and only then start the
Step 2 task.

| File | 1c / 1d change | Step 2 change | Keep the rebase clean by |
|---|---|---|---|
| `config/ChromeTokenGroups.hpp` | 1c T3: `TabShape`. 1c T4: new `SizeTokens`/`SpaceTokens`/`RadiusTokens`/`MotionTokens` fields, `TintTokens`, `LayoutTokens`, roles to `ChipTextStrong`, `ColorSourceKind` kinds, `level`, the factories, `statusInk`, every row. 1d T2: `BandBackdrop`/`BandFill`/`TitleBarGrowth`. | T3: new fields in the same structs, `DimTokens`, `MinimalDensityTokens`, 9 roles, `SchemeShade`/`Separator`, `BandFill::Background`, every row, the density parameter. | **Gate G1: 1c T4 and 1d T2.** Every row literal is edited by both (designated initializers), and T3 consumes 1c's factories, so it cannot be written earlier. Add fields after 1c's, by name, as the plan says. |
| `config/ChromeTokenGroups_test.cpp` | 1c T4 extends `numbersOf` and `everyRowMatchesTheDesignKit`. | T3 extends the same two. | G1. |
| `config/UiStyle.hpp`, `UiStyle_test.cpp` | 1c T4: `ProgressStatusRoles`, four glyphs beside `tabSeparator`. | T3: three glyph fields beside them, and drops `PaneDimPercent`. | G1. Place Step 2's glyphs after 1c's four. |
| `window/UiStyleProvider.{hpp,cpp}`, `UiStyleProvider_test.cpp` | 1c T4: `statusInk`, `badgeLook`, `hostChipLook`, `titleBand`, `tabWellLayout`, `snapToCells`, glyph properties. 1d T7: `bandFill`. | T3: `density`, glyphs, `paneHeader`, `paneHeaderName`, `colorOn`, `hairline`, `unfocusedDim`, `paneDimColor`. T7: `titleBand` and `bandFill` gain the density. T11: drops `paneDimTextColor`. | G1 for T3. Seam: put Step 2's declarations in their own `// {{{ Panes (Step 2)` group beside `paneDimColor()` (`.hpp:245`), not at the block's end where 1c and 1d append. Then 1d T7's later append merges cleanly. **Agree `bandFill`'s signature with 1d now** (**F16**). |
| `window/ChromeTokenObjects.{hpp,cpp}` | 1c T4: the group properties. | T3: the group properties. | G1. |
| `test/QmlChromeStyle.hpp` | — (1c and 1d use the existing `ChromeSetup` fields) | T3: `ChromeSetup::density {}` | Clean. |
| `ContourGuiApp.{hpp,cpp}` | 1d T3: `_titleBandMaterial` and `titleBandMaterial()` beside `_nativeMenus`. 1d T6: `titleBandTokens()`; `refreshChromeStyle()` gains `refreshTitleBands()`. | T3: `chromeInputs()` sets `density`. T5: `refreshChromeStyle()` refreshes the followers. T6: `splitChrome()`. | **Seam S1:** one manager fan-out, `TerminalSessionManager::refreshChromeDependents()`. 1d T6 creates it with `refreshTitleBands()`, and Step 2 T5 adds `refreshHeaderFollowers()` inside it, so `refreshChromeStyle()` is edited once. **Seam S2:** one `UiStyleProvider::resolvedRow()` (`ChromeTokenRow const&`) accessor, added by whichever of 1d T6 or Step 2 T6 lands first, read by both `titleBandTokens()` and `splitChrome()`. |
| `window/WindowController.{hpp,cpp}` | 1c T5: `Roles`, `hostChipState`, `data`, `resolvedTabLabel`, `refreshActiveTabHighlight`, `bindWindow` (`visibilityChanged`). 1d T4 (Branch A, S1 = Hint): `needsExpandedClientArea` beside `needsFramelessHint`. 1d T6: `eventFilter` (Expose, `ApplicationPaletteChange`), `onWindowScaleMaybeChanged`, `onWindowStateChanged`, `setTitleBarVisible`, `bindWindow` (`safeAreaMarginsChanged`), `refreshTitleBand`, two properties. | T4: `visiblePaneCount`. T5: `refreshHeaderFollowers`, `_headerReveal`. T6: the solver site. T7: `refreshTitleBand`. T9: `headerReveal` and the key cases of the filter. T10: the proxy refresh. T11: `shortcutText`. | **Gate G2: 1c T5** before T4 and T5. **Seam S3:** Step 2 T9 adds new `case` labels (`KeyPress`, `KeyRelease`, `MouseButtonPress`) to the existing `eventFilter` switch (`.cpp:1435-1489`) and leaves `bindWindow` untouched, because the filter is already installed there. 1d T6 edits only the Expose and palette cases. Keep Step 2's properties, signals and members in a `// {{{ Panes` group at the end of each block. **T7 waits for G4.** |
| `session/TerminalSession.{hpp,cpp}` | 1c T5: `foregroundProcessName()` (via `dynamic_cast`); `screenUpdated`'s post carries `OutputOrigin`; `handleCommandBlockFinished`; `setHostConnection` (`ResyncGrace`). | T4: two properties, two `Events` overrides, `refreshPaneProgramStatus`. T5: rewrites 1c's `foregroundProcessName()`, moves the timer out of `ExtensionState`. T10: `_end`. | G2 for T4 and T5: T5 edits the body 1c adds. T10 touches regions 1c does not (`:709-795`, `:1686-1785`, `:2831-2910`), so it can run before G2. |
| `session/TerminalSessionManager.{hpp,cpp}`, `TerminalSessionManager_test.cpp` | 1c T5: `setHostConnection`, the `refreshTabBadgeForSession` rename, `markVisibleSessionsSeen`, the removal "forgets the attention", `[manager][tabbadge]`/`[hostchip]` cases. 1d T6: `refreshTitleBands()` beside `controllerFor` (`.hpp:753`). | T4: the badge hook, `FocusPane*` recording, `lastPaneActivation`. T5: the followers. T8: drops `splitHandleThickness`. T9: `runPaneHeaderAction`. T10: `restartSession`, the `paneSessionReplaced` override, `[manager][paneend]`. | G2 for T4 and T5. T10 may run first, but its `restartSession` retires through `removeSession`, which 1c T5 also edits (the attention `forget`). A one-hunk conflict is expected there. Seam S1 (above). Test cases go in a `// {{{ Step 2: panes` section placed before 1c's new cases, not at end of file. |
| `qml/Main.qml` | 1c T7: `band`, `titleBar` placement, `tabsBar`, `bandRevealEdge`, `chromeHeight`, the drift chip. 1d T4: `flags` (`ExpandedClientAreaHint`). 1d T6: `flags` (`NoTitleBarBackgroundHint`). 1d T7: `titleBar.fill`, height, reveal top. | T9: the root `PaneNode` (`:219-220`) gains `visiblePanes` and `headerReveal`. | Low risk: different regions. 4a T4 also edits this `Loader` and lands first. |
| `qml/TitleBar.qml` | 1c T7 (restructure), 1d T7 (fill, height) | T7: `bandTitle` hidden on `"empty"` | **Gate G4: 1d T7** (in practice, all of 1d). |
| `qml/ToolbarButton.qml` | 1c T7 creates it. | T9 adds `extent`. | **Gate G3: 1c T7** before T9. |
| `window/TitleBandLayout.hpp`, `TitleBandLayout_test.cpp` | 1c T3 creates them. | T7: the density input and a Minimal rule. | G4. |
| `window/TitleBandSurface.hpp`, `TitleBandSurface_test.cpp` | 1d T2 creates them. | T7: `centre` and `density` inputs and the Minimal step. | G4. **F4** must be ruled on first. |
| `test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp`, `ChromeSnapshotPlan_test.cpp`, `ChromeSnapshot_test.cpp` | 4a T4: `surface`. 1c T6: `.decor` and three rows. 1c T7: `.drift`, `.frame`, `.mode`, `HostConnection` values and five rows. 1d T7: `.transparency` and `band-solid`. | T11: `ChromeSnapshot_test.cpp:105` (unchanged name). T12: `.density`, `.paneEnd`, `HoverPaneHeader`, four rows. | **T12 last, after G4.** Each step adds a field to every row literal, so earlier work guarantees a conflict on every row. |
| `test/MockMainController.hpp`, `MainWindowControllerMock.hpp` | 1c T5 (badge roles, `hostChipState`), T6 (`decor`), T7 (`titleBarVisible` setter, scene properties). 1d T7 (`titleBandHeight`, `titleBandRevealTop`). | T4 (`visiblePaneCount`), T8 (drops `:63`, `:154-157`), T9 (`headerReveal`), T11 (`shortcutText`) | Additive. Keep Step 2's members in one commented group. Expect small rebase hunks at G2 and G3. |
| `test/QmlComponents_test.cpp` | 1c T5 (`:190-239` mirrors renamed, `:1542-1700` retagged), 1c T6 (`[tabbadge]` cases) | T8 (`:102-120`, new handle cases), T9 (`:3456-3504`, `:6719-6760`), T11 (`:3038`) | Disjoint regions. Put Step 2's new cases next to the cases they replace, not at end of file. |
| `test/Accessibility_qml_test.cpp` (the kit table `:180-215`) | 1c T6 (`TabBadgeSlot`), 1c T7 (`HostChip`, `ToolbarButton`, `DaemonDriftBadge`) | T9 (`PaneHeader`), T11 (`ExitedFooter`) | G3: append Step 2's rows after 1c's. |
| `test/MainWindowQml_test.cpp` | 1c T6 and T7; 1d T7 and T8 | T7 (`[band][density]`) | G4. |
| `src/contour/CMakeLists.txt` | 1c T1–T5 append `contour_test` sources after `window/RemoteChrome_test.cpp`; 1c T6 and T7 add `_qml_files` after `qml/TabItem.qml` and others. 1d T2 adds after `TitleBandLayout_test.cpp`. 1d T3 adds `TitleBandMaterial_test` (light). 1d T4 adds a Qt floor. 1d T5 and T6 add the Cocoa `.mm`s and the heavy `TitleBandController_test`. | T1, T6, T10: four `contour_test` sources. T9, T11: three `_qml_files`. | **Seam S4: separate anchors.** Step 2's `contour_test` lines go as one block after `session/LocalFileAction_test.cpp`, not after `RemoteChrome_test`. Its `_qml_files` go after `qml/TerminalPane.qml` (`:397`), three lines from 1c's `TabItem` anchor. Both then merge without conflict. |
| `scripts/qml-size-literal-baseline.txt` | 1c T5, T6 (`TabItem`, `TabStrip`, `TabColorFlyout`), 1c T7 (`DaemonDriftBadge` removed, `TitleBar` 1→0) | T9 (`TerminalPane.qml` 3→2) | Lines 27–29 (`TabStrip`, `TerminalPane`, `TitleBar`) are edited by both, so a conflict is certain. On every rebase, regenerate with `python3 scripts/check-qml-sizes.py --print-baseline`, keeping the header. Never hand-merge. |
| `metainfo.xml` (0.9.0), the plans index's step table and *Last update* line, `docs/superpowers/ledgers/` | 1c T8, 1d T9 | T13 | Resolve at T13 on top of the main line. The index row for Step 2 already reads "(written)". |
| `ContourGuiApp.cpp` `chromeInputs()` | — | T3 | Clean. |
| `config/Config.{hpp,cpp}`, `GlobalOverrides.cpp`, `ConfigDocumentation.hpp`, `SettingsController.cpp`, vtpty, vtworkspace, `PaneNode.qml`, `TerminalPane.qml`, `QmlMocks.hpp`, `KeyboardRouting_test.cpp` | — (4a T3 edits `Config.cpp`, and lands first) | T2, T5, T6, T8, T9, T10, T11 | No parallel conflict. |

**The order that keeps Step 2 moving and every rebase small:**

| When | Step 2 tasks |
|---|---|
| Once 4a lands | 1, 2, 10, then Task 5's vtpty half (`Pty`/`Process`/`MockPty`/`MockPty_test`) as its own commit |
| **G1** (1c T4 and 1d T2 landed) | 3, then 6 and 8 |
| **G2** (1c T5 landed) | 4, then the rest of 5 |
| **G3** (1c T7 landed) | 9, then 11 |
| **G4** (1d T7 landed; in practice 1d done) | 7, then 12, then 13 |

Each gate is one `git rebase` of the Step 2 branch onto the main line, followed by its own filters. Step 2 is idle
between the first wave and G1. That idle time is the true cost of "parallel" here, and the controller should plan
for it rather than start Task 3 against a 1c that does not yet exist.

## Findings needing a ruling

Blocking (a task's own steps, the build or CI fail as written, or the plan mandates a defect or a contradiction that
must be ruled on before the task runs): **F1–F5**. The rest are non-blocking.

1. **F1 (blocking): the plan assumes 1c and 1d are done; in parallel, eight of its thirteen tasks cannot start.**
   - **Plan text:**
     - "Steps 4a, 1c and 1d (…) are assumed done" (`:50`).
     - "Written ahead of Steps 4a, 1c and 1d … A pre-flight scan re-verifies every reference once 1d has landed"
       (`:5-7`).
     - The Depends-on notes of Tasks 3, 4, 5, 7, 9, 11 and 12.
   - **Why it fails:** the 1d ledger's ruling runs Step 2 "in parallel with 1c/1d once 4a lands". Task 3
     uses `detail::fill/label/schemeOnBar`, `ChromeColorSource::level` and `toneOf`. Task 4 hooks
     `refreshTabBadgeForSession`. Task 5 rewrites 1c's `foregroundProcessName()`. Task 9 extends `ToolbarButton.qml`
     and reads `badgeLook`. Task 11 reads `chipOpenGlyph` and `badgeLook`. Tasks 7 and 12 edit 1c's and 1d's files.
     None of these exist on the worktree's base (41b99e0b) or at HEAD.
   - **Resolution:** adopt Part 4's gates G1–G4 and its task order (1, 2, 10, 5-vtpty → 3, 6, 8 → 4, 5 → 9, 11 →
     7, 12, 13), and the seams S1–S4. Record them as a ruling in `progress.md`. This scan is then re-run as a short
     delta at each gate: the references to 1c and 1d symbols can only be verified once they exist.

2. **F2 (blocking): Task 3 breaks three existing assertions and a QML binding.**
   - **Plan text:**
     - Task 3 Interfaces: "`paneDimTextColor` is removed (its one QML reader goes in Task 11)".
     - R-k2: `PaneDimPercent` goes, and `paneDimColor` becomes 15 % / 10 % / 5 %.
   - **Why it fails:**
     - `UiStyleProvider_test.cpp:348-349` asserts `paneDimColor().alpha() == 153` (60 %), and `:373` asserts
       `paneDimTextColor() == white`. Task 3 runs both files (`[contour][uistyle]`) and edits neither.
     - `TerminalPane.qml:220` still binds `chromeStyle.paneDimTextColor` until Task 11. The QML-warning gate then
       fails `[contour][gui][qml]`, which Task 3 also runs.
     - Task 11 Step 3 removes `paneDimTextColor` a second time, which contradicts Task 3.
   - **Resolution:** Task 3 keeps `paneDimTextColor` and rewrites the `:343-353` case to the kit values. Task 11
     removes `paneDimTextColor` together with its reader and the `:373` assertion.

3. **F3 (blocking): Task 10 breaks the two `[contour][session][start]` cases.**
   - **Plan text:** R-j: "Those three stop writing into the grid … Return restarts". Task 10 Files: "the two cases at
     `:3666-3718` are rewritten".
   - **Why it fails:**
     - `TerminalSession_test.cpp:336-379` and `:381-399` assert `screen.contains(ConfigurableStartPty::FailureText)`.
       The start failure no longer writes into the grid.
     - The first case also presses `Key::Enter` and expects `closed == 1`. Enter now restarts, because StartFailed
       is `Offered`. The session made by `makeSessionWith` has no manager, so nothing happens and `closed` stays 0.
     - Both cases are inside Task 10's own filter, `[contour][session]`.
   - **Resolution:** add both to Task 10's rewrite list:
     - assert `sessionEnd == StartFailed` and that `endMessage` contains `FailureText`;
     - assert the device is closed;
     - assert that `dismiss()` emits `sessionClosed` once;
     - assert that an Enter press with no manager is swallowed.

4. **F4 (blocking for Task 7; needs a ruling): Minimal's 28-pt bare band cannot be reached under 1d's geometry.**
   - **Plan text:** R-f and Review Focus 5 ("a bare 28-pt band"). Task 7: "`refreshTitleBand` passes
     `tokens.height = band.centre == Empty ? size.bareBand : size.titleBar`", and the surface step keeps "`<the row's
     growth>`".
   - **Why it fails:**
     - 1d's `resolveTitleBandGeometry` AppKit fit is `height = ceil(max(tokens.height, nativeHeight)·dpr)/dpr` (1d
       Task 2; 1d R-c: "the band is at least as tall as the native title bar").
     - Ruling S2 measured the native title bar at 40 pt with the compact toolbar that the MacOS row keeps (growth
       `CompactToolbar`), and at 32 pt without it.
     - So the real band is 40 pt, or 32 pt if the toolbar is dropped, never 28. The offscreen tests pass only
       because the mock's `titleBandHeight` is 0. The owner's manual check and the macOS snapshot would show 40.
   - **Resolution:** pick one.
     - **(a) Recommended:** in Minimal the bare band has growth `None` (no toolbar), and its height is
       `max(size.bareBand, h0)` = 32. Task 7 adds a `[contour][titleband][controller]` case with
       `record.metrics.height = 32`. R-f, Task 2's doc text and Review Focus 5 then say "a bare band as tall as the
       native title bar (28 pt or more)".
     - **(b)** Keep 40 and set `size.bareBand` to the measured height.
     - **(c)** Let the traffic lights overhang a 28-pt band. This contradicts 1d R-c.

5. **F5 (blocking): `restartSession` leaves the registries half-updated, so a restarted pane can never be pruned.**
   - **Plan text:** Task 10: "mints the next id (the `_pendingSessionId` pattern `splitActivePane` uses), calls
     `createBackingSession(…)`, then `_model->replaceLeafSession(…)` … and retires `ended` without pruning the leaf."
   - **Why it fails:**
     - `createBackingSession` stages `_pendingSessionId = id` (`TerminalSessionManager.cpp:331`).
       `replaceLeafSession` takes the id explicitly, so the model allocator (`:97-103`) never consumes it, and the
       next model-internal allocation would hand out a live id a second time. `splitActivePane` resets it at `:1866`
       for this reason.
     - Nothing writes `_tabBySession[new]` (compare `:1881`). When the restarted shell later exits,
       `removeSession(new)` finds no tab (`findTabHostingSession`, `:1224-1230`), skips `closePane`, and leaves a
       leaf with no session: a dead, blank pane.
     - 1c's tab badge (`findTabHostingSession`) also misses the new session.
   - **Resolution:** `restartSession` resets `_pendingSessionId` after `createBackingSession`, sets
     `_tabBySession[new] = tab`, and moves `_focusedSession` to the new session when it was the ended one. Then it
     calls `removeSession(ended)`, which tolerates the replaced leaf (Part 2). Add
     `[manager][paneend]` "a restarted pane closes normally": exit the new session, and the split collapses.

6. **F6: Task 2 runs its Config cases in the wrong binary.**
   - **Plan text:** Task 2 Step 4: "`out/appleclang-debug/src/contour/contour_test "[config]" && … contour_gui_test "[settings]"`".
   - **Why:** `Config_test.cpp` is in `contour_gui_test_heavy_sources` (`src/contour/CMakeLists.txt:866`, comment
     `:1147`). The three new Config cases never run, so red and green both pass vacuously. Task 3 (`contour_test
     "[…],[config],…"`) and Task 13 Step 2 repeat the mistake.
   - **Resolution:** Task 2 Step 4 runs `contour_gui_test "[config],[settings]"` and `contour_test "[config]"`
     (GlobalOverrides, UiStyle). Task 13 Step 2 adds `[config]` to the `contour_gui_test` line.

7. **F7: the re-tokenized pane dim and the new colour invokables read the palette, but nothing re-evaluates them.**
   - **Plan text:** R-k2: "`paneDimColor` … black at `dim.darkPercent` on a dark chrome … 'dark' decided as
     `statusColor` decides it". Task 3: `colorOn`, `unfocusedDim`.
   - **Why:**
     - `paneDimColor` is `Q_PROPERTY … CONSTANT` (`UiStyleProvider.hpp:143`), so a theme switch keeps the old alpha.
     - `colorOn(role, bg)` (for kinds other than `Fill`/`Label`/`Surface`/`Separator`) and `unfocusedDim`
       (the chrome tone) are invokables. The provider documents that each QML call site must read
       `paletteRevision` first (`.hpp:197-201`).
   - **Resolution:** `paneDimColor` becomes `NOTIFY paletteRevisionChanged`. Every QML call of `colorOn` or
     `unfocusedDim` uses the `paletteRevision < 0 ? … :` idiom. Add a `[contour][uistyle]` case that changes the
     palette and checks that `paneDimColor` changes.

8. **F8: the QML has no names for what it must pass the provider.**
   - **Plan text:**
     - Task 9: `colorOn(PaneHeaderText|PaneHeaderTextDimmed, …)`.
     - Task 9: "`accessibleName` from `PaneHeaderActions` via a provider list".
     - Task 11: `colorOn(PaneFooterFill, …)` and `badgeLook(3)`.
   - **Why:**
     - The global constraint says QML compares "the `int`s or the name strings the provider publishes". Nothing
       publishes `ChromeColorRole` or `TabBadgeKind` indices, so the QML would carry magic numbers.
     - No task creates the action-name list, and Task 9's file list omits `UiStyleProvider`. The test "names 'Split
       right', …" (Task 9, Accessibility) then fails.
   - **Resolution:** Task 3 adds the following, and Task 9 and Task 11 use them:
     - a `chromeStyle.roles` group of `CONSTANT int`s for the nine pane roles (the `statusDanger` pattern,
       `.hpp:139-142`);
     - `Q_PROPERTY(QStringList paneHeaderActionNames …)` from `PaneHeaderActions`;
     - a `badgeWorking`-style int, or `badgeLookByName`.

9. **F9: the grabber's radius is a size literal; one baseline claim and one pinned test do not exist.**
   - **Plan text:**
     - Task 8: "`splitGrabber` (… radius half its thickness …)".
     - Task 8: "`PaneNode.qml`'s size and colour baselines go down".
     - Task 8: "The `#2040` snapping case keeps passing at device-pixel ratios 1.25 and 1.75".
   - **Why:**
     - `radius` is a gated property (`check-qml-sizes.py:38`), and `/ 2` or `* 0.5` counts as a literal against
       `PaneNode.qml`'s baseline of 0, which fails the `lint` ctest.
     - `PaneNode.qml` is not in the baseline at all, so nothing can go down. Its handle colours are palette roles,
       which the colour gate does not count.
     - The #2040 case is `WindowGeometry_test.cpp:1005-1030` (`[contour][geometry]`, a literal 6), which no Task 8
       filter runs.
   - **Resolution:**
     - Add `radius.grabber` (1 on the native rows, 0 on Terminal) in Task 3.
     - Drop the baseline sentence and the staging of `qml-size-literal-baseline.txt` from Task 8.
     - Either add a `[contour][gui][qml][split][handle]` case asserting `secondOrigin·dpr` is whole at 1.25 and 1.75
       with the token gap, or name the geometry test and add `[contour][geometry]` to Task 8's filter.

10. **F10: the foreground-process poll is compiled only with extensions, and two details are underspecified.**
    - **Plan text:**
      - Task 5: "the timer leaves `ExtensionState` … for the session".
      - `enum class ProcessNameFollower : uint8_t { Extension = 0b01, Header = 0b10 };`
      - "a remote-hosted pane has no process name: `reportedHosting = Remote`; the name stays empty".
    - **Why:**
      - `pollsForegroundProcess`, `armForegroundProcessPoll` and `pollForegroundProcess` all sit inside
        `#ifdef CONTOUR_EXTENSIONS` (`TerminalSession.cpp:1078-1084`, `:1162-1261`, with a stub `#else` at
        `:1266+`). This Mac builds with extensions ON, so a build without them (a CI lane, or
        `CONTOUR_EXTENSIONS=OFF`) would silently keep "polls nothing" for headers.
      - A stored follower set needs a named zero (AGENT: "zero the default").
      - `MockPtySessionFactory` gives remote sessions a `MockPty` too, so the remote case is vacuous with no name
        set, and it fails with one, because nothing in the design checks the hosting.
    - **Resolution:**
      - The timer, `pollsForegroundProcess` and the name live outside the `#ifdef`. Only
        `_terminal.foregroundProcessChanged` and the `Extension` follower stay inside it.
      - Store the set as `core::Flags<ProcessNameFollower>` (`vendor/core-cpp/src/core/Flags.hpp:32`).
      - The remote case sets the mock's name to `"vim"`, and the session never follows `Header` while its hosting
        is `Remote`.
      - Run the `[manager][processname]` cases in a `CONTOUR_EXTENSIONS=OFF` configuration once, or state that CI
        does.

11. **F11: the held-⌥ reveal never fires where the modifier auto-repeats; the filter is already installed.**
    - **Plan text:**
      - Task 9: "a `KeyPress` of `Qt::Key_Alt` with no other modifier calls `modifierPressed(now)`".
      - Task 1: "a second press re-arms from its own time".
      - Task 9: "`bindWindow` installs an event filter".
    - **Why:**
      - On Windows a held Alt sends auto-repeated `WM_SYSKEYDOWN`, which Qt delivers as `KeyPress` with
        `isAutoRepeat()`. Each one re-arms, so 500 ms never elapses.
      - The press of a modifier carries its own modifier: `modifiers() == Qt::AltModifier`, not `NoModifier`.
      - `bindWindow` already does `osWindow->installEventFilter(this)` (`WindowController.cpp:1245`), and
        `eventFilter` (`:1435`) is the place to add cases. 1d Task 6 edits the same switch (Part 4, S3).
    - **Resolution:**
      - Ignore `isAutoRepeat()` presses.
      - Treat "lone" as `modifiers() & ~Qt::AltModifier == 0`. Add a `[heldmodifier]` row: a second press while
        armed keeps the first time.
      - Add `KeyPress`, `KeyRelease` and `MouseButtonPress` cases to the existing switch.
      - The delay comes from `resolveChromeTokens(...).motion.headerRevealMs` at construction. It is 500 on every
        row and unaffected by Reduce Motion, so no rebuild is needed. Document it.

12. **F12: `PaneProxy.focus` can go stale, and the per-pane status adds a terminal lock to the output path.**
    - **Plan text:**
      - Task 4: "`Q_PROPERTY(int focus READ focus NOTIFY activeChanged)`".
      - Task 4: "`refreshTabBadgeForSession` also calls the session's `refreshPaneProgramStatus()`".
    - **Why:**
      - Take a pane reached by `FocusPaneLeft` and then clicked. The active pane does not change, so
        `activeChanged` never fires, and `focus` stays `KeyboardFocused`. The actions stay shown.
      - 1c R-b routes every output burst through `refreshTabBadgeForSession` (`screenUpdated`'s post), so the hook
        takes the terminal lock and resolves the records once per burst for every pane.
    - **Resolution:**
      - The manager notifies the tab's proxies (`notifyActiveChanged`) whenever `lastPaneActivation` changes.
      - Call `refreshPaneProgramStatus()` only from the program-status path (`programStatusChanged` →
        `refreshTabProgramStatusForSession`/1c's rename with no output origin), not from the output path.

13. **F13: `session/` gains upward includes of `window/`.**
    - **Plan text:**
      - Task 10: "`_terminatedAndWaitingForKeyPress` becomes `window::SessionEnd _end`".
      - Task 4: the session computes its badge with `window::badgeFor` and `resolveTabBadge`.
    - **Why:** AGENT's `src/contour/` layering puts `session/` below `window/`. Today `TerminalSession.{hpp,cpp}`
      include no `window/` header (grep), so `PaneState.hpp` in a `TerminalSession` member would be the first.
    - **Resolution:**
      - `SessionEnd`, `FooterRestart`, `RestartSupport` and the footer table go to `session/SessionEnd.hpp`
        (Qt-free, tested in `contour_test`). `window/PaneState.hpp` keeps only `PaneState` and `paneStateOf`.
      - Task 4's badge kind is computed in the manager, which already includes the `window/` decisions, and handed
        to the session through a plain state setter.

14. **F14: small defects in Task 10 and Task 11's interfaces, and a wrong chord.**
    - **Plan text:**
      - "`[[nodiscard]] std::string paneFooterMessage(SessionEnd end, std::string_view detail);`" in a header-only
        file.
      - Rows like `"Process exited with code {}."`.
      - "⌘W still closes it" (Also pinned), the footer's "Close ⌘W", D3 "⌘W closes", Task 11's test "the Classic
        default's chord".
      - `Q_INVOKABLE [[nodiscard]] QString shortcutText(…)`.
    - **Why:**
      - A non-`inline` function defined in a header breaks the ODR across TUs.
      - A `std::string_view` from a table is not a constant format string, so it needs `std::vformat`.
      - The default `ClosePane` chord is Ctrl+Shift+X (`Config.hpp:1498-1501`). No edition binds ⌘W (issue #2175).
      - The house order is `[[nodiscard]] Q_INVOKABLE` (`UiStyleProvider.hpp:324`).
    - **Resolution:**
      - Make `paneFooterMessage` `inline` and use `std::vformat` (`std::format_string` cannot be stored in the row).
      - Reword the plan's ⌘W mentions as "the `ClosePane` binding (Ctrl+Shift+X by default)". Task 11's test
        expects `shortcutText("ClosePane")` to be that chord in the host's style.
      - Use the house attribute order.

15. **F15: `splitChrome()` needs an accessor that does not exist, and the display case is never run.**
    - **Plan text:**
      - Task 6: "`splitChrome()` — from the published provider's resolved tokens and its cell units".
      - Task 6: "the content-driven resize case gains a split section … The `[display]` case runs on weston in CI".
    - **Why:**
      - `UiStyleProvider` exposes its `ChromeInputs` (`.hpp:226`) and the per-group objects, but no
        `ChromeTokenRow`. 1d Task 6's `titleBandTokens()` needs the same one (Part 4, S2).
      - The new display section is display-gated (`REQUIRE_DISPLAY_OR_SKIP`). Task 6 Step 4 runs offscreen only, so
        it is first executed in Task 13 or in CI.
    - **Resolution:**
      - Add `[[nodiscard]] config::ChromeTokenRow const& resolvedRow() const noexcept` to the provider. Whichever of
        Step 2 Task 6 or 1d Task 6 lands first adds it, and the other reuses it.
      - Task 6 Step 4 adds `CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test
        "[display][controller][resize]"`.
      - The existing `[contour][sizing]` cases (`WindowSizing_test.cpp`) pin no split solver and are unaffected.

16. **F16: Task 7's interfaces do not fit 1c's and 1d's.**
    - **Plan text:**
      - "`TitleBandRules` gains a density column … `{ Pill, ClientBand, Minimal, gate Hidden, Top }`".
      - "`TitleBandSurfaceInputs` gains `BarCentre centre` …".
      - "`bandFill(frame, mode)` answers … `BandFill::Background` … QML calls are unchanged".
    - **Why:**
      - 1c matches `TitleBandRules` on `(shape-or-any, frame, position)` only. The gate is read inside the rule, so
        a row keyed on "gate Hidden" needs a new key column.
      - `bandFill(int frame, int mode)` (1d Task 7) has no centre or strip input, so it cannot know the band is bare
        unless the QML call changes.
      - `refreshTitleBand` would have to recompute `resolveTitleBand` in C++ per window (the strip gate). Today only
        the QML computes the band.
    - **Resolution:**
      - Express Minimal as a post-step after the rule match: under `Minimal`, a `Title` centre becomes `Empty`. It
        is data in a one-row table, so no key column is added.
      - Agree with 1d now that `bandFill` takes the centre (`bandFill(frame, mode, centre)`). Main.qml passes
        `band.centre`.
      - `refreshTitleBand` computes the band from `tabBarShouldShow`, the frame and the mode, through the same pure
        call.

17. **F17: three token-table details.**
    - **Plan text:**
      - "`struct MinimalDensityTokens { int paneHeader; int paneGap; int splitHit; int paneInset; int paneCardRadius; }`"
        while `resolveChromeTokens` "copies … into … `space.paneGap`, `space.paneGapRows`, …".
      - "`std::array<std::string_view, 4> paneActionGlyphs`".
      - `chromeDensityName` returning `"comfortable"`/`"minimal"`.
      - `SchemeShade` "moved by `level` HSL lightness points".
    - **Why:**
      - The sub-row has five fields for six targets.
      - The 4 duplicates `PaneHeaderActions.size()`, which lives in `window/` where `config/` cannot see it.
      - The name function re-spells `ChromeDensityTable`.
      - 1d's `bandTintOf` already carries a local constexpr RGB→HSL helper.
    - **Resolution:**
      - Add `paneGapRows` to the sub-row.
      - Move `PaneHeaderAction` and its count to `config/ChromeDensity.hpp` (or a `config/PaneActions.hpp`), and
        size the glyph array by the count.
      - Derive `chromeDensityName` from `configEnumValues<ChromeDensity>()`.
      - Hoist 1d's HSL helper to a shared header (for example beside `vtbackend::hslToRgb` in `core/Color.hpp`)
        rather than writing it twice.

18. **F18: the Minimal dim floor and the Terminal row's Minimal bar reach further than D5 and §3.6 say.**
    - **Plan text:**
      - R-g: "Under Minimal, the effective unfocused dim is `max(dim_unfocused, pane.dim)`".
      - R-f: "The Terminal row under `Multiple` already hides its bar row with one tab, which is §3.6's 'In Minimal,
        the bar row is hidden'".
    - **Why:**
      - `dimOverlayColor` (`TerminalPane.qml:184-189`) also dims every pane of an inactive window, a single pane
        included. With the floor, a Classic user's lone pane would gain a 15 % dim each time the window loses
        focus. That is not the "focus cue between panes" D5 is about.
      - Classic's `tab_bar_visibility` is `Always` (`Config.hpp:1101`), so on the Terminal row with Minimal and one
        tab the bar row stays. §3.6 says it is hidden.
    - **Resolution:**
      - Apply the floor only while `visiblePanes ≥ 2`.
      - Record the TUI deviation as a ruling ("Minimal does not override `tab_bar_visibility`"), or apply R-f's
        bare-band rule to the Terminal row's bar under `Minimal` as well.

19. **F19: opaque cards remove a split's background transparency.**
    - **Plan text:** "Card: fill = the pane's own terminal background". Task 8: "a `colors.paneGapFill` backdrop".
    - **Why:** `TerminalPane` honours `session.opacity`, `isBlurBackground` and image backgrounds. A card filled with
      an opaque `backgroundColor`, over an opaque gap backdrop, hides the desktop the moment a translucent profile
      splits, while a single pane stays translucent.
    - **Resolution:** the card fill and the backdrop take the session's background opacity (`opacityBackground`).
      Alternatively, the card paints only its border and header and leaves the fill to the terminal. Add a
      `[panecard]` case with `opacityBackground = 0.5`.

20. **F20: Reduce Motion is stated but not pinned.**
    - **Plan text:**
      - Task 8: "its opacity animates over `motion.fast` under `motion.full`, and cross-fades otherwise".
      - Task 9: the actions fade "(cross-fade under Reduced)".
      - Task 11: "The pill never animates its glyph".
    - **Why:**
      - No test asserts any of them. Under `Reduced`, `motion.fast` is already the 100 ms cross-fade
        (`resolveChromeTokens`, `:462-468`), so both branches are the same code.
      - The pill's glyph is the Working badge's, which 1c animates. The only guard is an untested sentence.
    - **Resolution:**
      - Add a `[contour][gui][qml][panecard]` case under `{ .motion = Reduced }`: `motion.style == crossFade`, no
        `y`/`x` animation on the reveal overlay, and the pill's `toastGlyph` has no running animation.
      - Drop the two-branch wording for opacity-only fades.

21. **F21: tooling for a parallel run.**
    - **Plan text:**
      - The plan never mentions clang-tidy.
      - "Never run two builds at once".
      - Task 13: "`/simplify` on … `git diff <the plan commit>..HEAD`".
    - **Why:**
      - `ENABLE_TIDY=OFF` in both trees. The 4a ruling makes every implementer run
        `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path)`
        on touched `.cpp` files, and 4a found three real errors that way.
      - Two worktrees mean two builds at once on one Mac. The heavy pool (`Config_test` at 7.96 GB) is per tree,
        and the disk has 11 GiB free.
      - After the rebases, `00ba0eed44ce6781907d7f0b3367b137bf013cc8..HEAD` contains 1c's and 1d's commits as well.
    - **Resolution:**
      - Add the clang-tidy line to every task's Step 5, run from the worktree so that `-p` names its own tree. New
        header-only files are linted through their `_test.cpp` (`HeaderFilterRegex`, `.clang-tidy:190`).
      - The controller serializes builds machine-wide: one build at a time across both trees.
      - Watch the disk before G1.
      - `/simplify` takes Step 2's own commits: `git diff $(git merge-base HEAD feature/native-redesign-phase0)..HEAD`
        taken before the final rebase, or the commit list from the ledger.

22. **F22: stale references (no build impact).**
    - `Pane::swapSession` is `swapLeafPayload` (`Pane.hpp:109-115`).
    - `contentSizeForLeaf`'s caller is `WindowController.cpp:1835`.
    - `ChromeSnapshotPlan.hpp`'s lines are `:35-49`, `:66-74` and `:97-108`.
    - `Config_test.cpp:3650` is `:3651`.
    - `win.hostLabel` predates 1c (`WindowController.hpp:132`).
    - The "#2040" case is a geometry test (**F9**).
    - Task 11's "`TerminalPane.qml`'s baselines go down" belongs to Task 9.
    - The spec cites `:821` and `:836` are `:820` and `:835`.

    Correct them when the tasks are dispatched, as the 4a run did (ruling P1).

23. **F23: "the cwd follows `cd` within a second" assumes shell integration.**
    - **Plan text:** Task 13 Step 4 (manual).
    - **Why:** the property is recomputed only on OSC 7 and OSC 3008, as §3.6 says. Without integration,
      `displayWorkingDirectory()` falls back to the process's start directory (`TerminalSession.cpp:5790-5792`),
      which is never re-read.
    - **Resolution:** reword the check as "with shell integration (OSC 7)". Optionally, the 1-s header poll also
      refreshes the fallback cwd while no OSC 7 has been seen. That would be a ruling, and §3.6 does not ask for it.
