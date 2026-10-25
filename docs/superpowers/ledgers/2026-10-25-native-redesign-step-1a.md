# SDD ledger — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-1a.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.1 (binds), with §2.1, §2.3
and §2.4 (and what they cite: §1, §2.2, §2.6, §2.7). Scanned against HEAD 6da3e0a0 on
`feature/native-redesign-phase0`. Rulings R14 (TestApp is Classic) and R20 (dialog placement is a token, no
Retry on the hosting banner, an explicit `classic` never sees the banner) are taken as settled and not reopened.

## Pre-flight scan

### 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → 2 | `config::AccessibilityPreferences` and its three enums (zero = default) | `IAccessibilityPreferenceSource::current()`, `AccessibilitySwitch` | Consistent. |
| 1 → 3 | `resolveChromeTokens`, `ChromeTokenRow` groups, `ChromeColorSource`, `ChromeSchemeColors`, `chromeSchemeColorsOf` | `ChromeTokenObjects` (one QObject per group), `ChromeInputs::scheme`, `ChromeColorTokens` | Names match. **F11**: the Selection slot's source is a `CellRGBColor` variant with no stated rule. **F13**: `setPixelSize(std::lround(v))` narrows `long` → `int`. |
| 1 → 5/6/7 | Row values: `space.s3` 6 (native) / 0 (Terminal), `size.toast` 36 / 1, `radii.toast` 18 / 0, `size.dialogWidth` 280 / 32, `layout.dialog` Sheet only on MacOS, Terminal colour sources | Banner height, Toast pill, KitDialog width and placement, through Task 3's groups | Values agree with every primitive test (36, 18, `2 * s3`, 32 cells, MacOS sheet). **F1**: the `2 *` the primitives need is itself a size literal under Task 4's rule. |
| 1 → 9 | `MotionPreference` | `motionAdjusted(settings, MotionPreference)` | Consistent. |
| 2 → 3 | `AppearanceServices`, `nullAppearanceServices()`, `NullAccessibilityPreferenceSource`, `NullPlatformAppearance`, `appearanceServicesWith`, `AccessibilitySwitch`/`AppearanceSwitch` | `ContourGuiApp` constructor parameter and members; `TestApp`'s trailing parameter defaulting to `nullAppearanceServices()`; the `[appearance]` cases | Signatures match. **Conflict F2**: the plan puts the null side in the `.cpp` "wholly inside `#if !defined(__APPLE__)`", so on Apple `nullAppearanceServices()` and the two Null classes have no definition. Every Apple `TestApp` then fails to link. |
| 2 → 9 | `AccessibilitySwitch::set` notifies the stored callback | Task 9's live cases (`sw.set(Reduced)` → sessions follow) | Consistent at the interface. **F3**: the session the test checks is never reached (see Task 9). |
| 3 → 5/6/7 | `chromeStyle.size/space/radii/type/motion/elevation/contrast/tui/layout/colors`; `tui.floating/inverse`, `layout.centered/sheet`; `ChromeSetup.scheme`/`.accessibility` | `Banner.qml`, `Toast.qml` (`enterOffset: chromeStyle.motion.travel`), `KitDialog.qml`, and their Terminal-row test setups | Every consumed name exists in Task 3's Interfaces (`space.s3/s4/s5/s6/s7/s8`, `type.footnote/body`, `contrast.separatorAlpha`, `elevation.e2`, `colors.banner*/toast*`). **F14**: `.accessibility.motion = Reduced` and `.appearance.accent = …` are nested designators, which C++ doesn't have. **F26**: `space.*` is quantised horizontally, but Banner uses `s3` vertically. |
| 3 → 8 | `UiStyleProvider.hpp/.cpp` gain the groups and `accentColor` | Task 8 deletes `hostingNotice*Alpha` (`:117-118`, `:224-228`, `.cpp:272-280`) | Same files. Task 8's line numbers will have moved by the time Task 3 has landed (**F19**). `chromeStyle.borderWidth` (Banner separator) already exists. Consistent otherwise. |
| 3 → 9 | `ContourGuiApp::accessibilityPreferences()`, `onAccessibilityPreferencesChanged`, `TestApp(…, AppearanceServices)` | Both `sessionSettings` call sites wrapped with `motionAdjusted`; `updateMotionPreference` called from the handler; `ConfiguredTestApp` forwards services | Consistent. **F3** (test route), and **F19**: Task 9's Files leave out `GuiTestFixtures.hpp`. |
| 3 → 10 | `refreshChromeStyle()`; `chromeInputs()` takes the scheme from the *configured* default-profile palette (R-f) | `sessionBackgroundChanged` → `applyGuiTheme(…, liveBackground)` | **Conflict F9**: Task 10 says the Terminal row "follows it the same way, through the rebuilt provider (Task 3)". But nothing in Task 10 rebuilds the provider, and its scheme is the config palette, never the live one. |
| 3 → 11 | `refreshChromeStyle()` at `TerminalSessionManager.cpp:741` | `onConfigurationReloaded()` replaces that call and also calls `_newDefaultsNotice->refresh()` | Hand-over is explicit and consistent. **Conflict F5**: the notice exists only once the QML engine does. `reloadAllSessions()` runs in engine-less `TestApp`s (`[manager][theme]`, the session reload cases), so the unguarded `->refresh()` dereferences null. |
| 4 → 5/6/7/8/11 | `check_qml_sizes` gate. Any non-zero numeric literal in a size property fails, baseline 0 for new files. Baseline lowered by 5 in Task 8 | Every new QML file and the Main.qml edits | **Conflict F1**: Banner's `implicitHeight … + 2 * chromeStyle.space.s3` and Toast's width `text + 2 * space.s7` are offences under the rule as written, so Task 5/6 Step 4 fails. The "5" for the removed bar is correct (`+ 8`, `border.width: 1`, two margins of 8, one of 4). **F21**: the rule is underspecified. |
| 5 → 8 | `Banner` (`text`, `dismissed`, children `bannerText`/`bannerDismiss`, `height: visible ? implicitHeight : 0`) | `hostingNoticeBar` in `bannerStack`, and Task 8's tests (scoped `bar->findChild("bannerText")`, click `bannerDismiss`) | Consistent. The objectNames survive, because `hostingNoticeBar` is set on the Banner instance. |
| 5 → 11 | `actionText`, `actionTriggered`, `dismissText` | `newDefaultsBanner` ("Use New" / "Keep Mine") | Consistent. |
| 6 → 7 → 8 → 11 | Sequential edits to `ChromeSnapshotPlan.hpp`: `ScriptAction` (+ShowToast, +OpenKitDialog), rows, the count `static_assert`; Task 8 adds the `banner` column to every existing row, its own included; Task 11 adds `SnapshotBanner::NewDefaults` | `ChromeSnapshotPlan_test.cpp` counts 7/112/28 → 8/128/32 → 9/144/36 → 10/160/40; the `ChromeSnapshot_test.cpp` switch | Counts are right: 16 variants per script, 4 under ContourTui. **Conflict F4**: `ChromeSnapshotPlan_test.cpp:62` pins `matrix.back()` to `remote-reconnecting.terminal.native2026.light.png`. That fails from Task 6 on, and no task lists it. Task 6's Files also leave out the `SnapshotScript` enumerator (`:35-43`). |
| 6/7 → 8/11 | `ChromeSnapshotScene` `show()` cases and `_overlay` | Task 8 (`MainWindowScene.hostingNotice` from the row) and Task 11 (`MockNewDefaultsNotice` set in `load()`) | Consistent ordering. **Conflict F7**: `QString hostingNotice;` has no default member initializer. The scene's designated `MainWindowScene { … }` (`ChromeSnapshotScene.hpp:168-175`) then trips clang 22's `-Wmissing-designated-field-initializers` (precedent 3635823a). **F15**: no scene sets `ChromeSetup.scheme`, so every Terminal-row snapshot paints the toast and banner from `ColorPalette {}`, not from the variant's scheme. |
| 8 → 11 | `bannerStack` Column; `SnapshotBanner { None = 0, Hosting }` | Second Banner in `bannerStack`; `SnapshotBanner::NewDefaults` | Consistent. |
| 9 → 10 | `TerminalSession.hpp/.cpp`, `TerminalSessionManager.hpp/.cpp` (motion) | Same files (background hook, `liveBackground`, `sessionBackgroundChanged`, `setFocusedSession`) | Disjoint functions. Consistent. |
| 10 (vtbackend) | `Events::defaultBackgroundColorChanged`, forwarder called from `resetDynamicColor`/`setDynamicColor` and RIS | `TerminalSession` override → manager | **F8**: DECSTR (`Terminal.cpp:7235`, `resetColorPalette()`), XTPOPCOLORS and the GUI re-seed (`resetColorPalette(colors)`) also change the default background, and none of them notify. |
| 1–11 → 12 | Each task's tags | Step 2 union | Covers every task (`[kit]` ⊇ banner/toast/dialog, `[mainwindow]` ⊇ hosting). **F20**: the metainfo target and the `/simplify` base. |

### 2. Per-task self-consistency

| Task | Agrees with itself? | Detail |
|---|---|---|
| 1 | Mostly | Files, commit and Interfaces match. **F12**: "every other group equals the unadjusted row's" needs `operator==` on the group structs, and none is declared. `contrastRatio(light, 0xFFFFFF)` doesn't compile: `RGBColor(uint32_t)` is `explicit`, and `light` is a `uint32_t`. R-c's numbers check out (lowest 4.40:1 for `#248A3D` on white; `#FF6961` on `#1C1C1E` is 6.0:1). |
| 2 | No | **F2**: the null half is unreachable on Apple. **F23**: the `[cocoa]` cases read the real `NSWorkspace` flags, which contradicts Global Constraints "No test reads the developer's real OS settings". The test compares against values it reads itself, so it doesn't depend on them, but the constraint text is wrong. CMake places (`platform/CMakeLists.txt:15-66`, `:82-83`; `src/contour/CMakeLists.txt:749-807`, `:809-816`) and the format glob are correct. |
| 3 | Mostly | Interfaces match the tests. **F14** (nested designators in test prose). **F25**: Global Constraints "names every field, defaulted or not" would force edits at `MainWindowQml_test.cpp:427` and `ChromeSnapshotScene.hpp:329`, which Task 3 doesn't list. **F19**: `ContourGuiApp.cpp:396-410` is the signature; the initialiser list starts at 411. Commit globs cover the files. |
| 4 | Partly | The self-test lists `text: "width: 120"` as a non-offence, but the shared `code_lines()` *keeps* string contents, so the rule has to anchor the property itself; that isn't stated (**F21**). **F1** against Tasks 5/6. **F18**: no editorconfig check for the new `.py` and `.txt`. `CMakeLists.txt:274-279` is in fact `:276-277` (**F19**). |
| 5 | Partly | Tests match the interface. **F1**: Step 4's gate fails on the `2 *`. **F18**: Step 5 commits `QmlComponents_test.cpp` and `Accessibility_qml_test.cpp` without clang-format. |
| 6 | Partly | **F4** (`:62`, enumerator unlisted), **F1** (`2 * space.s7`). **F22**: the fixed `qWait(standard + 50)` is timing-dependent; prefer `qWaitFor`. Expected red (compile on `SnapshotScript::Toast`) is real. |
| 7 | Partly | **F17**: "`y` is 0 (top-attached, a sheet under the title band)". `y == 0` in `Overlay.overlay` is *over* the title band, not under it. The test pins `y == 0`. **F27**: `PopupSurface` draws `chromeStyle.radius`, not `radii.popover`, so the dialog's corner is not a 1a token. The hard one-cell shadow claim holds (`shadowBlurPixels = 0`, `shadowOffsetUnits = 1`, `UiStyle.hpp:386-387`). |
| 8 | Partly | **F7** (`QString hostingNotice;`). **F16**: Step 2's expected red is a runtime "bannerText not found", but Step 1 also adds `case SnapshotScript::HostingBanner:`, so the first red is a compile error. **F19**: `Main.qml:228-270` → `228-275`, TitleBar `:143-169` → `:143-175`, content `:182-190` → `:182-192`, `MockMainController.hpp:31-44` → `:29-44`, ctor `:100-105` → `:103-109`. The baseline "five" is correct. |
| 9 | No | **F3**: "Turning Reduce Motion on and off follows live" uses `makeDisplaylessSession`, and that session is not in `_sessionsById` (`TerminalSession_test.cpp:121`, `GuiTestFixtures.hpp:155`), so `updateMotionPreference` never reaches it. The born-under and reload cases are fine: `configureTerminal` assigns both durations through `ReloadedSettings` (`TerminalSession.cpp:5908-5909`, `:6000`). Files omit `GuiTestFixtures.hpp`, though Step 1 and the format list touch it. |
| 10 | Partly | **F8** (DECSTR/XTPOPCOLORS/re-seed), **F9** (the "Terminal row follows" claim). `Terminal.cpp:7421` is really `:7422`. The `Terminal.DECAC` filter exists (`Terminal_test.cpp:1157`). The manager tests must create sessions through `manager().createSession` (as `TerminalSessionManager_test.cpp:516` does): an unregistered session has `modelSessionId() == {}`, which would make "unfocused" indistinguishable from focused. |
| 11 | No | **F6**: Files put the new `config/` sources in "`src/contour/CMakeLists.txt` … in `contour_config`", but `contour_config` is defined in `src/contour/config/CMakeLists.txt`. **F5** (null notice on reload). **F24**: `MockNewDefaultsNotice(bool offered = false)` is a defaulted `bool` constructor parameter. The decision table is complete and consistent over all 9 combinations, and matches R20. The `settings.yml` round trip is sound: `guiSettingsFrom` keeps `defaults_edition` as a `globalOverrides` entry (`Config.cpp:1186-1203`), so a later Settings-page save preserves it. Files omit `TerminalSessionManager.cpp` (named only in prose). |
| 12 | Partly | **F20**: `metainfo.xml:97` is inside an XML comment (the `x.y.z` template, `:95-105`); the open release is `0.9.0` at `:107`. `/simplify` diffs from `d210ae10`, which includes the plan commit `6da3e0a0`. Expected `160` PNGs (16 × 10) is right. |

### Plan references verified against HEAD

Present and correct:
- `UiStyle.hpp:256`, `:259-264`, `:272-273`
- `UiStyleProvider.hpp:110-113/117-118/143/224-228`
- `UiStyleProvider.cpp:51-93`, `:272-280`
- `UiStyleProvider_test.cpp:334-346`
- `Config.cpp:690-712`, `:700-705`, `:981-983`
- `Config.hpp:1086`, `:1227`, `:1735`, `:3139-3148`, `:3280-3284`
- `Terminal.cpp:1396`, `:7564`
- `Terminal.hpp:706-714/785-786/1672-1674`
- `Screen.cpp:3423-3474`, `:8097-8106`
- `MockTerm.hpp:290-332`
- `TerminalSession.cpp:388`, `:5925`, `:2114-2132`
- `TerminalSession.hpp:935-936`
- `TerminalSessionManager.cpp:376-384`, `:741`, `:1137-1141`
- `ContourGuiApp.hpp:230-245/459/856`
- `ContourGuiApp.cpp:1660`, `:1729`, `:2101`, `:2111`, `:2210-2211`, `:2252`
- `GuiTheme.hpp:43-58`, `GuiTheme_test.cpp:21`
- `main.cpp:220-234`
- `GuiTestFixtures.hpp:480-503/563-579` (`:518-533` → `:516-533`)
- `QmlChromeStyle.hpp:18-53`
- `src/contour/CMakeLists.txt:278/323/370-405/1115-1127`
- `platform/CMakeLists.txt:15-66/78-85`
- `TaskbarProgressSink.cpp:24`
- `check-qml-colors.py:38-79`
- `ChromeSnapshotPlan.hpp:60-64/87-136/143`
- `ChromeSnapshotScene.hpp:168-175/187-205/194-199/326-339`
- `ChromeSnapshotPlan_test.cpp:19/60/63`
- `ChromeSnapshot_test.cpp:60-91`
- `MainWindowQml_test.cpp:381-414`
- `ConfirmDialog.qml:4-7/79`
- `DaemonDriftBadge.qml:18`
- `TerminalSession_test.cpp:2421-2431`
- `GuiTestFixtures.hpp:660` (Classic)
- the `out/appleclang-debug` tree (`CONTOUR_EXTENSIONS_BUNDLED:BOOL=OFF`, `contour_test`, `contour_gui_test`, `vtbackend_test` present)

Stale or wrong: see F19 and F20. No API the plan names is missing. The selection colour (F11) and `RGBColor`'s explicit constructor (F12) behave differently from how the plan writes them.

## Findings needing a ruling

Blocking (a task's own steps, or CI, fail as written): **F1–F7**. The rest are non-blocking.

1. **F1 (blocking): the size-literal gate rejects the plan's own primitives (Tasks 4, 5, 6).**
   - Plan text: Task 4 "a line is an offence when it assigns one of the size properties and its value contains a numeric literal other than `0`"; Task 5 "`implicitHeight` = the taller of its label and buttons plus `2 * chromeStyle.space.s3`"; Task 6 "width = text + `2 * space.s7`".
   - Why: `2` is a numeric literal in `implicitHeight`/`implicitWidth`, and new files have baseline 0. Task 5 Step 4 and Task 6 Step 4 fail.
   - Proposed: keep the rule strict, since §1.1 says "QML interprets tokens and holds no literals". Build `Banner` and `Toast` on a Qt Quick Controls `Control`, with `topPadding`/`bottomPadding: chromeStyle.space.s3` and `leftPadding`/`rightPadding: chromeStyle.space.s7`. A `Control` derives its implicit size from content plus padding, so no multiplier is written. Add "no arithmetic literal either" to the Task 5/6 interface text.

2. **F2 (blocking): the null backends are compiled out on Apple (Task 2; consumed by Tasks 3 and 9).**
   - Plan text: "the non-Apple factory, wholly inside `#if !defined(__APPLE__)`". The plan gives no location for `NullAccessibilityPreferenceSource`, `NullPlatformAppearance` or `nullAppearanceServices()`, which is declared non-`inline` in `AppearanceServices.hpp`.
   - Why: if they live in the guarded `.cpp`, every Apple `TestApp` fails to link. TestApp must always pass the null services (R14, Review Focus 1).
   - Proposed:
     - Define both Null classes inline in their headers, and make `nullAppearanceServices()` `inline` in `AppearanceServices.hpp`.
     - Only `make*` goes in the guarded `.cpp` and the `.mm` (§1.4: "a null implementation … injected").
     - `AccessibilitySwitch::set` is defined in-class (header-only rule).

3. **F3 (blocking): Task 9's live case checks a session the manager never sees.**
   - Plan text: "Turning Reduce Motion on and off follows live … `makeDisplaylessSession(testApp.app())` … `sw.set(Reduced)` → both 0 ms".
   - Why: `makeTestSession` constructs the session with a manager pointer but never registers it (`_sessionsById` is filled only at `TerminalSessionManager.cpp:324`), so `updateMotionPreference` iterates past it.
   - Proposed: create the live-case session through the manager (`testApp.manager().createSession(win.id)`, as `TerminalSessionManager_test.cpp:516` does). Keep `makeDisplaylessSession` for "born under Reduce Motion" and the reload case. §2.3 requires "a test covers this".

4. **F4 (blocking): the snapshot plan test pins the last script's file name (Tasks 6, 7, 8, 11).**
   - Plan text: "`ChromeSnapshotPlan_test.cpp:19`, `:60`, `:63` (counts)".
   - Why: `:62` checks `snapshotFileName(matrix.back()) == "remote-reconnecting.terminal.native2026.light.png"`. Each new row becomes `back()`, so `contour_test "[snapshot]"` fails in Step 4 of each of these tasks.
   - Proposed: Task 6 rewrites `:62` to derive the name from `SnapshotScripts.back().name` (`std::format("{}.terminal.native2026.light.png", …)`), so later rows need no edit. Task 6's Files also name the `SnapshotScript` enumerator (`ChromeSnapshotPlan.hpp:35-43`).

5. **F5 (blocking): `onConfigurationReloaded` dereferences a notice that engine-less apps don't have (Task 11).**
   - Plan text: "`void onConfigurationReloaded();` (calls `refreshChromeStyle()` and `_newDefaultsNotice->refresh()`)", which replaces the call at `TerminalSessionManager.cpp:741`.
   - Why: the notice is built "after the QML engine (beside `:2252`)". `TestApp`-based reloads (`[manager][theme]` at `TerminalSessionManager_test.cpp:1066`, the session reload cases) crash.
   - Proposed: guard it as `refreshChromeStyle()` and `applyWindowControlStyle` (`ContourGuiApp.cpp:1729`) are guarded: `if (_newDefaultsNotice) _newDefaultsNotice->refresh();`. Declare the member before `_qmlEngine` (a borrowed context property, as `_uiStyleProvider` is at `ContourGuiApp.hpp:853-856`).

6. **F6 (blocking): Task 11 lists the new `config/` sources in the wrong CMake file.**
   - Plan text: "Modify: `src/contour/CMakeLists.txt` — the new `config/` files in `contour_config`".
   - Why: `contour_config` is defined in `src/contour/config/CMakeLists.txt` (Task 1 gets this right).
   - Proposed: add `GuiStateStore.cpp GuiStateStore.hpp` and `NewDefaultsOffer.hpp` there. The two `config/*_test.cpp` files go into `contour_test` (`src/contour/CMakeLists.txt:1115`), and `window/NewDefaultsNotice*` into `contour_core` (`:278`/`:323`) and `contour_gui_test`.

7. **F7 (blocking on CI): `MainWindowScene::hostingNotice` needs a default member initializer (Task 8).**
   - Plan text: "`MainWindowScene` gains `QString hostingNotice;`".
   - Why: clang 22's `-Wmissing-designated-field-initializers` flags a field without an in-class initializer that a designated initializer omits. `ChromeSnapshotScene.hpp:168-175` names every other field, which is the exact case fixed in 3635823a (`StatusLineConfig::extension`). AppleClang locally may not warn, so it would surface only in CI.
   - Proposed: `QString hostingNotice {};`, and have the scene name it (`.hostingNotice = …`). Apply the same rule to the three new `ChromeSetup` fields (F25).

8. **F8: the background event misses three palette paths (Task 10).**
   - Plan text: "`resetDynamicColor` and `setDynamicColor` call the forwarder … the hard reset calls it after `resetColorPalette()`".
   - Why: DECSTR also calls `resetColorPalette()` (`Terminal.cpp:7235`). XTPOPCOLORS (`popColorPalette`) restores the default background. `resetColorPalette(ColorPalette const&)` (`:8784`, called from `TerminalSession.cpp:1291`/`:5979`) re-seeds it. After `CSI ! p`, a chrome turned light by OSC 11 stays light over a dark terminal.
   - Proposed: notify from the funnel instead of three call sites. Compare `defaultBackground` before and after in `Terminal::setColorPalette` and `resetColorPalette(colors)`, and in the two `Screen` dynamic-colour cases. Add `\033[!p` to the `[background-event]` table (count +1). §2.2: chrome colour follows the terminal.

9. **F9: Task 10's claim that the Terminal row follows the live background is untrue (Tasks 3, 10).**
   - Plan text (Task 10 Step 4): "the Terminal row follows it the same way, through the rebuilt provider (Task 3) and the palette".
   - Why: `sessionBackgroundChanged` only calls `applyGuiTheme`. `ChromeInputs::scheme` comes from the configured palette (R-f) and is not rebuilt, so under OSC 11 a Terminal-row banner stays the config's palette[0] while the chrome flips light/dark.
   - Proposed (ruling): keep R-f for 1a, because §2.1 "the active terminal scheme" reads naturally as the configured scheme. Correct the Task 10 sentence to "the Terminal row's colour roles stay the configured scheme's; only the chrome's light/dark follows". Alternatively, rule that live tracking is wanted, and have `sessionBackgroundChanged` call `refreshChromeStyle()` with the live background in `scheme.Background`.

10. **F10: plan resolutions that depart from the spec's text need the owner's ratification (Tasks 1, 3, 9, 11).** The spec binds, and these are reasoned departures:
    - R-a publishes `radii.*` where §2.1 says `radius.*`, because `radius` is taken (`UiStyleProvider.hpp:143`).
    - The token rows live in a new `config/ChromeTokenGroups.hpp`, while §2.1 is headed "`config/UiStyle.hpp`".
    - R-b applies Reduce Motion in `sessionSettings`, where §2.3 says "the session's display is constructed with the snapshot". The code supports the plan: the display never sees either duration.
    - R-c keeps the status colours under Increased Contrast, where §2.1 says they "use their AA-large-safe variants". Measured: every Standard value is already ≥ 3:1, so they are their own variant.
    - `NewDefaultsOffer.hpp` is a pure decision in `config/`, which §1.3's list (`window/`, `command/`, `vtbackend/`) doesn't name.

    Proposed: accept all five as written. Each is justified by code at HEAD and none changes behaviour the spec asks for. Record the acceptance in the plan's "Spec resolutions".

11. **F11: the Selection slot's source is ambiguous (Task 1).**
    - Plan text: "Selection = `selection.background` (or its resolved RGB)".
    - Why: `selection.background` is `CellRGBColor = variant<RGBColor, CellForegroundColor, CellBackgroundColor>` (`Color.hpp:482`).
    - Proposed: an `RGBColor` is taken as-is; `CellForegroundColor` → `defaultForeground`; `CellBackgroundColor` → `defaultBackground`. Add one `schemeColours` row with a `CellForegroundColor` selection.

12. **F12: Task 1's tests don't compile as specified.**
    - "Every other group equals the unadjusted row's" needs `[[nodiscard]] bool operator==(…) const = default;` on each group struct and on `ChromeColorSource`. Add it to the Interfaces block.
    - `contrastRatio(light, 0xFFFFFF)`: `RGBColor(uint32_t)` is `explicit` (`Color.hpp:55`). Write `contrastRatio(RGBColor { row.light }, 0xFFFFFF_rgb)`.

13. **F13: `std::lround` narrows (Task 3).**
    - Plan text: "`setPixelSize(std::lround(value))`".
    - Why: `long` → `int` (`bugprone-narrowing-conversions`, the F14 precedent of 1e).
    - Proposed: `static_cast<int>(std::lround(value))`.

14. **F14: nested designators in test text (Tasks 3, 6).**
    - Plan text: "inputs with `.accessibility.motion = Reduced`" and "`.appearance.accent = 0xFF00AA_rgb`".
    - Why: C++ has no nested designators.
    - Proposed: `auto inputs = chromeInputsFor(row, font); inputs.accessibility.motion = MotionPreference::Reduced;`, or a full `.accessibility = { .motion = …, .transparency = …, .contrast = … }`. `ChromeInputs` is a value type, not a mock, so assigning before use is not post-construction configuration.

15. **F15: Terminal-row snapshots don't use the variant's scheme (Tasks 6, 8, 11).**
    - Why: `ChromeSnapshotScene::load()` (`:329-334`) builds `ChromeSetup` without `.scheme`, so the toast and banner render from `ColorPalette {}` (bg `#1a1716`) over a session in `#1C1E24`/`#FAFAF8`.
    - Proposed: in Task 6, set `.scheme = chromeSchemeColorsOf(*config::preferredColorPalette(config::defaultColors(variant.edition), variant.appearance))`. §2.1 TUI row: "every colour role comes from the active terminal scheme".

16. **F16: Task 8's expected red is a compile error, not a runtime miss.**
    - Plan text: "FAIL — `bannerText`/`bannerStack` are not found".
    - Why: Step 1 also adds `case SnapshotScript::HostingBanner:`.
    - Proposed: state "compile failure on `SnapshotScript::HostingBanner`". Or move that case to Step 3, and the runtime red becomes real.

17. **F17: "a sheet under the title band" vs `y == 0` (Task 7).**
    - Plan text: "`y` is 0 (top-attached, a sheet under the title band)", and the test asserts `y == 0`.
    - Why: in `Overlay.overlay`, `y == 0` covers the title band.
    - Proposed (ruling, R20 keeps placement a token): either say "attached to the window's top edge" and keep `y == 0`, or bind `y` to the title band's height and pin that. The first needs no new interface.

18. **F18: formatting and editorconfig steps are missing (Tasks 4, 5, 6, 7, 8, 11).**
    - Task 5 Step 5 commits two `.cpp` files without clang-format.
    - No task runs `editorconfig-checker` on the new `.py`, `.txt` and `.qml` files (CI gate `build.yml:390-396`; the F10 precedent of 1e).
    - Proposed: add `/opt/homebrew/opt/llvm@22/bin/clang-format -i` to Task 5, and an `editorconfig-checker <files>` line to every commit step that adds non-C++ files.

19. **F19: stale or omitted references.**
    - Stale: `Main.qml:228-270` → `228-275`, `:143-169` → `:143-175`, `:182-190` → `:182-192`; `MockMainController.hpp:31-44` → `:29-44`, `:100-105` → `:103-109`; `Terminal.cpp:7421` → `:7422`; `CMakeLists.txt:274-279` → `:276-277`; `GuiTestFixtures.hpp:518-533` → `:516-533`; `ContourGuiApp.cpp:396-410` is the signature (initialisers from `:411`).
    - Omitted from Files: Task 9 `GuiTestFixtures.hpp`; Task 11 `TerminalSessionManager.cpp`.
    - Task 8's `UiStyleProvider` line numbers move after Task 3.
    - Proposed: correct them in the plan text, and have Task 8 locate by name rather than line.

20. **F20: Task 12 targets the wrong release and diff base.**
    - `metainfo.xml:97` is the commented-out `x.y.z` template (`:95-105`); the notes belong in `0.9.0` at `:107`.
    - `git diff d210ae10..HEAD` includes the plan commit; use `6da3e0a0..HEAD`.

21. **F21: the size gate's matching rule is underspecified (Task 4).** `code_lines()` keeps string contents, so the property must be matched as an assignment target. Unstated cases:
    - `foo { width: 120; height: 4 }` on one line;
    - grouped `anchors { margins: 8 }` and `font { pixelSize: 12 }`;
    - `property real gap: 8`;
    - whether the count is per line or per literal;
    - path separators in the baseline (CI runs the gate on Windows).

    Proposed:
    - Match `(^|[{;])\s*(<prop>)\s*:`, with the grouped forms added to the table as `margins`, `pixelSize` and so on, scoped to their group.
    - Count literals, not lines.
    - Write baseline paths repository-relative with `/`, as the colour allowlist does.
    - Add self-test samples for each case.
    - Leave `property … : N` out of 1a, and say so.

22. **F22: a fixed wait in an offscreen animation test (Task 6).** `QTest::qWait(motion.standard + 50)` then `opacity == 1` depends on timing. Proposed: `QTest::qWaitFor([&] { return toast->property("opacity").toReal() == 1.0; }, 2000)`, as `QmlComponents_test.cpp:2312` does.

23. **F23: a Global Constraint contradicts the `[cocoa]` cases (Task 2).**
    - Plan text: "No test reads the developer's real OS settings".
    - Why: `CocoaAppearanceSources_test.mm` reads `NSWorkspace` and `controlAccentColor` by design.
    - Proposed: reword to "No test's outcome depends on the developer's real OS settings; only the `[cocoa]` backend cases read them, and compare against what they read". Review Focus 1 is unaffected.

24. **F24: a defaulted `bool` constructor parameter on a mock (Task 11).**
    - Plan text: "`explicit MockNewDefaultsNotice(bool offered = false)`".
    - Why: AGENT.md calls a defaulted `bool` parameter "the worst form", and its carve-out covers setters, not constructors.
    - Proposed: `explicit MockNewDefaultsNotice(config::NewDefaultsOffer offer)` with `offered = offer != NewDefaultsOffer::None`. The scene passes the row's offer directly, and the table stays the source.

25. **F25: the designated-initializer constraint is stricter than the compiler (Tasks 3, 8).**
    - Plan text: "a designated initializer names every field, defaulted or not".
    - Why: clang exempts fields with in-class initializers. The real rule is that every new member gets `{}` or a value, which `pro-type-member-init` also needs. Read literally, Task 3 must edit `MainWindowQml_test.cpp:427` and `ChromeSnapshotScene.hpp:329`, which it doesn't list.
    - Proposed: give the three new `ChromeSetup` fields `{}`, and correct the constraint's wording.

26. **F26: `space.*` is quantised horizontally but Banner uses `s3` vertically (Tasks 3, 5).**
    - Plan text: "`ChromeSpaceTokens` … (horizontal)" and Banner padding `space.s3`.
    - Why: harmless today (Terminal `s3 = 0`), but `s4`+ under `Cell` would pad by cell *width*.
    - Proposed: state in Task 3 that a vertical use of a space token is a later step's concern, or publish `space` vertically scaled as well. Non-blocking.

27. **F27: KitDialog's surface radius is not a 1a token (Task 7).** `PopupSurface` uses `chromeStyle.radius` for both the surface and its shadow corner (`PopupSurface.qml:49`, `:74`), not `radii.popover` (MacOS 14). Proposed: accept for 1a, since the spec only requires the primitives to read tokens and the legacy radius is one. Note it as the first reader to migrate when `radius` is retired (R-a).

## Rulings

## Rulings
- Ruling A1: all 27 pre-flight findings F1–F27 are accepted with their proposed resolutions. Because they touch most tasks, the plan text itself is amended to carry them (one docs commit) before Task 1, so each task brief is self-consistent — the spec is unaffected — cost if wrong: one docs commit to revert.
- Ruling A2 (= R20): dialog placement is a token; no Retry on the hosting banner; explicit classic never shows the new-defaults banner.
- Plan amended 24672647 (F1–F27 folded in; F1/F8/F19/F20 adjusted, see agent note). Task 1: BASE 24672647.
- Task 1: implementer dispatched (sonnet).
- Task 1: implementer DONE 1c8fe545 (13 cases/235 assertions). Review dispatched.
- Task 1: review Spec ✅ Quality Approved; minor fix round dispatched (pin all four rows, combined overrides, Doxygen, CMake order).
- Task 1: fix round 1 committed 58f11960 (all four rows pinned; combined overrides; Doxygen; CMake order). 15 cases/547 assertions. Accepted inline (test/doc-only).
- Task 1: complete (1c8fe545, 58f11960).
- Task 2: BASE 58f11960.
- Task 2: implementer dispatched (opus).
- Task 2: implementer DONE ad3dcd48 ([appearance-source] 7 cases; [mainwindow] 10). Non-Apple factories compile-probed only. Ruling A3: no light/dark field in PlatformAppearance — Qt's QStyleHints::colorScheme already carries it and GuiTheme consumes that — cost if wrong: one field later. Review dispatched.
- Task 2: review Spec ✅ Quality Approved (non-Apple guards, ODR, ARC, observers verified). Two Low fixes dispatched: dangling notify in test switch; @pre GUI thread on current().
- Task 2: fix round 1 committed a555ee72 (switch notify cleared on destruction + test; @pre GUI thread). 8 cases/37. Accepted inline.
- Task 2: complete (ad3dcd48, a555ee72).
- Task 3: BASE a555ee72.
- Task 3: implementer dispatched (opus).
- Task 3: implementer DONE 1d4f64e5 (brief filter 182 cases; mainwindow/chromerow/qml/provider 134; lint 8/8). Deviations: slots→bySlot (Qt keyword), scheme default = default palette, services by const&, capture after first provider, unit choices. Review dispatched.
- Task 3: review Spec ✅ Quality Approved (all 5 deviations justified; rebuild bounded, GUI-thread, no dangling QML). Minor fix dispatched: declare sources after engine and host so they die first. Repeated 'no profile named' log judged not worth fixing (rare, user-driven).
- Task 3: fix round 1 committed 2a204048 (sources declared last → destroyed first; comment corrected). Controller checked construction: host and engine are null unique_ptrs created later (ContourGuiApp.cpp:2299), refresh null-checks the host — safe. Approved.
- Task 3: complete (1d4f64e5, 2a204048).
- Task 4: BASE 2a204048.
- Task 4: implementer dispatched (sonnet).
- Task 4: implementer DONE f5aae0aa (lint 10/10; self-tests colour 21, sizes 31; baseline 32 files/367 literals). Reviewed inline by controller: code_lines moved verbatim into qml_lint.py; exact-count baseline (more or fewer fails) so it only shrinks. Parked for Task 12 /simplify: `# noqa: E402` on the sys.path import (suppression comment); multi-line values count first line only.
- Task 4: complete (f5aae0aa).
- Task 5: BASE f5aae0aa.
- Task 5: implementer dispatched (sonnet).
- Task 5: implementer DONE ddca983a ([banner] 2, [banner],[a11y] 35, [qml] 127, lint 10/10). Controller ran [banner] under CONTOUR_TEST_CONTROLS_STYLE=ContourTui: 65 assertions pass. Reviewed inline (Control, token padding, explicit implicit sizes, no literals, signals only, AlertMessage role). Approved.
- Task 5: complete (ddca983a).
- Task 6: BASE ddca983a.
- Task 6: implementer dispatched (sonnet).
- Task 6: implementer DONE ff74857b ([toast],[a11y],[snapshot],[qml] pass, also under ContourTui; lint 10/10; 7 scripts → 112 variants, 28 under ContourTui; counts derived from tables). Reviewed inline: Toast.qml presentational, host positions it; the one-time centring is only in the snapshot scene. Approved.
- Task 6: complete (ff74857b).
- Task 7: BASE ff74857b.
- Task 7: implementer dispatched (sonnet).
- Task 7: implementer DONE 2a009bed ([dialog],[a11y],[snapshot],[qml] pass, also ContourTui; lint 10/10; 8 scripts → 128 variants, 32 ContourTui). Reviewed inline: modal, CloseOnEscape, Enter accepts, placement from token (sheet vs centred, R20); a11y role on contentItem because Popup is not an Item. Approved.
- Task 7: complete (2a009bed).
- Task 8: BASE 2a009bed.
- Task 8: implementer dispatched (sonnet).
- Task 8: implementer DONE d7535561 ([hosting] 4 cases, also ContourTui; [mainwindow],[snapshot],[qml] 138+1 skip; lint 10/10; Main.qml size baseline 6→1). Reviewed inline: content anchors to bannerStack.bottom (same position when empty, both tab-bar positions); Banner replaces the overlay. Parked for Task 12: chromeStyle.hostingNoticeFillAlpha/BorderAlpha (Phase 0) now unused — remove.
- Task 8: complete (d7535561).
- Task 9: BASE d7535561.
- Task 9: implementer dispatched (opus).
- Task 9: implementer DONE 480419b9 ([motion]… 23 cases; 15; lint 10/10). Also writes factorySettings for RIS consistency. Review dispatched (opus, thread safety focus).
- Task 9: review Spec ✅ Quality Approved — no race (writes under std::scoped_lock{_terminal}, readers under the same lock), effective-only, restore verified, RIS doesn't read these fields. Minors parked for Task 12: factorySettings write untested; in-flight animation keeps copied duration (≤250 ms).
- Task 9: complete (480419b9).
- Task 10: BASE 480419b9.
- Task 10: implementer dispatched (opus).
- Task 10: implementer DONE 06e0a550 (vtbackend [background-event] 2 cases; gui [chromefollow],[manager][theme] 4; [theme],[chromehost],[appearance] 6; lint 10/10). Parser-thread → postToManager (queued) → GUI. No TSan run. Review dispatched (opus).
- Task 10: review Spec ✅ Quality Needs fixes (Medium: applyGuiTheme under any theme → needless full-app repaints; Lows: native frame scheme, reload ignores live bg, noexcept path, missing non-terminal test). Thread safety sound. Fix round 1 dispatched.
- Task 10: fix round 1 committed f786d35a (followLiveBackground: Terminal-theme-only + scheme-change gate; _guiPaletteOverridden bool → optional<Qt::ColorScheme> _appliedGuiScheme; native frame reads it; reload passes live bg; noexcept dropped on setColorPalette/resetColorPalette, documented). 2 new tests fail without the gate. Accepted inline.
- Task 10: complete (06e0a550, f786d35a). Phase 0 follow-up "chrome does not follow OSC 11" resolved.
- Task 11: BASE f786d35a.
- Task 11: implementer dispatched (opus).
- Task 11: implementer DONE 8167cb11 (contour_test 14+46; gui 26+1skip, 142+1skip; lint 10/10; ContourTui same; 10 scripts → 160, 40 ContourTui). Concerns: Keep Mine over malformed gui-state.yml overwrites it; error paths untested; no clang-tidy. Review dispatched (opus).
- Task 11: review Spec ✅ Quality Needs fixes (Important: Use New ignores gui_config_locked; malformed settings.yml could be replaced by one key. Minors: gui-state round-trip/.bak, keep-other-keys test, error-path fakes). Controller added: GuiStateStore reads via std::ifstream — match sibling stores / inject FileSystem. Fix round 1 dispatched.
- Task 11: fix round 1 committed 200d761d (locked → no offer; Config::guiSettingsFileHealth Usable/Unparsable → no offer; gui-state .bak + unknown-key round-trip; other settings keys preserved; failing-store fake). Sibling stores use plain streams; GuiStateStore interface is the seam. Accepted inline.
- Task 11: complete (8167cb11, 200d761d).
- Task 12: BASE 200d761d.
- Task 12: Steps 1–3 dispatched (opus): /simplify + parked items, all filters, 160 snapshots on display. Steps 4–5 (release notes, index, PR) by controller after.
- Task 12: Steps 1–3 done ddf32200 (simplify; noqa removed; factorySettings tested; size-gate limit documented). Filters: contour_test 31, gui 228+1 skip, vtbackend 2, lint 10/10. 160 snapshots rendered on display.
- Defect found in snapshots: hosting-banner.terminal.native2026.light — dark bar, message invisible, Dismiss dark-on-dark. Banner buttons take Qt palette (real-app risk under theme: light + dark scheme); scene feeds dark scheme for light appearance. Fix dispatched (opus) with a contrast test.
- Defect fixed 159b47dc: root cause contour-light palette[0] == foreground (1:1); new SchemeSlot::BarText picks the higher-contrast of fg/bg; Banner buttons take token text colour. Contrast test fails before (1.0, 1.5), passes after. Controller viewed fixed light PNG: legible.
- Task 12 Step 4–5: release notes + index done 89c21127, pushed. Final whole-step review dispatched (opus).
- Final review (opus): Ready: With fixes. Important: banner stack keeps old height after both banners dismissed in a certain order; Windows DWM frame doesn't re-apply on scheme change. Minor: chromeHeight ignores banners; OSC 11 note overclaims (single scheme only; focus move also flips); Toast/KitDialog Terminal-row contrast untested. One fix dispatch (opus).
- Final fixes d6f91bd6 (banner stack sums banners + order test; Win32 frame re-applies on ApplicationPaletteChange — Windows not compiled here; chromeHeight includes banners; note reworded; KitDialog labels fixed after contrast test caught 1.71:1). 78 cases ×2 styles; lint 10/10. Step 1a complete.
