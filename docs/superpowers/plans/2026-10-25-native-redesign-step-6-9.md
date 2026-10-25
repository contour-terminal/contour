# Native Redesign — Step 6.9 (Bell, Dock and Taskbar: a Bell Badge, and a Count of Tabs Waiting for You) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25). Line numbers are at HEAD `661b6029`. Every earlier step runs first:
4a is done, 1c is in progress (its badge slot and tab attention, Task 5, landed as `661b6029`), and 1d, 2, 3a–3f, 4b,
5a and 5b are written. Where this step uses code those steps add, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before Task 1.

**Goal:** A bell rung in a tab the user is not looking at puts a bell badge on that tab until it is seen, and, as
today, bounces the Dock icon or flashes the taskbar button while the window is inactive; nothing ever flashes the
screen. The Dock tile, the Windows taskbar button and the Linux launcher show how many tabs are Blocked (OSC 7501:
"waiting for you"), so an agent asking a question is visible from another app.

**Architecture:**
- **The bell is one more row of the badge tables.** `window::TabBadgeKind` gains `Bell`; `SessionAttention` gains a
  `BellMark`; `TabBadgeRules` gains a row between Done and Output; `TabBadgeLooks` gains its look. `TabAttention`
  marks a hidden session's bell, and seeing it clears the mark, exactly as unseen output does.
- **The bell reaches the GUI thread once per burst.** `TerminalSession::bell()` runs on the parser thread; it posts a
  coalesced note, as Step 1c's `noteOutputForTab()` does, to `TerminalSessionManager::noteBell()`, which marks the
  ledger and repaints the tab. The alert path (`requestWindowAlert` → `QWindow::alert(0)`: the Dock bounce, the
  taskbar flash, the X11 urgency hint) is unchanged.
- **The count is pure, the sink shows it.** `window/TaskbarAttention.hpp` tallies Blocked tabs per window and in all;
  `TerminalSessionManager` publishes the tally whenever a badge, a tab or a window changes. `platform::TaskbarProgressSink`
  gains `setApplicationAttention()` and `setAttention(window, …)`: the Cocoa sink sets `NSDockTile.badgeLabel`, the Linux
  sink adds `count` and `count-visible` to its LauncherEntry update, and the Windows sink sets an overlay icon per
  taskbar button.

**Tech Stack:** C++23, Qt 6.11 (Gui, Quick, QtDBus on Linux), QML, Objective-C++ with ARC (AppKit), Win32
(`ITaskbarList3::SetOverlayIcon`), Catch2 v3, CMake build tree `out/appleclang-debug`, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.9 binds, with what
it cites: §1 (principles; 3 pure decisions, 4 the OS through interfaces), §2.5–§2.6 (snapshots and testing), §2.7
(`DefaultsEdition`). The design record is `docs/superpowers/specs/2026-10-23-native-redesign-design.md` (Coverage
additions: "OSC 7501 program status shares one tab badge slot with the unseen-output dot and the finished-command
badge. Priority is blocked › error › working › done › output"; "bell, dock and taskbar"; the new defaults' "Bell | no
sound; alert on"). The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `surfaces`
("Bell, attention, dock and taskbar"), `kit-components` and `tui` are in `.superpowers/design/canvas/project/`. The
plans index is `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

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

- **The spec, verbatim (§3.16, row 6.9):** "Bell, dock, taskbar | The visual bell is a tab badge plus a dock bounce or
  taskbar flash. The dock and taskbar badge counts tabs that are Blocked | existing `TaskbarProgressSink` backends,
  extended with a badge count".
- **Canvas values, exact** (`surfaces`, "Bell, attention, dock and taskbar"; `BellSound / DeferredBell.qml ·
  platform/LauncherProgressSink · window alert`): "The visual bell is a tab badge plus dock bounce or taskbar flash,
  never a screen flash. OSC 9;4 drives the dock tile's progress bar or the taskbar's, as it does today. New: the dock and
  taskbar badge counts tabs that are waiting for you (OSC 7501 blocked), so an agent asking a question is visible from
  another app."
  - The bell's look (owner decision 2): Flat glyph `"♪"`, Pill mark `BadgeMark::Glyph`, ink `StatusColorRole::Warning`,
    accessible name "Bell".
  - Badge text: the Dock shows the count (`"3"`), none at 0; the Windows overlay shows `"1"`–`"9"`, then `"9+"`, with
    the description `"<n> tabs waiting for you"` (`"1 tab waiting for you"`); Linux sends the count.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the SDD ledgers of 1c, 1d, 2, 3a–3f, 4b, 5b):
  - The 1c badge slot: one slot per tab, the priority a table (`TabBadgeRules`), the looks a table indexed by kind
    (`TabBadgeLooks`, `static_assert`ed in order), QML reads the kind's `int` and `chromeStyle.badgeLook(kind)`. New
    enumerators are appended so existing values do not move.
  - The 1c attention seams: `TerminalSessionManager::noteSessionOutput`, `noteCommandFinished`,
    `markVisibleSessionsSeen` (called on `activeTabChanged`, `windowStateChanged` and `paneZoomChanged`) and
    `tabAttention()`; posts from the parser thread are coalesced with an atomic flag and go through
    `platform::postToObject`.
  - The OS is reached only through `TaskbarProgressSink` (the existing seam the spec names); `ContourGuiApp` already
    takes it as a constructor argument (`ContourGuiApp.hpp:269`), so tests inject a recording sink.
  - AppKit: the Cocoa sink is ARC, `@pre` main thread, inside the `if(APPLE)` block; Windows- and Linux-only units are
    wholly inside `#ifdef _WIN32` / `#ifdef __linux__`, listed unconditionally, and verified only by CI with a
    **Verified by** line; a task is not done until that run is green.
  - `[display]` cases skip offscreen. TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - No colour or size literal in chrome QML. Every visual surface is a snapshot row; every visual task renders the
    Terminal row (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - `enum class` over `bool`; `std::expected` for fallible calls.
  - New defaults go through `DefaultsEdition`. This step changes none: `bell.alert` keeps its value in both editions,
    and the badge and the count are not settings (owner decision 3).
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven tables (the badge rules and looks, the overlay text), dependency injection
  (the sink), configuration at construction, Doxygen on every new public declaration, `.hpp` with `#pragma once`, zero
  warnings, no `NOLINT`; `slots`, `signals` and `emit` are never identifiers.
- **Build** with every core, configured `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never two builds in one tree at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding (Windows- and Linux-only files: CI's `check_clang_tidy`); format with
  `/opt/homebrew/opt/llvm@22/bin/clang-format -i`; `editorconfig-checker <files>` for `.qml`, `.txt`, `CMakeLists.txt`.
  **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value;
  every table row names every field (`TabBadgeRules`, `TabBadgeLooks`, `SnapshotScripts`).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Qt-free stays Qt-free:** `window/TabBadge.hpp`, `window/TabAttention.hpp` and `window/TaskbarAttention.hpp` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, never `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 5).

## Review Focus

1. **A program rings the bell in a loop.** `yes $'\a'`, or a build that beeps per warning, in a background tab.
   Expected: one queued call per burst on the GUI thread, one badge, the Dock bouncing as it does today, and no growth
   in memory or events. Pinned in Task 2 ("a bell storm queues one note at a time", `[threading]`).
2. **The badge that never clears.** The bell rang in a tab that is then shown by a tab switch, a window restore, a zoom
   ending, or a mouse click into the window. Expected: the bell badge clears on each of 1c's seen seams; a bell in the
   focused, visible pane puts no badge anywhere. Pinned in Task 1 (`noteSeen` clears it) and Task 2.
3. **Blocked and bell together.** A tab both Blocked (OSC 7501) and rung. Expected: the badge shows Blocked (priority),
   the count includes it; once the program is no longer Blocked, the bell shows until seen. Pinned in Task 1
   (`tabBadge.priority`) and Task 3.
4. **The count goes stale.** A Blocked tab is closed, its window is closed, it moves to another window, or the program
   leaves the Blocked state. Expected: the Dock/launcher count and the per-window overlay follow at once, and at 0 the
   badge disappears entirely, leaving another owner's Dock badge alone (the existing Cocoa test's contract). Pinned in
   Task 3.
5. **Progress and count at once.** OSC 9;4 progress and a Blocked count together. Expected: the Dock shows both (the
   progress view is the tile's content, the count its badge label); the Linux update carries both properties; on
   Windows the overlay icon and the progress bar coexist. Pinned in Task 3 (Cocoa and Linux) and Task 4 (Windows).

Also pinned, beyond the five: the TUI row draws the bell glyph in its cells (Task 2's QML pass); remote panes' bells
badge as local ones do (Task 2).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Where the bell ranks (owner, 1).** Between Done and Output: `Blocked › Error › Working › Done › Bell ›
  Output`. A bell is louder than new output and quieter than a verdict.
- **R-b. When a bell badges.** Only when the ringing session is `Hidden` (`SessionVisibility`), as unseen output is; a
  bell in a visible pane already has the user's eyes, and, with Native2026's silent bell, no other visual sign
  (owner, 4).
- **R-c. The bounce.** Unchanged: `bell.alert` gates `requestWindowAlert`, and `QWindow::alert(0)` does nothing on an
  active window. The spec's "dock bounce or taskbar flash" is today's behaviour; this step adds the badge.
- **R-d. What is counted.** A tab counts when its resolved badge is `TabBadgeKind::Blocked`. `Blocked` shows in every
  tab (`BadgeVisibility::Always`, `ProgramStatusBadge.hpp:76-78`), so the foreground tab counts too: a question waiting
  in the window the user just left is still waiting.
- **R-e. Scope follows the sink.** `TaskbarProgressScope::Application` sinks (Cocoa, Linux) get the total;
  `TaskbarProgressScope::Window` sinks (Windows) get each window's own count.
- **R-f. The overlay's colours.** Drawn in C++ by the Windows sink: a disc in `StatusColorRole::Warning`'s dark value
  with the digits in the on-colour chosen by contrast (ruling R12), from the status table, not literals.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/window/TabBadge.hpp`, `TabAttention.hpp` (modify) | The bell's kind, mark, rule and look | 1 |
| `src/contour/session/TerminalSession.*`, `session/TerminalSessionManager.*` (modify); snapshot mock | The bell reaches the badge | 2 |
| `src/contour/window/TaskbarAttention.hpp` (create); `platform/TaskbarProgressSink.{hpp,cpp}`, `CocoaTaskbarProgressSink.mm`, `LauncherProgress.hpp`, `LauncherProgressSink.cpp`, `TaskbarProgressState.hpp` (modify) | The count, macOS, Linux | 3 |
| `src/contour/platform/TaskbarProgressSink.cpp` (the Win32 class), `platform/TaskbarOverlay.hpp` (create) | Windows | 4 |

---

### Task 1: The bell in the badge tables

**Files:**
- Modify: `src/contour/window/TabBadge.hpp` — `TabBadgeKind` (`:41-50`), `TabBadgeKindCount` (`:53`), `TabBadgeInputs`
  (`:56-62`), `TabBadgeRule` (`:74-80`), `TabBadgeRules` (`:83-90`), `tabBadgeRuleMatches` (`:96-102`), `TabBadgeLooks`
  (`:153-163`).
- Modify: `src/contour/window/TabAttention.hpp` — `SessionAttention` (`:40-46`), `noteBell`, `pooledAttention`
  (`:160-174`).
- Test: `src/contour/window/TabBadge_test.cpp`, `src/contour/window/TabAttention_test.cpp` (contour_test).

**Interfaces:**
- Produces (`contour::window`):
  - `TabBadgeKind` gains `Bell` after `Blocked` (values stay); `TabBadgeKindCount` derives from `Bell`.
  - `enum class BellMark : uint8_t { None = 0, Rang };`
  - `TabBadgeInputs` gains `BellMark bell = BellMark::None;` after `activity`; `TabBadgeRule` gains `BellMark bell;`
    after `activity`; every row names it; `tabBadgeRuleMatches` also matches a non-None `bell`.
  - `TabBadgeRules`: `TabBadgeRule { TabBadgeKind::Bell, BadgeKind::None, FinishedBlockBadge::None, TabActivity::None, BellMark::Rang }`
    between the Done and Output rows (R-a).
  - `TabBadgeLooks` gains `TabBadgeLook { TabBadgeKind::Bell, "♪", BadgeMark::Glyph, config::StatusColorRole::Warning, "Bell" }`
    last.
  - `SessionAttention` gains `BellMark bell = BellMark::None;`; `TabAttention` gains
    `[[nodiscard]] AttentionChange noteBell(vtworkspace::SessionId session, platform::SessionVisibility visibility);`
    (marks only `Hidden`); `noteSeen` already clears every mark; `pooledAttention` pools `bell` (any leaf rang).

- [ ] **Step 1: Write the failing tests.**
  - `TabBadge_test.cpp`, `[tabbadge]`:
    - `tabBadge.priority` gains: `{bell = Rang}` → `Bell`; `{bell = Rang, activity = UnseenOutput}` → `Bell`;
      `{finishedBlock = Succeeded, bell = Rang}` → `Done`; `{programStatus = Blocked, bell = Rang}` → `Blocked`
      (Review Focus 3).
    - `tabBadge.everyAdjacentPairResolvesToTheHigher` covers the new row with no edit (it walks `TabBadgeRules`).
    - "the bell's look": `tabBadgeLook(Bell)` is `"♪"`, `Glyph`, `Warning`, `"Bell"`.
  - `TabAttention_test.cpp`, `[tabattention]`: "a hidden bell marks once": `noteBell(s, Hidden)` → `Changed`, again →
    `Unchanged`, `of(s).bell == Rang`; "a visible bell marks nothing": `Focused` and `VisibleUnfocused` → `Unchanged`;
    "seeing clears the bell" (Review Focus 2): `noteSeen(s)` → `Changed`, `of(s) == SessionAttention {}`;
    "pooledAttention pools the bell": a split whose second leaf rang → `bell == Rang`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`. Expected: compile error, no
  `TabBadgeKind::Bell`.
- [ ] **Step 3: Implement** the enumerator, the mark, the columns, the row, the look and `noteBell`.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[tabbadge],[tabattention]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/TabBadge_test.cpp src/contour/window/TabAttention_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/TabBadge.hpp src/contour/window/TabAttention.hpp src/contour/window/TabBadge_test.cpp src/contour/window/TabAttention_test.cpp
git add src/contour/window/TabBadge.hpp src/contour/window/TabAttention.hpp src/contour/window/TabBadge_test.cpp src/contour/window/TabAttention_test.cpp
git commit -m "contour: a bell rung out of sight is a tab badge, ranked between done and new output" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The bell reaches the badge; the snapshot and the Terminal row

> **Depends on Step 1c** (`TerminalSessionManager::noteSessionOutput`, `refreshTabBadge`, `markVisibleSessionsSeen`,
> `WindowController::tabBadge`, the `BadgeRole` family, `TabDecor::EveryBadge` and the `tab-badges` snapshot script).

**Files:**
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — `bell()` (`:817-840`) posts a coalesced note;
  `_isBellNoticePending` beside `_isOutputNoticePending`.
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}` — `noteBell(vtworkspace::SessionId)`.
- Modify: `src/contour/window/WindowController.cpp` — `tabBadge()` passes the pooled `bell`.
- Modify: `src/contour/test/MockMainController.hpp` — `TabDecor::EveryBadge` gives one tab per `TabBadgeKind` from
  `TabBadgeKindCount` (if 1c listed the kinds by hand, derive them now), so the `tab-badges` script shows the bell.
- Test: `src/contour/session/TerminalSessionManager_test.cpp`, `src/contour/test/QmlComponents_test.cpp`,
  `src/contour/test/MainWindowQml_test.cpp` (contour_gui_test).

**Interfaces:**
- Consumes: `BellMark`, `TabAttention::noteBell`, `TabBadgeKind::Bell` (Task 1).
- Produces (`TerminalSession`): `void noteBellForTab();` (parser thread; posts at most one call at a time, which calls
  `_manager->noteBell(modelSessionId())`), called from `bell()` before `emit onBell(volume)`.
- Produces (`TerminalSessionManager`): `void noteBell(vtworkspace::SessionId session);` — `_attention.noteBell(session,
  sessionVisibility(*session))`; on `Changed`, `refreshTabBadge` of the hosting tab. GUI thread.

- [ ] **Step 1: Write the failing tests.**
  - `TerminalSessionManager_test.cpp`, `[manager][tabbadge]`, a `TestApp`, two tabs over `MockPty`:
    - "a bell in a background tab badges it": BEL written to tab 2's session, events processed → tab 2's `BadgeRole ==
      int(TabBadgeKind::Bell)`, tab 1's 0.
    - "seeing the tab clears the bell" (Review Focus 2): activate tab 2 → `BadgeRole == 0`; a bell then rung in the
      focused pane → still 0.
    - "the alert still fires": with `bell.alert: true`, the hosting controller emitted `alertRequested` once per bell
      in a background tab, as before.
    - "a remote-hosted session's bell badges like a local one": a remote-hosted test factory's session, hidden.
  - `TerminalSessionManager_test.cpp`, `[manager][tabbadge][threading]`, "a bell storm queues one note at a time"
    (Review Focus 1): 10 000 BEL bytes written in one chunk to a hidden session → at most one `noteBell` call is queued
    at any time (a counting double through the manager's test seam of 1c's output case), and the badge is `Bell`.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][tabbadge]`: a `TabItem` with `tabBadge: <Bell>` draws the glyph
    `"♪"` in the Warning ink on the Flat shape and a Glyph mark on the Pill shape, its accessible name "Bell".
  - `MainWindowQml_test.cpp`: `TabDecor::EveryBadge` shows `TabBadgeKindCount` tabs, the last with the bell.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`. Expected: compile error, no
  `noteBell`.
- [ ] **Step 3: Implement** the post, the manager's note, the controller's pooled input and the mock's derived decor.
- [ ] **Step 4: Run them and see them pass, on both rows.** Run:

```bash
out/appleclang-debug/src/contour/contour_gui_test "[manager][tabbadge],[contour][gui][qml][tabbadge],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][tabbadge]"
```

  Expected: PASS; under ContourTui the bell is the `♪` cell.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.cpp src/contour/session/TerminalSessionManager_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/test/MockMainController.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp
git commit -m "contour: a bell in a hidden pane badges its tab until the tab is seen" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: The count of Blocked tabs, on the Dock and the Linux launcher

> **Depends on Step 1c** (`WindowController::tabBadge`, the resolved kind per tab) and **Step 5b** (the Linux launcher
> sink is unchanged by 5b; re-verify at pre-flight).

**Files:**
- Create: `src/contour/window/TaskbarAttention.hpp` (header-only, Qt-free); list it in `_header_files`.
- Modify: `src/contour/platform/TaskbarProgressRouter.hpp` — `struct TaskbarAttention` beside `TaskbarProgress`
  (`:19-24`).
- Modify: `src/contour/platform/TaskbarProgressSink.hpp:21-50` — two pure virtuals; `TaskbarProgressSink.cpp` (the null
  sink's no-ops); `CocoaTaskbarProgressSink.mm` (the badge label); `LauncherProgress.hpp` (the properties);
  `LauncherProgressSink.cpp` (send both).
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}` — `publishTaskbarAttention()`.
- Modify: `src/contour/test/GuiTestFixtures.hpp` — a `RecordingTaskbarSink` if 1c has none, passed as `taskbarProgress`.
- Test: new `src/contour/window/TaskbarAttention_test.cpp` (contour_test); `platform/LauncherProgress_test.cpp`,
  `platform/CocoaTaskbarProgressSink_test.mm`, `session/TerminalSessionManager_test.cpp` (contour_gui_test).

**Interfaces:**
- Produces (`contour::platform`): `struct TaskbarAttention { unsigned waiting = 0; bool operator==(TaskbarAttention const&) const = default; };`
  and, on `TaskbarProgressSink`:
  - `virtual void setApplicationAttention(TaskbarAttention attention) = 0;` — the total, for `Application`-scope sinks.
  - `virtual void setAttention(uint64_t window, TaskbarAttention attention) = 0;` — one window's, for `Window`-scope
    sinks; retained while the button is not ready, as `setProgress` is.
- Produces (`contour::window`, `TaskbarAttention.hpp`):
  - `struct AttentionTally { std::vector<std::pair<uint64_t, unsigned>> perWindow {}; unsigned total = 0; bool operator==(AttentionTally const&) const = default; };`
  - `[[nodiscard]] inline AttentionTally tallyWaitingTabs(std::span<std::pair<uint64_t, TabBadgeKind> const> tabs);` —
    counts `TabBadgeKind::Blocked` per window (every window listed, zero included, so a window that drops to zero is
    told) and in all.
  - `[[nodiscard]] inline std::string dockBadgeText(unsigned waiting);` → `""` for 0, else the number.
- Produces (`LauncherProgress.hpp`): `[[nodiscard]] inline QVariantMap launcherEntryProperties(TaskbarProgress progress, TaskbarAttention attention);`
  — today's two progress keys plus `count` (`qint64`) and `count-visible` (`attention.waiting > 0`);
  `launcherProgressProperties` is folded into it.
- Produces (`TerminalSessionManager`): `void publishTaskbarAttention();` — builds the `(window, kind)` list from every
  controller's tabs, tallies it, and calls the sink by its `scope()`; only when the tally changed since the last call.
  Called from `refreshTabBadge`, tab and window removal, tab moves between windows, and controller creation.
- The Cocoa sink keeps the label it found before its first non-zero count and restores it at zero and in its
  destructor (the existing tests' rule that another owner's badge is left alone); it sets `NSApp.dockTile.badgeLabel`
  and calls `[NSApp.dockTile display]`. The progress content view and the label are independent.

- [ ] **Step 1: Write the failing tests.**
  - `TaskbarAttention_test.cpp`, `[taskbarattention]`: two windows with `{Blocked, Done}` and `{Blocked, Blocked}` →
    per window 1 and 2, total 3; a window with none → listed with 0; `dockBadgeText(0) == ""`, `(12) == "12"`.
  - `LauncherProgress_test.cpp`, `[contour][taskbar]`: `launcherEntryProperties({Normal, 40}, {2})` has
    `progress-visible`, `progress == 0.4`, `count == 2`, `count-visible == true`; `({}, {0})` has `count-visible ==
    false` (Review Focus 5).
  - `CocoaTaskbarProgressSink_test.mm`, `[contour][taskbar-native][display]`: "the Dock badge counts waiting tabs and
    restores the label it found": with `badgeLabel = @"7"` set by another owner, `setApplicationAttention({3})` →
    `@"3"`; `({0})` → `@"7"`; with progress Normal 60 % active, `({2})` → `@"2"` and the progress view still installed;
    destroying the sink with a count shown → `@"7"` (Review Focus 4, 5).
  - `TerminalSessionManager_test.cpp`, `[manager][taskbarattention]`, a `TestApp` with a recording `Application`-scope
    sink: an OSC 7501 Blocked record in tab 2 → `setApplicationAttention({1})`; a second window's Blocked tab →
    `({2})`; closing that tab → `({1})`; the program leaving Blocked → `({0})`; moving the Blocked tab to a new window
    → unchanged total and no extra call. With a `Window`-scope recording sink, the same steps call
    `setAttention(windowId, …)` per window, including `{0}` for the window that lost it (Review Focus 4).
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected:
  compile error, no `TaskbarAttention`.
- [ ] **Step 3: Implement** the struct, the virtuals (every sink, the Windows one as no-ops until Task 4), the tally, the
  manager's publication, the Cocoa label and the Linux properties.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_test "[taskbarattention]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][taskbar],[manager][taskbarattention],[manager][tabbadge]"`,
  then `CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[taskbar-native]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSessionManager.cpp src/contour/window/TaskbarAttention_test.cpp src/contour/platform/LauncherProgress_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/TaskbarAttention.hpp src/contour/window/TaskbarAttention_test.cpp src/contour/platform/TaskbarProgressRouter.hpp src/contour/platform/TaskbarProgressSink.hpp src/contour/platform/TaskbarProgressSink.cpp src/contour/platform/CocoaTaskbarProgressSink.mm src/contour/platform/CocoaTaskbarProgressSink_test.mm src/contour/platform/LauncherProgress.hpp src/contour/platform/LauncherProgressSink.cpp src/contour/platform/LauncherProgress_test.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/test/GuiTestFixtures.hpp
git add src/contour/window/TaskbarAttention.hpp src/contour/window/TaskbarAttention_test.cpp src/contour/platform/TaskbarProgressRouter.hpp src/contour/platform/TaskbarProgressSink.hpp src/contour/platform/TaskbarProgressSink.cpp src/contour/platform/CocoaTaskbarProgressSink.mm src/contour/platform/CocoaTaskbarProgressSink_test.mm src/contour/platform/LauncherProgress.hpp src/contour/platform/LauncherProgressSink.cpp src/contour/platform/LauncherProgress_test.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/test/GuiTestFixtures.hpp src/contour/CMakeLists.txt
git commit -m "contour: the Dock and the Linux launcher count the tabs waiting for you" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

**Verified by** (the Linux launcher adapter only): CI Ubuntu jobs (`ubuntu_2404_cc_matrix`), step `tests`, ctest
`contour_gui_test`, filter `[contour][taskbar-native]` ("Launcher adapter stays disconnected offscreen" still passes
with the new properties); and `check_clang_tidy`.

---

### Task 4: The Windows overlay icon

**Files:**
- Create: `src/contour/platform/TaskbarOverlay.hpp` (Qt-free): the overlay's text and description.
- Modify: `src/contour/platform/TaskbarProgressState.hpp` — generalise the button-readiness state to
  `template <typename Desired> class TaskbarButtonState`, with `using TaskbarProgressState = TaskbarButtonState<TaskbarProgress>;`
  and `using TaskbarAttentionState = TaskbarButtonState<TaskbarAttention>;` (one readiness rule for both).
- Modify: `src/contour/platform/TaskbarProgressSink.cpp` — `Win32TaskbarProgressSink::Window` gains a
  `TaskbarAttentionState overlay` and an owned `HICON`; `setAttention` and `publish` call
  `ITaskbarList3::SetOverlayIcon(hwnd, icon, description)` (a null icon clears it); the `TaskbarButtonCreated` and
  surface events replay it as they replay progress; icons are destroyed with `DestroyIcon` when replaced, on
  `removeWindow` and in the destructor.
- Test: new `src/contour/platform/TaskbarOverlay_test.cpp` (contour_test); the existing `TaskbarProgressState` cases
  (wherever 1c or earlier put them) pass unedited; new `src/contour/platform/Win32TaskbarOverlay_test.cpp` (wholly
  `#ifdef _WIN32`, contour_gui_test).

**Interfaces:**
- Produces (`TaskbarOverlay.hpp`): `[[nodiscard]] inline std::string overlayBadgeText(unsigned waiting);` → `""`, `"1"`
  … `"9"`, `"9+"`; `[[nodiscard]] inline std::string overlayDescription(unsigned waiting);` → `""`, `"1 tab waiting for
  you"`, `"<n> tabs waiting for you"`.
- The icon is a 16 × 16 (scaled by the window's device-pixel ratio) `QImage`, a disc in the Warning status colour and
  the text in the contrast-chosen on-colour (R-f), converted with `QImage::toHICON()`.

**Verified by:** CI `Windows` job (`build.yml` `windows:`), steps `Build` and `Test` (`ctest --preset msvc-release`),
ctest `contour_gui_test`, filter `[taskbar-native][win32]`, and ctest `contour_test`, filter `[taskbaroverlay]`; and
`check_clang_tidy`.

- [ ] **Step 1: Write the failing tests.**
  - `TaskbarOverlay_test.cpp`, `[taskbaroverlay]` (runs here): the text and description for 0, 1, 9, 10 and 250.
  - `Win32TaskbarOverlay_test.cpp`, `[taskbar-native][win32]`: a shown `QWindow` bound to the sink; `setAttention(id,
    {2})` before the button exists is retained and published once `TaskbarButtonCreated` arrives (a posted message);
    `setAttention(id, {0})` clears the overlay; with progress Normal 40 % also set, both calls succeed in either order
    (Review Focus 5); `removeWindow(id)` destroys the icon (no `HICON` leak: the sink's live-icon count is 0).
- [ ] **Step 2: Run the portable test and see it fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`. Expected: compile error, no
  `TaskbarOverlay.hpp`.
- [ ] **Step 3: Implement** the header, the generalised state and the Win32 overlay.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/contour/contour_test "[taskbaroverlay]"`
  here; push, and read the `Windows` job's log for `[taskbar-native][win32]`. Expected: PASS here, green in CI.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/TaskbarOverlay.hpp src/contour/platform/TaskbarOverlay_test.cpp src/contour/platform/TaskbarProgressState.hpp src/contour/platform/TaskbarProgressSink.cpp src/contour/platform/Win32TaskbarOverlay_test.cpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform/TaskbarOverlay.hpp src/contour/platform/TaskbarOverlay_test.cpp src/contour/platform/TaskbarProgressState.hpp src/contour/platform/TaskbarProgressSink.cpp src/contour/platform/Win32TaskbarOverlay_test.cpp src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "platform: the Windows taskbar button shows how many of its tabs wait for you" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, apply its
  findings, re-run the filters of every task it touches, and commit once with the Signed-off-by line, staging only the
  files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[tabbadge],[tabattention],[taskbarattention],[taskbaroverlay],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[manager][tabbadge],[manager][taskbarattention],[contour][taskbar],[contour][gui][qml][tabbadge],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][tabbadge]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS here; the Windows and Ubuntu jobs green.
- [ ] **Step 3: Snapshots, on this Mac's display:**

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot],[taskbar-native]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances` (no new script: the
  `tab-badges` script grows a tab). Look at `tab-badges.*`: the bell after Done, a warning-coloured `♪`.
- [ ] **Step 4: Owner manual checks**, batched per phase (listed in the ledger, not awaited), on
  `out/appleclang-debug/src/contour/contour.app` with a fresh (Native2026) config:
  - In tab 2, `sleep 3; printf '\a'`, then switch to tab 1: tab 2 shows `♪`; switch back: it clears. ⌘Tab to Safari
    first: the Dock icon bounces, as before.
  - `printf '\e]7501;state=blocked\a'` (the OSC 7501 form Step 1c's tests use) in two tabs: the Dock badge reads 2;
    end one: 1; close the other: the badge is gone. With `printf '\e]9;4;1;50\a'` too: progress and badge together.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release: "Shows a bell
  badge on a tab whose program rang the bell while out of sight, until the tab is seen; the Dock icon, the Windows
  taskbar button and the Linux launcher count the tabs waiting for you (OSC 7501 blocked), so a question asked in a
  terminal is visible from another app."
- [ ] **Step 6:** Mark Step 6.9 done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-9.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** one coalesced GUI-thread call per bell burst; the tally walks every tab on each badge refresh,
    O(tabs), and calls the OS only when it changed.
  - **Risk:** low. The badge reuses 1c's tables and seams; the Dock label restores whatever it found; the Windows
    overlay is verified in CI only and owns its icons.
  - **Coverage:** `[tabbadge]`, `[tabattention]`, `[taskbarattention]`, `[taskbaroverlay]` tables; the manager's bell and
    count cases incl. a `[threading]` storm; the Cocoa, Linux and Win32 sinks; the grown `tab-badges` snapshot.

---

## Owner decisions

The run proceeds on each recommended default.

1. **The bell's rank (R-a).** *Default: between Done and Output.* The alternative ranks it above Done, as an alert.
2. **The bell's glyph.** *Default: `♪` in the Warning ink, a glyph on both shapes.* The alternatives are a 🔔-like
   symbol (an emoji in most fonts, which the chrome avoids) or the Output dot in the Warning ink.
3. **Settings.** *Default: none; the badge and the count are always on.* The alternative adds `bell.badge` and a
   `dock_badge` switch, each an `EditionDefaults` column.
4. **A bell in the visible, focused pane (R-b).** *Default: no badge.* With Native2026's silent bell that bell shows
   nothing at all; the alternative briefly marks the pane header (Step 2), still never the screen.

## Spec gaps found

- The bell's place in the badge priority, its glyph and colour, and whether a visible pane's bell shows anything are
  not given (R-a, R-b; owner decisions 1, 2, 4).
- "Counts tabs that are Blocked" does not say whether the foreground tab counts (R-d) or how a per-window taskbar
  splits the count (R-e).
- The Windows overlay's look is not drawn; GNOME without a dock extension shows no launcher count at all.
- The dock bounce is today's `QWindow::alert(0)`, which bounces until the app is activated; the spec does not say
  whether one bounce was meant.
