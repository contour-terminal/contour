# Native Redesign — Step 6.8 (Keyboard Reaches the Chrome: ⌃F6 Cycles Terminal → Tabs → Toolbar) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25). Line numbers are at HEAD `661b6029`. Every earlier step runs first:
4a is done, 1c is in progress, and 1d, 2, 3a–3f, 4b, 5a and 5b are written. Where this step uses code those steps
add, the task names it as their plans do and carries a **Depends on Step <id>** note. A pre-flight scan re-verifies
every reference before Task 1.

**Goal:** From the keyboard alone, ⌃F6 (Ctrl+F6 on Windows and Linux) moves focus from the terminal to the tab well,
then to the title band's toolbar, then back; ⌃F5 on macOS goes straight to the toolbar; a bare F6 always reaches the
shell. In the tab well ←/→ move, Return activates, ⌫ closes (asking first if a program runs), ⌥←/→ reorders, Escape
returns to the terminal; the focus ring shows only for keyboard focus; and a test fails when a default chord is bound
twice.

**Architecture:**
- **The defaults are checked as data.** `config/BindingCollisions.hpp` finds every pair of default bindings that share
  an input, modifiers and click count under modes that can hold at once — the pairs `config::apply()`'s first-match
  lookup makes dead. A `[bindings]` table test runs it over every edition; the dead pairs it finds today are folded.
- **Three catalogue actions, bound per edition.** `FocusNextChromeArea`, `FocusPreviousChromeArea` and `FocusToolbar`
  join the action catalogue; the Native2026 bindings carry them (macOS ⌃F6, ⇧⌃F6, ⌃F5; elsewhere Ctrl+F6, Ctrl+Shift+F6);
  Classic gains none (§2.7).
- **Where focus goes is a table.** `window/ChromeFocus.hpp` holds the cycle order and skips an area that is not shown;
  `window/ChromeKeys.hpp` maps each key in each area to a command; `closeConfirmationFor()` decides whether ⌫ asks.
  `WindowController` interprets them and publishes `chromeFocusArea` and `focusedTabIndex`; QML renders the ring.
- **The chrome forwards what it does not handle.** A chrome item's `Keys` handler hands an unhandled chord to
  `WindowController::chromeKey()`, which looks it up in the configured bindings first, so ⌃F6 cycles from inside the
  chrome with the user's own chord.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), QML, Catch2 v3, CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.8 binds, with what
it cites: §1 (principles; 3 pure decisions), §2.1 (token rows), §2.5–§2.6 (snapshots; "every new control sets
`Accessible.role` and `Accessible.name`"), §2.7 (`DefaultsEdition`). The design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md`. The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `moments` (10 · Keyboard reaches the chrome),
`kit-components` (ToolbarButton: "Focus ring = system keyboard focus color, 3 pt, only for keyboard focus") and `tui`
are in `.superpowers/design/canvas/project/`. The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Step 1c lists "the keyboard focus ring on toolbar
buttons" as this step's.

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

- **The spec, verbatim (§3.16, row 6.8):** "Keyboard to chrome | ⌃F6 / Ctrl+F6 cycles focus: terminal → tabs → toolbar.
  A test checks that every chord in the default bindings is unique | none".
- **Canvas values, exact** (`moments` 10): "⌃F6 (macOS: ⌃F5 for the toolbar, ⌃F6 between areas; Windows and Linux:
  Ctrl+F6) cycles focus terminal → tabs → toolbar → back. A bare F6 always goes to the shell — mc, vim and htop use it.
  In the tab well ←/→ move, Return activates, ⌫ closes (asks if a process runs), ⌥←/→ reorders. The system focus ring
  (3 pt) only appears for keyboard focus. Every chord is unique: a unit test fails when one is bound to two actions."
  The board draws the ring as a 3-pt outset in the accent at 60 % (`0 0 0 3px rgba(10,132,255,0.6)`) around the
  focused pill.
  - New token `size.focusRing`: 3 MacOS, 2 Fluent, 2 ContourLinux, 0 Terminal (cells cannot draw an outset).
  - New colour roles, appended after the last `ChromeColorRole` at execution: `FocusRing` (native: the accent at 60 %;
    Terminal: `Fixed` at 0 %) and `FocusFill` (native: `Fixed` at 0 %; Terminal: `schemeColor(Selection)`), so the
    Terminal row shows keyboard focus as a selection-coloured cell run with no layout switch (§1.2).
  - Confirmation copy (⌫ on a busy tab): title `Close “<tab label>”?`, text `“<program>” is still running in this tab.`,
    buttons "Close" (destructive) and "Cancel" (default).
  - Actions' palette titles: "Focus Next Area", "Focus Previous Area", "Focus Toolbar".
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the SDD ledgers of 1c, 1d, 2, 3a–3f, 4b, 5b):
  - Any new default goes through `DefaultsEdition`: the three bindings are rows of the Native2026 bindings only
    (`makeDefaultInputMappings(DefaultsEdition)`, `Config.hpp:1283`).
  - The Step 1b menu-owned macOS chords (⌘Q ⌘, ⌘H ⌥⌘H ⌘M, `MenuOwnedKeys`) stay the menu's; a default binding never takes
    one (Task 1's test).
  - Every catalogue action is reachable from a menu or is a `MenuReachabilityExemptions` row with a reason
    (`command/MenuBarTable.hpp:114-130`, ruling B2/R-g).
  - The terminal claims every `ShortcutOverride` (1b R-j); this step adds no QML `Shortcut` and no `QShortcut`.
  - No colour or size literal in chrome QML; a new file's baseline is 0. QML compares `int` properties, never enums
    through `chromeStyle`.
  - Every visual surface is a snapshot row; every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`). TestApp is Classic (R14) and passes `nullAppearanceServices()`; a
    Native2026 case loads its bindings explicitly.
  - `enum class` over `bool`, zero the default; carve-outs are QML-facing `Q_PROPERTY`/`Q_INVOKABLE` booleans.
  - No OS service is reached here, so no platform seam and no Windows- or Linux-only code; the CI jobs on Windows and
    Linux run the same tests.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven tables (`ChromeAreaCycle`, `ChromeKeyRows`, the edition bindings, the
  exemptions), configuration at construction, Doxygen on every new public declaration, `.hpp` with `#pragma once`,
  zero warnings, no `NOLINT`; `slots`, `signals` and `emit` are never identifiers.
- **Build** with every core, configured `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never two builds in one tree at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding; format with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`; check `.qml`, `.txt` and
  `CMakeLists.txt` with `editorconfig-checker <files>`. **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value;
  every table row names every field (`ChromeTokenRows`, `SnapshotScripts`, `ChromeKeyRows`, the catalogue entries).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Qt-free stays Qt-free:** `config/BindingCollisions.hpp`, `window/ChromeFocus.hpp` and `window/ChromeKeys.hpp`
  include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, never `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 5).

## Review Focus

1. **A bare F6 in `mc`, `vim` or `htop`.** Expected: it reaches the program as `CSI 17 ~` in every edition and with
   the chrome never focused; only ⌃F6 (and ⇧⌃F6, ⌃F5) moves focus. Pinned in Task 1 ("a bare F6 is bound in no
   edition") and Task 3 ("a bare F6 is written to the PTY").
2. **Focus stranded in chrome that is not there.** One tab under `tab_bar: multiple` has no tab well; a window with no
   toolbar item; the tab well vanishing while focused (the last other tab closes). Expected: the cycle skips what is
   not shown, and focus returns to the terminal when its area disappears. Pinned in Task 2 (`nextChromeArea` skips
   absent areas) and Task 3 ("the tab well closing under focus returns it to the terminal").
3. **The ring after a click, and the way back.** Expected: clicking a tab never shows the ring; Escape in any chrome
   area, or a click in the terminal, returns focus there and hides the ring; keys typed while the ring shows never
   reach the shell. Pinned in Task 3 and Task 4 ("a pointer press shows no ring").
4. **⌫ on a tab running a build.** Expected: a dialog asks first; Cancel keeps the tab and the focus; an idle shell's
   tab closes at once; a pane whose program cannot be known (a remote pane) does not ask. Pinned in Task 3
   (`closeConfirmationFor`) and Task 4 (the dialog).
5. **Two bindings on one chord.** Today the defaults bind Ctrl+C in Select|Insert to both `CopySelection` and
   `CancelSelection` (`Config.hpp:1561-1578`), and Ctrl+V likewise (`:1579-1596`); `config::apply()` returns the first
   match (`:320-327`), so the second never runs from `Config{}`, while a generated `contour.yml` merges them through
   `appendOrCreateBinding` (`Config.cpp:4085`). Expected: the test finds such pairs, and the defaults behave as the
   generated file does. Pinned in Task 1.

Also pinned, beyond the five: ⌃F6 cycles from inside the chrome too (Task 3); ⌥←/→ at either end does nothing (Task 2);
the new actions are exempt from menu reachability with a reason (Task 2).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. "Unique" means "no two default bindings can fire for one key press".** Two mappings collide when they share the
  input kind, the input, the modifiers and the click count, and their modes overlap (no `MatchModes` flag is required
  on in one and off in the other). Identical-mode pairs that exist to run two actions become one mapping whose action
  list runs both, in the order `appendOrCreateBinding` gives a generated config (owner, 3). Character and key inputs
  are different inputs; mouse bindings are checked too.
- **R-b. Which editions (owner, 1).** Native2026 only, so a Classic user's Ctrl+F6 still reaches the shell (§2.7).
- **R-c. macOS chords.** ⌃F6 next area, ⇧⌃F6 previous, ⌃F5 toolbar (the canvas; AppKit's own "move focus to the
  toolbar" and "to the next window" chords are these, and our toolbar is drawn by QML, so the app must answer them).
  Elsewhere Ctrl+F6 and Ctrl+Shift+F6, and no toolbar chord (the canvas names none). macOS's Keyboard Navigation
  setting may route ⌃F5/⌃F6 to the system first; the Finish checks record whether they reach Contour.
- **R-d. Areas and their order.** `Terminal → Tabs → Toolbar → Terminal`. "Tabs" is the tab well (Step 1c's pills, or
  the Flat strip); "Toolbar" is the band's trailing items in order: the HostChip, the drift chip, then the toolbar
  buttons (Step 1c Task 7). An area with nothing shown is skipped.
- **R-e. In each area.** Tabs: ←/→ move the focused tab (no wrap), Home/End jump, Return and Space activate, ⌫ and
  Delete close, ⌥←/→ reorder (the existing `MoveTabToLeft`/`MoveTabToRight` semantics), Escape returns. Toolbar: ←/→
  move, Return and Space press, Escape returns. Entering Tabs focuses the active tab; entering Toolbar its first item.
- **R-f. "Asks if a process runs" (owner, 2).** A pane is `Running` when its foreground process is known and is not its
  shell (`vtpty::Process::foregroundProcessName()`, compared with the profile shell's base name, a leading `-` of a
  login shell removed); `Unknown` (a remote pane, Windows, no answer) does not ask.
- **R-g. The ring shows only for keyboard focus.** The controller sets `chromeFocusArea` only from the three actions and
  from keys inside the chrome; any pointer press clears it. QML draws the ring when an item has `activeFocus` and its
  area is the controller's `chromeFocusArea`, so no Qt focus-reason API is needed.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/BindingCollisions.hpp` (create); `config/Config.hpp` (modify) | Collisions; folded defaults | 1 |
| `src/contour/config/Actions.{hpp,cpp}`, `config/Config.hpp`, `command/MenuBarTable.hpp` (modify); `window/ChromeFocus.hpp` (create) | Actions, bindings, the cycle | 2 |
| `src/contour/window/ChromeKeys.hpp` (create); `window/WindowController.*`, `session/TerminalSession.*`, `session/TerminalSessionManager.*` (modify) | Keys per area, close confirmation, wiring | 3 |
| `src/contour/qml/TabStrip.qml`, `TabItem.qml`, `TitleBar.qml`, `ToolbarButton.qml`, `HostChip.qml`, `Main.qml` (modify); `qml/FocusRing.qml` (create); tokens; snapshots | Rendering | 4 |

---

### Task 1: No default chord fires twice

**Files:**
- Create: `src/contour/config/BindingCollisions.hpp` (header-only, Qt-free).
- Modify: `src/contour/config/Config.hpp:1283-1700` (`makeDefaultInputMappings`) — fold every pair the test reports.
- Test: new `src/contour/config/BindingCollisions_test.cpp` (contour_test); `src/contour/display/ShortcutOverride_test.cpp`
  (contour_gui_test).

**Interfaces:**
- Produces (`contour::config`):
  - `[[nodiscard]] constexpr bool modesOverlap(vtbackend::MatchModes a, vtbackend::MatchModes b) noexcept;` — false iff
    some flag is `Enabled` in one and `Disabled` in the other.
  - `enum class BindingKind : uint8_t { Key = 0, Char, Mouse };`
  - `struct BindingCollision { BindingKind kind = BindingKind::Key; std::size_t first = 0; std::size_t second = 0; std::string chord {}; };`
    — the two indices into that kind's vector, and the chord as `std::format` spells the modifiers and input.
  - `[[nodiscard]] std::vector<BindingCollision> findBindingCollisions(InputMappings const& mappings);` (`inline`).
- Consumes (`display/ShortcutOverride.hpp`): Step 1b's config-load check for a binding that takes a `MenuOwnedKeys`
  chord under a `Global` menu bar (ruling I3; its name is re-verified at pre-flight).

- [ ] **Step 1: Write the failing tests.**
  - `BindingCollisions_test.cpp`, `[bindings]`:
    - "modesOverlap": `{}` with `{Insert}` overlaps; `{Search on}` with `{Search off}` does not; `{Select on, Insert on}`
      with `{Select on}` overlaps.
    - "findBindingCollisions finds a crafted pair, and only that": two Ctrl+C char mappings with equal modes → one
      collision `{Char, 0, 1, …}`; the same with Search on/off → none; two mouse bindings differing only in click count
      → none.
    - "no default chord fires twice" (Review Focus 5): for every `DefaultsEdition` (`configEnumValues<DefaultsEdition>()`),
      `findBindingCollisions(defaultInputMappings(edition)).empty()`, with each collision's chord in the failure text.
    - "a bare F6 is bound in no edition" (Review Focus 1): no key mapping of any edition has `Key::F6` with
      `Modifier::None`.
    - "copying with Ctrl+C still clears the selection": the Native2026 defaults' Ctrl+C in Select|Insert runs
      `CopySelection` then `CancelSelection`, as the generated config does.
  - `ShortcutOverride_test.cpp`, `[shortcutoverride]`: for both editions, Step 1b's menu-owned check reports no default
    binding on a `Global` host.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test` and
  `out/appleclang-debug/src/contour/contour_test "[bindings]"`. Expected: "no default chord fires twice" FAILS, listing
  at least the Ctrl+C and Ctrl+V pairs. Record the full list in the ledger.
- [ ] **Step 3: Implement** the header, then fold each reported pair: identical modes → one mapping with both actions
  in file order; overlapping but different modes → make them disjoint as the pair's intent reads, each change recorded
  in the ledger with its reason.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[bindings],[actions]" && out/appleclang-debug/src/contour/contour_gui_test "[shortcutoverride],[config]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/config/BindingCollisions_test.cpp src/contour/display/ShortcutOverride_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/BindingCollisions.hpp src/contour/config/BindingCollisions_test.cpp src/contour/config/Config.hpp src/contour/display/ShortcutOverride_test.cpp
git add src/contour/config/BindingCollisions.hpp src/contour/config/BindingCollisions_test.cpp src/contour/config/Config.hpp src/contour/display/ShortcutOverride_test.cpp src/contour/CMakeLists.txt
git commit -m "config: a test fails when a default chord can fire two bindings; the dead pairs are folded" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: Three focus actions, their Native2026 chords, and the cycle

**Files:**
- Modify: `src/contour/config/Actions.hpp` (beside `MoveTabToLeft`, `:114`, the variant `:239`, the documentation
  `:587`, the catalogue `:856`), `Actions.cpp` if the catalogue needs it.
- Modify: `src/contour/config/Config.hpp` — the Native2026 rows of `makeDefaultInputMappings` (R-b, R-c), under
  `#ifdef __APPLE__` for the macOS chords as `Native2026FontSize` is.
- Modify: `src/contour/command/MenuBarTable.hpp:121-130` — three `MenuReachabilityExemptions` rows, reason "moves
  keyboard focus into the window's chrome".
- Create: `src/contour/window/ChromeFocus.hpp` (header-only, Qt-free); list it in `_header_files`.
- Test: `src/contour/config/Actions_test.cpp`, `src/contour/command/MenuBarTable_test.cpp`,
  `src/contour/config/BindingCollisions_test.cpp`, new `src/contour/window/ChromeFocus_test.cpp` (all contour_test).

**Interfaces:**
- Produces (`contour::actions`): `struct FocusNextChromeArea {};`, `struct FocusPreviousChromeArea {};`,
  `struct FocusToolbar {};`, each in `Action`, documented, with catalogue rows named as the structs.
- Produces (`contour::window`, `ChromeFocus.hpp`):
  - `enum class ChromeArea : uint8_t { Terminal = 0, Tabs, Toolbar };` and `ChromeAreaCount` from the last.
  - `enum class AreaPresence : uint8_t { Absent = 0, Present };`
  - `struct ChromeAreaPresence { AreaPresence tabs = AreaPresence::Absent; AreaPresence toolbar = AreaPresence::Absent; bool operator==(ChromeAreaPresence const&) const = default; };`
  - `inline constexpr std::array ChromeAreaCycle { ChromeArea::Terminal, ChromeArea::Tabs, ChromeArea::Toolbar };`
  - `enum class ChromeFocusAction : uint8_t { Next = 0, Previous, Toolbar };`
  - `[[nodiscard]] constexpr ChromeArea chromeFocusTarget(ChromeFocusAction action, ChromeArea current, ChromeAreaPresence presence) noexcept;`
    — `Next`/`Previous` walk `ChromeAreaCycle` skipping absent areas (Terminal is always present); `Toolbar` goes to
    Toolbar if present, else stays.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeFocus_test.cpp`, `[chromefocus]`: with both present, `Next` from Terminal → Tabs → Toolbar → Terminal, and
    `Previous` the reverse; tabs absent: Terminal → Toolbar → Terminal (Review Focus 2); both absent: `Next` from
    Terminal → Terminal; `Toolbar` with no toolbar from Tabs → Tabs; every `(action, current, presence)` lands on a
    present area.
  - `Actions_test.cpp`, `[actions]`: the three actions are in the catalogue by name and parse from YAML.
  - `MenuBarTable_test.cpp`, `[menubar]`: `menuBar.everyCatalogueActionIsReachable` passes with the three exemptions, and
    none of them appears in a menu.
  - `BindingCollisions_test.cpp`, `[bindings]`: the Native2026 bindings hold Control+F6 → `FocusNextChromeArea` and
    Control+Shift+F6 → `FocusPreviousChromeArea` (plus Control+F5 → `FocusToolbar` on Apple); the Classic bindings hold
    none of the three; "no default chord fires twice" still passes.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`. Expected: compile error, no
  `actions::FocusNextChromeArea`.
- [ ] **Step 3: Implement** the actions, the bindings, the exemptions and `ChromeFocus.hpp`.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[chromefocus],[actions],[menubar],[bindings],[contour][palette]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/config/Actions.cpp src/contour/window/ChromeFocus_test.cpp src/contour/config/Actions_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/Actions.hpp src/contour/config/Actions.cpp src/contour/config/Config.hpp src/contour/command/MenuBarTable.hpp src/contour/window/ChromeFocus.hpp src/contour/window/ChromeFocus_test.cpp src/contour/config/Actions_test.cpp src/contour/command/MenuBarTable_test.cpp src/contour/config/BindingCollisions_test.cpp
git add src/contour/config/Actions.hpp src/contour/config/Actions.cpp src/contour/config/Config.hpp src/contour/command/MenuBarTable.hpp src/contour/window/ChromeFocus.hpp src/contour/window/ChromeFocus_test.cpp src/contour/config/Actions_test.cpp src/contour/command/MenuBarTable_test.cpp src/contour/config/BindingCollisions_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: focus actions for the chrome, bound to ⌃F6, ⇧⌃F6 and ⌃F5 in the new defaults" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: Keys in each area, the close question, and the controller

> **Depends on Step 1c** (the tab well and the band's toolbar items; `WindowController`'s badge roles, whose
> numbering the new properties follow) and **Step 1a** (`KitDialog`, used by Task 4).

**Files:**
- Create: `src/contour/window/ChromeKeys.hpp` (header-only, Qt-free); list it in `_header_files`.
- Modify: `src/contour/window/WindowController.{hpp,cpp}` — the properties and invokables below.
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` (the three action handlers; `paneActivity()`),
  `src/contour/session/TerminalSessionManager.{hpp,cpp}` (`moveChromeFocus`).
- Test: new `src/contour/window/ChromeKeys_test.cpp` (contour_test); `src/contour/window/MultiWindow_test.cpp` or a new
  `window/ChromeFocusController_test.cpp` (contour_gui_test, `[windowcontroller][chromefocus]`).

**Interfaces:**
- Consumes: `ChromeArea`, `ChromeAreaPresence`, `ChromeFocusAction`, `chromeFocusTarget` and the three actions (Task 2).
- Produces (`contour::window`, `ChromeKeys.hpp`):
  - `enum class ChromeKey : uint8_t { Other = 0, Left, Right, Home, End, Return, Space, Backspace, Delete, Escape };`
  - `enum class ChromeKeyModifier : uint8_t { None = 0, Alt, Other };`
  - `enum class ChromeCommand : uint8_t { None = 0, FocusPrevious, FocusNext, FocusFirst, FocusLast, Activate, Close, MoveLeft, MoveRight, Leave };`
  - `struct ChromeKeyRow { ChromeArea area; ChromeKey key; ChromeKeyModifier modifier; ChromeCommand command; };` and
    `inline constexpr std::array ChromeKeyRows` with R-e's rows; `[[nodiscard]] constexpr ChromeCommand chromeCommandFor(ChromeArea, ChromeKey, ChromeKeyModifier) noexcept;`
  - `[[nodiscard]] constexpr int focusedIndexAfter(ChromeCommand, int focused, int count) noexcept;` — clamped, no wrap.
  - `enum class PaneActivity : uint8_t { Idle = 0, Running, Unknown };`
  - `[[nodiscard]] inline PaneActivity paneActivityOf(std::string_view shellProgram, std::optional<std::string> const& foreground);`
    (R-f).
  - `enum class CloseConfirmation : uint8_t { None = 0, Ask };` and
    `[[nodiscard]] constexpr CloseConfirmation closeConfirmationFor(std::span<PaneActivity const> panes) noexcept;`
- Produces (`TerminalSession`): `[[nodiscard]] window::PaneActivity paneActivity() const;` (GUI thread; `Unknown` for a
  remote-hosted session); handlers for the three actions call `_manager->moveChromeFocus(*this, action)`.
- Produces (`TerminalSessionManager`): `void moveChromeFocus(TerminalSession const& session, window::ChromeFocusAction action);`
  — the hosting window's controller moves its chrome focus.
- Produces (`WindowController`):
  - `Q_PROPERTY(int chromeFocusArea READ … NOTIFY chromeFocusChanged)` (a `ChromeArea` value; `Terminal` means no chrome
    focus) and `Q_PROPERTY(int focusedTabIndex …)`, `Q_PROPERTY(int focusedToolbarIndex …)`, same signal.
  - `void moveChromeFocus(ChromeFocusAction action);`
  - `Q_INVOKABLE void setChromePresence(bool tabsShown, int toolbarItems);` (QML reports what it shows; carve-out). If
    the focused area becomes absent, focus returns to the terminal (Review Focus 2).
  - `Q_INVOKABLE bool chromeKey(int qtKey, int qtModifiers);` — first matches the window's active session's key
    bindings (`config::apply` over `keyMappings` with no mode flags) for the three focus actions and runs one; else maps
    the Qt key to a `ChromeKey` and runs `chromeCommandFor(chromeFocusArea, …)`; returns whether it handled it (QML
    accepts the event then; carve-out).
  - `Q_INVOKABLE void leaveChrome();` (Escape, any pointer press, the terminal regaining focus) — area `Terminal`,
    terminal focus restored.
  - `signal void closeTabConfirmationRequested(int row, QString const& tabLabel, QString const& program);` and
    `Q_INVOKABLE void confirmCloseTab(int row);`

- [ ] **Step 1: Write the failing tests.**
  - `ChromeKeys_test.cpp`, `[chromekeys]`: every row's `(area, key, modifier)` is unique; Tabs: Left → `FocusPrevious`,
    Alt+Right → `MoveRight`, Backspace and Delete → `Close`, Return and Space → `Activate`, Escape → `Leave`, Alt+Return
    → `None`; Toolbar: Backspace → `None`; Terminal: every key → `None`; `focusedIndexAfter(FocusPrevious, 0, 3) == 0`,
    `(FocusNext, 2, 3) == 2`, `(FocusLast, 0, 3) == 2`; `paneActivityOf("/bin/zsh", "zsh") == Idle`,
    `("/bin/zsh", "-zsh") == Idle`, `("/bin/zsh", "make") == Running`, `("/bin/zsh", nullopt) == Unknown`;
    `closeConfirmationFor({Idle, Running}) == Ask`, `({Idle, Unknown}) == None` (Review Focus 4).
  - `[windowcontroller][chromefocus]`, a `TestApp` with Native2026 bindings loaded, three tabs, presence
    `(tabs shown, 2 toolbar items)`:
    - "⌃F6 in the terminal focuses the active tab": the session runs `FocusNextChromeArea` → `chromeFocusArea == Tabs`,
      `focusedTabIndex == activeTab`.
    - "keys move, activate and reorder": `chromeKey(Key_Right, 0)` → `focusedTabIndex` +1, active tab unchanged;
      `Key_Return` → that tab is active; `Key_Right` with `AltModifier` → the tab moves right, keeps focus.
    - "⌃F6 cycles from inside the chrome": `chromeKey(Key_F6, ControlModifier)` from Tabs → `Toolbar`, again →
      `Terminal`.
    - "Escape and a pointer press leave" (Review Focus 3): `chromeKey(Key_Escape, 0)` → `Terminal`, the session has
      focus; `leaveChrome()` does the same.
    - "the tab well closing under focus returns it to the terminal" (Review Focus 2): `setChromePresence(false, 2)`
      while in Tabs → `Terminal`.
    - "⌫ on an idle tab closes it at once": a MockPty session (activity `Unknown`) → the tab is gone, no signal.
    - "a bare F6 is written to the PTY" (Review Focus 1): a key event F6 with no modifiers on the session → the
      MockPty's stdin holds `"\x1b[17~"` and `chromeFocusArea == Terminal`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `ChromeKeys.hpp`.
- [ ] **Step 3: Implement** the table header, the session handlers and activity, the manager call and the controller.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[chromekeys],[chromefocus]" && out/appleclang-debug/src/contour/contour_gui_test "[windowcontroller][chromefocus],[contour][window]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/WindowController.cpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.cpp src/contour/window/ChromeKeys_test.cpp src/contour/window/ChromeFocusController_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/ChromeKeys.hpp src/contour/window/ChromeKeys_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/window/ChromeFocusController_test.cpp
git add src/contour/window/ChromeKeys.hpp src/contour/window/ChromeKeys_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/window/ChromeFocusController_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: the window moves keyboard focus through its chrome and answers keys there from a table" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The ring, the keys in QML, the close dialog, snapshots and the Terminal row

> **Depends on Step 1c** (`TabStrip.qml`, `TabItem.qml`, `TitleBar.qml`, `ToolbarButton.qml`, `HostChip.qml`), **Step
> 1a** (`KitDialog.qml`) and **Step 1e** (the snapshot driver).

**Files:**
- Create: `src/contour/qml/FocusRing.qml` (add to `_qml_files`): drawn behind or around its parent, `border.width:
  chromeStyle.size.focusRing`, `border.color` from `FocusRing`, `color` from `FocusFill`, radius the parent's plus the
  ring; `visible` per R-g; `Accessible.ignored: true`.
- Modify: `src/contour/qml/TabItem.qml` (focusable; a `FocusRing`; `Accessible.focused`), `TabStrip.qml` (focus moves to
  `focusedTabIndex`'s item when `chromeFocusArea` is Tabs; `Keys.onPressed: event.accepted = win.chromeKey(event.key,
  event.modifiers)`; reports `setChromePresence`), `TitleBar.qml`, `ToolbarButton.qml`, `HostChip.qml`,
  `DaemonDriftBadge.qml` (the same for the toolbar), `Main.qml` (any pointer press calls `win.leaveChrome()` when
  `chromeFocusArea` is not Terminal; a `KitDialog` for `closeTabConfirmationRequested`, `objectName:
  "closeTabConfirmation"`).
- Modify: `src/contour/config/ChromeTokenGroups.hpp` (`size.focusRing`, `FocusRing`, `FocusFill`),
  `window/ChromeTokenObjects.{hpp,cpp}`.
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`, `MockMainController.hpp`, `QmlMocks.hpp` —
  `SnapshotScript::KeyboardFocus` (`"keyboard-focus"`: three tabs, `chromeFocusArea` Tabs, `focusedTabIndex` 1; every
  field named) and the mock properties.
- Test: `src/contour/test/MainWindowQml_test.cpp`, `QmlComponents_test.cpp`, `Accessibility_qml_test.cpp`,
  `ChromeSnapshotPlan_test.cpp`, `config/ChromeTokenGroups_test.cpp`.

**Interfaces:**
- Consumes: `chromeFocusArea`, `focusedTabIndex`, `focusedToolbarIndex`, `chromeKey`, `leaveChrome`,
  `setChromePresence`, `closeTabConfirmationRequested`, `confirmCloseTab` (Task 3).

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp`, `[chrometokens]`: `size.focusRing` per row (3, 2, 2, 0); `FocusRing` and `FocusFill`
    resolve per R-g's roles; the colour-role count derives from the last role.
  - `MainWindowQml_test.cpp`, `[contour][gui][qml][chromefocus]`, MockMainController with three tabs:
    - "keyboard focus draws the ring on the focused tab only": `chromeFocusArea = Tabs`, `focusedTabIndex = 1` → tab 1's
      `FocusRing` visible with `border.width == chromeStyle.size.focusRing`, tabs 0 and 2 not.
    - "a pointer press shows no ring" (Review Focus 3): a mouse click on tab 2 → no ring visible, and the mock saw
      `leaveChrome()` only when the area was not Terminal.
    - "keys reach the controller": a `Key_Right` press on the focused tab → the mock's `chromeKey` called with
      `Qt.Key_Right`; the press is accepted and nothing reaches the mock session.
    - "the close question": the mock emits `closeTabConfirmationRequested(1, "build", "make")` →
      `closeTabConfirmation` is open with title `Close “build”?` and text `“make” is still running in this tab.`;
      "Cancel" calls nothing, "Close" calls `confirmCloseTab(1)` (Review Focus 4).
  - `Accessibility_qml_test.cpp`: the focused `TabItem` reports `Accessible.focused`; the dialog's buttons have names.
  - `ChromeSnapshotPlan_test.cpp`, `[snapshot]`: the script exists with its name and state.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `SnapshotScript::KeyboardFocus`.
- [ ] **Step 3: Implement** the ring, the QML keys and focus moves, the pointer leave, the dialog, the tokens and the
  script; regenerate size baselines with `python3 scripts/check-qml-sizes.py --print-baseline` (new file 0).
- [ ] **Step 4: Run them and see them pass, on both rows.** Run:

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][chromefocus],[a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][chromefocus]"
ctest --test-dir out/appleclang-debug -R "check_qml" --output-on-failure
```

  Expected: PASS; under ContourTui the focused tab is a selection-coloured run and `border.width == 0`.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/ChromeTokenObjects.cpp src/contour/test/MainWindowQml_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlMocks.hpp src/contour/test/MainWindowQml_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/FocusRing.qml src/contour/qml/TabItem.qml src/contour/qml/TabStrip.qml src/contour/qml/TitleBar.qml src/contour/qml/ToolbarButton.qml src/contour/qml/HostChip.qml src/contour/qml/DaemonDriftBadge.qml src/contour/qml/Main.qml scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git add src/contour/qml/FocusRing.qml src/contour/qml/TabItem.qml src/contour/qml/TabStrip.qml src/contour/qml/TitleBar.qml src/contour/qml/ToolbarButton.qml src/contour/qml/HostChip.qml src/contour/qml/DaemonDriftBadge.qml src/contour/qml/Main.qml src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlMocks.hpp src/contour/test/MainWindowQml_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git commit -m "contour: a keyboard focus ring in the tab well and toolbar, and a question before closing a busy tab" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, apply its
  findings, re-run the filters of every task it touches, and commit once with the Signed-off-by line, staging only the
  files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[bindings],[actions],[menubar],[chromefocus],[chromekeys],[chrometokens],[contour][palette],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[shortcutoverride],[config],[windowcontroller][chromefocus],[contour][gui][qml][chromefocus],[a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][chromefocus]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, on this Mac's display:**

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`. Look at
  `keyboard-focus.*`: a 3-pt accent ring around tab 2 on MacOS, 2 pt on Fluent and ContourLinux, a selection-coloured
  run under ContourTui.
- [ ] **Step 4: Owner manual checks**, batched per phase (listed in the ledger, not awaited), on
  `out/appleclang-debug/src/contour/contour.app` with a fresh (Native2026) config:
  - ⌃F6 with System Settings › Keyboard › "Keyboard navigation" both on and off: does Contour receive it, or does
    AppKit? Record both (R-c). Then ⌃F6 → tabs ring; ←/→, ⌥→, Return; ⌃F6 → toolbar; ⌃F6 → back to the shell.
  - ⌃F5 lands on the toolbar's first item; Space presses it.
  - `mc`: F6 opens its move dialog; nothing in the chrome moves.
  - `make` running in tab 2: ⌃F6, →, ⌫ asks; Cancel keeps it; an idle tab closes at once.
  - Click a tab: no ring. VoiceOver reads the focused tab.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Moves keyboard focus into the tab bar and toolbar with ⌃F6 (Ctrl+F6), and on macOS to the toolbar with ⌃F5; in the
    tab bar the arrow keys move, Return activates, ⌫ closes (asking first if a program runs) and ⌥←/→ reorders. A bare
    F6 still goes to the shell. Adds the actions `FocusNextChromeArea`, `FocusPreviousChromeArea` and `FocusToolbar`."
  - "Makes Ctrl+C and Ctrl+V in selection mode clear the selection after copying or pasting under the built-in
    bindings, as a generated configuration file already did."
- [ ] **Step 6:** Mark Step 6.8 done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-8.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none; the collision check runs only in tests.
  - **Risk:** low to medium. The folded default pairs change what Ctrl+C and Ctrl+V do in selection mode for
    `Config{}` (to what a generated config already does); the chrome takes focus only from the three new chords.
  - **Coverage:** `[bindings]`, `[chromefocus]`, `[chromekeys]` tables, the controller cases, the QML cases on both rows,
    one snapshot script.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Editions (R-b).** *Default: the three chords are Native2026 bindings only.* The alternative binds them for Classic
   too, an accessibility path at the price of a chord Classic users' programs may have used.
2. **What "a process runs" means (R-f).** *Default: the pane's foreground program is known and is not its shell; an
   unknown one does not ask.* The alternative asks whenever it cannot tell.
3. **Folding the dead default pairs (R-a).** *Default: Ctrl+C and Ctrl+V in selection mode run both of their actions,
   as a generated `contour.yml` already does.* The alternative drops the second action of each pair, keeping today's
   `Config{}` behaviour and changing what the generated file does.

## Spec gaps found

- "Every chord is unique" conflicts with today's defaults, which bind two actions to Ctrl+C and Ctrl+V under one mode,
  the second of each dead under first-match lookup (Review Focus 5, R-a).
- The toolbar's contents and order, the keys inside the toolbar, Home/End, Escape and what happens when an area is not
  shown are not specified (R-d, R-e).
- Windows and Linux have no "toolbar" chord on the canvas; there is no "previous area" chord anywhere (⇧⌃F6 chosen).
- The ring's width on Fluent and ContourLinux, and its Terminal-row form, are not drawn.
- macOS's own keyboard-navigation shortcuts may take ⌃F5/⌃F6 before the app (R-c); to be measured.
