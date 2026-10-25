# Native Redesign — Step 4a (Settings Window Shell) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Settings becomes one window per app. `SettingsWindow.qml` hosts today's `SettingsPage.qml`.
`OpenConfiguration` and Contour › Settings… show, raise and focus that window whatever state the terminal
windows are in. The settings pseudo-tab, `settingsActive` and the new-tab menu's Settings row are removed.

**Architecture:**
- **One controller, one window, one host.** The `SettingsController` moves from each `WindowController` to
  `ContourGuiApp`. A new `window::SettingsWindowHost` borrows it and owns the one `QQuickWindow`. The window
  is built on first show through an injected `SettingsWindowFactory`: on the app's shared engine in
  production, and on a test engine in `TestApp`. Closing hides the window and never destroys it, so a second
  request finds the same instance with its pane and draft intact.
- **Size and conventions are data.** The window's default and minimum extents are four new rows of
  `config::SizeTokens`, counted in cells on the Terminal row. Title and window buttons come from a
  per-`HostPlatform` table. Where the window opens is a pure function over plain `QRect`s, built on
  `reachableWindowPosition`.
- **Routing has one sink.** Every route to `OpenConfiguration {}` ends in `ContourGuiApp::openSettings(QScreen*)`:
  the session handler, the menu bar's Application rows, and the no-window path. A minimised or absent
  terminal window no longer matters, because Settings no longer lives in one.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), QML, Catch2 v3, Python 3 (lint), CMake build tree
`out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.3 binds. It cites
§1 (principles), §2.1 (token rows), §2.5 (snapshot driver) and §2.6 (testing). The visual design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md` ("Settings is a separate window (⌘, / Ctrl+,)").
The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `mac-settings`,
`win-settings`, `linux-settings`, `tui` and `surfaces` are in `.superpowers/design/canvas/project/`. The
plans index is `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. The carry-ins come from
`docs/superpowers/ledgers/2026-10-25-native-redesign-step-1b.md`.

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

- **The spec, verbatim (§3.3):**
  - "A new `SettingsWindow.qml` hosts today's `SettingsPage.qml` unchanged. It is a separate `QQuickWindow`,
    one per app, loaded on the shared engine."
  - "`OpenConfiguration` (⌘, / Ctrl+,) and Contour › Settings… show the window and focus it."
  - "Removed: the pseudo-tab …; `settingsActive` and its `Main.qml` loader …; `NewTabMenu`'s Settings row."
  - "Tests: Opening the window twice yields one instance. The tab model no longer has a settings row.
    `SettingsPageQml_test` still passes inside the window."
- **The canvas, verbatim (mac-settings):** "Settings, its own window. Opened by ⌘, from the Contour menu — not
  a tab. One instance; reopens the last section; minimize and zoom dimmed, as the HIG asks." The window
  extents come from the boards: macOS 960 × 820, Windows 1110 × 920 titled "Contour Settings" with
  Minimize/Maximize/Close, and Linux 1100 × 920 with a close button only.
- **The plans index, verbatim:** the `settings` row of the snapshot table (`SnapshotScripts`) "changes in the
  same commit" that removes the pseudo-tab (Task 4).
- **AGENT.md applies in full:**
  - **Data-driven.** Sizes are token rows. Title and buttons are a table indexed by `HostPlatform`. The
    snapshot change is a column of `SnapshotScripts`. A new host platform is one row.
  - **Dependency injection.** The window is built only through `window::SettingsWindowFactory`. The
    controller's collaborators stay injected as today (`SettingsController.hpp:122-126`).
  - **Configuration at construction.** `SettingsWindowHost` takes its controller, its conventions and its
    factory in the constructor, and has no setters. Building its window on first `show()` is realisation,
    not configuration. `ContourGuiApp` creates the controller and the host on first use, as it does
    `forcedFontDpiProvider()` (`ContourGuiApp.hpp:298-301`), because both need a `QGuiApplication`; document
    this at the declaration.
  - **`enum class` over `bool`,** with the default enumerator at zero (`SettingsWindowButtons`,
    `ConfigurationTarget`). This step touches `OpenConfiguration`'s handler, so its `bool inEditor` becomes
    an enum in the same change (§1.7). The YAML key `in_editor: true` is unchanged at the boundary.
  - **`std::expected`** for fallible calls (`SettingsWindowError`, starting at `= 0`).
  - **Doxygen** (`///`, `@param`, `@return`, `@pre`) on every new public declaration; `.hpp` with
    `#pragma once`.
  - **Zero warnings** under `-Werror`, and **no `NOLINT`**. `slots`, `signals` and `emit` are Qt macros:
    never use them as identifiers.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already set,
  `out/appleclang-debug/CMakeCache.txt:392`): `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`.
  Never run two builds at once.
- **Tests:** each task runs only its own filter. The full suite runs in CI on Linux, macOS and Windows.
- **Format** every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i` before each
  commit (CI's version). Check every new or touched `.qml`, `.py`, `.txt` and `CMakeLists.txt` with
  `editorconfig-checker <files>`. **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`,** and clang-tidy enforces
  `cppcoreguidelines-pro-type-member-init`. Every new struct member gets `{}` or a value. Every row of a
  table (`ChromeTokenRows`' `.size`, `SettingsWindowConventionRows`, `SnapshotScripts`) names every field.
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Counts in tests are derived from the tables** (`SnapshotScripts.size()`,
  `SettingsWindowConventionRows.size()`, `MockProfileNames.size()`), never written as literals.
- **QML:**
  - The size and colour lint gates pass with zero new literals (`check_qml_sizes`, `check_qml_colors`). A
    new file's baseline is 0. A file below its baseline fails too, so Task 4 lowers `TabStrip.qml`'s from 3
    to 2.
  - Every extent comes from `chromeStyle`. `SettingsWindow.qml` binds no `width`/`height` (R-c).
- **Never edit `vendor/core-cpp`.**
- **TestApp is Classic** and passes `nullAppearanceServices()`. From Task 3 it also passes a factory that
  builds Settings windows offscreen on an engine of its own.
- **Every visual task renders the Terminal (TUI) row** (canvas tui: "Settings window is the same separate
  window, built from ContourTui controls"). Tasks 1 and 2 test the Terminal row's cell sizing and run the
  host cases under `CONTOUR_TEST_CONTROLS_STYLE=ContourTui`. Task 4's `settings` snapshot renders on the
  Terminal row in the ContourTui pass.
- **Qt-free targets stay Qt-free:** `config/ChromeTokenGroups.hpp` gains plain `int`s only.
- **Line numbers** below are at HEAD `79061d14`. A later task locates code an earlier task moved by its name.
- **Commits:** one per task. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>` and carries **no** `Co-Authored-By` trailer
  (owner rule; it supersedes the 1b plan's trailer line).
- After the code changes, run `/simplify` (Task 5).

## Review Focus

1. **Settings is asked for again while open.** This covers ⌘, or Settings… with Settings already open behind
   a terminal window, minimised (Windows), or hidden after a close. Expected: the same one window, restored
   from minimised, raised and focused, where it was. Pinned in Task 2 ("opens once", "comes back in front")
   and Task 3 (twice through a session gives one window).
2. **Settings with no focused terminal window, or none at all.** This covers after ⌘M, with another app in
   front, or after the last terminal window closed while the app lives on. Expected: Settings opens and takes
   focus. The minimised terminal window stays minimised: nothing raises it, which fixes the 1b ledger's
   carry-in. Pinned in Task 3 (the minimised-window case, the no-focus case, and the no-window case through
   `runWithoutWindow`).
3. **Closing Settings while a profile edit is unsaved.** Profiles are drafts until Save today (§3.13; 4b
   makes them live). Expected: nothing is lost. Reopening shows the same pane, still editing the same draft
   with its edited value, and nothing prompts. Pinned in Task 2 ("closing keeps the window, its pane and its
   draft").
4. **Size and position.**
   - First open: centred on the screen of the window that asked, and fitted to a screen smaller than the
     default size (Windows' 920 is taller than a 768-high laptop's work area).
   - Reopen: where it was left. If that display was unplugged, it is centred again.
   - A user's resize survives a config reload or an OS appearance change, which republishes `chromeStyle`.
   - Pinned in Task 1 (the placement table) and Task 2 ("placed on first show", "a chrome republish keeps
     the user's size").
5. **Keyboard focus lands in Settings.** Expected: the window is active and the page holds the focus, so Tab
   reaches the first control. ⌘W (Ctrl+W elsewhere) closes Settings, not a pane. Pinned in Task 2 ("takes
   the keyboard focus", "the Close shortcut closes it").

Also pinned, beyond the five:
- Under `ui_style: terminal`, the window is sized in cells and drawn with ContourTui controls (Task 1, Task 2
  and the Task 4 snapshot).
- Settings is an independent window (no transient parent), so it keeps the app alive the way a terminal
  window does (Task 2, owner decision 1).
- On Linux and Windows the new-tab menu lists profiles only (Task 4, owner decision 3).

---

## Spec resolutions (read before Task 1)

These settle the points where the spec is silent, or where the code disagrees with it. Implementers follow
them; those marked **(owner)** are repeated under *Owner decisions*.

- **R-a. One controller per app.** Today each `WindowController` builds its own `SettingsController`
  (`WindowController.cpp:87-104`), and reload refreshes each one (`TerminalSessionManager.cpp:786-794`). One
  window per app means one controller per app. `ContourGuiApp` owns it, with the same collaborators, and
  `onConfigurationReloaded()` refreshes it. The host borrows it.
- **R-b. The window is made once and hidden on close.** Closing calls `QWindow::close()`, which hides the
  window but does not delete it. The next show finds the same window: that is the spec's "one instance" and
  the canvas's "reopens the last section" (within the process). The page's `editorMode` and the controller's
  draft survive.
- **R-c. Where it opens (`settingsWindowGeometry`, Task 1).**
  - First show: the row's size, fitted to the screen's available area (never below the minimum) and centred
    on the asking window's screen. That screen comes from `WindowController::screen()`; with none, the
    primary screen.
  - A hidden window that was shown before: its last geometry, made reachable on the screen it overlaps
    most. If it overlaps none, it is placed as on first show.
  - A visible or minimised window is not moved.
  - QML binds no `width`/`height`. `ChromeStyleHost` republishes `chromeStyle` on reloads and appearance
    changes, and a binding would snap a resized window back. The host reads `preferredSize` once per
    placement.
  - Not persisted across restarts **(owner)**.
- **R-d. OS conventions are a table by `HostPlatform`** (from the canvas):
  - MacOS: "Settings", close only (Qt dims the minimize and zoom buttons whose hints are absent; Task 5
    checks this by hand).
  - Windows: "Contour Settings", all three buttons.
  - KdePlasma and Other (Linux): "Settings", close only.
  - The Terminal row follows its host, as it keeps AppKit's traffic lights (canvas tui).
- **R-e. The extents.**
  - The default sizes are the canvas's: 960 × 820, 1110 × 920 and 1100 × 920 pixels, and 120 × 40 cells on
    the Terminal row.
  - The minimum is 720 × 480 on the pixel rows and 90 × 24 cells on the Terminal row, which leaves the
    page's 280-wide rail (`SettingsPage.qml:291`) and one field column in view. The canvas gives no minimum;
    if this is wrong, it costs one value per row.
- **R-f. Routing (carry-in 1).**
  - `TerminalSession::operator()(OpenConfiguration)` keeps calling `_manager->openSettings(this)`.
  - The manager now calls `_app.openSettings(screen of the acting session's window, or null)`, so a session
    whose window is minimised still opens Settings, and nothing raises the minimised window.
  - `ContourGuiApp::runWithoutWindow` (`ContourGuiApp.cpp:1864-1880`) handles
    `OpenConfiguration { .target = SettingsWindow }` with `openSettings(nullptr)`, so the menu bar's
    Settings… works with no terminal window at all.
  - The Application scope's minimised-window route (`:1845-1851`) needs no change: it reaches the same sink.
- **R-g. `OpenConfiguration { bool inEditor }` becomes `{ ConfigurationTarget target }`** with
  `enum class ConfigurationTarget : uint8_t { SettingsWindow = 0, Editor }` (§1.7). Config.cpp keeps reading
  `in_editor: true` and converts it at the boundary.
- **R-h. "The tab model no longer has a settings row."** The model never had one. The pseudo-tab is the
  pinned item `TabStrip.qml:211-270`, plus `IsActiveRole` and the badge focus, which deny every terminal tab
  while it shows (`WindowController.cpp:532-534`, `:590-592`). Task 4 removes all three. Its tests pin that
  the strip holds no `settingsTab`, and that the active tab stays active while Settings is open.
- **R-i. `SettingsPage.qml` is unchanged in behaviour.** Its Close button calls
  `windowController.closeSettings()` (`:270-272`). The window passes the host as `windowController`, and the
  host offers that invokable. Only the header comment (`:2-6`), which names `settingsActive` and Main.qml, is
  rewritten (Task 4).
- **R-j. "`SettingsPageQml_test` still passes inside the window."**
  - `SettingsPageView` (`SettingsPageQml_test.cpp:62-90`) builds `SettingsWindow.qml` through a
    `SettingsWindowHost` and takes the page from it.
  - The seven cases that showed the page in their own `TestWindow(1000, 800)` use the Settings window instead
    (Task 2). Every case then runs the page exactly as the app hosts it.
- **R-k. ⌘W / Ctrl+W closes Settings.** This is the HIG's convention for a settings window; the canvas is
  silent. It is a window-scoped QML `Shortcut` on `StandardKey.Close`. That adds no `ShortcutOverride` claim
  to any terminal (1b's R-j).
- **R-l. One palette binding.** Main.qml's palette block (`Main.qml:45-79`) moves into `ThemedWindow.qml`,
  an `ApplicationWindow` that both windows derive from, so the Settings window's controls follow
  `theme: dark|light` on KDE/GNOME as the terminal window's do.
- **R-m. Snapshots.** `ContentPage { Terminal, Settings }` becomes
  `SnapshotSurface { MainWindow = 0, SettingsWindow }`: which window the picture is of. The `settings` row
  renders the Settings window at its own placed size. The display case checks the grabbed window's size
  rather than `SnapshotWindowSize`.
- **R-n. Settings keeps the app alive (owner).** It has no transient parent, so under Qt's
  quit-on-last-window rule it counts as a window. `quitIfWindowless` counts it too (`ContourGuiApp.cpp:284-295`).
  Closing the last terminal window leaves Settings open, and closing Settings then quits.
- **R-o. Ctrl+, is not bound.** The spec says "⌘, / Ctrl+,", but the defaults bind Ctrl+Shift+,
  (`Config.hpp:1605`, `:1619`). This step changes no default binding: that is issue #2175's territory, like
  the macOS ⌘ chords.

### Non-goals

- Settings sections, the sidebar, controls, the one save model and live preview (Step 4b, §3.13).
- The canvas's macOS ⌘ default chords (⌘N, ⌘T, ⌘W, …, issue #2175), and a Ctrl+, default (R-o).
- About after ⌘M. `ShowAbout` still opens its dialog in the last-focused window, even when that window is
  minimised. §3.3 does not cover it. Park it for the step that next touches `ShowAbout` (the 1b ledger's
  carry-in).
- The Windows Mica frame and the Linux header bar for the Settings window (5a, 5b). The window keeps the
  OS's own frame here.
- Persisting the window's geometry, or "the last section", across restarts (owner decision 2).

---

### Task 1: The Settings window's extents, conventions and placement

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp:114-127` (`SizeTokens`); every row's `.size`: `MacRow`
  `:319-328`, `FluentRow` `:341-350`, `LinuxRow` `:363-372`, `TerminalRow` `:389-398`.
- Modify: `src/contour/window/ChromeTokenObjects.hpp:35-76` (`ChromeSizeTokens`).
- Create: `src/contour/window/SettingsWindowRules.hpp`, `src/contour/window/SettingsWindowRules.cpp`. List
  them in `src/contour/CMakeLists.txt`'s GUI block, the header beside `window/MenuRouting.hpp` (`:281`) and
  the source beside `window/WindowPositionStore.cpp` (`:336`).
- Test: `src/contour/config/ChromeTokenGroups_test.cpp:46-200`, `src/contour/window/UiStyleProvider_test.cpp:471-501`,
  and new `src/contour/window/SettingsWindowRules_test.cpp`. Add the new file to `contour_gui_test`'s list
  beside `window/MenuRouting_test.cpp` (`src/contour/CMakeLists.txt:798`). It needs QtCore, so it does not
  go in `contour_test`.

**Interfaces:**
- Produces (`config::SizeTokens`, appended after `swatchHeight` in this order): `int settingsWidth = 0;`,
  `int settingsHeight = 0;`, `int settingsMinWidth = 0;`, `int settingsMinHeight = 0;`. Values, as
  width × height and min width × min height:

  | Row | Default | Minimum |
  |---|---|---|
  | `MacRow` | 960 × 820 | 720 × 480 |
  | `FluentRow` | 1110 × 920 | 720 × 480 |
  | `LinuxRow` | 1100 × 920 | 720 × 480 |
  | `TerminalRow` (cells) | 120 × 40 | 90 × 24 |

- Produces (`window::ChromeSizeTokens`): `Q_PROPERTY(qreal settingsWidth …)`, `settingsHeight`,
  `settingsMinWidth` and `settingsMinHeight`, all `CONSTANT`. Widths multiply by `_units.horizontal`,
  heights by `_units.vertical`, as `dialogWidth` and `toast` do (`:65-67`).
- Produces (`window/SettingsWindowRules.hpp`, namespace `contour::window`):

```cpp
/// Which title-bar buttons the Settings window offers besides Close.
enum class SettingsWindowButtons : uint8_t
{
    All = 0,   //!< Minimize, maximize and close: the OS's default window.
    CloseOnly, //!< Close alone; macOS dims minimize and zoom, as the HIG asks of a settings window.
};

/// How the Settings window presents itself on one host.
struct SettingsWindowConventions
{
    std::string_view title {};                            ///< The window title.
    SettingsWindowButtons buttons = SettingsWindowButtons::All; ///< Its title-bar buttons.
};

/// The conventions per host, indexed by config::HostPlatform.
inline constexpr auto SettingsWindowConventionRows = std::array { /* Other, Windows, MacOS, KdePlasma */ };

[[nodiscard]] constexpr SettingsWindowConventions settingsWindowConventionsFor(config::HostPlatform host) noexcept;

/// What deciding where the Settings window goes needs to know, in logical pixels.
struct SettingsWindowPlacement
{
    QSize preferred {};               ///< The row's default size.
    QSize minimum {};                 ///< The row's minimum size.
    std::optional<QRect> previous {}; ///< Where it was last shown; none before its first show.
    QRect anchor {};                  ///< The available area of the screen to open on.
    std::vector<QRect> screens {};    ///< The available area of every screen.
    QMargins frame {};                ///< The native frame around the client area.
};

/// Where the Settings window goes (spec resolution R-c). @return The client geometry.
[[nodiscard]] QRect settingsWindowGeometry(SettingsWindowPlacement const& placement);
```

  The rows are, by `HostPlatform`:
  - `Other`: `{ "Settings", CloseOnly }`.
  - `Windows`: `{ "Contour Settings", All }`.
  - `MacOS`: `{ "Settings", CloseOnly }`.
  - `KdePlasma`: `{ "Settings", CloseOnly }`.

  A `static_assert` pins the row count to `static_cast<size_t>(HostPlatform::KdePlasma) + 1`.
- The algorithm `settingsWindowGeometry` implements (the signature does not determine it):
  1. When `previous` intersects a rect of `screens`, take the one with the largest intersection area, `s`,
     and return `QRect(reachableWindowPosition(previous->topLeft() - s.topLeft(), previous->size(), s, frame), previous->size())`.
  2. Otherwise, with `a = anchor`:
     - `w = max(minimum.width(), min(preferred.width(), a.width() - frame.left() - frame.right()))`, and `h`
       likewise with `top`/`bottom`.
     - Return `QRect(reachableWindowPosition(QPoint((a.width() - w) / 2, (a.height() - h) / 2), QSize(w, h), a, frame), QSize(w, h))`.

  `reachableWindowPosition` is `WindowPositionStore.hpp:32-35`.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp`, `[chrometokens]`, "chromeTokens.everyRowMatchesTheDesignKit":
    - `numbersOf` (`:46-74`) lists the four new fields after `swatchHeight`.
    - Each row's head gains the table's four values. For example, MacOS's head becomes
      `{ 38, 28, 24, 28, 22, 36, 280, 16, 16, 960, 820, 720, 480, 6, 10, 6 }`, and Terminal's
      `{ 1, 1, 1, 2, 1, 1, 32, 2, 1, 120, 40, 90, 24, 1, 1, 0 }`.
    - The comment `size(9)` reads `size(13)`.
  - `UiStyleProvider_test.cpp`, `[contour][uistyle]`, "UiStyleProvider publishes the 1a token groups per row":
    - On MacOS, `tokenOf(mac, "size", "settingsWidth") == 960.0` and `settingsMinHeight == 480.0`.
    - On Terminal, `settingsWidth == Approx(120 * terminal.widthQuantum())`,
      `settingsHeight == Approx(40 * terminal.cellHeight())` and
      `settingsMinWidth == Approx(90 * terminal.widthQuantum())`.
  - `SettingsWindowRules_test.cpp`, `[contour][settingswindow][rules]`:
    - "every host has its conventions": `settingsWindowConventionsFor` gives the four rows above, checked
      for every enumerator through `std::views::iota(std::size_t { 0 }, SettingsWindowConventionRows.size())`.
    - "the Settings window is placed": a table of `SettingsWindowPlacement` → expected `QRect`. Frames are
      zero unless named, and the preferred size is 960 × 820 with a 720 × 480 minimum unless named.

      | Case | previous | anchor | screens | expected |
      |---|---|---|---|---|
      | first show | — | (0,0,1440,875) | {anchor} | (240,27,960,820) |
      | small screen (preferred 1110×920) | — | (0,0,1366,728) | {anchor} | (128,0,1110,728) |
      | screen below the minimum | — | (0,0,600,400) | {anchor} | (0,0,720,480) |
      | a title bar to keep on screen (frame top 28) | — | (0,0,1440,875) | {anchor} | (240,28,960,820) |
      | reopened on a live second screen | (1600,100,960,820) | (0,0,1440,875) | {(0,0,1440,875), (1440,0,1920,1080)} | (1600,100,960,820) |
      | reopened after its screen was unplugged | (1600,100,960,820) | (0,0,1440,875) | {(0,0,1440,875)} | (240,27,960,820) |
      | reopened half off its screen | (1000,500,960,820) | (0,0,1440,875) | {(0,0,1440,875)} | (480,55,960,820) |
      | a user's own size is kept | (100,100,800,500) | (0,0,1440,875) | {(0,0,1440,875)} | (100,100,800,500) |

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

Expected: compile failure, because `SettingsWindowRules.hpp` does not exist.

- [ ] **Step 3: Implement** the four fields and their values in every row, the four properties, the
  conventions table and `settingsWindowGeometry`.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[chrometokens]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[contour][settingswindow][rules]"`

Expected: PASS.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/UiStyleProvider_test.cpp src/contour/window/SettingsWindowRules*.{hpp,cpp}
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/config/ChromeTokenGroups* src/contour/window/ChromeTokenObjects.hpp src/contour/window/UiStyleProvider_test.cpp src/contour/window/SettingsWindowRules* src/contour/CMakeLists.txt
git commit -m "contour: the Settings window's extents, per-OS conventions and placement" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `SettingsWindow.qml` and its host

**Files:**
- Create: `src/contour/qml/ThemedWindow.qml`. Main.qml's palette block (`Main.qml:45-79`) moves into it,
  with its comment. `src/contour/qml/Main.qml:8` becomes `ThemedWindow`.
- Create: `src/contour/qml/SettingsWindow.qml`. List both files in `_qml_files`
  (`src/contour/CMakeLists.txt:379-418`), `SettingsWindow.qml` beside `SettingsPage.qml`.
- Create: `src/contour/window/SettingsWindowHost.hpp`, `src/contour/window/SettingsWindowHost.cpp`. List them
  beside `window/SettingsController.{hpp,cpp}` (`src/contour/CMakeLists.txt:276`, `:321`).
- Test: new `src/contour/window/SettingsWindowHost_test.cpp`, in `contour_gui_test`'s list beside
  `window/AppMenuBar_test.cpp` (`:796`). Also `src/contour/test/SettingsPageQml_test.cpp`: `SettingsPageView`
  `:62-90`, `TestWindow` `:172-225`, and the seven `TestWindow(1000, 800)` + `window.show(*page)` sites at
  `:784-785`, `:943-944`, `:1028-1029`, `:1072-1073`, `:1204-1205`, `:1363-1364` and `:1426-1427`.

**Interfaces:**
- Consumes (Task 1): `SettingsWindowConventions`, `SettingsWindowButtons`, `SettingsWindowPlacement`,
  `settingsWindowGeometry`, and `chromeStyle.size.settings{Width,Height,MinWidth,MinHeight}`.
- Produces (namespace `contour::window`, `SettingsWindowHost.hpp`):

```cpp
/// Why the Settings window could not be shown.
enum class SettingsWindowError : uint8_t
{
    Unavailable = 0, //!< No QML engine to build it on (a headless app).
    LoadFailed,      //!< SettingsWindow.qml did not load; the component's errors were logged.
};

class SettingsWindowHost;

/// Builds the Settings window for @p host; called once, on the first show.
using SettingsWindowFactory =
    std::function<std::expected<std::unique_ptr<QQuickWindow>, SettingsWindowError>(SettingsWindowHost& host)>;

/// The app's one Settings window: built on first show, hidden (never destroyed) on close.
class SettingsWindowHost final: public QObject
{
    Q_OBJECT
    Q_PROPERTY(contour::window::SettingsController* controller READ controller CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(int windowFlags READ windowFlags CONSTANT)

  public:
    /// @param controller What the page edits; borrowed, outlives this host.
    SettingsWindowHost(SettingsController& controller, SettingsWindowConventions conventions,
                       SettingsWindowFactory factory, QObject* parent = nullptr);
    ~SettingsWindowHost() override;

    [[nodiscard]] SettingsController* controller() const noexcept;
    [[nodiscard]] QString title() const;               ///< conventions.title
    [[nodiscard]] int windowFlags() const noexcept;    ///< settingsWindowFlags(conventions.buttons).toInt()

    /// Refreshes the controller, builds the window on the first call, places a hidden window (R-c),
    /// clears a minimised state (keeping maximised), shows, raises and activates it.
    /// @param near The screen of the window that asked; null for the primary screen.
    /// @pre GUI thread.
    [[nodiscard]] std::expected<void, SettingsWindowError> show(QScreen* near);

    /// Closes (hides) the window: the page's Close button and the Close shortcut. A no-op before the first show.
    Q_INVOKABLE void closeSettings();

    /// @return The window, or null before the first successful show.
    [[nodiscard]] QQuickWindow* window() const noexcept;
};

/// Loads Contour.Ui's SettingsWindow on @p engine with `host` as its initial property.
[[nodiscard]] std::expected<std::unique_ptr<QQuickWindow>, SettingsWindowError> loadSettingsWindow(
    QQmlEngine& engine, SettingsWindowHost& host);

/// CloseOnly: Window | WindowTitleHint | WindowCloseButtonHint | CustomizeWindowHint; All: Window.
[[nodiscard]] Qt::WindowFlags settingsWindowFlags(SettingsWindowButtons buttons) noexcept;
```

  `loadSettingsWindow` uses `QQmlComponent::loadFromModule("Contour.Ui", "SettingsWindow")` (production
  loads `Main` the same way, `ContourGuiApp.cpp:2625`). It returns `LoadFailed` and logs
  `component.errorString()` with `errorLog()` when the component is not ready or the object is no
  `QQuickWindow`. `show()` reads `preferredSize` from the window's property of that name, and the rest from
  `minimumSize()`, `frameMargins()`, `QGuiApplication::screens()` and their `availableGeometry()`. It passes
  `previous` only when the window existed before this call and is hidden. When the factory fails, it keeps
  no window, and the next `show()` tries again.
- Produces (QML):
  - `ThemedWindow.qml`: an `ApplicationWindow` whose palette follows a `SystemPalette` (the moved block).
  - `SettingsWindow.qml`: a `ThemedWindow` with:
    - `objectName: "settingsWindow"`, `required property var host`, `visible: false`;
    - `title: host ? host.title : ""` and `flags: host ? host.windowFlags : Qt.Window`;
    - `minimumWidth: chromeStyle.size.settingsMinWidth`, `minimumHeight: chromeStyle.size.settingsMinHeight`;
    - `readonly property size preferredSize: Qt.size(chromeStyle.size.settingsWidth, chromeStyle.size.settingsHeight)`;
    - `color: palette.window`;
    - a `SettingsPage { objectName: "settingsPage"; anchors.fill: parent; focus: true; controller: host ? host.controller : null; windowController: host }`;
    - `Shortcut { objectName: "closeSettingsShortcut"; sequences: [StandardKey.Close]; context: Qt.WindowShortcut; onActivated: if (host) host.closeSettings() }`.

    It has no `width`/`height` binding.
- Produces (`SettingsPageQml_test.cpp`):
  - `TestWindow` moves above `SettingsPageView`. It gains `TestWindow(QQuickWindow& window, QSize size)`,
    which borrows @p window, resizes, shows and activates it, and parents its focus sink into the window's
    `contentItem()`. It holds `std::unique_ptr<QQuickWindow> _owned` plus `QQuickWindow* _window`.
  - `SettingsPageView` holds a `window::SettingsWindowHost host` over `*controller`, with
    `settingsWindowConventionsFor(config::HostPlatform::Other)` and a factory calling
    `loadSettingsWindow(engine, host)`.
  - Its constructor `REQUIRE`s `host.show(nullptr)` and sets `page` (now `QPointer<QObject>`, so
    `page.get()` and `page->` still compile) to the window's `"settingsPage"` child.
  - `[[nodiscard]] TestWindow activeWindow(QSize size)` returns `TestWindow { *host.window(), size }`.
- **TUI row:** the window is the same window under `ui_style: terminal`. Its extents are counted in cells
  (Task 1), and the app-wide style draws its controls with ContourTui.

- [ ] **Step 1: Write the failing tests.**
  - `SettingsWindowHost_test.cpp`, tag `[contour][gui][qml][settingswindow][host]`. Each case builds a
    `QQmlEngine` with `installChromeStyle(engine)`, a `SettingsController` as `SettingsPageQml_test`'s
    `ControllerFixture` builds it (`:94-128`), and a host whose factory counts its calls and calls
    `loadSettingsWindow`. It ends with zero `isQmlDiagnostic` warnings (`QmlMessageCapture`).
    - "the Settings window opens once, however often it is asked for": two `show(nullptr)` calls both
      succeed, the factory ran once, and `window()` is the same non-null, visible window both times.
    - "it comes back in front": after `show`, a second `QQuickWindow other` is shown and activated
      (`QTest::qWaitForWindowActive(&other)`). After that, `show(nullptr)` makes the Settings window active.
      Separately, `setWindowStates(Qt::WindowMinimized)` then `show` gives a window whose states lack
      `WindowMinimized`.
    - "it is placed on first show": `window()->geometry() == settingsWindowGeometry({ .preferred = preferredSize, .minimum = minimumSize(), .previous = std::nullopt, .anchor = primaryScreen()->availableGeometry(), .screens = { that rect }, .frame = frameMargins() })`.
    - "a chrome republish keeps the user's size": `resize(QSize(750, 500))`, then set a new `UiStyleProvider`
      as the `chromeStyle` context property, as `ChromeStyleHost` does. After
      `QCoreApplication::processEvents()`, `size() == QSize(750, 500)`.
    - "it takes the keyboard focus": after `show`, `QTest::qWaitForWindowActive(window())` holds, and the
      `"settingsPage"` item `hasActiveFocus()`.
    - "the Close shortcut closes it": `QTest::keySequence(window(), QKeySequence::Close)` leaves
      `isVisible() == false`. `window()` still returns the same object.
    - "closing keeps the window, its pane and its draft":
      - Click `newProfileButton`, set one scalar field of the draft with
        `controller.setProfileField(key, value)`, and note `editingProfile()` and the page's `editorMode`
        (`"profile"`).
      - Then `closeSettings()` and `show(nullptr)`.
      - Expect the same window, `editorMode == "profile"`, the same `editingProfile()`, and the field's value
        in `profileFields()` unchanged.
      - Take the key from `profileFields()` (the first row whose `type` is `"string"` or `"int"`), never a
        literal guessed here.
    - "the window follows its host's conventions": for each row of `SettingsWindowConventionRows`, a host
      built with it gives a window whose `title()` is the row's title. Its `flags()` lack
      `WindowMinimizeButtonHint` and `WindowMaximizeButtonHint` for `CloseOnly`, and equal `Qt::Window` for
      `All`.
    - "the Terminal row counts it in cells": with `installChromeStyle(engine, config::UiStyle::Terminal)`,
      `preferredSize.width == Approx(120 * provider.widthQuantum())` and
      `minimumHeight == Approx(24 * provider.cellHeight())`.
    - "it keeps the app alive": `window()->transientParent() == nullptr` (R-n).
    - "a factory that fails leaves no window": with a factory returning `SettingsWindowError::LoadFailed`,
      `show(nullptr).error() == LoadFailed` and `window() == nullptr`.
  - `SettingsPageQml_test.cpp`, `[contour][gui][qml][settings]`: no assertion changes. It is the R-j
    rewiring above; the seven sites become `auto window = fx.activeWindow(QSize(1000, 800));` (`view.` at
    `:1204`). It fails to compile until `SettingsWindowHost` exists.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

Expected: compile failure on `SettingsWindowHost.hpp`.

- [ ] **Step 3: Implement** `ThemedWindow.qml`, `SettingsWindow.qml`, the host, `loadSettingsWindow` and
  `settingsWindowFlags`, and then the test rewiring.

- [ ] **Step 4: Run them and see them pass.**

Run:

```bash
out/appleclang-debug/src/contour/contour_gui_test "[settingswindow][host],[contour][gui][qml][settings],[contour][gui][qml][mainwindow],[contour][gui][qml][a11y]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[settingswindow][host],[contour][gui][qml][settings]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

Expected: PASS. All 22 `SettingsPageQml_test` cases pass inside the window. `[mainwindow]` proves that
Main.qml as a `ThemedWindow` loads as before, and the lint passes with zero literals in both new files.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/SettingsWindowHost*.{hpp,cpp} src/contour/test/SettingsPageQml_test.cpp
editorconfig-checker src/contour/qml/ThemedWindow.qml src/contour/qml/SettingsWindow.qml src/contour/qml/Main.qml src/contour/CMakeLists.txt
git add src/contour/qml/ThemedWindow.qml src/contour/qml/SettingsWindow.qml src/contour/qml/Main.qml src/contour/window/SettingsWindowHost* src/contour/test/SettingsPageQml_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: a Settings window that hosts the settings page, made once and hidden on close" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: One Settings window per app, opened from anywhere

**Files:**
- Modify: `src/contour/ContourGuiApp.hpp`:
  - the constructor (`:250-266`), which gains a last parameter;
  - beside `forcedFontDpiProvider` (`:298-301`), the three accessors;
  - `runWithoutWindow`'s doc (`:572-576`);
  - the members after `_qmlEngine` (`:1001`), and `_hostPlatform` before `_menuBarPlacement` (`:1013`).
- Modify: `src/contour/ContourGuiApp.cpp`:
  - the constructor (`:401-416`, `:460`);
  - `runWithoutWindow` (`:1864-1880`) and `onConfigurationReloaded` (`:1882-1897`);
  - `releaseQtObjects` (`:2240-2252`);
  - the QML registrations (`:2383`, `:2388`).
- Modify: `src/contour/config/Actions.hpp:60`, `:476-479`; `src/contour/config/Config.cpp:4645-4651`;
  `src/contour/command/MenuBarTable.cpp:121`.
- Modify: `src/contour/session/TerminalSession.cpp:4754-4771`;
  `src/contour/session/TerminalSessionManager.hpp:256-259`, `.cpp:749-756`.
- Modify: `src/contour/window/WindowController.hpp`, which gains `screen()` beside `isMinimized()` (`:582`),
  and `.cpp`.
- Modify: `src/contour/test/GuiTestFixtures.hpp`: the TestApp constructors (`:483-512`, `:523-542`,
  `:569-595`) and the members (`:735-760`).
- Test: new `src/contour/window/SettingsWindowRouting_test.cpp`, in `contour_gui_test_heavy_sources` beside
  `window/WindowMenus_test.cpp` (`src/contour/CMakeLists.txt:879`), because it builds a `TestApp`. Also
  `src/contour/config/Config_test.cpp:814-834`, and `src/contour/session/TerminalSession_test.cpp:1197`,
  `:4207` and `:5129` (field rename only).

**Interfaces:**
- Consumes (Task 2): `SettingsWindowHost`, `SettingsWindowFactory`, `SettingsWindowError` and
  `loadSettingsWindow`. Consumes (Task 1): `settingsWindowConventionsFor`.
- Produces (`actions`):
  - `enum class ConfigurationTarget : uint8_t { SettingsWindow = 0, Editor };`
  - `struct OpenConfiguration { ConfigurationTarget target = ConfigurationTarget::SettingsWindow; };`
  - The documentation reads: "Opens the Settings window (profiles, color schemes, global settings). Set
    `in_editor: true` on the binding to instead open the configuration file in an external editor."
  - Config.cpp maps `in_editor` (`as<bool>()`) to `Editor`/`SettingsWindow`. The menu bar row becomes
    `OpenConfiguration { .target = ConfigurationTarget::Editor }`.
- Produces (`ContourGuiApp`):
  - `[[nodiscard]] window::SettingsController& settingsController();` — one per app, created on first use
    with today's collaborators (`WindowController.cpp:90-104`, moved): `_config`, a `FileGuiConfigStore` at
    `_config.configFile.parent_path()`, an apply step that reloads every session and re-applies the window
    control style, and `_gpuInventory`.
  - `[[nodiscard]] window::SettingsWindowHost& settingsWindow();` — one per app, created on first use over
    `settingsController()`, `settingsWindowConventionsFor(_hostPlatform)` and `_settingsWindowFactory`.
  - `void openSettings(QScreen* near);` — `settingsWindow().show(near)`. On an error it logs
    `"Cannot open the Settings window."` with the enumerator's name.
  - The constructor's new last parameter, `window::SettingsWindowFactory settingsWindowFactory = nullptr`.
    Null means "load `SettingsWindow` on the app's own engine": a lambda that gives
    `SettingsWindowError::Unavailable` while `_qmlEngine` is null, else `loadSettingsWindow(*_qmlEngine, host)`.
    `main.cpp:220-235` is unchanged.
  - The members are `config::HostPlatform _hostPlatform` (initialised from `detectHostPlatform(env)`, with
    `_menuBarPlacement` derived from it), `_settingsWindowFactory`, then
    `std::unique_ptr<window::SettingsController> _settingsController`, then
    `std::unique_ptr<window::SettingsWindowHost> _settingsWindow`. They are declared after `_qmlEngine`, so
    the window goes before the engine, and the controller outlives the host that borrows it.
  - `releaseQtObjects` resets `_settingsWindow` and then `_settingsController`, before `_qmlEngine`.
  - `onConfigurationReloaded()` calls `_settingsController->refresh()` when the controller exists.
  - `runWithoutWindow` runs `openSettings(nullptr)` for `OpenConfiguration` with target `SettingsWindow`,
    beside `Quit` and `NewTerminal`.
  - Registration: `qmlRegisterUncreatableType<window::SettingsWindowHost>("Contour.Terminal", 1, 0, "SettingsWindowHost", "Created by the application.")`
    and `qRegisterMetaType<window::SettingsWindowHost*>`. `SettingsController`'s reason becomes
    "Created by the application.".
- Produces (`WindowController`): `[[nodiscard]] QScreen* screen() const noexcept;` — the bound window's
  screen; null before `bindWindow`.
- Produces (`TerminalSessionManager::openSettings(TerminalSession* acting)`): it calls
  `_app.openSettings(screen)`, where `screen` is the acting session's controller's `screen()`, or null. It no
  longer touches the controller's settings state. Its doc says that Settings opens whatever state that window
  is in, even with no window.
- `TerminalSession::operator()(OpenConfiguration)` branches on `event.target`, with no other change.
- Produces (`TestApp`):
  - A private `std::unique_ptr<QQmlEngine> _settingsEngine`, declared before `_app`.
  - A private `[[nodiscard]] contour::window::SettingsWindowFactory settingsWindowFactory()`. On first use it
    creates the engine with `installChromeStyle(*_settingsEngine)`, then calls `loadSettingsWindow` on it.
  - All four constructors pass `core::platform::defaultSteadyClock().now(), settingsWindowFactory()` after
    the appearance services.

- [ ] **Step 1: Write the failing tests.**
  - `SettingsWindowRouting_test.cpp`, tag `[contour][settingswindow][routing]`. Each case uses
    `TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() }`, and a helper that runs a
    menu-bar row by title, as `WindowMenus_test.cpp:120-127` does.
    - "OpenConfiguration from a session opens the one Settings window": a `ScopedController` with one session
      tab. `(*session)(actions::OpenConfiguration {})` twice. Expect `app.app().settingsWindow().window()`
      non-null and visible, and the same pointer after the second call.
    - "Settings… opens Settings while the last focused window is minimised" (carry-in 1):
      - Set up a `ScopedControllerWithOsWindow` bound to its `osWindow`, with one session tab, then
        `setFocusedWindow` and `osWindow->setWindowStates(Qt::WindowMinimized)`.
      - Call `clearFocusedWindow`, and then run the row "Settings…" from
        `buildMenuTree(buildMenuBar(window->menuBarState()), {}, ShortcutStyle::Text)`.
      - The Settings window is visible and `QTest::qWaitForWindowActive` holds. `window->isMinimized()` is
        still true.
    - "Settings… opens Settings with no terminal window at all": with no controller,
      `app.app().runMenuBarAction(actions::OpenConfiguration {}, command::ActionScope::Application)`.
      Expect the Settings window visible, and `app.launcher().openedUrls` empty.
    - "Open Configuration File still opens the file": `(*session)(OpenConfiguration { .target = ConfigurationTarget::Editor })`
      records one opened URL, and `app.app().settingsWindow().window()` stays null.
    - "a reload refreshes the app's Settings": with `ConfiguredTestApp`, write a config, open Settings,
      rewrite it with a second profile, and `reloadAllSessions()`. `settingsController().profiles()` then
      lists the new profile. This replaces the manager loop's per-window refresh in Task 4.
  - `Config_test.cpp`, `[config]`, "Config: OpenConfiguration in_editor parameter parses": collect
    `oc->target`. One mapping is `ConfigurationTarget::SettingsWindow` and one is `ConfigurationTarget::Editor`.
  - `TerminalSession_test.cpp`: `.inEditor = true` becomes `.target = actions::ConfigurationTarget::Editor`
    at the three sites. The comment at `:1204-1205` now says the call opens the Settings window.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

Expected: compile failure on `actions::ConfigurationTarget`.

- [ ] **Step 3: Implement** the action change, the app's controller, host and `openSettings`, the manager
  and session routing, `runWithoutWindow`, `WindowController::screen()` and the TestApp factory. Leave
  `WindowController`'s own settings surface as it is: Task 4 removes it. After this task nothing but tests
  sets `settingsActive`.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[menubar],[actions]" && out/appleclang-debug/src/contour/contour_gui_test "[settingswindow],[contour][windowmenus],[contour][session][actions],[config],[manager],[contour][app]"`

Expected: PASS.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/ContourGuiApp.{hpp,cpp} src/contour/config/Actions.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp src/contour/command/MenuBarTable.cpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/session/TerminalSessionManager.{hpp,cpp} src/contour/window/WindowController.{hpp,cpp} src/contour/window/SettingsWindowRouting_test.cpp src/contour/test/GuiTestFixtures.hpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/ContourGuiApp.* src/contour/config src/contour/command/MenuBarTable.cpp src/contour/session src/contour/window src/contour/test/GuiTestFixtures.hpp src/contour/CMakeLists.txt
git commit -m "contour: OpenConfiguration opens the app's one Settings window, with or without a terminal window" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: Settings is a window, not a tab

**Files:**
- Modify: `src/contour/window/WindowController.hpp`:
  - drop the `SettingsController` forward declaration (`:52`), the properties (`:129`, `:134`) and the
    Settings page block (`:339-361`);
  - fix the new-tab doc (`:385`);
  - drop the signal (`:798-799`), `setSettingsActive` (`:810-814`) and the members (`:986-991`).
- Modify: `src/contour/window/WindowController.cpp`:
  - drop the include (`:21`), the controller construction (`:87-104`) and the four functions (`:123-150`);
  - `IsActiveRole` (`:532-534`) and the badge focus (`:590-592`) lose `_settingsActive`;
  - `createNewTab`/`activateTab` drop `setSettingsActive(false)` (`:641-642`, `:650-652`).
- Modify: `src/contour/session/TerminalSessionManager.cpp`:
  - in `sessionVisibility` (`:480-492`), `contentShown` goes;
  - in `revealSession` (`:515-518`), the `closeSettings` call goes;
  - in `reloadAllSessions` (`:786-794`), the per-window settings refresh and its comment go (Task 3's app
    refresh replaces it);
  - drop the `SettingsController.hpp` include (`:10`).
- Modify: `src/contour/qml/Main.qml`: the pane loader's `visible:` and its comment (`:247-248`, `:254`), and
  the settings loader (`:260-273`). Also `src/contour/qml/TabStrip.qml`: the header comment (`:4-7`), the
  settings tab (`:211-270`), and the new-tab dropdown comment (`:292-294`). And
  `src/contour/qml/SettingsPage.qml:2-6`, the header comment only (R-i).
- Modify: `src/contour/command/TabMenus.cpp:97-104`, `TabMenus.hpp:47`; `scripts/qml-size-literal-baseline.txt:27`
  (`TabStrip.qml 3` → `2`, regenerated with `python3 scripts/check-qml-sizes.py --print-baseline`).
- Modify: `src/contour/test/MockMainController.hpp:38-40` (`MainWindowScene::settingsController`),
  `:81-84` and `:170-175`. Also `src/contour/test/ChromeSnapshotPlan.hpp:58-63`, `:103` and every row's
  `.page`; `src/contour/test/ChromeSnapshotScene.hpp:133-142`, `:150-170`, `:328-345` and `:455-466`.
- Test:
  - `src/contour/test/ChromeSnapshotPlan_test.cpp:26`;
  - `src/contour/test/ChromeSnapshot_test.cpp:97-101` and `:258-276`;
  - `src/contour/command/TabMenus_test.cpp:138-150`;
  - `src/contour/window/WindowMenus_test.cpp:382-394`;
  - `src/contour/test/QmlComponents_test.cpp:1127-1128`;
  - `src/contour/window/MultiWindow_test.cpp:441-475`;
  - `src/contour/session/TerminalSessionManager_test.cpp:846-854` and `:907-912`;
  - `src/contour/test/SettingsPageQml_test.cpp:1196-1199`;
  - `src/contour/test/MainWindowQml_test.cpp` (one new case).

**Interfaces:**
- Consumes (Task 3): `ContourGuiApp::settingsController()`, `settingsWindow()`, `openSettings(QScreen*)`.
  Consumes (Task 2): `SettingsWindowHost`, `loadSettingsWindow`.
- Removed: `WindowController::settingsActive()`, `settingsController()`, `openSettings()`,
  `closeSettings()`, `toggleSettings()`, `setSettingsActive()`, `settingsActiveChanged()`, `_settingsActive`
  and `_settingsController`. Also removed: the mock's `settingsActive`, `settingsController` and
  `closeSettings`, and `MainWindowScene::settingsController`.
- Produces (`ChromeSnapshotPlan.hpp`):
  - `enum class SnapshotSurface : uint8_t { MainWindow = 0, SettingsWindow };` replaces `ContentPage`.
  - `SnapshotScriptRow::surface` (`SnapshotSurface surface = SnapshotSurface::MainWindow;`) replaces `page`.
  - Every row names `.surface`. The `settings` row is `.surface = SnapshotSurface::SettingsWindow`, and its
    other fields are as today.
- Produces (`ChromeSnapshotScene`):
  - `makeSettings(SnapshotSurface)` builds the controller only for the Settings surface.
  - A member `std::unique_ptr<window::SettingsWindowHost> _settingsWindow`, declared after `_engine`, over
    `*_settings`, with `settingsWindowConventionsFor(rowHost(_variant.row).host)` and
    `loadSettingsWindow(_engine, host)`.
  - `show()` `REQUIRE`s `_settingsWindow->show(nullptr)` for that surface, after Main.qml is shown.
  - `[[nodiscard]] QQuickWindow& subject() const` returns the window the picture is of: the Settings window
    for that surface, else `window()`. `@pre` `show()` ran. `grab()` grabs `subject()`.
- `buildNewTabMenu` gives one row per profile and nothing else. Its doc says so.

- [ ] **Step 1: Write the failing tests.**
  - `TabMenus_test.cpp`, `[newtabmenu]`, renamed "newTabMenu.profilesOnly":
    - `buildNewTabMenu({ .profileNames = { "dark", "light" } })` titles are
      `{ "New tab — dark", "New tab — light" }`.
    - No entry holds `actions::OpenConfiguration`.
    - `buildNewTabMenu({})` is empty.
  - `WindowMenus_test.cpp`, `[contour][windowmenus]`, "the new-tab menu lists the profiles": the last row's
    title starts with `"New tab — "`, and no row's title is `"Settings"`.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][menus]`: the count is
    `static_cast<int>(contour::test::MockProfileNames.size())`, and the comment says "One row per profile".
  - `MultiWindow_test.cpp`, `[contour][multiwindow][settings]`, replacing "settings tab: …" with "the active
    tab stays active while Settings is open":
    - Two tabs, the second activated, then `app.app().openSettings(nullptr)`.
    - `IsActiveRole` is true for row 1 and false for row 0, and `rowCount() == window->count() == 2`.
  - `MainWindowQml_test.cpp`, `[contour][gui][qml][mainwindow]`, new "Main.qml has no settings page or
    settings tab (offscreen)": after `loadMainWindow`, `findChild<QObject*>("settingsPageLoader")` and
    `findChild<QObject*>("settingsTab")` are both null.
  - `TerminalSessionManager_test.cpp`, `[manager][notification]`:
    - The section at `:846-854` becomes "the Settings window covers no pane": after
      `app.app().openSettings(nullptr)`, `second` is `Focused` and `first` is `VisibleUnfocused`.
    - Delete the section "from behind the settings page" (`:907-912`).
  - `SettingsPageQml_test.cpp`, "SettingsPage follows a configuration reload while it is open": the
    controller is `&testApp.app().settingsController()`, and the `ScopedController`/`openSettings()` lines
    go.
  - `ChromeSnapshotPlan_test.cpp`, `[snapshot]`:
    `CHECK(scriptRow(SnapshotScript::Settings).surface == SnapshotSurface::SettingsWindow);`.
  - `ChromeSnapshot_test.cpp`, `[contour][gui][qml][snapshot]`, the `Settings` case:
    - `scene.subject().objectName() == "settingsWindow"` and it `isVisible()`;
    - `scene.subject().findChild<QQuickItem*>("settingsPage") != nullptr`;
    - `scene.root().findChild<QObject*>("settingsPageLoader") == nullptr`.
  - `ChromeSnapshot_test.cpp`, `[display][snapshot]`: `qWaitForWindowExposed(&scene.subject(), 3000)`.
    The check is `image.size() == scene.subject().size() * scene.subject().devicePixelRatio()`, and the
    facts use `scene.subject()`'s `devicePixelRatio()`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_test "[newtabmenu]"`

Expected: compile failure on `SnapshotSurface`. With that stubbed, `[newtabmenu]` still fails, because it
finds "Settings".

- [ ] **Step 3: Implement** the removals, the new-tab table, the snapshot column and scene, the mock
  cleanup, the comments and the baseline.

- [ ] **Step 4: Run them and see them pass.**

Run:

```bash
out/appleclang-debug/src/contour/contour_test "[newtabmenu],[tabmenu],[menubar],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][multiwindow],[manager],[contour][windowmenus],[contour][gui][qml][menus],[contour][gui][qml][mainwindow],[contour][gui][qml][snapshot],[contour][gui][qml][settings],[settingswindow],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot],[settingswindow][host]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

Expected: PASS. `[menubar]` includes the reachability check (1b's R-g): `OpenConfiguration` stays reachable
through the menu bar's Settings… row.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/WindowController.{hpp,cpp} src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/command/TabMenus.{hpp,cpp} src/contour/command/TabMenus_test.cpp src/contour/window/WindowMenus_test.cpp src/contour/window/MultiWindow_test.cpp src/contour/test/MockMainController.hpp src/contour/test/ChromeSnapshot*.{hpp,cpp} src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/test/SettingsPageQml_test.cpp
editorconfig-checker src/contour/qml/Main.qml src/contour/qml/TabStrip.qml src/contour/qml/SettingsPage.qml scripts/qml-size-literal-baseline.txt
git add src/contour/window src/contour/session src/contour/command src/contour/qml src/contour/test scripts/qml-size-literal-baseline.txt
git commit -m "contour: Settings is a window, not a tab: the pseudo-tab, settingsActive and the new-tab menu's Settings row go" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, so
  the plan's own commit is left out. Apply its findings, and re-run the filters of every task it touches.
  One commit, with the Signed-off-by line.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[newtabmenu],[tabmenu],[menubar],[actions],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[settingswindow],[contour][gui][qml][settings],[contour][gui][qml],[contour][windowmenus],[contour][multiwindow],[manager],[contour][session][actions],[config],[contour][app]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[settingswindow][host],[contour][gui][qml][settings],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot],[display][menubar]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, with
  `SnapshotScripts.size()` read from `ChromeSnapshotPlan.hpp`.
  - Look at `settings.*`. All four rows should show the Settings window alone, with no tab strip. The
    `settings.terminal.*` pictures should show it in cells with ContourTui controls.
  - Look at two unchanged scripts (`single-tab.*`, `palette.*`). Main.qml as a `ThemedWindow` must look as
    it did.
- [ ] **Step 4: Manual check of the app on this Mac** (`out/appleclang-debug/src/contour/contour.app`).
  Record the results in the ledger.
  - ⌘, and Contour › Settings… open one Settings window, centred on the terminal window's display. Asking
    again with it open, or behind a terminal window, brings the same window forward.
  - Its minimize and zoom buttons are dimmed, and close works. ⌘W closes it, including with the owner's
    `⌘W → ClosePane` binding in `~/.config/contour/contour.yml`. A pane must not close.
  - ⌘M on the terminal window, then ⌘, opens Settings, focused, and the terminal window stays in the Dock.
    With every terminal window closed but Settings open, the app stays (owner decision 1). ⌘, after closing
    Settings reopens it.
  - Edit a profile field without saving, close Settings, and reopen: the same profile and value are shown.
  - Resize Settings, close it, and reopen: it keeps the size and position. Toggle the OS appearance while
    it is open: the size is kept.
  - Under `ui_style: terminal`, Settings is the same separate window with ContourTui controls.
  - The new-tab "▾" menu lists profiles only. The tab strip never shows "⚙ Settings".
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release at
  `:107` (the `x.y.z` block at `:97-105` is a commented-out template):
  - "Changes Settings to open in a window of its own, one per app, from Settings… in the Contour menu (⌘,),
    the command palette or the `OpenConfiguration` binding. It opens with or without a terminal window, and
    keeps its place and unsaved edits when closed."
  - "Removes the Settings tab from the tab strip and the Settings entry from the new-tab menu."
- [ ] **Step 6:** Mark Step 4a done in the plans index's step table and its *Last update* line, and on the
  board (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-4a.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the render path. The Settings window is built on the first ⌘, rather than at
    startup. Before this step, every window's controller was built with that window.
  - **Risk:** medium-low.
    - Routing moves out of the window: carry-in 1.
    - Main.qml's root type becomes `ThemedWindow`.
    - All 22 `SettingsPageQml_test` cases now run in a real window.
  - **Coverage:** the new `[settingswindow]` cases (rules, host, routing), the migrated
    `SettingsPageQml_test`, and the `settings` snapshot on all four rows.

---

## Owner decisions

1. **Settings keeps the app alive (R-n).** With the last terminal window closed, Settings stays open, and
   closing it quits. This is Qt's rule, and what a separate window implies. Recommended: yes. The
   alternative is to close Settings with the last terminal window. That needs a quit hook and a test, and
   loses an unsaved draft.
2. **Settings' place is not remembered across restarts (R-c).** Within a run, it reopens where it was left.
   Recommended: no persistence in 4a. The alternative stores it beside `window-position.yml`
   (`WindowPositionStore`), which is one more store call and file key.
3. **The new-tab menu's Settings row goes on every OS (§3.3).** On Linux and Windows, Settings is then reached
   through the palette, Ctrl+Shift+, and the title-bar menu's "Tab Bar Settings…" until 5a/5b give those
   platforms their own entry. Recommended: remove it everywhere, as the spec says. The alternative keeps the
   row off macOS: one predicate on the table row, and its test.
