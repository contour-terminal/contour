# Native Redesign — Step 6.3 (Moments: the Exited Process, after the Daemon Restarts) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling). Line numbers are at HEAD `661b6029`, before 1c (in progress), 1d, 2, 3a–3f,
4b, 5a and 5b land. Where this step uses code they add, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before 6.3 executes. Step 6.2 is
assumed done (it touches the same restore path).

**Goal:** When the daemon restarts, a restored pane whose command the restart stopped shows Step 2's footer —
"“make test” was stopped when the session ended." · Run Again · Dismiss — and a pane cut off from its daemon
offers Reconnect instead of Close alone.

**Architecture:**
- **The daemon says what it stopped.** At restore, PR #2176 starts every pane with a fresh shell and retypes only an
  allow-listed program (`vthost::rerunInputFor`, `RerunPolicy.hpp`). For a pane whose snapshot names a running
  command it did not retype, the hosted session keeps that command line as its *interruption*;
  `proto::SessionState` carries it to clients (one appended string); the host clears it when the pane's next
  command starts.
- **One footer, more rows.** Step 2's `window/PaneState.hpp` table (`PaneFooterRows`) gains the action each row
  offers and its label; a second, Live-shell cause (`PaneFooter::Interrupted`) shows the same `ExitedFooter.qml`
  while the shell runs, without taking keys from it. Run Again goes through Step 3c's guarded `RerunCommand` ask.
- **Reconnect is a footer action.** The Disconnected row (Step 2 R-j: "no Restart; the daemon owns it") offers
  Reconnect when the window is attached to a daemon; it restarts the attach (`NativeController::reconnect()`),
  shows Step 2's reconnecting pill meanwhile, and leaves the footer with the refusal's reason if it fails.

**Tech Stack:** C++23, Qt 6.11 (Quick), QML, Catch2 v3, the native protocol, CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.3 (`:820`):
"Rerun of the `ExitedFooter` from Step 2 when the daemon restarts", seam none. It binds with what it cites: §3.6
(Step 2's `ExitedFooter`, `:469-519`), §1 principles (`:28-64`), §2.6 (`:206-226`), §5 (`:844-854`). The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; local copies in `.superpowers/design/canvas/project/`:
`moments` board 5 ("A pane-level footer instead of “press any key”. Return restarts with the same command and cwd"),
`daemon` boards 2 ("When the supervisor gives up, the pane shows Moments' footer: “Disconnected · Reconnect ↩ ·
Close ⌘W”") and 6 ("killed ones come back with the footer"), `tui`. Deferred here by earlier plans: Step 2's
non-goal "Reconnect from the footer (Moments 6.3, once a reconnect API exists)" and Step 1c's owner decision 2
("Reconnect arrives with the Moments footer"). Step 3c's guarded `RerunCommand` ask is consumed. The plans index is
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

- **The spec, verbatim (§3.16 row 6.3):** "Rerun of the `ExitedFooter` from Step 2 when the daemon restarts".
  Canvas copy: "Disconnected · Reconnect ↩ · Close ⌘W" (`daemon` 2); "Restart ↩" (accent) · "Close ⌘W"
  (`moments` 5).
- **Binding rulings** (ledgers, `.superpowers/sdd/*/progress.md`, Step 2's plan):
  - Step 2 R-j: an ended pane swallows typed text; Return restarts when offered; the `ClosePane` binding closes.
  - Step 1c D2: no dead buttons — an action that cannot run is not shown.
  - No colour or size literal in chrome QML; every visual surface is a snapshot row and every visual task renders
    the Terminal row (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`). `[display]` tests skip offscreen.
  - The protocol grows only by appending fields (`Pdu.hpp:16-19`). A task touching `vthost` runs
    `ctest --test-dir out/appleclang-debug -L daemon`.
  - What an application or a snapshot says about a command line is untrusted: shown through
    `vtbackend::CommandLineText::display()`, run only after the user's answer (3c's `RerunCommand` default is Deny).
  - `enum class` over `bool`, zero the default; QML-facing booleans are the carve-out. `std::expected` for fallible
    calls. Owner manual checks are batched per phase.
- **AGENT.md applies in full:** data-driven (the footer rows hold message, action, label and dismissal); dependency
  injection; configuration at construction; Doxygen; zero warnings and no `NOLINT`.
- **Build** with every core; the tree is configured with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds at once.
- **Lint and format** before each commit: `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug
  --extra-arg=-isysroot$(xcrun --show-sdk-path) <touched .cpp files>`; `clang-format -i` (llvm@22) on touched
  `.cpp`/`.hpp`; `editorconfig-checker` on touched `.qml` and `CMakeLists.txt`. Never clang-format a `.qml` file.
- **Tests:** each task runs only its own filter; the full suite runs in CI.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every row of `PaneFooterRows` and
  `SnapshotScripts` names every field.
- **Qt-free stays Qt-free:** `window/PaneState.hpp` and everything in `vthost` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task, staging only that task's files by name, each message ending with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, never `Co-Authored-By`.

## Review Focus

1. **A forged or hostile command line in the snapshot** (OSC 133 `cmdline_url` with escapes, a bidi override, a
   newline, `; rm -rf ~`). Expected: the footer shows the sanitized display form, cut to 48 bytes; Run Again asks
   through 3c's Dialog, which shows the command and defaults to Deny; nothing is typed without the press. Pinned in
   Task 2 ("a hostile interruption is shown sanitized and asked about").
2. **Return at the restored prompt fires Run Again.** The shell is live. Expected: every key, Return included,
   reaches the shell; Run Again is a click or the footer's own keyboard focus. Pinned in Task 2 ("Return goes to the
   shell").
3. **The footer sticks, or comes back on every attach after the user moved on.** Expected: it clears when the pane's
   next command starts, on the daemon too, so a later attach does not bring it back; Dismiss clears it in this client.
   Pinned in Tasks 1 and 2.
4. **Reconnect against a daemon that is gone for good, or refuses** (an upgrade's version step). Expected: the pane
   shows the reconnecting pill while trying; a refusal puts the footer back with the reason; no window is duplicated
   and no session is lost. Pinned in Task 3.
5. **A re-run program also shows the footer.** An allow-listed program (`daemon.restore.rerun_programs`) is retyped
   by the daemon. Expected: no footer for it; one for the pane next to it whose command was not allowed. Pinned in
   Task 1.

Also pinned, beyond the five: an ended pane's footer wins over an interruption (Task 2); the Terminal row draws both
new rows with bracketed buttons (Task 2).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. What "the ExitedFooter when the daemon restarts" means (owner).** A restored pane is live: PR #2176 starts a
  fresh shell in it. "Killed ones come back with the footer" is read as: a pane whose snapshot names a running
  command (`PaneSnapshot::runningCommandLine`, `SessionSnapshot.hpp:41`) that `rerunInputFor` did not retype comes
  back with its shell and Step 2's footer naming that command. Restoring the pane *ended* (no shell until Restart)
  would need a parked hosted session, a protocol and lifecycle change; not here.
- **R-b. The footer over a live shell takes no keys.** Unlike Step 2's ended pane, nothing is swallowed and Return
  belongs to the shell. The footer's buttons are clicked or reached with Tab from the footer itself; its primary
  button shows no `↩`.
- **R-c. Run Again asks.** It calls the session's rerun ask with the stored command line — the path 3c's
  `RerunCommand` uses (`TerminalSession::operator()(actions::RerunCommand)`), factored into
  `requestRerun(vtbackend::CommandLineText commandLine)` — so the Dialog shows the line and defaults to Deny; Allow
  types it at the prompt and enters it, as 3c's `applyPendingRerun` does.
- **R-d. When it clears.** The daemon clears a session's interruption when that session's next command starts (its
  first new `Running` block after the restore); the client clears it on the same event, on Dismiss, and on Run Again
  once the user allowed it.
- **R-e. Copy.** `"“{}” was stopped when the session ended."` with the command's display form cut to
  `platform::MaxCommandNameBytes` (48) by `vtbackend::truncateUtf8`; buttons "Run Again" and "Dismiss". The
  Disconnected row keeps Step 2's message and offers "Reconnect ↩" and "Close ⌘W" (the chord from Step 2's
  `shortcutText("ClosePane")`).
- **R-f. Reconnect (owner).** Offered only when the app is attached to a daemon (`ReconnectSupport::Supported`).
  `NativeController::reconnect()` starts a new supervised run with `AttachIntentKind::Resume` of the windows it
  followed; meanwhile every Disconnected pane shows Step 2's reconnecting pill. When the layout comes back, each
  Disconnected pane whose remote session the layout still names is rebound in place (Step 2's
  `SessionModel::replaceLeafSession`, keeping the `PaneId`); one it does not name stays ended. A refusal leaves the
  footer with `"Could not reconnect: {reason}."`. **Pre-flight:** confirm how `RemoteLayout` (`RemoteLayout.cpp`)
  maps a pushed session onto an existing leaf whose local session ended; if it cannot, rebind through
  `restartSession` with the remote session id.
- **R-g. One table.** `PaneFooterRows` (Step 2) gains columns; the Interrupted cause is a row of its own, so a fourth
  cause is one more row.

### Non-goals

- A parked, shell-less restored pane (R-a). Re-running on the daemon side without the user (the allow-list does that).
- Reconnecting a tmux control-mode window (its gateway has no supervisor).

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/vthost/proto/Pdu.{hpp,cpp}`, `proto/PduTrace.cpp` (modify) | `SessionState::interruptedCommand` | 1 |
| `src/vthost/SessionHost.{hpp,cpp}` (modify) | Set at restore; clear on the next command; state it in snapshots | 1 |
| `src/vthost/client/ScreenMirror.{hpp,cpp}` (modify) | Expose it to the mirror's owner | 1 |
| `src/contour/window/PaneState.hpp` (modify, Step 2's) | `PaneFooter`, `FooterAction`, `FooterDismiss`, row columns | 2 |
| `src/contour/session/TerminalSession.{hpp,cpp}` (modify) | The footer properties, Run Again, Dismiss | 2, 3 |
| `src/contour/qml/ExitedFooter.qml`, `qml/PaneCard.qml` (modify, Step 2's) | Generic rows | 2 |
| `src/contour/remote/NativeController.{hpp,cpp}`, `session/TerminalSessionManager.{hpp,cpp}` (modify) | Reconnect | 3 |
| `src/contour/test/QmlMocks.hpp`, `ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | Mocks, snapshot rows | 2, 3 |

---

### Task 1: The daemon keeps what it stopped

**Files:**
- Modify: `src/vthost/proto/Pdu.hpp` (`SessionState`, `:499-568`), `Pdu.cpp`, `proto/PduTrace.cpp`.
- Modify: `src/vthost/SessionHost.hpp`, `.cpp` — where a restored pane's `initialInput` is chosen
  (`SessionHost.cpp:738`) the session keeps `runningCommandLine` when `rerunInputFor` gave nothing; the snapshot
  builder states it; the block-change listener clears it on the first new `Running` block.
- Modify: `src/vthost/client/ScreenMirror.hpp`, `.cpp` — `[[nodiscard]] std::string const& interruptedCommand() const noexcept;`
  from each adopted `SessionState`, cleared by the mirror on the first `Running` block it adopts live after it.
- Test: `src/vthost/proto/Pdu_test.cpp`, `src/vthost/proto/Compatibility_test.cpp`,
  `src/vthost/WorkspaceRestore_test.cpp`, `src/vthost/client/ScreenMirror_test.cpp`.

**Interfaces:**
- Produces (`proto::SessionState`): `std::string interruptedCommand = {};` appended after `commandBlocks`; empty means
  none; at most `vtbackend::MaxRecordedCommandLineBytes` bytes (cut on decode, as the other command lines are).
- Produces (`SessionHost`): `[[nodiscard]] std::string interruptedCommandOf(SessionId session) const;` (empty when none).
- Produces (`ScreenMirror`): `interruptedCommand()` as above.

- [ ] **Step 1: Write the failing tests.**
  - `Pdu_test.cpp` (`[vthost][proto]`): a `SessionState` with `interruptedCommand = "make test"` round-trips.
  - `Compatibility_test.cpp`: a `SessionState` encoded without the field decodes with it empty.
  - `WorkspaceRestore_test.cpp` (`[vthost][snapshot][restore]`), with a snapshot of two panes, one running
    `make test` (not allowed), one running `htop` with `rerunPrograms = {"htop"}`:
    - (Review Focus 5) `interruptedCommandOf(first) == "make test"`, `interruptedCommandOf(second)` empty, and the
      second's PTY received `"htop\r"`;
    - the first's next `OSC 133 ; C` clears it, and a new attach's `SessionState` for it carries `""` (Review Focus 3).
  - `ScreenMirror_test.cpp` (`[vthost][mirror]`): a mirror fed a `SessionState` with the field reports it; a
    later live `Running` block clears it; a resync's `SessionState` with `""` clears it too.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vthost_test`

  Expected: compile error, no `interruptedCommand`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vthost/vthost_test "[vthost][proto],[vthost][snapshot],[vthost][mirror]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "vthost: a restored pane remembers the command the restart stopped until its next one" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 2: The footer over a restored shell

> **Depends on Step 2:** `window/PaneState.hpp` (`SessionEnd`, `PaneFooterRows`, `restartOf`), `TerminalSession`'s
> `sessionEnd`/`endMessage`/`restartOffer`/`restart()`/`dismiss()`, `ExitedFooter.qml` and its place in
> `PaneCard.qml` (Step 2 Tasks 10–11). **Depends on Step 3c:** the rerun ask (`requestPermissionForRerunCommand`,
> `applyPendingRerun`, the `RerunCommand` row). **Depends on Step 1c:** `chipOpenGlyph`/`chipCloseGlyph`.

**Files:**
- Modify: `src/contour/window/PaneState.hpp`; test `src/contour/window/PaneState_test.cpp` (`[panestate]`).
- Modify: `src/contour/session/TerminalSession.hpp`, `.cpp` — the remote-hosted session reads its mirror's
  `interruptedCommand()` (pre-flight: the binding that owns the session's `ScreenMirror`); the footer properties;
  `requestRerun` factored out of `operator()(actions::RerunCommand)`.
- Modify: `src/contour/qml/ExitedFooter.qml` — binds the generic properties below instead of `endMessage` and
  `restartOffer`; `src/contour/qml/PaneCard.qml` — shows it while `session.paneFooter !== 0`.
- Modify: `src/contour/test/QmlMocks.hpp` (`MockSession`: the new properties, the two invokables counted),
  `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (`SnapshotPaneEnd` gains `Interrupted`; row
  `PaneInterrupted`, `"pane-interrupted"`, `SplitThree`).
- Test: `src/contour/session/TerminalSession_test.cpp`, `src/contour/test/QmlComponents_test.cpp`,
  `Accessibility_qml_test.cpp`, `ChromeSnapshotPlan_test.cpp`, `ChromeSnapshot_test.cpp`.

**Interfaces:**
- Produces (`window/PaneState.hpp`):
  - `enum class PaneFooter : uint8_t { None = 0, Ended, Interrupted };`
  - `enum class InterruptionPresence : uint8_t { Absent = 0, Present };`
  - `[[nodiscard]] constexpr PaneFooter paneFooterOf(SessionEnd end, InterruptionPresence interruption) noexcept;` — `Ended`
    when `end != None`, else `Interrupted` when present, else `None`.
  - `enum class FooterAction : uint8_t { None = 0, Restart, Reconnect, RunAgain };`,
    `enum class FooterDismiss : uint8_t { Close = 0, Dismiss };`
  - `PaneFooterRow` gains `FooterAction action; std::string_view actionLabel; FooterDismiss dismiss;`. Rows:
    `None` (None, "", Close); `Exited`, `Signalled`, `StartFailed` (Restart, "Restart", Close); `Disconnected`
    (Reconnect, "Reconnect", Close — offered per Task 3, until then `restartOf` keeps it `Unavailable`).
  - `inline constexpr auto InterruptedFooterRow = PaneFooterRow { … };` — `RunAgain`, "Run Again", `Dismiss`, message
    `"“{}” was stopped when the session ended."`.
  - `[[nodiscard]] std::string interruptedFooterMessage(vtbackend::CommandLineText const& commandLine);` — R-e.
- Produces (`TerminalSession`, all `NOTIFY paneFooterChanged`, which also fires with `sessionEndChanged`):
  `Q_PROPERTY(int paneFooter …)`, `Q_PROPERTY(QString footerMessage …)`, `Q_PROPERTY(QString footerActionLabel …)`
  (empty when the row's action is not offered), `Q_PROPERTY(QString footerDismissLabel …)` ("Close" plus the chord, or
  "Dismiss"), `Q_PROPERTY(int footerTakesReturn …)` (1 for `Ended`, 0 for `Interrupted`);
  `Q_INVOKABLE void runFooterAction();` (Restart → `restart()`; RunAgain → `requestRerun(interruption)`; Reconnect →
  Task 3), `Q_INVOKABLE void dismissFooter();` (Ended → `dismiss()`; Interrupted → clears it here);
  `void requestRerun(vtbackend::CommandLineText commandLine);`.

- [ ] **Step 1: Write the failing tests.**
  - `PaneState_test.cpp` (`[panestate]`): `paneFooterOf` over every `SessionEnd` × presence; every row names an action
    label exactly when its action is not `None`; `interruptedFooterMessage` of `"make test"` is
    `"“make test” was stopped when the session ended."`; of a 200-byte line it ends in `…` within 48 bytes of command.
  - `TerminalSession_test.cpp` (`[contour][session][paneend][interrupted]`), a remote-hosted session whose mirror
    reports `"make test"`:
    - `paneFooter == int(PaneFooter::Interrupted)`, `footerActionLabel == "Run Again"`, `footerDismissLabel == "Dismiss"`.
    - (Review Focus 2) "Return goes to the shell": a Return press writes `"\r"` to the PTY and starts no ask.
    - "Run Again asks first": `runFooterAction()` → `pendingRerunCommandLine() == "make test"`, nothing written; Allow
      types `"make test\r"`; the footer clears.
    - (Review Focus 1) "a hostile interruption is shown sanitized and asked about": a mirror reporting
      `"echo ‮evil; rm -rf ~\n"` → `footerMessage` holds no U+202E and no newline; `runFooterAction()` writes
      nothing before the answer; Deny writes nothing after it.
    - (Review Focus 3) "the next command clears it": a live `Running` block → `paneFooter == 0`; `dismissFooter()` on
      another such session clears it too.
    - "an ended pane's footer wins": the PTY closes while interrupted → `Ended`, Step 2's message and Restart offer.
  - `QmlComponents_test.cpp` (`[contour][gui][qml][paneend]`): with the mock at `Interrupted`, `exitedFooter` is visible
    under the cells, `exitedFooterMessage.text` the message, `exitedFooterRestart.text == "Run Again"` (no `↩`), clicking
    it counts one `runFooterAction()`, `exitedFooterClose.text == "Dismiss"`; Step 2's cases pass with the renamed
    properties. Under `ContourTui`, the footer is `cellHeight` high and both buttons bracketed.
  - `Accessibility_qml_test.cpp`: the Interrupted footer is an `AlertMessage` named by the message.
  - `ChromeSnapshotPlan_test.cpp` / `ChromeSnapshot_test.cpp`: `pane-interrupted` builds on MacOS and Terminal rows and
    finds `exitedFooter`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `PaneFooter`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[panestate],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][paneend],[contour][gui][qml][paneend],[contour][gui][qml][a11y],[contour][gui][qml][snapshot],[contour][session]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][paneend],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: a restored pane names the command the restart stopped, with Run Again and Dismiss" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 3: Reconnect from the footer

> **Depends on Step 2:** `SessionEnd::Disconnected`, the reconnecting pill (`session.isReconnecting`),
> `replaceLeafSession` (Step 2 Tasks 10–11).

**Files:**
- Modify: `src/contour/remote/NativeController.hpp`, `.cpp` — `[[nodiscard]] std::expected<void, ReconnectError> reconnect();`
  with `enum class ReconnectError : uint8_t { NotAttached = 0, AlreadyConnected };`; a new supervised run with
  `Resume` of the followed windows; state reported through the existing `connectionStateChanged`.
- Modify: `src/contour/session/TerminalSessionManager.hpp`, `.cpp` — `void reconnectHost();`,
  `[[nodiscard]] window::ReconnectSupport reconnectSupport() const noexcept;` (Supported while attached to a daemon);
  the rebinding of R-f when the layout returns.
- Modify: `src/contour/window/PaneState.hpp` — `enum class ReconnectSupport : uint8_t { Unsupported = 0, Supported };`,
  `[[nodiscard]] constexpr FooterAction offeredAction(SessionEnd end, RestartSupport restart, ReconnectSupport reconnect) noexcept;`
  (the row's action, or `None` when its support is missing).
- Modify: `src/contour/session/TerminalSession.cpp` — `runFooterAction()` for Reconnect → `_manager->reconnectHost()`;
  Return on a Disconnected footer reconnects; the message becomes `"Could not reconnect: {reason}."` after a refusal.
- Test: `src/contour/window/PaneState_test.cpp`, `src/contour/remote/NativeController_test.cpp`,
  `src/contour/session/TerminalSessionManager_test.cpp`, `src/contour/test/QmlComponents_test.cpp`.

**Interfaces:**
- Consumes: Task 2 (`FooterAction::Reconnect`, `runFooterAction`), Step 2 (`replaceLeafSession`, the pill).
- Produces: as listed in *Files*.

- [ ] **Step 1: Write the failing tests.**
  - `PaneState_test.cpp`: `offeredAction(Disconnected, Unsupported, Supported) == Reconnect`, `(…, Unsupported) == None`;
    `(Exited, Supported, Unsupported) == Restart`.
  - `NativeController_test.cpp` (`[attach][controller][reconnect]`), with the file's in-process daemon: a supervisor
    that ended `Refused` → `reconnect()` succeeds and reaches `Connected`; while connected it gives
    `AlreadyConnected`; a controller never attached gives `NotAttached`.
  - `TerminalSessionManager_test.cpp` (`[manager][paneend][reconnect]`):
    - "Reconnect rebinds the panes in place": two Disconnected panes in a split; `runFooterAction()` on one → both show
      `isReconnecting` while it runs; once the layout returns, both leaves keep their `PaneId`s, carry live sessions and
      `paneFooter == 0`; the window count is unchanged.
    - (Review Focus 4) "a refused reconnect keeps the footer": the daemon refuses → both panes `Ended`, message
      `"Could not reconnect: …"`, no new window.
    - "a pane the daemon no longer has stays ended": the returned layout omits one session → that pane keeps its footer.
  - `QmlComponents_test.cpp` (`[contour][gui][qml][paneend]`): a Disconnected mock with `footerActionLabel ==
    "Reconnect"` shows `exitedFooterRestart` reading `"Reconnect ↩"`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `reconnect`.
- [ ] **Step 3: Implement** as stated, after the pre-flight check of R-f.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[panestate]"
out/appleclang-debug/src/contour/contour_gui_test "[attach][controller],[manager][paneend],[contour][gui][qml][paneend],[manager]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: a pane cut off from its daemon offers Reconnect" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 4: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts`), apply its
  findings, re-run the filters of every task it touched, and commit once with the Signed-off-by line, staging only
  the files it changed.
- [ ] **Step 2: Every filter once more.**

```bash
out/appleclang-debug/src/vthost/vthost_test "[vthost][proto],[vthost][snapshot],[vthost][mirror]"
out/appleclang-debug/src/contour/contour_test "[panestate],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][paneend],[attach][controller],[manager],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][paneend],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, including the Terminal row** (the Step 6.1 plan's Task 6 Step 3 loop). Expected: the count
  is `SnapshotScripts.size() × 4 × 2 × 2`; `pane-interrupted.*` shows the footer under the cells with Run Again and
  Dismiss; `pane-exited.*` (Step 2) is unchanged.
- [ ] **Step 4: Owner manual checks** (batched for Phase 6; list them in the ledger, do not wait): with
  `daemon.mode: auto`, run `sleep 600` in one pane and `htop` in another with `daemon.restore.rerun_programs: [htop]`;
  `contour daemon stop --keep-snapshot`; relaunch → htop runs again with no footer; the other pane shows "“sleep 600”
  was stopped when the session ended." with Run Again (asks, then runs) and Dismiss; typing a new command clears it.
  Kill the daemon hard and keep it from restarting (rename the socket's directory) → Disconnected footer with
  Reconnect; restore the directory and press Reconnect → the panes come back in place.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  "Names the command a daemon restart stopped in each restored pane, with Run Again (after asking) and Dismiss." and
  "Offers Reconnect in a pane that lost its daemon connection."
- [ ] **Step 6:** Mark Step 6.3 done in the plans index's step table and *Last update*, and on the board with
  `/status-board update`. Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-3.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none; one string per restored session state.
  - **Risk:** medium. `SessionState` grows by one appended field; the footer's properties are renamed; Reconnect
    restarts the attach and rebinds panes in place.
  - **Coverage:** the wire field both ways, the host's set-and-clear, the mirror, the footer table, the session cases
    (Return, the ask, sanitizing), the reconnect cases, one snapshot script.
- [ ] **Step 7:** Run `/code-review` on the whole step and address every finding.

---

## Owner decisions

The run proceeds on each recommended default.

1. **The footer shows over a live, restored shell (R-a).** *Default: yes; the pane keeps its fresh shell.* The
   alternative restores such a pane ended, with Restart starting its shell and retyping the command: a parked hosted
   session, a larger daemon change.
2. **Run Again always asks (R-c).** *Default: yes, through 3c's Dialog (default Deny).* The command line came from the
   terminal and may be forged.
3. **Reconnect belongs to this step (R-f).** *Default: yes, as Step 2 and 1c deferred it here.* The alternative is a
   PR of its own after 6.3.
4. **When the interruption clears (R-d).** *Default: on the next command, on both ends; Dismiss in this client only.*
5. **The copy (R-e).** *Default: "“make test” was stopped when the session ended." · Run Again · Dismiss.*

## Spec gaps found

- "Rerun of the `ExitedFooter`" is ambiguous; Step 2's footer is for ended sessions, and a restored pane's shell is
  live (R-a, R-b).
- No per-pane record of a stopped command reaches the client; `SessionState` gains one field (Task 1).
- No reconnect API exists; Step 2 and 1c deferred Reconnect to this step (R-f).
- What a successful reconnect does to the ended panes (rebind in place versus new windows) is unspecified (R-f).
