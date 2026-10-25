# Native Redesign — Step 6.7 (Quick Terminal: a Global Hotkey Slides a Minimal Terminal Down From the Top) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25). Line numbers are at HEAD `661b6029`. Every earlier step runs first:
4a is done, 1c is in progress, and 1d, 2, 3a–3f, 4b, 5a and 5b are written. Where this step uses code those steps
add, the task names it as their plans do and carries a **Depends on Step <id>** note. A pre-flight scan re-verifies
every reference before Task 1.

**Goal:** A global hotkey, unbound by default and set in Settings › General, slides a terminal window down from the
top of the screen under the pointer, full width × 40 %, in Minimal density, with its bottom corners rounded; the same
hotkey hides it, and so (an option, on by default) does losing focus. On macOS it is a non-activating panel, so the app
the user came from stays frontmost.

**Architecture:**
- **The hotkey is an interface with three backends.** `platform::IGlobalHotkey` (null inline) registers one chord and
  calls back on the GUI thread: Cocoa `RegisterEventHotKey`, Win32 `RegisterHotKey` with a native event filter, and
  the XDG GlobalShortcuts portal behind a Qt-free channel seam. A table maps each supported key to its macOS virtual
  key code, Windows virtual-key code and XDG trigger name, so adding a key is adding a row.
- **The window's life is a table.** `window/QuickTerminalPolicy.hpp` holds the state machine (`Absent`, `Hidden`,
  `Showing`, `Shown`, `Hiding`) as transition rows, the shown and hidden frames from the screen's available geometry
  and the 40 % fraction, and the motion (slide over `motion.emphasized`, or a cross-fade under Reduce Motion).
  `window::QuickTerminalController` (a `QObject` adapter) interprets it with one `QPropertyAnimation`.
- **The window is an ordinary `Main.qml` with a role and its own density.** `ContourGuiApp::openQuickTerminal()` loads
  `Main` into a child `QQmlContext` that carries a second `ChromeStyleHost` built with `ChromeDensity::Minimal`
  (Step 2); `TerminalSessionManager` stages `WindowRole::QuickTerminal` for the controller it creates, as it stages the
  spawn screen; the controller derives the window flags from the role through a pure table.
- **The panel is a platform seam.** `platform::IQuickTerminalPanel` adopts, presents and dismisses the native window:
  the Cocoa backend makes it a non-activating panel on every Space with its bottom corners masked (Task 1's spike
  decides the exact AppKit calls); elsewhere a Qt implementation shows, raises and activates it.

**Tech Stack:** C++23, Qt 6.11 (Gui, Quick, QtDBus on Linux), QML, Objective-C++ with ARC (AppKit, Carbon hot keys),
Win32 (`RegisterHotKey`), xdg-desktop-portal GlobalShortcuts, Catch2 v3, CMake build tree `out/appleclang-debug`,
GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.7 binds, with what
it cites: §1 (principles; 4 the OS through interfaces), §2.1 (token rows; `density` from Step 2; `motion.emphasized`),
§2.3 (Reduce Motion), §2.5–§2.6 (snapshots and testing), §2.7 (`DefaultsEdition`). The design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md` (Phase 6, "Moments: first run, restore, quick terminal").
The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `moments` (3 · Quick terminal),
`kit-tokens`, `kit-components` and `tui` are in `.superpowers/design/canvas/project/`. The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. The Step 4b ledger assigns "the quick-terminal
hotkey" in Settings to this step.

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

- **The spec, verbatim (§3.16, row 6.7):** "Quick terminal | A global hotkey opens a window sliding down from the top,
  full width × 40 %, in Minimal density | `IGlobalHotkey`: macOS `RegisterEventHotKey`, Windows `RegisterHotKey`,
  Linux GlobalShortcuts portal. The window is `Qt::Tool | Qt::WindowDoesNotAcceptFocus` (Qt maps it to a
  non-activating `NSPanel`)".
- **Canvas values, exact** (`moments` 3): "A global hotkey (unbound by default; set in General) slides a terminal
  down from the top: full width × 40 %, motion.emphasized, Minimal density, bottom corners rounded. Hides on focus
  loss (option). macOS: a non-activating NSPanel, so the app you came from stays frontmost." The board starts the
  panel below the menu bar (the screen's available geometry), rounds the bottom corners 14, and casts
  `0 12px 30px rgba(0,0,0,0.5)` (the kit's `e3`); content padding 8 × 14 is the profile's own margins.
  - `QuickTerminalHeightFraction = 0.4`; the frame is the full available width.
  - New token `radius.quickTerminal`: 14 MacOS, 8 Fluent, 12 ContourLinux, 0 Terminal; the panel's shadow is
    `elevation.e3` on the native rows and the Terminal row's hard one-cell offset.
  - Motion: `motion.emphasizedMs` (220) with an ease-out; under Reduce Motion (`MotionStyle::CrossFade`) an opacity
    fade of `ReducedMotionCrossFadeMs` (100) with no travel.
  - Config: `quick_terminal: { hotkey: <absent>, hide_on_focus_loss: true }`. `hotkey` takes the input-mapping
    spelling (`{ mods: [Control], key: '`' }`). Unbound in both editions and `hide_on_focus_loss` true in both, so
    no `EditionDefaults` column: nothing changes until a user binds the key. A `Config_test` fixture pins both
    editions (the `DefaultsEdition` ruling).
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the SDD ledgers of 1c, 1d, 2, 3a–3f, 4b, 5b):
  - Every OS service is an interface in `contour/platform/` with a null implementation defined inline in its header
    and a test double, built through a `platform::AppearanceServices` factory that `nullAppearanceServices()` and
    `appearanceServicesWith(…)` both name, so no `TestApp` registers a real hotkey.
  - AppKit: ARC `.mm`s inside the `if(APPLE)` block of `src/contour/platform/CMakeLists.txt` (`:84-103`) and its
    `-fobjc-arc` list; every call `@pre` main thread, asserted with `NSThread.isMainThread`; non-Apple factories in a
    `.cpp` wholly inside `#ifndef __APPLE__`; headers listed in `add_library(contour_platform …)` (`:15-77`).
  - **This Mac cannot run Windows or a Linux desktop.** Windows-only and Linux-only translation units are wholly
    inside `#ifdef _WIN32` / `#ifdef __linux__`, listed unconditionally, and verified only by CI: each such task has a
    **Verified by** line, and is not done until that run is green (push, then read the log).
  - `[display]` cases skip offscreen. TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - No colour or size literal in chrome QML; a new file's baseline is 0.
  - Every visual surface is a snapshot row; every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - `enum class` over `bool`, zero the default; carve-outs: QML-facing `Q_PROPERTY`/`Q_INVOKABLE` booleans and
    `Config`'s public YAML `bool`s (`hide_on_focus_loss`), converted to an enum at the boundary.
  - `std::expected` for fallible calls: `HotkeyError` starts at `= 0`.
  - The Step 1b menu-owned macOS chords (⌘Q ⌘, ⌘H ⌥⌘H ⌘M, `display/ShortcutOverride.hpp`'s `MenuOwnedKeys`) can never be
    the hotkey (R-f).
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** dependency injection (hotkeys and the native panel through interfaces; time through
  the animation's own clock), configuration at construction (`QuickTerminalController` takes its hotkey, panel,
  motion and config in its constructor; re-registration on reload is live reconfiguration, documented at the
  declaration as `Renderer::setFonts()` is), data-driven tables (`HotkeyKeyRows`, `QuickTerminalTransitions`,
  `WindowRoleRows`), Doxygen on every new public declaration, `.hpp` with `#pragma once`, zero warnings, no `NOLINT`;
  `slots`, `signals` and `emit` are never identifiers.
- **Build** with every core, configured `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never two builds in one tree at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding (Windows- and Linux-only files are clang-tidied by CI's `check_clang_tidy`); format with
  `/opt/homebrew/opt/llvm@22/bin/clang-format -i`; check `.qml`, `.txt`, `CMakeLists.txt` and workflow files with
  `editorconfig-checker <files>`. **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value;
  every table row names every field (`ChromeTokenRows`, `SnapshotScripts`, `HotkeyKeyRows`,
  `QuickTerminalTransitions`, both null `AppearanceServices`).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Qt-free stays Qt-free:** `config/HotkeyChord.hpp`, `platform/HotkeyKeys.hpp`, `platform/GlobalShortcutsChannel.hpp`
  and `window/QuickTerminalPolicy.hpp` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, never `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 8).

## Review Focus

1. **The chord is already taken.** Another app (Spotlight's ⌘Space, a launcher, a second Contour) owns the chord, or
   the Linux portal's user declines it. Expected: no crash and no silent failure; a notice in the focused window says
   the hotkey could not be registered and why, and Settings shows the error beside the field. Pinned in Task 3
   (`HotkeyError` from the Cocoa and recording backends), Task 5 (the portal's refusal) and Task 6 ("a taken hotkey
   posts a notice").
2. **A config reload or a Settings edit re-registers.** Expected: an unchanged chord is not touched (the Linux portal
   must not ask again); a changed one is unregistered, then the new one registered; clearing it unregisters. Pinned in
   Task 6 ("an unchanged chord is not registered again").
3. **Focus loss, pressed again, closed.** Hotkey while shown and focused hides it; while shown but unfocused (with
   `hide_on_focus_loss: false`) focuses it; a press during the slide reverses it, never stacking a second window; the
   last tab closing ends the window and the next press makes a new one. Pinned in Task 2 (the transition table) and
   Task 6.
4. **The previous app stays frontmost on macOS, and the panel takes keys.** Expected: typing goes to the quick terminal
   while `NSWorkspace.frontmostApplication` is still the other app; dismissing returns keys to it with no Contour
   window raised. Decided in Task 1 (the spike measures it), pinned in Task 6's `[cocoa]` case and the Finish checks.
5. **Several screens, and a screen that goes away.** Expected: the panel opens full width on the screen under the
   pointer, below its menu bar, at 40 % of its available height; if that screen is unplugged while shown, it moves to
   the primary screen. Pinned in Task 2 (`quickTerminalFrames` over offset and odd-sized screens) and Task 6.

Also pinned, beyond the five: Reduce Motion fades without travel (Task 2); the quick terminal never restores into, or
saves as, a normal window position (Task 6); `ui_style: terminal` draws square corners and the hard shadow (Task 7).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. `Qt::WindowDoesNotAcceptFocus` may refuse keys (spike).** Qt's Cocoa plugin gives a window with that flag a
  non-activating panel style, but it also refuses to make such a window key, and a quick terminal that cannot take
  keys is no terminal. Task 1 measures three candidates and the ledger records the winner, which Task 6 implements:
  (A) the spec's `Qt::Tool | Qt::WindowDoesNotAcceptFocus`; (B) `Qt::Tool | Qt::FramelessWindowHint`, with the Cocoa
  panel adding `NSWindowStyleMaskNonactivatingPanel` and making it key without activating the app; (C) (B) plus
  `becomesKeyOnlyIfNeeded = NO`. The spec's intent, "non-activating, previous app frontmost", binds; its flag list
  yields to the measurement.
- **R-b. Which screen (owner, 1).** The screen under the pointer when the hotkey is pressed (`QCursor::pos()`), its
  `availableGeometry()`.
- **R-c. One quick terminal per app.** A second press never creates a second window; the controller owns at most one.
- **R-d. Wayland (owner, 2).** A Wayland client cannot place or move its window, so on Wayland the panel opens where
  the compositor puts it, with no slide (the cross-fade); the frame's width and height still apply. The portal's
  `Activated` signal may carry an `activation_token`; it is handed to Qt's activation (`XDG_ACTIVATION_TOKEN` before
  `requestActivate()`), so the compositor lets the window take focus.
- **R-e. Quitting (owner, 3).** As today: the app quits when its last visible window closes. A hidden quick terminal
  is not a reason to stay alive; on macOS the app stays as it does now.
- **R-f. The menu-owned chords.** `config/` is Qt-free and cannot read `MenuOwnedKeys` (a Qt table in `display/`), so
  the quick-terminal controller and `SettingsController::setHotkey` refuse a chord equal to any of them on a host whose
  menu bar is `Global`, through a new `display::isMenuOwnedChord(config::HotkeyChord const&, command::MenuBarPlacement)`
  beside `shortcutOverrideClaim`, with `HotkeyError::MenuOwned`; Settings says so.
- **R-g. Window position.** The quick terminal is never restored from, or saved to, `WindowPositionStore`; it is always
  placed by R-b.

## File structure

| File | Responsibility | Task |
|---|---|---|
| (scratchpad spike app, not committed) | AppKit measurements | 1 |
| `src/contour/config/HotkeyChord.hpp`, `QuickTerminalConfig.hpp` (create); `Config.*`, `ConfigDocumentation.hpp` (modify); `src/contour/window/QuickTerminalPolicy.hpp` (create) | Config, the state table, frames, motion | 2 |
| `src/contour/platform/GlobalHotkey.{hpp,cpp}`, `HotkeyKeys.hpp`, `CocoaGlobalHotkey.mm` (create); `AppearanceServices.hpp`; `test/HotkeyFixtures.hpp` | The interface, the key table, macOS | 3 |
| `src/contour/platform/Win32GlobalHotkey.cpp` (create) | Windows | 4 |
| `src/contour/platform/GlobalShortcutsChannel.hpp`, `PortalGlobalHotkey.{hpp,cpp}`, `DBusGlobalShortcutsChannel.{hpp,cpp}` (create) | Linux | 5 |
| `src/contour/platform/QuickTerminalPanel.{hpp,cpp}`, `CocoaQuickTerminalPanel.mm`, `src/contour/window/QuickTerminalController.{hpp,cpp}`, `window/WindowRole.hpp` (create); `ContourGuiApp.*`, `session/TerminalSessionManager.*`, `window/WindowController.*`, `qml/Main.qml` (modify) | The window | 6 |
| `src/contour/window/SettingsFields.*` (modify), `qml/KitShortcutField.qml` (create), snapshot files | Settings row, snapshots | 7 |

---

### Task 1: Spike — what makes a key-taking, non-activating panel on macOS

**Files:** none committed. The spike app lives in the session's scratchpad: one ARC `main.mm` linking `Qt6::Quick` and
`-framework AppKit`, showing a frameless 900 × 360 `QQuickWindow` with a `TextInput`, toggled by a 2-second timer.

**Interfaces:**
- Produces (in the ledger): `Ruling S1 (quick panel)`, naming the candidate (R-a's A, B or C) that passes every
  measurement, and the exact AppKit calls Task 6's `CocoaQuickTerminalPanel` makes.

- [ ] **Step 1: Build the spike** with each of R-a's three candidates selected by a command-line switch.
- [ ] **Step 2: Measure each candidate** with Finder in front, recording to `panel.jsonl` per run: whether typed text
  reaches the `TextInput`; `NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier` while shown; whether
  the window is key (`isKeyWindow`); whether it appears on a second Space and over a full-screen app with
  `NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary`; whether animating
  `QWindow::setY` 60 times over 220 ms shows tearing in a screen recording; whether `contentView.layer.cornerRadius`
  with `maskedCorners` of the two bottom corners clips the Qt surface.
- [ ] **Step 3: Record the ruling** in the SDD ledger (`.superpowers/sdd/2026-10-25-native-redesign-step-6-7/progress.md`)
  and update the board. If no candidate takes keys while the previous app stays frontmost, stop and raise it with the
  owner: §3.16's "non-activating" and "a terminal" cannot both hold.

---

### Task 2: The config, the state table, the frames and the motion

**Files:**
- Create: `src/contour/config/HotkeyChord.hpp` (Qt-free): the chord and its errors.
- Create: `src/contour/config/QuickTerminalConfig.hpp` (Qt-free): the block's struct.
- Modify: `src/contour/config/Config.hpp` (`ConfigEntry<QuickTerminalConfig, documentation::QuickTerminal> quickTerminal {};`
  beside `inputMethodEditor`, `:1224`), `Config.cpp` (parse `quick_terminal` with the input-mapping reader's
  `parseModifier`/`parseKeyOrChar`, write it back), `ConfigDocumentation.hpp`.
- Create: `src/contour/window/QuickTerminalPolicy.hpp` (header-only, Qt-free); list it in `_header_files`.
- Test: `src/contour/config/Config_test.cpp` (contour_gui_test), new `src/contour/window/QuickTerminalPolicy_test.cpp`
  (contour_test).

**Interfaces:**
- Produces (`contour::config`):
  - `struct HotkeyChord { vtbackend::Modifiers modifiers {}; std::variant<vtbackend::Key, char32_t> key {}; bool operator==(HotkeyChord const&) const = default; };`
  - `enum class HotkeyChordError : uint8_t { NoModifier = 0 };` — a chord needs at least one of Control, Alt, Super
    (Shift alone is not enough). Whether the key has a platform row, and whether the chord is menu-owned, are checked
    where those tables are visible: `HotkeyError::UnsupportedKey` (Task 3) and `HotkeyError::MenuOwned` (Task 6).
  - `enum class QuickTerminalFocusLoss : uint8_t { Hide = 0, Stay };`
  - `struct QuickTerminalConfig { std::optional<HotkeyChord> hotkey {}; QuickTerminalFocusLoss focusLoss = QuickTerminalFocusLoss::Hide; bool operator==(QuickTerminalConfig const&) const = default; };`
- Produces (`contour::window`, `QuickTerminalPolicy.hpp`):
  - `inline constexpr double QuickTerminalHeightFraction = 0.4;`
  - `struct ScreenRect { int x = 0; int y = 0; int width = 0; int height = 0; bool operator==(ScreenRect const&) const = default; };`
  - `struct QuickTerminalFrames { ScreenRect shown {}; ScreenRect hidden {}; };` and
    `[[nodiscard]] constexpr QuickTerminalFrames quickTerminalFrames(ScreenRect available) noexcept;` — `shown` is
    `{available.x, available.y, available.width, round(available.height × 0.4)}`; `hidden` is `shown` moved up by its
    own height.
  - `enum class QuickTerminalState : uint8_t { Absent = 0, Hidden, Showing, Shown, Hiding };`
  - `enum class QuickTerminalEvent : uint8_t { Hotkey = 0, FocusLost, FocusGained, AnimationDone, LastTabClosed };`
  - `enum class QuickTerminalFocus : uint8_t { Unfocused = 0, Focused };`
  - `enum class QuickTerminalEffect : uint8_t { None = 0, CreateAndShow, Show, Hide, Focus, Reverse, Destroy };`
  - `struct QuickTerminalTransition { QuickTerminalState from; QuickTerminalEvent event; QuickTerminalFocus focus; QuickTerminalFocusLoss focusLoss; QuickTerminalState to; QuickTerminalEffect effect; };`
    with `inline constexpr std::array QuickTerminalTransitions` and
    `[[nodiscard]] constexpr std::optional<QuickTerminalTransition> quickTerminalStep(QuickTerminalState, QuickTerminalEvent, QuickTerminalFocus, QuickTerminalFocusLoss) noexcept;`
    (a row whose `focus` or `focusLoss` column does not matter is listed once per value; an event with no row is
    ignored). The rows: `Absent+Hotkey → Showing, CreateAndShow`; `Hidden+Hotkey → Showing, Show`;
    `Showing+AnimationDone → Shown`; `Shown+Hotkey+Focused → Hiding, Hide`; `Shown+Hotkey+Unfocused → Shown, Focus`;
    `Shown+FocusLost+Hide → Hiding, Hide`; `Shown+FocusLost+Stay → Shown`; `Showing+Hotkey → Hiding, Reverse`;
    `Hiding+Hotkey → Showing, Reverse`; `Hiding+AnimationDone → Hidden`; any state `+LastTabClosed → Absent, Destroy`.
  - `enum class QuickTerminalMotion : uint8_t { Slide = 0, Fade };` and
    `struct QuickTerminalAnimation { QuickTerminalMotion motion; std::chrono::milliseconds duration; };`
    `[[nodiscard]] constexpr QuickTerminalAnimation quickTerminalAnimation(config::MotionTokens const& motion, PlacementControl placement) noexcept;`
    with `enum class PlacementControl : uint8_t { Client = 0, Compositor };` — `Slide` over `motion.emphasizedMs`
    under `MotionStyle::Full` and `Client`; otherwise `Fade` over `motion.emphasizedMs` (already
    `ReducedMotionCrossFadeMs` under Reduce Motion, `ChromeTokenGroups.hpp:827`).

- [ ] **Step 1: Write the failing tests.**
  - `QuickTerminalPolicy_test.cpp`, `[quickterminal]`:
    - "quickTerminal.framesTakeFortyPercentOfTheAvailableHeight" (Review Focus 5): `{0, 25, 1512, 957}` → shown
      `{0, 25, 1512, 383}`, hidden `{0, -358, 1512, 383}`; a screen at `{-1920, 0, 1920, 1080}` keeps its x; an odd
      height 1001 rounds to 400.
    - "quickTerminal.everyStateAndEventHasAtMostOneRow": for every combination, at most one row matches.
    - "quickTerminal.transitions" (Review Focus 3): each row above, e.g. `(Shown, Hotkey, Focused, Hide)` →
      `(Hiding, Hide)`; `(Showing, Hotkey, …)` → `(Hiding, Reverse)`; `(Shown, FocusLost, …, Stay)` → stays `Shown`;
      `(Hidden, FocusLost, …)` → none.
    - "quickTerminal.motion": Full + Client → `Slide`, 220 ms; CrossFade → `Fade`, 100 ms; Full + Compositor →
      `Fade`, 220 ms.
  - `Config_test.cpp`, `[config][quickterminal]`: both editions' defaults have no hotkey and `Hide`; `{ mods: [Control],
    key: '`' }` parses to `HotkeyChord{Control, U'`'}` and writes back; `{ mods: [], key: F12 }` warns and leaves it
    unbound (`NoModifier`); `hide_on_focus_loss: false` → `Stay`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `QuickTerminalPolicy.hpp`.
- [ ] **Step 3: Implement** the two config headers, the entry, the parser and writer (reusing the mapping reader's
  helpers), the documentation, and the policy header.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[quickterminal]" && out/appleclang-debug/src/contour/contour_gui_test "[config][quickterminal],[config]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/config/Config.cpp src/contour/window/QuickTerminalPolicy_test.cpp src/contour/config/Config_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/HotkeyChord.hpp src/contour/config/QuickTerminalConfig.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/window/QuickTerminalPolicy.hpp src/contour/window/QuickTerminalPolicy_test.cpp
git add src/contour/config/HotkeyChord.hpp src/contour/config/QuickTerminalConfig.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/config/CMakeLists.txt src/contour/window/QuickTerminalPolicy.hpp src/contour/window/QuickTerminalPolicy_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: quick_terminal config, and the quick terminal's states, frames and motion as tables" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `IGlobalHotkey`, the key table, and the macOS backend

**Files:**
- Create: `src/contour/platform/GlobalHotkey.hpp` (interface, `NullGlobalHotkey` inline), `GlobalHotkey.cpp` (the
  factory for hosts with no backend: wholly inside `#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__linux__)`).
- Create: `src/contour/platform/HotkeyKeys.hpp` (Qt-free table).
- Create: `src/contour/platform/CocoaGlobalHotkey.mm` (ARC; the Apple factory); `-framework Carbon` is linked by Step
  6.6, or here if 6.6 has not landed.
- Modify: `src/contour/platform/AppearanceServices.hpp` (`globalHotkey`), `src/contour/test/AppearanceFixtures.hpp`.
- Create: `src/contour/test/HotkeyFixtures.hpp` — `RecordingGlobalHotkey`.
- Modify: `src/contour/platform/CMakeLists.txt`, `src/contour/CMakeLists.txt`.
- Test: new `src/contour/platform/HotkeyKeys_test.cpp` (contour_test), new `CocoaGlobalHotkey_test.mm`
  (contour_gui_test, `if(APPLE)`), `AppearanceSources_test.cpp`.

**Interfaces:**
- Produces (`contour::platform`, `GlobalHotkey.hpp`):

```cpp
/// Why a global hotkey could not be registered.
enum class HotkeyError : uint8_t
{
    Unsupported = 0,  ///< This platform or session has no global hotkeys.
    UnsupportedKey,   ///< The key has no row in HotkeyKeyRows.
    AlreadyTaken,     ///< Another application owns the chord.
    MenuOwned,        ///< The chord is one the macOS menu bar keeps (MenuOwnedKeys); refused before the OS is asked.
    Declined,         ///< The user or the desktop refused it (the Linux portal).
    Failed,           ///< The OS call failed for another reason.
};

/// Called on the GUI thread when the registered chord is pressed; @p activationToken is the desktop's
/// focus-stealing token where one is given (the Linux portal), else empty.
using HotkeyPressed = std::function<void(std::string activationToken)>;

/// One application-wide hotkey. register() replaces any chord registered before.
class IGlobalHotkey
{
  public:
    virtual ~IGlobalHotkey() = default;
    /// @pre GUI (main) thread. @return An error when the chord cannot be bound; nothing is registered then.
    ///      The Linux portal answers later: its result arrives through @p onResult, and register() returns at once.
    [[nodiscard]] virtual std::expected<void, HotkeyError> registerHotkey(config::HotkeyChord const& chord,
                                                                          HotkeyPressed onPressed,
                                                                          std::function<void(std::expected<void, HotkeyError>)> onResult) = 0;
    /// @pre GUI (main) thread. Idempotent.
    virtual void unregisterHotkey() = 0;
};

class NullGlobalHotkey final: public IGlobalHotkey { /* registerHotkey → unexpected(Unsupported), inline */ };
[[nodiscard]] std::unique_ptr<IGlobalHotkey> makeGlobalHotkey();
```

- Produces (`HotkeyKeys.hpp`):
  - `struct HotkeyKeyRow { std::variant<vtbackend::Key, char32_t> key; std::uint16_t macVirtualKey; std::uint16_t windowsVirtualKey; std::string_view xdgName; };`
    and `inline constexpr std::array HotkeyKeyRows` for `A`–`Z`, `0`–`9`, `` ` ``, Space, Return, Tab, Escape,
    F1–F20 and the four arrows; values from HIToolbox's `Events.h` virtual key codes, `WinUser.h`'s virtual keys and
    XKB keysym names (`a`, `grave`, `space`, `Return`, `F12`, `Up`, …).
  - `[[nodiscard]] constexpr HotkeyKeyRow const* hotkeyKeyRow(std::variant<vtbackend::Key, char32_t> const& key) noexcept;`
    (letters match either case).
  - `struct HotkeyModifierRow { vtbackend::Modifier modifier; std::uint32_t macMask; std::uint32_t windowsMask; std::string_view xdgName; };`
    for Control, Alt, Shift and Super (`xdgName` `"CTRL"`, `"ALT"`, `"SHIFT"`, `"LOGO"`; macOS Control is ⌃ and Super
    is ⌘, under `AA_MacDontSwapCtrlAndMeta`).
  - `[[nodiscard]] std::string xdgTrigger(config::HotkeyChord const& chord);` → e.g. `"CTRL+grave"`.
- Produces (`AppearanceServices`): `std::function<std::unique_ptr<IGlobalHotkey>()> globalHotkey = makeGlobalHotkey;`,
  null in both null factories.
- Produces (`test/HotkeyFixtures.hpp`): `RecordingGlobalHotkey` — records `std::vector<config::HotkeyChord> registered`,
  `int unregisters`, holds `std::expected<void, HotkeyError> result {}`, and `void press(std::string token = {}) const`.
- The Cocoa class installs one `kEventClassKeyboard` hot-key-pressed handler on the application event target at the
  first registration, calls `RegisterEventHotKey` with the row's virtual key and the mapped modifier mask, maps an
  "already exists" status to `AlreadyTaken` and others to `Failed`, calls `onResult` synchronously, and unregisters
  and removes the handler in `unregisterHotkey()` and its destructor.

- [ ] **Step 1: Write the failing tests.**
  - `HotkeyKeys_test.cpp`, `[hotkeykeys]`: every row's key is distinct and so is every row's `xdgName`; `hotkeyKeyRow(U'a')
    == hotkeyKeyRow(U'A')`; `hotkeyKeyRow(U'é') == nullptr`; `xdgTrigger({Control|Alt, U'T'}) == "CTRL+ALT+t"`;
    `xdgTrigger({Super, Key::F12}) == "LOGO+F12"`.
  - `AppearanceSources_test.cpp`: both null factories' hotkey answers `Unsupported`.
  - `CocoaGlobalHotkey_test.mm`, `[globalhotkey][cocoa]`: `makeGlobalHotkey()` registers Control+Alt+Shift+F19 (a
    chord nothing binds) without error and `onResult` reports success; `unregisterHotkey()` twice is harmless;
    destroying it after registering leaves no handler (a second instance registers the same chord successfully).
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `GlobalHotkey.hpp`.
- [ ] **Step 3: Implement** the interface, the table, the Cocoa backend, the services field and the fixture.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[hotkeykeys]" && out/appleclang-debug/src/contour/contour_gui_test "[globalhotkey],[appearance-source]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/platform/GlobalHotkey.cpp src/contour/platform/HotkeyKeys_test.cpp src/contour/platform/AppearanceSources_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/GlobalHotkey.hpp src/contour/platform/GlobalHotkey.cpp src/contour/platform/HotkeyKeys.hpp src/contour/platform/HotkeyKeys_test.cpp src/contour/platform/CocoaGlobalHotkey.mm src/contour/platform/CocoaGlobalHotkey_test.mm src/contour/platform/AppearanceServices.hpp src/contour/platform/AppearanceSources_test.cpp src/contour/test/AppearanceFixtures.hpp src/contour/test/HotkeyFixtures.hpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform/GlobalHotkey.hpp src/contour/platform/GlobalHotkey.cpp src/contour/platform/HotkeyKeys.hpp src/contour/platform/HotkeyKeys_test.cpp src/contour/platform/CocoaGlobalHotkey.mm src/contour/platform/CocoaGlobalHotkey_test.mm src/contour/platform/AppearanceServices.hpp src/contour/platform/AppearanceSources_test.cpp src/contour/platform/CMakeLists.txt src/contour/test/AppearanceFixtures.hpp src/contour/test/HotkeyFixtures.hpp src/contour/CMakeLists.txt
git commit -m "platform: one application-wide hotkey behind an interface, a key table, and the macOS backend" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The Windows backend

**Files:**
- Create: `src/contour/platform/Win32GlobalHotkey.cpp` (wholly `#ifdef _WIN32`; the Windows `makeGlobalHotkey()`),
  listed unconditionally in `add_library(contour_platform …)`.
- Create: `src/contour/platform/Win32GlobalHotkey_test.cpp` (wholly `#ifdef _WIN32`), in contour_gui_test.

**Interfaces:**
- Consumes: `IGlobalHotkey`, `HotkeyKeyRows`, `HotkeyModifierRow` (Task 3).
- The class is a `QAbstractNativeEventFilter` installed on the application. `registerHotkey` calls
  `RegisterHotKey(nullptr, id, mask | MOD_NOREPEAT, vk)` on the GUI thread, so `WM_HOTKEY` is posted to that thread's
  queue; the filter answers the `"windows_dispatcher_MSG"` event type (Qt's dispatcher passes thread messages there),
  matches `msg->message == WM_HOTKEY && msg->wParam == id`, and calls `onPressed({})`. `ERROR_HOTKEY_ALREADY_REGISTERED`
  maps to `AlreadyTaken`; other failures to `Failed`. `unregisterHotkey()` and the destructor call `UnregisterHotKey`
  and remove the filter.

**Verified by:** CI `Windows` job (`build.yml` `windows:`), steps `Build` and `Test` (`ctest --preset msvc-release`),
ctest `contour_gui_test`, filter `[globalhotkey][win32]`; and `check_clang_tidy`.

- [ ] **Step 1: Write the failing tests** (`[globalhotkey][win32]`): registering Control+Alt+Shift+F19 succeeds;
  posting `WM_HOTKEY` with the id to the thread (`PostThreadMessageW(GetCurrentThreadId(), …)`) and spinning the event
  loop once calls `onPressed`; a second instance registering the same chord reports `AlreadyTaken` (Review Focus 1);
  after `unregisterHotkey()` the second instance succeeds.
- [ ] **Step 2: Push and see them fail in CI** (the file does not exist yet in the Windows factory): the Windows job's
  `Test` step reports the cases as failing or unknown.
- [ ] **Step 3: Implement** the backend; on non-Windows hosts the file compiles to nothing.
- [ ] **Step 4: Push and see them pass in CI.** Expected: the `Windows` job green, its log listing
  `[globalhotkey][win32]` cases passed.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/Win32GlobalHotkey.cpp src/contour/platform/Win32GlobalHotkey_test.cpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform/Win32GlobalHotkey.cpp src/contour/platform/Win32GlobalHotkey_test.cpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "platform: the global hotkey on Windows through RegisterHotKey" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: The Linux backend, the GlobalShortcuts portal

> **Depends on Step 5b** (the `portal settings adapter (private session bus)` CI step and its fake portal service,
> which this task extends with the GlobalShortcuts interface).

**Files:**
- Create: `src/contour/platform/GlobalShortcutsChannel.hpp` (Qt-free seam with `NullGlobalShortcutsChannel` inline).
- Create: `src/contour/platform/PortalGlobalHotkey.{hpp,cpp}` (Qt-free class over the channel; compiled everywhere,
  so it is tested here against a fake).
- Create: `src/contour/platform/DBusGlobalShortcutsChannel.{hpp,cpp}` (wholly `#ifdef __linux__`; the Linux
  `makeGlobalHotkey()` returns a `PortalGlobalHotkey` over it).
- Test: new `src/contour/platform/PortalGlobalHotkey_test.cpp` (contour_test, a fake channel); the fake portal script
  of Step 5b gains `org.freedesktop.portal.GlobalShortcuts`; new `DBusGlobalShortcutsChannel_test.cpp` (wholly
  `#ifdef __linux__`, contour_gui_test, tag `[globalhotkey][portal]`).

**Interfaces:**
- Produces (`GlobalShortcutsChannel.hpp`):
  - `struct ShortcutBinding { std::string id; std::string description; std::string preferredTrigger; };`
  - `enum class PortalResponse : uint8_t { Success = 0, Cancelled, Other };` (the portal's 0, 1, 2)
  - `class IGlobalShortcutsChannel` with `createSession(std::function<void(PortalResponse, std::string sessionHandle)>)`,
    `bindShortcuts(std::string const& sessionHandle, std::vector<ShortcutBinding> const&, std::function<void(PortalResponse)>)`,
    `closeSession(std::string const& sessionHandle)`, and `onActivated(std::function<void(std::string shortcutId, std::string activationToken)>)`;
    `NullGlobalShortcutsChannel` answers `Other` at once.
- Produces (`PortalGlobalHotkey`): `explicit PortalGlobalHotkey(std::unique_ptr<IGlobalShortcutsChannel> channel);`
  implementing `IGlobalHotkey`: one session per process, created on the first registration; one shortcut id
  `"quick-terminal"`, description "Show or hide the quick terminal", `preferredTrigger = xdgTrigger(chord)`;
  `registerHotkey` returns `{}` at once and reports through `onResult` (`Cancelled` → `Declined`, `Other` → `Failed`);
  `Activated` with that id calls `onPressed(activationToken)`; `unregisterHotkey()` closes the session. A second
  `registerHotkey` with an unchanged chord does nothing (Review Focus 2).
- `DBusGlobalShortcutsChannel` speaks `org.freedesktop.portal.GlobalShortcuts` at `PortalService`/`PortalPath`
  (`PortalCall.hpp`): `CreateSession` and `BindShortcuts` with `handle_token`/`session_handle_token`, the `Response`
  signal on the returned request path, and the `Activated` signal; calls are asynchronous with
  `DBusCallTimeoutMilliseconds`. It never opens the session bus before the `QGuiApplication` exists (Step 5b's rule).

**Verified by:** CI Ubuntu jobs (`ubuntu_2404_cc_matrix`, GCC 15 and Clang 22), the `tests` step (contour_test
`[portalhotkey]`) and Step 5b's gating `portal settings adapter (private session bus)` step running contour_gui_test
`[globalhotkey][portal]`; and `check_clang_tidy`.

- [ ] **Step 1: Write the failing tests.**
  - `PortalGlobalHotkey_test.cpp`, `[portalhotkey]`, with a fake channel: registering sends one `createSession`, then
    one `bindShortcuts` with id `"quick-terminal"` and trigger `"CTRL+grave"`; a `Cancelled` bind response reports
    `Declined` (Review Focus 1); `Activated("quick-terminal", "tok")` calls `onPressed("tok")`; another id calls
    nothing; registering the same chord again sends nothing; a changed chord sends one new `bindShortcuts`;
    `unregisterHotkey()` closes the session.
  - `DBusGlobalShortcutsChannel_test.cpp`, `[globalhotkey][portal]`: against the fake portal on a private bus, a
    session is created, a bind succeeds, and an emitted `Activated` reaches the callback.
- [ ] **Step 2: Run the fake-channel tests here and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`. Expected: compile error.
- [ ] **Step 3: Implement** the seam, the portal class, the D-Bus channel and the fake portal's interface.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/contour/contour_test "[portalhotkey]"`
  here; push and read the Ubuntu jobs' logs for `[globalhotkey][portal]`. Expected: PASS here, green in CI.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/platform/PortalGlobalHotkey.cpp src/contour/platform/PortalGlobalHotkey_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/GlobalShortcutsChannel.hpp src/contour/platform/PortalGlobalHotkey.hpp src/contour/platform/PortalGlobalHotkey.cpp src/contour/platform/PortalGlobalHotkey_test.cpp src/contour/platform/DBusGlobalShortcutsChannel.hpp src/contour/platform/DBusGlobalShortcutsChannel.cpp src/contour/platform/DBusGlobalShortcutsChannel_test.cpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt .github/workflows/build.yml
git add src/contour/platform/GlobalShortcutsChannel.hpp src/contour/platform/PortalGlobalHotkey.hpp src/contour/platform/PortalGlobalHotkey.cpp src/contour/platform/PortalGlobalHotkey_test.cpp src/contour/platform/DBusGlobalShortcutsChannel.hpp src/contour/platform/DBusGlobalShortcutsChannel.cpp src/contour/platform/DBusGlobalShortcutsChannel_test.cpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "platform: the global hotkey on Linux through the GlobalShortcuts portal" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

  Also stage the fake portal script by name (the path Step 5b gives it).

---

### Task 6: The quick terminal window

> **Depends on Step 2** (`ChromeDensity::Minimal`, `ChromeInputs::density`), **Step 1d** (the macOS window's alpha
> format, which the rounded corners need), **Step 3c** (`window::NoticeModel` and its `NoticeRows`, for the hotkey
> failure) and on **Task 1's ruling S1**.

**Files:**
- Create: `src/contour/window/WindowRole.hpp` (Qt-free): `WindowRole`, `WindowLevel`, `WindowRoleRows`.
- Create: `src/contour/platform/QuickTerminalPanel.{hpp,cpp}` (interface; `QtQuickTerminalPanel`, the non-Apple
  default, in the `.cpp` wholly inside `#ifndef __APPLE__`), `CocoaQuickTerminalPanel.mm` (ARC, per S1).
- Create: `src/contour/window/QuickTerminalController.{hpp,cpp}`.
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}` — the services, the child context with its `ChromeStyleHost`,
  `openQuickTerminal()`, the controller, re-registration on reload.
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}` (`stageWindowRole`, consumed by
  `createWindowController`; skip `WindowPositionStore` for the quick role, R-g),
  `src/contour/window/WindowController.{hpp,cpp}` (`role()`, `Q_PROPERTY(int windowFlags … CONSTANT)`,
  `Q_PROPERTY(int windowRole … CONSTANT)`), `src/contour/qml/Main.qml` (`flags: win ? win.windowFlags : Qt.Window`;
  the quick role's bottom-corner clip from `radius.quickTerminal`), `config/ChromeTokenGroups.hpp` (the token).
- Modify: Step 3c's `window/NoticeModel.hpp` — `NoticeKind::HotkeyUnavailable` (Toast, glyph `!`, 6000 ms, no action)
  and `hotkeyUnavailableNoticeText(platform::HotkeyError)`.
- Modify: `src/contour/display/ShortcutOverride.hpp` — `isMenuOwnedChord` (R-f).
- Create: `src/contour/test/QuickTerminalFixtures.hpp` — `RecordingQuickTerminalPanel`.
- Test: new `src/contour/window/WindowRole_test.cpp` (contour_test), new
  `src/contour/window/QuickTerminalController_test.cpp` (contour_gui_test), new
  `src/contour/platform/CocoaQuickTerminalPanel_test.mm` (`[display]`, `if(APPLE)`), `src/contour/display/ShortcutOverride_test.cpp`.

**Interfaces:**
- Consumes: `QuickTerminalConfig`, `HotkeyChord` (Task 2); `quickTerminalStep`, `quickTerminalFrames`,
  `quickTerminalAnimation` (Task 2); `IGlobalHotkey`, `RecordingGlobalHotkey` (Task 3).
- Produces (`WindowRole.hpp`): `enum class WindowRole : uint8_t { Normal = 0, QuickTerminal };`,
  `enum class WindowLevel : uint8_t { Normal = 0, Panel };`,
  `struct WindowRoleRow { WindowRole role; WindowLevel level; ChromeDensityOverride density; PositionMemory memory; };`
  with `enum class PositionMemory : uint8_t { Remembered = 0, Placed };` and
  `enum class ChromeDensityOverride : uint8_t { Configured = 0, Minimal };`, and
  `[[nodiscard]] constexpr WindowRoleRow const& windowRoleRow(WindowRole) noexcept;`. `WindowController.cpp` maps
  `WindowLevel::Panel` to the flags of ruling S1 (by default `Qt::Tool | Qt::FramelessWindowHint`).
- Produces (`QuickTerminalPanel.hpp`): `class IQuickTerminalPanel { virtual void adopt(QWindow&) = 0; virtual void present(QWindow&) = 0; virtual void dismiss(QWindow&) = 0; };`
  `@pre` GUI thread, each; `QtQuickTerminalPanel` (`present`: `show()`, `raise()`, `requestActivate()`; `dismiss`:
  `hide()`) and `makeQuickTerminalPanel()`; `AppearanceServices::quickPanel`, which both null factories set to
  `NullQuickTerminalPanel` (inline no-ops).
- Produces (`display/ShortcutOverride.hpp`): `[[nodiscard]] inline bool isMenuOwnedChord(config::HotkeyChord const& chord, command::MenuBarPlacement placement);`
  — true under `Global` when the chord, as a `QKeyCombination`, equals a `MenuOwnedKeys` row (R-f).
- Produces (`QuickTerminalController`, `QObject`): constructed with `(platform::IGlobalHotkey&, platform::IQuickTerminalPanel&, config::QuickTerminalConfig, config::MotionTokens, std::function<QWindow*()> createWindow, std::function<window::ScreenRect()> screenUnderPointer, std::function<void(platform::HotkeyError)> onHotkeyError)`;
  `void reconfigure(config::QuickTerminalConfig)` (live reconfiguration, documented; registers only on a changed
  chord, Review Focus 2); `[[nodiscard]] window::QuickTerminalState state() const noexcept`; `void press(std::string
  activationToken)` (what the hotkey calls); it watches its window's `activeChanged` (FocusLost/FocusGained),
  `destroyed` (LastTabClosed) and its one `QPropertyAnimation` (`y`, or `opacity` for `Fade`; AnimationDone). On a
  screen change while shown it re-places the frame on the primary screen (Review Focus 5). `ScreenRect` is Task 2's.
- Produces (`ContourGuiApp`): `void openQuickTerminal();` (stages `WindowRole::QuickTerminal`, creates a child
  `QQmlContext` of the root context holding a second `window::ChromeStyleHost` built from `chromeInputs()` with
  `density = ChromeDensity::Minimal`, and creates `Main` in it with `QQmlComponent`); `refreshChromeStyle()` updates
  both hosts.

- [ ] **Step 1: Write the failing tests.**
  - `WindowRole_test.cpp`, `[windowrole]`: one row per role in order; `QuickTerminal` → `Panel`, `Minimal`, `Placed`;
    `Normal` → `Normal`, `Configured`, `Remembered`.
  - `QuickTerminalController_test.cpp`, `[quickterminal][controller]`, a `TestApp` with `RecordingGlobalHotkey` and
    `RecordingQuickTerminalPanel`, `quick_terminal.hotkey` set:
    - "the hotkey creates one Minimal quick window at the top": `press()` → one window, its controller's `windowRole`
      is QuickTerminal, its context's `chromeStyle.density == "minimal"`, its geometry `quickTerminalFrames(screen).shown`
      once the animation ends, `panel.presents == 1`; `press()` again → `Hiding`, then `Hidden`, still one window.
    - "a press during the slide reverses it" (Review Focus 3).
    - "focus loss hides it, unless configured to stay": deactivate the window → `Hidden`; with `Stay` → `Shown`, and
      `press()` focuses it.
    - "closing its last tab ends it, and the next press makes a new one".
    - "an unchanged chord is not registered again" (Review Focus 2): `reconfigure` with the same config →
      `registered.size() == 1`; a new chord → `unregisters == 1`, `registered.size() == 2`; no chord → `unregisters == 2`.
    - "a taken hotkey posts a notice" (Review Focus 1): `result = unexpected(AlreadyTaken)` → the focused window's
      `NoticeModel` shows `hotkeyUnavailableNoticeText(AlreadyTaken)`.
    - "the quick window is never remembered": closing it writes nothing to the `WindowPositionStore` double.
    - "a menu-owned chord is refused before the OS is asked": under a `Global` menu bar, ⌘Q → `onHotkeyError(MenuOwned)`
      and `registered.empty()`.
  - `ShortcutOverride_test.cpp`, `[shortcutoverride]`: `isMenuOwnedChord({Super, U'Q'}, Global)`, `({Super|Alt, U'H'}, Global)`
    true; `({Super, U'Q'}, None)` and `({Control, U'`'}, Global)` false.
  - `CocoaQuickTerminalPanel_test.mm`, `[quickterminal][cocoa][display]` (skipped offscreen): after `adopt`, the
    `NSWindow` behind the `QWindow` has the style and collection behaviour of S1; after `present` it is key while
    `NSWorkspace.frontmostApplication` is unchanged (Review Focus 4).
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `WindowRole.hpp`.
- [ ] **Step 3: Implement** the role table, the panel interface and its two backends, the controller, the app's
  context and wiring, the manager's staging, the controller's properties, `Main.qml`'s flags and corner clip, the
  token and the notice row.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[windowrole],[chrometokens],[notices]" && out/appleclang-debug/src/contour/contour_gui_test "[quickterminal][controller],[shortcutoverride],[contour][gui][qml]"`,
  then `CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[quickterminal][cocoa]"`. Expected:
  PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/QuickTerminalController.cpp src/contour/platform/QuickTerminalPanel.cpp src/contour/ContourGuiApp.cpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.cpp src/contour/window/WindowRole_test.cpp src/contour/window/QuickTerminalController_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/WindowRole.hpp src/contour/window/WindowRole_test.cpp src/contour/platform/QuickTerminalPanel.hpp src/contour/platform/QuickTerminalPanel.cpp src/contour/platform/CocoaQuickTerminalPanel.mm src/contour/platform/CocoaQuickTerminalPanel_test.mm src/contour/window/QuickTerminalController.hpp src/contour/window/QuickTerminalController.cpp src/contour/window/QuickTerminalController_test.cpp src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/NoticeModel.hpp src/contour/config/ChromeTokenGroups.hpp src/contour/platform/AppearanceServices.hpp src/contour/test/AppearanceFixtures.hpp src/contour/test/QuickTerminalFixtures.hpp src/contour/display/ShortcutOverride.hpp src/contour/display/ShortcutOverride_test.cpp
editorconfig-checker src/contour/qml/Main.qml src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt
git add src/contour/window/WindowRole.hpp src/contour/window/WindowRole_test.cpp src/contour/platform/QuickTerminalPanel.hpp src/contour/platform/QuickTerminalPanel.cpp src/contour/platform/CocoaQuickTerminalPanel.mm src/contour/platform/CocoaQuickTerminalPanel_test.mm src/contour/window/QuickTerminalController.hpp src/contour/window/QuickTerminalController.cpp src/contour/window/QuickTerminalController_test.cpp src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/NoticeModel.hpp src/contour/config/ChromeTokenGroups.hpp src/contour/platform/AppearanceServices.hpp src/contour/test/AppearanceFixtures.hpp src/contour/test/QuickTerminalFixtures.hpp src/contour/display/ShortcutOverride.hpp src/contour/display/ShortcutOverride_test.cpp src/contour/qml/Main.qml src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt
git commit -m "contour: the hotkey slides a Minimal quick terminal down from the top, and hides it again" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The Settings row, the snapshot, and the Terminal row

> **Depends on Step 4b** (`window/SettingsFields.{hpp,cpp}`, `SettingsGroup`, `SettingsGroupRows`, the kit controls and
> one save model) and **Step 1e** (the snapshot driver).

**Files:**
- Modify: `src/contour/window/SettingsFields.{hpp,cpp}` — `SettingsGroup::GeneralQuickTerminal` after `GeneralWindows`,
  its `SettingsGroupRows` row ("Quick Terminal", Global scope), and two descriptors: `quick_terminal.hotkey` (control
  `Shortcut`, label "Hotkey", help "Shows or hides a terminal from anywhere. Not set by default.") and
  `quick_terminal.hide_on_focus_loss` (Switch, "Hide when it loses focus").
- Create: `src/contour/qml/KitShortcutField.qml` (add to `_qml_files`): shows the chord with `ShortcutStyle` glyphs or
  "None"; "Record" captures the next chord (accepting its `ShortcutOverride`, so menu chords reach it), Escape cancels,
  Delete clears; shows the parser's or the backend's error text below. `Accessible.role: Accessible.Button`,
  `Accessible.name` "Hotkey, <chord or None>".
- Modify: `src/contour/window/SettingsController.{hpp,cpp}` — `Q_INVOKABLE QString setHotkey(int key, int modifiers)`
  (returns the error text or empty; QML carve-out) and `clearHotkey()`.
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`, `MockMainController.hpp` — `SnapshotScript::QuickTerminal`
  (`"quick-terminal"`: one tab, one pane, the quick role, Minimal density; every field named) and a `SnapshotWindowMode`
  value for the panel frame (Step 1c's enum).
- Test: `src/contour/window/SettingsController_test.cpp`, `src/contour/test/QmlComponents_test.cpp`,
  `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/window/SettingsSections_test.cpp` (Step 4b's).

**Interfaces:**
- Consumes: `config::HotkeyChord`, `HotkeyChordError` (Task 2); `QuickTerminalController::reconfigure` (Task 6) is
  reached through the app's reload.

- [ ] **Step 1: Write the failing tests.**
  - `SettingsSections_test.cpp`, `[settingssections]`: `GeneralQuickTerminal` is in General with its two rows.
  - `SettingsController_test.cpp`, `[settings][quickterminal]`: `setHotkey(Qt::Key_QuoteLeft, Qt::ControlModifier)`
    stages `quick_terminal: { hotkey: { mods: [Control], key: '`' } }` into `settings.yml`; `setHotkey(Qt::Key_F12, 0)`
    returns the `NoModifier` text and stages nothing; on a macOS host `setHotkey(Qt::Key_Q, Qt::MetaModifier)` returns
    the `MenuOwned` text; `clearHotkey()` removes the key.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][kit][shortcut]`: `KitShortcutField` loads without warnings on both
    rows, shows "None" when empty, and its accessible name carries the chord.
  - `ChromeSnapshotPlan_test.cpp`, `[snapshot]`: `scriptRow(SnapshotScript::QuickTerminal).density == ChromeDensity::Minimal`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `SettingsGroup::GeneralQuickTerminal`.
- [ ] **Step 3: Implement** the group, the rows, the control, the invokables and the script.
- [ ] **Step 4: Run them and see them pass, on both rows.** Run:

```bash
out/appleclang-debug/src/contour/contour_test "[settingssections],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[settings][quickterminal],[contour][gui][qml][kit][shortcut],[settings]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][kit][shortcut]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/SettingsFields.cpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/SettingsFields.hpp src/contour/window/SettingsFields.cpp src/contour/window/SettingsController.hpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp src/contour/window/SettingsSections_test.cpp src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/KitShortcutField.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt
git add src/contour/window/SettingsFields.hpp src/contour/window/SettingsFields.cpp src/contour/window/SettingsController.hpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp src/contour/window/SettingsSections_test.cpp src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlComponents_test.cpp src/contour/qml/KitShortcutField.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt
git commit -m "contour: the quick terminal's hotkey and focus-loss switch in Settings › General" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts .github`, apply its
  findings, re-run the filters of every task it touches, and commit once with the Signed-off-by line, staging only the
  files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[quickterminal],[hotkeykeys],[portalhotkey],[windowrole],[chrometokens],[notices],[settingssections],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[config][quickterminal],[globalhotkey],[appearance-source],[quickterminal][controller],[shortcutoverride],[settings][quickterminal],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][kit][shortcut],[contour][gui][qml]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS here; the Windows and Ubuntu jobs green for Tasks 4 and 5.
- [ ] **Step 3: Snapshots, on this Mac's display:**

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot],[quickterminal][cocoa]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`. Look at
  `quick-terminal.*`: no tab bar (Minimal, one tab), bottom corners rounded 14 on MacOS, square with the hard shadow
  under ContourTui.
- [ ] **Step 4: Owner manual checks**, batched per phase (listed in the ledger, not awaited), on
  `out/appleclang-debug/src/contour/contour.app`:
  - Bind `quick_terminal.hotkey` in Settings › General to ⌃\`. From Safari, press it: the panel slides down full width
    below the menu bar, 40 % high; typing reaches it while Safari stays frontmost (its menu bar stays); press again:
    it slides up and keys go back to Safari.
  - Click Safari while it is shown: it hides; with the switch off it stays.
  - Bind ⌘Space: the notice says it is taken. Bind ⌘Q: Settings refuses it.
  - System Settings › Accessibility › Reduce Motion: it fades instead of sliding.
  - With a second display, move the pointer there and press: it opens on that display.
  - Edit an unrelated key in contour.yml: the hotkey keeps working and nothing is registered again (the log).
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release: "Adds a quick
  terminal: a global hotkey, set in Settings › General and unbound by default, slides a terminal down from the top of
  the screen and hides it again, and hides it when it loses focus unless turned off; on macOS the app you came from
  stays in front. Adds `quick_terminal` (`hotkey`, `hide_on_focus_loss`)."
- [ ] **Step 6:** Mark Step 6.7 done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-7.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the terminal paths; one animation per show or hide.
  - **Risk:** medium. A new window kind with its own flags and context, three OS hotkey backends of which two are
    verified only in CI, and an AppKit panel; unbound by default, so nothing changes until a user binds it.
  - **Coverage:** `[quickterminal]`, `[hotkeykeys]`, `[portalhotkey]`, `[windowrole]` tables; the controller cases;
    the Cocoa, Win32 and portal backends; the Settings row; one snapshot script.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Which screen (R-b).** *Default: the screen under the pointer.* The alternatives are the screen of the last focused
   Contour window, or always the primary one.
2. **Wayland (R-d).** *Default: the panel opens where the compositor puts it, fading in, with no slide.* The alternative
   is a `wlr-layer-shell` surface, which only some compositors offer and Qt does not.
3. **Quitting (R-e).** *Default: unchanged; a hidden quick terminal does not keep the app alive.* The alternative keeps
   Contour running while a hotkey is bound (a menu-bar-less background app).
4. **The spike's ruling (R-a).** *Default: whichever candidate takes keys while the previous app stays frontmost,
   expected to be B (`Qt::Tool` plus an AppKit non-activating style), not the spec's literal flags.*

## Spec gaps found

- `Qt::WindowDoesNotAcceptFocus` keeps a window from taking keys; the spec's flags and "a terminal" conflict (R-a).
- No screen choice, no Wayland behaviour (no client placement), no quit rule, no re-registration rule, and no failure
  path for a taken chord are given.
- "Set in General" needs a control Step 4b does not build (a shortcut recorder, Task 7).
- The corner radius (14) and shadow are on the canvas only; Fluent, ContourLinux and Terminal values are this plan's.
- Nothing says whether the quick terminal may hold tabs; it may, as any window, and Minimal hides the bar for one.
