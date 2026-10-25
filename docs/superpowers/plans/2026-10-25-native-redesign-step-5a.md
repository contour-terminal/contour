# Native Redesign — Step 5a (Windows 11: Mica, Snap Layouts, the Fluent Row, the Windows Backends, the Menu Decision) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead of Steps 1c (in progress), 1d, 2, 3a–3f and 4b (pipelining ruling). Line numbers are at HEAD
`8354b21e`. Where 5a touches code an earlier step adds, the task names it as that step's plan does and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before 5a executes. The Step 4b plan
was not on disk when this plan was written; its names are taken from spec §3.13 and are re-verified by the
pre-flight scan.

**Goal:** On Windows 11 the title bar is the tab bar: Mica Alt behind a transparent band, tabs joined to the
content, Fluent caption buttons whose maximise button opens Snap Layouts, the Fluent row's canvas geometry and the
FluentWinUI3 controls, accessibility and appearance read from Windows, and a recorded menu decision. Windows 10
and every failure fall back to a solid band.

**Architecture:**
- **Pure decisions first, Qt-free, testable on this Mac:**
  - `window/CaptionHitTest.hpp`: where the maximise button is, what `WM_NCHITTEST` answers, and what each
    non-client mouse message does to the button and to the window. One hit function, one transition table.
  - `platform/SystemBackdrop.hpp`: which `DWMWA_SYSTEMBACKDROP_TYPE` and which frame margins a window gets,
    from the band's backdrop, the window's kind and the user's `background.blur`.
  - `platform/WindowsSettings.hpp`: what the registry and `SystemParametersInfo` readings mean as
    `AccessibilityPreferences` and `PlatformAppearance`, and which `WM_SETTINGCHANGE` touches which source.
  - The Fluent token rows (`config/UiStyle.hpp`, `config/ChromeTokenGroups.hpp`), the title-band surface rows
    (Step 1d's `TitleBandSurfaceRows`) and the window-control rows (`config/WindowControlStyle.hpp`).
- **Every Win32 call sits behind an interface with a null implementation** (AGENT.md: dependency injection):
  - `platform::INativeCaptionButtons` (new) — Snap Layouts; null everywhere but the Windows QPA.
  - `platform::ITitleBandMaterial` (Step 1d) gains a Win32 implementation — Mica.
  - `platform::IWindowsSettingsReader` (new) — the registry, `SystemParametersInfoW`, `RtlGetVersion`; the
    accessibility and appearance sources built on it are cross-platform classes tested with a fake reader.
  - Each Win32 adapter is a `.cpp` wholly inside `#ifdef _WIN32`, returns `nullptr` from its factory unless
    `QGuiApplication::platformName() == "windows"`, and the shared factory falls back to the null object
    (`NativeWindowFrame.cpp:17-36` is the shape).
- **QML renders the answers.** `WindowControls.qml` reports each caption button's scene rectangle to its
  controller and draws the maximise button's hover and press from `win.nativeMaximizeVisual`; `TabStrip.qml`
  and `TabItem.qml` gain the `Joined` tab shape; `TitleBar.qml` paints `win.titleBandFill`.

**Tech Stack:** C++23, Qt 6.11.1 (Gui, Quick, Quick Controls incl. FluentWinUI3), Win32/DWM (`dwmapi`, `user32`,
`advapi32`, `ntdll`, resolved at runtime as `Dwmapi.hpp` does), QML, Catch2 v3, CMake trees
`out/appleclang-debug` (here) and `msvc-release` (CI), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.14 (`:763-790`) binds,
with what it cites: §1 (principles, `:28-64`), §2.1 (token rows, `:68-126`; "Until Phase 5, the Fluent and
ContourLinux rows keep today's geometry", `:105-110`), §2.3 (accessibility; "Windows: `SPI_GETCLIENTAREAANIMATION`,
`SPI_GETHIGHCONTRAST`, and `EnableTransparency` from the registry", `:155-158`), §2.4 (platform appearance,
`:169-185`), §2.6 (testing, `:206-226`), §3.2 (`:363-365`: "Windows uses in-scene menus until 5a decides"), §3.5
(`:458`: "The choice sits behind `ITitleBandMaterial`"), and §4's row "Snap Layouts break the QML caption buttons |
The NC mouse messages are handled, with a hit-test table" (`:840`). Design record:
`docs/superpowers/specs/2026-10-23-native-redesign-design.md` (`:16-17` "Menus are native on macOS and Windows";
`:40` "menus stay in-scene on every OS" for the TUI row; `:45-47` "Linux CSD and Windows draw ● ● ● cells from the
scheme"; `:87-88` Cascadia Mono 12 pt on Windows). Canvas: <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>;
local copies `win`, `win-settings`, `kit-tokens`, `tui` in `.superpowers/design/canvas/project/`. Plans index:
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Earlier plans this one builds on:
`2026-10-25-native-redesign-step-1c.md`, `-1d.md`, `-2.md`, `-3d.md`, `-4a.md`; Step 4b per spec §3.13.

## Status board and handoff (every step)

This run is tracked on a status board: <https://claude.ai/artifact/YZUVcC1vdaWaLGJLsdS2AX>. The
owner asked for the link to live here so that any agent continuing the work can find the board and
keep it current. The page is kept with `/status-board` (the `contour-workflows` plugin) on the machine doing the work; its source is local to that working tree and never committed.
- **Keep it up to date** with `/status-board update`, which republishes to the link above
  (`/status-board` remembers the link per working tree). The board belongs to the owner's account, so a
  session signed in as the owner can read and publish it; the design canvas, the owner's copy of
  Yaraslau's, is in the same account.
- **Every agent that continues this work updates the board.** Do it
  in the same turn as each state change: a step or task started, in review, committed, landed,
  blocked or unblocked, a plan written, or a ruling.
- A different worktree has a board of its own. Never write to another worktree's board.
  Report to the session that owns the board instead.
- If this worktree has no board, ask the owner rather than creating a second board.

## Global Constraints

- **This Mac cannot build or run Windows code.** Every `#ifdef _WIN32` line is compiled and tested only by CI: the
  `Windows` job (`.github/workflows/build.yml:1020-1300`, `windows-latest`, preset `msvc-release`, MSVC). Its
  `Test` step runs `ctest --preset msvc-release --build-config RelWithDebInfo`, which includes
  `contour_windows_native_test` (`src/contour/CMakeLists.txt:973-983`: `contour_gui_test "[native]"` under
  `QT_QPA_PLATFORM=windows`, label `native;windows`). CI runs on `pull_request` to master, so a push to the PR
  #2178 branch triggers it. Hence:
  - every Windows task keeps a cross-platform, macOS-testable part (a pure table, a token row, a null backend, a
    class driven by a fake) and isolates the Win32 adapter behind an interface with a null implementation;
  - every Windows-only task has a **Verified by** line naming the CI job, the ctest and the Catch2 filter;
  - a Windows-only case is tagged `[native][win32][…]`, starts with
    `if (QGuiApplication::platformName() != QStringLiteral("windows")) SKIP("Requires the native Windows platform.");`
    (the `NativeWindowFrame_test.cpp:28-31` shape), and sits inside `#ifdef _WIN32`;
  - a Catch2 `SKIP` exits 0, so Task 6 adds a CI step that fails the job when `[win32]` reports `skipped`, as
    the `[pwsh]` step does (`build.yml:1189-1215`).
- **Windows CI state at HEAD.** The Phase 0 ledger's finding (`docs/superpowers/ledgers/2026-10-23-native-redesign-phase0.md:140`:
  master's Windows build failed in `src/vthost/SshTransport.cpp:210-223`) **no longer holds**: master's run
  `40187029117` (`974feaf1`) passes `Windows`, and the PR's run `40233959931` (head `b4be789b`) compiles
  `SshTransport.cpp` (log line 2715). The PR's `Windows` job now fails in **Build** on this branch's own code:
  `src/contour/config/NewDefaultsOffer.hpp(69): error C3535: cannot deduce type for 'const auto *const' from
  'std::_Array_const_iterator<_Ty,2>'` (from `c2a4f551`, Step 1a), and the build stops there, so further MSVC
  errors may follow. `Latency trend (windows-2022)` fails on master too and is not this step's. Task 1 makes the
  `Windows` job green before any Windows task relies on it.
- **The spec, verbatim (§3.14):**
  - "Set `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_TABBEDWINDOW` on the window. Call `DwmExtendFrameIntoClientArea` with
    margins of `-1`. Today it uses a 1-px top margin (`extendFrame`, `Win32WindowFrame.cpp:133-140`,
    `cyTopHeight = 1` at `:139`)." (Still exact at HEAD.)
  - "The QQuickWindow gets an alpha-capable surface. The band is cleared to transparent, and the panes are painted
    opaque. If any of that fails, or under Reduced transparency, the band is `Solid`." The format already has an
    8-bit alpha (`display/ShaderConfig.cpp:30`) and `Main.qml` sets `color: "transparent"`; neither changes.
  - "`WM_NCHITTEST` returns `HTMAXBUTTON` over the maximise button. The filter must also handle:
    `WM_NCLBUTTONDOWN` and `WM_NCLBUTTONUP`: maximise or restore, and swallow the default handling;
    `WM_NCMOUSEMOVE` and `WM_NCMOUSELEAVE`: drive the QML button's hover state. All of these go into the hit-test
    table and its tests."
  - "Caption buttons are 46 wide, drawn with Segoe Fluent glyphs. The tabs join the content area. The profile
    dropdown stays. The Fluent row's `quickControlsStyle` is `"FluentWinUI3"`."
  - "Choose between in-scene menus and QML `Popup.Native` with sub-menus built before `open()`. The choice is
    recorded in the 5a plan" (R-a below).
  - "The Windows backends for accessibility preferences and platform appearance."
- **Canvas values, exact** (`win`, `win-settings`, `kit-tokens`):

  | Token | Fluent value | Source |
  |---|---|---|
  | `chromeHeightUnits` / `size.titleBar` | 40 | `win` "Height 40"; kit `size.titleBar 40 · 40` |
  | `tabHeightUnits` / `size.tab` | 34, bottom-aligned, top corners `radii.tab` 6 | `win`; kit "6 top only" |
  | `windowControlUnits` (caption button width) | 46, full bar height | `win` "measure 46 wide" |
  | caption glyphs (Close, Minimize, Maximize; Restore) | `U+E8BB`, `U+E921`, `U+E922`; `U+E923` | `win` |
  | caption glyph fonts | "Segoe Fluent Icons", then "Segoe MDL2 Assets" (same code points, Windows 10) | R-f |
  | close hover | `#C42B1C`, glyph white (`chromeStyle.onColor`) | `win`; `WindowControlStyle.hpp:158` |
  | other caption hover | kit `fill.4` (`FillAlphas[3]`: black 5 % / white 6 %) | `win` (maximise backplate `rgba(255,255,255,0.06)`) |
  | caption pressed | the hover fill, glyph at `label.secondary` (`LabelAlphas[1]`) | WinUI pressed state |
  | inactive window | caption glyphs and tab labels at `label.tertiary` (`LabelAlphas[2]`) | `win` "semi-transparent" |
  | tab strip leading inset (`space.tabStripLeading`) | 8 | `win` `padding-left: 8px` |
  | tab insets / icon gap | leading `s6` (12), trailing `s4` (8), icon-to-title `s4` (8) | `win` |
  | `size.tabIcon` / `size.tabClose` / `radii.tabClose` | 16 / 24 / 4 | `win` |
  | `+` (`stripButtonUnits`) / `▾` (`size.profileButton`) / their height (`size.stripButton`) | 36 / 28 / 30, 2 above the bar's bottom, `radii.control` 4 | `win` |
  | inactive tab rule | trailing 1-px rule at `contrast.separatorPercent` | `win` (`inset -1px 0 0 rgba(255,255,255,0.08)`) |
  | `radii.paneCard` / `radii.control` / `space.paneGap` | 4 / 4 / 4 (already, Step 2) | `win`, kit |
  | window radius | 8, 0 maximised or snapped: DWM's (`DWMWCP_ROUND`, `Win32WindowFrame.cpp:250-252`) | kit `radius.window 8 · 0 snapped` |
  | `radii.menu` / `radii.menuRow` | 8 / 4 | kit `radius.menu · row 8 · 4` |
  | chrome font | "Segoe UI Variable Text", then "Segoe UI" | `win`, kit "Segoe UI Variable (epx)" |
  | type | caption 12, body 14, tab 12, pane header 12 semibold (already) | kit type ramp |
  | backdrop | main window `DWMSBT_TABBEDWINDOW` (4, Mica Alt); Settings `DWMSBT_MAINWINDOW` (2, Mica) | `win`; `win-settings` "Mica window" |

  DWM constants, spelled out as `Dwmapi.hpp:75-78` does: `DWMWA_SYSTEMBACKDROP_TYPE = 38`, `DWMSBT_AUTO = 0`,
  `DWMSBT_NONE = 1`, `DWMSBT_MAINWINDOW = 2`, `DWMSBT_TRANSIENTWINDOW = 3`, `DWMSBT_TABBEDWINDOW = 4`. Builds:
  Windows 11 is build ≥ 22000; `DWMWA_SYSTEMBACKDROP_TYPE` needs build ≥ 22621 (22H2).
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a and the SDD ledgers of 1c, 1d, 2):
  - Step 1b: Windows keeps in-scene menus until 5a decides (§3.2; R-a).
  - Step 1c: the Fluent row kept today's geometry until now; this step changes the Fluent half of 1c's acceptance
    test ("the Fluent and ContourLinux rows keep today's geometry (offscreen)") and of
    `UiStyle.nativeRowsKeepTodaysGeometry` (`UiStyle_test.cpp:204`), and leaves ContourLinux's half to 5b.
  - Step 2, D1: pane cards on every row, Fluent included, with the canvas values (radius 4, gap 4, header 28).
  - Step 1a: the Win32 frame re-applies on `ApplicationPaletteChange` (`WindowController.cpp:1420-1426`); the null
    font and appearance factories off Apple (`PlatformAppearance.cpp:7`, `AccessibilityPreferenceSource.cpp:7`).
  - Step 1d: the band surface is one table (`TitleBandSurfaceRows`), "adding Windows' Mica in 5a is a row"; the
    material is built once per app and borrowed per controller; one owner per OS property.
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`; a file's baseline only goes
    down). Every visual surface is a snapshot row, and every visual task renders the Terminal row.
  - Reduce Motion is honoured; text contrast ≥ 4.5:1; status colours are tokens, Running is the accent.
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`; no test's outcome depends on the developer's
    or the runner's real OS settings.
  - Owner manual checks are batched per phase, listed in the ledger, never awaited.
- **AGENT.md applies in full:**
  - `enum class` over `bool`, zero the default. QML-facing `Q_PROPERTY`/`QVariantMap` booleans are the documented
    carve-out. Win32 `BOOL`s stay at the adapter boundary.
  - `std::expected` for fallible calls: `TitleBandMaterialError` (Task 10), starting at `= 0`.
  - Dependency injection: the registry, `SystemParametersInfoW`, `RtlGetVersion` and DWM are reached through
    `IWindowsSettingsReader` and an injected `DwmApi const&`; no adapter calls `dwmApi()` in a function a test
    cannot replace.
  - Configuration at construction: every new adapter and source is complete after its constructor.
  - Data-driven: the hit table, the NC transition table, the backdrop table, the setting-topic table, the
    window-control rows and the token rows.
  - Doxygen (`///`, `@param`, `@return`, `@pre`) on every new public declaration; `.hpp` with `#pragma once`.
    Every Win32 call is documented `@pre` GUI thread.
  - Zero warnings and no `NOLINT`. MSVC is stricter than AppleClang on iterator types (`auto const*` from a
    `std::array` iterator fails, Task 1) and on narrowing (`static_cast<DWORD>(sizeof x)`, `Win32WindowFrame.cpp:283-287`).
- **Build here** with every core, `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`):
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds at once.
- **clang-tidy** on every touched non-Windows `.cpp`:
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <files>`.
  A file wholly inside `#ifdef _WIN32` preprocesses to nothing here; CI's clang-tidy job does not cover it either,
  so its reviewer reads it against AGENT.md by hand.
- **Format** every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`. Check every new or
  touched `.qml`, `.txt`, `.sh`, `.yml`, `CMakeLists.txt` with `editorconfig-checker <files>`. **Never
  clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a
  value, and every row names every field (`ChromeTokenRows`, `UiStyleTokenTable`, `WindowControlTokenTable`,
  `TitleBandSurfaceRows`, `CaptionTransitions`, `SnapshotScripts`, every `AppearanceServices` literal).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type. Counts
  in tests are derived from the tables, never written as literals.
- **Qt-free targets stay Qt-free:** `window/CaptionHitTest.hpp`, `platform/SystemBackdrop.hpp`,
  `platform/WindowsSettings.hpp` and every `config/` change include no Qt header.
- **Never edit `vendor/core-cpp`.**
- **Tests:** each task runs only its own filter here; the full suite runs in CI on Linux, macOS and Windows.
- **Commits:** one per task, staging only that task's files. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>` and never carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 18).

## Shared files (Steps 5a and 5b may run in parallel worktrees)

Spec §3.15 (Step 5b) names, or its work necessarily touches, these files that 5a also edits. Whoever lands second
rebases (never merges) and resolves; each conflict is additive (a row, a field, an `#if` branch):
- `src/contour/config/UiStyle.hpp` (`UiStyleTokenTable`; 5b gives ContourLinux its own row and style) and
  `src/contour/config/UiStyle_test.cpp` (`nativeRowsKeepTodaysGeometry`).
- `src/contour/config/ChromeTokenGroups.hpp` (`FluentRow` / `LinuxRow`, `TabShape`, the new size and space fields
  every row names) and `src/contour/config/ChromeTokenGroups_test.cpp`.
- `src/contour/config/WindowControlStyle.hpp` (5b: "The window buttons follow the desktop's layout", §2.4; 5a:
  `PlasmaTokens` derives from `WindowsTokens`, Task 7) and `config/WindowControlStyle_test.cpp`.
- `src/contour/window/WindowControlStyleProvider.{hpp,cpp}`, `src/contour/qml/WindowControls.qml`,
  `src/contour/qml/TitleBar.qml`, `src/contour/qml/TabStrip.qml`, `src/contour/qml/TabItem.qml`.
- `src/contour/platform/PlatformAppearance.{hpp,cpp}` and `src/contour/platform/AccessibilityPreferenceSource.cpp`
  (5b adds the button layout field and turns the non-Apple factories into the portal backends; 5a adds two fields
  and a Windows branch, Tasks 12–14).
- `src/contour/platform/AppearanceServices.hpp`, `src/contour/test/AppearanceFixtures.hpp`.
- `src/contour/window/TitleBandSurface.hpp` (Step 1d's rows; 5b may add a Linux row).
- `src/contour/test/ChromeSnapshotPlan.hpp`, `src/contour/test/ChromeSnapshotScene.hpp`,
  `scripts/run-chrome-snapshots.sh` (5b adds a ContourLinux style pass, 5a a FluentWinUI3 pass).
- `src/contour/test/MainWindowQml_test.cpp` (1c's native-rows acceptance case), `src/contour/test/QmlComponents_test.cpp`.
- `src/contour/CMakeLists.txt`, `src/contour/platform/CMakeLists.txt`, `.github/workflows/build.yml`.
- `metainfo.xml` (release notes) and the plans index / board (Finish).
- Task 7's rule "the Terminal row draws its window controls as ● cells" applies on every host; 5b inherits it for
  Linux CSD and only adds its dot colours if they differ.

## Review Focus

1. **Snap Layouts on hover over maximise.** Hovering the maximise button (on the Fluent row and on the Terminal
   row's ● cell) for about half a second opens Windows 11's Snap Layouts flyout; the button shows its hover fill
   while the pointer is on it; a click maximises or restores exactly once; press, drag off, release does nothing;
   minimise and close keep working as QML buttons. Pinned in Task 4 (`captionHit.maximizeOnly`,
   `captionTransitions.*`), Task 5 (`the controller forwards hover, press and toggle`), Task 6
   (`WM_NCHITTEST over the maximise button answers HTMAXBUTTON`, `a non-client click toggles the window once`) and
   Task 7 (`the native hover state lights the maximise button`).
2. **A maximised window's 0 radius and edges.** Maximised, DWM squares the corners, the content fills the work
   area, and the caption buttons reach the screen's top and right edge pixels (no resize strip steals them);
   windowed, the top resize strip over the buttons still resizes. Pinned in Task 4 (`captionHit.maximizedReachesTheEdge`,
   `captionHit.windowedKeepsTheResizeStrip`), Task 6 (`the maximised window's edge pixel over the maximise button
   answers HTMAXBUTTON`) and Task 11 (`the corner preference stays round under Mica`).
3. **Windows 10 (or 11 21H2) without Mica.** No `DWMWA_SYSTEMBACKDROP_TYPE`: the band is the solid tint, the
   frame keeps its 1-px top margin (no black glass), and everything else of the Fluent row works. Pinned in Task 9
   (`backdrop.windows10IsSolid`, `margins.clientKeepsOnePixel`), Task 10 (`a failed apply falls back to Solid for
   that window`), Task 11 (`a rejected backdrop attribute reports Unsupported and restores the margins`, with an
   injected `DwmApi`) and Task 12 (`materialSupportFor` over builds 19045, 22000, 22621, 26100).
4. **High-contrast mode.** A Windows contrast theme turns Mica off (solid band), raises the chrome's contrast
   tokens, and lets the chrome take the OS palette instead of the terminal scheme's (owner decision 2); the close
   button's hover follows the palette, not `#C42B1C`. Pinned in Task 12 (`accessibility.highContrast`), Task 14
   (`a contrast theme hands the chrome to the OS palette, and back`) and Task 7 (`a contrast theme drops the red
   close hover`).
5. **A second monitor with a different DPI.** After a move from 100 % to 150 %, Snap Layouts still triggers over
   the maximise button and nowhere else, the frame thickness is the new monitor's, and the band height is whole
   device pixels. Pinned in Task 4 (`captionHit.sameZoneAtEveryScale` over dpr 1, 1.25, 1.5, 2), Task 6 (the
   scaled ctest `contour_windows_native_scaled_test`, `QT_SCALE_FACTOR=1.5`) and Task 5 (`a scale change keeps the
   reported rectangles logical`).

Also pinned, beyond the five: `background.blur: true` turns the whole window to Acrylic and off again restores
Mica, not `DWMSBT_NONE` (Task 9, Task 11); a DWM composition restart keeps the window's margins (Task 11); the
package ships the FluentWinUI3 style (Task 3); the Settings window's native title bar follows the chrome scheme
(Task 16).

---
## Spec resolutions (read before Task 1)

These settle where the spec is silent, or where the canvas or the code disagrees with it. Those marked
**(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. The menu decision: in-scene (owner, 1).** §3.14 asks for a spike between in-scene menus and QML
  `Popup.Native` with pre-built sub-menus. A spike needs a Windows desktop, which this machine is not, and CI
  cannot drive a modal `TrackPopupMenuEx` loop reliably. The decision is therefore made on these grounds, and the
  spike becomes owner check O-9:
  - `Popup.Native` on Windows is Qt's `QWindowsPopupMenu`, a Win32 `HMENU`. It cannot draw the tab menu's swatch
    row (1b's `MenuSwatchRow.qml`), does not follow the terminal scheme the chrome follows (§2.2), and has no
    supported dark mode of its own.
  - Runtime sub-menus are what made the tab menu come up empty on Windows before (`ContourMenu.qml:6-8`); 1b's
    `MenuTree` could pre-build them, but the swatch and scheme losses remain.
  - In-scene menus under FluentWinUI3 (Task 3) take the kit's Fluent radii (8 · 4, Task 15) and the e2 shadow.
  So the Fluent row keeps `layout.menu = MenuPresentation::InScene`. This departs from the design record's
  "Menus are native on macOS and Windows" (`:16-17`), which is why it is owner decision 1. Reverting is one token
  cell plus a Windows `INativeMenuPresenter`.
- **R-b. Only the maximise button is non-client.** §3.14 names `HTMAXBUTTON` alone. Minimise and close stay QML
  buttons (`HTCLIENT`), so their hover, press, focus and accessibility are today's. Drag and resize stay with
  `TitleBar.qml`'s drag region and `ResizeBorder.qml` (`enabled: window.visibility === Window.Windowed`,
  `ResizeBorder.qml:21`).
- **R-c. The rectangle comes from the scene, the decision from the table.** `WindowControls.qml` reports each
  control's scene rectangle in logical pixels to `WindowController::setCaptionButtonRect`; the adapter converts the
  message's physical point with the window's current device-pixel ratio and asks `captionHitAt`. A bottom tab bar,
  a right-to-left layout and the Terminal row's ● cells are therefore all covered by the same code.
- **R-d. Windowed, the top resize strip wins.** Windows' own caption buttons give their top
  `resizeBorderThickness` pixels to the top resize edge while windowed. Maximised or full screen there is no resize
  strip, and the button reaches `y = 0` and the right edge (Fitts).
- **R-e. Window corners are DWM's.** "Window 8, 0 when snapped or maximised" is what `DWMWCP_ROUND`
  (`Win32WindowFrame.cpp:250-252`) already gives: DWM squares a maximised or snapped window. Pane cards keep
  `radii.paneCard` 4 in every placement (kit: `radius.paneCard 4`, no snapped variant). Nothing in QML draws the
  window's corner.
- **R-f. Caption glyphs need a font that may be missing.** Segoe Fluent Icons is Windows 11's; Windows 10 has the
  same code points in Segoe MDL2 Assets; neither exists on macOS or Linux (snapshots, and a Linux user who chose
  `window_controls: windows`). The first installed family of `{"Segoe Fluent Icons", "Segoe MDL2 Assets"}` draws the
  `U+E8BB/E921/E922/E923` glyphs; with neither, today's Unicode glyphs (`✕ — ▢ ❐`) are drawn in the chrome font.
  `PlasmaTokens` clears the icon families, so Breeze never borrows them.
- **R-g. Mica support is an OS fact, and a failed apply is a window fact.** The Windows appearance backend
  publishes `MaterialSupport::Unsupported` below build 22621; `resolveChromeTokens` then turns `Mica` into
  `Solid`, as Reduced transparency already does (`ChromeTokenGroups.hpp:847-852`). If DWM still rejects the
  attribute on a supported build, `ITitleBandMaterial::apply` reports it (Step 1d's `void` becomes
  `std::expected<void, TitleBandMaterialError>`), and that window falls back to `Solid` for its lifetime.
- **R-h. The material owns the backdrop and the margins; the frame owns the frame.** The Win32 frame keeps the
  style bits, `WM_NCCALCSIZE`, the dark-mode attribute (`DwmwaUseImmersiveDarkMode`) and the corner preference. The
  Win32 material owns `DWMWA_SYSTEMBACKDROP_TYPE` and `DwmExtendFrameIntoClientArea`'s margins, records both on the
  `QWindow`, and `WM_DWMCOMPOSITIONCHANGED` re-extends from that record instead of today's fixed 1-px margin.
  Step 1d's controller applies the material only under `FrameShadowStrategy::FullSizeContent`; from 5a it applies
  under `NativeFrameKept` too.
- **R-i. `background.blur` and Mica share one attribute.** `BlurBehind.cpp:139-150` writes
  `DWMWA_SYSTEMBACKDROP_TYPE` itself (Acrylic on, `DWMSBT_NONE` off), which would turn Mica off whenever a profile
  without blur activates. From 5a it records the wish on the window and asks `systemBackdropFor`: blur on is
  Acrylic (`DWMSBT_TRANSIENTWINDOW`) over the whole window, the user's explicit choice (owner, 4); blur off is the
  band's backdrop. The Windows 10 `SetWindowCompositionAttribute` fallback (`BlurBehind.cpp:152-…`) is unchanged.
- **R-j. The joined tab strip keeps its bar.** `Joined` tabs are the title bar, so the Fluent band never hides:
  1c's rule `{ Flat, ClientBand, Windowed, Hidden, Top } → { Hidden, … }` would leave a one-tab Native2026
  window with no caption buttons. `Joined` gets `Pill`'s rows (`band = Shown` while the frame is the client band),
  and with the tab strip gate hidden it still shows its one tab, as Windows Terminal does.
- **R-k. The active joined tab is the content's colour.** "The tabs join the content area": the active tab fills
  with the colour of the content backdrop behind the panes (Step 2's `colors.paneGapFill`, the item Main.qml
  paints behind the pane tree), so the tab and the content read as one surface; inactive tabs are transparent over
  the band. Under Mica the content backdrop and every pane stay opaque, so no terminal text sits on glass
  (design record `:14`).
- **R-l. High contrast is a theme, not a preference.** Windows' contrast themes set `HCF_HIGHCONTRASTON`. The
  readings map it to `ContrastPreference::Increased` **and** `TransparencyPreference::Reduced` (Windows itself
  drops transparency effects there), and the appearance publishes `ContrastTheme::Active`, under which the chrome
  takes the OS palette (owner, 2). macOS's Increase Contrast is not a theme and keeps today's behaviour.
- **R-m. The Settings window keeps the server frame (owner, 3).** It gets the Mica backdrop (`DWMSBT_MAINWINDOW`)
  in its native title bar and the dark-mode attribute from the applied chrome scheme, through the same material
  and frame interfaces. Snap Layouts and the caption buttons there are Windows' own. 4b owns the page.
- **R-n. The Terminal row draws its window controls as ● cells on every host but macOS.** On macOS AppKit's lights
  stay (§3.4 TUI row). Elsewhere the Terminal row uses the `TrafficLight` presentation with the configured style's
  side and order (Windows: trailing, minimise · maximise · close; owner, 5), and each dot is inked from the
  scheme: close Danger, minimise Warning (a glyph, never a fill), maximise Success, through `legibleOnBar`.
- **R-o. Inactive window.** "Title bar elements semi-transparent": labels and caption glyphs drop to
  `label.tertiary`. "Mica falls back to solid" is DWM's own behaviour for an inactive window; nothing is coded, it
  is owner check O-4.

### Non-goals

- A native Win32 menu presenter (R-a). Windows 10's acrylic look. Per-monitor accent. The Settings page's
  layout (4b). Linux (5b). The quick terminal and global hotkey (6.7). Taskbar badges (6.9).

---

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/NewDefaultsOffer.hpp` (modify) | MSVC-clean iterator type | 1 |
| `src/contour/config/UiStyle.hpp` (modify) | `FluentTokens`, `fontFamilies`, `firstAvailableFamily`, `quickControlsStyle` | 2, 3 |
| `src/contour/config/ChromeTokenGroups.hpp` (modify) | `TabShape::Joined`, Fluent sizes, `radii.menuRow`, `MaterialSupport`, `BackdropKind`, the `resolveChromeTokens` overload | 2, 9, 15 |
| `src/contour/window/UiStyleProvider.{hpp,cpp}` (modify) | the Fluent chrome font; material support; caption and joined-tab invokables | 2, 7, 8, 9 |
| `src/contour/CMakeLists.txt`, `scripts/run-chrome-snapshots.sh`, `.github/workflows/build.yml` (modify) | the FluentWinUI3 run and pass; the scaled native ctest; the CI guards | 3, 6, 17 |
| `src/contour/window/CaptionHitTest.hpp` (create) | hit function, NC transition table, physical→logical | 4 |
| `src/contour/platform/NativeCaptionButtons.{hpp,cpp}` (create) | `INativeCaptionButtons`, null, factory | 5 |
| `src/contour/platform/Win32CaptionButtons.{hpp,cpp}` (create) | the Win32 adapter | 6 |
| `src/contour/window/WindowController.{hpp,cpp}` (modify) | caption rects and visual; material on `NativeFrameKept`; failure latch; `titleBandFill` | 5, 10 |
| `src/contour/config/WindowControlStyle.hpp`, `window/WindowControlStyleProvider.{hpp,cpp}`, `qml/WindowControls.qml` (modify) | Fluent glyphs and fills, Terminal-row dots, rect reporting | 7 |
| `src/contour/qml/TabStrip.qml`, `qml/TabItem.qml`, `window/TitleBandLayout.hpp` (modify) | the `Joined` shape | 8 |
| `src/contour/platform/SystemBackdrop.hpp` (create), `window/TitleBandSurface.hpp` (modify) | backdrop and margins tables; Fluent surface rows | 9 |
| `src/contour/platform/TitleBandMaterial.{hpp,cpp}`, `CocoaTitleBandMaterial.mm`, `test/AppearanceFixtures.hpp` (modify) | `apply` returns `std::expected` | 10 |
| `src/contour/platform/Win32TitleBandMaterial.{hpp,cpp}`, `Win32SystemBackdrop.{hpp,cpp}` (create); `Win32WindowFrame.cpp`, `BlurBehind.cpp` (modify) | Mica | 11 |
| `src/contour/platform/WindowsSettings.hpp`, `WindowsAppearanceSources.{hpp,cpp}` (create); `PlatformAppearance.hpp` (modify) | readings, mapping, cross-platform sources | 12 |
| `src/contour/platform/Win32Settings.{hpp,cpp}` (create); `PlatformAppearance.cpp`, `AccessibilityPreferenceSource.cpp` (modify) | registry/SPI reader, watcher, factories | 13 |
| `src/contour/platform/GuiTheme.hpp`, `ContourGuiApp.{hpp,cpp}` (modify) | contrast themes hand the chrome to the OS palette | 14 |
| `src/contour/qml/ContourMenu.qml`, `qml/ActionContextMenu.qml` (modify) | the Fluent menu radii | 15 |
| `src/contour/window/SettingsWindowHost.{hpp,cpp}` (modify) | Settings frame and Mica | 16 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp`, `MockMainController.hpp` (modify) | Fluent and Windows-host snapshot rows | 17 |

---
### Task 1: A green Windows job to verify against

**Files:**
- Modify: `src/contour/config/NewDefaultsOffer.hpp:69` — `auto const rule = std::ranges::find_if(…)`; the comparison
  at `:73` is then iterator against iterator.
- Modify: whatever further file the next MSVC run names, under the same rule (fix the cause; no `#pragma`, no
  `-Wno-error`).

**Interfaces:** none. Every later "Verified by" line relies on this task's green run.

- [ ] **Step 1: See the failure.** `gh pr checks 2178`; for the `Windows` job,
  `gh api repos/contour-terminal/contour/actions/jobs/<job-id>/logs | grep -E "error C|FAILED:"` (the
  `--log-failed` form refuses while the run is in progress). Expected at HEAD: the three `NewDefaultsOffer.hpp`
  errors above.
- [ ] **Step 2: Fix, and check here that nothing else regressed.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test && out/appleclang-debug/src/contour/contour_test "[config]"`

  Expected: PASS.
- [ ] **Step 3: Commit, push, and repeat Steps 1–2 until `Windows` passes Build and Test.** One commit per round,
  each naming the MSVC diagnostic it fixes. A `Test` failure that master's last green `Windows` run
  (`gh run list --branch master --workflow Build`) also has is recorded in the ledger and not fixed here; any other
  is fixed.

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/NewDefaultsOffer.hpp
git add src/contour/config/NewDefaultsOffer.hpp
git commit -m "config: deduce the rule iterator by value, which MSVC requires" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
git push
```

**Verified by:** CI `Windows` job (`build.yml` `windows:`), steps `Build` and `Test` (`ctest --preset msvc-release`,
including `contour_windows_native_test`, `[native]`). Record the run id in the ledger.

---

### Task 2: The Fluent row's values (pure)

**Files:**
- Modify: `src/contour/config/UiStyle.hpp` — `UiStyleTokens` (`:79-…`) gains `fontFamilies`; a new
  `detail::FluentTokens` beside `NativeTokens` (`:312-372`); `UiStyleTokenTable` (`:454-459`) uses it for `Fluent`;
  `firstAvailableFamily`.
- Modify: `src/contour/config/ChromeTokenGroups.hpp` — `TabShape` (`:50-54`) gains `Joined`; `SizeTokens`
  (`:197-224`) gains `stripButton`, `profileButton`; `SpaceTokens` (`:228-241`) gains `tabStripLeading`; every row
  names them; `FluentRow` (`:599-648`) takes the canvas values.
- Modify: `src/contour/window/UiStyleProvider.cpp` — `resolveChromeFont` picks the Fluent family.
- Test: `src/contour/config/UiStyle_test.cpp`, `src/contour/config/ChromeTokenGroups_test.cpp`,
  `src/contour/window/UiStyleProvider_test.cpp`, `src/contour/test/MainWindowQml_test.cpp` (1c's native-rows case).

**Interfaces:**
- Produces (`config/UiStyle.hpp`):
  - `UiStyleTokens::fontFamilies` — `std::span<std::string_view const> fontFamilies;` the chrome font families to
    try in order, empty for the platform UI font. `NativeTokens` and `TerminalTokens` name it `{}`.
  - `inline constexpr auto FluentFontFamilies = std::array { std::string_view { "Segoe UI Variable Text" }, std::string_view { "Segoe UI" } };`
  - `detail::FluentTokens`: a copy of `NativeTokens` (the `PlasmaTokens` lambda shape, `WindowControlStyle.hpp:190-195`)
    with `chromeHeightUnits = 40`, `tabHeightUnits = 34`, `windowControlUnits = 46`, `stripButtonUnits = 36`,
    `fontFamilies = FluentFontFamilies`. `quickControlsStyle` stays `"Fusion"` until Task 3.
  - `template <std::predicate<std::string_view> IsInstalled> [[nodiscard]] constexpr std::optional<std::string_view> firstAvailableFamily(std::span<std::string_view const> families, IsInstalled const& isInstalled);`
- Produces (`config/ChromeTokenGroups.hpp`):
  - `enum class TabShape : uint8_t { Flat = 0, Pill, Joined };` — `Joined`: tabs top-rounded, bottom-aligned in the
    bar, the active one filled with the content's colour (R-k).
  - `SizeTokens::stripButton` (height of `+` and `▾` in the `Joined` strip; Fluent 30), `SizeTokens::profileButton`
    (width of `▾` there; Fluent 28), `SpaceTokens::tabStripLeading` (Fluent 8). All three are read by the `Joined`
    shape only and are 0 on the other rows, as `tabWell` is 0 where it is unread.
  - `FluentRow`: `size.tabIcon = 16`, `size.tabClose = 24`, `radii.tabClose = 4`, `layout.tabShape = TabShape::Joined`.
    Its other values already match the canvas and stay.
- Produces (`window/UiStyleProvider`): `resolveChromeFont` returns, on a row whose `fontFamilies` is non-empty,
  `QGuiApplication::font()` with the family set to `firstAvailableFamily(row.fontFamilies, [](auto f){ return QFontDatabase::hasFamily(QString::fromUtf8(f)); })`,
  or the platform font unchanged when none is installed.

- [ ] **Step 1: Write the failing tests.**
  - `UiStyle_test.cpp` (`[uistyle]`):
    - `UiStyle.fluentRowMatchesTheWinBoard`: `uiStyleTokens(Fluent)` has `chromeHeightUnits == 40`,
      `tabHeightUnits == 34`, `windowControlUnits == 46`, `stripButtonUnits == 36`, and
      `std::ranges::equal(fontFamilies, FluentFontFamilies)`.
    - `UiStyle.nativeRowsKeepTodaysGeometry` (`:204`): now over `{ MacOS, ContourLinux }` only, unedited otherwise.
    - `UiStyle.firstAvailableFamily`: `{ "A", "B" }` with only `"B"` installed gives `"B"`; with both, `"A"`; with
      neither, `std::nullopt`; an empty span gives `std::nullopt`.
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`):
    - extend `numbersOf` and `everyRowMatchesTheDesignKit` with `stripButton`, `profileButton`, `tabStripLeading`
      in declaration order, every row's value;
    - `chromeTokens.fluentRowMatchesTheWinBoard`: `resolveChromeTokens(Fluent, {})` has `size.titleBar == 40`,
      `size.tab == 34`, `size.tabIcon == 16`, `size.tabClose == 24`, `size.stripButton == 30`,
      `size.profileButton == 28`, `space.tabStripLeading == 8`, `radii.tab == 6`, `radii.tabClose == 4`,
      `radii.paneCard == 4`, `radii.control == 4`, `space.paneGap == 4`, `type.body == 14`, `type.caption == 12`,
      `layout.tabShape == TabShape::Joined`, `layout.profileMenu == ProfileMenuAffordance::Caret`;
    - `chromeTokens.onlyTheMacRowIsAPill` (1c) still holds; new `chromeTokens.onlyTheFluentRowJoins`.
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`): "the Fluent chrome font is the first installed Fluent family":
    on this Mac neither is installed, so `resolveChromeFont(config, Fluent, "main") == QGuiApplication::font()`;
    the family choice itself is `firstAvailableFamily`'s test.
  - `MainWindowQml_test.cpp`, 1c's "the Fluent and ContourLinux rows keep today's geometry (offscreen)" splits:
    ContourLinux keeps every assertion; a new "the Fluent row takes the win board's geometry (offscreen)" asserts
    `titleBar.height == 40`, a TabItem's `height == 34`, and `windowControl_maximize.width == 46`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on `fontFamilies`, `TabShape::Joined` and the new fields.
- [ ] **Step 3: Implement** the fields, the rows and the font pick. QML compares `tabShape` as an `int`; until
  Task 8, `Joined` renders as `Flat` (Task 8 adds its branch).
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[uistyle],[chrometokens],[titleband],[tabwell]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[contour][gui][qml][mainwindow],[contour][gui][qml][titlebar]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Format, tidy and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/UiStyle.hpp src/contour/config/UiStyle_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/UiStyleProvider.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/test/MainWindowQml_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/UiStyleProvider.cpp
git add src/contour/config/UiStyle.hpp src/contour/config/UiStyle_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/UiStyleProvider.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/test/MainWindowQml_test.cpp
git commit -m "contour: the Fluent row takes the Windows 11 board's geometry and font" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: FluentWinUI3 for the Fluent row

**Files:**
- Modify: `src/contour/config/UiStyle.hpp` — `FluentTokens.quickControlsStyle = "FluentWinUI3"`.
- Modify: `src/contour/ContourGuiApp.cpp:2315-2332` — the comment that pins Fusion now says which row takes which
  style and why FluentWinUI3 is safe for the hand-drawn chrome (every chrome control sets its own `background` and
  `contentItem`, and `ContourMenu.qml` sets its palette per colour group).
- Modify: `src/contour/CMakeLists.txt` (after `contour_gui_test`'s `set_tests_properties`, `:965-969`) — a ctest
  `contour_gui_test_fluent`: `contour_gui_test "[contour][gui][qml]"` with
  `CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3;QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=detect_leaks=0;${CONTOUR_QML_TEST_ENVIRONMENT}`.
- Modify: `scripts/run-chrome-snapshots.sh:33-41` — a third pass under `CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3`;
  the header comment says three passes.
- Modify: `.github/workflows/build.yml` — after "Check that the package ships no benchmark tool" (`:1236-1262`), a
  step "Check that the package ships the FluentWinUI3 style": the zip must hold an entry ending in
  `qml/QtQuick/Controls/FluentWinUI3/qmldir` (`QQuickStyle::setStyle` names it at run time, which `windeployqt
  --qmldir` cannot see), with the same "no contour.exe → fail" guard.
- Modify, if Step 4 shows QML diagnostics: the chrome QML a diagnostic names (each fix sets the control's own
  `background`/`contentItem`, or reads the palette as `ContourMenu.qml` does).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`.

**Interfaces:**
- Consumes: Task 2's `FluentTokens`.
- Produces: `rowsRenderableUnder("FluentWinUI3") == { Fluent }`; `rowsRenderableUnder("Fusion") == { MacOS, ContourLinux }`.

- [ ] **Step 1: Write the failing test.** `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): "each controls style
  renders its own rows": the two equalities above, and `rowsRenderableUnder("ContourTui") == { Terminal }`.
- [ ] **Step 2: Run it and see it fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test && out/appleclang-debug/src/contour/contour_test "[snapshot]"`

  Expected: FAIL, Fluent is under Fusion.
- [ ] **Step 3: Implement** the token, the ctest, the snapshot pass and the CI step. The style module is on this
  Mac at `/opt/homebrew/share/qt/qml/QtQuick/Controls/FluentWinUI3` (Qt 6.11.1), so the run below is real here.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test
ctest --test-dir out/appleclang-debug -R "contour_gui_test_fluent" --output-on-failure
editorconfig-checker scripts/run-chrome-snapshots.sh src/contour/CMakeLists.txt .github/workflows/build.yml
```

  Expected: PASS, and `test_main.cpp`'s run-wide gate reports no QML diagnostic under FluentWinUI3.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/UiStyle.hpp src/contour/ContourGuiApp.cpp src/contour/test/ChromeSnapshotPlan_test.cpp
git add src/contour/config/UiStyle.hpp src/contour/ContourGuiApp.cpp src/contour/CMakeLists.txt scripts/run-chrome-snapshots.sh .github/workflows/build.yml src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/qml
git commit -m "contour: the Fluent row's controls use FluentWinUI3, with a test run and a snapshot pass under it" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

**Verified by:** CI `Windows` job, step "Check that the package ships the FluentWinUI3 style", and step `Test`
(ctest `contour_gui_test_fluent`); also the Linux and macOS jobs' `Test`.

---
### Task 4: The caption hit-test and the non-client message table (pure)

**Files:**
- Create: `src/contour/window/CaptionHitTest.hpp`.
- Test: `src/contour/window/CaptionHitTest_test.cpp`, added to `contour_test` beside `window/TitleBandLayout_test.cpp`
  (`src/contour/CMakeLists.txt:1205`).

**Interfaces:**
- Consumes: `config::WindowControlKind`, `config::WindowControlCount` (`WindowControlStyle.hpp`).
- Produces (namespace `contour::window`, Qt-free):

```cpp
struct LogicalPoint { double x = 0; double y = 0; };
struct LogicalRect  { double x = 0; double y = 0; double width = 0; double height = 0;
                      [[nodiscard]] constexpr bool contains(LogicalPoint p) const noexcept; // half-open
                      bool operator==(LogicalRect const&) const = default; };
struct PhysicalPoint { int x = 0; int y = 0; };

/// Where the window stands, as far as its caption cares.
enum class WindowPlacement : uint8_t { Windowed = 0, Maximized, FullScreen };

/// The caption buttons' scene rectangles, by WindowControlKind; empty where a button is not shown.
struct CaptionLayout { std::array<std::optional<LogicalRect>, config::WindowControlCount> buttons {};
                       double windowWidth = 0;   ///< The window's logical width.
                       double resizeBorder = 0;  ///< ResizeBorder.qml's thickness (UiStyleProvider::resizeBorderThickness).
                       bool operator==(CaptionLayout const&) const = default; };

/// What WM_NCHITTEST answers.
enum class CaptionHit : uint8_t { PassThrough = 0, MaximizeButton };

/// How the maximise button looks while the OS drives it.
enum class CaptionButtonVisual : uint8_t { Rest = 0, Hover, Pressed };

/// The non-client mouse messages the filter handles.
enum class NcEvent : uint8_t { MouseMove = 0, MouseLeave, LeftDown, LeftDoubleClick, LeftUp };
enum class NcEffect : uint8_t { None = 0, TrackLeave, ToggleMaximize };
enum class NcDisposition : uint8_t { DefaultHandling = 0, Swallow };

struct NcTransition { CaptionButtonVisual visual = CaptionButtonVisual::Rest; NcEffect effect = NcEffect::None;
                      NcDisposition disposition = NcDisposition::DefaultHandling;
                      bool operator==(NcTransition const&) const = default; };

/// One row: an event, whether it lands on the maximise button, the visual before; and what follows.
struct NcTransitionRow { NcEvent event; CaptionHit hit; std::optional<CaptionButtonVisual> before; NcTransition after; };
inline constexpr auto CaptionTransitions = std::array { /* the table below */ };

[[nodiscard]] constexpr LogicalPoint toLogical(PhysicalPoint client, double devicePixelRatio) noexcept;
[[nodiscard]] constexpr CaptionHit captionHitAt(LogicalPoint point, CaptionLayout const& layout,
                                                WindowPlacement placement) noexcept;
[[nodiscard]] constexpr NcTransition onNcEvent(NcEvent event, CaptionHit hit, CaptionButtonVisual before) noexcept;
```

  - `captionHitAt` is `MaximizeButton` when the maximise rectangle contains the point, except that under
    `Windowed` a point with `y < layout.resizeBorder` is `PassThrough` (R-d). Maximised and full screen, a
    rectangle whose top is 0 also covers `y < 0`, and one whose trailing edge is `windowWidth` also covers
    `x ≥ windowWidth`, so the screen's edge pixel hits.
  - `onNcEvent` takes the first row matching `(event, hit, before)`, `before == nullopt` matching any; no match
    gives `NcTransition { .visual = before }` with default handling. The rows, in order:

    | event | hit | before | visual | effect | disposition |
    |---|---|---|---|---|---|
    | MouseMove | Maximize | Rest | Hover | TrackLeave | Default |
    | MouseMove | Maximize | any | (unchanged: Hover or Pressed) | None | Default |
    | MouseMove | PassThrough | any | Rest | None | Default |
    | MouseLeave | any | any | Rest | None | Default |
    | LeftDown | Maximize | any | Pressed | None | Swallow |
    | LeftDoubleClick | Maximize | any | Pressed | None | Swallow |
    | LeftUp | Maximize | Pressed | Hover | ToggleMaximize | Swallow |
    | LeftUp | Maximize | any | Hover | None | Swallow |
    | LeftUp | PassThrough | any | Rest | None | Default |

    The "unchanged" row is written as two rows (Hover → Hover, Pressed → Pressed), so every row is data.

- [ ] **Step 1: Write the failing tests** (`[captionhit]`), with `layout` holding Minimize `{ 868, 0, 46, 40 }`,
  Maximize `{ 914, 0, 46, 40 }`, Close `{ 960, 0, 46, 40 }`, `windowWidth = 1006`, `resizeBorder = 4`:
  - `captionHit.maximizeOnly` (Review Focus 1): every point of the maximise rectangle at `y ≥ 4` is
    `MaximizeButton`; points in the minimise and close rectangles, the drag region (`{ 400, 20 }`) and the content
    (`{ 400, 300 }`) are `PassThrough`; an empty maximise slot gives `PassThrough` everywhere.
  - `captionHit.windowedKeepsTheResizeStrip` (Review Focus 2): `{ 930, 2 }` is `PassThrough` under `Windowed` and
    `MaximizeButton` under `Maximized` and `FullScreen`.
  - `captionHit.maximizedReachesTheEdge` (Review Focus 2): under `Maximized`, `{ 930, 0 }` and `{ 930, -1 }` are
    `MaximizeButton`; with Maximize moved to the trailing edge (`{ 960, 0, 46, 40 }`), `{ 1005, 10 }` and
    `{ 1006, 10 }` are too.
  - `captionHit.sameZoneAtEveryScale` (Review Focus 5): for dpr in `{ 1.0, 1.25, 1.5, 2.0 }` and each probe
    `{ 937, 20 }`, `{ 890, 20 }`, `{ 980, 20 }`, `{ 400, 300 }`,
    `captionHitAt(toLogical({ int(probe.x · dpr), int(probe.y · dpr) }, dpr), …)` equals the dpr-1 answer;
    `toLogical({ 3, 3 }, 1.5) == LogicalPoint { 2, 2 }`.
  - `captionTransitions.hoverThenLeave`: `onNcEvent(MouseMove, MaximizeButton, Rest) == { Hover, TrackLeave, DefaultHandling }`;
    then `(MouseMove, MaximizeButton, Hover)` gives `{ Hover, None, DefaultHandling }`; then `(MouseLeave, PassThrough, Hover)`
    gives `{ Rest, None, DefaultHandling }`.
  - `captionTransitions.clickTogglesOnce` (Review Focus 1): `(LeftDown, MaximizeButton, Hover)` gives
    `{ Pressed, None, Swallow }`; `(LeftUp, MaximizeButton, Pressed)` gives `{ Hover, ToggleMaximize, Swallow }`;
    a second `(LeftUp, MaximizeButton, Hover)` gives no toggle.
  - `captionTransitions.dragOffCancels` (Review Focus 1): Down on the button, `(MouseMove, PassThrough, Pressed)`
    → `Rest`, then `(LeftUp, PassThrough, Rest)` → `{ Rest, None, DefaultHandling }`; and Down, `MouseLeave`,
    `(LeftUp, MaximizeButton, Rest)` → no toggle.
  - `captionTransitions.doubleClickIsAPress`: `(LeftDoubleClick, MaximizeButton, Hover)` → `{ Pressed, None, Swallow }`
    (so a double click on the button is one toggle, not the caption's maximise).
  - `captionTransitions.everyRowIsReachable`: for each row of `CaptionTransitions`, `onNcEvent` on its key with a
    concrete `before` (its own, or each of the three when `nullopt`) returns the row's `after` unless an earlier row
    matches; the count of rows comes from `CaptionTransitions.size()`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `window/CaptionHitTest.hpp` not found.
- [ ] **Step 3: Implement** the header as stated.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[captionhit]"`

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/CaptionHitTest.hpp src/contour/window/CaptionHitTest_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/window/CaptionHitTest.hpp src/contour/window/CaptionHitTest_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: decide the caption's non-client hit and button state from tables" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: `platform::INativeCaptionButtons`, its null implementation, and the controller's side

**Files:**
- Create: `src/contour/platform/NativeCaptionButtons.hpp`, `src/contour/platform/NativeCaptionButtons.cpp` (the
  factory: Windows branch filled by Task 6, null elsewhere); list both in `add_library(contour_platform …)`
  (`platform/CMakeLists.txt:15-72`). `contour_platform` already links `contour_config`; `CaptionHitTest.hpp` is
  header-only under `src/`.
- Modify: `src/contour/platform/AppearanceServices.hpp:17-50` — `.caption`; `nullAppearanceServices()` names it.
- Modify: `src/contour/test/AppearanceFixtures.hpp` — `CaptionRecord`, `RecordingCaptionButtons`, and every
  `appearanceServicesWith` overload names `.caption` (the recording one where a record is given, else null).
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}` — `_captionButtons`, `captionButtons()`, built after
  `_titleBandMaterial` (Step 1d).
- Modify: `src/contour/window/WindowController.{hpp,cpp}` — attach in `bindWindow`, detach when the OS window goes,
  the rect slot, the visual property; `MainWindowControllerMock.hpp` and `MockMainController.hpp` mirror them.
- Test: new `src/contour/platform/NativeCaptionButtons_test.cpp` (`contour_gui_test`, beside
  `platform/NativeWindowFrame_test.cpp`, `src/contour/CMakeLists.txt:802`); new
  `src/contour/window/CaptionController_test.cpp` in `contour_gui_test_heavy_sources` (it builds a `TestApp`).

> **Depends on Step 1d:** `AppearanceServices::titleBand`, the three-switch `appearanceServicesWith`, and
> `ContourGuiApp::titleBandMaterial()` are 1d Task 3's.

**Interfaces:**
- Consumes: Task 4's `CaptionLayout`, `CaptionButtonVisual`, `WindowPlacement`.
- Produces (`platform/NativeCaptionButtons.hpp`, namespace `contour::platform`):

```cpp
/// What a window lends the caption adapter: where its buttons are, and what to do when the OS drives one.
struct CaptionBinding
{
    std::function<window::CaptionLayout()> layout;              ///< The buttons' scene rectangles now.
    std::function<void(window::CaptionButtonVisual)> visual;    ///< The maximise button's new look.
    std::function<void()> toggleMaximize;                       ///< Maximise or restore, as the QML button does.
};

/// Hands a client-drawn window's maximise button to the OS, so Windows 11 shows Snap Layouts over it.
class INativeCaptionButtons
{
  public:
    virtual ~INativeCaptionButtons() = default;
    /// Starts answering @p window's non-client messages from @p binding. Re-attaching replaces the binding.
    /// A window with no platform window yet is remembered and attached on its first expose by the caller.
    /// @pre GUI thread.
    virtual void attach(QWindow& window, CaptionBinding binding) = 0;
    /// Stops; a window never attached is ignored. @pre GUI thread.
    virtual void detach(QWindow& window) = 0;
};

class NullNativeCaptionButtons final: public INativeCaptionButtons { /* both do nothing */ };

/// @return The Win32 adapter under the Windows QPA, otherwise the null one; never null.
[[nodiscard]] std::unique_ptr<INativeCaptionButtons> makeNativeCaptionButtons();
```

- Produces (`AppearanceServices`): `std::function<std::unique_ptr<INativeCaptionButtons>()> caption = makeNativeCaptionButtons;`
- Produces (`test/AppearanceFixtures.hpp`): `struct CaptionRecord { std::vector<QWindow*> attached {}; std::vector<QWindow*> detached {}; std::optional<platform::CaptionBinding> binding {}; };`,
  `class RecordingCaptionButtons final: public platform::INativeCaptionButtons`, and
  `[[nodiscard]] platform::AppearanceServices appearanceServicesWith(AccessibilitySwitch&, AppearanceSwitch&, TitleBandRecord&, CaptionRecord&);`
- Produces (`ContourGuiApp`): `[[nodiscard]] platform::INativeCaptionButtons& captionButtons() noexcept;`
- Produces (`WindowController`):
  - `Q_INVOKABLE void setCaptionButtonRect(int kind, qreal x, qreal y, qreal width, qreal height);` and
    `Q_INVOKABLE void clearCaptionButtonRect(int kind);` — `kind` is `int(config::WindowControlKind)`; out-of-range
    kinds are ignored. Stored in `_captionLayout` (`window::CaptionLayout`); the binding's `layout()` fills in
    `windowWidth` (the OS window's width) and `resizeBorder` (the published provider's `resizeBorderThickness()`)
    when it is called.
  - `Q_PROPERTY(int nativeMaximizeVisual READ nativeMaximizeVisual NOTIFY nativeMaximizeVisualChanged)` —
    `int(CaptionButtonVisual)`, `Rest` until the adapter says otherwise.
  - `bindWindow` attaches `CaptionBinding { .layout = [this]{ return _captionLayout; }, .visual = …set and notify…, .toggleMaximize = [this]{ toggleMaximized(); } }`
    only when `framePolicy().shadow == FrameShadowStrategy::NativeFrameKept` (the Windows client frame);
    `setTitleBarVisible(true)` detaches, `false` re-attaches; the destructor and `SurfaceAboutToBeDestroyed`
    detach.

- [ ] **Step 1: Write the failing tests.**
  - `NativeCaptionButtons_test.cpp` (`[caption-buttons]`): "the null adapter ignores attach and detach"; "the factory
    is null off the Windows QPA" (`dynamic_cast<NullNativeCaptionButtons*>(makeNativeCaptionButtons().get())` under
    the suite's offscreen QPA, every OS); "the null services build the null adapter".
  - `CaptionController_test.cpp` (`[contour][caption]`), a `TestApp` with the four-switch services and a
    `ScopedControllerWithOsWindow`:
    - "a Windows client frame attaches, others do not": where `framePolicyFor(currentFramePlatform(), Client).shadow
      == NativeFrameKept`, `record.attached.size() == 1` after `bindWindow`; elsewhere (this Mac, Linux) it is empty.
      The cases below call the controller's binding builder directly (`controller.captionBinding()`, a private
      accessor exposed to the test through the existing `friend struct` test seam of `WindowController`, or a
      `[[nodiscard]] platform::CaptionBinding captionBinding();` documented as the adapter's seam), so they run on
      every OS.
    - "the controller forwards hover, press and toggle" (Review Focus 1): `binding.visual(Hover)` makes
      `nativeMaximizeVisual() == 1` with one `nativeMaximizeVisualChanged` (`QSignalSpy`); a repeat emits nothing;
      `binding.toggleMaximize()` maximises the OS window, a second restores it.
    - "the reported rectangles are what the layout answers": `setCaptionButtonRect(int(Maximize), 914, 0, 46, 40)`
      then `binding.layout().buttons[Maximize] == LogicalRect { 914, 0, 46, 40 }`; `clearCaptionButtonRect` empties
      it; `setCaptionButtonRect(7, …)` changes nothing.
    - "a scale change keeps the reported rectangles logical" (Review Focus 5): after
      `QEvent::DevicePixelRatioChange` on the window, `binding.layout()` is unchanged.
    - "show_title_bar detaches and re-attaches" (Windows frame hosts only, guarded as the first case).
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, `platform/NativeCaptionButtons.hpp` not found.
- [ ] **Step 3: Implement** the interface, the null object, the factory (`#ifdef _WIN32` calls Task 6's
  `makeWin32CaptionButtons()` — until Task 6 that branch returns the null object too), the services field, the
  fixtures, the app member, the controller members and the two mocks.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[caption-buttons],[contour][caption],[appearance-source],[contour][app][appearance],[contour][frame],[contour][multiwindow]"`

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/NativeCaptionButtons*.{hpp,cpp} src/contour/platform/AppearanceServices.hpp src/contour/test/AppearanceFixtures.hpp src/contour/ContourGuiApp.{hpp,cpp} src/contour/window/WindowController.{hpp,cpp} src/contour/window/CaptionController_test.cpp src/contour/test/MainWindowControllerMock.hpp src/contour/test/MockMainController.hpp
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/platform/NativeCaptionButtons.cpp src/contour/window/WindowController.cpp
editorconfig-checker src/contour/CMakeLists.txt src/contour/platform/CMakeLists.txt
git add src/contour/platform/NativeCaptionButtons* src/contour/platform/AppearanceServices.hpp src/contour/platform/CMakeLists.txt src/contour/test/AppearanceFixtures.hpp src/contour/ContourGuiApp.* src/contour/window/WindowController.* src/contour/window/CaptionController_test.cpp src/contour/test/MainWindowControllerMock.hpp src/contour/test/MockMainController.hpp src/contour/CMakeLists.txt
git commit -m "platform: a native caption-button interface with a null implementation; the controller lends it its buttons" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: The Win32 caption adapter — Snap Layouts (Windows-only)

**Files:**
- Create: `src/contour/platform/Win32CaptionButtons.hpp`, `src/contour/platform/Win32CaptionButtons.cpp` (wholly
  inside `#ifdef _WIN32`, listed unconditionally in `add_library(contour_platform …)` as `Win32WindowFrame.cpp` is).
- Modify: `src/contour/platform/NativeCaptionButtons.cpp` — the `_WIN32` branch.
- Modify: `src/contour/CMakeLists.txt:973-983` — a second Windows ctest `contour_windows_native_scaled_test`:
  `contour_gui_test "[win32][hittest]"` with the native test's environment plus `QT_SCALE_FACTOR=1.5`,
  `RUN_SERIAL TRUE`, `TIMEOUT 120`, `LABELS "native;windows"`.
- Modify: `.github/workflows/build.yml` — after "verify the PowerShell integration tests actually ran"
  (`:1189-1215`), a step "verify the Win32 chrome tests actually ran": runs
  `out/build/$env:CMAKE_PRESET/bin/contour_gui_test.exe "[win32]"` with `QT_QPA_PLATFORM=windows`,
  `QT_QUICK_BACKEND=software`, `CONTOUR_TEST_DISPLAY=1`, and fails when the output matches `skipped` or the binary
  is missing (the `[pwsh]` step's shape).
- Test: `src/contour/platform/Win32CaptionButtons_test.cpp`, in `contour_gui_test`'s sources (unconditional; its
  body is inside `#ifdef _WIN32`).

**Interfaces:**
- Consumes: Task 4 (`toLogical`, `captionHitAt`, `onNcEvent`), Task 5 (`INativeCaptionButtons`, `CaptionBinding`).
- Produces: `[[nodiscard]] std::unique_ptr<INativeCaptionButtons> makeWin32CaptionButtons();` — `nullptr` unless
  `platformName() == "windows"`. The adapter:
  - owns one `QAbstractNativeEventFilter` (process-scoped, as `frameEventFilter()` is, `Win32WindowFrame.cpp:147-156`)
    and a map `HWND → { QPointer<QWindow>, CaptionBinding, CaptionButtonVisual }`, re-keyed when the window's
    `winId()` changes (looked up on `attach` and on each `QPlatformSurfaceEvent::SurfaceCreated` it observes);
  - `WM_NCHITTEST`: `ScreenToClient` the `GET_X_LPARAM/GET_Y_LPARAM` point, `toLogical(…, window.devicePixelRatio())`,
    `captionHitAt(…, binding.layout(), placementOf(window))`; `MaximizeButton` →
    `*result = HTMAXBUTTON; return true;`, else `return false`. `placementOf`: `IsZoomed` → `Maximized`,
    `Qt::WindowFullScreen` → `FullScreen`, else `Windowed`.
  - `WM_NCMOUSEMOVE`, `WM_NCMOUSELEAVE`, `WM_NCLBUTTONDOWN`, `WM_NCLBUTTONDBLCLK`, `WM_NCLBUTTONUP` → `NcEvent`;
    `wParam == HTMAXBUTTON` → `CaptionHit::MaximizeButton`; `onNcEvent` decides. A changed visual calls
    `binding.visual`; `TrackLeave` calls `TrackMouseEvent` for a non-client mouse-leave notification; `ToggleMaximize`
    posts `binding.toggleMaximize` through `QMetaObject::invokeMethod(window, …, Qt::QueuedConnection)` so the
    window is not resized inside its own message; `Swallow` → `*result = 0; return true;`.

- [ ] **Step 1: Write the failing tests** (Windows-only, inside `#ifdef _WIN32`, each starting with the
  `platformName() != "windows"` `SKIP`), tag `[native][win32][caption]`, plus `[hittest]` on the first three:
  - "WM_NCHITTEST over the maximise button answers HTMAXBUTTON" (Review Focus 1): a 1006 × 600 `QWindow` with the
    Win32 frame applied (`makeWin32WindowFrame(...)->apply(window, WindowDecoration::Client)`, as
    `NativeWindowFrame_test.cpp:32-36` does), attached with the Task 4 layout; for the maximise centre, the minimise
    centre and `{ 400, 300 }` (logical), converted to screen pixels with `devicePixelRatio()` and `ClientToScreen`,
    `SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(x, y))` is `HTMAXBUTTON`, `HTCLIENT`, `HTCLIENT`.
  - "the maximised window's edge pixel over the maximise button answers HTMAXBUTTON" (Review Focus 2): maximised,
    with the maximise rectangle at the trailing edge, the screen point `(work.right − 1, work.top)` answers
    `HTMAXBUTTON`.
  - "the scaled run hits the same button" (Review Focus 5): the first case's three probes, unchanged; under
    `contour_windows_native_scaled_test` the window's `devicePixelRatio()` is 1.5, which the case `CHECK`s when
    `qEnvironmentVariable("QT_SCALE_FACTOR") == "1.5"`.
  - "a non-client click toggles the window once" (Review Focus 1): `WM_NCMOUSEMOVE(HTMAXBUTTON)` then
    `WM_NCLBUTTONDOWN(HTMAXBUTTON)` then `WM_NCLBUTTONUP(HTMAXBUTTON)` via `SendMessageW`, then
    `QCoreApplication::processEvents()`: the toggle callback ran once; visuals reported were `Hover, Pressed, Hover`;
    both button messages returned 0 (swallowed).
  - "press, leave, release does not toggle": `DOWN`, `WM_NCMOUSELEAVE`, `UP` → no toggle; last visual `Hover`
    after the `UP` (the pointer is over it again) and `Rest` after the leave.
  - "detach stops answering": after `detach`, the maximise centre answers `HTCLIENT`.
- [ ] **Step 2: See them fail.** Push; the `Windows` job's `Test` fails at link (`makeWin32CaptionButtons`
  undefined) or on the cases. Here, only `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`
  runs (the file compiles to nothing).
- [ ] **Step 3: Implement** the adapter, the factory branch, the ctest and the CI step. Document at the filter why
  only the maximise button is non-client (R-b) and why the toggle is queued.
- [ ] **Step 4: See them pass.** Push; wait for `Windows`. Also run here:
  `out/appleclang-debug/src/contour/contour_test "[captionhit]" && out/appleclang-debug/src/contour/contour_gui_test "[caption-buttons],[contour][caption]"`.

  Expected: PASS here; on CI, `contour_windows_native_test` and `contour_windows_native_scaled_test` pass and the
  new step reports the `[win32]` cases ran with none skipped.
- [ ] **Step 5: Commit** (before the push of Step 2; fix-ups after CI are further commits of this task, squashed
  with `/absorb` before review).

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/Win32CaptionButtons*.{hpp,cpp} src/contour/platform/NativeCaptionButtons.cpp
editorconfig-checker src/contour/CMakeLists.txt src/contour/platform/CMakeLists.txt .github/workflows/build.yml
git add src/contour/platform/Win32CaptionButtons* src/contour/platform/NativeCaptionButtons.cpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt .github/workflows/build.yml
git commit -m "platform: the Win32 caption answers HTMAXBUTTON over the maximise button, so Snap Layouts appear" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

**Verified by:** CI `Windows` job, step `Test` — ctest `contour_windows_native_test` (`[native]`) and
`contour_windows_native_scaled_test` (`[win32][hittest]`, `QT_SCALE_FACTOR=1.5`); step "verify the Win32 chrome
tests actually ran" (`[win32]`, no `skipped`).

---
### Task 7: The caption buttons' look — Fluent glyphs, fills, the inactive window, the Terminal row's ● cells

**Files:**
- Modify: `src/contour/config/WindowControlStyle.hpp` — `WindowControlTokens` (`:85-…`) gains the icon font and
  glyphs and the wash; `WindowsTokens` (`:153-163`) and `PlasmaTokens` (`:190-195`) name them; new pure
  `controlGlyphsFor`, `presentationFor`, `DotRoles`.
- Modify: `src/contour/window/WindowControlStyleProvider.{hpp,cpp}` — constructed with the chrome row; each
  `buttons` entry gains `font`, `glyph`/`restoreGlyph` from `controlGlyphsFor`, `wash`, and `dotRole`;
  `presentation` from `presentationFor`.
- Modify: `src/contour/ContourGuiApp.cpp:2407-2408`, `:1757-1772` — pass `_chromeStyle` and the font predicate.
- Modify: `src/contour/qml/WindowControls.qml` — glyph font per button; hover/pressed fills from the provider's
  `wash`; `win.nativeMaximizeVisual` lights the maximise button; inactive window alpha; dot colours from
  `chromeStyle.statusInk(dotRole)` on the Terminal row; report each slot's scene rectangle.
- Test: `src/contour/config/WindowControlStyle_test.cpp`, `src/contour/window/WindowControlStyleProvider_test.cpp`,
  `src/contour/test/QmlComponents_test.cpp` (the `[windowcontrols]` cases at `:5456-5640`).

**Interfaces:**
- Consumes: Task 5's `setCaptionButtonRect`, `clearCaptionButtonRect`, `nativeMaximizeVisual`; `config::FillAlphas`,
  `LabelAlphas` (`ChromeTokenGroups.hpp:157-170`); `config::StatusColorRole`.
- Produces (`config/WindowControlStyle.hpp`):
  - `enum class ControlWash : uint8_t { Highlight = 0, KitFill };` — `Highlight` is today's `hoverWashPercent` of
    the palette highlight; `KitFill` is `FillAlphas[3]` (black 5 % / white 6 %). `WindowsTokens` take `KitFill`,
    `PlasmaTokens` and `MacOsTokens` `Highlight`.
  - `WindowControlTokens` gains `std::span<std::string_view const> iconFamilies;`, `std::array<std::string_view, WindowControlCount> iconGlyphs;`,
    `std::string_view iconRestoreGlyph;`, `ControlWash wash;`. `WindowsTokens`: `iconFamilies = { "Segoe Fluent Icons", "Segoe MDL2 Assets" }`
    (an `inline constexpr std::array` `CaptionIconFamilies`), `iconGlyphs = { "", "", "" }`
    (Close, Minimize, Maximize: `WindowControlKind` order), `iconRestoreGlyph = ""`. `PlasmaTokens` sets
    `iconFamilies = {}` (R-f); `MacOsTokens` names empty ones.
  - `struct ControlGlyphs { std::string_view family {}; std::array<std::string_view, WindowControlCount> glyphs {}; std::string_view restore {}; bool operator==(ControlGlyphs const&) const = default; };`
    — `family` empty means the chrome font.
  - `template <std::predicate<std::string_view> IsInstalled> [[nodiscard]] constexpr ControlGlyphs controlGlyphsFor(WindowControlTokens const& tokens, IsInstalled const& isInstalled);`
    — the first installed icon family with the icon glyphs, else `{ "", tokens.glyphs, tokens.restoreGlyph }`
    (uses Task 2's `firstAvailableFamily`).
  - `[[nodiscard]] constexpr WindowControlPresentation presentationFor(WindowControlTokens const& tokens, ChromeStyle row) noexcept;`
    — `TrafficLight` on the Terminal row (R-n), else `tokens.presentation`.
  - `inline constexpr std::array<StatusColorRole, WindowControlCount> DotRoles { Danger, Warning, Success };`
    (Close, Minimize, Maximize).
- Produces (`WindowControlStyleProvider`): `explicit WindowControlStyleProvider(config::WindowControlStyle resolved, config::ChromeStyle row, std::function<bool(std::string_view)> isFontInstalled, QObject* parent = nullptr);`
  `setStyle` keeps its signature. Each `buttons` entry (`QVariantMap`) gains `font` (QString), `wash` (int),
  `dotRole` (int, `-1` off the Terminal row). `presentation` reports `presentationFor`.
- Produces (QML, `WindowControls.qml`): each slot calls `root.controller.setCaptionButtonRect(kind, …)` with its
  `mapToItem(null, 0, 0, width, height)` on `Component.onCompleted` and on its own and the window's geometry
  changes (a `Connections` on `root.window`'s `widthChanged`/`heightChanged`), and `clearCaptionButtonRect(kind)`
  when it becomes invisible or is destroyed. The maximise slot's hover is `control.hovered || root.controller.nativeMaximizeVisual >= 1`,
  its pressed `control.pressed || root.controller.nativeMaximizeVisual === 2`. Glyphs, washes and dots read the
  provider; while `!root.window.active`, glyphs take `label.tertiary`. No literal.

- [ ] **Step 1: Write the failing tests.**
  - `WindowControlStyle_test.cpp` (`[windowcontrols]`):
    - "the Windows row draws Segoe Fluent glyphs where installed": `controlGlyphsFor(windowControlTokens(Windows), installed{"Segoe MDL2 Assets"})`
      is `{ "Segoe MDL2 Assets", { "", "", "" }, "" }`; with both installed the family is
      `"Segoe Fluent Icons"`; with none it is `{ "", { "✕", "—", "▢" }, "❐" }`.
    - "Breeze never borrows the Segoe glyphs": `controlGlyphsFor(windowControlTokens(Plasma), everythingInstalled).family.empty()`.
    - "the Terminal row draws dots on every host": `presentationFor(windowControlTokens(s), Terminal) == TrafficLight`
      for every style; `presentationFor(windowControlTokens(Windows), Fluent) == Button`.
    - "the Windows row washes with the kit fill": `windowControlTokens(Windows).wash == ControlWash::KitFill`;
      Plasma and MacOS `Highlight`; Plasma differs from Windows in exactly the fields its comment names (update
      that comment-test pair, `:186-189`).
  - `WindowControlStyleProvider_test.cpp` (`[windowcontrols]`): built with `(Windows, Terminal, noFonts)`:
    `presentation() == "trafficLight"`, the close entry's `dotRole == int(StatusColorRole::Danger)`, the maximise
    entry's `int(Success)`; built with `(Windows, Fluent, segoeFluent)`: the close entry's `font == "Segoe Fluent Icons"`
    and `glyph == ""`, `dotRole == -1`.
  - `QmlComponents_test.cpp` (`[contour][gui][qml][titlebar][windowcontrols]`), Fluent row, Windows controls:
    - "the native hover state lights the maximise button (offscreen)" (Review Focus 1): with the mock's
      `nativeMaximizeVisual = 1`, the maximise slot's fill item (`objectName: "windowControlFill_maximize"`) is
      visible with the kit fill's colour; with 0, transparent; with 2 the glyph's colour is `label.secondary`.
    - "the caption buttons report their rectangles (offscreen)": the mock records
      `setCaptionButtonRect(int(Maximize), …)` with the slot's scene rectangle (width 46, height 40); after the window
      grows by 100, the recorded `x` grows by 100; with `show_title_bar` the slot is gone and the mock recorded
      `clearCaptionButtonRect` for each kind.
    - "an inactive window dims the caption glyphs (offscreen)": the close glyph's colour alpha equals
      `LabelAlphas[2]`'s on an inactive window and the primary label alpha on an active one.
    - "a contrast theme drops the red close hover (offscreen)" (Review Focus 4) — written here, made to pass in
      Task 14: under the mock appearance's `ContrastTheme::Active`, the hovered close fill is the palette highlight
      wash, not `#C42B1C`.
    - Terminal row on a Windows host (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui` run): the three slots draw `●`
      cells in `statusInk(Danger/Warning/Success)`, trailing, in the order minimise, maximise, close; the maximise
      cell reports its rectangle too.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on `controlGlyphsFor` and the provider's constructor.
- [ ] **Step 3: Implement** the tokens, the pure functions, the provider and the QML. The contrast-theme case stays
  failing until Task 14; mark it `[!mayfail]` here and remove that tag in Task 14.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[windowcontrols],[uistyle]"
out/appleclang-debug/src/contour/contour_gui_test "[windowcontrols],[contour][caption]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][titlebar][windowcontrols]"
CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3 out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][titlebar][windowcontrols]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS (the contrast case reported as an allowed failure).
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/WindowControlStyle.hpp src/contour/config/WindowControlStyle_test.cpp src/contour/window/WindowControlStyleProvider.{hpp,cpp} src/contour/window/WindowControlStyleProvider_test.cpp src/contour/ContourGuiApp.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/MockMainController.hpp
editorconfig-checker src/contour/qml/WindowControls.qml
git add src/contour/config/WindowControlStyle* src/contour/window/WindowControlStyleProvider* src/contour/ContourGuiApp.cpp src/contour/qml/WindowControls.qml src/contour/test/QmlComponents_test.cpp src/contour/test/MockMainController.hpp
git commit -m "contour: Fluent caption buttons with Segoe glyphs and kit fills; the Terminal row's controls are scheme-coloured cells" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

**Verified by** (the glyph font is installed only there): CI `Windows` job, step `Test`, ctest `contour_gui_test`
case "the Windows row draws Segoe Fluent glyphs where installed" run through the provider with
`QFontDatabase::hasFamily` — add a `[native][win32][caption]` case asserting the provider built with the real
predicate reports `font == "Segoe Fluent Icons"` on the runner (Windows Server 2025 ships it; if the runner lacks
it, the case asserts `"Segoe MDL2 Assets"` and the ledger records which).

---

### Task 8: The joined tabs (`TabShape::Joined`)

> **Depends on Steps 1c and 2:** 1c's `TitleBandLayout.hpp` (`TitleBandRules`, `resolveTitleBand`), its `Flat`
> strip in `TabStrip.qml`/`TabItem.qml`, and its `UiStyleProvider::titleBand` invokable; Step 2's content backdrop
> behind the pane tree in `Main.qml` (painted with `colors.paneGapFill`). Locate each by these names.

**Files:**
- Modify: `src/contour/window/TitleBandLayout.hpp` — `TitleBandRules` gains the `Joined` rows (R-j).
- Modify: `src/contour/qml/TabStrip.qml`, `src/contour/qml/TabItem.qml`, `src/contour/qml/TitleBar.qml` — the
  `Joined` branch beside `Flat` and `Pill`; `Main.qml`'s content backdrop gets `objectName: "contentBackdrop"` if
  Step 2 did not name it.
- Test: `src/contour/window/TitleBandLayout_test.cpp`, `src/contour/test/MainWindowQml_test.cpp`.

**Interfaces:**
- Consumes: Task 2's tokens (`size.tab` 34, `size.stripButton` 30, `size.profileButton` 28,
  `stripButtonUnits` 36, `space.tabStripLeading` 8, `size.tabIcon` 16, `size.tabClose` 24, `radii.tab` 6,
  `radii.tabClose` 4, `radii.control` 4, the scale `s4`/`s6`, `contrast.separatorPercent`).
- Produces (`TitleBandLayout.hpp`): for `shape == Joined`, the `Pill` rows' answers with `centre = Tabs` in every
  windowed `ClientBand` row (one tab is still a tab, R-j); `NativeFrame` rows as `Flat`'s.
- Produces (QML): under `Joined` —
  - the strip is the band: tabs `size.tab` high, bottom-aligned in `size.titleBar`, after `space.tabStripLeading`;
  - a tab: top corners `radii.tab`, bottom square; insets `s6` leading, `s4` trailing, icon `size.tabIcon` then `s4`;
    close `size.tabClose` square, `radii.tabClose`, shown on the active and the hovered tab;
  - the active tab's fill binds to `contentBackdrop.color` (R-k); inactive tabs are transparent with a trailing
    1-px rule at `contrast.separatorPercent`; hover takes `fill.4`;
  - `+` is `stripButtonUnits` × `size.stripButton`, `▾` (the profile dropdown, which stays) `size.profileButton` ×
    `size.stripButton`, both `radii.control`, `s1` above the bar's bottom;
  - labels follow Task 7's inactive-window rule.
  Objects carry the existing names (`tabStrip`, `newTabButton`, `newTabMenuButton`, `tabCloseButton`, `tabLabel`)
  plus `tabShapeJoined` on the strip's `Joined` container.

- [ ] **Step 1: Write the failing tests.**
  - `TitleBandLayout_test.cpp` (`[titleband]`): `titleBand.joinedKeepsItsBar`:
    `{ Joined, ClientBand, Windowed, Hidden, Top }` → `{ Shown, Top, Tabs, Hidden }`;
    `{ Joined, ClientBand, Windowed, Shown, Bottom }` → the `Pill` row's answer with `centre = Tabs`;
    `{ Joined, NativeFrame, Windowed, Shown, Top }` → `Flat`'s; full screen turns a top `Shown` band into
    `RevealOnEdge`, as for every shape.
  - `MainWindowQml_test.cpp` (`[contour][gui][qml][mainwindow][joinedtabs]`), Fluent row, three tabs, under
    `CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3`:
    - "joined tabs sit on the band's floor (offscreen)": every TabItem `height == 34`, `y + height == titleBar.height`;
      the first tab's `x == 8`; the active tab's background `topLeftRadius == 6` and `bottomLeftRadius == 0`;
    - "the active tab is the content's colour (offscreen)": the active tab's fill equals
      `contentBackdrop.color`; an inactive tab's fill alpha is 0 at rest;
    - "the profile dropdown stays (offscreen)": `newTabMenuButton.visible`, its `width == 28`, `height == 30`;
      `newTabButton.width == 36`;
    - "one tab still shows the bar and its caption buttons (offscreen)" (R-j): one tab under `Multiple`:
      `titleBar.visible`, one TabItem, `windowControl_close.visible`.
  - The Terminal row and the ContourLinux row: the existing `[contour][gui][qml][mainwindow]` cases pass unedited.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_test "[titleband]"`

  Expected: FAIL on `titleBand.joinedKeepsItsBar`.
- [ ] **Step 3: Implement** the rows and the QML branch.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[titleband],[tabwell]"
CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3 out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][titlebar]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/TitleBandLayout.hpp src/contour/window/TitleBandLayout_test.cpp src/contour/test/MainWindowQml_test.cpp
editorconfig-checker src/contour/qml/TabStrip.qml src/contour/qml/TabItem.qml src/contour/qml/TitleBar.qml src/contour/qml/Main.qml
git add src/contour/window/TitleBandLayout* src/contour/qml/TabStrip.qml src/contour/qml/TabItem.qml src/contour/qml/TitleBar.qml src/contour/qml/Main.qml src/contour/test/MainWindowQml_test.cpp
git commit -m "contour: on Windows the tabs are the title bar and join the content" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 9: The backdrop decision (pure) — Fluent surface rows, the DWM backdrop, the margins, material support

> **Depends on Step 1d:** `config::BandBackdrop`, `config::BandFill`, `config::TitleBarGrowth`,
> `window::TitleBandSurfaceRows`, `resolveTitleBandSurface` and `platform::TitleBandRequest` are 1d Task 2's and
> Task 3's.

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp` — `MaterialSupport`, `BackdropKind` beside `Material`
  (`:19-26`); the three-argument `resolveChromeTokens` (`:810-…`).
- Modify: `src/contour/window/TitleBandSurface.hpp` — two Fluent rows.
- Modify: `src/contour/platform/TitleBandMaterial.hpp` — `TitleBandRequest` gains `kind`.
- Modify: `src/contour/platform/PlatformAppearance.hpp:14-20` — `materials`.
- Modify: `src/contour/window/UiStyleProvider.cpp:198` — passes `_inputs.appearance.materials`.
- Create: `src/contour/platform/SystemBackdrop.hpp` (Qt-free; listed in `add_library(contour_platform …)`).
- Test: `src/contour/config/ChromeTokenGroups_test.cpp`, `src/contour/window/TitleBandSurface_test.cpp`, new
  `src/contour/platform/SystemBackdrop_test.cpp` (in `contour_gui_test` beside `platform/NativeWindowFrame_test.cpp`),
  `src/contour/window/UiStyleProvider_test.cpp`.

**Interfaces:**
- Produces (`config/ChromeTokenGroups.hpp`):
  - `enum class MaterialSupport : uint8_t { Unknown = 0, Supported, Unsupported };` — what the OS can draw;
    `Unknown` changes nothing (every OS but Windows publishes it).
  - `enum class BackdropKind : uint8_t { Tabbed = 0, Main };` — Mica Alt for a tabbed window, Mica for Settings.
  - `[[nodiscard]] constexpr ChromeTokenRow resolveChromeTokens(ChromeStyle row, AccessibilityPreferences preferences, MaterialSupport materials) noexcept;`
    — the two-argument form becomes `resolveChromeTokens(row, preferences, MaterialSupport::Unknown)`.
    `Unsupported` turns `Glass` and `Mica` into `Solid` in both material slots, exactly as Reduced transparency does.
- Produces (`window/TitleBandSurface.hpp`), appended to `TitleBandSurfaceRows`:

  | style | material | backdrop | fill | growth |
  |---|---|---|---|---|
  | `Fluent` | `Mica` | `NativeMaterial` | `Clear` | `None` |
  | `Fluent` | `Solid` | `Client` | `Tint` | `None` |

  1d's full-screen step applies (Mica in full screen is `Client` + `Tint`).
- Produces (`platform/TitleBandMaterial.hpp`): `TitleBandRequest::kind` — `config::BackdropKind kind = config::BackdropKind::Tabbed;`,
  declared after `backdrop`; the Cocoa material ignores it.
- Produces (`platform/PlatformAppearance.hpp`): `PlatformAppearance::materials` — `config::MaterialSupport materials = config::MaterialSupport::Unknown;`.
  `CocoaPlatformAppearance.mm`'s literals name it `Unknown`.
- Produces (`platform/SystemBackdrop.hpp`, namespace `contour::platform`):

```cpp
/// DWMWA_SYSTEMBACKDROP_TYPE values, spelled out because the SDK may predate them.
enum class DwmBackdrop : uint32_t { Auto = 0, None = 1, MainWindow = 2, TransientWindow = 3, TabbedWindow = 4 };
inline constexpr uint32_t DwmwaSystemBackdropType = 38;

/// Whether the user asked for blur behind the window (`background.blur`).
enum class BlurBehindWish : uint8_t { Off = 0, On };

/// DwmExtendFrameIntoClientArea's margins.
struct FrameMargins { int left = 0; int right = 0; int top = 0; int bottom = 0; bool operator==(FrameMargins const&) const = default; };

[[nodiscard]] constexpr DwmBackdrop systemBackdropFor(config::BandBackdrop band, config::BackdropKind kind, BlurBehindWish blur) noexcept;
[[nodiscard]] constexpr FrameMargins frameMarginsFor(config::BandBackdrop band, BlurBehindWish blur) noexcept;
```

  - `systemBackdropFor`: blur `On` → `TransientWindow` (R-i); else `NativeMaterial` → `TabbedWindow` for `Tabbed`,
    `MainWindow` for `Main`; else `None`. One table, `SystemBackdropRows`.
  - `frameMarginsFor`: `NativeMaterial` or blur `On` → `{ -1, -1, -1, -1 }`; else `{ 0, 0, 1, 0 }` (today's,
    `Win32WindowFrame.cpp:138-139`, whose comment moves here).

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`):
    - `chromeTokens.unsupportedMaterialIsSolid`: `resolveChromeTokens(Fluent, {}, Unsupported).material.titleBand == Solid`;
      with `Supported` or `Unknown`, `Mica`; MacOS `Glass` under `Unsupported` is `Solid`; the Terminal row's `None`
      stays `None`.
    - `chromeTokens.twoArgumentFormIsUnknown`: for every row and both transparency preferences, the two- and
      three-argument (`Unknown`) forms are equal.
  - `TitleBandSurface_test.cpp` (`[titlebandsurface]`): `surface.fluentRows`: `{ Fluent, Mica, ClientBand, Windowed }`
    → `{ NativeMaterial, Clear, None }`; `{ Fluent, Solid, … }` → `{ Client, Tint, None }`;
    `{ Fluent, Mica, ClientBand, FullScreen }` → `{ Client, Tint, None }`; `{ Fluent, Mica, NativeFrame, Windowed }`
    → `TitleBandSurface {}`. 1d's `surface.otherRowsKeepToday` now iterates every row but `MacOS` and `Fluent`.
  - `SystemBackdrop_test.cpp` (`[systembackdrop]`):
    - `backdrop.table`: `(NativeMaterial, Tabbed, Off) → TabbedWindow`, `(NativeMaterial, Main, Off) → MainWindow`,
      `(Client, Tabbed, Off) → None`, `(Client, Tabbed, On) → TransientWindow`, `(NativeMaterial, Tabbed, On) → TransientWindow`.
    - `backdrop.blurOffRestoresMica` (Also pinned): `systemBackdropFor(NativeMaterial, Tabbed, Off) != DwmBackdrop::None`.
    - `margins.clientKeepsOnePixel` (Review Focus 3): `frameMarginsFor(Client, Off) == FrameMargins { 0, 0, 1, 0 }`;
      `frameMarginsFor(NativeMaterial, Off) == FrameMargins { -1, -1, -1, -1 }`.
    - `backdrop.windows10IsSolid` (Review Focus 3): `resolveTitleBandSurface({ Fluent, resolveChromeTokens(Fluent, {}, Unsupported).material.titleBand, ClientBand, Windowed })`
      has `backdrop == Client` and `fill == Tint`, and its margins are `{ 0, 0, 1, 0 }`.
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`): "an unsupported material resolves the band solid": a
    provider built with `appearance.materials = Unsupported` on the Fluent row reports `material.titleBand == Solid`
    (the provider's material group).
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on `MaterialSupport` and `SystemBackdrop.hpp`.
- [ ] **Step 3: Implement** the enums, the overload, the rows, the field, the header.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[titlebandsurface],[titlebandgeometry]"
out/appleclang-debug/src/contour/contour_gui_test "[systembackdrop],[contour][uistyle],[titleband-material],[appearance-source]"
```

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/TitleBandSurface.hpp src/contour/window/TitleBandSurface_test.cpp src/contour/platform/TitleBandMaterial.hpp src/contour/platform/PlatformAppearance.hpp src/contour/platform/SystemBackdrop.hpp src/contour/platform/SystemBackdrop_test.cpp src/contour/window/UiStyleProvider.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/platform/CocoaPlatformAppearance.mm
git add src/contour/config/ChromeTokenGroups* src/contour/window/TitleBandSurface* src/contour/platform/TitleBandMaterial.hpp src/contour/platform/PlatformAppearance.hpp src/contour/platform/SystemBackdrop* src/contour/platform/CocoaPlatformAppearance.mm src/contour/platform/CMakeLists.txt src/contour/window/UiStyleProvider* src/contour/CMakeLists.txt
git commit -m "contour: decide Windows' backdrop and frame margins from tables; a band without OS material support is solid" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: A material that can fail, and the band on Windows' kept frame

> **Depends on Step 1d:** `ITitleBandMaterial::apply` (1d Task 3), `CocoaTitleBandMaterial.mm` (1d Task 5),
> `RecordingTitleBandMaterial` and `TitleBandRecord` (1d Task 3), `WindowController::refreshTitleBand`,
> `titleBandRequest`, `_appliedTitleBand` (1d Task 6) and TitleBar.qml's fill binding (1d Task 7, through
> `chromeStyle.bandFill(frame, mode)`).

**Files:**
- Modify: `src/contour/platform/TitleBandMaterial.hpp` — `apply` returns `std::expected`; `TitleBandMaterialError`.
- Modify: `src/contour/platform/CocoaTitleBandMaterial.mm`, the null material, `test/AppearanceFixtures.hpp`.
- Modify: `src/contour/window/WindowController.{hpp,cpp}` — the gate, the latch, `titleBandFill`; the two mocks.
- Modify: `src/contour/qml/TitleBar.qml` — the band's fill reads `win.titleBandFill`.
- Test: `src/contour/platform/TitleBandMaterial_test.cpp`, `src/contour/window/TitleBandController_test.cpp`,
  `src/contour/test/MainWindowQml_test.cpp`.

**Interfaces:**
- Produces (`platform/TitleBandMaterial.hpp`):
  - `enum class TitleBandMaterialError : uint8_t { Unsupported = 0, Rejected };` — `Unsupported`: the OS has no
    such backdrop; `Rejected`: the call failed on a supported OS.
  - `[[nodiscard]] virtual std::expected<void, TitleBandMaterialError> apply(QWindow& window, TitleBandRequest const& request) = 0;`
    The null and Cocoa materials return `{}`. `TitleBandRecord` gains `std::optional<platform::TitleBandMaterialError> failWith {};`,
    and the recording material returns it for a request whose backdrop is `NativeMaterial`.
- Produces (`config::`, beside `BandFill`): `enum class BandMaterialHealth : uint8_t { Working = 0, Failed };`
- Produces (`WindowController`):
  - `refreshTitleBand` applies under `FrameShadowStrategy::FullSizeContent` **or** `NativeFrameKept` (R-h); the rule
    is a pure `[[nodiscard]] constexpr bool hostsTitleBandMaterial(platform::FrameShadowStrategy) noexcept` in
    `NativeWindowFrame.hpp`, a question-named predicate, with a table test.
  - On an `apply` error: log it once (`display::displayLog()`), set `_bandMaterialHealth = Failed`, forget
    `_appliedTitleBand`, and compute the request again with `material = Material::Solid`; applied, it emits
    `titleBandChanged`. `Failed` lasts for the window's lifetime (R-g).
  - `Q_PROPERTY(int titleBandFill READ titleBandFill NOTIFY titleBandChanged)` — `int(titleBandSurface().fill)`,
    the same answer `chromeStyle.bandFill` gives unless this window's material failed.
  - The request's `kind` is `BackdropKind::Tabbed`.

- [ ] **Step 1: Write the failing tests.**
  - `TitleBandMaterial_test.cpp` (`[titleband-material]`): "the null material always succeeds": `apply(window, { .backdrop = NativeMaterial })`
    has a value.
  - `TitleBandController_test.cpp` (`[contour][titleband][controller]`), with `record.failWith = Unsupported` and the
    Fluent row's tokens (the chrome style host built on the Fluent row, as 1d's cases build MacOS):
    - "a failed apply falls back to Solid for that window" (Review Focus 3): two applied requests, the first
      `NativeMaterial`, the last `Client`; `titleBandFill() == int(BandFill::Tint)`; one `titleBandChanged`;
      a further expose applies nothing.
    - "a second window still asks for the material": a second controller's first request is `NativeMaterial`.
    - "the kept Windows frame hosts the band": `hostsTitleBandMaterial(NativeFrameKept)`,
      `hostsTitleBandMaterial(FullSizeContent)`, `!hostsTitleBandMaterial(None)`.
  - `MainWindowQml_test.cpp` (`[contour][gui][qml][mainwindow]`): "the band paints the window's own fill
    (offscreen)": with the mock's `titleBandFill = Clear`, the band's colour alpha is 0; with `Tint`, opaque; every
    pane card and `contentBackdrop` stay opaque under `Clear` (R-k; alpha 1).
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile errors on `std::expected` from `apply`.
- [ ] **Step 3: Implement** the signature, the doubles, the latch and the binding.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[titleband-material],[contour][titleband][controller],[contour][frame],[contour][gui][qml][mainwindow]"
out/appleclang-debug/src/contour/contour_gui_test "[cocoa][titleband]"
```

  Expected: PASS (the `[cocoa]` cases skip offscreen, as 1d made them).
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/TitleBandMaterial.{hpp,cpp} src/contour/platform/TitleBandMaterial_test.cpp src/contour/platform/CocoaTitleBandMaterial.mm src/contour/platform/NativeWindowFrame.hpp src/contour/test/AppearanceFixtures.hpp src/contour/window/WindowController.{hpp,cpp} src/contour/window/TitleBandController_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp src/contour/config/ChromeTokenGroups.hpp
editorconfig-checker src/contour/qml/TitleBar.qml
git add src/contour/platform/TitleBandMaterial* src/contour/platform/CocoaTitleBandMaterial.mm src/contour/platform/NativeWindowFrame.hpp src/contour/test/AppearanceFixtures.hpp src/contour/window/WindowController.* src/contour/window/TitleBandController_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp src/contour/config/ChromeTokenGroups.hpp src/contour/qml/TitleBar.qml
git commit -m "contour: a title-band material reports failure, and a window whose material failed paints a solid band" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: The Win32 material — Mica Alt, the margins, blur, composition restarts (Windows-only)

**Files:**
- Create: `src/contour/platform/Win32SystemBackdrop.{hpp,cpp}` — the one writer of `DWMWA_SYSTEMBACKDROP_TYPE` and
  the margins (wholly `#ifdef _WIN32`).
- Create: `src/contour/platform/Win32TitleBandMaterial.{hpp,cpp}` (wholly `#ifdef _WIN32`).
- Modify: `src/contour/platform/TitleBandMaterial.cpp` — `#ifdef _WIN32` returns `makeWin32TitleBandMaterial(dwmApi())`
  or the null one; `#elif !defined(__APPLE__)` the null one.
- Modify: `src/contour/platform/Win32WindowFrame.cpp:119-141` — `WM_DWMCOMPOSITIONCHANGED` and `extendFrame` re-apply
  from the window's record via `Win32SystemBackdrop`.
- Modify: `src/contour/platform/BlurBehind.cpp:124-150` — records the blur wish and calls `applySystemBackdrop`
  instead of writing attribute 38.
- List the four new files in `add_library(contour_platform …)`; `target_link_libraries` needs nothing new
  (`dwmapi` is resolved at run time).
- Test: `src/contour/platform/Win32TitleBandMaterial_test.cpp` (in `contour_gui_test`; body `#ifdef _WIN32`).

**Interfaces:**
- Consumes: Task 9 (`systemBackdropFor`, `frameMarginsFor`, `BlurBehindWish`, `DwmBackdrop`), Task 10 (`apply`'s
  result), `DwmApi` (`Dwmapi.hpp:22-40`).
- Produces (`Win32SystemBackdrop.hpp`, namespace `contour::platform`):
  - `void recordBandBackdrop(QWindow& window, config::BandBackdrop band, config::BackdropKind kind);` and
    `void recordBlurBehind(QWindow& window, BlurBehindWish blur);` — window properties
    `contour_band_backdrop` / `contour_band_kind` / `contour_blur_behind` (the `BorderSchemeKey` precedent,
    `Win32WindowFrame.cpp:264-273`, with the same +1 encoding so "never recorded" is distinct).
  - `[[nodiscard]] std::expected<void, TitleBandMaterialError> applySystemBackdrop(QWindow& window, HWND hwnd, DwmApi const& api);`
    — reads the three records, writes `systemBackdropFor(…)` and `frameMarginsFor(…)`; a failing attribute write
    (`FAILED(hr)`) writes `DwmBackdrop::None` and the `Client` margins back and returns `Unsupported` when
    `hr == E_INVALIDARG` (the attribute is unknown), else `Rejected`. `@pre` GUI thread.
- Produces (`Win32TitleBandMaterial.hpp`): `[[nodiscard]] std::unique_ptr<ITitleBandMaterial> makeWin32TitleBandMaterial(DwmApi const& api);`
  — `nullptr` unless `platformName() == "windows"` and `api.setWindowAttribute` and `api.extendFrameIntoClientArea`
  are resolved. `apply`: no handle → `{}` (best effort, as 1d's material); else `recordBandBackdrop`, then
  `applySystemBackdrop`. `metrics()` returns `{}` (no native title bar). The `DwmApi` is held by reference and
  injected, so a test passes a stub.

- [ ] **Step 1: Write the failing tests** (`[native][win32][mica]`, Windows-only, `SKIP` off the Windows QPA):
  - "Mica Alt on a tabbed window": a `QQuickWindow` (`color: transparent`) shown, the real `dwmApi()`;
    `apply(window, { .backdrop = NativeMaterial, .kind = Tabbed })` has a value on build ≥ 22621 (read
    `RtlGetVersion` in the test; on an older runner the case asserts `Unsupported` instead), and
    `DwmGetWindowAttribute(hwnd, 38, …)` reads `4`.
  - "Client restores none and the 1-px margin": then `apply(…, { .backdrop = Client })` reads `1`.
  - "a rejected backdrop attribute reports Unsupported and restores the margins" (Review Focus 3): a `DwmApi` stub
    whose `setWindowAttribute` returns `E_INVALIDARG` for attribute 38 and records `extendFrameIntoClientArea`
    calls: `apply(…, NativeMaterial)` is `std::unexpected(Unsupported)` and the last margins recorded are
    `{ 0, 0, 1, 0 }`.
  - "blur wins over Mica, and off restores it" (Also pinned): `apply(NativeMaterial)`, then
    `platform::setBlurBehind(&window, true)` reads `3`, then `setBlurBehind(&window, false)` reads `4`, not `1`.
  - "a composition restart keeps the window's margins" (Also pinned): after `apply(NativeMaterial)`, sending
    `WM_DWMCOMPOSITIONCHANGED` to the HWND leaves attribute 38 at `4` (and the stub variant records margins
    `{ -1, -1, -1, -1 }` again).
  - "the corner preference stays round under Mica" (Review Focus 2): after the frame's `apply` and the material's,
    `DwmGetWindowAttribute(hwnd, DwmwaWindowCornerPreference)` reads `DwmwcpRound`.
- [ ] **Step 2: See them fail** on CI (`Windows`, `Test`); here, build `contour_gui_test` only.
- [ ] **Step 3: Implement** the four files and the two edits. Doc comments carry the owner split (R-h) and why
  blur wins (R-i).
- [ ] **Step 4: See them pass** on CI. Here: `out/appleclang-debug/src/contour/contour_gui_test "[systembackdrop],[titleband-material],[contour][titleband][controller]"`.
- [ ] **Step 5: Commit** (before the push; CI fix-ups are folded in with `/absorb`).

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/Win32SystemBackdrop.{hpp,cpp} src/contour/platform/Win32TitleBandMaterial*.{hpp,cpp} src/contour/platform/TitleBandMaterial.cpp src/contour/platform/Win32WindowFrame.cpp src/contour/platform/BlurBehind.cpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform/Win32SystemBackdrop.* src/contour/platform/Win32TitleBandMaterial* src/contour/platform/TitleBandMaterial.cpp src/contour/platform/Win32WindowFrame.cpp src/contour/platform/BlurBehind.cpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "platform: Mica Alt behind the Windows title band, one owner for the backdrop with blur, margins that survive a composition restart" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

**Verified by:** CI `Windows` job, step `Test`, ctest `contour_windows_native_test` (`[native]`, which includes
`[native][win32][mica]`), and step "verify the Win32 chrome tests actually ran" (`[win32]`).

---
### Task 12: The Windows settings readings and the sources built on them (cross-platform)

**Files:**
- Create: `src/contour/platform/WindowsSettings.hpp` (Qt-free) — readings, the reader interface, the mappings, the
  setting-topic table.
- Create: `src/contour/platform/WindowsAppearanceSources.{hpp,cpp}` — the two sources, compiled on every OS.
- Modify: `src/contour/platform/PlatformAppearance.hpp` — `ContrastTheme` and its field.
- List the new files in `add_library(contour_platform …)`.
- Test: new `src/contour/platform/WindowsSettings_test.cpp` and `src/contour/platform/WindowsAppearanceSources_test.cpp`
  (both in `contour_gui_test`, beside `platform/AppearanceSources_test.cpp`, `src/contour/CMakeLists.txt:772`).

**Interfaces:**
- Produces (`platform/PlatformAppearance.hpp`): `enum class ContrastTheme : uint8_t { None = 0, Active };` and
  `PlatformAppearance::contrastTheme` — `ContrastTheme contrastTheme = ContrastTheme::None;` (R-l).
- Produces (`platform/WindowsSettings.hpp`, namespace `contour::platform`):

```cpp
enum class HighContrast : uint8_t { Off = 0, On };            ///< SPI_GETHIGHCONTRAST's HCF_HIGHCONTRASTON.
enum class ClientAreaAnimation : uint8_t { On = 0, Off };     ///< SPI_GETCLIENTAREAANIMATION.

/// What Windows reports, raw; each optional is absent when the value could not be read.
struct WindowsSettingsReadings
{
    std::optional<uint32_t> accentColor {};        ///< HKCU\Software\Microsoft\Windows\DWM\AccentColor, 0xAABBGGRR.
    std::optional<uint32_t> enableTransparency {}; ///< HKCU\…\CurrentVersion\Themes\Personalize\EnableTransparency.
    HighContrast highContrast = HighContrast::Off;
    ClientAreaAnimation animation = ClientAreaAnimation::On;
    std::optional<uint32_t> osBuild {};            ///< RtlGetVersion's dwBuildNumber.
    [[nodiscard]] bool operator==(WindowsSettingsReadings const&) const = default;
};

/// Reads the settings. @pre GUI thread.
class IWindowsSettingsReader
{
  public:
    virtual ~IWindowsSettingsReader() = default;
    [[nodiscard]] virtual WindowsSettingsReadings read() const = 0;
};

inline constexpr uint32_t Windows11Build = 22000;
inline constexpr uint32_t SystemBackdropBuild = 22621;
inline constexpr int Windows11CornerRadius = 8;

[[nodiscard]] constexpr vtbackend::RGBColor rgbFromAbgr(uint32_t abgr) noexcept;
[[nodiscard]] constexpr config::MaterialSupport materialSupportFor(std::optional<uint32_t> build) noexcept;
[[nodiscard]] constexpr std::optional<int> windowCornerRadiusFor(std::optional<uint32_t> build) noexcept;
[[nodiscard]] constexpr config::AccessibilityPreferences accessibilityFrom(WindowsSettingsReadings const& readings) noexcept;
[[nodiscard]] constexpr PlatformAppearance appearanceFrom(WindowsSettingsReadings const& readings) noexcept;

/// Which source a settings broadcast concerns.
enum class SettingTopic : uint8_t { None = 0, Accessibility, Appearance, Both };
/// The broadcast, as the adapter decoded it.
enum class SettingMessage : uint8_t { SettingChange = 0, ColorizationChanged, ThemeChanged };
struct SettingChange { SettingMessage message = SettingMessage::SettingChange; uint32_t action = 0; std::u16string_view area {}; };
[[nodiscard]] constexpr SettingTopic topicOf(SettingChange const& change) noexcept;
```

  - `materialSupportFor`: absent → `Unknown`; `< 22621` → `Unsupported`; else `Supported`.
  - `windowCornerRadiusFor`: absent → `nullopt`; `< 22000` → `0` (Windows 10's square corners); else `8`.
  - `accessibilityFrom`: `animation == Off` → `motion = Reduced`; `enableTransparency == 0` or `highContrast == On`
    → `transparency = Reduced`; `highContrast == On` → `contrast = Increased` (R-l).
  - `appearanceFrom`: `accent = rgbFromAbgr(*accentColor)` when present; `windowCornerRadius`,
    `materials` from the build; `contrastTheme = Active` when `highContrast == On`.
  - `topicOf`, one table `SettingTopicRows`: `SettingChange` with `action == 0x0043` (`SPI_SETHIGHCONTRAST`) →
    `Both`; `0x1043` (`SPI_SETCLIENTAREAANIMATION`) → `Accessibility`; `area == u"ImmersiveColorSet"` → `Both`
    (accent and transparency); `ColorizationChanged` → `Appearance`; `ThemeChanged` → `Both`; else `None`. The
    numeric values are the documented `SPI_*` constants, restated with a comment because the header is Qt- and
    Windows-free.
- Produces (`platform/WindowsAppearanceSources.hpp`):
  - `class WindowsAccessibilityPreferenceSource final: public IAccessibilityPreferenceSource` —
    `WindowsAccessibilityPreferenceSource(std::shared_ptr<IWindowsSettingsReader const> reader, AccessibilityPreferencesChanged onChange);`,
    `current()` maps a fresh `read()`; `void settingsChanged(SettingTopic topic);` re-reads on `Accessibility` or
    `Both` and calls `onChange` only when the mapped value differs from the last reported one.
  - `class WindowsPlatformAppearance final: public IPlatformAppearance` — the same shape over `appearanceFrom`,
    reacting to `Appearance` or `Both`.

- [ ] **Step 1: Write the failing tests** (`[windowssettings]`, `[appearance-source]`):
  - `WindowsSettings_test.cpp`:
    - `rgbFromAbgr(0xFFD77800) == RGBColor { 0x00, 0x78, 0xD7 }` (Windows' default blue).
    - `materialSupportFor` over `{ nullopt, 19045, 22000, 22621, 26100 }` → `Unknown, Unsupported, Unsupported,
      Supported, Supported` (Review Focus 3); `windowCornerRadiusFor` → `nullopt, 0, 8, 8, 8`.
    - `accessibility.defaults`: `accessibilityFrom({})` equals `AccessibilityPreferences {}`.
    - `accessibility.animationOff`, `accessibility.transparencyOff` (`enableTransparency = 0` → Reduced; `1` and
      absent → Allowed).
    - `accessibility.highContrast` (Review Focus 4): `{ .highContrast = On }` gives `contrast == Increased` and
      `transparency == Reduced`; `appearanceFrom` gives `contrastTheme == Active`.
    - `topics.table`: each row of `SettingTopicRows` decodes to its topic; `{ SettingChange, 0, u"Policy" }` is `None`.
  - `WindowsAppearanceSources_test.cpp`, with `FakeWindowsSettingsReader` (a `readings` member):
    - "a source reports a change once": set `highContrast = On`, `settingsChanged(Both)` → one call with
      `contrast == Increased`; the same call again → none; `settingsChanged(Appearance)` on the accessibility
      source → none.
    - "the appearance source follows the accent and the build": `accentColor = 0xFF2B1CC4`, `osBuild = 19045`:
      `current() == PlatformAppearance { .accent = RGBColor { 0xC4, 0x1C, 0x2B }, .windowCornerRadius = 0, .materials = Unsupported, .contrastTheme = None }`.
    - "a destroyed source no longer calls back": the callback's owner is gone; `settingsChanged` on a source
      built with an empty callback does nothing.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, `platform/WindowsSettings.hpp` not found.
- [ ] **Step 3: Implement** the header, the sources and the field (Cocoa's literal names `ContrastTheme::None`).
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[windowssettings],[appearance-source]"`

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/WindowsSettings*.{hpp,cpp} src/contour/platform/WindowsAppearanceSources*.{hpp,cpp} src/contour/platform/PlatformAppearance.hpp src/contour/platform/CocoaPlatformAppearance.mm
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/platform/WindowsAppearanceSources.cpp
git add src/contour/platform/WindowsSettings* src/contour/platform/WindowsAppearanceSources* src/contour/platform/PlatformAppearance.hpp src/contour/platform/CocoaPlatformAppearance.mm src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "platform: what Windows' accessibility and appearance settings mean, as tables, and the sources that report them" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: The Win32 settings reader, its watcher, and the Windows factories (Windows-only)

**Files:**
- Create: `src/contour/platform/Win32Settings.{hpp,cpp}` (wholly `#ifdef _WIN32`).
- Modify: `src/contour/platform/AccessibilityPreferenceSource.cpp:7` and `src/contour/platform/PlatformAppearance.cpp:7`
  — the null factories' guard becomes `#if !defined(__APPLE__) && !defined(_WIN32)` (Shared files: 5b replaces
  the Linux side of the same guard).
- Modify: `src/contour/platform/CMakeLists.txt:75-77` — `advapi32` joins the `WIN32` link list.
- Test: `src/contour/platform/Win32Settings_test.cpp` (in `contour_gui_test`; body `#ifdef _WIN32`).

**Interfaces:**
- Consumes: Task 12.
- Produces (`Win32Settings.hpp`):
  - `class Win32SettingsReader final: public IWindowsSettingsReader` — `RegGetValueW(HKEY_CURRENT_USER, …, RRF_RT_REG_DWORD, …)`
    for the two DWORDs; `SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(HIGHCONTRASTW), …)` and
    `SPI_GETCLIENTAREAANIMATION`; `RtlGetVersion` resolved from `ntdll.dll` at run time (`GetVersionEx` lies
    without a manifest). A failed read leaves the optional empty or the enum at its default.
  - `class Win32SettingsWatcher final: public QAbstractNativeEventFilter` —
    `explicit Win32SettingsWatcher(std::function<void(SettingTopic)> onTopic);` installs itself on the application
    (removes on destruction), decodes `WM_SETTINGCHANGE` (`wParam` action, `lParam` area string),
    `WM_DWMCOLORIZATIONCOLORCHANGED` and `WM_THEMECHANGED` into `SettingChange`, and calls `onTopic(topicOf(…))`
    when it is not `None`. Never consumes the message (returns `false`).
  - The Windows definitions of `makeAccessibilityPreferenceSource` and `makePlatformAppearance`: under the
    Windows QPA, Task 12's source over a shared `Win32SettingsReader`, owning a watcher that calls its
    `settingsChanged`; under any other QPA, the null source.

- [ ] **Step 1: Write the failing tests** (`[native][win32][settings]`, Windows-only):
  - "the reader agrees with the system": `Win32SettingsReader{}.read()` has `osBuild` set and
    `highContrast == (SystemParametersInfoW HIGHCONTRAST flags & HCF_HIGHCONTRASTON ? On : Off)`, read directly in
    the test; `animation` likewise. (No case changes a system setting.)
  - "the factories build the Windows sources": `dynamic_cast<WindowsPlatformAppearance*>(makePlatformAppearance({}).get())`
    and the accessibility twin are non-null.
  - "a broadcast reaches the source": with a factory-built appearance source and a counting callback,
    `SendMessageW(hwndOfAShownQWindow, WM_DWMCOLORIZATIONCOLORCHANGED, 0, 0)` re-reads; the callback runs only if
    the readings changed, so the case asserts the read count through a `Win32SettingsWatcher` built directly with
    a counting `onTopic` (one `Appearance`).
- [ ] **Step 2: See them fail** on CI; here, build `contour_gui_test`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: See them pass** on CI. Here: `out/appleclang-debug/src/contour/contour_gui_test "[appearance-source],[windowssettings]"`.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/Win32Settings*.{hpp,cpp} src/contour/platform/AccessibilityPreferenceSource.cpp src/contour/platform/PlatformAppearance.cpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform/Win32Settings* src/contour/platform/AccessibilityPreferenceSource.cpp src/contour/platform/PlatformAppearance.cpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "platform: read Windows' motion, transparency, contrast, accent and build, and follow their changes" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

**Verified by:** CI `Windows` job, step `Test`, ctest `contour_windows_native_test` (`[native][win32][settings]`),
and the `[win32]` guard step.

---

### Task 14: A contrast theme hands the chrome to the OS palette

**Files:**
- Modify: `src/contour/platform/GuiTheme.hpp` — `effectiveGuiTheme`.
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}` — `applyGuiTheme`'s callers (`:1641-1686` region per §2.2, and the
  platform-appearance change handler) pass the current `contrastTheme`; a change of `contrastTheme` re-applies the
  theme and refreshes the chrome style.
- Modify: `src/contour/window/WindowControlStyleProvider.{hpp,cpp}` — `setContrastTheme(platform::ContrastTheme)`
  (live OS state, not configuration: documented at the declaration); under `Active` the close entry's `hoverFill`
  is empty (the palette wash).
- Test: `src/contour/platform/GuiTheme_test.cpp`, `src/contour/test/ContourGuiApp_test.cpp`,
  `src/contour/test/QmlComponents_test.cpp` (drop Task 7's `[!mayfail]`).

**Interfaces:**
- Produces (`platform/GuiTheme.hpp`): `[[nodiscard]] constexpr config::GuiTheme effectiveGuiTheme(config::GuiTheme configured, ContrastTheme contrast) noexcept;`
  — `Active` → `GuiTheme::System` (no forced palette, so Qt's Windows theme palette, which is the contrast
  theme's, reaches the chrome); `None` → `configured`.

- [ ] **Step 1: Write the failing tests.**
  - `GuiTheme_test.cpp` (`[guitheme]`): for every `GuiTheme`, `effectiveGuiTheme(t, None) == t` and
    `effectiveGuiTheme(t, Active) == System`.
  - `ContourGuiApp_test.cpp` (`[contour][app][appearance]`): "a contrast theme hands the chrome to the OS palette,
    and back" (Review Focus 4): `TestApp` with `theme: terminal` and an `AppearanceSwitch`;
    `appearance.set({ .contrastTheme = Active })` → `appliedGuiScheme() == std::nullopt`; `set({})` → the
    terminal scheme is applied again (`appliedGuiScheme().has_value()`).
  - `QmlComponents_test.cpp`: Task 7's contrast case without `[!mayfail]`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[guitheme],[contour][app][appearance]"`

  Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[guitheme],[contour][app][appearance],[contour][gui][qml][titlebar][windowcontrols],[windowcontrols]"`

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/GuiTheme.hpp src/contour/platform/GuiTheme_test.cpp src/contour/ContourGuiApp.{hpp,cpp} src/contour/window/WindowControlStyleProvider.{hpp,cpp} src/contour/test/ContourGuiApp_test.cpp src/contour/test/QmlComponents_test.cpp
git add src/contour/platform/GuiTheme* src/contour/ContourGuiApp.* src/contour/window/WindowControlStyleProvider.* src/contour/test/ContourGuiApp_test.cpp src/contour/test/QmlComponents_test.cpp
git commit -m "contour: under a Windows contrast theme the chrome takes the system palette" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 15: The menu decision, as data — in-scene on the Fluent row, with the kit's radii

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp` — `RadiusTokens` (`:244-262`) gains `menuRow`; every row names
  it; `FluentRow.layout.menu` keeps `InScene` with a comment citing R-a and this plan.
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}` / `ChromeTokenObjects` — `radii.menuRow`.
- Modify: `src/contour/qml/ContourMenu.qml`, `src/contour/qml/ActionContextMenu.qml`, `src/contour/qml/PopupSurface.qml`
  — the menu's corner from `radii.menu` (not the generic `chromeStyle.radius`, `PopupSurface.qml:49`) and the row
  highlight's corner from `radii.menuRow`.
- Test: `src/contour/config/ChromeTokenGroups_test.cpp`, `src/contour/window/MenuRouting_test.cpp`,
  `src/contour/test/QmlComponents_test.cpp`.

**Interfaces:**
- Produces: `RadiusTokens::menuRow` — MacOS 5, Fluent 4, ContourLinux 6, Terminal 0 (kit `radius.menu · row`).
  `radii.menu` is already 10 / 8 / 10 / 0.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`): `chromeTokens.menuRadii` (the four rows' `menu`/`menuRow`);
    `chromeTokens.menusAreNativeOnlyOnMac`: over `ChromeTokenRows`, `layout.menu == Native` exactly for `MacOS`
    (this pins R-a; reversing it is a deliberate edit of this test).
  - `MenuRouting_test.cpp` (`[menurouting]`): "the Fluent row routes in-scene even with a willing presenter": a
    presenter double that would present returns `MenuRoute::InScene` under the Fluent row's `layout.menu`, and is
    never called.
  - `QmlComponents_test.cpp` (`[contour][gui][qml][contextmenu]`): "an in-scene menu takes its row's radii
    (offscreen)": on the Fluent row the surface's radius is 8 and a highlighted row's is 4; on the Terminal row both
    are 0; run also under `CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error on `menuRow`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens]"
out/appleclang-debug/src/contour/contour_gui_test "[menurouting],[contour][gui][qml][contextmenu],[contour][uistyle]"
CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3 out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][contextmenu]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][contextmenu]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/UiStyleProvider.{hpp,cpp} src/contour/window/ChromeTokenObjects.{hpp,cpp} src/contour/window/MenuRouting_test.cpp src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/ContourMenu.qml src/contour/qml/ActionContextMenu.qml src/contour/qml/PopupSurface.qml
git add src/contour/config/ChromeTokenGroups* src/contour/window/UiStyleProvider.* src/contour/window/ChromeTokenObjects.* src/contour/window/MenuRouting_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/qml/ContourMenu.qml src/contour/qml/ActionContextMenu.qml src/contour/qml/PopupSurface.qml
git commit -m "contour: Windows keeps in-scene menus, drawn with the Fluent radii" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 16: The Settings window's frame on Windows

> **Depends on Steps 4a and 4b:** `SettingsWindowHost` (4a; `settingsWindowFlags`, `SettingsWindowHost.cpp:145-153`)
> and 4b's Settings sidebar (spec §3.13: General, Appearance, Keyboard, Shell Integration, Profiles). Locate the
> sidebar by the name 4b's plan gives it.

**Files:**
- Modify: `src/contour/platform/Win32WindowFrame.cpp:195-200` — under `WindowDecoration::Server` the frame still
  tints the border from the scheme (`tintBorder`), then stops intercepting as today.
- Modify: `src/contour/window/SettingsWindowHost.{hpp,cpp}` — borrows a `platform::NativeWindowFrame&` and the app's
  `platform::ITitleBandMaterial&`; on `Expose` and `ApplicationPaletteChange` it applies `WindowDecoration::Server`
  and its backdrop request; publishes `backdropFill`.
- Create: `src/contour/window/SettingsBackdrop.hpp` — `settingsBackdropFor` (it names `Qt::ColorScheme` through the
  request, so it is not one of the Qt-free `window/` headers).
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}` — `_settingsFrame` (`makeNativeWindowFrame` with the effective-scheme
  lambda 1d made `effectiveColorScheme()`), handed to the host.
- Modify: 4b's sidebar QML — its background reads `host.backdropFill` (`Clear` → transparent, else today's).
- Test: new `src/contour/window/SettingsBackdrop_test.cpp`, `src/contour/window/SettingsWindowHost_test.cpp`, and a
  `[native][win32][settings-frame]` case in `src/contour/platform/Win32TitleBandMaterial_test.cpp`.

**Interfaces:**
- Produces (`window/SettingsBackdrop.hpp`): `[[nodiscard]] constexpr std::optional<platform::TitleBandRequest> settingsBackdropFor(config::ChromeStyle row, config::Material material, Qt::ColorScheme scheme) noexcept;`
  — `Fluent` + `Mica` → `{ .backdrop = NativeMaterial, .kind = Main, .scheme = scheme }`;
  every other pair → `nullopt` (R-m; macOS keeps 1d R-h).
- Produces (`SettingsWindowHost`): `Q_PROPERTY(int backdropFill READ backdropFill NOTIFY backdropFillChanged)` —
  `int(BandFill::Clear)` after a successful apply of a request, else `int(BandFill::Palette)`; a failed apply
  (Task 10's `std::expected`) keeps `Palette`.

- [ ] **Step 1: Write the failing tests.**
  - `SettingsBackdrop_test.cpp` (`contour_gui_test`, `[settingsbackdrop]`): the Fluent/Mica pair gives `kind == Main`; Fluent/Solid, MacOS/Glass
    and Terminal/None give `nullopt`.
  - `SettingsWindowHost_test.cpp` (`[settings][window]`): with a `RecordingTitleBandMaterial` and a recording frame
    (`NativeWindowFrame` double recording `apply` calls), opening Settings on the Fluent row applies one `Server`
    decoration and one `Main` request; a palette change applies the decoration again and no second request (the
    request did not change); `failWith = Unsupported` leaves `backdropFill() == int(Palette)`; on the MacOS row no
    request is applied.
  - `[native][win32][settings-frame]` (Windows-only): a Server-decorated `QWindow` with the frame applied under a
    dark scheme reads `DwmwaUseImmersiveDarkMode == TRUE`, and after the Settings request, attribute 38 reads `2`
    on build ≥ 22621.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, `window/SettingsBackdrop.hpp` not found.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[settingsbackdrop],[settings][window],[settings],[contour][frame]"`

  Expected: PASS here; the Windows case on CI.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/Win32WindowFrame.cpp src/contour/window/SettingsBackdrop*.{hpp,cpp} src/contour/window/SettingsWindowHost.{hpp,cpp} src/contour/window/SettingsWindowHost_test.cpp src/contour/ContourGuiApp.{hpp,cpp} src/contour/platform/Win32TitleBandMaterial_test.cpp
git add src/contour/platform/Win32WindowFrame.cpp src/contour/window/SettingsBackdrop* src/contour/window/SettingsWindowHost* src/contour/ContourGuiApp.* src/contour/platform/Win32TitleBandMaterial_test.cpp src/contour/qml src/contour/CMakeLists.txt
git commit -m "contour: on Windows the Settings window's title bar is Mica and follows the chrome's light or dark" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

**Verified by:** CI `Windows` job, step `Test`, ctest `contour_windows_native_test` (`[native][win32][settings-frame]`).

---

### Task 17: Snapshot rows for the Fluent row and the Windows host

> **Depends on Steps 1c, 1d, 2 and 3a–3f:** every row of `SnapshotScripts` names the fields those steps added
> (`.drift`, `.frame`, `.mode`, `.density`, `.paneEnd`, …). Locate them by name.

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` — `SnapshotScript` (`:35-49`), `ScriptAction` (`:66-74`), the
  script rows, `SnapshotScriptRow` gains `host`; `src/contour/test/ChromeSnapshotScene.hpp` (drives the mock's
  `nativeMaximizeVisual`, the window's activity, the host override); `src/contour/test/MockMainController.hpp`.
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Produces:
  - `SnapshotScriptRow::host` — `std::optional<config::HostPlatform> host {};` overrides `rowHost(row).host` for the
    window controls and shortcut spelling.
  - `ScriptAction` gains `HoverMaximize` (the mock's `nativeMaximizeVisual = Hover`) and `DeactivateWindow`.
  - New scripts, appended: `CaptionHover` (`"caption-hover"`, three tabs, `HoverMaximize`, `host = Windows`),
    `InactiveWindow` (`"inactive-window"`, three tabs, `DeactivateWindow`, `host = Windows`), `BandSolid`'s Fluent
    variant is 1d's `band-solid` row, which now also renders the Fluent row (no new row).
  - Under `host = Windows` the Terminal row shows the ● cells (R-n); the Fluent row renders only in the FluentWinUI3
    pass (Task 3).
- [ ] **Step 1: Write the failing tests.**
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): `scriptRow(SnapshotScript::CaptionHover).host == HostPlatform::Windows`;
    every other pre-existing row has `host == nullopt`; the enumerator-order `static_assert` holds; counts from
    `SnapshotScripts.size()`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): `caption-hover` on the Fluent row finds
    `windowControlFill_maximize` visible; on the Terminal row with the Windows host, `windowControl_maximize` draws
    `●`; `inactive-window` finds the close glyph at `label.tertiary`; no QML warning on any row.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `SnapshotScript::CaptionHover`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3 out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
```

  Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/MockMainController.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git add src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/MockMainController.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git commit -m "contour: snapshot the Windows caption hover, the inactive window and the Terminal row's Windows controls" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 18: Finish

- [ ] **Step 1:** Run `/simplify` on `git diff <the plan commit>..HEAD -- src scripts .github`. Apply its findings,
  re-run the filters of every task it touches, and commit once (Signed-off-by line, only the files it changed).
- [ ] **Step 2:** Every filter, here:

```bash
out/appleclang-debug/src/contour/contour_test "[config],[uistyle],[chrometokens],[captionhit],[titleband],[tabwell],[titlebandsurface],[titlebandgeometry],[windowcontrols],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[caption-buttons],[contour][caption],[windowcontrols],[systembackdrop],[titleband-material],[contour][titleband][controller],[windowssettings],[appearance-source],[guitheme],[contour][app][appearance],[menurouting],[settingsbackdrop],[settings],[contour][frame],[contour][multiwindow],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3 out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][titlebar],[contour][gui][qml][mainwindow],[contour][gui][qml][contextmenu],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) $(git diff --name-only <the plan commit>..HEAD -- 'src/*.cpp' | grep -v '/Win32')
```

  Expected: PASS, no clang-tidy diagnostic.
- [ ] **Step 3: CI, the Windows run.** Push. `gh pr checks 2178 --watch`. Required green: `Windows` (Build, Test incl.
  `contour_windows_native_test`, `contour_windows_native_scaled_test`, `contour_gui_test_fluent`; "verify the Win32
  chrome tests actually ran"; "Check that the package ships the FluentWinUI3 style"), the Linux and macOS jobs, and
  clang-tidy. A failure that master's latest run shows too is recorded, not fixed (`Latency trend (windows-2022)`).
- [ ] **Step 4: Snapshots, on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=FluentWinUI3 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances` (sizes read from
  `ChromeSnapshotPlan.hpp`). Look at `*.fluent.*` (40-high band, joined tabs, 46-wide caption buttons with Unicode
  fallback glyphs here, `caption-hover.fluent.*` with the maximise backplate), `band-solid.fluent.*` (the tint),
  `*.terminal.*` (`caption-hover.terminal.*` with `● ● ●` trailing) and `*.macos.*`/`*.linux.*` unchanged since the
  step before. Snapshots are artefacts, not acceptance.
- [ ] **Step 5: Owner manual checks — they need a Windows 11 machine or VM** (and O-6 a Windows 10 VM). Batched for
  the phase, listed in the ledger, not awaited. On the MSI from the `Windows` job's artefact:
  - O-1 Hover the maximise button ~0.5 s: Snap Layouts opens; pick a layout; the button's hover backplate shows;
    click maximises once, again restores; press, drag off, release does nothing. Same over the Terminal row's ●.
  - O-2 Maximised: the close button is hit at the screen's top-right pixel; corners square; windowed, corners round
    and the top edge above the buttons resizes.
  - O-3 Mica Alt behind the band, tinted by the wallpaper; panes opaque; text never on glass; light and dark both.
  - O-4 An inactive window: the band goes solid (DWM) and labels dim.
  - O-5 Settings › Personalization › Colors › Transparency effects off: the band goes solid at once, and back.
  - O-6 Windows 10 VM: a solid band, no black area, Segoe MDL2 caption glyphs, everything else works.
  - O-7 A contrast theme (Settings › Accessibility › Contrast themes): the chrome takes its colours, no Mica, close
    hover follows the theme; leaving it restores the terminal-scheme chrome.
  - O-8 Two monitors at 100 % and 150 %: drag across; Snap Layouts triggers only over maximise on both; the band
    height and frame are crisp.
  - O-9 The menu spike R-a replaced: open the tab menu, the new-tab menu and the title-bar menu under FluentWinUI3;
    confirm they read as Windows 11 menus and their swatches show; say whether `Popup.Native` should be tried.
  - O-10 Measure against Windows Terminal at 100 %: the band 40, caption buttons 46 wide, tab 34.
  - O-11 `background.blur: true`: Acrylic over the whole window; toggling a profile without blur restores Mica.
  - O-12 The Settings window: Mica title bar, light/dark follows the chrome, Snap Layouts on its own maximise.
- [ ] **Step 6: Release notes** with `/add-release-note` in `metainfo.xml`, under the open release:
  - "On Windows 11 the tabs are the title bar, on Mica, with tabs that join the terminal below them."
  - "Hovering the maximise button opens Windows 11's Snap Layouts."
  - "Windows caption buttons use the Segoe Fluent icons, and the Windows chrome uses Segoe UI Variable and the
    FluentWinUI3 controls."
  - "Contour follows Windows' animation, transparency-effects, contrast-theme and accent settings, live."
  - "On Windows 10, and with transparency effects off, the title bar is solid."
- [ ] **Step 7:** Mark Step 5a done in the plans index's step table and *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-5a.md`,
  with the CI run ids and the owner checks. Update the PR with `/update-pr`, reporting:
  - **Performance:** one extra native event filter per process (caption) plus one per appearance source; each
    filter's first test is a message-id switch, the hit-test runs only on `WM_NCHITTEST` for attached windows. No
    change on macOS or Linux.
  - **Risk:** medium-high, concentrated in code this machine cannot run: the caption adapter (Snap Layouts),
    the backdrop owner split with blur, and FluentWinUI3 replacing Fusion on Windows. Mitigations: pure tables
    tested on every OS, native cases on CI with a skip guard, solid fallbacks.
  - **Coverage:** every pure table and source on all three CI OSes; the Win32 adapters by `[native][win32]` on the
    Windows job; the QML under FluentWinUI3 and ContourTui runs; the owner checks above.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Menus on Windows stay in-scene (R-a).** The design record says "Menus are native on macOS and Windows"; the
   spec asks for a spike this machine cannot run. *Default: in-scene under FluentWinUI3 with the kit's radii; the
   swatch row and the terminal-scheme colours stay.* The alternative is `Popup.Native` with pre-built sub-menus
   (Win32 `HMENU`: no swatches, no scheme colours, dark mode unsupported), or `Popup.Window` for unclipped
   in-scene menus.
2. **A Windows contrast theme overrides `theme: terminal` (R-l).** *Default: yes, the chrome takes the OS palette
   while a contrast theme is on.* The alternative keeps the terminal scheme's chrome and only raises the contrast
   tokens.
3. **The Settings window keeps Windows' own title bar (R-m)**, with Mica and the chrome's light/dark. *Default: the
   server frame.* The alternative draws the canvas's 40-px title bar with our caption buttons and a second Snap
   Layouts hookup.
4. **`background.blur: true` makes the whole window Acrylic and replaces Mica (R-i).** *Default: yes.* The
   alternative ignores blur on Windows 11 while Mica is on.
5. **The Terminal row's Windows controls are ● cells at the trailing edge in Windows' order (R-n).** *Default:
   trailing, minimise · maximise · close, inked Warning · Success · Danger.* The alternative copies macOS' leading
   close-first order.

## Spec gaps found

- §3.14 assumes `DWMSBT_TABBEDWINDOW` exists; it needs Windows 11 22H2 (build 22621). Windows 10 and 21H2 get
  `Solid` through `MaterialSupport` (R-g).
- §3.14's "If any of that fails … `Solid`" needs a failure signal, but Step 1d's `ITitleBandMaterial::apply` returns
  `void`; Task 10 changes it to `std::expected`, and 1d's controller gate (`FullSizeContent` only) is widened to
  `NativeFrameKept` (R-h).
- `BlurBehind.cpp:139-150` already writes `DWMWA_SYSTEMBACKDROP_TYPE` and would switch Mica off; the spec does not
  mention it (R-i).
- The menu "spike" cannot run off Windows; the decision is made on documented grounds and departs from the design
  record (R-a, owner 1).
- The kit says "window.controls only where we draw them (TUI style, Linux CSD); native elsewhere", while the `win`
  board and §3.14 draw the Fluent caption buttons ourselves; the spec wins.
- `win`'s "0 when snapped or maximised" is read as the window radius, which DWM owns; pane cards keep 4 (R-e).
- 1c's band rules hide a one-tab Fluent window's bar under `Multiple`, which on Windows would remove the caption
  buttons; `Joined` keeps its bar (R-j).
- The Windows TUI row's `● ● ●` (design record, §3.4) was left to no step; Task 7 adds it for every non-macOS host.
- High-contrast themes, the inactive window and mixed-DPI hit-testing are not in §3.14; R-l, R-o and Task 4 cover
  them.
- FluentWinUI3 is chosen by name at run time, so `windeployqt --qmldir` may not ship it; Task 3 adds a package check.
- The Phase 0 ledger's Windows blocker (`SshTransport.cpp`) is fixed on master; the PR's Windows build now fails in
  `NewDefaultsOffer.hpp:69` (Step 1a's code), which Task 1 fixes first.
- The Step 4b plan was not on disk when this plan was written, although the brief lists it as written; Task 16
  names 4b's sidebar by spec §3.13 only.
