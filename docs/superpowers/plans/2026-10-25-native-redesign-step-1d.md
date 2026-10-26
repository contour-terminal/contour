# Native Redesign — Step 1d (macOS Title-Band Material and Safe Area) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead of Steps 4a and 1c (pipelining ruling). Line numbers are at HEAD `8a8aa716`, before either
lands. Where 1d touches code that 1c adds or changes (`window/TitleBandLayout.hpp`, `TitleBar.qml`'s band,
Main.qml's placement and reveal, the snapshot rows), the task names it by the name 1c's plan gives it and
carries a **Depends on Step 1c** note. A pre-flight scan re-verifies every reference once 1c has landed.

**Goal:** On macOS the title band shows AppKit's own title-bar material behind a transparent band, with
opaque panes, the window shadow kept, a solid fallback tinted from the terminal background under Reduce
Transparency and in full screen, a band height and full-screen position measured from the OS, and the
Terminal row's traffic lights snapped to its first cell row. Linux and Windows are unchanged.

**Architecture:**
- **A spike decides first.** Task 1 measures, on this Mac, what AppKit draws in the band under each
  candidate configuration. Four decisions (D1–D4) follow from written selection rules, so no human is
  needed; the later tasks state both branches of each decision.
- **Pure decisions, Qt-free, in `window/`, tested in `contour_test`:**
  - `TitleBandSurface.hpp` decides, from the row, the resolved material, the frame and the window mode,
    what AppKit draws behind the band (`BandBackdrop`), what the band paints (`BandFill`), and whether the
    title bar is grown to the band (`TitleBarGrowth`). It also computes the terminal-background tint.
  - `TitleBandGeometry.hpp` decides the band's height, its full-screen top, and where the traffic lights'
    centre goes, from the token height and the native title bar's measured metrics.
- **The OS is reached through `platform::ITitleBandMaterial`.** It has an inline null implementation, a
  non-Apple factory, and a Cocoa `.mm`. `AppearanceServices` builds it, so every `TestApp` gets the null one
  and a test can record what the controller asks for.
- **One owner per AppKit property.** The safe-area branch (D1) settles who sets the full-size content
  view: Qt, through `Qt::ExpandedClientAreaHint`, or today's `CocoaWindowFrame`. The material owns the
  title bar's transparency, its appearance, the grown title bar and the traffic-light position, and
  re-asserts the shadow after each change.
- **The controller drives it.** `WindowController::refreshTitleBand()` computes the request and the
  geometry, applies the request only when it changes, and publishes `titleBandHeight` and
  `titleBandRevealTop` to QML. It runs on expose, on a palette, state, scale or screen change, and from
  `ContourGuiApp::refreshChromeStyle()`, which is where a live Reduce Transparency change arrives.

**Tech Stack:** C++23, Qt 6.11.1 (Gui, Quick; `/opt/homebrew/opt/qt`), Objective-C++ with ARC (AppKit) on
Apple, QML, Catch2 v3, CMake build tree `out/appleclang-debug`, macOS 26.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.5 (`:446-468`)
binds, with what it cites: §1 (principles, `:28-64`), §2.1 (token rows; `material.titleBand`), §2.3
(accessibility; `TransparencyPreference`), §2.6 (testing), and §6's row "`safeAreaMargins` was not a drop-in
replacement for the inset | 1d keeps the inset; a spike decides the height source" (`:866`). The design
record is `docs/superpowers/specs/2026-10-23-native-redesign-design.md` ("Glass and material appear on
chrome only, never behind terminal text", `:14`). The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `kit-tokens`, `mac-dark`, `mac-light`,
`tui` and `surfaces` are in `.superpowers/design/canvas/project/`. The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. The Step 1c plan
(`2026-10-25-native-redesign-step-1c.md`) and the Step 4a plan (`2026-10-25-native-redesign-step-4a.md`) are
assumed done. Binding rulings come from `docs/superpowers/ledgers/*.md` and from the 1c ledger
(`.superpowers/sdd/2026-10-25-native-redesign-step-1c/progress.md`), which defers two items to 1d.

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

- **The spec, verbatim (§3.5):**
  - "keep `NSWindowStyleMaskFullSizeContentView`, and set `titlebarAppearsTransparent = NO`. AppKit then
    draws its own title-bar material beneath the content view."
  - "The QQuickWindow gets an alpha-capable format. The band area is cleared to transparent, and panes stay
    opaque." The format already has an 8-bit alpha (`display/ShaderConfig.cpp:30`), and Main.qml's
    `color: "transparent"` (`:43`) already holds. Neither changes.
  - "CocoaWindowFrame warns that an alpha surface can clear `hasShadow` (`CocoaWindowFrame.mm:62-64`). The
    step re-asserts the shadow after `apply()`."
  - "**Fallback:** a `Solid` band, painted from the terminal background tint. It is used under
    `TransparencyPreference::Reduced`, or if the material flickers on resize."
  - "The choice sits behind `ITitleBandMaterial`, which also has a null implementation."
  - "`nativeControlsInset()` (`CocoaWindowFrame.mm:81-97`) stays the horizontal inset source for the
    traffic lights."
  - "Under Qt ≥ 6.9, the band's height comes from `QWindow::safeAreaMargins().top`, but only if the window
    adopts `Qt::ExpandedClientAreaHint`." Every Qt ≥ 6.9 API sits behind `QT_VERSION_CHECK(6, 9, 0)`
    (spec baseline).
- **The canvas, verbatim:**
  - `kit-tokens`: "chrome.band material + terminal background tint (±4 % L). Reduce Transparency makes it
    solid".
  - `mac-dark`: "AppKit's traffic lights, laid out with safeAreaMargins (Qt 6.9+) — never a hard-coded
    inset." "Title band: Liquid Glass (regular), tinted from the terminal background. Panes: opaque, the
    user's scheme." *Superseded for the band by ruling S6:* AppKit's material lets its `NSTitlebarView` take
    the band's clicks, so the band ships as the tint alone (`material.titleBand = Solid`); Liquid Glass returns
    with a within-window `NSVisualEffectView` of our own behind the Qt content (follow-up).
  - `mac-light`: "Inactive window (kit rule): grey traffic lights, no material, labels one level dimmer."
  - `tui`: "On macOS the TUI row keeps AppKit's real traffic lights, snapped to the first cells of the bar
    row".
- **Exact values.** The tint moves the terminal background's HSL lightness by 4 percentage points: up on a
  dark background, down on a light one, clamped to 0–100. "Dark" is relative luminance below 0.18
  (`platform::DarkBackgroundLuminance`, `platform/GuiTheme.hpp:20`); the pure header restates it as
  `TitleBandDarkLuminance = 0.18` with a `static_assert` in the provider's `.cpp` that the two agree. The
  macOS band token is 1c's `size.titleBar` = 38.
- **Carried from 1c's ledger:** "the Terminal row's vertical traffic-light snap on macOS and the full-screen
  band's place under the menu bar" are this step's (Tasks 6–8).
- **Binding rulings (ledgers 0, 1a, 1b, 1c):**
  - Every AppKit call runs on the main thread, asserts `Require(NSThread.isMainThread)`, and is documented
    `@pre` main thread.
  - New `.mm` files are ARC: each goes in `target_sources(contour_platform PRIVATE …)` inside `if(APPLE)`
    (`src/contour/platform/CMakeLists.txt:81-96`) and in its `-fobjc-arc` list; Cocoa test `.mm`s go in
    `contour_gui_test`'s `if(APPLE)` block and its `-fobjc-arc` list (`src/contour/CMakeLists.txt:827-841`).
    Under ARC, `winId()` is cast as `(__bridge NSView*) reinterpret_cast<void*>(window.winId())`
    (`CocoaNativeMenuPresenter.mm:174`), and only after `window.handle() != nullptr`.
  - Every Cocoa factory returns the null object unless `QGuiApplication::platformName() == "cocoa"`
    (`CocoaWindowFrame.mm:106`).
  - Every new `platform/` header is listed in `add_library(contour_platform …)` (`:15-69`). A non-Apple
    factory lives in a `.cpp` wholly inside `#ifndef __APPLE__` (the `MenuBarIntegration.cpp` shape).
  - The services are built through `platform::AppearanceServices`, so `TestApp` never reaches Cocoa. Every
    literal of `AppearanceServices` names every field: `nullAppearanceServices()`
    (`AppearanceServices.hpp:33-50`) and `appearanceServicesWith()` (`test/AppearanceFixtures.hpp:101-127`).
  - Display tests skip offscreen: a `[cocoa]` case returns unless `displayTestsEnabled()` and
    `platformName() == "cocoa"`, as `REQUIRE_DISPLAY_OR_SKIP()` does (`display/DisplayRendering_test.cpp:88-97`).
  - Reduce Transparency comes from Step 1a's source (`IAccessibilityPreferenceSource`,
    `TransparencyPreference`), which already turns `material.titleBand` from `Glass` into `Solid`
    (`ChromeTokenGroups.hpp:450-455`). No new OS read.
  - No test's outcome depends on the developer's real OS settings.
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:**
  - Data-driven: the band surface is one table (`TitleBandSurfaceRows`); adding Windows' Mica in 5a is a row.
  - `enum class` over `bool`, with zero as the default. New QML-facing `Q_PROPERTY` booleans are the
    documented carve-out, as `needsFramelessHint` is.
  - `std::expected` where a call can fail; nothing here can (apply is best-effort, as the frame is).
  - Doxygen (`///`, `@param`, `@return`, `@pre`) on every new public declaration; `.hpp` with `#pragma once`.
  - Configuration at construction: the material is built once per app and borrowed by each controller.
  - Zero warnings and no `NOLINT`. `slots`, `signals` and `emit` are Qt macros: never use them as identifiers.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already set in
  `out/appleclang-debug/CMakeCache.txt`): `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`.
  Never run two builds at once.
- **Tests:** each task runs only its own filter. The full suite runs in CI on Linux, macOS and Windows.
- **Format** every touched `.cpp`/`.hpp`/`.mm` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i` (CI's
  version). Check every new or touched `.qml`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`.
  **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`.** Every new struct member gets `{}` or a
  value, and every row of a table names every field: `TitleBandSurfaceRows`, `FramePolicy` literals (Branch
  A), `SnapshotScripts`, `AppearanceServices` literals.
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Counts in tests are derived from the tables** (`TitleBandSurfaceRows.size()`, `SnapshotScripts.size()`).
- **QML:** no colour or size literal (`check_qml_colors`, `check_qml_sizes`; a file's baseline only goes
  down). Enums are unreachable through `chromeStyle`: QML passes and compares `int`s.
- **Qt-free targets stay Qt-free:** the two new `window/*.hpp` and the new `config/` enums include no Qt header.
- **Never edit `vendor/core-cpp`.**
- **Spike code is never committed.** It lives in `out/spike-1d/` (git-ignored, `.gitignore:22`); its written
  result lives in the git-ignored ledger directory and is copied into the docs ledger at Finish.
- **Commits:** one per task (Task 1 has none). Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, and never carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 9).

## Review Focus

1. **Resize flicker.** A live drag-resize must not flash the desktop or a stale frame through the band, and
   must leave the shadow and the rounded corners intact. Expected: no flash; the shadow follows the new size.
   Pinned in Task 1 (the flicker metric selects `Solid` if the material adds flicker, D4), Task 5
   ("forty resizes keep the shadow and the title-bar setting") and Task 6 ("a resize re-applies nothing").
2. **Reduce Transparency toggled live.** Expected: the band turns into the solid tint at once, AppKit stops
   drawing its material (otherwise its grey title bar covers the solid band), and turning it off restores
   both, without a restart or a terminal resize. Pinned in Task 6 ("Reduce Transparency switches the request
   live, and back") and Task 7 ("the band is the tint under Reduce Transparency").
3. **A light scheme under a dark OS** (`theme: terminal` with contour-light, or `theme: light`, while macOS is
   dark). Expected: AppKit's material is light, matching the light labels, and a solid tint never pairs a
   dark fill with dark labels. Pinned in Task 2 (`legibleBandTint` falls back when the tones disagree),
   Task 5 ("a light request gives a light window under a dark app") and Task 6 ("the request follows the
   applied chrome scheme").
4. **A second display with a different backing scale.** Expected: after a move from a 2× to a 1× display, the
   band's height lands on whole device pixels, the native metrics are measured again, and the shadow is
   re-derived. Pinned in Task 2 (`geometry.devicePixelSnap`), Task 6 ("a scale change measures the title
   bar again") and Task 5 (a two-screen case, skipped on one screen).
5. **Full screen, entered and left.** Expected: in full screen, the revealed band sits below the menu bar
   and is the solid tint, because it overlays terminal cells and glass may never sit behind terminal text.
   After leaving, the material, the shadow and the band height come back. Pinned in Task 2
   (`surface.fullScreenIsSolid`, `geometry.revealTop`), Task 5 ("full screen and back"), Task 6 ("full screen
   asks for the client backdrop, and back") and Task 7 ("the revealed band sits at the reveal top").

Also pinned, beyond the five:
- `show_title_bar: true` applies no material and keeps the ordinary title bar; toggling back applies it
  again (Task 6).
- The Fluent and ContourLinux rows keep today's band colour and geometry (Tasks 2 and 7; 1c's §2.1
  acceptance test passes unedited).

---

## Spec resolutions (read before Task 1)

These settle where the spec is silent, or where the canvas or the code disagrees with it. Those marked
**(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. What the band paints under Glass: `Clear` or `Tint` (spike, D4).** §3.5 says "cleared to
  transparent"; the kit says "material + terminal background tint". Both hold if AppKit's material blends
  *behind the window* (the desktop shows, blurred), and the band then paints nothing. If it blends *within
  the window*, it blurs what the band paints, and a cleared band would show the desktop unblurred; the band
  then paints the tint, opaque, under the material. Task 1 measures the blending mode, and D4 picks the row.
- **R-b. Full screen is solid (owner, 2).** 1c's revealed band overlays the top terminal rows. A cleared band
  there would show terminal text through the chrome, which the design record forbids ("never behind
  terminal text"). So under `WindowMode::FullScreen` the backdrop is `Client` and the fill is `Tint`, and
  AppKit's revealed title bar is transparent, so it carries only the traffic lights over the band.
- **R-c. The band is at least as tall as the native title bar.** §3.5 sources the height from the safe area;
  the canvas wants 38. The height is `max(size.titleBar, native height)`, rounded up to whole device pixels.
  If the native title bar is shorter than the band, AppKit's material would stop part-way down it; D2
  decides whether an empty compact `NSToolbar` grows the title bar to the band, and if nothing does, D4 makes
  the Glass fill `Tint`, so the uncovered strip is the tint, not the desktop.
- **R-d. The tint is app-wide (owner, 3).** It is computed from the scheme the chrome already reads
  (`ContourGuiApp::chromeInputs()`, `ContourGuiApp.cpp:1796-1805`: the default profile's preferred palette),
  not per window. When the background's tone disagrees with the chrome's (a dark terminal under a light
  `theme: light` chrome), the tint gives way to the palette window colour, so the labels, which follow the
  chrome palette (1c R-h), stay legible.
- **R-e. Who owns what.** The frame owns the style mask and the title's visibility (or Qt does, under D1 =
  Hint). The material owns `titlebarAppearsTransparent` (or Qt's `NoTitleBarBackgroundHint` does, under
  Hint), `appearance`, the toolbar, the traffic-light position, and the shadow re-assertion after each of
  them. The controller applies the material only where the frame policy is `FullSizeContent`: under
  `show_title_bar: true` the band does not exist.
- **R-f. The appearance follows the applied chrome scheme.** The request carries
  `_manager.app().appliedGuiScheme()`, else `QGuiApplication::styleHints()->colorScheme()`, which is the
  lambda `WindowController` already hands the frame (`WindowController.cpp:80-85`). It becomes one private
  helper, `effectiveColorScheme()`, used by both.
- **R-g. The inactive window's flat title bar is AppKit's.** `mac-light`'s "no material" for an inactive
  window is what AppKit does to its own title bar. Nothing is coded; it is an owner manual check.
- **R-h. The Settings window keeps the OS's frame** (4a non-goal). No material there.

### Non-goals

- Windows' Mica (5a) and Linux materials (5b). Their rows give `Client`/`Palette`.
- Per-window or per-session band tints (R-d).
- A material behind popovers, the palette or the find bar (`material.popover`): Phase 3 and 5.

---

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/ChromeTokenGroups.hpp` (modify) | `BandBackdrop`, `BandFill`, `TitleBarGrowth`: the vocabulary `window/` and `platform/` share | 2 |
| `src/contour/window/TitleBandSurface.hpp` (create) | The surface table, `resolveTitleBandSurface`, `bandTintOf`, `legibleBandTint` | 2 |
| `src/contour/window/TitleBandGeometry.hpp` (create) | `resolveTitleBandGeometry`: height, reveal top, traffic-light centre | 2 |
| `src/contour/platform/TitleBandMaterial.hpp` (create) | `ITitleBandMaterial`, `TitleBandRequest`, `NativeTitleBarMetrics`, the null one, the factory | 3 |
| `src/contour/platform/TitleBandMaterial.cpp` (create) | The non-Apple factory, `#ifndef __APPLE__` | 3 |
| `src/contour/platform/AppearanceServices.hpp` (modify) | `.titleBand` factory | 3 |
| `src/contour/test/AppearanceFixtures.hpp` (modify) | `TitleBandRecord`, `RecordingTitleBandMaterial`, the three-switch `appearanceServicesWith` | 3 |
| `src/contour/ContourGuiApp.{hpp,cpp}` (modify) | Owns the material; `titleBandTokens()`; `refreshChromeStyle` refreshes the bands | 3, 6 |
| `src/contour/platform/NativeWindowFrame.hpp`, `CocoaWindowFrame.mm` (modify) | The safe-area branch | 4 |
| `src/contour/platform/CocoaTitleBandMaterial.mm` (create) | The AppKit side | 5, 8 |
| `src/contour/window/WindowController.{hpp,cpp}` (modify) | `refreshTitleBand`, the two geometry properties | 6 |
| `src/contour/session/TerminalSessionManager.{hpp,cpp}` (modify) | `refreshTitleBands()` | 6 |
| `src/contour/window/UiStyleProvider.{hpp,cpp}` (modify) | `bandFill(frame, mode)` | 7 |
| `src/contour/qml/TitleBar.qml`, `Main.qml` (modify) | The band's fill, height and full-screen top | 7 |
| `src/contour/test/MockMainController.hpp`, `ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | Mock properties; the `band-solid` script | 7 |

---

### Task 1: Spike — what AppKit draws in the band, and who owns the safe area (time-boxed, never committed)

**Files:**
- Create (git-ignored, never committed): `out/spike-1d/CMakeLists.txt`, `out/spike-1d/main.mm`,
  `out/spike-1d/Band.qml`.
- Create (git-ignored): `.superpowers/sdd/2026-10-25-native-redesign-step-1d/spike.md`, and the runs'
  `out/spike-1d/runs/*.jsonl`.

**Interfaces:**
- Produces: the decisions D1–D4, each recorded in `spike.md` with its measurements and the rule line it met,
  and appended as rulings to `.superpowers/sdd/2026-10-25-native-redesign-step-1d/progress.md`
  ("Ruling S1: D1 = Hint — H1–H5 held; see spike.md §D1"). Tasks 4, 5, 7 and 8 read them.

**Time box:** two hours of wall-clock. A measurement not taken within the box takes its decision's
conservative default, named in the rules below, and is recorded as such.

**How the spike observes AppKit's material.** `grabWindow()` sees only Qt's own surface, and a screen capture
needs a permission grant nobody may be there to give (owner rule). So the spike reads AppKit's state and asks
AppKit to render its own views:
- **E1, the view tree:** walk `nsWindow.contentView.superview` (the theme frame) four levels deep. Record each
  view's class name and frame in window coordinates (from the top), and for every `NSVisualEffectView`, or
  any class whose name contains `Glass` or `Titlebar`, its `material`, `blendingMode`, `state` and
  `effectiveAppearance.name`.
- **E2, AppKit's own pixels:** `-[NSView cacheDisplayInRect:toBitmapImageRep:]` of the theme frame over the
  band's rectangle. It renders AppKit's views and not Qt's `CAMetalLayer`, so the band rows' mean alpha and
  colour are AppKit's contribution alone. Record them per 2-pt row band, giving the height AppKit paints.
- **E3, window state:** `styleMask` (the FullSizeContentView bit), `titlebarAppearsTransparent`,
  `titleVisibility`, `isOpaque`, `backgroundColor`'s alpha, `hasShadow`, `toolbar`, `toolbarStyle`,
  `appearance.name`, `effectiveAppearance.name`, the native title-bar height
  (`NSHeight(frame) - NSHeight(contentLayoutRect)`), `QWindow::safeAreaMargins()` and every
  `safeAreaMarginsChanged`, the three `standardWindowButton` frames (window coordinates from the top) and
  their superview's `isFlipped`, `winId()`, every `QPlatformSurfaceEvent`, `NSApp.mainMenu.menuBarHeight`,
  and each screen's `backingScaleFactor` and `safeAreaInsets.top`.
- **The owner's eye** is a manual check at Finish, listed and not awaited.

- [ ] **Step 1: Write the spike app.** One Objective-C++ `main.mm` (ARC) linking `Qt6::Quick` and `-framework
  AppKit`, against `/opt/homebrew/opt/qt`. It refuses to run unless `CONTOUR_TEST_DISPLAY=1` (windows flash)
  and `platformName() == "cocoa"`. It shows a 900 × 600 `QQuickWindow` with `color: transparent`, the default
  format with an 8-bit alpha (as `createSurfaceFormat` makes it), and `Band.qml`: a 38-pt band `Rectangle`
  whose colour is a command-line choice (`transparent`, or an opaque tint), above an opaque pane. Flags:
  - `--frame=today|manual|hint|hint-nobg`: `today` and `manual` run `CocoaWindowFrame::apply`'s code with
    `titlebarAppearsTransparent` = YES or NO; `hint` sets `Qt::ExpandedClientAreaHint` and touches no
    AppKit property; `hint-nobg` adds `Qt::NoTitleBarBackgroundHint`.
  - `--toolbar=none|compact|unified`: an empty `NSToolbar` (`allowsUserCustomization = NO`) with
    `NSWindowToolbarStyleUnifiedCompact` or `Unified`.
  - `--band=clear|tint`, `--app-appearance=aqua|dark`, `--window-appearance=none|aqua|dark`.
  - `--lights=<centre-y>`: move the three buttons so their centre is that many points from the window's top.

  It prints one JSON line per observation (E1–E3), then runs this script, printing after each step:
  1. the first expose;
  2. forty `setFrame:display:YES` steps alternating ±7 pt in width and height; after each, wait up to 50 ms for
     `frameSwapped`, and record whether it came and whether the `CAMetalLayer`'s `drawableSize` equals the
     bounds times the backing scale;
  3. `toggleFullScreen:`, waiting for `NSWindowDidEnterFullScreenNotification`; then
     `CGWarpMouseCursorPosition` to the screen's top edge, a 1-s wait, and the frame of every `NSApp.windows`
     entry whose class name contains `FullScreen` (AppKit's revealed title bar);
  4. `toggleFullScreen:` again, waiting for `DidExitFullScreen`;
  5. a second window made key and then closed (resign and become key);
  6. with two screens, a move to the other screen and a wait for `screenChanged`; otherwise
     `"screens": 1`;
  7. for the `hint` frames only, toggling `Qt::NoTitleBarBackgroundHint` on and off with `setFlag`, and,
     separately, removing `Qt::ExpandedClientAreaHint` (the `show_title_bar: true` path);
  8. the View menu's item titles, before and after the toolbar is attached.
- [ ] **Step 2: Run the matrix** and keep each run's output in `out/spike-1d/runs/<frame>-<toolbar>-<band>.jsonl`:

```bash
cmake -S out/spike-1d -B out/spike-1d/build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt && cmake --build out/spike-1d/build
for frame in today manual hint hint-nobg; do for toolbar in none compact unified; do for band in clear tint; do
  CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=$frame --toolbar=$toolbar --band=$band > out/spike-1d/runs/$frame-$toolbar-$band.jsonl
done; done; done
CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=manual --app-appearance=dark --window-appearance=aqua > out/spike-1d/runs/appearance.jsonl
CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=today --lights=9 > out/spike-1d/runs/lights.jsonl
```

  Expected: 26 files, each with an observation after every script step.
- [ ] **Step 3: Decide D2, the title bar's growth.** Let `h0` be the native title-bar height under
  `manual`/`none`, and `hc` under `manual`/`compact`.
  - `None` if `h0 ≥ 37.5`.
  - Else `CompactToolbar` if all hold: `37.5 ≤ hc ≤ 42`; the traffic lights' centre is within 1 pt of `hc / 2`;
    the View menu gained no item.
  - Else `None`. Conservative default: `None`.
- [ ] **Step 4: Decide D1, the safe area's owner.** `Hint` if all of H1–H5 hold, else `Manual`. Conservative
  default: `Manual`.
  - **H1.** Under `hint` (with D2's toolbar): the FullSizeContentView bit is set,
    `titlebarAppearsTransparent == NO`, the three buttons exist and are visible, and `hasShadow == YES`.
    Record `titleVisibility`: if it is visible, Task 4 keeps the one `titleVisibility` line.
  - **H2.** Under `hint`, toggling `NoTitleBarBackgroundHint` flips `titlebarAppearsTransparent` to YES and
    back, with no `SurfaceAboutToBeDestroyed` event and an unchanged `winId()`.
  - **H3.** `safeAreaMargins().top` equals the native title-bar height within 0.5 pt when windowed (with D2's
    toolbar), and `safeAreaMarginsChanged` fires on entering and on leaving full screen.
  - **H4.** Removing `ExpandedClientAreaHint` gives an ordinary titled window: the FullSizeContentView bit is
    clear, and the title is visible.
  - **H5.** After the forty resizes, `hasShadow == YES` and nothing in E3 changed except the frame.
- [ ] **Step 5: Decide D4, the macOS row's band material and its Glass fill.** Let `F` be the fraction of the
  forty resize steps without a frame within 50 ms or with a mismatched `drawableSize`. Compare `F_mat`
  (`manual`, or `hint` if D1 = Hint, with `band=clear`) against `F_today` (`today`, `band=clear`).
  - `Solid` if E1 finds no material view covering the band's top `min(38, native height)` points and E2's
    mean alpha over those rows is below 0.5 under `manual`: AppKit draws no material there. Record it as a
    spec gap.
  - `Solid` if `F_mat − F_today > 0.05`: the material adds flicker (§3.5's fallback).
  - Otherwise `Glass`, with the fill `Clear` if the covering view's `blendingMode` is `BehindWindow` and the
    native height (with D2) is at least 37.5, else `Tint` (R-a, R-c).
  - Conservative default: `Glass` with `Tint`, if E1 or E2 shows a material, else `Solid`.
- [ ] **Step 6: Decide D3, the Terminal row's traffic lights.** From `lights.jsonl`:
  - `SnapToFirstRow` if, re-asserting the moved origin on each of `NSWindowDidResizeNotification`,
    `DidEndLiveResize`, `DidExitFullScreen`, `DidBecomeKey` and `DidResignKey`, the close button's centre is
    within 0.5 pt of 9 after every script step. Record which of those notifications were needed; Task 8
    observes exactly those.
  - Else `GrowBandToCells` if `ceil(h0 / 17) ≤ 2` (17 pt is the default chrome cell height on this Mac;
    record the real one from `chromeStyle.cellHeight`): the band grows to the title bar in whole cells.
  - Else `AppKit`: 1c's placement stays, and the item is parked in the ledger. This is the conservative
    default.
- [ ] **Step 7: Record the full-screen reveal.** From step 3 of the script: the revealed title bar's top
  relative to the screen's top, `menuBarHeight` and `safeAreaInsets.top`. If the revealed title bar's top
  equals `max(menuBarHeight, safeAreaInsets.top)` within 1 pt, `fullScreenTop` is that maximum (Task 5);
  otherwise it is the measured offset, recorded as a formula over the same two values.
- [ ] **Step 8: Write `spike.md`** with the environment (macOS 26.x build, Qt 6.11.1, each screen and its scale),
  one table per decision with the values its rule read, the decision, and the rule line that selected it.
  Append the four rulings and the full-screen formula to `progress.md`, and update the board.

No commit.

---

### Task 2: The band's surface and geometry, as pure tables

> **Depends on Step 1c:** `window::WindowFrame` and `window::WindowMode` are 1c's, in `TitleBandLayout.hpp`
> (1c Task 3). `TitleBandLayout_test.cpp` is beside the new tests in `contour_test`.

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp`: three enums beside `Material` (`:19-26`).
- Create: `src/contour/window/TitleBandSurface.hpp`, `src/contour/window/TitleBandGeometry.hpp`.
- Test: `src/contour/window/TitleBandSurface_test.cpp`, `src/contour/window/TitleBandGeometry_test.cpp`, added
  to `contour_test` after 1c's `window/TitleBandLayout_test.cpp`.

**Interfaces:**
- Consumes: `config::ChromeStyle`, `config::Material`, `window::WindowFrame`, `window::WindowMode`,
  `vtbackend::RGBColor`, `vtbackend::relativeLuminance`, `vtbackend::contrastRatio`, `vtbackend::hslToRgb`
  (`vtbackend/core/Color.hpp:109-185`).
- Produces (`config/ChromeTokenGroups.hpp`):
  - `enum class BandBackdrop : uint8_t { Client = 0, NativeMaterial };` — what the OS draws behind the band.
  - `enum class BandFill : uint8_t { Palette = 0, Clear, Tint };` — what the band paints: today's palette
    window colour, nothing, or the terminal-background tint.
  - `enum class TitleBarGrowth : uint8_t { None = 0, CompactToolbar };`
- Produces (`window/TitleBandSurface.hpp`, namespace `contour::window`):
  - `struct TitleBandSurfaceInputs { config::ChromeStyle style = config::ChromeStyle::ContourLinux; config::Material material = config::Material::None; WindowFrame frame = WindowFrame::ClientBand; WindowMode mode = WindowMode::Windowed; };`
  - `struct TitleBandSurface { config::BandBackdrop backdrop = config::BandBackdrop::Client; config::BandFill fill = config::BandFill::Palette; config::TitleBarGrowth growth = config::TitleBarGrowth::None; bool operator==(TitleBandSurface const&) const = default; };`
  - `struct TitleBandSurfaceRow { config::ChromeStyle style; config::Material material; TitleBandSurface surface; };`
  - `inline constexpr auto TitleBandSurfaceRows = std::array { … };`:

    | style | material | backdrop | fill | growth |
    |---|---|---|---|---|
    | `MacOS` | `Glass` | `NativeMaterial` | D4's fill (`Clear` or `Tint`) | D2 |
    | `MacOS` | `Solid` | `Client` | `Tint` | D2 |

    If D4 = `Solid`, the MacOS row's token `material.titleBand` becomes `Solid` (`ChromeTokenGroups.hpp:346`,
    citing `spike.md`), and the `Glass` row stays as data for the day the material is usable.
  - `[[nodiscard]] constexpr TitleBandSurface resolveTitleBandSurface(TitleBandSurfaceInputs const& inputs) noexcept;`
    - `frame == NativeFrame` gives `TitleBandSurface {}` (no band, R-e);
    - else the first row matching `(style, material)`, else `TitleBandSurface {}`;
    - then the full-screen step (R-b): under `FullScreen`, `NativeMaterial` becomes `Client`, and `Clear`
      becomes `Tint`. `Palette` and `growth` are unchanged.
  - `inline constexpr double TitleBandDarkLuminance = 0.18;`
  - `enum class ChromeTone : uint8_t { Light = 0, Dark };`
  - `[[nodiscard]] constexpr ChromeTone toneOf(vtbackend::RGBColor color) noexcept;` (dark below 0.18).
  - `[[nodiscard]] constexpr vtbackend::RGBColor bandTintOf(vtbackend::RGBColor background) noexcept;`
    converts to HSL, moves the lightness by +4 points on a dark background and −4 on a light one, clamped,
    and converts back with `hslToRgb`. The RGB→HSL step is a local `constexpr` helper.
  - `[[nodiscard]] constexpr std::optional<vtbackend::RGBColor> legibleBandTint(vtbackend::RGBColor background, ChromeTone chrome) noexcept;`
    gives `bandTintOf(background)` when `toneOf(background) == chrome`, else `std::nullopt` (the caller uses
    the palette window colour, R-d).
- Produces (`window/TitleBandGeometry.hpp`):
  - `struct TitleBandTokens { config::ChromeStyle style = config::ChromeStyle::ContourLinux; config::Material material = config::Material::None; double height = 0; double cellHeight = 0; };`
    `height` is the band's token height in logical pixels (1c's `size.titleBar` on native rows; one cell,
    `chromeHeight`, on the Terminal row); `cellHeight` is 0 on native rows.
  - `enum class TrafficLightFit : uint8_t { AppKit = 0, SnapToFirstRow, GrowBandToCells };`
  - `inline constexpr TrafficLightFit TerminalRowTrafficLightFit = /* D3 */;` with a comment citing `spike.md`.
  - `struct TitleBandGeometryInputs { TitleBandTokens tokens {}; WindowMode mode = WindowMode::Windowed; double devicePixelRatio = 1; double nativeHeight = 0; double fullScreenTop = 0; };`
    `nativeHeight` is 0 where there is no native title bar (Linux, Windows, offscreen, a native frame).
  - `struct TitleBandGeometry { double height = 0; double revealTop = 0; std::optional<double> controlsCentreY {}; bool operator==(TitleBandGeometry const&) const = default; };`
  - `[[nodiscard]] constexpr TitleBandGeometry resolveTitleBandGeometry(TitleBandGeometryInputs const& inputs) noexcept;`
    - The fit is `TerminalRowTrafficLightFit` on the Terminal row and `AppKit` otherwise, and `AppKit`
      whenever `nativeHeight == 0`.
    - `AppKit`: `height = ceil(max(tokens.height, nativeHeight) · dpr) / dpr`; no centre.
    - `SnapToFirstRow`: `height = tokens.height`; `controlsCentreY = tokens.cellHeight / 2`.
    - `GrowBandToCells`: `height = cellHeight · ceil(max(tokens.height, nativeHeight) / cellHeight)`; no centre.
    - `revealTop = mode == FullScreen ? fullScreenTop : 0`.

- [ ] **Step 1: Write the failing tests.**
  - `TitleBandSurface_test.cpp` (`[titlebandsurface]`):
    - `surface.macRows`: `{ MacOS, Glass, ClientBand, Windowed }` gives `{ NativeMaterial, <D4 fill>, <D2> }`;
      `{ MacOS, Solid, ClientBand, Windowed }` gives `{ Client, Tint, <D2> }`.
    - `surface.otherRowsKeepToday` (Linux and Windows unchanged): for every `ChromeStyle` but `MacOS`, every
      `Material` (`iota` over the enumerators), both frames and both modes, the surface is
      `TitleBandSurface {}`.
    - `surface.nativeFrameHasNoBand`: `{ MacOS, Glass, NativeFrame, Windowed }` gives `TitleBandSurface {}`.
    - `surface.fullScreenIsSolid` (Review Focus 5): `{ MacOS, Glass, ClientBand, FullScreen }` gives backdrop
      `Client` and fill `Tint`, with the windowed row's growth.
    - `surface.rowsAreUnique`: no two rows of `TitleBandSurfaceRows` share `(style, material)`.
    - `tint.values`: `bandTintOf(0x000000) == 0x0A0A0A`, `bandTintOf(0xFFFFFF) == 0xF4F4F4` (`hslToRgb` truncates),
      `bandTintOf(0x1E1E1E) == 0x282828`; for every bundled scheme (`defaultColorPaletteNames()`), the tint's
      tone equals the background's and the tint differs from it.
    - `tint.legibility` (Review Focus 3): `legibleBandTint(0x1E1E1E, Light) == std::nullopt`;
      `legibleBandTint(0xFAFAFA, Light) == bandTintOf(0xFAFAFA)`; for every bundled scheme whose tint is
      given, `contrastRatio(tint, label)` ≥ 4.5, where `label` is the composite of 1c's `label(1)` alpha
      (88 % black on light, 92 % white on dark) over the tint.
  - `TitleBandGeometry_test.cpp` (`[titlebandgeometry]`), with `mac = { MacOS, Glass, 38, 0 }` and
    `tui = { Terminal, None, 17, 17 }`:
    - `geometry.nativeRows`: `{ mac, Windowed, 2, 28 }` → height 38; `{ mac, Windowed, 2, 52 }` → 52;
      `{ mac, Windowed, 1, 0 }` → 38 (no native title bar: the token, as off macOS).
    - `geometry.devicePixelSnap` (Review Focus 4): native 38.25 gives 38.5 at dpr 2 and 39 at dpr 1; the
      height times the dpr is an integer for every dpr in `{ 1, 2 }` and native height in
      `{ 0, 27.5, 28, 32.25, 38, 38.25, 52 }`.
    - `geometry.revealTop` (Review Focus 5): `{ mac, FullScreen, 2, 28, 24 }` has `revealTop == 24`, and the
      windowed one 0.
    - `geometry.terminalRow`: under `TerminalRowTrafficLightFit`, `{ tui, Windowed, 2, 28 }` gives
      `SnapToFirstRow` → `{ 17, 0, 8.5 }`, or `GrowBandToCells` → `{ 34, 0, nullopt }`, or `AppKit` →
      `{ 28, 0, nullopt }`. Write the one case that matches D3. With native height 0, every fit gives `{ 17 }`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `window/TitleBandSurface.hpp` not found.
- [ ] **Step 3: Implement** the enums and both headers as the Interfaces block states, with D2, D3 and D4 from
  the ledger's rulings. If D4 = `Solid`, change the MacOS row's `material.titleBand` and update
  `ChromeTokenGroups_test.cpp`'s MacOS material expectation in the same commit, citing `spike.md`.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[titlebandsurface],[titlebandgeometry],[titleband],[chrometokens]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/TitleBandSurface*.{hpp,cpp} src/contour/window/TitleBandGeometry*.{hpp,cpp}
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/config/ChromeTokenGroups* src/contour/window/TitleBandSurface* src/contour/window/TitleBandGeometry* src/contour/CMakeLists.txt
git commit -m "contour: decide the title band's backdrop, fill, height and reveal top from tables" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `platform::ITitleBandMaterial`, its null implementation, and the services seam

**Files:**
- Create: `src/contour/platform/TitleBandMaterial.hpp` and `src/contour/platform/TitleBandMaterial.cpp`; list
  both in `add_library(contour_platform …)` (`platform/CMakeLists.txt:15-69`).
- Modify: `src/contour/platform/AppearanceServices.hpp:17-50`: the `.titleBand` factory, and
  `nullAppearanceServices()` names it.
- Modify: `src/contour/test/AppearanceFixtures.hpp:101-127`: `.titleBand` in `appearanceServicesWith`, the
  recording double, and a three-switch overload.
- Modify: `src/contour/ContourGuiApp.hpp/.cpp` (beside `_nativeMenus`, `ContourGuiApp.cpp:457`,
  `ContourGuiApp.hpp:1011`): `_titleBandMaterial` and `titleBandMaterial()`.
- Test: new `src/contour/platform/TitleBandMaterial_test.cpp` in `contour_gui_test`'s sources (beside
  `platform/WindowResizeGrid_test.cpp`, `src/contour/CMakeLists.txt:826`), and
  `src/contour/test/ContourGuiApp_test.cpp`.

**Interfaces:**
- Consumes: Task 2's `config::BandBackdrop`, `config::TitleBarGrowth`.
- Produces (`platform/TitleBandMaterial.hpp`, namespace `contour::platform`):

```cpp
/// What the controller asks the OS to draw behind and around a window's title band.
struct TitleBandRequest
{
    config::BandBackdrop backdrop = config::BandBackdrop::Client;  ///< The OS's material, or nothing.
    config::TitleBarGrowth growth = config::TitleBarGrowth::None;  ///< Whether the title bar grows to the band.
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;             ///< The appearance; Unknown follows the app.
    std::optional<qreal> controlsCentreY {};                       ///< Traffic lights' centre from the top; none keeps the OS's.
    [[nodiscard]] bool operator==(TitleBandRequest const&) const = default;
};

/// The native title bar as measured, in logical pixels; all zero where there is none.
struct NativeTitleBarMetrics
{
    qreal height = 0.0;        ///< The title bar's height above the content layout rectangle.
    qreal fullScreenTop = 0.0; ///< In full screen, where a revealed title bar's top sits below the screen's top.
    [[nodiscard]] bool operator==(NativeTitleBarMetrics const&) const = default;
};

/// Draws the operating system's material behind a client-drawn title band, and measures its title bar.
class ITitleBandMaterial
{
  public:
    virtual ~ITitleBandMaterial() = default;
    /// Applies @p request to @p window. Best-effort: a window with no platform window yet is left alone.
    /// @pre GUI (main) thread.
    virtual void apply(QWindow& window, TitleBandRequest const& request) = 0;
    /// @return @p window's native title bar; zeros where there is none.
    /// @pre GUI (main) thread.
    [[nodiscard]] virtual NativeTitleBarMetrics metrics(QWindow& window) const = 0;
};
```

  - `class NullTitleBandMaterial final: public ITitleBandMaterial`, inline: `apply` does nothing, `metrics`
    returns `{}`.
  - `[[nodiscard]] std::unique_ptr<ITitleBandMaterial> makeTitleBandMaterial();` — the null one off Apple
    (`TitleBandMaterial.cpp`, wholly inside `#ifndef __APPLE__`); Task 5 defines the Apple one.
- Produces (`AppearanceServices`): `std::function<std::unique_ptr<ITitleBandMaterial>()> titleBand = makeTitleBandMaterial;`
  Until Task 5, the Apple build has no `makeTitleBandMaterial`, so this task adds a temporary Apple definition
  returning the null one at the end of `TitleBandMaterial.cpp` under `#else`, which Task 5 deletes.
- Produces (`test/AppearanceFixtures.hpp`):
  - `struct TitleBandRecord { std::vector<platform::TitleBandRequest> applied {}; platform::NativeTitleBarMetrics metrics {}; int measured = 0; };`
  - `class RecordingTitleBandMaterial final: public platform::ITitleBandMaterial` appends each applied request
    and counts each `metrics()` call, returning `record.metrics`.
  - `[[nodiscard]] platform::AppearanceServices appearanceServicesWith(AccessibilitySwitch&, AppearanceSwitch&, TitleBandRecord&);`
    The two-switch overload calls it with no record and passes the null material.
- Produces (`ContourGuiApp`): `[[nodiscard]] platform::ITitleBandMaterial& titleBandMaterial() noexcept;`,
  built once from `appearanceServices.titleBand()` in the member-initializer list after `_menuBarIntegration`.

- [ ] **Step 1: Write the failing tests.**
  - `TitleBandMaterial_test.cpp` (`[titleband-material]`). It runs on Linux, Windows and macOS in CI, which
    is how "Linux and Windows unaffected" is proved for the adapter:
    - "the null material does nothing and measures nothing": on a `QWindow`, `apply` with a `NativeMaterial`
      request leaves `window.flags()` unchanged, and `metrics(window) == NativeTitleBarMetrics {}`.
    - "the native factory is null off the Cocoa plugin": under the suite's offscreen QPA,
      `makeTitleBandMaterial()->metrics(window) == NativeTitleBarMetrics {}` on every OS.
    - "the null services build the null material": `dynamic_cast<NullTitleBandMaterial*>(nullAppearanceServices().titleBand().get()) != nullptr`.
  - `ContourGuiApp_test.cpp`, `[contour][app][appearance]`, "the app builds its title-band material through
    its services": a `TitleBandRecord`, a `TestApp` built with the three-switch `appearanceServicesWith`;
    `app.app().titleBandMaterial().apply(window, {})` appends one request to the record.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, `platform/TitleBandMaterial.hpp` not found.
- [ ] **Step 3: Implement** the header, the factory, the services field, the fixtures and the app member.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[titleband-material],[contour][app][appearance],[appearance-source]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/TitleBandMaterial*.{hpp,cpp} src/contour/platform/AppearanceServices.hpp src/contour/test/AppearanceFixtures.hpp src/contour/test/ContourGuiApp_test.cpp src/contour/ContourGuiApp.{hpp,cpp}
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform src/contour/test/AppearanceFixtures.hpp src/contour/test/ContourGuiApp_test.cpp src/contour/ContourGuiApp.* src/contour/CMakeLists.txt
git commit -m "platform: a title-band material interface with a null implementation, built through the appearance services" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The safe-area branch (D1)

Follow the branch `spike.md` selected. Only that branch's steps run; the other is deleted from the task's
brief.

**Files:**
- Modify: `src/contour/platform/CocoaWindowFrame.mm:44-65` and its doc comment (`CocoaWindowFrame.hpp:11-18`).
- Branch A only: `src/contour/platform/NativeWindowFrame.hpp:79-146` (`FramePolicy`), `src/contour/window/WindowController.{hpp,cpp}`
  (beside `needsFramelessHint`, `.hpp:116`, `.cpp:1034-1037`), `src/contour/qml/Main.qml:38`,
  `src/contour/CMakeLists.txt` (beside `CONTOUR_QT_VALIDATED_VERSION`).
- Test: `src/contour/platform/NativeWindowFrame_test.cpp`; new `src/contour/platform/CocoaWindowFrame_test.mm`
  (ARC; `contour_gui_test`'s `if(APPLE)` block and its ARC list).

**Interfaces:**
- **Branch A (`Hint`): Qt owns the expansion.**
  - `enum class ClientAreaExpansion : uint8_t { None = 0, QtHint };` and `FramePolicy::expansion`. The MacOS
    client row is `QtHint`; every other row and `ServerFramePolicy` is `None`. Every `FramePolicy` literal
    names it.
  - `WindowController`: `Q_PROPERTY(bool needsExpandedClientArea READ needsExpandedClientArea NOTIFY titleBarVisibleChanged)`
    (QML-facing carve-out, documented as `needsFramelessHint` is) and
    `[[nodiscard]] bool needsExpandedClientArea() const noexcept;` = `framePolicy().expansion == QtHint`.
  - Main.qml's `flags` gains `| ((win && win.needsExpandedClientArea) ? Qt.ExpandedClientAreaHint : 0)`.
  - `CocoaWindowFrame::apply` loses the style-mask and transparency lines in both paths (`:48-49`, `:58-59`).
    It keeps `titleVisibility` only if H1 found the title visible, and keeps `hasShadow = YES`.
  - The configure step rejects a macOS Qt below 6.9: `if(APPLE AND Qt6_VERSION VERSION_LESS 6.9)`
    `message(FATAL_ERROR "macOS builds need Qt 6.9 or later (Qt::ExpandedClientAreaHint, QWindow::safeAreaMargins).")`
    (owner, 1). The `.mm`s that use either API keep the `QT_VERSION_CHECK(6, 9, 0)` guard, with an `#error`
    naming the floor in the `#else`.
- **Branch B (`Manual`): the frame keeps the expansion.**
  - `CocoaWindowFrame::apply` keeps the style mask and `titleVisibility`, and drops only
    `titlebarAppearsTransparent = YES` (`:59`), which becomes the material's (R-e). The server path keeps
    `titlebarAppearsTransparent = NO` (`:49`): a native title bar is opaque.
  - The class comment records the decision: "Keeps the manual style mask; the band's height is measured
    from `contentLayoutRect` (Step 1d spike, D1 = Manual: <the failed H-rule>)."
- In both branches `nativeControlsInset()` (`:81-97`) is unchanged.

- [ ] **Step 1: Write the failing tests.**
  - Branch A, `NativeWindowFrame_test.cpp` (`[contour][frame]`), "framePolicyFor expands the client area
    through Qt on macOS only": `framePolicyFor(MacOS, Client).expansion == QtHint`; every other
    `(platform, decoration)` pair gives `None`.
  - `CocoaWindowFrame_test.mm` (`[cocoa][frame]`, display-gated as Global Constraints say), on a shown
    `QQuickWindow` with the frame applied as `WindowController` applies it (Branch A: with the hint flag set):
    - "the client band keeps a full-size content view and the shadow": the FullSizeContentView bit is set,
      the title is hidden, `hasShadow == YES`.
    - "the native title bar is an ordinary one": after `apply(window, Server)` (Branch A: and clearing the
      hint flag), the bit is clear and the title is visible.
    - Branch B only: "the frame leaves the title bar's transparency to the material":
      `titlebarAppearsTransparent` after `apply(window, Client)` equals its value before.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[contour][frame],[cocoa][frame]"`

  Expected: FAIL (Branch A: `expansion` is not a member; Branch B: the transparency changes).
- [ ] **Step 3: Implement** the selected branch.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[contour][frame],[cocoa][frame]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][frame],[cocoa][frame],[contour][gui][qml][mainwindow]"
```

  Expected: PASS; the offscreen run reports the `[cocoa]` cases as skipped.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/CocoaWindowFrame.{hpp,mm} src/contour/platform/CocoaWindowFrame_test.mm src/contour/platform/NativeWindowFrame*.{hpp,cpp} src/contour/window/WindowController.{hpp,cpp}
editorconfig-checker src/contour/qml/Main.qml src/contour/CMakeLists.txt
git add src/contour/platform src/contour/window/WindowController.* src/contour/qml/Main.qml src/contour/CMakeLists.txt
git commit -m "platform: one owner for the macOS full-size content view, as the 1d spike decided" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: The Cocoa title-band material

**Files:**
- Create: `src/contour/platform/CocoaTitleBandMaterial.mm` (ARC; `platform/CMakeLists.txt`'s `if(APPLE)`
  `target_sources` and its `-fobjc-arc` list). Delete Task 3's temporary Apple factory.
- Test: new `src/contour/platform/CocoaTitleBandMaterial_test.mm` (ARC; `contour_gui_test`'s `if(APPLE)`
  block and its ARC list).

**Interfaces:**
- Consumes: Task 3's `ITitleBandMaterial`, `TitleBandRequest`, `NativeTitleBarMetrics`; D1, D2 and the
  full-screen formula from `spike.md`.
- Produces: `makeTitleBandMaterial()` on Apple: a `CocoaTitleBandMaterial` when `platformName() == "cocoa"`,
  else `NullTitleBandMaterial`.
- `apply(window, request)`, `@pre` main thread (`Require(NSThread.isMainThread)`), returns at once when
  `window.handle() == nullptr`. Otherwise, in this order:
  1. Branch B: `titlebarAppearsTransparent = backdrop == NativeMaterial ? NO : YES`. Branch A: nothing; the
     controller sets Qt's `NoTitleBarBackgroundHint` (Task 6).
  2. `appearance`: `Dark` → `NSAppearanceNameDarkAqua`, `Light` → `NSAppearanceNameAqua`, `Unknown` → `nil`.
  3. `growth == CompactToolbar`: attach, once per window, an empty `NSToolbar` (identifier
     `org.contourterminal.titleband`, `allowsUserCustomization = NO`) with
     `toolbarStyle = NSWindowToolbarStyleUnifiedCompact`; `None` detaches a toolbar this class attached.
  4. `controlsCentreY` (Task 8; until then ignored).
  5. `hasShadow = YES`, then `[nsWindow invalidateShadow]` (§3.5: re-assert after `apply()`).
- `metrics(window)`, `@pre` main thread: zeros without a platform window. `height` is
  `safeAreaMargins().top` (Branch A, behind `QT_VERSION_CHECK(6, 9, 0)`) or
  `NSHeight(frame) - NSHeight(contentLayoutRect)` (Branch B). `fullScreenTop` is the spike's formula over
  `NSApp.mainMenu.menuBarHeight` and the window's screen's `safeAreaInsets.top` while the style mask has
  `NSWindowStyleMaskFullScreen`, else 0.

- [ ] **Step 1: Write the failing tests** in `CocoaTitleBandMaterial_test.mm`, tag `[cocoa][titleband]`,
  display-gated, each on a shown 900 × 600 `QQuickWindow` with `color: transparent` whose frame has been
  applied (Task 4's way):
  - "the native backdrop shows AppKit's material and keeps the shadow": `apply({ .backdrop = NativeMaterial })`
    gives `titlebarAppearsTransparent == NO` (Branch A: with the hint flags as the controller sets them) and
    `hasShadow == YES`; `apply({ .backdrop = Client })` gives `YES` and `hasShadow == YES`.
  - "a light request gives a light window under a dark app" (Review Focus 3): with
    `NSApp.appearance = DarkAqua` (restored at the end), `apply({ .scheme = Light })` gives
    `[nsWindow.effectiveAppearance bestMatchFromAppearancesWithNames:@[Aqua, DarkAqua]] == Aqua`;
    `Unknown` gives `DarkAqua`.
  - "metrics measure the title bar": `metrics(window).height > 0`, and equals
    `NSHeight(frame) - NSHeight(contentLayoutRect)` within 0.5; with `growth = CompactToolbar` (only if D2
    chose it), `metrics(window).height ≥ 37.5`.
  - "forty resizes keep the shadow and the title-bar setting" (Review Focus 1): after `apply(NativeMaterial)`
    and forty `setFrame:display:YES` steps of ±7 pt, `hasShadow == YES` and `titlebarAppearsTransparent == NO`.
  - "full screen and back" (Review Focus 5): `toggleFullScreen:`, wait (`QTest::qWaitFor`, 5 s) for the
    full-screen style mask; `metrics(window).fullScreenTop > 0`. Toggle back and wait; after
    `apply(NativeMaterial)`, `hasShadow == YES`, `titlebarAppearsTransparent == NO`, `fullScreenTop == 0`.
  - "a second screen measures again" (Review Focus 4): `SKIP` with one screen. Otherwise move the window to the
    screen with the other `backingScaleFactor`, wait for `screenChanged`; `metrics(window).height` equals the
    measured title bar there, and `hasShadow == YES` after `apply`.
  - "no window, no AppKit": a `QWindow` never created gives `metrics(window) == {}`, and `apply` does not
    create it (`window.handle() == nullptr` afterwards).
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[cocoa][titleband]"`

  Expected: FAIL: the temporary factory gives the null material.
- [ ] **Step 3: Implement** `CocoaTitleBandMaterial.mm` as stated. The class comment cites `spike.md` for D1,
  D2 and the full-screen formula.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[cocoa][titleband],[cocoa][frame]"
out/appleclang-debug/src/contour/contour_gui_test "[cocoa][titleband],[titleband-material]"
```

  Expected: PASS; offscreen, the `[cocoa]` cases skip and `[titleband-material]` still sees the null one.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/CocoaTitleBandMaterial*.mm src/contour/platform/TitleBandMaterial.cpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform src/contour/CMakeLists.txt
git commit -m "platform: AppKit's title-bar material behind the macOS band, with the shadow re-asserted" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: The controller applies the band's request and publishes its geometry

> **Depends on Step 1c:** `WindowMode` and `WindowFrame` come from 1c's `TitleBandLayout.hpp`; 1c's
> `bindWindow` changes (`visibilityChanged` → `markVisibleSessionsSeen`) sit beside the hooks this task adds.

**Files:**
- Modify: `src/contour/window/WindowController.hpp/.cpp`:
  - the constructor (`:80-85`): the colour-scheme lambda calls the new `effectiveColorScheme()`;
  - `eventFilter` (`:1434-1478`): Expose and `ApplicationPaletteChange` call `refreshTitleBand()` after the
    frame's `apply`;
  - `onWindowScaleMaybeChanged` (`:1494-…`) and `onWindowStateChanged`: call `refreshTitleBand()`;
  - `setTitleBarVisible` (`:1054-…`): forgets the last applied request when the decoration changes;
  - Branch A: `bindWindow` connects `QWindow::safeAreaMarginsChanged` to `refreshTitleBand`.
- Modify: `src/contour/session/TerminalSessionManager.hpp/.cpp` (beside `controllerFor`, `.hpp:753`).
- Modify: `src/contour/ContourGuiApp.hpp/.cpp`: `refreshChromeStyle` (`.cpp:1806-1812`) and `titleBandTokens()`.
- Test: new `src/contour/window/TitleBandController_test.cpp`, in `contour_gui_test_heavy_sources`
  (`src/contour/CMakeLists.txt:861-…`), because it builds a `TestApp`.

**Interfaces:**
- Consumes: Task 2 (`resolveTitleBandSurface`, `resolveTitleBandGeometry`, `TitleBandTokens`), Task 3
  (`titleBandMaterial()`, the recording fixtures).
- Produces (`ContourGuiApp`): `[[nodiscard]] window::TitleBandTokens titleBandTokens() const;` From the
  published provider when `_chromeStyleHost` exists: `style = _chromeStyle`, `material` = the resolved
  `material.titleBand`, `height` = 1c's band token in pixels, `cellHeight` = the provider's `cellHeight` on the
  Terminal row, else 0. Without a host: `{ .style = _chromeStyle, .material = Material::None }`.
- Produces (`WindowController`):
  - `void refreshTitleBand();` computes `titleBandRequest()` and `titleBandGeometry()`. It applies the request
    through `_manager.app().titleBandMaterial()` only when `framePolicy().shadow == FrameShadowStrategy::FullSizeContent`
    and the request differs from `_appliedTitleBand` (a `std::optional<platform::TitleBandRequest>`
    member). It emits `titleBandChanged()` when the geometry or the request changed.
  - `[[nodiscard]] platform::TitleBandRequest titleBandRequest() const;` from
    `resolveTitleBandSurface({ tokens.style, tokens.material, frame, mode })`, `effectiveColorScheme()`, and
    the geometry's `controlsCentreY`. `frame` follows `titleBarVisible()`, and `mode` follows
    `_osWindow->windowStates() & Qt::WindowFullScreen`.
  - `[[nodiscard]] TitleBandGeometry titleBandGeometry() const;` from `titleBandTokens()`, the mode,
    `_osWindow->devicePixelRatio()` and `titleBandMaterial().metrics(*_osWindow)`.
  - `Q_PROPERTY(qreal titleBandHeight READ titleBandHeight NOTIFY titleBandChanged)` and
    `Q_PROPERTY(qreal titleBandRevealTop READ titleBandRevealTop NOTIFY titleBandChanged)`.
  - Branch A: `Q_PROPERTY(bool clearsTitleBarBackground READ clearsTitleBarBackground NOTIFY titleBandChanged)`,
    true when the request's backdrop is `Client`; Main.qml's `flags` gains
    `| ((win && win.clearsTitleBarBackground) ? Qt.NoTitleBarBackgroundHint : 0)`.
  - `[[nodiscard]] Qt::ColorScheme effectiveColorScheme() const;` (private, R-f).
- Produces (`TerminalSessionManager`): `void refreshTitleBands();` calls `refreshTitleBand()` on every
  controller in `_controllersByWindow`. `ContourGuiApp::refreshChromeStyle()` calls it after the host's `update`.

- [ ] **Step 1: Write the failing tests** in `TitleBandController_test.cpp`, tag `[contour][titleband][controller]`.
  Each builds a `TestApp` with `appearanceServicesWith(accessibility, appearance, record)`, a
  `ScopedControllerWithOsWindow` bound with `bindWindow(osWindow.get())`, and the record's
  `metrics = { .height = 28, .fullScreenTop = 24 }`. On a host whose frame policy is not `FullSizeContent`
  (Linux, Windows), the apply-count cases assert `record.applied.empty()` instead, which is how CI proves
  those hosts unaffected. The macOS expectations:
  - "an expose applies once, and a resize re-applies nothing" (Review Focus 1): after a first
    `refreshTitleBand()`, `record.applied.size() == 1`; twenty `QEvent::Expose`s and twenty resizes of the
    window (`osWindow->resize`) leave it at 1.
  - "Reduce Transparency switches the request live, and back" (Review Focus 2): with the GUI's chrome style
    host built (as `[contour][app][appearance]` cases do), `accessibility.set({ .transparency = Reduced })`
    makes the last request's backdrop `Client`; `set({})` makes it `NativeMaterial` again (or `Client` both
    times if D4 = `Solid`, asserted from `TitleBandSurfaceRows`). `titleBandHeight()` does not change across
    the switch.
  - "the request follows the applied chrome scheme" (Review Focus 3): with `appliedGuiScheme() == Light`
    (`app.applyGuiTheme` with `theme: light`), the last request's `scheme == Qt::ColorScheme::Light`; with
    `Dark`, `Dark`.
  - "a scale change measures the title bar again" (Review Focus 4): sending `QEvent::DevicePixelRatioChange`
    to the window raises `record.measured`, and with `record.metrics.height = 38.25` before it,
    `titleBandHeight() * osWindow->devicePixelRatio()` is an integer.
  - "full screen asks for the client backdrop, and back" (Review Focus 5): `osWindow->setWindowStates(Qt::WindowFullScreen)`
    gives a last request with `backdrop == Client` and `titleBandRevealTop() == 24`, with one
    `titleBandChanged` (`QSignalSpy`); `setWindowStates(Qt::WindowNoState)` gives the windowed backdrop and
    `titleBandRevealTop() == 0`.
  - "show_title_bar applies no material, and turning it off applies again" (Also pinned):
    `setTitleBarVisible(true)` then `refreshTitleBand()` adds nothing to `record.applied`;
    `setTitleBarVisible(false)` adds one request.
  - "the band is at least the token tall": `titleBandHeight() == max(titleBandTokens().height, 28)` on a macOS
    host, and `titleBandTokens().height` elsewhere.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, `refreshTitleBand` is not a member.
- [ ] **Step 3: Implement** the controller members, the manager's fan-out, `titleBandTokens()` and the hooks.
  The frame keeps its own `apply` on Expose and on palette change; `refreshTitleBand()` runs after it, so the
  material's settings win (R-e).
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][titleband][controller],[contour][app][appearance],[manager],[contour][multiwindow],[contour][frame]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/WindowController.{hpp,cpp} src/contour/window/TitleBandController_test.cpp src/contour/session/TerminalSessionManager.{hpp,cpp} src/contour/ContourGuiApp.{hpp,cpp}
editorconfig-checker src/contour/CMakeLists.txt src/contour/qml/Main.qml
git add src/contour/window src/contour/session src/contour/ContourGuiApp.* src/contour/qml/Main.qml src/contour/CMakeLists.txt
git commit -m "contour: each window asks for its title-band material and measures its band, live across Reduce Transparency, scale and full screen" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The band paints its surface, takes the measured height, and reveals below the menu bar

> **Depends on Step 1c:** the band is 1c's `TitleBar.qml` with its `presence`/`centre`/`revealed`/`slideOffset`
> properties, placed by Main.qml's `band` (`chromeStyle.titleBand(frame, mode, strip, position)`) with the
> `bandRevealEdge` item and the `tabsBar`. 1c leaves the band's fill as `systemPalette.window` (1c R-h; today's
> `TitleBar.qml:58-61`) and its height as the `size.titleBar` token. The snapshot rows carry 1c's `.decor`,
> `.drift`, `.frame` and `.mode` fields. Locate each by these names.

**Files:**
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}`: `bandFill`.
- Modify: `src/contour/qml/TitleBar.qml`: the background `Rectangle` gets `objectName: "bandBackground"` and
  `color: root.fill`; `property color fill`; the band's height binding.
- Modify: `src/contour/qml/Main.qml`: assigns `titleBar.fill`, its height, and its full-screen top. `tabsBar`
  keeps the token height and the palette fill: it carries no traffic lights.
- Modify: `src/contour/test/MockMainController.hpp` (`titleBandHeight`, `titleBandRevealTop`, each with a
  setter for tests), `src/contour/test/MainWindowControllerMock.hpp`.
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` and `ChromeSnapshotScene.hpp`: `SnapshotScriptRow` gains
  `config::TransparencyPreference transparency`, every row names it (`Allowed`), and a new `band-solid` row
  (`SnapshotScript::BandSolid`, `single-tab`'s fields with `.transparency = Reduced`) is appended. The scene
  builds its chrome with that preference.
- Test: `src/contour/window/UiStyleProvider_test.cpp`, `src/contour/test/MainWindowQml_test.cpp`
  (`[contour][gui][qml][mainwindow][band]`), `ChromeSnapshotPlan_test.cpp`, `ChromeSnapshot_test.cpp`.

**Interfaces:**
- Consumes: Task 2 (`resolveTitleBandSurface`, `legibleBandTint`, `toneOf`), Task 6 (`titleBandHeight`,
  `titleBandRevealTop`).
- Produces (`UiStyleProvider`): `[[nodiscard]] Q_INVOKABLE QColor bandFill(int frame, int mode) const;`
  resolves the surface from the row, the resolved `material.titleBand`, `WindowFrame(frame)` and
  `WindowMode(mode)` (out-of-range values give `Palette`):
  - `Palette` → the application palette's window colour (today's fill);
  - `Clear` → `Qt::transparent`;
  - `Tint` → `legibleBandTint(scheme Background, chrome tone)` as a `QColor`, else the palette window colour.
    The chrome tone is decided as `statusColor` decides light (`UiStyleProvider.cpp:243-259`).
  The `.cpp` `static_assert`s `window::TitleBandDarkLuminance == platform::DarkBackgroundLuminance`.
- Produces (QML):
  - `TitleBar.fill`, assigned in Main.qml as `chromeStyle.bandFill(<frame>, <mode>)`, with the same frame and
    mode expressions Main.qml passes to `titleBand(…)`.
  - The band's height: `(appWindow.win && appWindow.win.titleBandHeight) || <1c's token binding>`.
  - In full screen, the revealed band's resting `y` is `appWindow.win.titleBandRevealTop` instead of 0; 1c's
    slide and cross-fade run from there. `chromeHeight` still excludes the revealed band (1c).
  - `objectName`: `bandBackground`.

- [ ] **Step 1: Write the failing tests.**
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`), "bandFill follows the material, the mode and the
    tone":
    - MacOS row, windowed: `bandFill(0, 0)` is transparent if D4's fill is `Clear`, else the tint of the
      scheme's background;
    - MacOS row under `{ .transparency = Reduced }`: an opaque colour equal to
      `legibleBandTint(background, tone)` (Review Focus 2);
    - MacOS row, full screen (`mode` 1): opaque (Review Focus 5);
    - a dark scheme background (`#1E1E1E`) under a light palette (`#F5F5F7`): the palette window colour
      (Review Focus 3);
    - Fluent, ContourLinux and Terminal rows, every frame and mode: the palette window colour (Linux and
      Windows unchanged);
    - `bandFill(7, 9)` is the palette window colour.
  - `MainWindowQml_test.cpp`, `[contour][gui][qml][mainwindow][band]`:
    - "the macOS band paints its surface (offscreen)": `ChromeSetup { .row = MacOS }`;
      `bandBackground.color == chromeStyle.bandFill(0, 0)`; the panes' `content` item is unaffected.
    - "the band is the tint under Reduce Transparency (offscreen)": the chrome installed from
      `{ .transparency = Reduced }` gives `bandBackground.color.alpha == 1` and equal to `bandFill(0, 0)`.
    - "the band takes the controller's height (offscreen)": with the mock's `titleBandHeight` set to 52,
      `titleBar.height == 52` and `controller.chromeHeight()` counts 52 while the band is shown.
    - "the revealed band sits at the reveal top (offscreen)" (Review Focus 5): MacOS row,
      `root->showFullScreen()`, the mock's `titleBandRevealTop` set to 24; after `QTest::mouseMove` onto
      `bandRevealEdge`, `titleBar.revealed` is true, `titleBar.y == 24`, `bandBackground.color.alpha == 1`,
      and `content.y`/`content.height` are unchanged. Under `MotionPreference::Reduced`, `titleBar.y == 24`
      throughout the fade.
    - "Fluent and ContourLinux bands keep today's colour (offscreen)": `bandBackground.color == palette.window`,
      and 1c's "the Fluent and ContourLinux rows keep today's geometry" passes unedited.
    - The ContourTui pass (the TUI row): under `ChromeSetup { .row = Terminal }`, `bandBackground.color ==
      palette.window` and `titleBar.height == chromeStyle.cellHeight` while the mock's `titleBandHeight` is 0.
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`):
    `scriptRow(SnapshotScript::BandSolid).transparency == TransparencyPreference::Reduced`, and every other row
    is `Allowed`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): `band-solid` builds on the MacOS and Terminal
    rows, and its `bandBackground` is opaque on MacOS.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow][band]"`

  Expected: FAIL, `bandBackground` not found.
- [ ] **Step 3: Implement** the invokable, the QML bindings, the mock properties and the snapshot row.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[contour][gui][qml][mainwindow],[contour][gui][qml][snapshot],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS, with no new colour or size literal.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/UiStyleProvider.{hpp,cpp} src/contour/window/UiStyleProvider_test.cpp src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp src/contour/test/ChromeSnapshot*.{hpp,cpp} src/contour/test/MainWindowQml_test.cpp
editorconfig-checker src/contour/qml/TitleBar.qml src/contour/qml/Main.qml scripts/qml-size-literal-baseline.txt
git add src/contour/window src/contour/qml src/contour/test scripts/qml-size-literal-baseline.txt
git commit -m "contour: the macOS band shows the material or its solid tint, takes the measured height, and reveals below the menu bar in full screen" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: The Terminal row's traffic lights on macOS (D3)

> **Depends on Step 1c:** the horizontal inset is 1c's `chromeStyle.snapToCells(nativeControlsInset)` (1c R-q);
> 1c Task 7's "the Terminal row snaps the native inset to cells" case is beside this one.

If D3 = `AppKit`, this task only records the parked item in the ledger and has no commit.

**Files:**
- Modify (`SnapToFirstRow`): `src/contour/platform/CocoaTitleBandMaterial.mm`: `apply`'s step 4.
- Test: `src/contour/platform/CocoaTitleBandMaterial_test.mm`; `src/contour/window/TitleBandController_test.cpp`.

**Interfaces:**
- Consumes: Task 2's `TerminalRowTrafficLightFit` and `controlsCentreY`; Task 6's request.
- **`SnapToFirstRow`:** with a `controlsCentreY`, `apply` records each standard button's original origin once,
  then sets each button's origin `y` so its centre lies `controlsCentreY` below the window's top, converting
  with the superview's `isFlipped` as `spike.md` recorded it. It keeps the buttons' `x`. It re-asserts the
  position on exactly the notifications `spike.md` lists, through observers owned by an object associated
  with the `NSWindow` (`objc_setAssociatedObject`), so they die with it. Without a `controlsCentreY`, it
  restores the original origins and removes the observers.
- **`GrowBandToCells`:** no AppKit change; Task 2's geometry already grows the band, and this task's tests
  pin it.

- [ ] **Step 1: Write the failing tests.**
  - `SnapToFirstRow`, `CocoaTitleBandMaterial_test.mm`, `[cocoa][titleband][trafficlights]`, display-gated:
    - "the lights sit on the first cell row": `apply({ .controlsCentreY = 9 })` puts the close button's centre
      9 ± 0.5 pt below the window's top, and the zoom button's `x` is unchanged;
    - "and stay there": after twenty `setFrame:` resizes, a posted `NSWindowDidEndLiveResizeNotification`,
      and a second window taking and giving back key status, still 9 ± 0.5;
    - "and leave": `apply({})` puts them back at the recorded origins.
  - Both branches, `TitleBandController_test.cpp`, "the Terminal row's request carries the geometry's
    centre": the last request's `controlsCentreY == titleBandGeometry().controlsCentreY`, on whatever row the
    `TestApp` has. On a native row both are `std::nullopt`. Task 2's `geometry.terminalRow` pins the value.
  - `GrowBandToCells`, `MainWindowQml_test.cpp`, ContourTui pass, "the band grows to the title bar in whole
    cells (offscreen)": with the mock's `titleBandHeight` set to `2 · chromeStyle.cellHeight`,
    `titleBar.height` is that value, and the leading slot's width is still
    `chromeStyle.snapToCells(nativeControlsInset)`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[cocoa][titleband][trafficlights]"`

  Expected: FAIL: the lights stay where AppKit put them.
- [ ] **Step 3: Implement** the selected fit.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[cocoa][titleband]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][titleband][controller]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow]"
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/CocoaTitleBandMaterial*.mm src/contour/window/TitleBandController_test.cpp src/contour/test/MainWindowQml_test.cpp
git add src/contour/platform src/contour/window/TitleBandController_test.cpp src/contour/test/MainWindowQml_test.cpp
git commit -m "contour: the Terminal row's traffic lights sit on its first cell row on macOS" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts CMakeLists.txt`,
  so the plan's own commit is left out. Apply its findings, re-run the filters of every task it touches, and
  commit once with the Signed-off-by line.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[titlebandsurface],[titlebandgeometry],[titleband],[chrometokens],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[titleband-material],[contour][app][appearance],[appearance-source],[contour][frame],[contour][titleband][controller],[manager],[contour][multiwindow],[contour][uistyle],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][snapshot]"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[cocoa][frame],[cocoa][titleband]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, with
  `SnapshotScripts.size()` read from `ChromeSnapshotPlan.hpp`. Look at `band-solid.macos.*` and
  `single-tab.macos.*` (the same opaque tint, light and dark: under S6 the MacOS band is `Solid`, so Reduce
  Transparency changes nothing), and `macos-host.terminal.*` (the Terminal row on a macOS host: its band grown
  to whole cells around the traffic lights, S3). `three-tabs.*` and `single-tab.*` on Fluent, Linux and
  Terminal look as they did after 1c.
- [ ] **Step 4: Owner manual checks.** Batched per phase (ruling): list them in the ledger for the owner, and do
  not wait on them. On this Mac, `out/appleclang-debug/src/contour/contour.app`:
  - Windowed, with the OS in light and then dark mode: the band is the opaque tint of the terminal background
    (S6: `Solid`, no material), the panes are opaque, and the shadow and rounded corners are there. Note
    whether macOS 26's toolbar glass draws over the tint, and whether a hairline shows at y 40.
  - Drag-resize quickly for five seconds: no flash in the band, and the shadow follows.
  - Toggle Reduce Transparency (System Settings › Accessibility › Display) with the window open: the band
    stays the same solid tint, and the terminal does not resize.
  - `theme: terminal` with contour-light while the OS is dark: a light band with legible labels.
  - Move the window to a display with another scale: the band's height and the shadow are right.
  - Full screen: the revealed band sits below AppKit's revealed strip with the traffic lights (a window of
    its own, which takes the clicks over itself: final review finding 3), and is solid; its top is right
    once the animation settles (M7), also after toggling full screen twice quickly. Moving down from the
    screen edge across the menu bar keeps the band revealed. Click a tab while the band is revealed: the tab
    is selected. Whether AppKit's strip ever hides again in full screen, or stays over the window's top
    40 pt, and whether the terminal's top rows under it take clicks. A pinned band (kill the
    daemon connection) rests at the top and slides down below the menu bar on hover. After leaving, the
    tint and the shadow return.
  - Click another app: the inactive title bar goes flat, as AppKit draws it (R-g), and the labels dim.
  - `ui_style: terminal`: the traffic lights sit per D3, and stay through a resize, a full-screen exit and a
    focus change.
  - `show_title_bar: true`: an ordinary title bar, no tint.
  - Record what was seen against the spike's measurements in the ledger.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Shows the macOS title-bar material behind the tab bar, tinted from the terminal background, with a solid
    tab bar under Reduce Transparency and in full screen." (Reword per D4: under `Solid`, "Paints the macOS
    tab bar in a tint of the terminal background".)
  - "Sizes the macOS tab bar from the window's own title bar, and places it below the menu bar in full
    screen."
  - Under D3 = `SnapToFirstRow` or `GrowBandToCells`: "Aligns the macOS window buttons with the tab row under
    `ui_style: terminal`."
- [ ] **Step 6:** Mark Step 1d done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger and `spike.md` to
  `docs/superpowers/ledgers/2026-10-25-native-redesign-step-1d.md`. Update the PR body with `/update-pr`,
  reporting:
  - **Performance:** none on the render path. The material is applied only when its request changes; the
    title bar is measured on expose, scale, state and screen changes; one `invalidateShadow` per applied
    change.
  - **Risk:** medium. AppKit's title bar now draws a material over the band, the full-size content view's
    owner may have moved (D1), a toolbar may be attached (D2), and AppKit's buttons may be moved (D3).
    Linux and Windows: low; their path is the null material and `Palette`.
  - **Coverage:** the two pure tables, the controller cases with a recording material, the `[cocoa]` cases
    (display-only; no CI lane runs them, as with 1b's `[shortcutoverride]`), the `[band]` property tests and
    the `band-solid` snapshot.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Under D1 = Hint, macOS builds need Qt ≥ 6.9 (Task 4).** "Delete the manual style-mask code, keeping a
   single owner" is only possible if a macOS build can never fall back to it; a `QT_VERSION_CHECK` that kept
   the manual code for older Qt would keep two owners. CI and Homebrew are at 6.11.1. *Default: the configure
   step rejects a macOS Qt below 6.9.* The alternative keeps the manual code under `#if QT_VERSION < 6.9`.
2. **Full screen is solid, not glass (R-b).** The revealed band overlays terminal rows, and the design record
   forbids glass behind terminal text. *Default: solid tint in full screen.*
3. **The tint is app-wide (R-d)**, from the default profile's scheme, as the rest of the chrome. A window whose
   active session uses another scheme keeps the app's tint. *Default: app-wide.* Per-window tints would make
   the provider per-window.
4. **If D2 selects it, an empty compact `NSToolbar` grows the title bar to 38 pt.** The spike rejects it if
   AppKit adds a View-menu item. *Default: accept it.* The alternative is a band shorter than the canvas,
   or a tinted strip below AppKit's material.

## Spec gaps found

- §3.5 says the band is "cleared to transparent"; the kit says "material + terminal background tint". Both
  hold only for a behind-window material; D4 picks `Clear` or `Tint` from the measured blending mode (R-a).
- §3.5 does not say whether AppKit's title bar is as tall as the 38-pt band. If it is shorter, the material
  stops part-way; D2 and R-c resolve it.
- §3.5 is silent on full screen, where the band overlays terminal cells (R-b).
- The Hint branch conflicts with "every Qt ≥ 6.9 API behind `QT_VERSION_CHECK`" unless macOS gets a Qt floor
  (owner, 1).
- The kit's "no material" for an inactive window is AppKit's own behaviour and has no automated test (R-g).
- The traffic lights' vertical place on the 38-pt band is not in §3.5; with D2's toolbar AppKit centres them.
- If D4 = `Solid` because AppKit draws no material under the primary mechanism on macOS 26, §3.5's primary
  mechanism does not hold there; the spike records it.
