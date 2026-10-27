# Pre-flight scan: Step 6.3 (Moments: the exited process after a daemon restart, and Reconnect)

Plan: `docs/superpowers/plans/2026-10-25-native-redesign-step-6-3.md` (written at `661b6029`).
Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.3 (`:820`, binding),
§3.6 (`:469-519`), §1, §2.6, §5. Index: `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.
Rules: `AGENT.md`. Ledger: `.superpowers/sdd/2026-10-25-native-redesign-step-6-3/progress.md` (rulings D1, D2, and
the note "Step 2 named the enum EndReason … 6.3's tasks use EndReason").

Scanned against `work/native-redesign-step-2` at `324c198c`, read in `../contour-step2`. Not landed, read from plans,
pre-flights and rulings: 3b–3f, 4b, 5a, 5b; and the other 6.x plans for overlap. Nothing was built or run.

**Counts:** 4 tasks; 24 pair rows (12 inside 6.3 / across 6.x, 12 with 3b–5b); 18 findings, 5 of them blocking;
3 spec asks that no task delivers; 8 owner questions.

---

## 1. Tasks that share a file or an interface

### 1a. Inside 6.3, and across the Phase 6 moments

| Pair | Shared file / interface | Status |
|---|---|---|
| 1 → 2 | `proto::SessionState::interruptedCommand` → `ScreenMirror::interruptedCommand()` → `TerminalSession` | The mirror is reactor-owned, the session GUI-only (**F3**) |
| 1 → 1 | `SessionHost.cpp:738` seeder → the hosted session's interruption | The seeder never sees a session id; carry it in `SessionSpawnRequest` (**F10**) |
| 2 → 3 | `FooterAction::Reconnect`, `runFooterAction()`, `offeredAction(...)` | Built on `SessionEnd`/`PaneFooterRow` columns that moved (**F1**) |
| 2 ↔ 3 | `TerminalSession.{hpp,cpp}`, `window/PaneState.hpp`, `PaneState_test.cpp` | Sequential |
| 2 ↔ Step 2 | `ExitedFooter.qml` (label `qsTr("Restart %1").arg(chromeStyle.restartGlyph)`, `chordFits`, `closeChord`), `PaneCard.qml:83-90,161` | The plan's `footerDismissLabel` "Close plus the chord" would undo Step 2's narrow-pane fix `4316b617` (**F8**) |
| 3 ↔ Step 2 | `window::paneStateOf(end, connection)`: Ended wins over Reconnecting | The pill cannot show on Disconnected panes during Reconnect (**F4**) |
| 1 ↔ 6.2 T1/T2 | `SessionHost.cpp` restore seeder (`:704-764`), `WorkspaceRestore_test.cpp` | 6.2 lands first; additive |
| 1 ↔ 6.2 T2 | `proto/Pdu.{hpp,cpp}`, `PduTrace.cpp`, `Pdu_test.cpp`, `Compatibility_test.cpp` | Different PDUs; filter (**F11**) |
| 1 ↔ 6.2 F5 | A seeded Running record under the divider | If 6.2 abandons it, 6.3's "first new Running block" is clean (**F9**) |
| 2 ↔ 6.1 T4 | `PaneCard.qml` bottom chrome: the shell-integration card hides on an ended footer | Widen 6.1's rule to `paneFooter !== 0` here |
| 3 ↔ 6.2 T3 | `NativeController` (6.2's `workspaceRestored`; 6.3's `reconnect()`) | An in-place rebind shows no new window; 6.2's pending toast goes stale (6.2 F1) |
| 2 ↔ 6.1/6.2/6.4 | `QmlMocks.hpp`, `ChromeSnapshotPlan.hpp` (`SnapshotPaneEnd` gains `Interrupted`) | Append in order |

### 1b. With Steps 3b–5b

| Pair | Shared file / interface | Status |
|---|---|---|
| 3c T3/T4 → 6.3 T2 | The rerun ask: 3c removes `requestPermissionForRerunCommand` and `pendingRerunCommandLine` (3c `:445`, `:512`), drops `Q_INVOKABLE` from `applyPendingRerun`, adds `answerPermission(PermissionDecision, PermissionMemory)`, `pendingPermissionAsk()`, `pendingPermission` (`rerunPrompt`: "Run this command again?", default Deny) | The plan's tests name the removed API (**F2**) |
| 3c Q5 → 6.3 T2 | `PasteStrip` | Not used; OK |
| 3f T12 → 6.3 T1 | `CommandBlockEnd::Abandoned`, `abandonRunningCommand()`; 3f Q6 "superseded" on the wire | Interplay with the seeded Running record (**F9**) |
| 3f T12 → 6.3 T2 | 3f names `window::SessionEnd` (3f `:974`) — also stale | Same rename as F1 |
| 3e R-l → 6.3 T2 | The size pill on a page resize | The Interrupted footer appears over a live shell: the pill flashes, the fresh shell gets SIGWINCH (**F12**) |
| 3e Q2 → 6.3 T2 | Link pill bottom-right of the grid | Above the footer; OK |
| 4b → 6.3 T2 | Settings preview session (4b T13) | Never remote; OK |
| 5a/5b → 6.3 T4 | Extra snapshot passes | Count (**F13**) |
| 1c D2 → 6.3 T3 | "the Disconnected chip has no Reconnect button until something can reconnect" | Now something can (**F15**) |
| Step 2 R-j / D3 → 6.3 T2/T3 | Ended pane swallows keys; bare unclaimed Return restarts when offered | Disconnected Return → Reconnect is a behaviour change to say aloud |
| Step 2 ruling (Disconnected Close-only) → 6.3 T3 | `PaneFooterRows[Disconnected].restart = Unavailable`, message "…reattach with: contour client" | The message contradicts a Reconnect button (**F7**) |
| Owner rule 17:25 → 6.3 T4 | One targeted display pass | (**F13**) |

---

## 2. One row per task

| Task | References today | Self-consistency / issues |
|---|---|---|
| 1 daemon keeps it | OK: `SessionState` (`Pdu.hpp:499-567`, `commandBlocks` last, encoded after `commandBlocksFloor`, `Pdu.cpp`), `PaneSnapshot::runningCommandLine` (`SessionSnapshot.hpp:43`, cited `:41`), `rerunInputFor` (`RerunPolicy.hpp`, used at `SessionHost.cpp:738`), `MaxRecordedCommandLineBytes` (`CommandBlock.hpp:71`), `ScreenMirror` (`client/ScreenMirror.hpp:154`, `applySessionState :240`); tags `[vthost][proto]`, `[vthost][snapshot][restore]`, `[vthost][mirror]` ✓ | Seeder shape (**F10**); filter (**F11**); Running record (**F9**) |
| 2 footer | OK, but moved: `session/EndReason.hpp` holds `EndReason {None, Exited, Signalled, StartFailed, Disconnected}`, `FooterRestart`, `RestartSupport`, `PaneFooterRow {end, restart, message, messageWithDetail}`, `PaneFooterRows`, `restartOf`, `paneFooterMessage`; `window/PaneState.hpp` holds `PaneState`/`paneStateOf`. `TerminalSession` `sessionEnd`/`endMessage`/`restartOffer` (`:271-273`), `restart()`/`dismiss()` (`:821/:825`), `paneState` (`:267`). `checkRerun` (`TerminalSession.cpp:2092`) needs `shellIsPrompting()`. `platform::MaxCommandNameBytes` (`FinishNotification.hpp:105`), `vtbackend::truncateUtf8` (`CommandBlock.hpp:254`). ExitedFooter objectNames `exitedFooterRestart`, `exitedFooterClose`, `exitedFooterMessage` ✓. Tags `[contour][gui][qml][paneend]` (13) ✓; `[contour][session][paneend]` does not exist yet (Step 2's session cases are `[contour][session][start]`, `[manager][paneend]`) | **F1**, **F2**, **F3**, **F6**, **F8** |
| 3 Reconnect | OK: `ConnectionState {Connecting, Connected, Reconnecting, Failed, Refused, Stopped}`; `AttachIntentKind::Resume` (`Pdu.hpp:203`); `NativeController::onConnectionState` (Refused → `State::Failed`, `connectionClosed`), `_supervisor` (non-owning, `NativeController.hpp:616`); `SessionModel::replaceLeafSession` (`SessionModel.hpp:208`); `restartSession` → `std::expected<TerminalSession*, RestartError>` (`TerminalSessionManager.hpp:478`); `applyRemoteLayout` binds leaves by the pty's binding (`RemoteLayout.cpp:113`) and closes panes whose remote session vanished. `[attach][controller][reconnect]` exists (9 cases of the automatic reconnect) | **F4**, **F5**, **F7** |
| 4 finish | — | **F13** |

**DI.** No new OS service. **Threading.** The mirror (reactor) → session (GUI) handoff is unspecified (**F3**);
`reconnect()` must run its supervisor start on the reactor. **Real sessions.** The session cases need
`MockPtySessionFactory` with `reportedHosting = Remote`; `NativeController_test`'s daemon serves `MockPty`.
The plan does not say how a `TerminalSession_test` gets "a remote-hosted session whose mirror reports" (**F3**).
**QML literals.** "Run Again", "Dismiss", "Reconnect" come from the C++ rows (good). **bool parameters.** None; the
`footerTakesReturn` int is fine. **Display tests.** Offscreen per task; Finish's loop (**F13**).
**Renamed names.** `SessionEnd`→`EndReason` (`session/`); `PaneFooterRows`/`restartOf` moved to
`session/EndReason.hpp`; `session.isReconnecting` is not a `Q_PROPERTY` (QML reads `paneState === paneStates.reconnecting`);
`requestPermissionForRerunCommand`/`pendingRerunCommandLine` removed by 3c.

---

## 3. Findings

### Blocking

**F1 — The footer table moved and its shape changed (Tasks 2, 3; ledger note).**
The plan says: "`window/PaneState.hpp` (`SessionEnd`, `PaneFooterRows`, `restartOf`)", "`PaneFooterRow` gains
`FooterAction action; std::string_view actionLabel; FooterDismiss dismiss;`", "`inline constexpr auto
InterruptedFooterRow = PaneFooterRow { … }`", and "`offeredAction(SessionEnd end, RestartSupport restart, ReconnectSupport reconnect)`".
*Today:* `session/EndReason.hpp` holds `EndReason`, `PaneFooterRow { end, restart, message, messageWithDetail }`,
`FooterRestart` and `restartOf`. A `PaneFooterRow` needs an `end`, but no `EndReason` describes an interruption. QML
reads `restartOffer` against `chromeStyle.paneEnds.restartOffered` (`ExitedFooter.qml`; `UiStyleProvider.hpp:251`), and
13 `[contour][gui][qml][paneend]` cases, `MockSession`, and the `pane-exited` snapshot (`SnapshotExitMessage`) read
these names.
*Resolution:* put the columns where the table is. `session/EndReason.hpp` rows gain `FooterAction action` (replacing
`FooterRestart` with `{None, Restart, Reconnect}`) and the label. The Interrupted footer gets its own small row type
(`InterruptionFooter { message, actionLabel = "Run Again", dismiss = Dismiss }`). `PaneFooter`/`paneFooterOf` stay in
`window/PaneState.hpp` beside `PaneState`. `offeredAction(EndReason, RestartSupport, ReconnectSupport)` replaces
`restartOf`. Migrate `restartOffer`/`paneEnds` and every reader in the same commit. 3f Task 12's text uses the same
stale `window::SessionEnd`.

**F2 — The rerun ask the tests drive is gone after 3c, and it refuses without a prompt mark (Task 2).**
The plan says "`runFooterAction()` → `pendingRerunCommandLine() == "make test"`, nothing written; Allow types
`"make test\r"`", and "**Depends on Step 3c:** the rerun ask (`requestPermissionForRerunCommand`, `applyPendingRerun`, …)".
*After 3c:* the signal and `pendingRerunCommandLine` are removed, and the ask is read through
`pendingPermissionAsk() == GuardedRole::RerunCommand` and `pendingPermission()["lines"]`. It is answered through
`answerPermission(PermissionDecision::Allow, PermissionMemory::Once)`.
*Also today:* `operator()(RerunCommand)` and `applyPendingRerun` both run `checkRerun`, which refuses unless
`terminal.shellIsPrompting()` (`TerminalSession.cpp:2092-2100`, `:2131`). A restored pane's fresh shell without
shell integration never reports a prompt, so Run Again asks, the user allows, and nothing is typed (only a log line).
That is a dead button (1c D2).
*Resolution:* rewrite the tests against 3c's API. `requestRerun(CommandLineText)` keeps `checkRerun`. Offer
`FooterAction::RunAgain` only while the session has seen a prompt mark (`commandBlocks().current()` prompting);
otherwise the Interrupted row offers Dismiss alone and says nothing about running. Owner Q2.

**F3 — The interruption crosses threads, and the session test has no mirror (Tasks 1, 2).**
The plan says "the remote-hosted session reads its mirror's `interruptedCommand()` (pre-flight: the binding that owns
the session's `ScreenMirror`)".
*Today:* each binding owns `std::unique_ptr<vthost::client::ScreenMirror> mirror` (`NativeController.hpp:397`). It is
applied on the reactor thread in `onUpdate` (`NativeController.cpp:355`). Step 2 keeps the footer state GUI-only
(`_end`, "GUI thread only"; `_footerLines` measured on the GUI thread per Task 13).
*Resolution:* when an adopted `SessionState` or a live `Running` block changes the interruption, `NativeController`
posts it to the GUI thread (`QMetaObject::invokeMethod(session, …, Qt::QueuedConnection)`). `TerminalSession` keeps
`std::optional<vtbackend::CommandLineText> _interruption`, GUI-only, set through
`void noteInterruption(std::optional<vtbackend::CommandLineText>)`. The session tests call that seam on a
`MockPtySessionFactory{ reportedHosting = Remote }` session. One `[attach][controller]` case covers the
controller's posting, and one `[threading]` case runs under TSan.

**F4 — The reconnecting pill cannot show on a Disconnected pane (Task 3, R-f).**
The plan says "meanwhile every Disconnected pane shows Step 2's reconnecting pill", and the test "both show
`isReconnecting` while it runs".
*Today:* `paneStateOf(end, connection)` returns `Ended` whenever `end != None` (`window/PaneState.hpp:31-39`), and the
pill shows on `paneState === paneStates.reconnecting` (`PaneCard.qml:108`). `isReconnecting()` is a C++ method, not a
property.
*Resolution:* add a row. `EndReason::Disconnected` plus a connection that is `Connecting`/`Reconnecting` gives
`Reconnecting`. While the manual reconnect runs, the ended session's `_hostConnection` follows the new supervised
run. Update `PaneState_test`, and assert `paneState` in the tests.

**F5 — Reconnect after `Refused` has no path back to the panes (Task 3, R-f).**
The plan says "When the layout comes back, each Disconnected pane whose remote session the layout still names is
rebound in place … **Pre-flight:** confirm how `RemoteLayout` maps a pushed session onto an existing leaf whose local
session ended; if it cannot, rebind through `restartSession` with the remote session id."
*Pre-flight result:* it cannot. `applyRemoteLayout` matches leaves through `controller.sessionForPty(&session->terminal().device())`
(`RemoteLayout.cpp:113`). An ended pane's pty has no live binding, so the layout would treat its session as new and
close the ended leaf. On `Refused`, `onConnectionState` sets `State::Failed` and emits `connectionClosed`
(`NativeController.cpp:226-246`). `reconnect()` must therefore restart a supervisor the controller only borrows
(`_supervisor`, non-owning) and reset `_state`. Because `_acceptedConnections != 0`, the hello takes the
`adoptReconnect` path, which judges windows lost by `daemonInstance`.
*Resolution:* use the plan's fallback. Before applying the layout, the manager rebinds each Disconnected leaf whose
remote session id the layout names: `setNextBindSession(remote)` + `restartSession`/`replaceLeafSession`, keeping
the `PaneId`. Leaves it does not name keep their footer. Name who owns the new `ClientSupervisor` run (the
controller, on the reactor), and test a refused-then-accepted sequence on `DaemonFixture`.

### Non-blocking

**F6 — Test tags and filters (Tasks 2, 3).** `[contour][session][paneend]` is new: Step 2's session cases are
`[contour][session][start]` and `[manager][paneend]`, so Task 2's filter re-runs none of Step 2's. Add
`[contour][session][start],[manager][paneend]`. `[attach][controller][reconnect]` already holds 9 automatic-reconnect
cases; tag the new ones `[attach][controller][reconnect][manual]`.

**F7 — The Disconnected message contradicts a Reconnect button (Task 3).** It says "…Its sessions are still running;
reattach with: contour client" (`EndReason.hpp`). When Reconnect is offered, use "Disconnected from the Contour
daemon. Its sessions are still running." (a `messageWithReconnect` column); keep the reattach hint when it is not.

**F8 — Keep the chord out of `footerDismissLabel` (Task 2).** The plan says "`Q_PROPERTY(QString footerDismissLabel …)`
("Close" plus the chord, or "Dismiss")". Step 2's final fix (`4316b617`) has QML drop the chord when Restart, Close
and the chord do not fit (`chordFits`, `closeChord`). A C++ label carrying the chord brings the overflow back. Return
"Close" or "Dismiss", and let QML add the chord as today. Likewise, `footerActionLabel` stays glyph-free, and QML
appends `restartGlyph` only when `footerTakesReturn`.

**F9 — The seeded Running record (Task 1; 6.2 F5; 3f T12).** The restored history may hold the snapshot's Running block.
"Clears on the first new `Running` block after the restore" must exclude adopted records: compare against the
highest id adopted at seed time. Better, 6.2 abandons the seeded record.

**F10 — Carry the interruption in `SessionSpawnRequest` (Task 1).** The seeder (`SessionHost.cpp:717-751`) builds a
request and calls `seedSession`; it never sees the new `SessionId`. Add `std::string interruptedCommand {}` beside
`restoredMarker`/`initialInput`, and store it on the hosted session in `seedSession`.

**F11 — Compatibility cases are `[vthost][negotiation]` (Task 1, Finish).** They are outside the `vthost_test`
filters; add the tag (CI's `-L daemon` runs them anyway).

**F12 — The Interrupted footer resizes a live shell at attach (Task 2; 3e R-l).** The fresh shell gets SIGWINCH as
soon as the footer appears, and 3e's size pill flashes. See 6.1 F11: suppress the pill for chrome-band resizes.

**F13 — Finish's display pass (Task 4 Step 3).** One targeted pass: `pane-interrupted` and `pane-exited` (changed
properties), native and Terminal rows, per the owner rule. The count formula ignores 5a/5b's passes.

**F14 — Return on a Disconnected footer now reconnects (Task 3).** Step 2 R-j/D3 swallow it today. Say so in the
release notes and in the footer's `Accessible.description` ("Return reconnects.").

**F15 — The host chip (1c D2).** Now that something can reconnect, D2's condition ends: the chip could offer
Reconnect as well. Owner Q5.

**F16 — Snapshot and mock (Task 2).** `SnapshotPaneEnd { None, Exited }` (`ChromeSnapshotPlan.hpp:157`) gains
`Interrupted`. `MockSession` gains `paneFooter`, `footerMessage`, `footerActionLabel`, `footerDismissLabel`,
`footerTakesReturn`, `runFooterAction`, `dismissFooter`, and loses `restartOffer` if F1 migrates it.

**F17 — `interruptedFooterMessage` cut (Task 2).** The plan says it "ends in `…` within 48 bytes of command". The
display form must be cut after `display()` sanitizes it: sanitizing can grow the text (replacement glyphs). Test a
line of controls.

**F18 — Line drift.** `Pdu.hpp:499-568` → `:499-567`; `SessionSnapshot.hpp:41` → `:43`; Step 2's footer files: locate
by objectName.

---

## 4. Spec asks that no task delivers

| # | Ask | Source | Note |
|---|---|---|---|
| S1 | `moments` 5: "Return restarts with the same command and cwd" | canvas | R-b gives Return to the live shell instead (ruled D1); Run Again is click/focus only |
| S2 | §1 principle 2/§5: the TUI row for the Reconnect pill on an ended pane | §1, §3.6 | Needs F4's row first |
| S3 | `daemon` board 2: "Disconnected · Reconnect ↩ · Close ⌘W" for tmux control-mode windows | canvas | Non-goal in the plan (no supervisor); recorded |

---

## 5. Owner questions (proposed defaults)

1. **Footer table shape (F1).** *Default:* `FooterAction` columns in `session/EndReason.hpp`, a separate Interrupted
   row type, and `restartOffer` migrated in the same commit.
2. **Run Again without a prompt mark (F2).** *Default:* offer Run Again only once the shell has reported a prompt;
   otherwise Dismiss alone.
3. **The handoff seam (F3).** *Default:* `noteInterruption()` posted by the controller to the GUI thread.
4. **Disconnected panes during Reconnect (F4).** *Default:* they show the pill (a new `paneStateOf` row).
5. **The host chip's Reconnect (F15).** *Default:* not in 6.3; a follow-up once the footer's path is proven.
6. **Rebinding after a refusal (F5).** *Default:* rebind by remote session id before the layout applies; unnamed leaves
   keep their footer.
7. **The Disconnected message (F7).** *Default:* drop the reattach hint while Reconnect is offered.
8. **Display pass (F13).** *Default:* one filtered pass in Finish, or the CI artefact.
