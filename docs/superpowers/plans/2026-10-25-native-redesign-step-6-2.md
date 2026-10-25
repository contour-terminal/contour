# Native Redesign — Step 6.2 (Moments: Restore) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling). Line numbers are at HEAD `661b6029`, before 1c (in progress), 1d, 2, 3a–3f,
4b, 5a and 5b land. Where this step uses code they add, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before 6.2 executes.

**Goal:** A pane the daemon restored shows a quiet "Restored · session ended 14:02" divider between the history it
brought back and its new prompt, and the first window shown after a restore says "Restored 2 windows, 7 tabs",
once.

**Architecture:**
- **The divider is terminal content, seeded by the daemon.** PR #2176 already writes a dim marker under the restored
  rows (`vthost::restoredMarker`, `HistorySeed.cpp:103-106`: `── restored · snapshot of YYYY-MM-DD HH:MM ──`).
  This step replaces it with a pure, width-aware `restoredDivider`: the canvas's words, centred between `─` rules
  across the pane, in SGR dim. It scrolls with the history, is copyable, and draws the same on every row, the
  Terminal row included.
- **The summary rides the hello.** No restore event reaches a client today (3c R-l). `proto::ServerHello` gains two
  appended counts; the host hands them to the first accepted `Attach` connection after it realized a restore and
  then forgets them, which is "one Toast per relaunch".
- **The GUI posts 3c's row.** `NativeController` reads the counts from the hello; `TerminalSessionManager` holds
  them until the first window of the new layout is shown, then posts `NoticeKind::RestoreSummary` with
  `restoreSummaryText(windows, tabs)` there.

**Tech Stack:** C++23, Qt 6.11, Catch2 v3, the native protocol (`vthost::proto`), CMake build tree
`out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.2 (`:819`):
"Daemon restore (PR #2176), plus a "Restored · session ended hh:mm" divider and one Toast", seam none. It binds with
what it cites: §1 principles (`:28-64`; 5, nothing over non-blank cells), §2.5–§2.6 snapshots and testing
(`:187-226`), §5 (`:844-854`). The design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md` ("Moments: first run, restore, …"). The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies are in `.superpowers/design/canvas/project/`:
`moments` board 4 ("Relaunch brings back windows, tabs, splits, cwd and scrollback … a quiet divider marks the
seam"), `daemon` board 6 ("It adds one Toast per relaunch: “Restored 2 windows, 7 tabs”. Panes that were closed
stay closed, and killed ones come back with the footer" — the footer is Step 6.3's), `kit-components` (Toast),
`tui`. Step 3c's plan ships the toast row (`2026-10-25-native-redesign-step-3c.md` R-l, Task 5). The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

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

- **The spec, verbatim (§3.16 row 6.2):** "Daemon restore (PR #2176), plus a "Restored · session ended hh:mm"
  divider and one Toast". Canvas copy: divider "Restored · session ended 14:02"; toast "Restored 2 windows, 7 tabs".
- **Canvas values** (`moments` 4): the divider is a centred 11-pt label in secondary text between two hairlines;
  in a grid that is SGR 2 (dim) text between `─` rules (R-a).
- **Binding rulings** (ledgers in `docs/superpowers/ledgers/`, `.superpowers/sdd/*/progress.md`):
  - Time is read through `core::platform::IWallClock`, injected (§1.4: DI for every OS service).
  - Every visual surface is a snapshot row (`ChromeSnapshotPlan.hpp`), and every visual task renders the Terminal
    row (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`). `[display]` tests are opt-in and skip offscreen.
  - The protocol grows only by appending fields at a PDU's end; a decoder gives a missing trailing field its default
    (`Pdu.hpp:16-19`). Renumbering a tag takes a version step.
  - A task touching `vthost` also runs `ctest --test-dir out/appleclang-debug -L daemon`.
  - `enum class` over `bool`, the default at zero; `std::expected` for fallible calls.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven (the divider's two time forms are a table; the notice is 3c's row);
  dependency injection (the wall clock); configuration at construction; Doxygen on every new public declaration;
  zero warnings and no `NOLINT`.
- **Build** with every core; the tree is configured with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds at once.
- **Lint and format** before each commit: `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug
  --extra-arg=-isysroot$(xcrun --show-sdk-path) <touched .cpp files>`; `/opt/homebrew/opt/llvm@22/bin/clang-format -i`
  on touched `.cpp`/`.hpp`; `editorconfig-checker` on touched `CMakeLists.txt`.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new member gets `{}` or a value;
  every new table row names every field.
- **Qt-free stays Qt-free:** `vthost` links no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, and never carries `Co-Authored-By`.

## Review Focus

1. **The toast comes back on every relaunch of the GUI, every reconnect, or after `contour ls`.** Expected: only
   the first accepted `Attach` connection after the daemon realized a restore is told; a second client, a
   reconnect to the same daemon run, and every `Control` connection read zero counts. Pinned in Task 2 ("the
   summary is told once").
2. **The divider breaks in a narrow pane, or after a resize.** Expected: at the restore width the text is centred
   between rules that end at the last column; narrower than the text plus two cells it is the text alone; a later
   narrower or wider resize never wraps it onto a second row. Pinned in Task 1.
3. **A snapshot from yesterday reads as today.** Expected: "session ended 14:02" only when the snapshot was taken on
   the daemon's current local day; otherwise "session ended 2026-10-24 14:02". Pinned in Task 1.
4. **Mixed versions after an upgrade**, which is exactly when a drift restart restores: a new GUI against an old
   daemon, an old GUI against a new one. Expected: an old daemon's hello decodes with zero counts and no toast; an
   old client ignores the appended fields; nothing refuses the connection. Pinned in Task 2
   (`Compatibility_test.cpp`).
5. **The toast is posted before any window exists, or into a window about to close.** A GUI connected while the
   daemon restarts sees its old windows lost and the restored ones arrive (`NativeController::adoptReconnect`,
   `NativeController.cpp:255-326`). Expected: the toast lands in the first restored window once it is shown, never
   in a lost one, and is not dropped. Pinned in Task 3.

Also pinned, beyond the five: windows whose every pane failed to spawn are not counted (Task 2); a pane restored
with no history still gets the divider on its first row (Task 1).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. The divider stays in the grid (owner).** It is drawn where today's marker is, by the daemon, so a client of
  any version shows it, it scrolls with the history it marks, and the Terminal row draws it unchanged. Shape: one
  row, `columns` cells wide: `─` rules, a space, the text, a space, rules; the left rule takes the floor of the
  leftover cells, the right the rest; the whole row in SGR 2. When `columns` is less than the text's width plus 2,
  the row is the text alone. Written with autowrap off (`CSI ? 7 l` … `CSI ? 7 h`) and followed by CR LF, and the
  row is then marked not wrappable, so reflow cuts it instead of wrapping it. **Pre-flight:** confirm reflow of a
  line whose `LineFlag::Wrappable` is cleared keeps it one row (`grid/Line.hpp:348-353`); if not, keep the row at
  the restore width and accept the cut.
- **R-b. "Session ended" is the snapshot's `takenAt`** (`SessionSnapshot.hpp:98`), the last write before the daemon
  ended — the shutdown's final write, or on a crash the newest periodic one (`interval`, 30 s). Read in the daemon's
  local time zone (`vthost::localTimeOf`, `WorkspacePersistence.hpp:306`).
- **R-c. Two forms (owner).** Same local day as `now` → `Restored · session ended {:%H:%M}`; otherwise
  `Restored · session ended {:%Y-%m-%d %H:%M}`. `now` comes from an injected `core::platform::IWallClock`;
  production passes `core::platform::defaultSystemWallClock()` at the daemon's composition root.
- **R-d. The counts are what the host realized.** A window counts when at least one of its tabs was created; a tab
  when at least one of its panes spawned. Panes the user closed are not in the snapshot, so they stay closed.
- **R-e. Telling it once.** `SessionHost` keeps `std::optional<RestoredCounts>` from the moment the restore is
  realized; the hello of the first accepted connection whose role is `Attach` carries it and the host clears it.
  `ServerHello` appends `uint32_t restoredWindows = 0; uint32_t restoredTabs = 0;` after `daemonInstance`; zero
  means "nothing to tell". No capability is needed: appended fields are read as their defaults by an older peer.
- **R-f. Where the toast goes.** `NativeController` emits `workspaceRestored(windows, tabs)` once per hello that
  carries counts. `TerminalSessionManager` keeps it pending until the first window created from the following
  layout is shown, then posts the 3c row to that window (`postNotice(notice, <a session of that window>)`). A
  pending summary is replaced by a newer one.

### Non-goals

- The killed panes' footer (Step 6.3). Restoring windows' positions on screen (`WindowPositionStore` already does).
- Telling a client what a restore could not bring back (a failed spawn is logged by the daemon, as today).

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/vthost/HistorySeed.{hpp,cpp}` (modify) | `RestoreDivider`, `restoredDividerText`, `restoredDivider`; the seed writes it | 1 |
| `src/vthost/SessionHost.hpp` (`WorkspaceRestore`, `:621-627`), `SessionHost.cpp` (modify) | Carry the divider; count what was realized; tell it once | 1, 2 |
| `src/vthost/WorkspacePersistence.{hpp,cpp}`, `src/vthost/Daemon.cpp` (modify) | `prepareRestore` takes the wall clock | 1 |
| `src/vthost/RestoredCounts.hpp` (create) | `RestoredCounts` | 2 |
| `src/vthost/proto/Pdu.{hpp,cpp}`, `proto/PduTrace.cpp` (modify) | The hello's two fields | 2 |
| `src/vthost/client/NativeClient.{hpp,cpp}` (modify) | `restoredCounts()` | 2 |
| `src/contour/remote/NativeController.{hpp,cpp}`, `session/TerminalSessionManager.{hpp,cpp}` (modify) | The toast | 3 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | The `restore-summary` row | 3 |

---

### Task 1: The divider

**Files:**
- Modify: `src/vthost/HistorySeed.hpp`, `.cpp` — `restoredMarker` (`.hpp:28-31`, `.cpp:103-106`) goes; the seed
  (`.cpp:91-101`) writes the divider.
- Modify: `src/vthost/SessionHost.hpp` — `WorkspaceRestore::marker` becomes `RestoreDivider divider {};`;
  `SessionHost.cpp` — the seed call.
- Modify: `src/vthost/WorkspacePersistence.hpp`, `.cpp` — `prepareRestore(store, config, report, core::platform::WallClockRef wall)`;
  `src/vthost/Daemon.cpp` — passes `core::platform::defaultSystemWallClock()`.
- Test: `src/vthost/HistorySeed_test.cpp` (the case at `:45` is rewritten), `src/vthost/WorkspaceRestore_test.cpp`
  (`:216-233` asserts the new text), `src/vthost/WorkspacePersistence_test.cpp`.

**Interfaces:**
- Produces (`vthost/HistorySeed.hpp`):
  - `struct RestoreDivider { std::chrono::local_seconds sessionEndedAt {}; std::chrono::local_days today {}; bool operator==(RestoreDivider const&) const = default; };`
  - `[[nodiscard]] std::string restoredDividerText(RestoreDivider const& divider);` — R-c.
  - `[[nodiscard]] std::string restoredDivider(RestoreDivider const& divider, vtbackend::ColumnCount columns);` — the
    bytes R-a writes, CR LF included.
  - `void seedRestoredHistory(vtbackend::Terminal& terminal, proto::Delta const& history, RestoreDivider const& divider);`
    — as today, then the divider at the terminal's own width on the page's top row, which is then marked not
    wrappable.
- Produces (`WorkspacePersistence.hpp`): `prepareRestore` fills `divider` from `localTimeOf(snapshot.takenAt)` and
  `floor<days>(localTimeOf(floor<seconds>(wall.now())))`.

- [ ] **Step 1: Write the failing tests** (`[vthost][snapshot][seed]`, `[vthost][snapshot][restore]`):
  - `restoredDividerText({ 2026-10-25 14:02:31, 2026-10-25 })` is `"Restored · session ended 14:02"`; with `today`
    2026-10-26 it is `"Restored · session ended 2026-10-25 14:02"`.
  - `restoredDivider(d, 40)`: between `"\033[?7l\033[2m"` and `"\033[m\033[?7h\r\n"`, the text has a display width of
    exactly 40 cells, the text is centred with one space either side, the left rule is `(40 − 30 − 2) / 2` `─`.
    `restoredDivider(d, 20)` holds the text alone, no `─`.
  - (Review Focus 2) seed a 40 × 10 terminal: row 0 reads the divider; resize to 30 columns, then to 60: the divider
    is still one row, and the row below it is the new prompt's.
  - `WorkspaceRestore_test.cpp`: a restored pane's row 0 contains `"Restored · session ended"`, under the history.
    A pane restored with no history still has it on row 0.
  - `WorkspacePersistence_test.cpp`: `prepareRestore` with a `ManualWallClock` set to the next day gives the dated
    form.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vthost_test`

  Expected: compile error, no `RestoreDivider`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vthost/vthost_test "[vthost][snapshot]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "vthost: a restored pane shows \"Restored · session ended hh:mm\" across its width" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 2: The host tells the first client what it restored

**Files:**
- Create: `src/vthost/RestoredCounts.hpp` (list it in `vthost`'s sources).
- Modify: `src/vthost/proto/Pdu.hpp` (`ServerHello`, `:261-298`), `Pdu.cpp` (its encode and decode rows),
  `proto/PduTrace.cpp` (the trace prints both counts).
- Modify: `src/vthost/SessionHost.hpp`, `.cpp` — count what the restore realized (where `rerunInputFor` is read,
  `SessionHost.cpp:738`, walks the restored panes); `[[nodiscard]] std::optional<RestoredCounts> takeRestoreAnnouncement();`.
- Modify: where the daemon builds an accepted `ServerHello` (pre-flight: `grep -n "ServerHello {" src/vthost/*.cpp`)
  — fills the counts from `takeRestoreAnnouncement()` for an `Attach` role only.
- Modify: `src/vthost/client/NativeClient.hpp`, `.cpp` — `[[nodiscard]] std::optional<RestoredCounts> restoredCounts() const noexcept;`
  beside `daemonInstance()` (`NativeClient.hpp:482`); empty for zero counts.
- Test: `src/vthost/proto/Pdu_test.cpp`, `src/vthost/proto/Compatibility_test.cpp`, `src/vthost/SessionHost_test.cpp`,
  `src/vthost/Daemon_test.cpp`.

**Interfaces:**
- Produces: `struct RestoredCounts { uint32_t windows = 0; uint32_t tabs = 0; bool operator==(RestoredCounts const&) const = default; };`
  in `namespace vthost`.
- Produces (`proto::ServerHello`): `uint32_t restoredWindows = 0;` and `uint32_t restoredTabs = 0;`, appended after
  `daemonInstance`, varint-encoded like the fields before them.
- Produces (`SessionHost`): the counts per R-d, kept until the first `takeRestoreAnnouncement()`.

- [ ] **Step 1: Write the failing tests.**
  - `Pdu_test.cpp` (`[vthost][proto]`): "every catalog PDU round-trips" covers a hello with counts `{2, 7}`.
  - `Compatibility_test.cpp` (Review Focus 4): a hello encoded without the two fields (the bytes before them) decodes
    with zeros; a hello with them decodes in the previous layout's reader with its other fields intact.
  - `SessionHost_test.cpp` (`[vthost][snapshot][restore]`): restoring a snapshot of two windows with three and four
    tabs gives `takeRestoreAnnouncement() == RestoredCounts { 2, 7 }`, then empty; a window whose every pane's spawn
    fails (a factory that refuses its sessions) is not counted.
  - `Daemon_test.cpp` (`[vthost][daemon][restore]`), Review Focus 1, "the summary is told once": after a restore, a
    `Control` connection's hello reads zeros; the first `Attach` connection's reads `{2, 7}`; a second `Attach` and a
    reconnect of the first read zeros; `NativeClient::restoredCounts()` of the first is `{2, 7}`, of the others empty.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vthost_test`

  Expected: compile error, no `restoredWindows`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vthost/vthost_test "[vthost][proto],[vthost][snapshot],[vthost][daemon]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "vthost: the first client after a restore is told how many windows and tabs came back" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 3: One toast in the first restored window

> **Depends on Step 3c:** `NoticeKind::RestoreSummary`, `restoreSummaryText`, `TerminalSessionManager::postNotice`,
> `WindowController::notices()` (3c Tasks 5–6).

**Files:**
- Modify: `src/contour/remote/NativeController.hpp`, `.cpp` — after a hello (the first attach, `:207`, and
  `adoptReconnect`, `:255`), a signal `void workspaceRestored(quint32 windows, quint32 tabs);` (reactor thread,
  connect queued) when `restoredCounts()` holds a value.
- Modify: `src/contour/session/TerminalSessionManager.hpp`, `.cpp` — `void noteWorkspaceRestored(vthost::RestoredCounts counts);`
  keeps it pending; the first window shown after it (where a controller's window is first exposed — pre-flight
  names the hook) posts the notice there and clears it.
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` — row `RestoreSummary`
  (`"restore-summary"`, single leaf, the scene posts `restoreSummaryText(2, 7)`).
- Test: `src/contour/remote/NativeController_test.cpp`, `src/contour/session/TerminalSessionManager_test.cpp`,
  `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Consumes: Task 2 (`NativeClient::restoredCounts()`, `vthost::RestoredCounts`).
- Produces: `NativeController::workspaceRestored`, `TerminalSessionManager::noteWorkspaceRestored`; the wiring in the
  attach path connects the first to the second.

- [ ] **Step 1: Write the failing tests.**
  - `NativeController_test.cpp` (`[attach][controller][restore]`), against the in-process daemon the file already
    stands up: a daemon that restored `{2, 7}` → one `workspaceRestored(2, 7)`; a second controller attached to it →
    none; a hello with zeros → none.
  - `TerminalSessionManager_test.cpp` (`[contour][session][manager][notices][restore]`):
    - "the toast waits for a window": `noteWorkspaceRestored({2, 7})` with no window → nothing posted; the first
      window is shown → its `notices().toastText() == "Restored 2 windows, 7 tabs"`; a second window shows nothing.
    - (Review Focus 5) "a reconnect to a restarted daemon puts the toast in a restored window": windows A and B lost,
      C and D arrive, then the summary → the toast is in C (the first shown), A and B never held one.
    - `{1, 1}` reads `"Restored 1 window, 1 tab"` (3c's singular forms).
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): the row's fields; `ChromeSnapshot_test.cpp`
    (`[contour][gui][qml][snapshot]`): it builds on the MacOS and Terminal rows without a QML warning and finds
    `noticeToast` with that text.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `workspaceRestored`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[attach][controller],[contour][session][manager][notices],[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: one \"Restored 2 windows, 7 tabs\" toast after the daemon brings a workspace back" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 4: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts`), apply its
  findings, re-run the filters of every task it touched, and commit once with the Signed-off-by line, staging only
  the files it changed.
- [ ] **Step 2: Every filter once more.**

```bash
out/appleclang-debug/src/vthost/vthost_test "[vthost][snapshot],[vthost][proto],[vthost][daemon]"
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[attach][controller],[contour][session][manager],[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, including the Terminal row** (the Step 6.1 plan's Task 6 Step 3 loop). Expected: the count
  is `SnapshotScripts.size() × 4 × 2 × 2`; `restore-summary.*` shows the toast in the band (MacOS) and in the bar row
  (Terminal). The divider is grid content and the snapshot terminal is a stub, so it is not in the pictures; Task 1
  pins it.
- [ ] **Step 4: Owner manual checks** (batched for Phase 6; list them in the ledger, do not wait): with
  `daemon.mode: auto`, open two windows with splits, run `make test` in one pane, then `contour daemon stop
  --keep-snapshot` (or reboot); launch Contour → the windows come back, each pane's history above
  "Restored · session ended hh:mm" across its width, one toast "Restored 2 windows, 7 tabs"; quit and relaunch the GUI
  without stopping the daemon → no toast; `ui_style: terminal` → the same divider, the toast in the bar.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  "Marks the history a restored pane brings back with a \"Restored · session ended hh:mm\" divider." and "Says once
  how many windows and tabs the daemon restored."
- [ ] **Step 6:** Mark Step 6.2 done in the plans index's step table and *Last update*, and on the board with
  `/status-board update`. Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-2.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none; one row written per restored pane, two varints per hello.
  - **Risk:** low. The hello grows by two appended fields (compatibility-tested both ways); the marker text changes.
  - **Coverage:** the divider's text and geometry, the host's counts and its once-only rule, the controller and
    manager cases, one snapshot script.
- [ ] **Step 7:** Run `/code-review` on the whole step and address every finding.

---

## Owner decisions

The run proceeds on each recommended default.

1. **The divider is a grid row, not chrome (R-a).** *Default: grid row in SGR dim with `─` rules*, the closest a
   cell grid gets to the canvas's hairlines and 62 % label; it works for every client version and the Terminal row.
   The alternative is a chrome divider drawn over a reserved row, which needs a new line mark on the wire.
2. **The date form (R-c).** *Default: hh:mm on the same local day, else `YYYY-MM-DD hh:mm`.*
3. **What is counted (R-d).** *Default: what the host realized*, so the toast never claims a window that failed.
4. **Who is told (R-e).** *Default: the first accepted Attach connection*; a GUI started later against the same
   daemon run is not told.
5. **The daemon's time zone (R-b).** *Default: the daemon's*, which is the machine's for a local daemon.

## Spec gaps found

- Nothing records when a session ended; the snapshot's `takenAt` is the best bound (R-b), up to `interval` early
  after a crash.
- No restore event reaches the client (3c R-l); the hello's two appended fields carry it (R-e).
- "One Toast" is per relaunch per the `daemon` board, but a GUI relaunch against a running daemon is no restore; the
  step tells only the first client after a restore.
- The canvas's divider (hairlines at 12 %, a 62 % label) cannot be drawn exactly in cells; SGR dim and `─` are used
  (owner 1).
