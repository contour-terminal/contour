# Native Redesign — Step 6.4 (Moments: Drag and Drop) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling). Line numbers are at HEAD `661b6029`, before 1c (in progress), 1d, 2, 3a–3f,
4b, 5a and 5b land. Where this step uses code they add, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before 6.4 executes.

**Goal:** Files dropped on a pane type their shell-escaped paths at the prompt; a tab dragged onto a pane's outer
20 % shows a half-pane ghost and, dropped, becomes a split on that side; files never split, and a sloppy drop
rearranges nothing.

**Architecture:**
- **One pure decision.** `window/DropZonePolicy.hpp` maps a payload, the pane's extent and the pointer to a
  `DropZone` and its ghost; `window/DroppedText.hpp` turns dropped paths into text for the pane's shell. Both
  Qt-free, table-tested in `contour_test`.
- **One model operation.** `vtworkspace::SessionModel::mergeTabIntoSplit` grafts a whole tab's pane tree beside a
  leaf of another tab, keeping its sessions, as `moveTabToWindow` keeps them across windows.
- **QML asks, C++ decides.** A `DropArea` on `PaneCard.qml` (Step 2) reports what is over it; the window controller
  answers with the zone from the policy, and performs the drop: paths into the session as a paste, or the merge
  through `TerminalSessionManager`.
- **Today's tab drag is kept.** `TabItem.qml`'s automatic `QDrag` carries `application/x-contour-tab`
  (`TabItem.qml:279-284`); a drop the pane accepts ends the drag with `MoveAction`, so `onDragFinished` does not
  tear the tab off (`:288-291`).

**Tech Stack:** C++23, Qt 6.11 (Quick), QML, Catch2 v3, CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.4 (`:821`):
"Dropped files insert shell-escaped paths. A tab dropped on a pane's outer 20 % splits it (pure `DropZonePolicy`)",
seam none. It binds with what it cites: §1 principles (`:28-64`; 3 pure decisions, 5 nothing over non-blank
cells), §2.1 token rows (`:68-126`), §2.6 testing (`:206-226`), §5 (`:844-854`). The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; local copies in `.superpowers/design/canvas/project/`:
`moments` board 7 ("Files dropped on a pane insert shell-escaped paths (2 pt accent ring while hovering). Only tabs
dragged onto a pane's outer 20 % make a split (half-pane ghost); files never split, so a sloppy drop cannot
rearrange the window"; the ghost: accent at 22 %, a 1.5-pt accent inset border, radius 6, label "Tab → split
right"), `tui`. The plans index is `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

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

- **The spec, verbatim (§3.16 row 6.4):** "Dropped files insert shell-escaped paths. A tab dropped on a pane's outer
  20 % splits it (pure `DropZonePolicy`)". Canvas: "files never split".
- **Canvas values, exact** (`moments` 7): ring 2 pt accent while hovering; ghost = half the pane, accent at 22 %
  fill, 1.5-pt accent inset border, radius 6, inset 6 from the pane's edges; label 11 pt primary text, "Tab → split
  right".
- **Binding rulings** (ledgers, `.superpowers/sdd/*/progress.md`):
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`); every visual surface is a
    snapshot row and every visual task renders the Terminal row (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
    `[display]` tests skip offscreen.
  - Step 1c D3: a transient, user-driven overlay of cells is accepted (the ring and the ghost exist only while a drag
    hovers).
  - Text that reaches a PTY on the user's behalf goes through the session's paste path (bracketed paste when the
    application asked for it, 3c's paste limits).
  - `enum class` over `bool`, zero the default; QML-facing booleans are the carve-out; `std::expected` for fallible
    calls. Owner manual checks are batched per phase.
- **AGENT.md applies in full:** data-driven (the zone rows and their labels, the quoting rows); configuration at
  construction; Doxygen; zero warnings and no `NOLINT`.
- **Build** with every core; the tree is configured with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds at once.
- **Lint and format** before each commit: `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug
  --extra-arg=-isysroot$(xcrun --show-sdk-path) <touched .cpp files>`; `clang-format -i` (llvm@22) on touched
  `.cpp`/`.hpp`; `editorconfig-checker` on touched `.qml` and `CMakeLists.txt`. Never clang-format a `.qml` file.
- **Tests:** each task runs only its own filter; a task touching `vtworkspace` also runs
  `ctest --test-dir out/appleclang-debug -L daemon`. The full suite runs in CI.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every row of `DropZoneRows`,
  `ShellQuotingRows`, `NoticeRows`, `ChromeTokenRows` and `SnapshotScripts` names every field.
- **Header-only functions are `inline` or `constexpr`.** Counts in tests come from tables.
- **Qt-free stays Qt-free:** the two new `window/*.hpp` and `vtworkspace` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task, staging only that task's files by name, each message ending with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, never `Co-Authored-By`.

## Review Focus

1. **A file name that would run something** — a newline, `$(…)`, backticks, `;`, a leading `-`. Expected: every shell
   metacharacter is escaped for the pane's shell, a name holding a control character inserts nothing and says why,
   and the text goes in as a paste, never as keystrokes that could press Return. Pinned in Task 1
   (`droppedPathsText` cases) and Task 3 ("a dropped path arrives as a paste").
2. **A sloppy tab drop in a pane's middle tears the tab into a new window or rearranges it.** Today a tab released
   anywhere but a tab strip tears off (`TabItem.qml:288-291`). Expected: the middle of a pane accepts the tab and does
   nothing. Pinned in Task 1 (`Inert`) and Task 4 ("a tab dropped in the middle stays put").
3. **A tab dropped on its own pane, or a window's only tab dropped on another window.** Expected: on its own tab's
   pane nothing happens; from another window it merges and the emptied source window closes, no session lost.
   Pinned in Task 1 (`OwnTab`) and Task 2.
4. **A daemon-hosted window splits locally and the daemon's next layout push undoes it,** dropping panes or sessions.
   Expected: no split zones (and no ghost) while the window is hosted on a daemon; files still insert. Pinned in
   Task 3 ("a daemon-hosted window offers no split").
5. **Files dropped on a pane running PowerShell, cmd, vim or ssh.** Expected: PowerShell gets single quotes, cmd
   double quotes, everything else POSIX escapes (vim's `:e` and a remote shell read them). Pinned in Task 1.

Also pinned, beyond the five: corners pick the nearer edge (Task 1); a URL or text drop is pasted as text (Task 3);
the Terminal row draws the ring and the ghost in cells (Task 4).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Zones.** Each edge band is `DropEdgeFraction` (0.20) of the pane card's width (left, right) or height (top,
  bottom). In a corner, the edge whose distance divided by its band is smaller wins; a tie goes to left/right. Files
  and text never get a split zone: anywhere over the pane they are `InsertText`. A tab in the middle is `Inert`
  (owner).
- **R-b. Which tab drops can split.** A tab from any window of this process, onto a pane of another tab; onto a pane
  of its own tab it is `Inert`. Only while the target window's sessions are hosted here (`SessionHosting::Local`);
  a daemon-hosted window gets `Inert` everywhere for a tab (owner): the native protocol has no verb to merge a tab
  into a split, and a local merge would be undone by the next layout push.
- **R-c. What the merge does.** The dropped tab's whole pane tree becomes the new half at that side of the target
  leaf, at ratio 0.5, with its sessions, active pane and zoom state dropped (a zoomed source shows all its panes); the
  source tab closes without closing a session; an emptied source window closes (`closeWindowIfEmpty`); the grafted
  tree's active leaf becomes the target tab's active pane, and the target tab its window's active tab.
- **R-d. Quoting (owner).** By the pane's foreground process name (Step 2's `foregroundProcessName`): `pwsh`,
  `powershell` → single quotes, `'` doubled; `cmd` → double quotes; anything else (bash, zsh, fish, sh, vim, ssh, …)
  → POSIX: each of `` \t!"#$&'()*,;<=>?[\]^`{|}~`` and space escaped with a backslash, a leading `-` prefixed with
  `./` when the path is relative (never for absolute paths). Paths are joined with one space and followed by one
  space, so typing can go on. A path holding a control character (U+0000–U+001F, U+007F, U+0080–U+009F) refuses the
  whole drop.
- **R-e. As a paste.** File URLs become local paths (`QUrl::toLocalFile`); other URLs and plain text are text. The
  text goes through the session's paste path, so bracketed paste wraps it when the application enabled it, and 3c's
  limits apply. A refused drop posts `NoticeKind::DropRefused` ("Not inserted: a file name holds a control
  character.", 4000 ms, glyph `!`).
- **R-f. Visuals (tokens).** `size.dropRing` (2 · 2 · 2 · 1 cell), `size.dropGhostBorder` (1.5 · 1.5 · 1.5 · 0),
  `space.dropGhostInset` (6 · 4 · 6 · 0), `radius.dropGhost` (6 · 4 · 6 · 0); roles `DropRing` and `DropGhostBorder`
  (accent), `DropGhostFill` (accent at the kit's alpha step nearest 22 %), `DropGhostText` (`label.primary`). The ring
  shows while any accepted drag hovers and the ghost is not shown; the ghost replaces it over an edge band. Terminal
  row: the ring is a one-cell accent frame, the ghost a half-pane box of accent cells with the label centred.

### Non-goals

- Merging tabs on a daemon (a new PDU; R-b). Dropping onto the tab strip (already a reorder or a move). Dragging a
  pane out of a split.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/window/DropZonePolicy.hpp` (create) | Zones, ghost geometry, labels | 1 |
| `src/contour/window/DroppedText.hpp` (create) | Quoting and the dropped text | 1 |
| `src/vtworkspace/Primitives.hpp`, `Pane.{hpp,cpp}`, `SessionModel.{hpp,cpp}`, `ModelEvents.hpp` (modify) | `SplitSide`, the graft, the merge | 2 |
| `src/contour/session/TerminalSessionManager.{hpp,cpp}` (modify) | `mergeTabIntoSplit` | 2 |
| `src/contour/window/WindowController.{hpp,cpp}`, `session/TerminalSession.{hpp,cpp}`, `window/NoticeQueue.hpp` (modify) | Zone queries, performing drops | 3 |
| `src/contour/qml/PaneCard.qml` (modify), token files, `test/QmlMocks.hpp`, `MockMainController.hpp`, snapshot plan (modify) | The drop target, ring and ghost | 4 |

---

### Task 1: `DropZonePolicy` and the dropped text

**Files:**
- Create: `src/contour/window/DropZonePolicy.hpp`, `src/contour/window/DroppedText.hpp`; tests
  `src/contour/window/DropZonePolicy_test.cpp`, `src/contour/window/DroppedText_test.cpp` in `contour_test`.
- Modify: `src/vtworkspace/Primitives.hpp` (`SplitSide`, beside `FocusDirection`, `:52`).

**Interfaces:**
- Produces (`window/DropZonePolicy.hpp`, namespace `contour::window`):
  - `enum class DropPayload : uint8_t { None = 0, Files, Text, Tab };`
  - `enum class SplitSupport : uint8_t { Unsupported = 0, Supported };`, `enum class DropOrigin : uint8_t { OtherTab = 0, OwnTab };`
  - `enum class DropZone : uint8_t { None = 0, InsertText, Inert, SplitLeft, SplitRight, SplitTop, SplitBottom };`
  - `struct DropExtent { double width = 0.0; double height = 0.0; };`, `struct DropPoint { double x = 0.0; double y = 0.0; };`,
    `struct DropRect { double x = 0.0; double y = 0.0; double width = 0.0; double height = 0.0; bool operator==(DropRect const&) const = default; };`
  - `inline constexpr double DropEdgeFraction = 0.20;`
  - `struct DropZoneInputs { DropPayload payload = DropPayload::None; DropExtent pane {}; DropPoint at {}; SplitSupport split = SplitSupport::Unsupported; DropOrigin origin = DropOrigin::OtherTab; };`
  - `[[nodiscard]] constexpr DropZone resolveDropZone(DropZoneInputs const& in) noexcept;` — R-a, R-b; `None` for a
    `None` payload, an empty extent, or a point outside it.
  - `struct DropZoneRow { DropZone zone; std::optional<vtworkspace::SplitSide> side; std::string_view label; };`
    `inline constexpr auto DropZoneRows = std::array { … };` (enumerator order; the four split rows carry their side and
    `"Tab → split left"`, `"Tab → split right"`, `"Tab → split up"`, `"Tab → split down"`; the rest no side, no label).
  - `[[nodiscard]] constexpr std::optional<DropRect> dropGhostOf(DropZone zone, DropExtent pane, double inset) noexcept;`
    — the half of the pane on the zone's side, inset by `inset`; empty for non-split zones.
- Produces (`window/DroppedText.hpp`):
  - `enum class ShellQuoting : uint8_t { Posix = 0, PowerShell, Cmd };`
    `struct ShellQuotingRow { std::string_view process; ShellQuoting quoting; };`
    `inline constexpr auto ShellQuotingRows = std::array { ShellQuotingRow { "pwsh", PowerShell }, { "powershell", PowerShell }, { "cmd", Cmd } };`
  - `[[nodiscard]] constexpr ShellQuoting shellQuotingFor(std::string_view foregroundProcess) noexcept;` — basename, a
    trailing `.exe` ignored in any case; `Posix` when no row matches.
  - `enum class DropTextError : uint8_t { NoPaths = 0, ControlCharacter };`
  - `[[nodiscard]] std::expected<std::string, DropTextError> droppedPathsText(std::span<std::string const> paths, ShellQuoting quoting);`
    — R-d, the paths in UTF-8.
- Produces (`vtworkspace/Primitives.hpp`): `enum class SplitSide : uint8_t { Left = 0, Right, Top, Bottom };` — added
  here because the zone rows name it; Task 2 adds its helpers.

- [ ] **Step 1: Write the failing tests.**
  - `DropZonePolicy_test.cpp` (`[dropzone]`), a 1000 × 500 pane:
    - a tab at (100, 250) → `SplitLeft`; (950, 250) → `SplitRight`; (500, 50) → `SplitTop`; (500, 460) → `SplitBottom`;
      (500, 250) → `Inert` (Review Focus 2); the band edges: x = 199.9 → `SplitLeft`, 200.0 → `Inert`.
    - a corner (50, 20): left distance 50/200 = 0.25, top 20/100 = 0.2 → `SplitTop`; a tie → the vertical edge.
    - files at (50, 250) → `InsertText` ("files never split"); text anywhere → `InsertText`.
    - (Review Focus 3) a tab with `OwnTab` → `Inert` everywhere; with `SplitSupport::Unsupported` → `Inert` everywhere.
    - a point outside, a zero extent, payload `None` → `None`.
    - `dropGhostOf(SplitRight, {1000, 500}, 6) == DropRect { 506, 6, 488, 488 }`; `dropGhostOf(Inert, …)` empty.
    - `DropZoneRows` in enumerator order; the label of `SplitRight` is `"Tab → split right"`.
  - `DroppedText_test.cpp` (`[droppedtext]`):
    - (Review Focus 1) Posix: `"/Users/me/My Report.pdf"` → `"/Users/me/My\ Report.pdf "`; `"/tmp/$(rm -rf ~)"` →
      `"/tmp/\$\(rm\ -rf\ \~\) "`; `` "/tmp/a`b`;c" `` escapes the backticks and `;`; two paths are joined with one space;
      a path holding `"\n"` or `"\x1b"` or U+009B → `ControlCharacter`; no paths → `NoPaths`.
    - (Review Focus 5) `shellQuotingFor("pwsh") == PowerShell`, `("C:\\Windows\\System32\\cmd.exe") == Cmd`,
      `("vim") == Posix`; PowerShell: `"C:\Users\me\it's.txt"` → `"'C:\Users\me\it''s.txt' "`; Cmd: `"C:\My Files\a.txt"`
      → `"\"C:\My Files\a.txt\" "`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `window/DropZonePolicy.hpp` not found.
- [ ] **Step 3: Implement** both headers and `SplitSide`.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[dropzone],[droppedtext]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: where a drop lands on a pane, and dropped paths escaped for its shell, as data" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 2: Merging a tab into a split

**Files:**
- Modify: `src/vtworkspace/Primitives.hpp` (`SplitSide` helpers), `src/vtworkspace/Pane.hpp`, `.cpp` (beside `split`,
  `Pane.hpp:91`), `src/vtworkspace/SessionModel.hpp`, `.cpp` (beside `moveTabToWindow`, `SessionModel.hpp:153`),
  `src/vtworkspace/ModelEvents.hpp`.
- Modify: `src/contour/session/TerminalSessionManager.hpp`, `.cpp` (beside `moveTabToWindow`, `.cpp:1681-1695`).
- Test: `src/vtworkspace/SessionModel_test.cpp`, `src/contour/session/TerminalSessionManager_test.cpp`.

**Interfaces:**
- Consumes: `vtworkspace::SplitSide` (Task 1).
- Produces (`vtworkspace`):
  - `[[nodiscard]] constexpr SplitState splitStateOf(SplitSide side) noexcept;` (Left/Right → `Vertical`, Top/Bottom →
    `Horizontal`), `[[nodiscard]] constexpr GraftOrder graftOrderOf(SplitSide side) noexcept;` with
    `enum class GraftOrder : uint8_t { GraftSecond = 0, GraftFirst };` (Left/Top → `GraftFirst`).
  - `Pane* Pane::graft(std::unique_ptr<Pane> subtree, SplitState state, GraftOrder order, double ratio);` — `@pre isLeaf()`;
    the leaf becomes a split node whose children are its old session's leaf and `subtree`; returns the grafted root.
  - `[[nodiscard]] Pane* SessionModel::mergeTabIntoSplit(WindowId from, TabId source, WindowId to, TabId target, PaneId leaf, SplitSide side);`
    — R-c; nullptr and no change when `source == target`, an id is unknown, or `leaf` is not a leaf of `target`.
  - `virtual void tabMergedIntoSplit(WindowId from, TabId source, WindowId to, TabId target) { … }` on `ModelEvents`,
    defaulting to no-op; the source tab's removal raises what `moveTabToWindow` raises for the source side, without a
    session close (pre-flight names those events).
- Produces (`TerminalSessionManager`):
  `void mergeTabIntoSplit(vtworkspace::WindowId from, int fromIndex, vtworkspace::WindowId to, vtworkspace::TabId target, vtworkspace::PaneId leaf, vtworkspace::SplitSide side);`
  — resolves the tab by row as `moveTabToWindow` does, merges, rebinds the moved sessions to the destination window's
  displays, focuses the grafted active pane, and `closeWindowIfEmpty(from)`.

- [ ] **Step 1: Write the failing tests.**
  - `SessionModel_test.cpp` (`[vtworkspace][model][merge]`):
    - a tab `S` of two panes merged `Right` of the single leaf of tab `T` → `T`'s root is `Vertical`, first child the old
      leaf, second `S`'s former root with both sessions; `S` is gone; no `sessionClosed` event; `T`'s active pane is
      `S`'s former active one.
    - `Left` puts the graft first; `Top`/`Bottom` give `Horizontal`.
    - (Review Focus 3) `source == target` → nullptr, the model unchanged; a leaf of another tab → nullptr.
    - a zoomed source is un-zoomed in the result.
  - `TerminalSessionManager_test.cpp` (`[contour][session][manager][merge]`):
    - two windows, the source window's only tab merged into a pane of the other → the source window is closed, both
      sessions live in the destination, the destination has one more pane.
    - same window: a background tab merged `Bottom` of the active tab's pane → one tab fewer, the split exists.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtworkspace_test contour_gui_test`

  Expected: compile error, no `mergeTabIntoSplit`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vtworkspace/vtworkspace_test "[vtworkspace]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][manager],[contour][multiwindow]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "vtworkspace: merge a whole tab into a split beside a pane, keeping its sessions" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 3: The controller answers and performs drops

> **Depends on Step 2:** `vtpty::Pty::foregroundProcessName()` (Step 2 Task 5). **Depends on Step 3c:** `NoticeRows`,
> `postNotice`, the paste path with its limits (3c Tasks 3, 5–6).

**Files:**
- Modify: `src/contour/window/WindowController.hpp`, `.cpp` — the two invokables below.
- Modify: `src/contour/session/TerminalSession.hpp`, `.cpp` — `insertDroppedUrls`, `insertDroppedText`.
- Modify: `src/contour/window/NoticeQueue.hpp` — row `DropRefused` (Toast, `!`, 4000 ms, None) and
  `dropRefusedText()`.
- Test: `src/contour/window/WindowMenus_test.cpp` or the controller's own test file (pre-flight: where
  `WindowController` invokables are tested), `src/contour/session/TerminalSession_test.cpp`,
  `src/contour/window/NoticeQueue_test.cpp`.

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces (`WindowController`):
  - `[[nodiscard]] Q_INVOKABLE int dropZoneAt(QObject* paneSession, int payload, qreal x, qreal y, qreal width, qreal height, QString const& tabPayload) const;`
    — a `DropZone` as int; `payload` a `DropPayload`; `tabPayload` the drag's `application/x-contour-tab` JSON
    (`windowId`, `tabIndex`), from which `DropOrigin` is decided; `SplitSupport` from the session factory's hosting
    (`Local` → `Supported`).
  - `Q_INVOKABLE void dropTabOnPane(QObject* paneSession, int zone, QString const& tabPayload);` — for a split zone,
    `TerminalSessionManager::mergeTabIntoSplit(…)` with the zone row's side; nothing for `Inert`.
  - `[[nodiscard]] Q_INVOKABLE QString dropGhostLabel(int zone) const;` — the row's label.
- Produces (`TerminalSession`): `Q_INVOKABLE void insertDroppedUrls(QList<QUrl> const& urls);` (local files →
  `droppedPathsText` with `shellQuotingFor(foregroundProcessName())`; other URLs as text; a refusal posts `DropRefused`);
  `Q_INVOKABLE void insertDroppedText(QString const& text);` — both through the session's paste path.

- [ ] **Step 1: Write the failing tests.**
  - Controller (`[windowcontroller][drop]`): `dropZoneAt` for a tab payload from another tab at the right band →
    `SplitRight`; from the pane's own tab → `Inert`; files → `InsertText`. (Review Focus 4) "a daemon-hosted window
    offers no split": a factory reporting `SessionHosting::Remote` → `Inert` at every band, files still `InsertText`.
    `dropTabOnPane(…, SplitRight, …)` merges (the model has the split); `Inert` changes nothing.
  - `TerminalSession_test.cpp` (`[contour][session][drop]`), a `MockPty` whose foreground process is `"zsh"`:
    - (Review Focus 1) "a dropped path arrives as a paste": with `\033[?2004h` set, dropping `file:///Users/me/My%20Report.pdf`
      writes exactly `"\033[200~/Users/me/My\ Report.pdf \033[201~"`; without bracketed paste, the text without the
      markers and no `\r`.
    - a name with a newline writes nothing and the window's toast is `"Not inserted: a file name holds a control character."`.
    - foreground `"pwsh"` → single quotes; an `https://` URL is pasted as its text.
  - `NoticeQueue_test.cpp` (`[notices]`): the row and its text.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `dropZoneAt`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[notices],[dropzone],[droppedtext]"
out/appleclang-debug/src/contour/contour_gui_test "[windowcontroller][drop],[contour][session][drop],[contour][session][clipboard]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: dropped files type their escaped paths; a tab dropped on a pane's edge splits it" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 4: The pane as a drop target, the ring and the ghost

> **Depends on Step 2:** `PaneCard.qml` (Step 2 Task 9), `radius.paneCard`. **Depends on Step 1c:** the accent and
> alpha steps of the token rows.

**Files:**
- Modify: `src/contour/qml/PaneCard.qml` — a `DropArea` (`objectName: "paneDropArea"`, no `keys`, so files, text and
  tabs all enter), a ring (`objectName: "paneDropRing"`) and a ghost (`objectName: "paneDropGhost"`, its label
  `paneDropGhostLabel`). The handlers call two functions on the card that tests call directly:
  `function dropZoneFor(formats, x, y)` (payload from `formats`: `application/x-contour-tab` → Tab, `text/uri-list` with
  file URLs → Files, else Text) and `function performDrop(drop)` (accepts with `Qt.MoveAction` for a tab zone and
  `Qt.CopyAction` otherwise; `Inert` is accepted too, so the tab is not torn off).
- Modify: `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `src/contour/window/ChromeTokenObjects.{hpp,cpp}`,
  `UiStyleProvider.{hpp,cpp}`, `src/contour/test/QmlChromeStyle.hpp` — R-f's tokens and roles on all four rows.
- Modify: `src/contour/test/QmlMocks.hpp` (`MockSession` counts `insertDroppedUrls`/`insertDroppedText`),
  `MockMainController.hpp` (`dropZoneAt` from the real policy, `dropTabOnPane` recorded, `dropGhostLabel`).
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` — `ScriptAction` gains
  `HoverDropFiles` and `HoverDropTabRight`; rows `DropFilesHover` (`"drop-files-hover"`, single leaf) and
  `DropTabSplit` (`"drop-tab-split"`, single leaf).
- Test: `src/contour/test/QmlComponents_test.cpp`, `src/contour/config/ChromeTokenGroups_test.cpp`,
  `ChromeSnapshotPlan_test.cpp`, `ChromeSnapshot_test.cpp`.

**Interfaces:**
- Consumes: Task 3's invokables, Task 1's zones (as ints).
- Produces: the QML above.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`): R-f's values per row.
  - `QmlComponents_test.cpp` (`[contour][gui][qml][drop]`), a 1000 × 500 pane card:
    - `dropZoneFor(["text/uri-list"], 50, 250)` → `InsertText`, the ring visible with `border.width == 2`, no ghost;
      `performDrop` with two file URLs counts one `insertDroppedUrls` holding both.
    - `dropZoneFor(["application/x-contour-tab"], 950, 250)` → `SplitRight`; the ghost visible at
      `(506, 6, 488, 488)`, its label `"Tab → split right"`, the ring hidden; `performDrop` records one
      `dropTabOnPane(…, SplitRight, …)` and accepts with `Qt.MoveAction`.
    - (Review Focus 2) "a tab dropped in the middle stays put": at (500, 250) → `Inert`, no ghost, `performDrop`
      accepts with `Qt.MoveAction` and records nothing, and the mock controller's `tearOffTab` was not called.
    - leaving the area hides the ring and the ghost.
    - Under `ContourTui`: the ring is one cell wide; the ghost is cell-aligned and its label centred.
  - `ChromeSnapshotPlan_test.cpp` / `ChromeSnapshot_test.cpp`: both rows build on the MacOS and Terminal rows without a
    QML warning; `drop-files-hover` finds `paneDropRing` visible, `drop-tab-split` finds `paneDropGhost` visible.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][drop]"`

  Expected: FAIL, `paneDropArea` not found.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][drop],[contour][gui][qml][panecard],[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][drop],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS; `PaneCard.qml`'s QML baselines do not go up.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: panes take dropped files and tabs, with a ring and a half-pane ghost" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 5: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts`), apply its
  findings, re-run the filters of every task it touched, and commit once with the Signed-off-by line, staging only
  the files it changed.
- [ ] **Step 2: Every filter once more.**

```bash
out/appleclang-debug/src/contour/contour_test "[dropzone],[droppedtext],[notices],[chrometokens],[snapshot]"
out/appleclang-debug/src/vtworkspace/vtworkspace_test "[vtworkspace]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][manager],[windowcontroller][drop],[contour][session][drop],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][drop],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, including the Terminal row** (the Step 6.1 plan's Task 6 Step 3 loop). Expected: the count
  is `SnapshotScripts.size() × 4 × 2 × 2`; `drop-files-hover.*` shows the 2-pt ring, `drop-tab-split.*` the right-half
  ghost with "Tab → split right"; on the Terminal row a one-cell frame and a cell box.
- [ ] **Step 4: Owner manual checks** (batched for Phase 6; list them in the ledger, do not wait): drag
  `~/Desktop/My Report.pdf` from Finder onto a zsh pane → `~/Desktop/My\ Report.pdf ` at the prompt, not run; drag a
  file into vim's command line (`:e `) → the escaped path; drag a tab onto another window's pane edge → the ghost, then
  a split, the source window closes if it was its only tab; release a tab in a pane's middle → nothing moves; drag a
  file to a pane's edge → no ghost, the path inserts; a daemon-hosted window → no ghost for tabs.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  "Inserts the shell-escaped paths of files dropped on a pane." and "Splits a pane when a tab is dropped on its outer
  edge, showing where the tab will go; a tab dropped in a pane's middle stays where it was."
- [ ] **Step 6:** Mark Step 6.4 done in the plans index's step table and *Last update*, and on the board with
  `/status-board update`. Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-4.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the render path; one policy call per drag move.
  - **Risk:** medium. Tab drops over a pane no longer tear the tab off; the model gains a graft operation; dropped
    text reaches the PTY (escaped, as a paste).
  - **Coverage:** the zone and quoting tables, the model merge, the controller and session drop cases, the QML drop
    cases on the native and Terminal rows, two snapshot scripts.
- [ ] **Step 7:** Run `/code-review` on the whole step and address every finding.

---

## Owner decisions

The run proceeds on each recommended default.

1. **A tab in a pane's middle is accepted and does nothing (R-a).** *Default: yes, "a sloppy drop cannot rearrange the
   window".* Today it tears the tab into a new window; dropping on the desktop still does.
2. **No split zones in a daemon-hosted window (R-b).** *Default: none until the native protocol has a merge verb.*
   The alternative adds that PDU here (a `vthost` change and a capability).
3. **Quoting by the foreground process (R-d).** *Default: PowerShell and cmd rows, POSIX for the rest.*
4. **A control character refuses the whole drop (R-d).** *Default: refuse, with a toast.* The alternative is `$'…'`
   quoting, which only bash and zsh read.
5. **Text and web URLs are pasted (R-e).** *Default: yes, through the paste path and its limits.*

## Spec gaps found

- §3.16 does not say what a tab dropped in a pane's middle does; today's tear-off would make it a rearrangement
  (owner 1).
- The native protocol has no verb to merge a tab into a split, so daemon-hosted windows cannot do it (owner 2).
- "Shell-escaped" does not name a shell; the pane's foreground process decides (owner 3).
- Corners of two edge bands are unspecified (R-a); the canvas's ring and ghost tokens are not in the kit (R-f).
