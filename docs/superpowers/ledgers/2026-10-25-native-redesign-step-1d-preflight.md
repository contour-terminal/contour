# Pre-flight scan — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-1d.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.5 (`:446-468`, binds). Scanned
against HEAD 2982edf0 on `feature/native-redesign-phase0`. The plan's line numbers are at 8a8aa716. Since then, Step 4a
and Step 1c's Tasks 1–4 have landed (7b1eea05, 30cdc3f0, 6f72f082, 24799670, 07d3fbbe, 23dab8a1). 1c Task 5 is being
implemented in this tree. At scan time it had no uncommitted tracked edits; its files are listed under **F20**.

The rulings D1–D4 and S1–S5 (in `progress.md`, recorded in `spike-decisions.md`) are settled, and this scan does not
reopen them. Where a finding touches one, it says so and stays inside that ruling's stated cost-if-wrong.

## Pre-flight scan

### 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → all | `spike.md`; rulings S1–S5 | Tasks 2, 4, 5, 7, 8 "cite `spike.md`" | The record is **`spike-decisions.md`**, not `spike.md` (**F6**). Every value the plan needs is in it. See §4. |
| 2 → 3 | `config::BandBackdrop`, `config::TitleBarGrowth` (in `ChromeTokenGroups.hpp`, beside `Material`, `:20-26`) | `TitleBandRequest` | Consistent. |
| 2 → 6 | `TitleBandSurfaceInputs`/`resolveTitleBandSurface`, `TitleBandTokens`/`TitleBandGeometryInputs`/`resolveTitleBandGeometry` | `titleBandRequest()`, `titleBandGeometry()` | Names match. **F10**: the device-pixel snap changes the Flat rows' bands. **F12**: `constexpr` over `std::ceil` and the non-`constexpr` `relativeLuminance`. Under S3, `controlsCentreY` is dead data (**F6**). |
| 2 → 7 | `legibleBandTint`, `toneOf`, `TitleBandDarkLuminance` | `UiStyleProvider::bandFill` | Consistent. `platform::DarkBackgroundLuminance` is at `GuiTheme.hpp:20`, as the plan says. The `static_assert` works. |
| 3 → 5 | `ITitleBandMaterial`, `NullTitleBandMaterial`, a temporary Apple `makeTitleBandMaterial` | `CocoaTitleBandMaterial.mm` replaces the temporary factory | **F1 (blocking)**: the factory runs before a `QApplication` exists, so the `platformName()` check makes it null in production. |
| 3 → 6 | `ContourGuiApp::titleBandMaterial()`, `TitleBandRecord`, the three-switch `appearanceServicesWith` | `TitleBandController_test` | **F2 (blocking)**: a `TestApp` never has the MacOS row or a chrome host, so the macOS cases cannot pass. **F16**: the member's lifetime. **F17**: the two-switch overload cannot "call it with no record". |
| 4 → 5 | Branch A: `ClientAreaExpansion::QtHint`, `needsExpandedClientArea`, the hint in Main.qml's `flags`, `apply` without the style-mask lines | the material's tests set "the hint flags as the controller sets them"; `metrics().height = safeAreaMargins().top` | Consistent under S1. **F14**: the safe area of an `ApplicationWindow`. **F5 (blocking)**: the configure-time Qt floor. |
| 4 → 6 | `needsExpandedClientArea` | `clearsTitleBarBackground` | **F4 (blocking)**: `clearsTitleBarBackground` is true under `show_title_bar: true` and on every Linux and Windows window. |
| 5 → 6 | `apply`, `metrics` (with `fullScreenTop`) | `titleBandGeometry()` → `titleBandRevealTop` | **F3 (blocking)**: `fullScreenTop` is measured from the screen, but the band is placed in window coordinates. **F9**: the toolbar moves the inset from 78 to 84 pt, and nothing re-emits `nativeControlsInsetChanged`. |
| 6 → 7 | `titleBandHeight`, `titleBandRevealTop` (+ the mock's copies) | `TitleBar.qml`/Main.qml bindings | Consistent once 1c Task 7 lands (**F20**). **F19**: test palettes. |
| 2 → 8 | `TerminalRowTrafficLightFit = GrowBandToCells` | Task 8's QML case | Consistent. Under S3, Task 8's AppKit half and its `controlsCentreY` case go (§4). |
| 1–8 → 9 | the filters; the snapshot count; the release notes | Finish | The union covers every tag. Under S3/S4 the release-note, risk and snapshot texts are stale (**F6**). There is no clang-tidy step (**F18**). |
| 1d ↔ 1c (landed: T1–T4) | `WindowFrame`/`WindowMode` (`TitleBandLayout.hpp:22-34`); `UiStyleProvider::bandHeight` (`.hpp:190`, `.cpp:339-347`); `snapToCells` | Tasks 2, 6, 7, 8 | Names match. The band token is **`bandHeight`**: 38 on MacOS, and `chromeHeight` (34, or one cell) on Flat rows. It is not `size.titleBar` (**F7**). |
| 1d ↔ 1c (pending: T5, T7) | `bindWindow`/`onWindowStateChanged` (T5); `TitleBar.presence/centre/revealed/slideOffset`, `band`, `bandRevealEdge`, `tabsBar`, the snapshot `.frame/.mode` fields (T7) | Tasks 4, 6, 7, 8 | Not in the code yet. **F20** gives the gates. |

### 2. Per-task self-consistency

| Task | Agrees with itself? | Detail |
|---|---|---|
| 1 | Done | The spike ran (`spike-decisions.md`): 26 + 2 runs, and no decision took its default. Nothing is committed. |
| 2 | Mostly | Every arithmetic expectation recomputes. `bandTintOf(0x000000) = 0x0A0A0A` (0.04·255 = 10.2 → 10), `0xFFFFFF → 0xF4F4F4` (244.8 → 244), `0x1E1E1E → 0x282828` (40.2 → 40). `{tui, Windowed, 2, 28}` under GrowBandToCells gives 17·⌈28/17⌉ = 34. The bundled schemes' backgrounds (`ColorPalette.cpp:61-393`: `0x1C1E24`, `0xFAFAF8`, `0x272822`, `0xF8F8F8`, …, `0x1C1C1C`) are all far from luminance 0.18, so `tint.values`' "same tone" and `tint.legibility`'s ≥ 4.5 hold. `iota` over `Material` needs a count: there is no `MaterialCount`. Derive one beside the enum, as `BarCentreCount` is derived (`TitleBandLayout.hpp:72`). `ChromeStyleCount` exists (`config::detail::ChromeStyleCount`). **F6**, **F10**, **F12**. |
| 3 | Mostly | The seam fits `AppearanceServices` (`:17-29`; every literal: `nullAppearanceServices()` `:33-50`, `appearanceServicesWith()` `AppearanceFixtures.hpp:102-126`; `main.cpp:234` uses `AppearanceServices {}`, which takes the default). **F1**, **F16**, **F17**. `TitleBandMaterial_test.cpp` goes beside `platform/WindowResizeGrid_test.cpp`, now at `CMakeLists.txt:833`. |
| 4 | Yes (Branch A) | `FramePolicy` (`NativeWindowFrame.hpp:80-88`) has no default member initializers. Its literals are `ServerFramePolicy` and `ClientFramePolicy` (`:93-102`), the two in `framePolicyFor` (`:128-141`), and two in `NativeWindowFrame_test.cpp:83`/`:106`. Each one must name `.expansion`. **F5**, **F14**, **F15**. |
| 5 | Mostly | Step 1 runs: under S1 it does nothing. Step 3 attaches the toolbar (S2). Step 4 never runs (S3). The "≥ 37.5" case runs. **F1**, **F3**, **F9**, **F13**. |
| 6 | **No** | **F2** (blocking), **F4** (blocking), **F9**, **F10**, **F11**. Line numbers: the constructor's lambda is at `WindowController.cpp:79-84` (the plan has `:80-85`), `setTitleBarVisible` at `:997`, `eventFilter` at `:1378-1432`, `onWindowScaleMaybeChanged` at `:1434`, `onWindowStateChanged` at `:1249` and `bindWindow` at `:1145`. |
| 7 | Mostly | Waits on 1c Task 7 (**F20**). The `[band]` tag is 1c Task 7's. `ChromeSetup::accessibility` exists (`QmlChromeStyle.hpp:32`), so `{ .transparency = Reduced }` can be expressed. **F8** (`bandFill` signature), **F19**. |
| 8 | Collapses | Under S3 it is the QML ContourTui case and the ledger note. The AppKit step, its `[cocoa][titleband][trafficlights]` cases, and the "request carries the geometry's centre" case go (§4). The commit message "sit on its first cell row" is wrong under S3. |
| 9 | Mostly | The release notes are at `metainfo.xml:107` (0.9.0, development), which is right. Fix the texts for S3/S4 (**F6**). Add clang-tidy (**F18**). |

### 3. References verified against HEAD 2982edf0

Present and correct:
- `CocoaWindowFrame.mm`: `:44-65` (`apply`), `:48-50` and `:58-60` (the mask, transparency and title lines), `:62-64`
  (the shadow warning), `:81-97` (`nativeControlsInset`), `:106` (`platformName`).
- `CocoaWindowFrame.hpp:11-18` (the class comment).
- `display/ShaderConfig.cpp:30` (`setAlphaBufferSize(8)`); `Main.qml:38` (`flags`), `:43` (`color: "transparent"`).
- `CocoaNativeMenuPresenter.mm:174` (the `__bridge` cast).
- `platform/GuiTheme.hpp:20` (`DarkBackgroundLuminance`).
- `vtbackend/core/Color.hpp`:
  - `relativeLuminance` `:109` and `contrastRatio` `:116`, both **non-`constexpr`**, defined out of line;
  - `hslToRgb` `:170` (`constexpr`, truncating).
- `AppearanceServices.hpp:17-50`.
- `ContourGuiApp.cpp`:
  - `chromeInputs` `:1795-1805`, `refreshChromeStyle` `:1807-1813`, the services in the init list `:451-460`;
  - `appliedGuiScheme` (`ContourGuiApp.hpp:525`).
- `display/DisplayRendering_test.cpp:90-98` (`REQUIRE_DISPLAY_OR_SKIP`, which checks `displayTestsEnabled` only, not
  `platformName`; see **F18**).
- `.gitignore:22` (`out`); `metainfo.xml:107`; `scripts/qml-size-literal-baseline.txt`, `check-qml-colors.py`,
  `check-qml-sizes.py`.
- Tools: `/opt/homebrew/opt/llvm@22/bin/clang-format`, `…/clang-tidy`, `/opt/homebrew/bin/editorconfig-checker`;
  `out/appleclang-debug/compile_commands.json` (1030 entries, `.mm` included; `ENABLE_TIDY=OFF`, so the build never runs
  clang-tidy).
- `TerminalSessionManager`: `controllerFor` `.hpp:754`, `_controllersByWindow` `.hpp:1175`
  (`unordered_map<uint64_t, WindowController*>`).
- `ScopedControllerWithOsWindow` (`GuiTestFixtures.hpp:866`); `displayTestsEnabled` (`test/TestEnvironment.hpp:20`).
- `currentFramePlatform()` (`NativeWindowFrame.hpp:26-35`, `constexpr`: MacOS on any Apple host, offscreen included).
- Tags present: `[contour][frame]`, `[appearance-source]`, `[manager]`, `[contour][multiwindow]`,
  `[contour][gui][qml][mainwindow]`, `[snapshot]`, `[display][snapshot]`, `[contour][uistyle]`, `[chrometokens]`,
  `[titleband]` (1c), `[contour][app][appearance]`, `[cocoa]`.
- Tags created: `[titlebandsurface]`, `[titlebandgeometry]`, `[titleband-material]`, `[cocoa][frame]`,
  `[cocoa][titleband]`, `[contour][titleband][controller]`. `[…][mainwindow][band]` is 1c Task 7's.
- What 1c Tasks 1–4 provide, against what the plan expected:
  - `WindowFrame {ClientBand=0, NativeFrame}` and `WindowMode {Windowed=0, FullScreen}` (`TitleBandLayout.hpp:22-34`),
    as expected.
  - `TitleBandInputs`, `TitleBand`, `TitleBandRules`, `resolveTitleBand` (`:75-252`), and
    `UiStyleProvider::titleBand(int frame, int mode, int strip, int position)` (`.hpp:371`), as expected.
  - **`bandHeight`** is a `UiStyleProvider` property (`.hpp:190`, `.cpp:339-347`), not a token field: `size.titleBar` on
    `Pill`, else `chromeHeight`. The plan expected "1c's `size.titleBar` = 38" (**F7**).
  - `size.titleBar` is `SizeTokens::titleBar` (`ChromeTokenGroups.hpp:199`): 38/40/46/1.
  - `snapToCells` (`.hpp:381`) and `cellHeight()` (`.hpp:291`) exist. `ChromeStyleHost::provider()`/`inputs()`
    (`ChromeStyleHost.hpp:39-42`) exist.
  - 1c did **not** add a tone enum (the Task 4 report says so, to leave `ChromeTone` to 1d). There is no collision.

Stale (locate by name; the line given is the current one):
- `ChromeTokenGroups.hpp`:
  - `Material` `:20-26` (the plan has `:19-26`);
  - the MacOS `material.titleBand` row `:587` (`:346`; under S4 it is not edited);
  - the Reduce Transparency step in `resolveChromeTokens` `:824-828` (`:450-455`).
- `platform/CMakeLists.txt`:
  - `add_library(contour_platform …)` `:15-72` (`:15-69`);
  - `if(APPLE)` `:84-103` (`:81-96`). `target_sources` is `:88-95` and the ARC list `:96-99`. That list does **not**
    contain `CocoaWindowFrame.mm` or `CocoaWindowResizeGrid.mm`: both compile without ARC (**F18**).
- `src/contour/CMakeLists.txt`:
  - `contour_gui_test` `:769-834`, its `if(APPLE)` `:836-850` (ARC list `:846-849`);
  - the heavy list `:867-891`;
  - `contour_test` `:1153-1207` (`TitleBandLayout_test` `:1204`);
  - `CONTOUR_QT_VALIDATED_VERSION` `:84`, which is **outside** `if(CONTOUR_FRONTEND_GUI)` (`:46-69`) (**F5**).
- `NativeWindowFrame.hpp`: `FramePolicy` `:80-88`, `framePolicyFor` `:117-145` (the plan has `:79-146`).
- `WindowController.hpp`:
  - `needsFramelessHint` `:116` (as the plan says) and `:461`;
  - `nativeControlsInset` `:121`/`:477`, `nativeControlsInsetChanged` `:770`;
  - `_osWindow` `:986`, `_nativeFrame` `:1013`.
- `WindowController.cpp`:
  - `needsFramelessHint` `:977-980` (`:1034-1037`), `framePolicy` `:972`, `setTitleBarVisible` `:997` (`:1054`);
  - `eventFilter` `:1378-1432` (`:1434-1478`): Expose `:1393-1415`, `ApplicationPaletteChange` `:1417-1423`;
  - `onWindowScaleMaybeChanged` `:1434` (`:1494`).
- `ContourGuiApp.hpp`: `_nativeMenus` `:1052`, `_menuBarIntegration` `:1059` (the plan's `:1011` is `_chromeStyle`).
  `ContourGuiApp.cpp:457`/`:460` (the init list).
- `test/AppearanceFixtures.hpp`: `appearanceServicesWith` `:102-126` (`:101-127`).
- `UiStyleProvider.cpp`: `statusColor` and its light decision `:349-364` (`:243-259`).
- `TitleBar.qml`: the background `Rectangle` `:59-62` (`:58-61`).

### 4. The spike's selected branches, applied (the one path for each implementer)

S1 = Hint, S2 = CompactToolbar, S3 = GrowBandToCells, S4 = Glass with fill Tint, S5 = `max(menuBarHeight,
safeAreaInsets.top)`. Delete the other branch's text from each brief.

| Where | Two branches in the plan | Follow (S1–S5) | Delete |
|---|---|---|---|
| Task 2, Interfaces table | MacOS/Glass fill "D4's (`Clear` or `Tint`)", growth "D2" | `{ MacOS, Glass } → { NativeMaterial, Tint, CompactToolbar }`; `{ MacOS, Solid } → { Client, Tint, CompactToolbar }` | "If D4 = `Solid`, the MacOS row's token … becomes `Solid`" and Step 3's matching `ChromeTokenGroups_test` edit. `ChromeTokenGroups.hpp` gets the three enums only. |
| Task 2, `TerminalRowTrafficLightFit` | `/* D3 */` | `= TrafficLightFit::GrowBandToCells;`, citing `spike-decisions.md` §D3 | — |
| Task 2, `geometry.terminalRow` | three alternative expectations | `{ tui, Windowed, 2, 28 }` → `{ 34, 0, nullopt }`; with native 0 → `{ 17 }` | the SnapToFirstRow and AppKit lines. Under S3, `controlsCentreY` and `SnapToFirstRow` are dead (§D3: "no `controlsCentreY` and no observers"). Proposed in **F6**: drop both. |
| Task 2, `surface.macRows` | `<D4 fill>`, `<D2>` | `Tint`, `CompactToolbar` | — |
| Task 4 | Branch A / Branch B | **Branch A** in full. `apply` keeps `titleVisibility = NSWindowTitleHidden` in the client path (H1 found it Visible), keeps `NSWindowTitleVisible` in the server path, and keeps `hasShadow = YES`. `:48-49`/`:58-59` go. Add the configure floor (placed per **F5**) and the `#error`. | Branch B's interfaces, its class-comment text, and its test ("the frame leaves the title bar's transparency to the material"). |
| Task 5, `apply` step 1 | Branch B transparency / Branch A nothing | **Nothing.** The controller sets `Qt::NoTitleBarBackgroundHint` (Task 6, through **F4**'s expression). | the Branch B line |
| Task 5, step 3 | `growth == CompactToolbar` | Attach, once per window, the empty compact `NSToolbar`. `None` detaches it (Step 2's P3 relies on the detach, **F8**). | — |
| Task 5, step 4 | `controlsCentreY` "(Task 8; until then ignored)" | Absent (S3; see **F6**). | step 4 |
| Task 5, `metrics` | Branch A / Branch B height | `height = safeAreaMargins().top`, behind `QT_VERSION_CHECK(6, 9, 0)` with an `#error` in the `#else`. `fullScreenTop` per S5, made window-relative per **F3**. | `NSHeight(frame) - NSHeight(contentLayoutRect)` as the source. The test keeps it as the cross-check, which the spike showed is equal. |
| Task 5, tests | "with `growth = CompactToolbar` (only if D2 chose it)" | It runs. `metrics(window).height ≥ 37.5`; the spike's value is 40. | — |
| Task 6, Branch A items | `safeAreaMarginsChanged` connection, `clearsTitleBarBackground` | Both, with **F4**'s expression and **F15**'s guard | — |
| Task 6, Reduce Transparency case | "(or `Client` both times if D4 = `Solid`)" | `NativeMaterial` → `Client` → `NativeMaterial` | the D4 = Solid parenthesis |
| Task 7, `bandFill` case | "transparent if D4's fill is `Clear`, else the tint" | **The tint** (`legibleBandTint`), with **F19**'s palette setup | the `Clear` alternative |
| Task 8 | SnapToFirstRow / GrowBandToCells / AppKit | **GrowBandToCells**: no AppKit change. Keep only the ContourTui QML case "the band grows to the title bar in whole cells", plus the ledger line. The commit becomes "contour: the Terminal row's macOS band grows to whole cells around the traffic lights". | the SnapToFirstRow interfaces and tests; the "request carries the geometry's centre" case (both sides are `nullopt` by construction); the "If D3 = AppKit" line |
| Task 9, step 3 | `single-tab.macos.*` "a clear band if D4's fill is `Clear`" | The band is the tint under AppKit's material, and the picture cannot show the material. | the Clear text |
| Task 9, step 5 | the Solid rewording; the D3 line | Keep the Glass wording. For D3 write: "Grows the tab row under `ui_style: terminal` on macOS to whole cells, so the window buttons sit inside it." | the Solid rewording |
| Task 9, step 6 Risk | "AppKit's buttons may be moved (D3)" | "the Terminal row's band grows to two cells on macOS (D3)" | — |
| Goal (`:10-13`), Architecture | "traffic lights snapped to its first cell row", "the later tasks state both branches" | Rewrite to the GrowBandToCells outcome, with a note that the branches were resolved by S1–S4. | — |

### 5. Interfaces 1d produces that Step 2 consumes (by Step 2's plan's names and its accepted scan rulings P1–P5)

| Step 2 expects | 1d's plan gives | Status |
|---|---|---|
| `config::BandFill` (Task 3 appends `Background`) | `BandFill { Palette = 0, Clear, Tint }` in `ChromeTokenGroups.hpp` | Matches. An exhaustive `switch` in `bandFill` takes the new enumerator. |
| `bandTintOf`, `toneOf` (Task 3's `colorOn` resolves by "1d's `toneOf`") | both in `window/TitleBandSurface.hpp` | Names match. **Gate mismatch (F8)**: this is a **compile** dependency of Step 2 Task 3 on 1d Task 2. The Step 2 ledger's last ruling ("only ordering on rows, not a compile dependency") is wrong on this point. On the other hand, under S4 1d Task 2 edits no token row, so the feared row conflict cannot happen. |
| `TitleBandSurfaceInputs` (+ `centre`, `density`), `TitleBandSurfaceRows`, `resolveTitleBandSurface` (Task 7) | the same | Matches. |
| `UiStyleProvider::bandFill(frame, mode, centre)` (Step 2 scan F16, accepted by P5; "Main.qml passes `band.centre`") | `bandFill(int frame, int mode)` | **Mismatch (F8)**: adopt the three-argument form in 1d Task 7 now. |
| `WindowController::refreshTitleBand`, `titleBandHeight` (Task 7 picks `tokens.height` by centre) | the tokens come from `ContourGuiApp::titleBandTokens()` inside `titleBandGeometry()` | Workable. Step 2 edits `titleBandGeometry()`'s inputs, not `refreshTitleBand`. Say so in Step 2 Task 7 (**F8**). |
| Seam S1: `TerminalSessionManager::refreshChromeDependents()`, created by 1d T6 with `refreshTitleBands()` inside | `refreshTitleBands()` called directly from `refreshChromeStyle()` | **Mismatch (F8)**. |
| Seam S2: `UiStyleProvider::resolvedRow()` (`ChromeTokenRow const&`) | `titleBandTokens()` reads "the resolved `material.titleBand`" with no accessor | **Mismatch (F8)**: add `resolvedRow()` (returning `_groups`, `.hpp:494`) in Task 6. |
| P3: on macOS, Minimal's bare band is 32 pt without the compact toolbar | `growth == None` detaches the toolbar; the geometry is `max(token, native)` | Matches, if growth can change live and the inset follows it (**F9**: 84 → 78 pt). |
| `band-solid` snapshot row and `.transparency` (Task 12) | Task 7 | Matches. |
| `MockMainController::titleBandHeight`/`titleBandRevealTop` with setters | Task 7 | Matches. |

## Findings needing a ruling

**Blocking** (a task's steps, the build, CI, or the shipped behaviour fail as written): **F1–F5**. **Non-blocking:**
**F6–F20**.

1. **F1 (blocking): the Cocoa factory runs before a `QApplication` exists, so production always gets the null material
   (Tasks 3, 5).**
   - **Plan text:** Task 5: "`makeTitleBandMaterial()` on Apple: a `CocoaTitleBandMaterial` when `platformName() ==
     "cocoa"`, else `NullTitleBandMaterial`". Task 3: "built once from `appearanceServices.titleBand()` in the
     member-initializer list after `_menuBarIntegration`". Global Constraints: "Every Cocoa factory returns the null
     object unless `platformName() == "cocoa"` (`CocoaWindowFrame.mm:106`)".
   - **Why it fails:**
     - `ContourGuiApp` is constructed in `main.cpp` (`:234`). The `QApplication` is created much later, inside
       `terminalGuiAction()` (`ContourGuiApp.cpp:2244`).
     - `QGuiApplication::platformName()` is empty before then, so the factory always returns the null object, and the
       shipped app never draws the material. Every offscreen test still passes.
     - `CocoaWindowFrame.mm:106` can check at factory time only because `makeNativeWindowFrame` runs in
       `WindowController`'s constructor, after the `QApplication` exists. The services-built Cocoa adapters (menus
       `CocoaNativeMenuPresenter.mm:166`, taskbar `CocoaTaskbarProgressSink.mm:82`) check **at call time** instead.
   - **Proposed:** on Apple, `makeTitleBandMaterial()` returns the `CocoaTitleBandMaterial` unconditionally. Its `apply`
     and `metrics` return early unless `platformName() == "cocoa"`, as `CocoaNativeMenuPresenter::present` does. Task
     3's test "the native factory is null off the Cocoa plugin" stays true by value (offscreen, `metrics == {}`). Correct
     the Global Constraints line: the check is made at call time for services built in `ContourGuiApp`'s constructor.

2. **F2 (blocking, needs a ruling): Task 6's macOS cases cannot be reached through a `TestApp` (Task 6).**
   - **Plan text:** `titleBandTokens()`: "From the published provider when `_chromeStyleHost` exists … Without a host:
     `{ .style = _chromeStyle, .material = Material::None }`." The test: "with the GUI's chrome style host built (as
     `[contour][app][appearance]` cases do), `accessibility.set({ .transparency = Reduced })` makes the last request's
     backdrop `Client`; `set({})` makes it `NativeMaterial` again".
   - **Why it fails:**
     - No `[contour][app][appearance]` case builds the host. `ContourGuiApp_test.cpp:425` says "Before the GUI exists
       there is no chromeStyle to rebuild".
     - `_chromeStyleHost` is created only in `terminalGuiAction()` (`:2456`), which a `TestApp` never runs.
     - `_chromeStyle` defaults to `ContourLinux` (`.hpp:1011`) and is resolved only at `:2330`, also in
       `terminalGuiAction()`.
     - So a `TestApp` on macOS always resolves `{ContourLinux, None}` → `TitleBandSurface {}` → `Client`. The cases
       "switches … back to `NativeMaterial`" and "the band is at least the token tall" fail, and the scale/full-screen
       cases pass vacuously.
     - Also, `refreshChromeStyle()` is guarded by `if (_chromeStyleHost)` (`:1811`). A `refreshTitleBands()` placed
       "after the host's update" inside that guard never runs in a test, nor before the GUI exists.
   - **Proposed (rule one):**
     - **(a) Recommended.** Resolve `_chromeStyle` in the member-initializer list from `_config.uiStyle` and
       `_hostPlatform`. Both are known at construction, so this is configuration. The `QQuickStyle` calls stay at
       `:2331-2332`.
       - `titleBandTokens()` then reads `config::resolveChromeTokens(_chromeStyle, accessibilityPreferences())` with no
         host needed. The height is `size.titleBar` on `Pill` and the provider's `bandHeight`/`cellHeight` when the
         host exists. Without a host, the Terminal row gets height 0 and `cellHeight` 0, so the geometry falls back.
       - Call `refreshTitleBands()` outside the host guard.
       - Cost: `menuPresentation()` (`:1792`) also reads `_chromeStyle`, so a macOS `TestApp` becomes the MacOS row
         there. Run `[menu]`, `[contour][app]` and `[contour][multiwindow]` on macOS and Linux to see what moves.
     - **(b)** A narrower test seam. `TestApp` takes an optional `ChromeStyle`, passed to `ContourGuiApp`'s constructor
       as configuration, and `titleBandTokens()` resolves as in (a).
     - **(c)** Move the macOS expectations into a pure helper `titleBandTokensFor(ChromeStyle, AccessibilityPreferences,
       qreal bandHeight, qreal cellHeight)` tested in `contour_test`. The controller cases then only pin "applies once"
       and "show_title_bar applies nothing". This loses the liveness proof Review Focus 2 asks for.

3. **F3 (blocking): `fullScreenTop` is measured in screen coordinates, but the band is placed in window coordinates
   (Tasks 2, 5, 7).**
   - **Plan text:** `NativeTitleBarMetrics::fullScreenTop`: "In full screen, where a revealed title bar's top sits below
     the screen's top." Task 7: "the revealed band's resting `y` is `appWindow.win.titleBandRevealTop`". The test
     expects "`metrics(window).fullScreenTop > 0`".
   - **Why it fails:**
     - The spike measured the revealed `NSToolbarFullScreenWindow` at 29 pt below the screen's top. It also recorded:
       "In full screen the `QNSWindow` itself starts at 29 under `hint*`" (`spike-decisions.md` §Full-screen reveal).
     - On this notched Mac, a full-screen window sits below the camera housing, so in **window** coordinates the
       revealed title bar's top is 0. The plan would put the band 29 pt below AppKit's revealed traffic lights.
     - On an external display without a notch, the window starts at 0, the menu bar overlays it, and the window-relative
       offset is the full 24 pt.
     - S5's formula is correct for the screen offset. The plan's mistake is the frame of reference, so S5 is not
       reopened.
   - **Proposed:**
     - `metrics().fullScreenTop = max(0, max(menuBarHeight, screen.safeAreaInsets.top) − (NSMaxY(screen.frame) −
       NSMaxY(window.frame)))`, documented as window-relative. On this Mac that is 0.
     - The Task 5 test asserts `fullScreenTop >= 0` and equals the revealed full-screen window's top minus the window's
       top, read from `NSApp.windows` as the spike did. It does not assert `> 0`.
     - Task 7's mock value of 24 stays as an arbitrary test value.
     - Add to the owner checks: full screen on the built-in display, and on an external one if available.

4. **F4 (blocking): `clearsTitleBarBackground` makes the native title bar transparent under `show_title_bar: true`, and
   sets the hint on Linux and Windows (Task 6, Branch A).**
   - **Plan text:** "`Q_PROPERTY(bool clearsTitleBarBackground …)`, true when the request's backdrop is `Client`;
     Main.qml's `flags` gains `| ((win && win.clearsTitleBarBackground) ? Qt.NoTitleBarBackgroundHint : 0)`."
   - **Why it fails:**
     - Under `NativeFrame`, `resolveTitleBandSurface` returns `TitleBandSurface {}`, whose backdrop is `Client`. So
       `show_title_bar: true` on macOS sets `NoTitleBarBackgroundHint`, which H2 showed sets
       `titlebarAppearsTransparent = YES`, on an ordinary titled window. That breaks "an ordinary title bar, no material"
       and Task 4's "native title bar is an ordinary one".
     - The same holds for every Linux and Windows window, whose surface is always `{}`, so Qt's Windows plugin would
       receive the hint too ("Linux and Windows unchanged").
     - No test catches it: Task 6 checks only `record.applied`, and Task 4 applies only the frame.
   - **Proposed:** `clearsTitleBarBackground() = needsExpandedClientArea() && titleBandRequest().backdrop ==
     BandBackdrop::Client`. Add a controller case: under `setTitleBarVisible(true)`, and on a non-macOS host,
     `clearsTitleBarBackground()` is false.

5. **F5 (blocking): the Qt ≥ 6.9 floor "beside `CONTOUR_QT_VALIDATED_VERSION`" fails the headless macOS CI job (Task 4,
   D1).**
   - **Plan text:** "`src/contour/CMakeLists.txt` (beside `CONTOUR_QT_VALIDATED_VERSION`)" … "`if(APPLE AND Qt6_VERSION
     VERSION_LESS 6.9)` `message(FATAL_ERROR …)`".
   - **Why it fails:**
     - `CONTOUR_QT_VALIDATED_VERSION` is set at `:84`, outside `if(CONTOUR_FRONTEND_GUI)` (`:46-69`).
     - Without the GUI, Qt is not looked for at all (`:40-45`), so `Qt6_VERSION` is empty. CMake treats an empty value as
       `VERSION_LESS 6.9`.
     - `build_headless_macos` (`build.yml:322-360`, `-D CONTOUR_FRONTEND_GUI=OFF`) would stop at configure.
   - **Proposed:** put the check inside `if(CONTOUR_FRONTEND_GUI)`, right after `find_package(Qt6 COMPONENTS
     ${QT_COMPONENTS} REQUIRED)` (`:68`). D1 holds against CI: every macOS GUI job uses Qt 6.11.1 (`build.yml:136/142`,
     `:731-738`), and the latency job and the local tree use Homebrew 6.11. Windows (6.10.*) and Linux (6.11.1) are not
     gated.

6. **F6: texts the spike settled, and the record's name (Tasks 1–9).**
   - The record is `spike-decisions.md`, not `spike.md`, in every "cite `spike.md`" and in Task 9 step 6's copy.
   - Rewrite the Goal's last sentence and the Architecture line "the later tasks state both branches" (§4).
   - **Dead data under S3.** `TitleBandRequest::controlsCentreY`, `TitleBandGeometry::controlsCentreY` and
     `TrafficLightFit::SnapToFirstRow` are never set, and §D3 says "no `controlsCentreY`".
     - Proposed: drop them, leaving `TrafficLightFit { AppKit = 0, GrowBandToCells }` with the ledger's parked note.
     - Or keep them, if the owner wants the enumerator as data. Then `SnapToFirstRow` would need AppKit work that §D3
       shows fights `DidResize`.

7. **F7: the band token is `bandHeight`, not `size.titleBar` (Global Constraints, Tasks 2, 6, 7).**
   - **Plan text:** "The macOS band token is 1c's `size.titleBar` = 38". `titleBandTokens().height` = "1c's band token"
     (`size.titleBar` on native rows).
   - **Why:** 1c's ruling F5 (ledger, Task 4) made `UiStyleProvider::bandHeight` the source: 38 on MacOS, and
     `chromeHeight` (34, or one cell) on Flat rows. `size.titleBar` is 40/46 on Fluent/Linux and unread there. Reading it
     would grow those bars and break 1c's acceptance test.
   - **Proposed:** `titleBandTokens().height = provider.bandHeight()` (with **F2**'s no-host fallback), and Task 7's
     "<1c's token binding>" is `chromeStyle.bandHeight`.

8. **F8: align the Step 2 seams now (Tasks 2, 6, 7).**
   - `bandFill(int frame, int mode, int centre)`, as Step 2's scan F16 resolved and P5 accepted. 1d ignores `centre`
     until Step 2 adds it to `TitleBandSurfaceInputs`. Main.qml passes `band.centre`'s int.
   - Seam S1: Task 6 creates `TerminalSessionManager::refreshChromeDependents()`, which calls each controller's
     `refreshTitleBand()`. `refreshChromeStyle()` calls it once, outside the host guard (**F2**).
   - Seam S2: Task 6 adds `UiStyleProvider::resolvedRow()` and reads the material from it.
   - Tell Step 2's controller two things:
     - Step 2 Task 3 cannot compile before 1d Task 2 (`BandFill`, `toneOf`), so G1's "not a compile dependency" ruling
       needs correcting. Alternatively, Step 2 moves `BandFill::Background` and the `toneOf` use to its Task 7.
     - Under S4, 1d Task 2 adds no token-row edit, so no row conflict is possible.

9. **F9: the toolbar moves the traffic lights' inset (78 → 84 pt), and the safe area changes after apply (Tasks 5, 6).**
   - **Why:** S2 measured the inset widening when the toolbar attaches. `nativeControlsInsetChanged` is emitted only at
     the end of Expose (`WindowController.cpp:1412-1415`). The material is applied in `refreshTitleBand()`, and whenever
     `growth` changes live (Step 2's P3 detaches it for Minimal), the leading slot stays 6 pt short and the first tab
     overlaps the zoom button. The safe-area top also reaches 40 only after the toolbar exists, so a geometry measured
     before `apply` is stale until the signal.
   - **Proposed:**
     - `refreshTitleBand()` applies first and then measures.
     - It emits `nativeControlsInsetChanged` whenever it applied a request whose `growth` differs from the previous one.
     - On Expose, it runs before the existing emit.
     - Add a `[cocoa][titleband]` case: after `apply({ .growth = CompactToolbar })`, `nativeControlsInset` grows by the
       measured amount, and `None` restores it.

10. **F10: the device-pixel snap changes the Linux and Windows bands at fractional scales (Task 2).**
    - **Plan text:** "`AppKit`: `height = ceil(max(tokens.height, nativeHeight) · dpr) / dpr`", which applies "whenever
      `nativeHeight == 0`" too. QML takes `titleBandHeight` whenever it is non-zero.
    - **Why:** at dpr 1.25 a 34-pt Flat band becomes ⌈42.5⌉/1.25 = 34.4. This breaks "Linux and Windows unchanged" and
      1c's geometry pins at non-integer scales. On the Terminal row, a cell height is not a pixel quantity to snap.
    - **Proposed:** snap only when `nativeHeight > 0`. With no native title bar, `height = tokens.height` verbatim. Add a
      `geometry.devicePixelSnap` row: `{ flat34, Windowed, 1.25, 0 } → 34`.

11. **F11: the scale, screen and state hooks (Task 6).**
    - `onWindowScaleMaybeChanged` (`:1434`) returns early with no display or session, and again on the
      `_lastAppliedScale` latch.
      - A `refreshTitleBand()` placed inside it never runs in the "scale change" case, which uses a bare
        `ScopedControllerWithOsWindow` and an unchanged offscreen scale.
      - Proposed: call it from the `DevicePixelRatioChange` case of `eventFilter` (`:1384`), before the delegate.
    - Review Focus 4 also names a screen change. Connect `QWindow::screenChanged` in `bindWindow`.
    - Offscreen `setWindowStates(Qt::WindowFullScreen)` delivers `windowStateChanged` through the event queue, so the
      cases need `QTRY_`/`QTest::qWaitFor`.

12. **F12: `constexpr` over non-`constexpr` callees (Task 2).**
    - `toneOf` and `legibleBandTint` call `relativeLuminance`, and `tint.legibility` calls `contrastRatio`. Both are
      declared without `constexpr` and defined out of line (`Color.hpp:109`, `:116`).
    - `resolveTitleBandGeometry` uses `std::ceil`, which is `constexpr` only where the library implements P0533.
    - C++23 permits a `constexpr` function that is never constant-evaluated. MSVC (the `msvc-release` preset builds
      `contour_test`) is the compiler least likely to accept it quietly.
    - The tree's convention is `inline` for these (`snapPaneExtentToDevicePixels`, `WindowGeometry.hpp:772`).
    - **Proposed:** make `toneOf`, `legibleBandTint` and `resolveTitleBandGeometry` `inline`. Keep `bandTintOf` and
      `resolveTitleBandSurface` `constexpr`.

13. **F13: neither input nor legibility under AppKit's material was measured (Task 5; within S4's cost-if-wrong).**
    - **Why:**
      - With `titlebarAppearsTransparent = NO`, AppKit's title-bar view sits **above** the full-size content view. The
        spike found its material blending WithinWindow, so it blurs Qt's band, tabs and labels in the top 40 pt.
      - The empty toolbar (`NSToolbarView › NSGlassContainerView`, 900 × 40) also sits over the band, and AppKit may
        claim clicks there for window dragging.
      - The spike's E2 cannot render the blur, and it did no click test. Owner check 1 is the only guard.
    - **Proposed:**
      - Add a `[cocoa][titleband]` case: with the hint, the compact toolbar and `NativeMaterial`, a `QTest::mouseClick`
        at (300, 20) reaches a `MouseArea` in the band (press and release delivered).
      - Front-load owner check 1 ("does the glass read as material + tint, with legible tab labels") to Task 5's
        landing, before Tasks 6–7 build on it.
      - If either fails, S4's stated cost applies: the Solid row (`material.titleBand = Solid`).

14. **F14: Main.qml's root is an `ApplicationWindow`, and the spike used a bare `QQuickView` (Task 4).**
    - `ThemedWindow.qml` is `ApplicationWindow`. Under Qt ≥ 6.9, Quick Controls' windows and pages take the window's
      safe-area margins into their padding.
    - If `ApplicationWindow` does so here, `ExpandedClientAreaHint` would push the content, band included, 40 pt down,
      below the title bar it is meant to fill.
    - **Proposed:**
      - Task 4's `[cocoa][frame]` case loads the real `Main.qml`-shaped root (an `ApplicationWindow`) and asserts
        `contentItem.y == 0` and the band's `mapToItem(null, 0, 0).y == 0` under the hint.
      - If it is padded, zero the top padding on the root, with a comment citing the hint.

15. **F15: Qt ≥ 6.9 APIs in cross-platform code (Tasks 4, 6).**
    - `WindowController.cpp` compiles everywhere, and older Qt is still supported off macOS (`CMakeLists.txt:76-83`). Its
      `QWindow::safeAreaMarginsChanged` connection must be guarded. Use `#ifdef __APPLE__`, where F5's floor guarantees
      ≥ 6.9, or `QT_VERSION_CHECK(6, 9, 0)`.
    - Main.qml's `Qt.ExpandedClientAreaHint` and `Qt.NoTitleBarBackgroundHint` are evaluated only when the macOS-only
      properties are true. That is safe at run time; check that `ctest -L lint` stays clean.

16. **F16: the material's lifetime, and its place among the members (Task 3).**
    - **Plan text:** "beside `_nativeMenus` … `ContourGuiApp.hpp:1011`", "after `_menuBarIntegration`".
    - **Why:**
      - `:1011` is `_chromeStyle`.
      - The appearance sources are declared last (`:1044-1059`) on purpose, so that they are destroyed first. That is
        right for sources that call *into* the app. The material is the reverse: the controllers owned by
        `_sessionManager` (`:894`) and the windows owned by `_qmlEngine` (`:1033`) call into it, and an Expose or
        palette event during teardown would reach a destroyed object.
    - **Proposed:** declare `_titleBandMaterial` before `_sessionManager`, with a comment saying the controllers borrow
      it. It is initialized from the constructor parameter, so the declaration order is free.

17. **F17: the two-switch `appearanceServicesWith` cannot "call it with no record" (Task 3).**
    - The three-switch overload takes `TitleBandRecord&`, so a reference cannot be absent.
    - **Proposed:** the two-switch overload builds the null material itself, under `.titleBand`, beside `.menus`. The
      three-switch overload calls the two-switch one and replaces `.titleBand` with the recording factory.

18. **F18: CMake, ARC, the display gate, staging and clang-tidy (every task).**
    - `CocoaWindowFrame.mm` is **not** ARC (`platform/CMakeLists.txt:96-99`), so Task 4 keeps its plain
      `reinterpret_cast<NSView*>` and adds no `__bridge`. The new `CocoaTitleBandMaterial.mm` goes in both
      `target_sources` (`:88-95`) and the ARC list. `CocoaWindowFrame_test.mm` and `CocoaTitleBandMaterial_test.mm` go in
      `:837-842` and `:846-849`.
    - `REQUIRE_DISPLAY_OR_SKIP` does not check `platformName()`. Gate the `[cocoa]` cases as
      `CocoaMenuBarIntegration_test.mm:42` does, on both the display setting and `cocoa`.
    - `git add src/contour/platform|window|session|test|qml` (Tasks 3–8) would stage 1c Task 5's uncommitted work in this
      tree. Stage exact paths.
    - No step runs clang-tidy (`ENABLE_TIDY=OFF`). Add `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug
      --extra-arg=-isysroot$(xcrun --show-sdk-path) <new and changed .cpp/.mm>` to every Step 5 that touches C++. The
      new `_test.cpp`/`_test.mm` cover the headers. A new `.mm` reaches `compile_commands.json` only after the build
      re-runs CMake.
    - Every `@pre` main-thread AppKit entry asserts `Require(NSThread.isMainThread)`, as
      `CocoaNativeMenuPresenter.mm:160` does. The plan states this; repeat it in Task 5's brief.

19. **F19: Task 7's tint expectations depend on the test's palette (Task 7).**
    - `bandFill` → `Tint` → `legibleBandTint(scheme background, chrome tone)`. The tone is the application palette's
      `Window` luminance (`statusColor`'s rule, `:362-363`).
    - With `ChromeSetup`'s default scheme (`ColorPalette {}`) under a palette of the other tone, the answer is the
      palette colour. "MacOS row, windowed: the tint of the scheme's background" would then fail.
    - **Proposed:** the windowed, Reduce Transparency and full-screen cases set a `ChromeSetup::scheme` and an app palette
      of the same tone. Review Focus 3's case sets them apart on purpose.
    - Optionally give `MockMainController` `needsExpandedClientArea`/`clearsTitleBarBackground` (false), so Main.qml's
      `flags` reads defined properties.

20. **F20: sequencing against 1c, and one builder per tree.**
    - Tasks 2 and 3 have no 1c dependency left. Each touches `src/contour/CMakeLists.txt` and builds, and Task 3 also
      touches `ContourGuiApp.{hpp,cpp}`, so they run once 1c Task 5 commits.
    - Task 4 and Task 6 edit `WindowController.{hpp,cpp}`, `TerminalSessionManager` and `bindWindow`, which 1c Task 5 is
      changing: base them on 1c Task 5's commit. They do not need 1c Tasks 6–7.
    - Task 7 needs 1c Task 7: `TitleBar.presence/centre/revealed/slideOffset`, Main.qml's `band`, `bandRevealEdge`,
      `tabsBar`, and the snapshot rows' `.frame/.mode`. 1c's scan also asked for `y: restingTop + slideOffset`, so 1d's
      `titleBandRevealTop` is that `restingTop`.
    - Task 8 (the QML case only) needs 1c Task 7's leading slot.
    - Step 2's gates follow from this: G1's 1d part is 1d Task 2 (**F8**), and G4 is 1d Task 7.
