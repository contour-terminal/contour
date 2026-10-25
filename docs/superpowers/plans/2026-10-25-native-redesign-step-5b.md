# Native Redesign — Step 5b (Linux: ContourLinux Style, Header Bar, Portal Backends) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelined). Line numbers are at HEAD `8354b21e`, before 1c (in progress), 1d, 2, 3a–3f, 4b
and 5a. Where 5b touches code those steps add, a task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before 5b executes. 5a and 5b
may run in parallel worktrees; see *Shared files*.

**Goal:** On Linux the window gets one design for every desktop: our own `ContourLinux` Quick Controls
style, a unified 46-px header bar (new tab, tabs inline, search, menu, then window buttons in the order and
on the side the desktop's button layout says), client-side decorations by default with `show_title_bar:
true` handing the frame to the window manager, and accent, light/dark, contrast and reduced motion read from
the XDG Settings portal.

**Architecture:**
- **Pure decisions first, Qt-free, table-tested on macOS.** `config/DesktopButtonLayout.hpp` parses
  GNOME's / GTK's `button-layout` string and KWin's `ButtonsOnLeft`/`ButtonsOnRight` letters, reads the
  KWin and GTK files through the injected `core::platform::FileSystem`, and resolves the first source that
  answers. `platform/PortalSettings.hpp` decodes portal values into `PlatformAppearance` and
  `AccessibilityPreferences`. `window/HeaderBarLayout.hpp` decides where the header items and separators go.
  `platform/WindowShadowGeometry.hpp` gains the window corner radius beside the shadow decision it shares
  inputs with.
- **The OS through one seam.** `platform::IPortalSettingsChannel` (Qt-free, with a null implementation)
  carries `ReadAll` replies and `SettingChanged` signals as plain values. The two portal sources
  (`PortalPlatformAppearance`, `PortalAccessibilityPreferenceSource`) are Qt-free classes over that seam and
  run on macOS against a fake channel. Only `DBusPortalSettingsChannel` speaks D-Bus; it is Linux-only and
  tested in CI against a fake portal service on a private session bus.
- **Two layout switches, as data.** The ContourLinux row takes 1c's `TabShape::Pill` with no well
  (`size.tabWell` 0) and a new `layout.headerBar` ∈ `HeaderBarArrangement { Band = 0, Unified }` (§1.2's
  "header-bar arrangement"). The Fluent, MacOS and Terminal rows keep `Band`.
- **Window controls follow the desktop.** `window_control_style: auto` resolves to a new `Linux` style on
  every non-macOS, non-Windows host (round 24-px buttons, themed icons). Its placement is the desktop's
  `WindowButtonLayout`, leading and trailing groups both allowed. The Terminal row keeps `● ● ●` cells,
  placed the same way.
- **No bus before the GUI.** `ContourGuiApp` builds its appearance sources on first use, so `contour daemon`
  and every headless verb never open the session bus, and the D-Bus channel never exists before the
  `QGuiApplication`.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls, QtDBus on Linux), QML, Catch2 v3, Python 3 (lint),
CMake build tree `out/appleclang-debug`, GitHub Actions (Ubuntu 24.04, headless weston, `dbus-run-session`).

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.15 (`:792-812`)
binds, with what it cites: §1 principles (`:28-64`), §2.1 token rows (`:68-126`; native rows keep today's
geometry "until Phase 5", `:105-110`), §2.3 accessibility (`:139-167`; Linux: `contrast` and
`reduced-motion`, `:158-161`), §2.4 platform appearance (`:169-185`; the Linux backend `:176-185`), §2.5 and
§2.6 snapshots and testing (`:187-226`), §2.7 existing users (`:228-284`). The design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md` (Decisions: "**one** Linux design that serves
every desktop. There is no GNOME-specific or KDE-specific variant"; Coverage additions, TUI row: "Linux CSD
and Windows draw "● ● ●" cells from the scheme").

The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>. Its local copies are in
`.superpowers/design/canvas/project/`: `linux` (the window), `linux-settings` (Step 4b owns the page; 5b
only the window and its header row), `kit-tokens` (the ContourLinux column), `tui` (the `● ● ●` cells). The
plans index is `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Binding rulings come
from `docs/superpowers/ledgers/*.md` and `.superpowers/sdd/*/progress.md`: Step 1c (the ContourLinux row
kept today's geometry until now), Step 2 D1 (pane cards on the ContourLinux row), Step 1b R-b (`platform/`
sits below `window/`) and R-i (in-scene menus show no chords).

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

- **This Mac cannot run a Linux desktop.** Linux-only code — the XDG portal over D-Bus, CSD versus SSD,
  KWin and Mutter behaviour — is compiled, clang-tidied and tested only by CI: the Ubuntu jobs of
  `.github/workflows/build.yml` (`ubuntu_2404_cc_matrix`, GCC 15 and Clang 22, its offscreen `tests` step,
  the headless-weston `display-gated GUI tests` and `chrome snapshots` steps at `:1568-1594`, and the new
  gating `portal settings adapter (private session bus)` step of Task 7; `check_clang_tidy` at `:91`).
  Therefore:
  - every Linux task keeps a cross-platform part that builds and runs here: pure decisions, token rows,
    null backends, Qt-free sources over a fake channel;
  - the portal/D-Bus adapter sits behind `platform::IPortalSettingsChannel`, with
    `NullPortalSettingsChannel` (AGENT.md dependency injection);
  - Linux-only translation units are wholly inside `#ifdef __linux__` and listed unconditionally in
    `add_library(contour_platform …)` (`platform/CMakeLists.txt:15-69`), as `PortalCall.cpp` is;
  - each Linux-only step carries a **Verified by** line naming the CI job, step and test filter. A task is
    not done until that CI run is green; push the branch and read the log.
- **The spec, verbatim (§3.15):** "One `ContourLinux` Quick Controls module, in
  `src/contour/styles/ContourLinux/`, built the way `ContourTui` is." "It is 46 high, with the tabs inline,
  a search button and a menu button. The window buttons follow the desktop's layout (§2.4). Linux uses
  client-side decorations by default. With `show_title_bar: true`, the server draws the frame. The tab row
  stays, and only the window buttons move out of it. There is no `window_decorations` key; it is
  `show_title_bar`". "Accent and light/dark come from the portal." Tests: "The button-layout parser, for
  both the GNOME string and the KWin keys. Parsing portal replies. The style module loads every control."
- **§2.4, verbatim:** "`ReadAll` plus `SettingChanged` on `org.freedesktop.appearance` (`accent-color`,
  `color-scheme`)". Button layout from the first that answers: "`org.gnome.desktop.wm.preferences
  button-layout`, where the portal serves it; KWin's `ButtonsOnLeft`/`ButtonsOnRight`; otherwise, a close
  button on the right". "The fallback accent colour is `#3584E4`." §2.3: "Linux: XDG
  `org.freedesktop.appearance` `contrast` and `reduced-motion`."
- **Canvas values, exact** (`linux`, `kit-tokens` ContourLinux column). Tasks 1 and 9 copy them.

  | Token | MacOS | Fluent | ContourLinux | Terminal (cells) |
  |---|---|---|---|---|
  | `size.titleBar` / `tab` / `tabWell` (exist) | 38 / 24 / 28 | 40 / 34 / 0 | 46 / 34 / 0 | 1 / 1 / 1 |
  | `size.control` / `toolbarButton` (exist) | 28 / 28 | 32 / 32 | 34 / 34 | 2 / 2 |
  | `size.findBar` (3b) | 32 | 32 | **34** (3b wrote 32) | 1 |
  | `size.windowButton` (new; round button) | 0 | 0 | 24 | 1 |
  | `size.windowButtonGlyph` (new) | 0 | 0 | 10 | 1 |
  | `size.headerIcon` (new; header-bar icon edge) | 16 | 16 | 16 | 1 |
  | `size.controlsSeparator` (new; its height) | 0 | 0 | 20 | 1 |
  | `radius.window` (new; CSD corner) | 0 | 8 | 12 | 0 |
  | `radius.windowButton` (new) | 0 | 0 | 12 | 0 |
  | `radius.menuRow` (new) | 5 | 4 | 6 | 0 |
  | `radius.tab` / `control` / `menu` / `popover` / `paneCard` (exist) | 12 / 6 / 10 / 14 / 10 | 6 / 4 / 8 / 8 / 4 | 8 / 8 / 10 / 12 / 6 | 0 |
  | `space.paneGap` / `paneInset` (exist) | 6 / 6 | 4 / 4 | 6 / 6 | 1 / 0 |
  | `type.caption` / `footnote` / `body` / `tab` / `title` (exist) | 10 / 11 / 13 / 12 / 15 | 12 / 12 / 14 / 12 / 20 | 12 / 12.5 / 14 / 13.5 / 15 | 0 |
  | `layout.tabShape` | `Pill` | `Flat` | **`Pill`** (was `Flat`) | `Flat` |
  | `layout.headerBar` (new) | `Band` | `Band` | `Unified` | `Band` |
  | `layout.profileMenu` | `SecondaryClick` | `Caret` | **`SecondaryClick`** (owner decision 6) | `Caret` |
  | colour `WindowButtonFill` (new role) | `fill(3)` | `fill(3)` | `fill(3)` | `schemeColor(Bar)` |
  | colour `TabActiveFill` | 1c's | 1c's | `fill(3)` (canvas: white 11 % on the bar) | 1c's |

  - Header-bar spacing uses the existing scale: bar inset `s4` (8, canvas `padding: 0 8px`), gap between
    items `s3` (6), gap between tabs `s2` (4), separator margins `s2` (4). The separator is
    `chromeStyle.borderWidth` wide and `size.controlsSeparator` tall, coloured at `contrast.separatorPercent`.
  - Tabs: 120–240 wide (the existing `minTabWidth`/`maxTabWidth`), 34 tall, radius 8; the active tab
    filled `TabActiveFill`, the others transparent with `TabText`; unseen output is 1c's badge dot.
  - Window buttons: 24 × 24 circles filled `WindowButtonFill`, a 10-px glyph icon.
  - Accessible names, from the canvas: "New tab", "Search", "Main menu", "Minimize", "Maximize", "Restore",
    "Close window".
  - Geometry: window 12 (CSD), pane cards 6 (12 − 6), gap 6, controls 8, menus 10. No blur by default:
    `background_blur` stays an opt-in (`BlurBehind.cpp`, KWin only), and `material.titleBand` stays `Solid`.
  - The pane card radius is 6, the canvas's and the kit's ("6 (12 − 6)"), already in `LinuxRow`
    (`ChromeTokenGroups.hpp:650-699`).
- **Themed icons with a bundled fallback.** Header-bar and window-button icons use Qt Quick Controls'
  `icon.name` (the freedesktop name, resolved through `QIcon::fromTheme`) with `icon.source` set to a
  bundled SVG, which Qt uses when the theme lacks the name. Names: `tab-new-symbolic`,
  `edit-find-symbolic`, `open-menu-symbolic`, `window-minimize-symbolic`, `window-maximize-symbolic`,
  `window-restore-symbolic`, `window-close-symbolic`. The bundled SVGs are 16 × 16, `stroke="currentColor"`,
  their paths copied from `linux.dc.html`, in `src/contour/res/icons/` and `resources.qrc` under
  `/contour/icons/`. QML tints them with `icon.color`.
- **Config keys.** There is no `window_decorations` key (§3.15; owner decision 1): `show_title_bar`
  (`Config.hpp:1188`, parsed at `Config.cpp:1734`, round-tripped by `Config_test.cpp:5070-5078` and
  `:5250-5262`) is the switch, its default (`false`, CSD) is unchanged, so no `DefaultsEdition` row changes.
  The one config-visible addition is the `window_control_style` enumerator `linux` (Task 4): parsed and
  written back through its `ConfigEnumInfo` row, documented in `ConfigDocumentation.hpp:305-315`, listed by
  the Settings page from `configEnumValues()`. Its default stays `auto`, so the edition gate is untouched.
- **AGENT.md applies in full.**
  - Data-driven: the GTK names, the KWin letters, the portal keys and their decoders, the header items, the
    header arrangement rules and the snapshot control styles are each a table. Adding a header item or a
    button name is adding a row.
  - Dependency injection: the portal through `IPortalSettingsChannel`; files through
    `core::platform::FileSystem`; the environment through `core::Environment`. Factories are the
    composition point: `NativeFileSystem::instance()` and `core::LiveEnvironment {}` appear only in the
    Linux factory (Task 6).
  - Configuration at construction: every new class takes its collaborators in the constructor.
    `WindowControlStyleProvider::update` is the documented live-reconfiguration carve-out (config reload,
    desktop change), as `setStyle` was.
  - `enum class` over `bool`, the default enumerator at zero. New QML-facing `Q_INVOKABLE` parameters and
    `QVariantMap` booleans are the documented carve-out.
  - `std::expected` for fallible calls: `readAll` replies with `std::expected<PortalSettings,
    PortalSettingsError>`. Parsers never fail (GTK ignores what it does not know) and return values.
  - Doxygen (`///`, `@param`, `@return`, `@pre`) on every new public declaration; `.hpp` with
    `#pragma once`. Zero warnings, no `NOLINT`. `slots`, `signals` and `emit` are Qt macros: never use them
    as identifiers.
- **Qt-free targets stay Qt-free:** `config/DesktopButtonLayout.hpp`, `platform/PortalSettings.hpp`,
  `platform/PortalSettingsChannel.hpp`, `platform/PortalAppearanceSources.hpp` and
  `window/HeaderBarLayout.hpp` include no Qt header.
- **Build** on every core with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in
  `out/appleclang-debug/CMakeCache.txt`): `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`.
  Never run two builds at once.
- **clang-tidy** every touched cross-platform `.cpp`:
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <files>`.
  Linux-only files are tidied by CI's `check_clang_tidy`.
- **Format** every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`. Never
  clang-format `.qml`. Check every new or touched `.qml`, `.py`, `.sh`, `.yml`, `.svg`, `CMakeLists.txt`
  with `editorconfig-checker <files>`.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`;** every new struct member gets `{}`
  or a value, and every new **row** of a table (`ChromeTokenRows`, `UiStyleTokenTable`,
  `WindowControlTokenTable`, `SnapshotScripts`, `HeaderBarRules`) names every field.
- **Counts in tests come from the tables** (`SnapshotScripts.size()`, `HeaderBarItems.size()`,
  `WindowControlCount`, `SnapshotControlsStyles.size()`), never literals.
- **QML:** `check_qml_sizes` and `check_qml_colors` pass with zero new literals (a new file's baseline is
  0, and `src/contour/styles/**` is scanned). A `Popup`/`Menu` carries no `Accessible` or `Keys` attached
  property: put them on an `Item`. QML compares `int`/string properties, never C++ enums through
  `chromeStyle`. Every new control sets `Accessible.role` and `Accessible.name`.
- **Every visual surface is a snapshot row, and every visual task renders the Terminal (TUI) row** (on
  Linux CSD: `● ● ●` cells from the scheme, placed by the desktop layout).
- **TestApp is Classic (R14)** and passes `nullAppearanceServices()`; no test reaches the session bus
  except under `contour::test::onPrivateSessionBus()` (`test_main.cpp:157-169` fails the run otherwise).
- **Tests:** each task runs only its own filter. The full suite runs in CI on Linux, macOS and Windows.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task; stage only that task's files (`git add <paths>`, never `-A`). Every message ends
  with exactly `Signed-off-by: Christian Parpart <christian@parpart.family>`, passed as a second `-m`; never
  `Co-Authored-By`.

## Review Focus

1. **Buttons on the left, macOS-style, from gtk-decoration-layout** (`close,minimize,maximize:`, served as
   GNOME's `button-layout` or found in GTK's `settings.ini`). Expected: close, minimize, maximize at the
   leading edge in that order, a separator after them, then the new-tab button; search and menu stay
   trailing with no separator and no trailing buttons; a right-to-left layout mirrors it. Pinned in Task 3
   (`buttonLayout.gtk` rows), Task 4 (the provider's `leadingButtons`), Task 8 (`headerBar.separators`),
   Task 9 (the QML x-order case) and, in CI, Task 7 (the fake portal serves it).
2. **No portal at all (a minimal WM).** Expected: startup never waits for D-Bus; the accent is `#3584E4`;
   the button layout is GTK's `settings.ini` if present, otherwise close on the right; accessibility is the
   default; light/dark is Qt's own; `contour daemon` never opens the bus. Pinned in Task 6 (null channel,
   the lazy sources, the daemon case) and, in CI, Task 7 (no portal on the private bus: `NoPortal`
   promptly).
3. **The accent or the theme changed live.** Expected: one change callback per real change, none for a
   repeated value; the chrome repaints with the new accent; a `color-scheme` flip moves the terminal's
   light/dark preference while `theme` defers to the system; an out-of-range accent falls back to
   `#3584E4`. Pinned in Task 5 (merge, decode), Task 6 (source and app) and, in CI, Task 7 (a real
   `SettingChanged`).
4. **KDE with server-side decorations** (`show_title_bar: true`). Expected: KWin draws the frame, its
   buttons and its shadow; our header bar keeps new tab, tabs, search and menu, with no window buttons, no
   separators, no corner radius and no CSD shadow; with one tab under `tab_bar: multiple` only KWin's frame
   remains, as today. `window_control_style: plasma` still means Breeze-like buttons under CSD. Pinned in
   Task 4, Task 8, Task 10 and Task 9.
5. **A tiling WM where no decorations are wanted.** Expected: `show_title_bar: true`, or a desktop layout
   of `:`, gives no window buttons, no separator, no radius and no shadow, and the tabs still show; a
   maximised, full-screen or tiled CSD window has square corners. Pinned in Task 3 (`:` → empty), Task 10
   (`Tiled` → 0) and Task 9 (no control loader active).

Also pinned: KWin's defaults (`MS` / `HIAX`) apply when `kwinrc` lacks the keys (Task 3); a reply arriving
after the source died runs nothing (Task 6, Task 7).

---

## Spec resolutions (read before Task 1)

Implementers follow these; those marked **(owner)** are repeated under *Owner decisions*.

- **R-a. No `window_decorations` key (owner).** The canvas writes `window_decorations: server`; §3.15 rules
  it is `show_title_bar`. 5b adds no key and documents `show_title_bar`'s Linux meaning.
- **R-b. A third button-layout source.** The canvas reads "the desktop's button layout
  (gtk-decoration-layout / KWin)". GNOME's portal `button-layout` is what GTK's `gtk-decoration-layout`
  follows; desktops without it (Xfce, tiling WMs) set the GTK key in `settings.ini`. So the chain is: (1)
  the portal's `org.gnome.desktop.wm.preferences button-layout`; (2) KWin's keys on a KDE session
  (`detectDesktopPlatform == KdePlasma`), from `<configHome>/kwinrc` `[org.kde.kdecoration2]`, KWin's
  defaults `MS`/`HIAX` for a missing key; (3) `gtk-decoration-layout` from `<configHome>/gtk-4.0/settings.ini`,
  then `gtk-3.0/settings.ini`, `[Settings]`; (4) close on the right (§2.4, owner decision 3). The files are
  read once, when the source is built; a KWin change applies on the next start.
- **R-c. One Linux control style (owner).** `window_control_style: auto` resolves to the new `Linux` style
  on `KdePlasma` and `Other` hosts (today: `Plasma` and `Windows`), per the design record's "no KDE-specific
  variant". An explicit `plasma`, `windows` or `macos` keeps its fixed side and order.
- **R-d. Pill without a well.** The header bar's inline tabs are 1c's `Pill` shape with `size.tabWell` 0:
  TabStrip draws no well when the token is 0. 1c's `TitleBandRules` `Pill` rows then decide the band (a
  title with one tab under `Multiple`; under a native frame the tab-bar gate decides, as today).
- **R-e. The `+` is a header slot.** Under `Pill`, 1c's trailing `+` becomes the header bar's `newTab`
  slot: `Band` puts it trailing, `Unified` leading. `Flat` keeps the `+` inside `TabStrip` (slot `Hidden`).
- **R-f. The main menu (owner).** The menu button opens the six menu-bar menus (`command::buildMenuBar`,
  1b) as one in-scene menu of six submenus, through 1b's in-scene path, with no chords (1b R-i).
- **R-g. Window corners (owner).** Under CSD, windowed, the header bar's two top corners take
  `radius.window` (12) and the window paints nothing outside them. The bottom corners stay square: rounding
  them would mask the terminal's RHI surface every frame. Maximised, full screen, tiled or SSD: 0.
- **R-h. Light/dark.** `PlatformAppearance::colorScheme` carries the portal's answer;
  `platform::osColorPreference` takes it over Qt's `styleHints()->colorScheme()` when it is set, so the
  terminal's light/dark preference follows the portal while `theme` defers to the system. The chrome's own
  light/dark still follows the terminal background by default (§2.2).
- **R-i. Built on first use.** `ContourGuiApp`'s constructor (`ContourGuiApp.cpp:451-456`) builds the
  appearance sources for every verb, before any `QCoreApplication`. A D-Bus source built there would open
  the session bus in `contour daemon` (which once cost the daemon its orderly shutdown, `PortalCall.cpp`)
  and before the `QGuiApplication`. So the app keeps the factories and builds each source the first time it
  is read. This is memoisation, not a second phase: the app is usable with zero calls.
- **R-j. Linux only.** `PortalCall`, `DBusSignalSubscription` and the new channel are `#ifdef __linux__`;
  the BSDs keep the null sources (and so the fallback layout).
- **R-k. The UI font** is the system font through `ChromeFontSource::PlatformUi` (Qt's platform theme);
  the portal is not asked for it.
- **R-l. The Settings window's header row (4a deferred it to 5b).** Under CSD on a `Unified` row the
  Settings window is frameless; 4b's page header row (`settings.headerBar`, 46 on ContourLinux) becomes its
  drag region and holds its window controls: Close alone (`SettingsWindowButtons::CloseOnly`), on the side
  the desktop layout puts close.

## Shared files (5a runs in parallel)

§3.14 (5a) names Mica (`Win32WindowFrame.cpp:133-140`, which 5b does not touch), Snap Layouts, 46-wide
caption buttons with Segoe glyphs, joined tabs, the Fluent row's `quickControlsStyle` `"FluentWinUI3"`, the
menu decision, and the Windows accessibility and appearance backends. 5b also edits these files that 5a's
section implies; whichever step lands second rebases onto the other and keeps both sets of rows:

- `src/contour/config/ChromeTokenGroups.hpp` and `ChromeTokenGroups_test.cpp` (5a: `FluentRow`; 5b: the new
  fields on every row, `LinuxRow`, `layout.headerBar`). 5b leaves `FluentRow`'s existing values alone.
- `src/contour/config/UiStyle.hpp`, `UiStyle_test.cpp` (5a: Fluent's `quickControlsStyle`; 5b: the
  ContourLinux row and `windowControlPresentation`).
- `src/contour/config/WindowControlStyle.{hpp,cpp}`, `WindowControlStyle_test.cpp`,
  `src/contour/window/WindowControlStyleProvider.{hpp,cpp}`, `_test.cpp`, `src/contour/qml/WindowControls.qml`
  (5a: caption buttons; 5b: `Linux`, `Round`, per-side lists).
- `src/contour/qml/TitleBar.qml`, `TabStrip.qml`, `TabItem.qml`, `src/contour/window/TitleBandLayout.hpp`
  (5a: joined tabs; 5b: header slots, no well).
- `src/contour/platform/PlatformAppearance.{hpp,cpp}`, `AccessibilityPreferenceSource.cpp`,
  `platform/CMakeLists.txt`, `AppearanceSources_test.cpp` (both narrow the null factories' `#ifndef
  __APPLE__`; the result is `#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__linux__)`).
- `src/contour/ContourGuiApp.{hpp,cpp}` (the sources' wiring).
- `src/contour/window/SettingsWindowHost.{hpp,cpp}`, `src/contour/qml/SettingsWindow.qml` (5a: Mica).
- `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotPlan_test.cpp`, `scripts/run-chrome-snapshots.sh`
  (each adds a control-style pass: 5a `FluentWinUI3`, 5b `ContourLinux`).
- `src/contour/test/MainWindowQml_test.cpp` (1c's "Fluent and ContourLinux rows keep today's geometry":
  5b drops ContourLinux from it, 5a drops Fluent).
- `src/contour/CMakeLists.txt` (test sources), `.github/workflows/build.yml`, `metainfo.xml`.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `config/ChromeTokenGroups.hpp` (modify) | `HeaderBarArrangement`, new size/radius fields, `WindowButtonFill`, `LinuxRow` | 1, 9 |
| `config/UiStyle.hpp` (modify) | `ContourLinuxTokens` (style `"ContourLinux"`), `windowControlPresentation` | 2, 4 |
| `styles/ContourLinux/*.qml` (create, 15 files) | The Quick Controls style | 2 |
| `test/ContourLinuxStyle_test.cpp` (create) | Packaging, every control loads, token metrics | 2 |
| `config/DesktopButtonLayout.{hpp,cpp}` (create) | `WindowButtonLayout`, GTK/KWin parsers, ini lookup, file sources, the chain, placement | 3, 4 |
| `config/WindowControlStyle.{hpp,cpp}` (modify) | `Linux` style, `Round` presentation, icon names, Auto → Linux | 4 |
| `window/WindowControlStyleProvider.{hpp,cpp}` (modify) | Per-side button lists, `update()` | 4 |
| `qml/WindowControls.qml`, `qml/TitleBar.qml` (modify) | The round delegate; one group per side | 4, 9 |
| `platform/PortalSettings.hpp` (create) | Portal values and their decoders | 5 |
| `platform/PlatformAppearance.hpp` (modify) | `colorScheme`, `buttonLayout` | 5 |
| `platform/PortalSettingsChannel.hpp` (create) | `IPortalSettingsChannel`, null, factory type | 6 |
| `platform/PortalAppearanceSources.{hpp,cpp}` (create) | The two Qt-free portal sources | 6 |
| `platform/PortalSourceFactories.cpp` (create, `#ifdef __linux__`) | The Linux factories | 6 |
| `platform/GuiTheme.hpp` (modify) | `osColorPreference` | 6 |
| `ContourGuiApp.{hpp,cpp}` (modify) | Lazy sources; colour scheme and layout follow | 6 |
| `platform/DBusPortalSettingsChannel.{hpp,cpp}` (create, `#ifdef __linux__`) | The D-Bus adapter | 7 |
| `test/FakeSettingsPortal.hpp` (create, `#ifdef __linux__`) | A Settings portal on the private bus | 7 |
| `window/HeaderBarLayout.hpp` (create) | Header items, arrangement rules, separators | 8 |
| `window/WindowController.{hpp,cpp}` (modify) | `activateHeaderBarItem`, the main menu, `windowCornerRadius` | 8, 10 |
| `window/UiStyleProvider.{hpp,cpp}`, `ChromeTokenObjects.{hpp,cpp}` (modify) | New tokens, `headerBar()`, `headerBarItems` | 1, 8 |
| `res/icons/*.svg`, `resources.qrc` (create/modify) | Bundled icon fallbacks | 9 |
| `platform/WindowShadowGeometry.hpp` (modify) | `windowCornerRadius` | 10 |
| `window/SettingsWindowRules.hpp`, `SettingsWindowHost.{hpp,cpp}`, `qml/SettingsWindow.qml` (modify) | The Settings window's header row | 11 |
| `test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp`, `scripts/run-chrome-snapshots.sh` (modify) | Linux snapshot rows and the third style pass | 2, 12 |
| `.github/workflows/build.yml` (modify) | The gating portal-adapter step | 7 |

---
## Phase A — tokens, the style module, the button layout (pure; all on macOS)

### Task 1: The ContourLinux token row

> **Depends on Steps 1c, 2, 3b, 3d, 4b:** they add `TabShape`, `TintTokens`, the band colour roles
> (1c), `type.paneHeader` (2), `size.findBar` (3b), `size.paletteRow` (3d) and the `settings.*` group
> (4b). Add this task's fields after theirs, by name. 3b wrote `size.findBar` 32 on ContourLinux; the kit
> says 34, and this task corrects it.

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp`: `HeaderBarArrangement` beside `TabShape` (`:50`);
  `SizeTokens` (`:197`), `RadiusTokens` (`:244`) and `LayoutTokens` (`:347`) gain fields; `ChromeColorRole`
  (`:71`) gains `WindowButtonFill` after `ChipTextStrong` (`ChromeColorRoleCount` is derived from the last
  enumerator, so it moves with it); `NativeColors` (`:495`) and the four rows (`:548-790`) name the new fields;
  `LinuxRow` takes `.colors = LinuxColors`.
- Modify: `src/contour/window/ChromeTokenObjects.{hpp,cpp}`: the new `size`, `radii` and `layout` properties
  (`layout.headerBar` as an `int`, as `tabShape` is).
- Test: `src/contour/config/ChromeTokenGroups_test.cpp` (in `contour_test`; its `numbersOf` lists every
  field), `src/contour/window/UiStyleProvider_test.cpp` (in `contour_gui_test`).

**Interfaces:**
- Produces (`contour::config`):
  - `enum class HeaderBarArrangement : uint8_t { Band = 0, Unified };` — `Band` is 1c's title band; `Unified`
    is the Linux header bar (new tab leading, search and menu trailing, window buttons beyond a separator).
  - `LayoutTokens::headerBar = HeaderBarArrangement::Band;`
  - `SizeTokens`: `int windowButton = 0; int windowButtonGlyph = 0; int headerIcon = 0; int controlsSeparator = 0;`
  - `RadiusTokens`: `int window = 0; int windowButton = 0; int menuRow = 0;`
  - `ChromeColorRole::WindowButtonFill` (the round window button's rest fill).
  - `detail::LinuxColors`: `NativeColors` with `TabActiveFill = fill(3)`.
- Values: the Global Constraints table, except `LinuxRow.layout` (`tabShape`, `headerBar`, `profileMenu`),
  which stays `{ Flat, Band, Caret }` until Task 9 flips it with the QML that reads it.

- [ ] **Step 1: Write the failing tests.**
  - `chromeTokens.linuxColumn` (`[chrometokens]`): `ChromeTokenRows[ContourLinux]` has `size.titleBar == 46`,
    `size.tab == 34`, `size.tabWell == 0`, `size.control == 34`, `size.toolbarButton == 34`,
    `size.findBar == 34`, `size.windowButton == 24`, `size.windowButtonGlyph == 10`, `size.headerIcon == 16`,
    `size.controlsSeparator == 20`, `radii.window == 12`, `radii.windowButton == 12`, `radii.menuRow == 6`,
    `radii.tab == 8`, `radii.control == 8`, `radii.menu == 10`, `radii.popover == 12`, `radii.paneCard == 6`,
    `space.paneGap == 6`, `space.paneInset == 6`, `type.footnote == 12.5`, `type.tab == 13.5`,
    `material.titleBand == Material::Solid` (no blur by default).
  - `chromeTokens.newFieldsPerRow`: the table's other three columns, row by row (`menuRow` 5/4/0,
    `window` 0/8/0, `headerIcon` 16/16/1, `windowButton` 0/0/1, `controlsSeparator` 0/0/1); every row's
    `layout.headerBar == Band` at this task.
  - `chromeTokens.windowButtonFill`: the native rows' `colors[WindowButtonFill] == detail::fill(3)`, the
    Terminal row's `== detail::schemeColor(SchemeSlot::Bar)`; `LinuxRow.colors[TabActiveFill] == fill(3)`.
  - `numbersOf` gains the new fields, so the existing "every native row is positive" cases cover them.
  - `UiStyleProvider_test.cpp`, `[uistyle]`: under ContourLinux, `size.windowButton == 24`,
    `radii.window == 12`, `radii.menuRow == 6`, `layout.headerBar == 0`.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`
  Expected: compile errors on the new fields.
- [ ] **Step 3: Implement** the enum, fields, role, rows and the provider's properties.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[chrometokens]" && out/appleclang-debug/src/contour/contour_gui_test "[uistyle],[contour][gui][qml][mainwindow][chromerow]"`
  Expected: PASS. `[chromerow]` proves Main.qml still loads under every row.
- [ ] **Step 5: Tidy, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/ChromeTokenObjects.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups*.{hpp,cpp} src/contour/window/ChromeTokenObjects.{hpp,cpp} src/contour/window/UiStyleProvider_test.cpp
git add src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider_test.cpp
git commit -m "contour: the ContourLinux token row from the kit and the Linux board" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The ContourLinux Quick Controls style

**Files:**
- Create: `src/contour/styles/ContourLinux/` with the 15 controls `ContourTui` has (`Button`, `CheckBox`,
  `ComboBox`, `ItemDelegate`, `Label`, `Menu`, `MenuItem`, `MenuSeparator`, `Popup`, `ScrollBar`,
  `ScrollView`, `Switch`, `TextField`, `ToolButton`, `ToolTip`), each a `QtQuick.Templates` control as
  `styles/ContourTui/ToolButton.qml` is.
- Modify: `src/contour/CMakeLists.txt`: a `contour_linux_style` module beside `contour_tui_style`
  (`:463-503`: same `QT_RESOURCE_ALIAS` loop, `URI ContourLinux`, `RESOURCE_PREFIX /qt/qml`,
  `OUTPUT_DIRECTORY …/ContourLinux`, `NO_GENERATE_QMLTYPES`, `NO_PLUGIN`), linked where `contour_tui_style`
  is (`:738`); `test/ContourLinuxStyle_test.cpp` in `contour_gui_test` (`:769-829`).
- Modify: `src/contour/config/UiStyle.hpp`: `detail::ContourLinuxTokens` = `NativeTokens` with
  `.quickControlsStyle = "ContourLinux"` (built by copy, as `PlasmaTokens` is); `UiStyleTokenTable`
  (`:454`) row `ChromeStyle::ContourLinux` uses it.
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`: `inline constexpr std::array SnapshotControlsStyles
  { "Fusion", "ContourTui", "ContourLinux" };` — the passes `run-chrome-snapshots.sh` makes. 
- Modify: `scripts/run-chrome-snapshots.sh` (`:35-39`): a third pass, `CONTOUR_TEST_CONTROLS_STYLE=ContourLinux`,
  whose status joins the other two; the header comment names three passes and `SnapshotControlsStyles`.
- Test: `src/contour/test/ContourLinuxStyle_test.cpp`; `src/contour/test/ChromeSnapshotPlan_test.cpp`
  (`:50-57`); `src/contour/config/UiStyle_test.cpp` (`:124`, `:216`).

**Interfaces:**
- Consumes: Task 1's `size.control`, `radii.control`, `radii.menu`, `radii.menuRow`, `radii.popover`, and
  the kit's colour roles and `elevation` through `chromeStyle`.
- Produces: the style `ContourLinux`, selected app-wide by the existing
  `QQuickStyle::setStyle(uiStyleTokens(_chromeStyle).quickControlsStyle)` (`ContourGuiApp.cpp:2331-2332`),
  with Fusion the fallback.
- Metrics, from the tokens: a `Button`/`ToolButton`/`ComboBox`/`TextField` is `size.control` (34) tall with
  `radii.control` (8); `Menu`'s background `radii.menu` (10) and elevation `e2`; a `MenuItem`'s highlight
  `radii.menuRow` (6) with `s1` inset; `Popup` `radii.popover` (12), `e3`; `ToolTip` `PlainText`, as
  ContourTui's is. Text in `chromeStyle.font` (the system UI font); colours from the control's palette and
  `chromeStyle.colors`. Both `T.Overlay.modal:` and `T.Overlay.modeless:` on `Popup` and `Menu`, and a
  `Menu.delegate` that builds a sub-menu row with an `arrow`.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][style][contourlinux]`), mirroring
  `ContourTuiStyle_test.cpp`:
  - `:/qt/qml/ContourLinux/qmldir` and `ToolButton.qml` exist;
  - every control in the list loads (`component.isReady()`) and instantiates, under
    `installChromeStyle(engine, UiStyle::Native)` resolved to `ChromeStyle::ContourLinux`;
  - `ToolButton`'s `implicitHeight == chromeStyle.size.control` (34) and its background's `radius ==
    chromeStyle.radii.control` (8); `Menu`'s background radius `== radii.menu`; a `MenuItem`'s highlight
    radius `== radii.menuRow`;
  - a `Menu` is at least as wide as its widest item, and its `delegate` builds a row with an `arrow`;
  - `Popup.qml` and `Menu.qml` declare both overlay scrims; a `ToolTip` shows `<img src=x>` as text.
  - `UiStyle_test.cpp`: `uiStyleTokens(ContourLinux).quickControlsStyle == "ContourLinux"`, MacOS and Fluent
    stay `"Fusion"`.
  - `ChromeSnapshotPlan_test.cpp`: `rowsRenderableUnder("Fusion") == { MacOS, Fluent }`,
    `rowsRenderableUnder("ContourLinux") == { ContourLinux }`, and `snapshot.everyRowHasAPass`: the union of
    `rowsRenderableUnder(style)` over `SnapshotControlsStyles` is every `ChromeStyle`, each exactly once.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`
  Expected: the style tests fail (no `qmldir`); `[snapshot]` fails on the Fusion row set.
- [ ] **Step 3: Implement** the module, the row, the table and the third pass.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[uistyle],[snapshot]" && out/appleclang-debug/src/contour/contour_gui_test "[contourlinux],[contour][gui][style]" && CONTOUR_TEST_CONTROLS_STYLE=ContourLinux out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow][chromerow]"`
  Expected: PASS, and the last run emits no QML warning (the run-wide gate).
- [ ] **Step 5: Lint, format and commit.**

```bash
ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)" --output-on-failure
editorconfig-checker src/contour/styles/ContourLinux/*.qml src/contour/CMakeLists.txt scripts/run-chrome-snapshots.sh
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ContourLinuxStyle_test.cpp src/contour/test/ChromeSnapshotPlan*.{hpp,cpp} src/contour/config/UiStyle*.{hpp,cpp}
git add src/contour/styles/ContourLinux src/contour/test/ContourLinuxStyle_test.cpp src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/config/UiStyle.hpp src/contour/config/UiStyle_test.cpp src/contour/CMakeLists.txt scripts/run-chrome-snapshots.sh
git commit -m "contour: the ContourLinux Qt Quick Controls style" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: The desktop's button layout, parsed and resolved

**Files:**
- Create: `src/contour/config/DesktopButtonLayout.{hpp,cpp}` (add to `contour_config`,
  `config/CMakeLists.txt:7-34`).
- Test: `src/contour/config/DesktopButtonLayout_test.cpp` (add to `contour_test`, `src/contour/CMakeLists.txt:1153-1206`).

**Interfaces:**
- Produces (namespace `contour::config`, Qt-free):
  - `struct WindowButtonLayout { std::vector<WindowControlKind> leading {}; std::vector<WindowControlKind> trailing {}; [[nodiscard]] bool operator==(WindowButtonLayout const&) const = default; };`
    Each side lists its buttons in drawing order, leading edge first. A kind appears at most once in the
    whole layout.
  - `struct KWinButtonKeys { std::optional<std::string> left {}; std::optional<std::string> right {}; [[nodiscard]] bool operator==(KWinButtonKeys const&) const = default; };`
  - `struct ButtonLayoutSources { std::optional<std::string> gnomeButtonLayout {}; std::optional<KWinButtonKeys> kwin {}; std::optional<std::string> gtkDecorationLayout {}; };`
    `kwin` is set only on a KDE session; an empty `KWinButtonKeys` there means "KWin's defaults".
  - `inline constexpr std::string_view KWinDefaultButtonsOnLeft = "MS";`, `KWinDefaultButtonsOnRight = "HIAX";`
  - Tables: `GtkButtonNames` — `{ "minimize", Minimize }`, `{ "maximize", Maximize }`, `{ "close", Close }`;
    `KWinButtonLetters` — `{ 'I', Minimize }`, `{ 'A', Maximize }`, `{ 'X', Close }`. Any other GTK name
    (`menu`, `appmenu`, `icon`, `spacer`, unknown) and any other KWin letter (`M N S H F B L _`) is skipped:
    the header bar has its own menu button.
  - `[[nodiscard]] WindowButtonLayout parseGtkButtonLayout(std::string_view layout);` — GTK's own rule: split
    at the first `:` (leading before, trailing after; no `:` means everything is leading), then at `,`;
    names are trimmed and case-sensitive; a kind already placed is skipped.
  - `[[nodiscard]] WindowButtonLayout parseKWinButtonLayout(KWinButtonKeys const& keys);` — `left` → leading,
    `right` → trailing, a missing key taking KWin's default.
  - `[[nodiscard]] std::optional<std::string_view> iniValue(std::string_view text, std::string_view group, std::string_view key);`
    — the value of `key=` in `[group]`; lines starting `#` or `;` are comments; a key's KDE `[$i]` suffix is
    ignored; whitespace around `=` and the value trimmed; CRLF tolerated.
  - `[[nodiscard]] KWinButtonKeys kwinButtonKeysOf(std::string_view kwinrc);` — group `org.kde.kdecoration2`,
    keys `ButtonsOnLeft`, `ButtonsOnRight`.
  - `struct DesktopConfigFiles { core::platform::FileSystem const& fileSystem; std::optional<std::filesystem::path> configHome; HostPlatform desktop = HostPlatform::Other; };`
  - `[[nodiscard]] ButtonLayoutSources fileButtonLayoutSources(DesktopConfigFiles const& files);` — R-b's
    sources 2 and 3: `kwin` from `<configHome>/kwinrc` on `KdePlasma` (`KWinButtonKeys {}` when the file is
    missing or unreadable), `gtkDecorationLayout` from `gtk-4.0/settings.ini`, else `gtk-3.0/settings.ini`,
    `[Settings] gtk-decoration-layout`. No `configHome` gives no file sources.
  - `[[nodiscard]] WindowButtonLayout fallbackButtonLayout();` → `{ .leading = {}, .trailing = { Close } }`.
  - `[[nodiscard]] WindowButtonLayout resolveDesktopButtonLayout(ButtonLayoutSources const& sources);` — the
    first of `gnomeButtonLayout`, `kwin`, `gtkDecorationLayout` that is set, else the fallback.

- [ ] **Step 1: Write the failing tests** (`[buttonlayout]`), as tables of `{ input, expected }`:
  - `buttonLayout.gtk`:
    - `"appmenu:close"` → `{ {}, {Close} }` (GNOME's default);
    - `"appmenu:minimize,maximize,close"` → `{ {}, {Minimize, Maximize, Close} }`;
    - `"close,minimize,maximize:"` → `{ {Close, Minimize, Maximize}, {} }` (Review Focus 1);
    - `"close:minimize,maximize"` → `{ {Close}, {Minimize, Maximize} }` (both sides);
    - `"minimize,close"` (no colon) → `{ {Minimize, Close}, {} }`;
    - `":"` and `""` → `{ {}, {} }` (Review Focus 5);
    - `"icon,menu:spacer,close,bogus,Close"` → `{ {}, {Close} }`;
    - `"close:close,minimize"` → `{ {Close}, {Minimize} }` (a kind once);
    - `" minimize , close :"` → `{ {Minimize, Close}, {} }`;
    - `"a:b:close"` → `{ {}, {} }` (GTK splits once; `b:close` is one unknown name).
  - `buttonLayout.kwin`: `{ "MS", "HIAX" }` → `{ {}, {Minimize, Maximize, Close} }`; `{ "XIA", "" }` →
    `{ {Close, Minimize, Maximize}, {} }`; `{}` (both missing) equals `{ "MS", "HIAX" }`'s result;
    `{ nullopt, "X" }` → `{ {}, {Close} }`.
  - `buttonLayout.ini`: `iniValue` finds `ButtonsOnLeft` in `[org.kde.kdecoration2]` and not in another
    group; skips `#`/`;` comments; reads `ButtonsOnRight[$i]=X` as `X`; trims; tolerates `\r\n`; misses give
    `nullopt`. `kwinButtonKeysOf` of a real Plasma 6 `kwinrc` excerpt gives both keys.
  - `buttonLayout.files` (an `InMemoryFileSystem`, `core/platform/testing/InMemoryFileSystem.hpp`):
    - `KdePlasma` with `/cfg/kwinrc` holding `ButtonsOnLeft=X` → `kwin == KWinButtonKeys { "X", nullopt }`;
    - `KdePlasma` with no `kwinrc` → `kwin == KWinButtonKeys {}`;
    - `Other` with a `kwinrc` → `kwin` unset;
    - `gtk-4.0/settings.ini` wins over `gtk-3.0/settings.ini`; only `gtk-3.0` → its value;
    - no `configHome` → nothing set.
  - `buttonLayout.chain`: GNOME set beats KWin and GTK; KWin beats GTK; GTK alone is used; nothing →
    `fallbackButtonLayout()` (Review Focus 2).
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`
  Expected: compile error, no `DesktopButtonLayout.hpp`.
- [ ] **Step 3: Implement** the header and `.cpp`.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[buttonlayout]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/config/DesktopButtonLayout.cpp src/contour/config/DesktopButtonLayout_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/DesktopButtonLayout*.{hpp,cpp}
editorconfig-checker src/contour/config/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/config/DesktopButtonLayout.hpp src/contour/config/DesktopButtonLayout.cpp src/contour/config/DesktopButtonLayout_test.cpp src/contour/config/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "config: read the desktop's window button layout from GNOME, KWin and GTK" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 4: Window controls follow the desktop's layout

> **Depends on Step 1c:** the Terminal row's `● ● ●` cells inked from the scheme (1c's TUI rule). This
> task only selects the `TrafficLight` presentation for the Terminal row and places it; the pre-flight
> scan confirms 1c inks the dots from the scheme, and reports it as a finding if not.

**Files:**
- Modify: `src/contour/config/WindowControlStyle.{hpp,cpp}`: `WindowControlStyle` (`:35`) gains `Linux`
  after `Plasma`; `WindowControlPresentation` (`:58`) gains `Round`; `WindowControlTokens` gains
  `iconNames` and `restoreIconName`; `detail::LinuxControlTokens`; `WindowControlTokenTable` (`:206`) and
  `WindowControlStyleTable` (`:209`) gain the row `{ Linux, "linux", "Linux" }`;
  `resolveWindowControlStyle` (`:264`): `KdePlasma` and `Other` → `Linux`.
- Modify: `src/contour/config/DesktopButtonLayout.{hpp,cpp}`: `windowButtonPlacement`.
- Modify: `src/contour/config/UiStyle.hpp`: `UiStyleTokens::windowControlPresentation`.
- Modify: `src/contour/config/ConfigDocumentation.hpp:305-315`: `auto` = macOS → macos, Windows →
  windows, anything else → linux (the desktop's own button layout); the new `linux` line; `plasma` stays.
- Modify: `src/contour/window/WindowControlStyleProvider.{hpp,cpp}`: per-side lists, `update()`.
- Modify: `src/contour/ContourGuiApp.cpp`: the provider's construction (`:2407-2408`) and
  `applyWindowControlStyle` (`:1760-1773`) pass the placement and the row's presentation.
- Modify: `src/contour/qml/WindowControls.qml`: `property string side` (required) picks
  `windowControls.leadingButtons` or `trailingButtons`; a `Round` delegate. `src/contour/qml/TitleBar.qml`
  (`:91-158`): each Loader is active when `root.useCustomWindowControls` and its side's list is non-empty,
  and passes `side`.
- Test: `src/contour/config/WindowControlStyle_test.cpp`, `src/contour/config/DesktopButtonLayout_test.cpp`
  (both `contour_test`), `src/contour/window/WindowControlStyleProvider_test.cpp`,
  `src/contour/test/QmlComponents_test.cpp` (both `contour_gui_test`), `src/contour/config/Config_test.cpp`.

**Interfaces:**
- Consumes: Task 3's `WindowButtonLayout`, `fallbackButtonLayout()`.
- Produces (`contour::config`):
  - `WindowControlPresentation::Round` — a circle of `size.windowButton` filled `WindowButtonFill`, with a
    themed icon of `size.windowButtonGlyph` (hover: `fill(2)`, pressed `fill(1)`).
  - `WindowControlTokens`: `std::array<std::string_view, WindowControlCount> iconNames {};` (indexed by
    `WindowControlKind`), `std::string_view restoreIconName {};` — empty on the glyph styles.
  - `detail::LinuxControlTokens`: `side = Trailing`, `presentation = Round`, `order = { Minimize, Maximize,
    Close }` (used only where no layout is given), `hoverCornerPercent = 100`, `closeHoverColor = 0`,
    `dotColors = {0,0,0}`, `inactiveDotColor = 0`, `glyphs = { "✕", "—", "▢" }`, `restoreGlyph = "❐"`,
    `iconNames = { "window-close-symbolic", "window-minimize-symbolic", "window-maximize-symbolic" }`,
    `restoreIconName = "window-restore-symbolic"`.
  - `UiStyleTokens::windowControlPresentation` — `std::optional<WindowControlPresentation>`: `TrafficLight`
    on `TerminalTokens`, `std::nullopt` on `NativeTokens` (and so on `ContourLinuxTokens`). A row that sets
    it draws every style's controls in that shape.
  - `[[nodiscard]] WindowButtonLayout windowButtonPlacement(WindowControlStyle resolved, std::optional<WindowButtonLayout> const& desktop);`
    — `Linux`: `desktop`, or `fallbackButtonLayout()` without one; any other style: its tokens' `order` on
    its `side`, the other side empty.
- Produces (`window::WindowControlStyleProvider`):
  - `WindowControlStyleProvider(config::WindowControlStyle resolved, config::WindowButtonLayout placement, std::optional<config::WindowControlPresentation> rowPresentation, QObject* parent = nullptr);`
    The row presentation is fixed per process (the style takes effect on the next start).
  - `void update(config::WindowControlStyle resolved, config::WindowButtonLayout placement);` replaces
    `setStyle`; emits `changed` only when either differs. Documented as live reconfiguration (a reload or
    the desktop's layout changing).
  - `Q_PROPERTY(QVariantList leadingButtons …)`, `Q_PROPERTY(QVariantList trailingButtons …)` replace
    `side` and `buttons`. Each row keeps today's keys (`action`, `glyph`, `restoreGlyph`, `hoverFill`, the
    dot colour) and adds `iconName`, `restoreIconName`. `presentation` gives `"round"` for `Round` and the
    row's override when set.

- [ ] **Step 1: Write the failing tests.**
  - `WindowControlStyle_test.cpp` (`[windowcontrols]`):
    - "Auto resolves against the host" (`:100`) changes: `MacOS → MacOS`, `Windows → Windows`,
      `KdePlasma → Linux`, `Other → Linux` (R-c);
    - "an explicit style survives every host" covers `Linux` and keeps `Plasma` on `KdePlasma`;
    - the Linux row has `Round`, its icon names and an empty `closeHoverColor`; "every style has a usable
      token row" covers it (the table is walked, not listed).
  - `DesktopButtonLayout_test.cpp`, `buttonLayout.placement`: `(Linux, {{Close,Minimize,Maximize},{}})` is
    that layout; `(Linux, nullopt) == fallbackButtonLayout()`; `(MacOS, any)` → `{ {Close, Minimize,
    Maximize}, {} }`; `(Windows, any)` and `(Plasma, any)` → `{ {}, {Minimize, Maximize, Close} }`.
  - `Config_test.cpp`: `window_control_style: linux` parses to `WindowControlStyle::Linux` and writes back as
    `linux`; the generated config's documentation names `linux`.
  - `WindowControlStyleProvider_test.cpp` (`[windowcontrols]`): `actionsOf(leadingButtons)` /
    `actionsOf(trailingButtons)` for the five placements above (Review Focus 1: the leading list is
    `close, minimize, maximize`); `update` with an equal pair emits no `changed`, a new layout emits one;
    the Linux rows carry `iconName == "window-close-symbolic"`; `rowPresentation = TrafficLight` makes
    `presentation == "trafficLight"` for the `Linux` style.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][windowcontrols]`: a `TitleBar` with a provider of
    `{ {Close}, {Minimize, Maximize} }` activates both Loaders (`leadingWindowControls`,
    `trailingWindowControls`) with one and two `windowControl_*` children; with `{ {}, {} }` neither is
    active and both are 0 wide (Review Focus 5); under the ContourLinux row a round control is
    `size.windowButton` square with `radius == radii.windowButton`; under the Terminal row the same layout
    draws `trafficLightGlyph` cells.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`
  Expected: compile errors (`WindowControlStyle::Linux`, the provider's constructor).
- [ ] **Step 3: Implement** the style row, placement, provider, wiring (the app passes
  `windowButtonPlacement(resolved, platformAppearance().buttonLayout)` — `buttonLayout` arrives in Task 5;
  until then pass `std::nullopt`), the QML and the documentation.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[windowcontrols],[buttonlayout]" && out/appleclang-debug/src/contour/contour_gui_test "[windowcontrols],[contour][gui][qml]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[windowcontrols]"`
  and `out/appleclang-debug/src/contour/contour_test "[config]" -c "window_control_style"` for the config rows.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)" --output-on-failure
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/config/WindowControlStyle.cpp src/contour/config/DesktopButtonLayout.cpp src/contour/window/WindowControlStyleProvider.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/WindowControlStyle*.{hpp,cpp} src/contour/config/DesktopButtonLayout*.{hpp,cpp} src/contour/config/UiStyle.hpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/window/WindowControlStyleProvider*.{hpp,cpp} src/contour/ContourGuiApp.cpp src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/WindowControls.qml src/contour/qml/TitleBar.qml
git add src/contour/config/WindowControlStyle.hpp src/contour/config/WindowControlStyle.cpp src/contour/config/WindowControlStyle_test.cpp src/contour/config/DesktopButtonLayout.hpp src/contour/config/DesktopButtonLayout.cpp src/contour/config/DesktopButtonLayout_test.cpp src/contour/config/UiStyle.hpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/window/WindowControlStyleProvider.hpp src/contour/window/WindowControlStyleProvider.cpp src/contour/window/WindowControlStyleProvider_test.cpp src/contour/ContourGuiApp.cpp src/contour/qml/WindowControls.qml src/contour/qml/TitleBar.qml src/contour/test/QmlComponents_test.cpp
git commit -m "contour: window buttons in the desktop's order, on either side; one Linux style" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

## Phase B — the XDG Settings portal

### Task 5: Portal values, decoded

**Files:**
- Create: `src/contour/platform/PortalSettings.hpp` (header-only, Qt-free; listed in `contour_platform`).
- Modify: `src/contour/platform/PlatformAppearance.hpp` (`:13-20`): two fields.
- Test: `src/contour/platform/PortalSettings_test.cpp` (in `contour_test`, beside
  `platform/FinishNotification_test.cpp`).

**Interfaces:**
- Produces (`contour::platform`):
  - `PlatformAppearance` gains `std::optional<vtbackend::ColorPreference> colorScheme {};` and
    `std::optional<config::WindowButtonLayout> buttonLayout {};` (includes `vtbackend/core/ColorPalette.hpp`
    and `config/DesktopButtonLayout.hpp`).
  - `struct PortalColor { double red = 0; double green = 0; double blue = 0; [[nodiscard]] bool operator==(PortalColor const&) const = default; };`
  - `using PortalSettingValue = std::variant<std::monostate, std::uint32_t, std::string, PortalColor>;`
  - `struct PortalSetting { std::string group {}; std::string key {}; PortalSettingValue value {}; [[nodiscard]] bool operator==(PortalSetting const&) const = default; };`
  - `using PortalSettings = std::vector<PortalSetting>;`
  - `enum class PortalSettingsError : uint8_t { NoPortal = 0, Failed };` — no Settings portal on the bus
    (unknown service, interface or method), or any other failure (a timeout).
  - Names: `AppearanceNamespace = "org.freedesktop.appearance"`, `GnomeWmNamespace =
    "org.gnome.desktop.wm.preferences"`; keys `"color-scheme"`, `"accent-color"`, `"contrast"`,
    `"reduced-motion"`, `"button-layout"`.
  - `inline constexpr vtbackend::RGBColor PortalFallbackAccent { 0x35, 0x84, 0xE4 };`
  - `[[nodiscard]] inline PortalSettings mergePortalSetting(PortalSettings settings, PortalSetting change);`
    — replaces the `(group, key)` entry or appends it.
  - `[[nodiscard]] inline std::optional<vtbackend::RGBColor> accentOf(PortalColor color);` — each channel in
    `[0, 1]` maps to `lround(channel × 255)`; any channel outside it (the portal's "unset") gives `nullopt`.
  - `[[nodiscard]] inline PlatformAppearance appearanceFromPortal(PortalSettings const& settings, config::ButtonLayoutSources fileSources);`
    — `accent`: the portal's, else `PortalFallbackAccent`; `colorScheme`: `1` Dark, `2` Light, anything else
    unset; `buttonLayout`: `resolveDesktopButtonLayout` with the portal's `button-layout` as
    `gnomeButtonLayout`; `windowCornerRadius` unset. A value of the wrong type counts as absent.
  - `[[nodiscard]] inline config::AccessibilityPreferences accessibilityFromPortal(PortalSettings const& settings);`
    — `contrast` `1` → `Increased`; `reduced-motion` `1` → `Reduced`; `transparency` stays `Allowed`.
  - The key → decoder mapping is one table per result (`ColorSchemeValues`, the accessibility rows), so a
    new portal key is a row.

- [ ] **Step 1: Write the failing tests** (`[portal]`):
  - `portal.accent`: `{0.2078, 0.5176, 0.8941}` → `#3584E4`; `{1,1,1}` → white; `{-1,-1,-1}` and `{1.5,0,0}`
    → `nullopt`.
  - `portal.appearance`: empty settings → `{ .accent = #3584E4, .colorScheme = nullopt, .buttonLayout =
    fallbackButtonLayout() }` (Review Focus 2); `color-scheme` `0`/`1`/`2`/`7` → `nullopt`/Dark/Light/`nullopt`;
    `color-scheme` as a string → `nullopt`; an out-of-range accent → `#3584E4` (Review Focus 3);
    `button-layout` `"close,minimize,maximize:"` → leading three (Review Focus 1); the same with
    `fileSources.kwin` set still prefers the portal; no portal key but `fileSources.gtkDecorationLayout` set
    → the GTK layout.
  - `portal.accessibility`: `contrast 1`, `reduced-motion 1` → Increased / Reduced; `0` → defaults; absent
    → defaults.
  - `portal.merge`: merging a new accent replaces the old one (size unchanged); a new key appends; a merge
    with an equal value gives an equal vector (the sources' "no callback" relies on it).
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`
  Expected: compile error, no `PortalSettings.hpp`.
- [ ] **Step 3: Implement** the header and the two fields.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[portal]" && cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[appearance-source]"`
  Expected: PASS; the Cocoa source (`CocoaPlatformAppearance.mm:61-67`) still compiles with the new fields
  defaulted.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/PortalSettings*.{hpp,cpp} src/contour/platform/PlatformAppearance.hpp
git add src/contour/platform/PortalSettings.hpp src/contour/platform/PortalSettings_test.cpp src/contour/platform/PlatformAppearance.hpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "platform: decode the Settings portal's appearance and accessibility values" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 6: The portal sources, built on first use

**Files:**
- Create: `src/contour/platform/PortalSettingsChannel.hpp` (Qt-free).
- Create: `src/contour/platform/PortalAppearanceSources.{hpp,cpp}` (Qt-free, compiled on every platform).
- Create: `src/contour/platform/PortalSourceFactories.cpp`, wholly inside `#ifdef __linux__`: the Linux
  `makePlatformAppearance` and `makeAccessibilityPreferenceSource`. The channel factory it names is Task 7's;
  in this task it passes `nullPortalSettingsChannels()`.
- Modify: `src/contour/platform/PlatformAppearance.cpp`, `AccessibilityPreferenceSource.cpp`: the null
  factories' guard becomes `#if !defined(__APPLE__) && !defined(__linux__)` (see *Shared files* for 5a's
  `_WIN32`). `platform/CMakeLists.txt` lists the new files.
- Modify: `src/contour/platform/GuiTheme.hpp`: `osColorPreference`.
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}`: the ctor (`:451-456`) keeps `appearanceServices`; the
  sources are built on first read; the `colorSchemeChanged` handler (`:2290-2308`) and the seed
  (`:2282-2284`) go through `osColorPreference`; `onPlatformAppearanceChanged` (`:1916-1919`) also updates
  the colour preference and the window-control placement.
- Test: `src/contour/platform/PortalAppearanceSources_test.cpp` (in `contour_gui_test`; it needs
  `contour_platform`), `src/contour/platform/GuiTheme_test.cpp`, `src/contour/test/ContourGuiApp_test.cpp`
  (`:405-431`, `[contour][app][appearance]`), `src/contour/test/AppearanceFixtures.hpp` (a counting
  variant of `appearanceServicesWith`).

**Interfaces:**
- Produces (`platform/PortalSettingsChannel.hpp`):
  - `using PortalSettingsReply = std::function<void(std::expected<PortalSettings, PortalSettingsError>)>;`
  - `using PortalSettingChanged = std::function<void(PortalSetting)>;`
  - `class IPortalSettingsChannel { public: virtual ~IPortalSettingsChannel() = default; virtual void readAll(PortalSettingsReply onReply) = 0; };`
    — asks for every setting of the namespaces the channel was built for and returns without waiting. The
    reply runs at most once, on the GUI thread, and never after the channel is destroyed. Changes arrive
    through the `PortalSettingChanged` given to its constructor.
  - `class NullPortalSettingsChannel final: public IPortalSettingsChannel` — replies
    `std::unexpected(PortalSettingsError::NoPortal)` at once.
  - `using PortalSettingsChannelFactory = std::function<std::unique_ptr<IPortalSettingsChannel>(std::vector<std::string> namespaces, PortalSettingChanged onChange)>;`
  - `[[nodiscard]] inline PortalSettingsChannelFactory nullPortalSettingsChannels();`
- Produces (`platform/PortalAppearanceSources.hpp`):
  - `inline constexpr std::array PortalAppearanceNamespaces { AppearanceNamespace, GnomeWmNamespace };`,
    `inline constexpr std::array PortalAccessibilityNamespaces { AppearanceNamespace };`
  - `class PortalPlatformAppearance final: public IPlatformAppearance` —
    `PortalPlatformAppearance(PortalSettingsChannelFactory const& channels, config::ButtonLayoutSources fileSources, PlatformAppearanceChanged onChange);`
    builds its channel and calls `readAll` in the constructor; `current()` is
    `appearanceFromPortal(_settings, _fileSources)`, so before any reply it is the fallback appearance. A
    reply replaces `_settings`, a change merges into it; either calls `onChange` once, and only when
    `current()` differs from what it was. The channel is the last member, so it dies first.
  - `class PortalAccessibilityPreferenceSource final: public IAccessibilityPreferenceSource` — the same over
    `accessibilityFromPortal`, with `PortalAccessibilityNamespaces`.
- Produces (`platform/GuiTheme.hpp`):
  `[[nodiscard]] inline vtbackend::ColorPreference osColorPreference(std::optional<vtbackend::ColorPreference> published, Qt::ColorScheme qtScheme) noexcept;`
  — `published` when set, else `Dark` for `Qt::ColorScheme::Dark` and `Light` otherwise (today's rule).
- Produces (`ContourGuiApp`, private): `AppearanceServices _appearanceServices;`,
  `mutable std::unique_ptr<…> _accessibilitySource, _platformAppearance;` and
  `[[nodiscard]] IAccessibilityPreferenceSource& accessibilitySource() const;`,
  `[[nodiscard]] IPlatformAppearance& platformAppearanceSource() const;`, which build on first call. The
  members' comment states R-i. `accessibilityPreferences()` and `platformAppearance()` go through them.
- The Linux factory: `PortalPlatformAppearance(<channels>, config::fileButtonLayoutSources({ NativeFileSystem::instance(), core::platform::configHome(core::LiveEnvironment {}), config::detectDesktopPlatform(core::LiveEnvironment {}) }), onChange)`.

- [ ] **Step 1: Write the failing tests.**
  - `PortalAppearanceSources_test.cpp` (`[portal-source]`), with a `FakePortalSettingsChannel` in the test
    (records the namespaces and the reply handler; `reply(…)` and `change(…)` deliver):
    - "the source asks for its namespaces once": one channel, namespaces
      `{ "org.freedesktop.appearance", "org.gnome.desktop.wm.preferences" }`, one `readAll`;
    - "before any reply it is the fallback" (accent `#3584E4`, close on the right) and calls nothing;
    - "no portal" (Review Focus 2): replying `NoPortal` leaves `current()` the fallback and calls nothing;
      with the null factory the same holds; with `fileSources.gtkDecorationLayout = "close,minimize,maximize:"`
      the layout is that one;
    - "a reply that changes the look calls once": a reply with an accent calls `onChange` once with it;
    - "live changes" (Review Focus 3): `change(accent-color = …)` calls once with the new accent; the same
      value again calls nothing; `change(color-scheme = 1)` → `colorScheme == Dark`; a change to an
      unrelated key calls nothing;
    - "a reply after the source died": destroy the source, then deliver; nothing runs (the fake checks the
      channel's destructor ran, and a dangling handler would trip ASan);
    - the accessibility source: `contrast 1` reply → `Increased`, one callback; `reduced-motion 1` change →
      `Reduced`.
  - `GuiTheme_test.cpp`, `[guitheme]`: `osColorPreference(Dark, Light) == Dark`, `(Light, Dark) == Light`,
    `(nullopt, Dark) == Dark`, `(nullopt, Unknown) == Light`.
  - `ContourGuiApp_test.cpp`, `[contour][app][appearance]`:
    - "no source is built until one is read" (Review Focus 2's daemon half): factories that count their
      calls; after constructing the `TestApp`, both counts are 0; `accessibilityPreferences()` makes the
      accessibility count 1, a second read keeps it 1; `platformAppearance()` likewise;
    - the existing case at `:405-431` passes unedited;
    - "the desktop's layout reaches the window controls": a switched appearance whose `buttonLayout` becomes
      `{ {Close}, {} }`; after `set`, the app's `WindowControlStyleProvider` (reached as the
      `windowControls` context property's object, or through a test accessor) has `leadingButtons` of one
      `close` row — guarded as `applyWindowControlStyle` is when the provider does not exist yet;
    - "the portal's light/dark moves the terminal's preference": `theme: system`; a switched appearance with
      `colorScheme = Dark` then `Light` moves `colorPreference()` to Dark then Light.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`
  Expected: compile errors on the new headers and accessors.
- [ ] **Step 3: Implement** the channel header, the two sources, the Linux factories (on the null channel
  factory for now), `osColorPreference`, and the app's lazy sources and handlers.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[portal-source],[guitheme],[contour][app][appearance],[appearance-source]"`
  Expected: PASS.
- **Verified by (Linux factories):** CI `ubuntu_2404_cc_matrix` › `tests` (offscreen `ctest`), which compiles
  `PortalSourceFactories.cpp` and runs `contour_gui_test` with `nullAppearanceServices()` everywhere, and
  `check_clang_tidy`.
- [ ] **Step 5: Tidy, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/platform/PortalAppearanceSources.cpp src/contour/platform/PortalAppearanceSources_test.cpp src/contour/ContourGuiApp.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/PortalSettingsChannel.hpp src/contour/platform/PortalAppearanceSources*.{hpp,cpp} src/contour/platform/PortalSourceFactories.cpp src/contour/platform/PlatformAppearance.cpp src/contour/platform/AccessibilityPreferenceSource.cpp src/contour/platform/GuiTheme*.{hpp,cpp} src/contour/ContourGuiApp.{hpp,cpp} src/contour/test/ContourGuiApp_test.cpp src/contour/test/AppearanceFixtures.hpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform/PortalSettingsChannel.hpp src/contour/platform/PortalAppearanceSources.hpp src/contour/platform/PortalAppearanceSources.cpp src/contour/platform/PortalAppearanceSources_test.cpp src/contour/platform/PortalSourceFactories.cpp src/contour/platform/PlatformAppearance.cpp src/contour/platform/AccessibilityPreferenceSource.cpp src/contour/platform/GuiTheme.hpp src/contour/platform/GuiTheme_test.cpp src/contour/platform/CMakeLists.txt src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp src/contour/test/ContourGuiApp_test.cpp src/contour/test/AppearanceFixtures.hpp src/contour/CMakeLists.txt
git commit -m "contour: portal-backed appearance and accessibility sources, built on first use" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The D-Bus channel (Linux only, CI-verified)

**Files:**
- Create: `src/contour/platform/DBusPortalSettingsChannel.{hpp,cpp}`, wholly inside `#ifdef __linux__`,
  listed unconditionally in `contour_platform` (AUTOMOC must see the `Q_OBJECT`).
- Modify: `src/contour/platform/PortalSourceFactories.cpp`: pass `dbusPortalSettingsChannels()`.
- Create: `src/contour/test/FakeSettingsPortal.hpp` (`#ifdef __linux__`, `Q_OBJECT`, listed in
  `contour_gui_test`): a `QDBusAbstractAdaptor` for `org.freedesktop.portal.Settings` with `ReadAll(as)` →
  `a{sa{sv}}` from a test-supplied table and a `SettingChanged(s, s, v)` emitter, registered as
  `org.freedesktop.portal.Desktop` at `/org/freedesktop/portal/desktop`.
- Test: `src/contour/platform/DBusPortalSettingsChannel_test.cpp` (`#ifdef __linux__`, `contour_gui_test`).
- Modify: `.github/workflows/build.yml`: a gating step in `ubuntu_2404_cc_matrix` after "tests" (`:1497`).

**Interfaces:**
- Consumes: `PortalService`, `PortalPath`, `DBusCallTimeoutMilliseconds` (`PortalCall.hpp`),
  `DBusSignalSubscriber`, `sessionBusSubscriber()`, `DBusSignalSubscriptionGuard` (`DBusSignalSubscription.hpp`).
- Produces (`contour::platform`, Linux):
  - `class DBusPortalSettingsChannel final: public QObject, public IPortalSettingsChannel` —
    `DBusPortalSettingsChannel(std::vector<std::string> namespaces, PortalSettingChanged onChange, DBusSignalSubscriber subscriber = sessionBusSubscriber());`
    subscribes to `SettingChanged` in the constructor (guard member); `readAll` sends `ReadAll(namespaces)`
    with `asyncCall(…, DBusCallTimeoutMilliseconds)` on `QDBusConnection::sessionBus()` resolved per call,
    its watcher parented to the channel. Errors `ServiceUnknown`, `UnknownInterface`, `UnknownMethod`,
    `UnknownObject` → `NoPortal`; any other → `Failed`; each logged once with `portalLog()`. A change for a
    namespace the channel did not ask for is dropped.
    `@pre` a `QCoreApplication` exists (R-i guarantees it).
  - private slot `void onSettingChanged(QString const& group, QString const& key, QDBusVariant const& value);`
    — declares every parameter the signal carries (`DBusSignalSubscription.hpp`'s note).
  - `[[nodiscard]] PortalSettingValue portalSettingValueOf(QVariant const& value);` — unwraps nested
    `QDBusVariant`s (old portals double-wrap); `uint` and non-negative `int` → `std::uint32_t`; `QString` →
    `std::string`; a `QDBusArgument` structure of three doubles `(ddd)` → `PortalColor`; anything else →
    `std::monostate`.
  - `[[nodiscard]] PortalSettingsChannelFactory dbusPortalSettingsChannels();`
  - `QMap<QString, QVariantMap>` registered with `qDBusRegisterMetaType` once, in the constructor.

- [ ] **Step 1: Write the failing tests** (`[portal-settings]`, each case `SKIP`s unless
  `contour::test::onPrivateSessionBus()`):
  - "ReadAll decodes a real reply": the fake serves `color-scheme` `uint 1`, `accent-color` `(0.2078,
    0.5176, 0.8941)`, `contrast` `uint 1`, `button-layout` `"close,minimize,maximize:"`; the reply (pumped
    with `QTRY_VERIFY`-style `processEvents` until it lands, 5 s cap) holds those four settings as
    `uint32_t`, `PortalColor`, `uint32_t`, `std::string`; fed to `appearanceFromPortal` it gives Dark,
    `#3584E4`, the leading three (Review Focus 1).
  - "a double-wrapped value": `v(v(u 2))` decodes to `2`.
  - "SettingChanged arrives" (Review Focus 3): the fake emits `(org.freedesktop.appearance, accent-color,
    (1, 0, 0))`; `onChange` receives one `PortalSetting` with `PortalColor{1,0,0}`; a change for
    `org.example.other` is dropped.
  - "no portal on the bus" (Review Focus 2): no fake registered; `readAll` replies `NoPortal`, and the
    channel's construction plus `readAll` pass `checkReturnsWithoutWaiting` (`NotificationFixtures.hpp`).
  - "a reply after the channel died": destroy the channel with a `ReadAll` outstanding against a fake that
    delays; pump events; the handler never ran.
  - "the real factory end to end": `PortalPlatformAppearance(dbusPortalSettingsChannels(), {}, …)` against
    the fake reports the fake's accent after one callback.
- [ ] **Step 2: Add the CI step.** In `ubuntu_2404_cc_matrix`, after "tests":

```yaml
      - name: "portal settings adapter (private session bus)"
        # Gating, unlike the weston steps: it needs a bus, not a compositor. A SKIP is a failure here,
        # as for the tmux oracles: it would mean the adapter never met a portal.
        run: |
          set -o pipefail
          sudo apt-get install -qy dbus-daemon
          output=$(QT_QPA_PLATFORM=offscreen ./scripts/run-display-tests.sh --run \
              out/gcc-debug/src/contour/contour_gui_test "[portal-settings]" 2>&1) || { printf '%s\n' "$output"; exit 1; }
          printf '%s\n' "$output"
          if printf '%s' "$output" | grep -q 'skipped'; then
            echo "::error::[portal-settings] reported SKIPPED; the D-Bus portal adapter is not being exercised"
            exit 1
          fi
```

  `run-display-tests.sh` uses a preset `QT_QPA_PLATFORM` verbatim and always runs under its private bus
  with `CONTOUR_TEST_PRIVATE_SESSION_BUS` set.
- [ ] **Step 3: Implement** the channel and the fake; switch the factory.
- [ ] **Step 4: Verify locally what this Mac can:**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[portal-source]" && ctest --test-dir out/appleclang-debug -R check_workflows --output-on-failure && editorconfig-checker .github/workflows/build.yml`
  Expected: PASS (the new files compile to nothing here; the workflow lint passes).
- **Verified by:** CI `ubuntu_2404_cc_matrix` (GCC 15 and Clang 22) › "portal settings adapter (private
  session bus)", filter `[portal-settings]`, no skips; › "tests" (offscreen `ctest`, the cases skip there);
  `check_clang_tidy` for the two new `.cpp`s. Push and read the logs before calling the task done.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/DBusPortalSettingsChannel*.{hpp,cpp} src/contour/platform/PortalSourceFactories.cpp src/contour/test/FakeSettingsPortal.hpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt .github/workflows/build.yml
git add src/contour/platform/DBusPortalSettingsChannel.hpp src/contour/platform/DBusPortalSettingsChannel.cpp src/contour/platform/DBusPortalSettingsChannel_test.cpp src/contour/platform/PortalSourceFactories.cpp src/contour/test/FakeSettingsPortal.hpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt .github/workflows/build.yml
git commit -m "platform: read the XDG Settings portal over D-Bus, tested against a fake portal in CI" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
## Phase C — the header bar, CSD/SSD, the Settings header, snapshots

### Task 8: The header bar's arrangement and the main menu

> **Depends on Steps 1b and 1c:** 1b's `buildMenuBar` and the in-scene path `openNewTabMenu` follows
> (`WindowController.hpp:395-400`); 1c's `WindowFrame`, `TabShape`, and `TitleBar`'s trailing `+` under
> `Pill`.

**Files:**
- Create: `src/contour/window/HeaderBarLayout.hpp` (Qt-free).
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}`: `headerBar()`, `headerBarItems`.
- Modify: `src/contour/window/WindowController.{hpp,cpp}`: `activateHeaderBarItem`, the main menu.
- Test: `src/contour/window/HeaderBarLayout_test.cpp` (in `contour_test`), `src/contour/window/UiStyleProvider_test.cpp`,
  `src/contour/window/WindowMenus_test.cpp` (in the heavy sources, as 1b placed it).

**Interfaces:**
- Produces (`contour::window`, `HeaderBarLayout.hpp`):
  - `enum class HeaderBarItem : uint8_t { NewTab = 0, Search, MainMenu };`
  - `struct HeaderBarItemRow { HeaderBarItem item = HeaderBarItem::NewTab; std::string_view objectName {}; std::string_view accessibleName {}; std::string_view iconName {}; std::string_view bundledIcon {}; };`
  - `inline constexpr std::array HeaderBarItems` in enumerator order:
    `{ NewTab, "headerNewTab", "New tab", "tab-new-symbolic", "qrc:/contour/icons/tab-new-symbolic.svg" }`,
    `{ Search, "headerSearch", "Search", "edit-find-symbolic", "qrc:/contour/icons/edit-find-symbolic.svg" }`,
    `{ MainMenu, "headerMainMenu", "Main menu", "open-menu-symbolic", "qrc:/contour/icons/open-menu-symbolic.svg" }`;
    a `static_assert` pins the order.
  - `enum class HeaderBarSlot : uint8_t { Hidden = 0, Leading, Trailing };`
  - `enum class ControlsPresence : uint8_t { None = 0, Present };`
  - `enum class ControlsSeparator : uint8_t { None = 0, Shown };`
  - `struct HeaderBarRule { config::HeaderBarArrangement arrangement; std::optional<config::TabShape> shape; HeaderBarSlot newTab; HeaderBarSlot search; HeaderBarSlot mainMenu; };`
  - `inline constexpr std::array HeaderBarRules` (R-e): `{ Band, Flat, Hidden, Hidden, Hidden }`,
    `{ Band, Pill, Trailing, Hidden, Hidden }`, `{ Unified, nullopt, Leading, Trailing, Trailing }`; the
    first matching row wins; no match gives all `Hidden`.
  - `struct HeaderBarInputs { config::HeaderBarArrangement arrangement = config::HeaderBarArrangement::Band; config::TabShape shape = config::TabShape::Flat; WindowFrame frame = WindowFrame::ClientBand; ControlsPresence leading = ControlsPresence::None; ControlsPresence trailing = ControlsPresence::None; };`
  - `struct HeaderBar { HeaderBarSlot newTab = HeaderBarSlot::Hidden; HeaderBarSlot search = HeaderBarSlot::Hidden; HeaderBarSlot mainMenu = HeaderBarSlot::Hidden; ControlsSeparator leadingSeparator = ControlsSeparator::None; ControlsSeparator trailingSeparator = ControlsSeparator::None; [[nodiscard]] bool operator==(HeaderBar const&) const = default; };`
  - `[[nodiscard]] constexpr HeaderBar resolveHeaderBar(HeaderBarInputs const& inputs) noexcept;` — the
    rule's slots; a side's separator is `Shown` when the arrangement is `Unified`, the frame is
    `ClientBand`, that side has controls, and some item's slot is on that side.
  - `[[nodiscard]] constexpr std::string_view headerBarSlotName(HeaderBarSlot)` → `"hidden"`, `"leading"`,
    `"trailing"`.
- Produces (`UiStyleProvider`):
  - `Q_INVOKABLE [[nodiscard]] QVariantMap headerBar(bool nativeFrame, int leadingControls, int trailingControls) const;`
    → keys `newTab`, `search`, `mainMenu` (slot names), `leadingSeparator`, `trailingSeparator` (booleans).
    The `bool` and the counts are the QML-facing carve-out, documented at the declaration.
  - `Q_PROPERTY(QVariantList headerBarItems READ headerBarItems CONSTANT)` — one map per row
    (`item` as int, `objectName`, `accessibleName`, `iconName`, `bundledIcon`).
- Produces (`WindowController`):
  - `Q_INVOKABLE void activateHeaderBarItem(int item, QPointF windowPosition);` — `NewTab` → `createNewTab()`;
    `Search` → `runCommand("SearchReverse")` (the action that opens the find bar; 3b keeps the name);
    `MainMenu` → `openMainMenu(windowPosition)`; an out-of-range `item` does nothing. One `switch` over the
    enum, no `default`.
  - `void openMainMenu(QPointF windowPosition);` and `Q_INVOKABLE void triggerMainMenuAction(int actionId);`
    — `buildMenuBar(menuBarState())` (six submenus, in `MenuBarMenu` order), presented in-scene the way
    `openNewTabMenu` is, without chords (1b R-i).

- [ ] **Step 1: Write the failing tests.**
  - `headerBar.rules` (`[headerbar]`), a table of `{ inputs, expected }`:
    - `{ Unified, Pill, ClientBand, None, Present }` → `{ Leading, Trailing, Trailing, None, Shown }` (the canvas);
    - `{ Unified, Pill, ClientBand, Present, None }` → `{ Leading, Trailing, Trailing, Shown, None }` (Review Focus 1);
    - `{ Unified, Pill, ClientBand, Present, Present }` → both separators `Shown`;
    - `{ Unified, Pill, NativeFrame, None, None }` → `{ Leading, Trailing, Trailing, None, None }` (Review Focus 4);
    - `{ Unified, Pill, ClientBand, None, None }` → no separators (Review Focus 5, layout `:`);
    - `{ Band, Pill, ClientBand, Present, None }` → `{ Trailing, Hidden, Hidden, None, None }` (macOS, unchanged);
    - `{ Band, Flat, ClientBand, None, Present }` → all `Hidden`, no separators (Fluent and Terminal, unchanged).
  - `headerBar.items`: `HeaderBarItems.size() == static_cast<size_t>(HeaderBarItem::MainMenu) + 1`; names
    and icons are non-empty and distinct; `headerBarSlotName` covers every enumerator.
  - `UiStyleProvider_test.cpp`, `[uistyle]`: `headerBar(false, 0, 3)` on a row with `Unified` gives
    `{ newTab: "leading", search: "trailing", mainMenu: "trailing", leadingSeparator: false,
    trailingSeparator: true }`; `headerBarItems` has `HeaderBarItems.size()` rows.
  - `WindowMenus_test.cpp`, `[windowmenus][mainmenu]`: `openMainMenu` presents (through the null presenter,
    in-scene) a menu of `MenuBarMenuCount` submenu rows titled as `buildMenuBar`'s; triggering the Shell
    menu's "New Tab" row adds a tab; `activateHeaderBarItem(Search, {})` runs `SearchReverse` on the active
    session; `activateHeaderBarItem(NewTab, {})` adds a tab; `activateHeaderBarItem(99, {})` changes nothing.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`
  Expected: compile error, no `HeaderBarLayout.hpp`.
- [ ] **Step 3: Implement** the header, the provider's two members and the controller's three.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[headerbar]" && out/appleclang-debug/src/contour/contour_gui_test "[uistyle],[windowmenus]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/HeaderBarLayout_test.cpp src/contour/window/UiStyleProvider.cpp src/contour/window/WindowController.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/HeaderBarLayout*.{hpp,cpp} src/contour/window/UiStyleProvider*.{hpp,cpp} src/contour/window/WindowController.{hpp,cpp} src/contour/window/WindowMenus_test.cpp
git add src/contour/window/HeaderBarLayout.hpp src/contour/window/HeaderBarLayout_test.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/WindowMenus_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: decide the header bar's slots from a table; a main menu of the six menus" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: The header bar in QML; the ContourLinux row takes it

> **Depends on Step 1c:** `TitleBar.qml`'s band (`centre`, `presence`, `tabWellLayout`), `TabStrip.qml`'s
> `Pill` well and `TabItem.qml`'s pill, and its acceptance case "the Fluent and ContourLinux rows keep
> today's geometry (offscreen)" in `MainWindowQml_test.cpp`. Locate them by `objectName`.

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp`: `LinuxRow.layout = { …, .tabShape = TabShape::Pill,
  .headerBar = HeaderBarArrangement::Unified, .profileMenu = ProfileMenuAffordance::SecondaryClick }`.
- Modify: `src/contour/qml/TitleBar.qml`: `readonly property var header: chromeStyle.headerBar(!useCustomWindowControls, windowControls.leadingButtons.length, windowControls.trailingButtons.length)`;
  the RowLayout becomes, leading to trailing: leading controls Loader · leading separator · leading items ·
  centre (tabs or title) · drag region · 1c's chips · trailing items · trailing separator · trailing controls
  Loader. Items are a `Repeater` over `chromeStyle.headerBarItems` filtered by slot into a `ToolButton`
  (`toolbarButton` square, `radii.control`, `icon.name`/`icon.source`/`icon.color`, `size.headerIcon`,
  `Accessible.role: Accessible.Button`, `Accessible.name`, `objectName`), whose `onClicked` calls
  `controller.activateHeaderBarItem(modelData.item, mapToItem(null, 0, height))`. Margins `s4`, spacing
  `s3`. 1c's trailing `+` under `Pill` is removed (it is the `newTab` slot now).
- Modify: `src/contour/qml/TabStrip.qml`: no well background, padding 0 and gap `s2` when
  `chromeStyle.size.tabWell === 0`.
- Create: `src/contour/res/icons/{tab-new,edit-find,open-menu,window-minimize,window-maximize,window-restore,window-close}-symbolic.svg`;
  modify `src/contour/resources.qrc` (prefix `/contour/icons`).
- Modify: `scripts/qml-size-literal-baseline.txt` only downward (no new literal).
- Test: `src/contour/test/MainWindowQml_test.cpp` (new `[contour][gui][qml][mainwindow][headerbar]` cases;
  1c's acceptance case keeps Fluent only), `src/contour/test/Accessibility_qml_test.cpp`,
  `src/contour/config/ChromeTokenGroups_test.cpp` (the Linux layout row).

**Interfaces:**
- Consumes: Task 1's tokens, Task 4's per-side lists and round delegate, Task 8's `headerBar()`,
  `headerBarItems`, `activateHeaderBarItem`.

- [ ] **Step 1: Write the failing tests** (offscreen, the ContourLinux row, a mock provider of
  `{ {}, {Minimize, Maximize, Close} }` unless stated):
  - "the header bar is the canvas's": `titleBar.height == 46` (`size.titleBar`); the active tab item is
    34 tall with `radius == 8`; the well item is absent or 0 wide; items in x order: `headerNewTab` < the
    first tab < `headerSearch` < `headerMainMenu` < the trailing separator < `windowControl_minimize` <
    `windowControl_maximize` < `windowControl_close`; the separator is `size.controlsSeparator` tall; the
    bar's left margin is `s4`.
  - "buttons on the left" (Review Focus 1): provider `{ {Close, Minimize, Maximize}, {} }`: x order
    `windowControl_close` < `…minimize` < `…maximize` < leading separator < `headerNewTab`; no trailing
    separator; under `LayoutMirroring.enabled` the order reverses.
  - "server-side decorations" (Review Focus 4): `titleBarVisible = true`: no `windowControl_*` and no
    separator visible; `headerNewTab`, the tabs, `headerSearch`, `headerMainMenu` visible; with one tab under
    `Multiple` the bar is hidden (1c's rule).
  - "no buttons wanted" (Review Focus 5): provider `{ {}, {} }`: no controls, no separators; tabs visible.
  - "clicking the items": clicking `headerSearch` calls `activateHeaderBarItem(1, …)` on the mock;
    `headerMainMenu` calls it with `2`.
  - "the other rows are unchanged": under MacOS, Fluent and Terminal, `headerSearch` and `headerMainMenu`
    are absent and 1c's existing band assertions pass unedited; the Terminal row with `{ {Close, Minimize,
    Maximize}, {} }` draws three `●` cells at the leading edge.
  - `Accessibility_qml_test.cpp`: the accessible walk under ContourLinux finds Buttons named "New tab",
    "Search", "Main menu", "Minimize", "Maximize", "Close window".
  - `ChromeTokenGroups_test.cpp`: `LinuxRow.layout` is `{ Pill, Unified, SecondaryClick }`.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[headerbar]"`
  Expected: FAIL (no `headerSearch`).
- [ ] **Step 3: Implement** the row flip, the QML, the icons and the resource entries.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[chrometokens]" && out/appleclang-debug/src/contour/contour_gui_test "[headerbar],[a11y],[contour][gui][qml]" && CONTOUR_TEST_CONTROLS_STYLE=ContourLinux out/appleclang-debug/src/contour/contour_gui_test "[headerbar]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[headerbar]"`
  Expected: PASS, no QML warnings.
- [ ] **Step 5: Lint and commit.**

```bash
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
editorconfig-checker src/contour/qml/TitleBar.qml src/contour/qml/TabStrip.qml src/contour/res/icons/*.svg src/contour/resources.qrc
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups*.{hpp,cpp} src/contour/test/MainWindowQml_test.cpp src/contour/test/Accessibility_qml_test.cpp
git add src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/qml/TitleBar.qml src/contour/qml/TabStrip.qml src/contour/res/icons src/contour/resources.qrc scripts/qml-size-literal-baseline.txt src/contour/test/MainWindowQml_test.cpp src/contour/test/Accessibility_qml_test.cpp
git commit -m "contour: the Linux header bar: new tab, inline tabs, search, menu, the desktop's buttons" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 10: Client-side by default, server-side on request; the window's corners

**Files:**
- Modify: `src/contour/platform/WindowShadowGeometry.hpp`: `windowCornerRadius` beside
  `shadowVisibilityFor` (`:285-310`), which shares its inputs.
- Modify: `src/contour/window/WindowController.{hpp,cpp}`: `Q_PROPERTY(int windowCornerRadius READ windowCornerRadius NOTIFY windowCornerRadiusChanged)`,
  recomputed where the controller refreshes its shadow (the `WindowShadowController::refresh` call site:
  presentation, decoration).
- Modify: `src/contour/qml/TitleBar.qml` (its background `Rectangle`: `topLeftRadius`/`topRightRadius` =
  `controller.windowCornerRadius`) and `src/contour/qml/Main.qml` (the window's own background, if it paints
  one, takes the same top radii, so nothing is painted outside the corners; R-g).
- Modify: `src/contour/config/ConfigDocumentation.hpp:112-118` (`ShowTitleBarConfig`): "On Linux: false
  (the default) = Contour's own header bar with the desktop's window buttons; true = the window manager
  draws the frame (KWin, Mutter, or a tiling WM's border or none); the header bar keeps the tabs, new tab,
  search and menu; only the window buttons move out of it."
- Test: `src/contour/platform/WindowShadowGeometry_test.cpp` (`contour_gui_test`), `src/contour/test/MainWindowQml_test.cpp`,
  `src/contour/platform/NativeWindowFrame_test.cpp` (pins the Linux rows unchanged).

**Interfaces:**
- Produces: `[[nodiscard]] constexpr int windowCornerRadius(int rowRadius, WindowPresentation presentation, WindowDecoration decoration) noexcept;`
  — `rowRadius` (`radii.window`) only for `Windowed` and `Client`; 0 otherwise.

- [ ] **Step 1: Write the failing tests.**
  - `windowCorner.table` (`[windowshadow]`): for `rowRadius` 12, every `WindowPresentation` × `WindowDecoration`:
    `(Windowed, Client)` → 12; `Maximized`, `FullScreen`, `Tiled` (Review Focus 5) and every `Server` row
    (Review Focus 4) → 0; `rowRadius` 0 → 0 everywhere.
  - `NativeWindowFrame_test.cpp`: `framePolicyFor(Other, Client)` keeps `FramelessHint::Apply` and
    `FrameAffordances::Client`; `framePolicyFor(Other, Server)` is `ServerFramePolicy` (CSD default, SSD on
    request: existing behaviour, pinned).
  - `MainWindowQml_test.cpp`, `[headerbar][corners]`: ContourLinux row, windowed, CSD → the title bar
    background's `topLeftRadius == 12`; after `controller` reports maximised → 0; with `titleBarVisible =
    true` → 0.
  - `Config_test.cpp`: the generated documentation for `show_title_bar` contains "the window manager draws
    the frame".
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test contour_test`
  Expected: compile error on `windowCornerRadius`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[windowshadow],[nativeframe],[headerbar]" && out/appleclang-debug/src/contour/contour_test "[config]" -c "show_title_bar"`
  Expected: PASS.
- **Verified by (the real frame):** CI `ubuntu_2404_cc_matrix` › "display-gated GUI tests (headless
  weston)", filter `[display]` (a frameless CSD window under a real Wayland compositor), and the owner checks
  of Task 13 on GNOME and KDE (no compositor in CI honours SSD the way KWin does).
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/WindowShadowGeometry*.{hpp,cpp} src/contour/platform/NativeWindowFrame_test.cpp src/contour/window/WindowController.{hpp,cpp} src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/test/MainWindowQml_test.cpp
editorconfig-checker src/contour/qml/TitleBar.qml src/contour/qml/Main.qml
git add src/contour/platform/WindowShadowGeometry.hpp src/contour/platform/WindowShadowGeometry_test.cpp src/contour/platform/NativeWindowFrame_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/qml/TitleBar.qml src/contour/qml/Main.qml src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/test/MainWindowQml_test.cpp
git commit -m "contour: rounded CSD corners only while windowed; document show_title_bar on Linux" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: The Settings window's header row on Linux

> **Depends on Step 4b:** the page header row (`settings.headerBar`, 46 on ContourLinux), the sidebar's
> "Settings" title and "Search settings" field. 5b only makes the row the window's drag region and places
> its window controls (R-l).

**Files:**
- Modify: `src/contour/window/SettingsWindowRules.hpp`: two pure functions.
- Modify: `src/contour/window/SettingsWindowHost.{hpp,cpp}`: `windowFlags()` (`.cpp:38`) frameless under
  `WindowDecoration::Client` on a `Unified` row; `Q_PROPERTY(QVariantList leadingControls …)`,
  `trailingControls`, from the app's placement; the host applies `NativeWindowFrame::apply` with the
  decoration, as the main window does.
- Modify: `src/contour/qml/SettingsWindow.qml`: when `host.frameless`, a `ResizeBorder`, the header row's
  `DragHandler` (`startSystemMove`, double-click maximises) and a `WindowControls` per non-empty side, at
  the row's ends.
- Test: `src/contour/window/SettingsWindowRules_test.cpp`, `src/contour/window/SettingsWindowHost_test.cpp`,
  `src/contour/test/SettingsPageQml_test.cpp`.

**Interfaces:**
- Produces (`contour::window`):
  - `[[nodiscard]] config::WindowButtonLayout settingsWindowControls(config::WindowButtonLayout const& placement, SettingsWindowButtons buttons);`
    — `All`: unchanged; `CloseOnly`: only `Close`, on the side the placement has it, else nothing.
  - `[[nodiscard]] constexpr platform::WindowDecoration settingsWindowDecoration(config::HeaderBarArrangement arrangement, platform::WindowDecoration mainWindow) noexcept;`
    — `Client` only for `Unified` with a `Client` main window (the default profile's `show_title_bar:
    false`); `Server` otherwise, so every other row keeps 4a's OS frame.

- [ ] **Step 1: Write the failing tests** (`[settingswindow]`):
  - `settingsWindowControls`: `({ {}, {Min, Max, Close} }, CloseOnly)` → `{ {}, {Close} }`;
    `({ {Close, Min, Max}, {} }, CloseOnly)` → `{ {Close}, {} }` (Review Focus 1); `({ {}, {} }, CloseOnly)`
    → empty (Review Focus 5); `All` passes through.
  - `settingsWindowDecoration`: `(Unified, Client)` → `Client`; `(Unified, Server)` → `Server` (Review
    Focus 4); `(Band, Client)` → `Server`.
  - `SettingsWindowHost_test.cpp`: on a ContourLinux `TestApp` with `show_title_bar: false`, `windowFlags()`
    has `Qt::FramelessWindowHint` and `trailingControls` is one `close` row; with `show_title_bar: true` no
    frameless hint and no controls.
  - `SettingsPageQml_test.cpp`: the frameless Settings window under ContourLinux has a `windowControl_close`
    inside the 46-px header row and none under MacOS.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[settingswindow]"`
  Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[settingswindow],[settings]"`
  Expected: PASS.
- **Verified by (a frameless window on Linux):** CI `ubuntu_2404_cc_matrix` › "display-gated GUI tests
  (headless weston)", `[display]`; the snapshot of Task 12's `settings` row under ContourLinux.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/SettingsWindowRules*.{hpp,cpp} src/contour/window/SettingsWindowHost*.{hpp,cpp} src/contour/test/SettingsPageQml_test.cpp
editorconfig-checker src/contour/qml/SettingsWindow.qml
git add src/contour/window/SettingsWindowRules.hpp src/contour/window/SettingsWindowRules_test.cpp src/contour/window/SettingsWindowHost.hpp src/contour/window/SettingsWindowHost.cpp src/contour/window/SettingsWindowHost_test.cpp src/contour/qml/SettingsWindow.qml src/contour/test/SettingsPageQml_test.cpp
git commit -m "contour: the Linux Settings window draws its own header row, with the desktop's close button" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: Snapshot rows for the Linux header bar

> **Depends on Step 1c:** `SnapshotFrame` (`ClientBand`, `NativeFrame`) and the row fields 1c names.

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`: `SnapshotScript` gains `ButtonsLeading`, `NoButtons`
  and `MainMenu` (SSD needs no new script: 1c's `native-frame` row covers it on every row);
  `enum class SnapshotButtons : uint8_t { Desktop = 0, Leading, None };` and the row field
  `SnapshotButtons buttons = SnapshotButtons::Desktop;` (every existing row names it); `ScriptAction`
  gains `OpenMainMenu`.
- Modify: `src/contour/test/ChromeSnapshotScene.hpp` and `MockMainController.hpp`: the scene feeds
  `buttons` into the `WindowControlStyleProvider` (`Desktop` → `fallbackButtonLayout()`, `Leading` →
  `parseGtkButtonLayout("close,minimize,maximize:")`, `None` → `{}`); `OpenMainMenu` calls the controller.
- Modify: `src/contour/test/ChromeSnapshot_test.cpp` only if the scene's API changed.
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`.

**Interfaces:**
- New rows: `{ ButtonsLeading, "buttons-leading", tabCount 3, buttons Leading }`, `{ NoButtons,
  "no-buttons", tabCount 3, buttons None }`, `{ MainMenu, "main-menu", tabCount 2, action OpenMainMenu }`
  They render on every row: on MacOS, Fluent and Terminal they
  show those rows' own answers (the Terminal row's `● ● ●` cells move to the leading edge under
  `buttons-leading`).

- [ ] **Step 1: Write the failing tests** (`[snapshot]`): `scriptRow` of each new script has its name and
  fields; names stay unique; `snapshotMatrix(rowsRenderableUnder("ContourLinux")).size() ==
  SnapshotScripts.size() × SnapshotEditions.size() × SnapshotAppearances.size()`; the existing "rows are in
  enumerator order" case covers the new rows.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test && out/appleclang-debug/src/contour/contour_test "[snapshot]"`
  Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass; render here.**
  Run: `out/appleclang-debug/src/contour/contour_test "[snapshot]"`, then

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
for style in Fusion ContourTui ContourLinux; do
  CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=$style out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
done
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count is `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, read from the
  table. Look at `buttons-leading.contourlinux.*`, `no-buttons.contourlinux.*`, `main-menu.contourlinux.*`,
  `three-tabs.contourlinux.*`, `native-frame.contourlinux.*`, `settings.contourlinux.*` against
  `linux.dc.html`, and `buttons-leading.terminal.*` against `tui.dc.html`.
- **Verified by:** CI `ubuntu_2404_cc_matrix` (GCC 15, ubuntu-24.04) › "chrome snapshots (headless weston)",
  `[display][snapshot]` in three passes, artefact `chrome-snapshots` (review only, non-gating).
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshot*.{hpp,cpp} src/contour/test/MockMainController.hpp
git add src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/MockMainController.hpp src/contour/test/ChromeSnapshot_test.cpp
git commit -m "contour: snapshot rows for the Linux header bar: buttons leading, none, the main menu" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts .github`)
  and apply its findings; re-run the filters of every task it touched.
- [ ] **Step 2: Every filter on macOS.**

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[uistyle],[snapshot],[buttonlayout],[windowcontrols],[portal],[headerbar],[config]"
out/appleclang-debug/src/contour/contour_gui_test "[uistyle],[contourlinux],[contour][gui][style],[windowcontrols],[appearance-source],[portal-source],[guitheme],[contour][app][appearance],[windowmenus],[headerbar],[a11y],[windowshadow],[nativeframe],[settingswindow],[settings],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourLinux out/appleclang-debug/src/contour/contour_gui_test "[contourlinux],[headerbar],[contour][gui][qml][mainwindow]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[windowcontrols],[headerbar],[contour][gui][style]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots incl. ContourTui and ContourLinux** on this Mac's display (Task 12, Step 4's loop),
  then push and confirm CI's "chrome snapshots (headless weston)" artefact holds the same count.
- [ ] **Step 4: CI's Linux runs.** Push; on `ubuntu_2404_cc_matrix` (both compilers) confirm green: "tests",
  "portal settings adapter (private session bus)" (no skips), "display-gated GUI tests (headless weston)"
  (non-gating: read its log for the `[portal-settings]`, CSD and Settings cases anyway), "chrome snapshots";
  and `check_clang_tidy`. Fix with `/fix-ci` if not.
- [ ] **Step 5: Owner checks, batched per phase, need a Linux desktop or VM** (record results in the
  ledger; none of these can run on this Mac):
  - Phase A: GNOME 47+ (Wayland) with `gsettings set org.gnome.desktop.wm.preferences button-layout
    'close,minimize,maximize:'` — buttons leading in that order; KDE Plasma 6 with KWin's buttons rearranged
    in System Settings — Contour follows after a restart; `window_control_style: plasma` keeps Breeze-like
    buttons.
  - Phase B: on GNOME, toggle dark style and change the accent in Settings — the chrome repaints live with
    the new accent, the terminal's light/dark follows under `theme: system`; with High Contrast and Reduce
    Animations on, the chrome follows; on a minimal WM with no `xdg-desktop-portal` (Openbox or i3) —
    startup is immediate, the accent is `#3584E4`, close on the right; `contour daemon` starts and stops
    cleanly (no bus).
  - Phase C: the header bar matches `linux.dc.html` on GNOME and KDE (46 high, inline tabs, search opens the
    find bar, menu opens the six menus); KDE with `show_title_bar: true` — KWin's frame and buttons, our tab
    row stays without buttons; sway or i3 with `show_title_bar: true` — no buttons, no rounded corners;
    maximise and tile on GNOME — corners square; the Settings window has its own header row with one close
    button on GNOME; `ui_style: terminal` on GNOME draws `● ● ●` cells where the desktop puts its buttons.
- [ ] **Step 6: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release
  (`:107`): the Linux header bar; window buttons in the desktop's order and on its side (GNOME, KDE, GTK);
  accent, light/dark, contrast and reduced motion from the XDG Settings portal; the ContourLinux control
  style; `window_control_style: linux` (now what `auto` means on Linux).
- [ ] **Step 7:** Mark Step 5b done in the plans index and on the board with `/status-board update`, and
  update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the render path; two portal `ReadAll`s at GUI start, asynchronous; the files
    read once.
  - **Risk:** medium. KDE users on `auto` see the new buttons (R-c); the appearance sources now build on
  first use (R-i); the D-Bus adapter is verified only in CI and by the owner checks.
  - **Coverage:** the new Catch2 tables, the private-bus adapter cases, the QML header-bar cases and three
    snapshot scripts per row.
- [ ] **Step 8:** Run `/code-review` on the whole step and address every finding.

---

## Owner decisions

The run proceeds on each recommended default.

1. **No `window_decorations` key (R-a).** The canvas writes `window_decorations: server`; §3.15 says the
   switch is `show_title_bar`. Default: no new key; `show_title_bar` documented for Linux.
2. **Rounded corners on top only (R-g).** Rounding the bottom two corners means masking the terminal's RHI
   surface every frame. Default: top two rounded by the header bar, bottom square; revisit after measuring.
3. **The fallback layout.** §2.4 says close on the right alone, which takes minimize and maximize away from
   a minimal-WM user who has them today. Default: the spec (close only); the alternative is `minimize,
   maximize, close`, a one-line change to `fallbackButtonLayout()`.
4. **KDE `auto` users get the Linux buttons (R-c).** Default: yes, per "no KDE-specific variant";
   `window_control_style: plasma` keeps the old look.
5. **The main menu holds the six menu-bar menus (R-f).** Default: yes, reusing `buildMenuBar`; the
   alternative is a short GNOME-style primary menu, which would be a new table.
6. **No `▾` beside `+` on Linux.** The kit records `Caret`, the Linux board shows none. Default: the board,
   `SecondaryClick`.
