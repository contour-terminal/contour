# Native Redesign — Step 3c (Notices, Dialogs and Permissions) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25) at HEAD `fdb54a64`, before Steps 1c, 1d and 2 land (4a has landed).
Line numbers are at that HEAD. Where 3c touches code those steps add or change, the task names it as their plans do
and carries a **Depends on Step <id>** note; locate such code by name. A pre-flight scan re-verifies every reference
before Task 1. Steps 3a–3e may run in parallel worktrees: see *Shared files*.

**Goal:** Every permission wall becomes one kit Dialog whose title names the action, with two buttons and one checkbox
answered as `(PermissionDecision, PermissionMemory)`; a session asks one question at a time; a daemon-hosted capture is
refused instead of left hanging; the window queues its notices (config reload errors as a Banner, "Copied N lines", a
refused paste and Save Layout's outcome as Toasts); extension and control consent use the same Dialog, with grants that
can type in bold; Save Layout becomes a sheet that says what it saves and offers "Replace?" for a taken name.

**Architecture:**
- **The API speaks enums.** `PermissionDecision { Deny = 0, Allow }` and `PermissionMemory { Once = 0, Session,
  Persistent }` are `Q_ENUM_NS` in `session/Permission.hpp`, registered once as `Permission` in `Contour.Terminal`.
  Every `(bool allow, bool remember)` of the guarded-role API takes them, and `PermissionCache` stores the decision.
- **Questions are data.** `session/PermissionPrompt.hpp` holds one row per `GuardedRole` (its setting, title, button
  labels, default button) and the builders that turn a role's pending facts into a `PermissionPrompt` (title, lines,
  checkbox scope). `session/PermissionQueue.hpp` holds the asks in order, one shown at a time; the session publishes
  the front as one `pendingPermission` property and takes one `answerPermission(decision, memory)`.
- **Notices are data.** `window/NoticeQueue.hpp` (Qt-free) is the table of notice kinds and the queue policy (one toast
  at a time, coalescing, hover hold, one banner); `window/NoticeModel` is its Qt adapter, one per `WindowController`,
  with an injected `IClock` and `Announcer`. Sessions and the manager post through the hosting window.
- **QML renders answers.** `PermissionDialog.qml` and `SaveLayoutDialog.qml` sit on `KitDialog`; Main.qml hosts one
  Toast and one more Banner from the window's `NoticeModel`. Pure decisions (`ToastPlacement.hpp`,
  `SaveLayoutCheck.hpp`, `PastePreview.hpp`) are Catch2 tables.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls, Qt Test), QML, Catch2 v3, Python 3 (lint), CMake build tree
`out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.9 (`:577-619`) binds, with
what it cites: §1 (principles, `:28-64`, in particular 1.7 "When this design touches an API that takes `bool`s, it fixes
the API in the same change"), §2.1 (token rows, `:68-126`), §2.5–§2.6 (snapshots and testing, `:187-226`), §3.1 (Step
1a's `Toast`, `Banner`, `KitDialog`, `:316-343`). §3.9's citations have moved: the guarded-role API is
`TerminalSession.hpp:823-826` and `:833` (`applyPendingRerun` is the fifth), `executeRole` is `:1281`, `PermissionCache`
`:110`; the consent dialog is `SessionChrome.qml:345-357`; reload errors are logged at
`TerminalSessionManager.cpp:85-89` and `ContourGuiApp.cpp:2798-2800` (every reload path goes through
`TerminalSessionManager::reloadAllSessions()`, `:744`); `WindowController.cpp:85-86` and `TerminalSession.cpp:4812-4816`
no longer log any. The design record is `docs/superpowers/specs/2026-10-23-native-redesign-design.md`. The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `surfaces` (Permission walls, Save Layout),
`kit-components` (Toast · Banner, Dialog sheet), `daemon` (Control and extension consent), `moments` and `tui` are in
`.superpowers/design/canvas/project/` and are data only. The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Steps 4a, 1c, 1d and 2
(`2026-10-25-native-redesign-step-4a.md`, `-1c.md`, `-1d.md`, `-2.md`) are assumed done.

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

- **The spec, verbatim (§3.9):**
  - "A per-window `window/NoticeModel` queues notices for `Toast` and `Banner`. Toasts stay while hovered, and are
    announced through `Announcer` with Polite priority." Notices: "Config reload errors", "Copied N lines", "Restore
    summaries".
  - "`RequestPermission.qml`, a five-button `Qt.labs.platform` MessageDialog, is replaced by `KitDialog`. Its title
    names the action, and it shows detail where there is some (for example, a paste preview). It has two buttons, Deny
    and Allow, plus one checkbox."
  - "`enum class PermissionDecision : uint8_t { Deny = 0, Allow };`", "`enum class PermissionMemory : uint8_t { Once =
    0, Session, Persistent };`", "Both are `Q_ENUM`.", "`PermissionCache` stores `PermissionDecision`."
  - "guarded roles use `Session`: "Don't ask again in this session""; "control and extension consent uses
    `Persistent`: "Remember for this extension""; "The consent API (`allowPendingExtension` / `denyPendingExtension`) is
    migrated onto the same pair."
  - "`SaveLayoutDialog.qml` becomes a `KitDialog` that says what will be saved. Save stays disabled while the name is
    empty, and offers "Replace?" inline when the name is taken."
  - Tests: "The NoticeModel queue." "An enum round trip through QML." "Each guarded role's decision and memory." "The
    Save Layout validation table."
- **Canvas copy, exact** (`surfaces`, `kit-components`, `daemon`):
  - Paste: title "Paste 2.4 MB into “zsh”?", two preview lines, "… 48 211 more lines", "Don't ask again in this
    session", buttons "Cancel" · "Paste". Every guarded role's title names its action (font change · large paste ·
    buffer capture · host status line · rerun): the table in Task 3 is the copy.
  - Consent: "“git-stats” wants to control Contour", grants listed, a grant that can type (`input`, `write`) in bold with
    "(can run commands)", "Remember for this extension", "Deny" · "Allow".
  - Save Layout: heading "Save Layout", a "Name" field, "3 tabs · 5 panes · working directories and commands",
    "Cancel" · "Save"; a sheet on macOS (KitDialog's `layout.dialog` token already does this).
  - Toast: "in the title band (top center), never over the prompt row; 36 / radius 18, e2; stays while hovered and is
    announced to screen readers". Banner: "config-reload problems surfaced in the window", action "Show in Editor".
  - Dialog: "Two buttons, destructive in status.danger".
- **Binding rulings** (ledgers 0, 1a, 1b, 1e; the 4a, 1c, 1d, 2, 3a and 3b SDD ledgers):
  - Toast, Banner and KitDialog are Step 1a's primitives; this step extends KitDialog, never forks it.
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`); a new file's baseline is 0 and a
    baseline only goes down (`SaveLayoutDialog.qml` is at 5, `scripts/qml-size-literal-baseline.txt`, and goes to 0;
    `RequestPermission.qml` is deleted). Text contrast at least 4.5:1 in every row, checked against contour-dark and
    contour-light. Status colours are tokens (`chromeStyle.statusColor(role)`/`onStatusColor(role)`); a destructive
    button is `acceptRole: chromeStyle.statusDanger`. This step adds no colour role and no size token.
  - Every visual surface is a snapshot row, and every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`). TUI rendering is 1a's: the Dialog is a boxed popup with the hard
    shadow, the Toast a one-row inverse strip, the Banner one row in palette[0]; ContourTui draws the checkbox `[x]` and
    the buttons bracketed.
  - Program-supplied text (session titles, font names, paste text, command lines, extension names, YAML errors) is
    shown as plain text: every Label that shows it sets `textFormat: Text.PlainText`, and the C++ side passes it through
    `vtbackend::sanitizeCommandLine(text, vtbackend::SanitizePurpose::Display)` (or `vtextension::neutralizedForDisplay`
    for an extension's) before it reaches QML.
  - `enum class` over `bool`, zero the default/negative case, explicit `: uint8_t`. QML-facing `Q_PROPERTY`,
    `QVariantMap` keys and `Q_INVOKABLE` parameters may stay `bool` as the documented carve-out (say so at the
    declaration). `std::expected` for every fallible call (`ConsentAnswerError`, `LayoutSaveError`).
  - Dependency injection: time is `core::platform::IClock` (`ManualClock` in tests), speech is `platform::Announcer`,
    files are `config::LayoutStore`, URLs `platform::ExternalLauncher`. Configuration at construction: `NoticeModel`
    takes its clock, announcer and action handler in its constructor and has no setters.
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`. Owner manual checks are batched per phase: listed in
    the ledger, never awaited.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`):
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds in one tree at
  once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows. A task touching
  `vthost` also runs `ctest --test-dir out/appleclang-debug -L daemon --output-on-failure`.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding (no `NOLINT`). Format every touched `.cpp`/`.hpp` with
  `/opt/homebrew/opt/llvm@22/bin/clang-format -i`; check every new or touched `.qml`, `.txt` and `CMakeLists.txt` with
  `editorconfig-checker <files>`. **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value,
  and every table row names every field (`GuardedRoleRows`, `NoticeRows`, `PermissionPhrases`, `SnapshotScripts`).
- **Header-only functions are `inline` or `constexpr`.** Tables carry a `static_assert` that they are in enumerator
  order. Counts in tests are derived from tables (`GuardedRoleRows.size()`, `NoticeRows.size()`,
  `SnapshotScripts.size()`), never literals.
- **Qt-free targets stay Qt-free:** `window/NoticeQueue.hpp`, `window/ToastPlacement.hpp`, `window/SaveLayoutCheck.hpp`
  and `session/PastePreview.hpp` include no Qt and are tested in `contour_test`. `session/Permission.hpp` includes
  `QtCore/QObject` for `Q_NAMESPACE`, so what consumes the two enums is tested in `contour_gui_test` (Spec gaps).
- **QML:** every new control sets `Accessible.role` and `Accessible.name`; a `Popup` carries neither, so its content
  item does. `slots`, `signals` and `emit` are Qt macros, never identifiers.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name (other agents share the tree; never `git add -A` or
  a directory). Each message ends with exactly `Signed-off-by: Christian Parpart <christian@parpart.family>` and never
  carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 11).

## Review Focus

1. **Two permission requests queued at once.** A program changes the font and asks for a capture in the same output
   chunk, or an extension asks while a capture prompt is up. Expected: one Dialog at a time, in the order asked; the
   second opens after the first closes; a role already waiting is not asked twice (a second capture is answered by the
   one verdict); neither answer is applied to the other. Pinned in Task 3 ("two questions asked at once are shown one
   after the other"), Task 7 ("the dialog reopens for the next question") and Task 8 ("an extension waits behind a
   guarded role").
2. **"Remember" across a config reload.** A user ticks "Don't ask again in this session" and allows capture, then edits
   `capture_buffer: deny`, reloads, and later sets it back to `ask`. Expected: the remembered answer outlives a reload
   that leaves the setting alone; a configured `allow`/`deny` always wins; a reload or profile switch that changes the
   role's setting forgets the remembered answer, so `ask` asks again. Pinned in Task 3 ("a remembered answer outlives
   a reload that leaves its setting alone, and not one that changes it").
3. **A paste dialog for a 50 MB clipboard.** Expected: nothing over the 1 MiB hard limit is asked about: no Dialog, a
   Toast says it was not pasted, the PTY gets nothing; a 600 KB paste asks with a preview bounded to two lines of at
   most 160 bytes plus "… N more lines", built once, never the whole text in a QML `Text`. Pinned in Task 3
   (`pastePreview.*`, "the paste prompt's preview is bounded however large the paste") and Task 6 ("a clipboard over
   the hard limit is refused with a toast, not a dialog").
4. **A remote/daemon session asking for a capture.** A daemon-hosted pane's program sends XTCAPTURE; today the daemon's
   `Terminal::Events` default drops it and the program reading the reply blocks forever. Expected: the daemon refuses
   it with the empty terminating chunk, as `capture_buffer: deny` does locally; the GUI shows nothing. Pinned in
   Task 4 ("a hosted session refuses a buffer capture rather than dropping it").
5. **Keyboard-only answering and VoiceOver.** Expected: focus lands on the row's default button (Deny for what a
   program asks, Allow for the user's own large paste); Return takes it; Tab reaches the other button and the checkbox;
   Space ticks it; Escape answers Deny once even with the box ticked; focus returns to the terminal after; the dialog
   is an accessible Dialog named by its title with its detail as description, the checkbox a CheckBox named by its
   label, and opening it announces the title politely. Pinned in Task 3 ("asking announces the title") and Task 7
   ("keyboard-only answering", "the permission dialog's accessible tree").

Also pinned, beyond the five: a program-chosen title or YAML error with `<img src=…>` renders as text (Tasks 6, 7); a
pane switched away and back still shows the question it owes (Task 7); Save on a name taken between the check and the
click does not overwrite (Task 9); an unreadable `layouts.yml` says so in a toast (Task 9); two quick copies show one
"Copied" toast, not a backlog (Task 5).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Where the enums live.** `Q_ENUM_NS` needs `Q_NAMESPACE` in the namespace the enum is declared in, and moc on
  that header. They go in their own namespace, `contour::session::permission` (one `staticMetaObject`, no clash with a
  later `Q_NAMESPACE` on `contour::session`), re-exported into `contour::session` by `using`. QML reaches them as
  `Permission.Allow`, `Permission.Session` after `import Contour.Terminal` (registered with
  `qmlRegisterUncreatableMetaObject`, as `ContourGuiApp.cpp:2388-2395` registers the module's other types). If the
  round-trip test shows Qt 6.11 cannot pass a `uint8_t`-based `Q_ENUM_NS` through a `Q_INVOKABLE`, record it in the
  ledger and stop for a ruling.
- **R-b. The five executors.** `applyPendingFontChange`, `applyPendingPaste`, `executePendingBufferCapture`,
  `executeShowHostWritableStatusLine`, `applyPendingRerun` and `executeRole` take `(PermissionDecision,
  PermissionMemory)`. `Once` remembers nothing; `Session` stores the decision in `PermissionCache`; `Persistent` is
  stored as `Session` for a guarded role (a role has nowhere persistent to keep it; its row never offers it),
  documented at `executeRole`. A configured `allow`/`deny`, or a remembered answer, executes as `(decision, Once)`.
- **R-c. One question at a time, per session.** `PermissionQueue` holds `std::variant<GuardedRole,
  ExtensionConsentAsk>` asks in arrival order and shows the front; an ask equal to one already waiting is not queued
  again (each role's pending state already coalesces: captures accumulate and one verdict answers all, a later font
  change or paste replaces the earlier). The front is published as `pendingPermission`; answering it executes the role,
  drops the front and, if another is waiting, notifies on the next event-loop turn, so the next Dialog opens after this
  one closes. Two panes asking at once still show two Dialogs, each over its own pane.
- **R-d. Which button is the default (owner).** Each role's row names `defaultDecision`: `Deny` for what a program on
  the PTY asks (font change, capture, host status line) and for a rerun (its command line is forgeable); `Allow` for a
  large paste (the user just asked to paste). Consent defaults to `Deny`.
- **R-e. Reload forgets what it changed (owner).** `activateProfile` (which every reload and profile switch runs,
  `TerminalSession.cpp:5893-5911`) compares each role's configured permission before and after `_profile` is replaced,
  and erases the remembered answer of every role whose setting changed. Roles with no setting (BigPaste, RerunCommand)
  keep theirs for the session.
- **R-f. Consent's checkbox (owner).** An extension's grant lives only in a grant store (`IGrantStore::record`), bound
  to the manifest digest, so "allow just this once" has nowhere to live. The consent prompt's box starts ticked;
  `(Allow, Persistent)` records Granted, `(Deny, Persistent)` records Denied, `(Deny, Once)` is today's "dismissed"
  (nothing recorded, asked again in a new pane), and `(Allow, Once)` is refused (`ConsentAnswerError::AllowMustBeRemembered`):
  the Dialog disables Allow while the box is unticked.
- **R-g. Escape is no answer.** Escape, or closing the Dialog without a button, answers `(Deny, Once)` whatever the
  checkbox says: a guarded role is refused and not remembered, consent is dismissed.
- **R-h. A daemon-hosted capture is refused (owner).** `vthost::HostedSession` answers `requestCaptureBuffer` with
  `replyCaptureBufferEnd()`, flushed by its pump, as `TerminalSession`'s display-less branch does
  (`TerminalSession.cpp:1318-1333`). Relaying the question to an attached client is a protocol change for a later step.
  The override touches `Terminal::Events::requestCaptureBuffer(LineCount, bool logical)`, so §1.7 turns its `bool` into
  `vtbackend::CaptureLineMode { Physical = 0, Logical }`, converted where XTCAPTURE's parameter is decoded
  (`Screen.cpp:4550-4565`).
- **R-i. Notice texts and lifetimes are a table** (`NoticeRows`, Task 5). A toast shows for its row's duration from the
  moment it becomes current; while hovered its deadline is suspended, and leaving restarts the full duration. A toast
  posted while one of the same kind is current replaces its text and restarts it; one posted while another kind shows
  waits, replacing a waiting one of its own kind; at most `NoticeWaitingLimit` (4) wait, the oldest dropped. The
  banner slot holds one notice; a later post replaces it; a successful reload clears it.
- **R-j. Where the toast goes (owner).** In the title band, centred, whenever the band shows (Step 1c's `band`). With no
  band (a single tab under `Multiple` on Fluent/ContourLinux, full screen before reveal, Minimal), it sits at the top
  centre of the content, or at the bottom centre when the active pane's cursor is in the top `ToastCoveredRows` (2)
  rows, so it is never over the prompt row (`window/ToastPlacement.hpp`, decided as the toast shows).
- **R-k. What "Copied N lines" counts.** An explicit copy only: `CopySelection` (text format, including its
  last-command fallback) and `copyCommandBlock`. Copy-on-select into the X11 selection posts nothing. N counts `\n`
  plus one for a last line without one; an empty copy posts nothing.
- **R-l. Restore summaries.** No restore event reaches the client today (the daemon restores; the client sees new
  windows). This step ships the `RestoreSummary` row and `restoreSummaryText(windows, tabs)` ("Restored 2 windows,
  7 tabs"); Step 6.2 posts it.
- **R-m. Button labels.** The Dialog's two buttons answer Deny and Allow; their labels are the row's (canvas: "Cancel" ·
  "Paste", "Deny" · "Change Font").
- **R-n. The paste limits stay** (ask over 512 KiB, refuse over 1 MiB, `TerminalSession.cpp:1806-1835`); sizes are shown
  in decimal units ("2.4 MB"), as macOS does.
- **R-o. Save Layout.** The typed name is trimmed (`QString::trimmed`, as `saveLayoutAs` does today) before it is
  checked. "Taken" means a key of the app's merged `Config::layouts` (inline and `layouts.yml`), compared exactly. Taken
  and not yet confirmed: Save is disabled and "“dev” already exists. Replace?" shows inline; clicking "Replace?" makes
  the accept button "Replace" in `statusDanger`; editing the name withdraws the confirmation. `saveLayoutAs` refuses a
  taken name, `replaceLayout` overwrites; both post `LayoutSaved` or `LayoutSaveFailed` (which names
  `describe(LayoutSaveError)`).

### Non-goals

- Relaying guarded-role questions from a daemon to its client; the daemon's own host-writable status-line answer
  (always Shown, `SessionHost.cpp:100-102`) is unchanged.
- The "Opened by contour new (agent)" Toast and the failed-attach Dialog (`daemon` board): no window origin or attach
  failure reaches the client yet (Spec gaps).
- Config-load warnings that are not failures (the canvas's "5 key bindings were skipped"): the loader does not return
  them.
- Changing the paste limits; a monospace token for previews.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/session/Permission.hpp`, `.cpp` (create) | `PermissionDecision`, `PermissionMemory` (`Q_ENUM_NS`), `GuardedRole`, `PermissionCache`, formatters, QML registration | 1 |
| `src/contour/session/TerminalSession.{hpp,cpp}` (modify) | The executors take the enums; the queue; `pendingPermission`; notices posted; consent onto the pair | 2, 3, 4, 6, 8 |
| `src/contour/session/PastePreview.hpp` (create) | Bounded paste preview and byte formatting, Qt-free | 3 |
| `src/contour/session/PermissionQueue.hpp` (create) | One question at a time | 3 |
| `src/contour/session/PermissionPrompt.{hpp,cpp}` (create) | `GuardedRoleRows`, `PermissionPrompt`, the builders, `consentVerdictFor` | 3, 8 |
| `src/vtbackend/core/CaptureLineMode.hpp` (create); `screen/Screen.{hpp,cpp}`, `Terminal.{hpp,cpp}`, `testing/MockTerm.hpp` (modify) | Capture's line mode as an enum | 4 |
| `src/vthost/SessionHost.{hpp,cpp}` (modify) | A hosted capture is refused | 4 |
| `src/contour/window/NoticeQueue.hpp` (create) | Notice kinds, rows, texts, the queue policy, Qt-free | 5 |
| `src/contour/window/NoticeModel.{hpp,cpp}` (create); `platform/Announcer.hpp` (modify) | The window's notices for QML | 5 |
| `src/contour/window/ToastPlacement.hpp` (create) | Where a toast goes | 6 |
| `src/contour/window/WindowController.{hpp,cpp}`, `session/TerminalSessionManager.{hpp,cpp}` (modify) | Own and feed the notices; Save Layout's checks | 6, 9 |
| `src/contour/qml/Main.qml`, `Toast.qml`, `Banner.qml` (modify) | Host the toast and the notice banner; plain text | 6 |
| `src/contour/qml/KitDialog.qml` (modify), `PermissionDialog.qml` (create), `SessionChrome.qml` (modify), `RequestPermission.qml` (delete) | The one permission Dialog | 3, 7, 8 |
| `src/vtcontrol/Catalog.hpp`, `src/vtextension/ExtensionConsent.{hpp,cpp}` (modify) | Grants that can type, as data; the consent question as lines | 8 |
| `src/contour/window/SaveLayoutCheck.hpp` (create), `qml/SaveLayoutDialog.qml` (rewrite) | Save Layout | 9 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp`, `QmlMocks.hpp`, `MockMainController.hpp`, `MainWindowControllerMock.hpp`, `RecordingAnnouncer.hpp` (create) | Test doubles and snapshot rows | 2–10 |

## Shared files

Files another Phase 3 step's spec section also names, or that every Phase 3 step edits. A parallel worktree that
touches one rebases before its task commits and re-runs that task's filter; the merge order is the board's.

- `src/contour/session/TerminalSession.{hpp,cpp}`: §3.8 (3b, the find-bar block and one action handler), §3.12 (3f's
  block menu "Rerun" reaches `RerunCommand`, whose executor this step retypes; 3f calls the action, not the executor).
  3c's hunks: `:94-110`, `:823-853`, `:1274-1281`, `:1324-1330`, `:2015`, `:2110-2127` (hpp); `:952-1067`,
  `:1214-1245`, `:1318-1460`, `:1788-1950`, `:4122-4145`, `:4364-4395`, `:5893-5911` (cpp).
- `src/contour/qml/SessionChrome.qml`: §3.8 (3b, the `SearchBar` instance), §3.11 (3e, the scrollbar and hyperlink
  tooltip), §3.12 (3f, the block tooltip). 3c replaces only the permission dialogs (`:288-357`) and their
  `Connections` handlers (`:467-478`).
- `src/vtbackend/screen/Screen.{hpp,cpp}`, `Terminal.{hpp,cpp}`: §3.7 (3a), §3.8 (3b), §3.11 (3e), §3.12 (3f). 3c's
  hunks are capture only: `Screen.hpp:599`, `Screen.cpp:2579-2592` and `:4550-4565`, `Terminal.hpp:516`, `:757`,
  `:3161`, `Terminal.cpp:4922-4925`.
- `src/contour/window/WindowController.{hpp,cpp}`: §3.10 (3d, the palette model), §3.12 (3f, the block menu presenter).
  3c adds the `notices` property and the Save Layout invokables (`hpp:255-262`, `.cpp:749-767`).
- `src/contour/qml/Main.qml`: §3.10 (3d, `CommandPalette`). 3c edits `bannerStack` (`:149-186`), adds the toast, and
  `SaveLayoutDialog`'s instance (`:242-249`).
- `src/contour/qml/Toast.qml`: §3.11 (3e, the size indicator becomes a Toast pill). 3c adds `textFormat` only.
- `src/contour/qml/KitDialog.qml`: consumed by 1c's drift confirmation and 1b's About; 3c's additions default to
  today's behaviour.
- Edited by every Phase 3 step: `src/contour/test/ChromeSnapshotPlan.hpp` and `ChromeSnapshotScene.hpp` (append
  scripts; whichever lands second names the other's new fields in its rows), `src/contour/test/QmlMocks.hpp`,
  `MockMainController.hpp`, `src/contour/CMakeLists.txt`, `scripts/qml-size-literal-baseline.txt`, `metainfo.xml`,
  `src/contour/test/QmlComponents_test.cpp`, `src/contour/session/TerminalSession_test.cpp`,
  `src/contour/test/MainWindowQml_test.cpp`, `src/contour/test/Accessibility_qml_test.cpp`.

---

### Task 1: The permission vocabulary

**Files:**
- Create: `src/contour/session/Permission.hpp`, `src/contour/session/Permission.cpp`; list them in the GUI block's
  `_header_files` and `_core_source_files` of `src/contour/CMakeLists.txt` (beside `session/TerminalSession.hpp`, `:278`,
  and `session/TerminalSession.cpp`, `:327`).
- Modify: `src/contour/session/TerminalSession.hpp` — `GuardedRole` and `PermissionCache` (`:94-110`) and
  `std::formatter<GuardedRole>` (`:2110-2127`) move to `Permission.hpp`; include it.
- Modify: `src/contour/ContourGuiApp.cpp:2388-2395` (call the registration), `src/contour/test/test_main.cpp` (call it
  once after the application object is built, `:116`).
- Test: create `src/contour/session/Permission_test.cpp`; add it to `contour_gui_test` (`src/contour/CMakeLists.txt`,
  the `add_executable(contour_gui_test` list, `:769-866`).

**Interfaces:**
- Produces (`session/Permission.hpp`):
  - `namespace contour::session::permission { Q_NAMESPACE … }` holding
    `enum class PermissionDecision : uint8_t { Deny = 0, Allow }; Q_ENUM_NS(PermissionDecision)` and
    `enum class PermissionMemory : uint8_t { Once = 0, Session, Persistent }; Q_ENUM_NS(PermissionMemory)`, each
    enumerator Doxygen-documented (Once: "this answer only"; Session: "until the session ends"; Persistent: "recorded
    where the asker's grants live").
  - In `contour::session`: `using permission::PermissionDecision; using permission::PermissionMemory;`,
    `enum class GuardedRole : uint8_t { ChangeFont = 0, CaptureBuffer, ShowHostWritableStatusLine, BigPaste, RerunCommand };`
    (today's order, now with `= 0`), `using PermissionCache = std::map<GuardedRole, PermissionDecision>;`,
    `void registerPermissionQmlTypes();` — registers `permission::staticMetaObject` as the uncreatable `Permission` in
    `Contour.Terminal` 1.0 ("Permission holds enums only."); idempotent (a function-local `static bool`).
  - `std::formatter` for `GuardedRole` (moved unchanged), `PermissionDecision` (`"deny"`, `"allow"`) and
    `PermissionMemory` (`"once"`, `"session"`, `"persistent"`), each with the
    `// NOLINT(readability-identifier-naming)` the AGENT.md exception names.

- [ ] **Step 1: Write the failing test** (`Permission_test.cpp`, `[contour][permission][qml]`).
  - "the permission enums round-trip through QML": a test-only `class PermissionEcho : public QObject` (Q_OBJECT, in
    the `.cpp` with `#include "Permission_test.moc"`, as `QmlComponents_test.cpp:4063` does) with
    `Q_INVOKABLE void take(contour::session::PermissionDecision decision, contour::session::PermissionMemory memory)`
    and `Q_PROPERTY(contour::session::PermissionMemory memory READ memory CONSTANT)` returning `Persistent`. Loaded QML:
    `import QtQuick; import Contour.Terminal; QtObject { property bool sawPersistent: echo.memory === Permission.Persistent; Component.onCompleted: echo.take(Permission.Allow, Permission.Session) }`
    with `echo` a context property. CHECK the echo received `PermissionDecision::Allow` and `PermissionMemory::Session`,
    `sawPersistent == true`, and no QML warning.
  - "each enumerator keeps its value in QML": for every `PermissionMemory` and `PermissionDecision` enumerator (iterate
    `QMetaEnum::fromType<…>()`), the QML expression `Permission.<key>` evaluates to `static_cast<int>(value)`.
  - "the log spells each value": `std::format("{}", PermissionMemory::Session) == "session"`, and so on for all five.
- [ ] **Step 2: Run it and see it fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, `contour/session/Permission.hpp` not found.
- [ ] **Step 3: Implement** the header, the registration, and the two call sites.
- [ ] **Step 4: Run it and see it pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][permission][qml]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/Permission.hpp src/contour/session/Permission.cpp src/contour/session/Permission_test.cpp src/contour/session/TerminalSession.hpp src/contour/ContourGuiApp.cpp src/contour/test/test_main.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/session/Permission.hpp src/contour/session/Permission.cpp src/contour/session/Permission_test.cpp src/contour/session/TerminalSession.hpp src/contour/ContourGuiApp.cpp src/contour/test/test_main.cpp src/contour/CMakeLists.txt
git commit -m "contour: permission decisions and their memory as enums QML can name" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The guarded-role API takes a decision and a memory

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp:823-833` (the five executors), `:1281` (`executeRole`), `:2015`
  (`_rememberedPermissions`); `src/contour/session/TerminalSession.cpp:952-964` (`executeRole`), `:997-1032`
  (`requestPermission`), `:1343-1346`, `:1389-1392`, `:1440-1443`, `:1862-1866`, `:1921-1925`.
- Modify: `src/contour/display/TerminalDisplay.cpp:2031` (comment names the new signature).
- Modify: `src/contour/test/QmlMocks.hpp:163-167` (the stubs take the enums).
- Modify: `src/contour/qml/SessionChrome.qml:288-337` (each handler passes the pair; add `import Contour.Terminal`).
- Test: `src/contour/session/TerminalSession_test.cpp` (every call site below, plus one new case),
  `src/contour/display/DisplayRendering_test.cpp:789`, `:815`, `:823`, `:830`, `:929`, `:2442`, `:2460`, `:2522`.

**Interfaces:**
- Consumes: Task 1 (`PermissionDecision`, `PermissionMemory`, `PermissionCache`).
- Produces (`TerminalSession`):
  - `Q_INVOKABLE void applyPendingFontChange(PermissionDecision decision, PermissionMemory memory);` and likewise
    `applyPendingPaste`, `executePendingBufferCapture`, `executeShowHostWritableStatusLine`, `applyPendingRerun`
    (still `Q_INVOKABLE` until Task 3).
  - `void executeRole(GuardedRole role, PermissionDecision decision, PermissionMemory memory);` (private), documented
    with R-b.
  - `[[nodiscard]] std::optional<PermissionDecision> rememberedDecision(GuardedRole role) const;` (public, for the
    gate's tests and Task 3).
- The SessionChrome mapping, one row per old button: Yes → `(Permission.Allow, Permission.Once)`; Yes to All →
  `(Permission.Allow, Permission.Session)`; No → `(Permission.Deny, Permission.Once)`; No to All →
  `(Permission.Deny, Permission.Session)`; rejected → `(Permission.Deny, Permission.Once)`. The extension dialog is
  untouched (Task 8).

- [ ] **Step 1: Write the failing test.** In `TerminalSession_test.cpp`, new
  "TerminalSession: each guarded role remembers its decision for the session and nothing else",
  `[contour][session][permission]`: a table of the five public executors as member-function pointers paired with
  their `GuardedRole`; for each row and each answer in `{(Allow, Once), (Deny, Once), (Allow, Session), (Deny, Session),
  (Allow, Persistent)}`, on a fresh `makeDisplaylessSession`, call the executor, then CHECK `rememberedDecision(role)`
  is `nullopt` for `Once` and the decision otherwise (`Persistent` stored as Session). Rewrite every existing call site
  to the enums with the same meaning (`/*allow=*/true, /*remember=*/false` → `PermissionDecision::Allow,
  PermissionMemory::Once`, `true, true` → `Allow, Session`, `false, true` → `Deny, Session`, `false, false` → `Deny,
  Once`): `TerminalSession_test.cpp:1255-1257`, `:1291`, `:1337`, `:1358`, `:1376`, `:2491`, `:2514`, `:2703`,
  `:2716`, `:2730`, `:2751`, `:3402`, `:3406`, `:3427-3428`, `:4454`, `:4474`, `:4514`, `:6587`, `:6595`, `:6602`,
  `:6613`, `:6630`, `:10140`, `:10358`, and the `DisplayRendering_test.cpp` lines above.
- [ ] **Step 2: Run it and see it fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, no `applyPendingFontChange(PermissionDecision, PermissionMemory)`.
- [ ] **Step 3: Implement.** `remember` becomes `memory != PermissionMemory::Once`; `requestPermission`'s configured
  and remembered branches call `executeRole(role, decision, PermissionMemory::Once)`; every `allow ? … : …` reads
  `decision == PermissionDecision::Allow`; the logs print the enums through their formatters.
- [ ] **Step 4: Prove no `(bool, bool)` is left, then run the tests.**

```bash
grep -rnE "(applyPendingFontChange|applyPendingPaste|executePendingBufferCapture|executeShowHostWritableStatusLine|applyPendingRerun|executeRole)\((true|false|/\*allow|bool)" src
grep -rnE "bool allow, bool remember|\(bool, bool\) \{\}|std::map<GuardedRole, bool>" src
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][permission],[contour][session][clipboard],[contour][session][actions],[contour][session]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][display],[contour][gui][qml]"
```

  Expected: both greps print nothing; the tests PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/display/TerminalDisplay.cpp src/contour/test/QmlMocks.hpp src/contour/session/TerminalSession_test.cpp src/contour/display/DisplayRendering_test.cpp
editorconfig-checker src/contour/qml/SessionChrome.qml
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/display/TerminalDisplay.cpp src/contour/test/QmlMocks.hpp src/contour/qml/SessionChrome.qml src/contour/session/TerminalSession_test.cpp src/contour/display/DisplayRendering_test.cpp
git commit -m "contour: guarded roles take a permission decision and memory, not two booleans" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: A session asks one question at a time, and says what it asks

**Files:**
- Create: `src/contour/session/PastePreview.hpp` (Qt-free, header-only); test `src/contour/session/PastePreview_test.cpp`
  in `contour_test` (`src/contour/CMakeLists.txt:1153-1205`, beside `session/SearchStatus_test.cpp`).
- Create: `src/contour/session/PermissionQueue.hpp` (header-only), `src/contour/session/PermissionPrompt.hpp` and
  `.cpp` (list in `_header_files`/`_core_source_files`); test `src/contour/session/PermissionPrompt_test.cpp` in
  `contour_gui_test`.
- Modify: `src/contour/session/TerminalSession.hpp` — signals `:1324-1328` removed; `pendingRerunCommandLine`
  (`:835-837`) removed; `configuredPermissionFor` (`:1274`) reads the table; the executors lose `Q_INVOKABLE`; the new
  members below. `TerminalSession.cpp:966-1032`, `:1318-1340`, `:1386`, `:1437`, `:1833`, `:1948-1951`, `:4197`,
  `:5905-5911`.
- Modify: `src/contour/qml/SessionChrome.qml` — the five guarded `RequestPermission` instances (`:288-337`) become one
  `RequestPermission { id: permissionDialog; objectName: "permissionDialog" }` fed from `session.pendingPermission`
  (`title` = the prompt's title; `text` = its lines joined by `\n`), opened on `onPendingPermissionChanged` when the
  map has a title, answering through `answerPermission` with Task 2's mapping; the five `Connections` handlers
  (`:467-478`, all but the extension one) go.
- Modify: `src/contour/test/QmlMocks.hpp` — `MockSession` gains `pendingPermission`, `pendingPermissionChanged`,
  `answerPermission` (recording the last pair and a count) and loses the five stubs and their signals.
- Test: `src/contour/session/TerminalSession_test.cpp` (the signal spies at `:1281`, `:1369`, `:4404`, `:6578` move to
  `pendingPermissionChanged` + `pendingPermissionAsk()`; new cases below), `src/contour/display/DisplayRendering_test.cpp:782`,
  `:803`, `src/contour/test/QmlComponents_test.cpp` (one new SessionChrome case).

**Interfaces:**
- Produces (`session/PastePreview.hpp`, namespace `contour::session`):
  - `struct PastePreviewLimits { std::size_t lines = 2; std::size_t bytesPerLine = 160; };`
  - `struct PastePreview { std::vector<std::string> lines {}; std::size_t moreLines = 0; };`
  - `[[nodiscard]] inline PastePreview makePastePreview(std::string_view text, PastePreviewLimits limits = {});` — one
    pass over `\n`; a trailing `\n` opens no line; each shown line cut to at most `bytesPerLine` bytes at a UTF-8 lead
    byte, `…` appended when cut, then `vtbackend::sanitizeCommandLine(line, SanitizePurpose::Display)`; copies only the
    shown prefixes.
  - `[[nodiscard]] inline std::string formatByteCount(std::size_t bytes);` — `"1 byte"`, `"{} bytes"` below 1000,
    `"{:.1f} kB"` below 1 000 000, `"{:.1f} MB"` otherwise.
- Produces (`session/PermissionQueue.hpp`):
  - `struct ExtensionConsentAsk { constexpr bool operator==(ExtensionConsentAsk const&) const noexcept = default; };`
  - `using PermissionAsk = std::variant<GuardedRole, ExtensionConsentAsk>;`
  - `enum class PromptChange : uint8_t { None = 0, Due };`
  - `class PermissionQueue` with `[[nodiscard]] PromptChange offer(PermissionAsk ask);` (Due when it became the front;
    None when queued behind, or already waiting), `[[nodiscard]] std::optional<PermissionAsk> current() const noexcept;`,
    `[[nodiscard]] PromptChange finish();` (drops the front; Due when another is now the front), `[[nodiscard]]
    std::size_t size() const noexcept;`.
- Produces (`session/PermissionPrompt.hpp`):
  - `enum class PromptLineWeight : uint8_t { Regular = 0, Strong };`
  - `enum class PromptLineStyle : uint8_t { Prose = 0, Verbatim };` (Verbatim: one row, elided, never wrapped)
  - `struct PromptLine { std::string text {}; PromptLineWeight weight = PromptLineWeight::Regular; PromptLineStyle style = PromptLineStyle::Prose; };`
  - `enum class AllowOnce : uint8_t { Offered = 0, Withheld };`
  - `struct PermissionPrompt { std::string title {}; std::vector<PromptLine> lines {}; std::string denyText {}; std::string allowText {}; std::string rememberText {}; PermissionMemory rememberScope = PermissionMemory::Session; PermissionMemory initialMemory = PermissionMemory::Once; PermissionDecision defaultDecision = PermissionDecision::Deny; AllowOnce allowOnce = AllowOnce::Offered; };`
  - `struct GuardedRoleRow { GuardedRole role; config::Permission config::PermissionsConfig::* setting; std::string_view denyText; std::string_view allowText; PermissionDecision defaultDecision; };`
    and `inline constexpr std::array GuardedRoleRows` in enumerator order (`static_assert`); `setting` is `nullptr`
    for BigPaste and RerunCommand.
  - `[[nodiscard]] config::Permission configuredPermission(config::PermissionsConfig const& permissions, GuardedRole role) noexcept;`
    (`Ask` where `setting` is null) — replaces `configuredPermissionFor`'s switch.
  - `inline constexpr std::string_view SessionRememberText = "Don't ask again in this session";`
  - Builders (subject = the session's title, sanitized for display and cut to 64 bytes with `…`; an empty title reads
    "this terminal", unquoted):
    `[[nodiscard]] PermissionPrompt fontChangePrompt(std::string_view subject, vtbackend::FontDef const& font);`,
    `[[nodiscard]] PermissionPrompt bufferCapturePrompt(std::string_view subject, vtbackend::LineCount lines);`,
    `[[nodiscard]] PermissionPrompt hostStatusLinePrompt(std::string_view subject);`,
    `[[nodiscard]] PermissionPrompt bigPastePrompt(std::string_view subject, std::size_t bytes, PastePreview const& preview);`,
    `[[nodiscard]] PermissionPrompt rerunPrompt(std::string_view displayedCommandLine);`.

  | Role | Title | Lines | Deny · Allow | Default |
  |---|---|---|---|---|
  | ChangeFont | `Allow “{s}” to change the font?` | `{regular}, {size} pt` (the parts the request sets; none: `It asks for another font.`) | Deny · Change Font | Deny |
  | CaptureBuffer | `Allow “{s}” to read the screen?` | `It asks for the last {n} lines of the terminal, to read them back.` | Deny · Allow | Deny |
  | ShowHostWritableStatusLine | `Allow “{s}” to show its own status line?` | `It writes the text of the line below the terminal.` | Deny · Show | Deny |
  | BigPaste | `Paste {formatByteCount} into “{s}”?` | each preview line (Verbatim), then `… {moreLines} more lines` when non-zero | Cancel · Paste | Allow |
  | RerunCommand | `Run this command again?` | the command line (Verbatim), then `It is typed at the prompt and entered.` | Cancel · Run | Deny |

  Every guarded prompt: `rememberText = SessionRememberText`, `rememberScope = Session`, `initialMemory = Once`.
- Produces (`TerminalSession`):
  - `Q_PROPERTY(QVariantMap pendingPermission READ pendingPermission NOTIFY pendingPermissionChanged)` — empty while
    nothing is asked; else `title` (QString), `lines` (QVariantList of `{text, strong, verbatim}` maps; the two `bool`s
    are the QML carve-out), `denyText`, `allowText`, `rememberText`, `rememberScope`, `initialMemory`,
    `defaultDecision` (each `QVariant::fromValue` of its enum), `allowNeedsRemember` (bool carve-out, from `AllowOnce`).
  - `void pendingPermissionChanged();` replaces `requestPermissionFor{FontChange,PasteLargeFile,BufferCapture,ShowHostWritableStatusLine,RerunCommand}`.
  - `Q_INVOKABLE void answerPermission(PermissionDecision decision, PermissionMemory memory);` — executes the front
    role (`executeRole`), `finish()`es it, and on Due notifies on the next event-loop turn
    (`QMetaObject::invokeMethod(…, Qt::QueuedConnection)`, as `answerExtensionConsent` does, `:1241-1244`).
  - `[[nodiscard]] std::optional<PermissionAsk> pendingPermissionAsk() const;` (tests, Task 8).
  - The Ask branch of `requestPermission` offers the role; on Due it notifies and announces the prompt's title
    politely through the session's announcer. The prompt is built when read, from the role's pending state (the
    font def, the largest pending capture, the paste's size and a preview made once when the paste is held and kept in
    `BigPasteRequest`, the rerun's `display()`).
  - `activateProfile` erases the remembered answer of each role whose `configuredPermission` differs before and after
    `_profile` is replaced (R-e).

- [ ] **Step 1: Write the failing tests.**
  - `PastePreview_test.cpp`, `[pastepreview]`:
    - `pastePreview.boundsLinesAndCountsTheRest`: 48 213 lines `"line {i}\n"` → `lines == {"line 0", "line 1"}`,
      `moreLines == 48211`.
    - `pastePreview.cutsALongLineAtACharacter`: one 600 000-byte line of `"é"` → one line, at most `160 + 3` bytes,
      valid UTF-8 (round-trips through `std::u8string_view` decoding), ends with `…`, `moreLines == 0`.
    - `pastePreview.showsControlsAsReplacement`: `"\x1b[31mred\n"` → the shown line holds no `\x1b`.
    - `pastePreview.emptyAndTrailingNewline`: `""` → no lines; `"a\n"` → `{"a"}`, `moreLines == 0`.
    - `formatByteCount`: `1 → "1 byte"`, `999 → "999 bytes"`, `1000 → "1.0 kB"`, `524289 → "524.3 kB"`,
      `2400000 → "2.4 MB"`, `52428800 → "52.4 MB"`.
  - `PermissionPrompt_test.cpp`, `[contour][permission][prompt]`:
    - the rows are in enumerator order and cover every `GuardedRole` (`GuardedRoleRows.size()`);
    - `configuredPermission` reads each role's own setting (set one field to `Allow` at a time) and `Ask` for BigPaste
      and RerunCommand;
    - each builder's title, buttons and default match the table, for subject `"zsh"`; `bigPastePrompt("zsh", 2400000,
      preview)` has title `"Paste 2.4 MB into “zsh”?"` and its last line `"… 48211 more lines"`;
    - a subject `"<img src=x>\x1b]0;evil\x07"` keeps `<img src=x>` as text and holds no ESC or BEL; an empty subject
      reads `Allow this terminal to read the screen?`.
    - `PermissionQueue`: offer A → Due; offer B → None; offer A again → None and `size() == 2`; `current() == A`;
      `finish()` → Due with `current() == B`; `finish()` → None, `current()` empty.
  - `TerminalSession_test.cpp`, `[contour][session][permission][queue]` (Review Focus 1):
    "two questions asked at once are shown one after the other": a session with a recording surface
    (`makeSessionWithSurface`, profile `change_font: ask`, `capture_buffer: ask`); `setFontDef(...)` then
    `captureWholePage(*session)`, processing posted events; `pendingPermissionAsk() == GuardedRole::ChangeFont` and one
    `pendingPermissionChanged`; `answerPermission(Deny, Once)`, process events: the front is CaptureBuffer and the font
    did not change; `answerPermission(Allow, Once)`: `capturedSeededScreen(*session)`, and nothing is pending. A second
    `captureWholePage` while the first is asked adds no ask, and the one Allow writes two replies
    (`countOccurrences(…, CaptureReplyTerminator) == 2`).
  - `[contour][session][permission]` (Review Focus 2): "a remembered answer outlives a reload that leaves its setting
    alone, and not one that changes it": profile `capture_buffer: ask`; ask, `answerPermission(Allow, Session)`;
    `applyReloadedConfig` with the same file (`contour::test::writeConfigFile` + `config::tryLoadConfigFromFile`):
    a capture is answered without asking; reload with `capture_buffer: deny`: refused without asking
    (terminator only); reload with `ask`: `pendingPermissionAsk() == CaptureBuffer`.
  - `[contour][session][permission]` (Review Focus 5): "asking announces the title": with the file's
    `RecordingAnnouncer` (`:5457`) set, the font ask announces `"Allow “…” to change the font?"` once, Polite.
  - `[contour][session][clipboard][permission]` (Review Focus 3): "the paste prompt's preview is bounded however large
    the paste": a 600 KB clipboard of 30 000 lines; `pendingPermission()["lines"]` has 3 entries, the first two
    `verbatim` and each at most 163 bytes of UTF-8.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][permission]`: "SessionChrome opens one dialog for the session's
    question": a `MockSession` whose `pendingPermission` gets a title; `permissionDialog` opens with that title;
    invoking its `yesToAllClicked` makes the mock record `(Allow, Session)`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors, `PastePreview.hpp` and `PermissionPrompt.hpp` not found.
- [ ] **Step 3: Implement** the three headers, the builders, the queue in the session, the property and the QML.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[pastepreview]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][permission],[contour][session][permission],[contour][session][clipboard],[contour][session]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][display],[contour][gui][qml]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** (the task's `.cpp`/`.hpp` files by name; `SessionChrome.qml` and
  `CMakeLists.txt` through `editorconfig-checker`):

```bash
git add src/contour/session/PastePreview.hpp src/contour/session/PastePreview_test.cpp src/contour/session/PermissionQueue.hpp src/contour/session/PermissionPrompt.hpp src/contour/session/PermissionPrompt.cpp src/contour/session/PermissionPrompt_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/display/DisplayRendering_test.cpp src/contour/qml/SessionChrome.qml src/contour/test/QmlMocks.hpp src/contour/test/QmlComponents_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: a session asks one permission question at a time and names the action" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: A daemon-hosted capture is refused, and capture's line mode is an enum

**Files:**
- Create: `src/vtbackend/core/CaptureLineMode.hpp`; list it in `src/vtbackend/CMakeLists.txt` beside the other `core/`
  headers.
- Modify: `src/vtbackend/screen/Screen.hpp:599`, `Screen.cpp:2579-2592` and `:4556-4562` (decode the parameter into
  the enum: 0 → Physical, 1 → Logical, anything else stays `ApplyResult::Invalid`), `src/vtbackend/screen/Terminal.hpp:516`,
  `:757`, `:3161`, `Terminal.cpp:4922-4925`, `src/vtbackend/testing/MockTerm.hpp:460-463`,
  `src/vtbackend/screen/Screen_osc_test.cpp:86-124`.
- Modify: `src/contour/session/TerminalSession.hpp:895`, `:1979-1983`; `TerminalSession.cpp:1318`, `:1337`, `:1368`;
  the test call sites `TerminalSession_test.cpp:4373`, `DisplayRendering_test.cpp:812`, `:820`, `:824`.
- Modify: `src/vthost/SessionHost.hpp:434-491` (the `Events` struct), `src/vthost/SessionHost.cpp:98-102` (constructor).
- Test: `src/vthost/SessionHost_test.cpp`.

**Interfaces:**
- Produces: `namespace vtbackend { enum class CaptureLineMode : uint8_t { Physical = 0, Logical }; }` with Doxygen
  (Physical: "rows as drawn"; Logical: "wrapped rows joined"), and a `std::formatter` (`"physical"`, `"logical"`).
- `Terminal::Events::requestCaptureBuffer(LineCount lines, CaptureLineMode mode)`, `Screen::captureBuffer(LineCount
  lineCount, CaptureLineMode mode)`, `Terminal::requestCaptureBuffer(LineCount, CaptureLineMode)`;
  `TerminalSession::CaptureBufferRequest::mode` replaces `logical`.
- `vthost::HostedSession::Events` gains `std::function<void()> onCaptureBuffer;` and the override
  `void requestCaptureBuffer(vtbackend::LineCount, vtbackend::CaptureLineMode) override` that calls it; the
  constructor sets it to `_terminal.primaryScreen().replyCaptureBufferEnd()` (no lock: the parser thread holds it, as
  the hook's contract says; the pump's `flushInput()` after each batch, `SessionHost.cpp:183-186`, writes it).

- [ ] **Step 1: Write the failing test** (`SessionHost_test.cpp`, `[vthost][host][capture]`, Review Focus 4):
  "a hosted session refuses a buffer capture rather than dropping it": a `HostedSession` over a scripted PTY double
  (the file's `ExitingPty`, extended to record what is written to it if it does not already) whose output is
  `"\033[>0,t"`; once it has closed, the bytes written to the PTY contain `"\033^314;\033\\"`. Rewrite the
  `Screen_osc_test.cpp` and `contour` call sites to `CaptureLineMode::Physical`/`Logical`.
- [ ] **Step 2: Run it and see it fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vthost_test`

  Expected: compile error, `vtbackend/core/CaptureLineMode.hpp` not found.
- [ ] **Step 3: Implement** the enum through every site and the hosted refusal.
- [ ] **Step 4: Run them and see them pass.**

```bash
cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test vthost_test contour_gui_test
out/appleclang-debug/src/vtbackend/vtbackend_test "[screen]"
out/appleclang-debug/src/vthost/vthost_test "[vthost][host]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][permission],[contour][display]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
grep -rn "bool logical" src/vtbackend src/contour src/vthost
```

  Expected: PASS; the grep prints nothing.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "vthost: refuse a daemon-hosted buffer capture instead of leaving it unanswered" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 5: The NoticeModel, as data

**Files:**
- Create: `src/contour/window/NoticeQueue.hpp` (Qt-free, header-only); test `src/contour/window/NoticeQueue_test.cpp`
  in `contour_test` (beside `window/TabBadge_test.cpp`).
- Create: `src/contour/window/NoticeModel.hpp`, `.cpp` (list in `_header_files`/`_core_source_files`); test
  `src/contour/window/NoticeModel_test.cpp` in `contour_gui_test`.
- Create: `src/contour/test/RecordingAnnouncer.hpp` — the `RecordingAnnouncer` moved out of
  `TerminalSession_test.cpp:5457`, which includes it instead.
- Modify: `src/contour/platform/Announcer.hpp:52-72` — `QtAnnouncer` gains an anchor-provider constructor.

**Interfaces:**
- Produces (`window/NoticeQueue.hpp`, namespace `contour::window`):
  - `enum class NoticeKind : uint8_t { Copied = 0, PasteTooLarge, LayoutSaved, LayoutSaveFailed, RestoreSummary, ConfigReloadFailed };`
  - `enum class NoticeSurface : uint8_t { Toast = 0, Banner };`, `enum class NoticeAction : uint8_t { None = 0, OpenConfigFile };`
  - `struct NoticeRow { NoticeKind kind; NoticeSurface surface; std::string_view glyph; std::chrono::milliseconds duration; NoticeAction action; std::string_view actionText; };`

    | Kind | Surface | Glyph | Duration | Action |
    |---|---|---|---|---|
    | Copied | Toast | — | 2000 ms | None |
    | PasteTooLarge | Toast | `!` | 4000 ms | None |
    | LayoutSaved | Toast | — | 2000 ms | None |
    | LayoutSaveFailed | Toast | `!` | 6000 ms | None |
    | RestoreSummary | Toast | — | 4000 ms | None |
    | ConfigReloadFailed | Banner | `!` | 0 (until cleared) | OpenConfigFile, "Show in Editor" |

    `inline constexpr std::array NoticeRows` (enumerator order, `static_assert`), `[[nodiscard]] constexpr NoticeRow const& noticeRow(NoticeKind kind) noexcept;`.
  - `struct Notice { NoticeKind kind = NoticeKind::Copied; std::string text {}; bool operator==(Notice const&) const = default; };`
  - Texts: `[[nodiscard]] inline std::string copiedNoticeText(std::size_t lines);` ("Copied 1 line", "Copied 3 lines");
    `pasteTooLargeNoticeText(std::size_t limitBytes)` ("Not pasted: the clipboard holds more than 1.0 MB of text.",
    via `session::formatByteCount`); `layoutSavedNoticeText(std::string_view name)` ("Saved layout “dev”");
    `layoutSaveFailedNoticeText(std::string_view name, config::LayoutSaveError error)` ("Layout “dev” was not saved:
    {describe(error)}."); `restoreSummaryText(std::size_t windows, std::size_t tabs)` ("Restored 2 windows, 7 tabs",
    singular forms for 1); `configReloadFailedNoticeText(std::string_view error)` ("Contour could not reload its
    configuration: {error}"). Every name and error is sanitized for display.
  - `enum class ToastHover : uint8_t { Away = 0, Hovered };`, `enum class NoticeChange : uint8_t { None = 0, Changed };`,
    `inline constexpr std::size_t NoticeWaitingLimit = 4;`
  - `class NoticeQueue` (`using TimePoint = std::chrono::steady_clock::time_point;`):
    `[[nodiscard]] NoticeChange post(Notice notice, TimePoint now);`, `[[nodiscard]] NoticeChange clear(NoticeKind kind);`,
    `[[nodiscard]] NoticeChange hover(ToastHover hover, TimePoint now);`, `[[nodiscard]] NoticeChange expire(TimePoint now);`,
    `[[nodiscard]] NoticeChange dismissBanner();`, `[[nodiscard]] Notice const* toast() const noexcept;`,
    `[[nodiscard]] Notice const* banner() const noexcept;`, `[[nodiscard]] std::optional<TimePoint> nextDeadline() const noexcept;`
    — the policy is R-i.
- Produces (`window/NoticeModel.hpp`):
  - `class NoticeModel final: public QObject` with `Q_PROPERTY(QString toastText READ toastText NOTIFY toastChanged)`,
    `toastGlyph`, `Q_PROPERTY(bool toastShown …)` (QML carve-out), `bannerText`, `bannerGlyph`, `bannerActionText`
    (`NOTIFY bannerChanged`);
    `NoticeModel(core::platform::IClock const& clock, platform::Announcer& announcer, std::function<void(NoticeAction)> onAction, QObject* parent = nullptr);`
    `void post(Notice notice);`, `void clear(NoticeKind kind);`, `Q_INVOKABLE void setToastHovered(bool hovered);`
    (carve-out), `Q_INVOKABLE void dismissBanner();`, `Q_INVOKABLE void triggerBannerAction();` (calls `onAction` with
    the banner row's action), `void expireDue();` (what its single-shot `QTimer`, armed at `nextDeadline()`, runs;
    tests call it after advancing a `ManualClock`). A toast that becomes current, and a banner that is posted, is
    announced once, Polite.
- Produces (`platform/Announcer.hpp`): `explicit QtAnnouncer(std::function<QObject*()> anchor);` — the anchor is
  resolved per announcement; the `QObject*` constructor delegates to it.

- [ ] **Step 1: Write the failing tests.**
  - `NoticeQueue_test.cpp`, `[notices]`: the rows are in enumerator order and every Banner row has an action text;
    the texts above; with `t0`:
    - post Copied at `t0` → `toast()->text == "Copied 3 lines"`, `nextDeadline() == t0 + 2000ms`; `expire(t0 + 1999ms)`
      → None; `expire(t0 + 2000ms)` → Changed, no toast;
    - post Copied, then PasteTooLarge → the toast stays Copied; at its deadline PasteTooLarge becomes current with
      deadline `+ 4000ms`;
    - post Copied twice 500 ms apart → one toast, the second text, deadline from the second post (coalescing);
    - six different-kind toasts behind a current one → at most `NoticeWaitingLimit` wait, the oldest dropped;
    - `hover(Hovered)` → `nextDeadline()` empty and `expire` far past does nothing; `hover(Away, t1)` → deadline
      `t1 + duration`;
    - post ConfigReloadFailed → `banner()` set and no toast; post it again → replaced; `clear(ConfigReloadFailed)` and
      `dismissBanner()` each remove it.
  - `NoticeModel_test.cpp`, `[contour][notices]`, with `core::platform::ManualClock` and `RecordingAnnouncer`:
    posting Copied makes `toastShown` true, emits `toastChanged` once and announces `"Copied 3 lines"` Polite once;
    `setToastHovered(true)`, advance the clock 10 s, `expireDue()` → still shown; `setToastHovered(false)`, advance
    2 s, `expireDue()` → hidden; a banner's `triggerBannerAction()` calls the handler with `OpenConfigFile`;
    `dismissBanner()` empties `bannerText`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `window/NoticeQueue.hpp` not found.
- [ ] **Step 3: Implement** the queue, the model and the announcer constructor.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[notices]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][notices],[contour][session]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: notices are a table, queued per window for a toast and a banner" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 6: The window shows its notices

> **Depends on Step 1c:** the title band (`titleBar`, the `band` value Main.qml binds, Task 7 of the 1c plan) and
> `bannerStack`'s place under it. Locate them by `objectName`.

**Files:**
- Create: `src/contour/window/ToastPlacement.hpp` (Qt-free); test `src/contour/window/ToastPlacement_test.cpp` in
  `contour_test`.
- Modify: `src/contour/window/WindowController.hpp/.cpp` — own the notice announcer and the model (constructor,
  `:49-85`); the property and invokables below.
- Modify: `src/contour/session/TerminalSessionManager.hpp/.cpp` — `reloadAllSessions` (`:744-…`) posts and clears;
  the timer's log (`:85-89`) stays.
- Modify: `src/contour/session/TerminalSession.hpp/.cpp` — `operator()(actions::CopySelection)` (`:4364-4395`) and
  `copyCommandBlock` (`:4122-4145`) post Copied; `pasteFromClipboard`'s hard limit (`:1806-1817`) posts PasteTooLarge
  instead of `showNotification`; `cursorLine()`.
- Modify: `src/contour/qml/Main.qml` — `bannerStack` (`:149-186`) gains `noticeBanner`, its height sums it; a
  `noticeToast` placed per `win.toastHost(…)`.
- Modify: `src/contour/qml/Toast.qml` (`toastText`), `src/contour/qml/Banner.qml` (`bannerText`): `textFormat:
  Text.PlainText`.
- Modify: `src/contour/test/MockMainController.hpp`, `src/contour/test/MainWindowControllerMock.hpp` — a `notices`
  property backed by a real `NoticeModel` (`ManualClock`, `NullAnnouncer`), and `toastHost`.
- Test: `src/contour/test/MainWindowQml_test.cpp`, `src/contour/session/TerminalSessionManager_test.cpp`,
  `src/contour/session/TerminalSession_test.cpp`, `src/contour/test/QmlComponents_test.cpp`.

**Interfaces:**
- Consumes: Task 5 (`Notice`, `NoticeKind`, the text builders, `NoticeModel`).
- Produces (`window/ToastPlacement.hpp`): `enum class BandPresence : uint8_t { Hidden = 0, Shown };`,
  `enum class ToastHost : uint8_t { TitleBand = 0, ContentTop, ContentBottom };`,
  `struct ToastPlacementInputs { BandPresence band = BandPresence::Hidden; int cursorRow = 0; int coveredRows = 0; };`,
  `inline constexpr int ToastCoveredRows = 2;`, `[[nodiscard]] constexpr ToastHost placeToast(ToastPlacementInputs const& in) noexcept;` (R-j).
- Produces (`WindowController`): `Q_PROPERTY(contour::window::NoticeModel* notices READ notices CONSTANT)`,
  `[[nodiscard]] NoticeModel& notices() noexcept;`, `[[nodiscard]] Q_INVOKABLE int toastHost(bool bandShown) const;`
  (carve-out; the active session's `cursorLine()`, `ToastCoveredRows`); the model's action handler runs
  `_manager.openConfigFile()`. The announcer is a `platform::QtAnnouncer` anchored at `[this] { return _osWindow; }`,
  declared before the model; the clock is `core::platform::defaultSteadyClock()` (the composition default, documented).
- Produces (`TerminalSessionManager`): `void postNotice(window::Notice notice, TerminalSession const* acting);` (to the
  hosting window; logs when none), `void postNoticeToEveryWindow(window::Notice const& notice);`,
  `void clearNoticeInEveryWindow(window::NoticeKind kind);`, `void openConfigFile();` (`_app.externalLauncher().openUrl(
  QUrl::fromLocalFile(configFile))`, logging a `LaunchError`). `reloadAllSessions` posts `ConfigReloadFailed` with
  `configReloadFailedNoticeText(error)` to every window when the load fails, and clears it in every window when it
  succeeds.
- Produces (`TerminalSession`): `[[nodiscard]] int cursorLine() const;` (the cursor page's cursor line, under the
  terminal lock).
- QML: `noticeToast` (`objectName`), `text`/`glyph`/`shown` from `win.notices`, a `HoverHandler` calling
  `setToastHovered`, centred horizontally; vertically centred on `titleBar` for `TitleBand`, anchored to `content`'s top
  or bottom with `chromeStyle.space.s4` margins otherwise. `noticeBanner` (`objectName`) shows `bannerText`, its
  action `triggerBannerAction()`, its dismiss `dismissBanner()`.

- [ ] **Step 1: Write the failing tests.**
  - `ToastPlacement_test.cpp`, `[toastplacement]`: band Shown → TitleBand for any cursor row; Hidden with
    `cursorRow` 0 or 1 → ContentBottom; Hidden with `cursorRow` 2 → ContentTop.
  - `TerminalSessionManager_test.cpp`, `[contour][session][manager][notices]`: with two windows, a config file
    rewritten to `"profiles: [\n"` and `reloadAllSessions()` failing → both windows' `notices().bannerText` start
    with `"Contour could not reload its configuration:"`; a valid file and a second reload clear both;
    `triggerBannerAction()` makes `testApp.launcher().openedUrls` hold the config file's URL.
  - `TerminalSession_test.cpp`, `[contour][session][clipboard][notices]`: a selection of two lines and `CopySelection {}`
    → the hosting controller's `notices().toastText() == "Copied 2 lines"`; an empty selection without a fallback posts
    nothing. (Review Focus 3) "a clipboard over the hard limit is refused with a toast, not a dialog": a 50 MB
    clipboard → `pendingPermissionAsk()` empty, the PTY untouched, `toastText()` is
    `pasteTooLargeNoticeText(1024 * 1024)`.
  - `MainWindowQml_test.cpp`, `[contour][gui][qml][mainwindow][notices]`: on the MacOS row a posted Copied shows
    `noticeToast` with its centre inside `titleBar`'s bounds and `height == chromeStyle.size.toast`; on the Fluent row
    with one tab (no band) it sits inside `content`; a posted banner makes `noticeBanner` visible and moves `content.y`
    by its height; a banner text `"<b>bold</b> <img src=x>"` is shown with the angle brackets
    (`bannerText.textFormat == Text.PlainText`). The same file under `CONTOUR_TEST_CONTROLS_STYLE=ContourTui` finds
    the toast in the one-row bar.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `window/ToastPlacement.hpp` not found.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[toastplacement],[notices]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][manager],[contour][session][clipboard],[contour][notices],[contour][gui][qml][mainwindow],[contour][gui][qml][kit]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow][notices],[contour][gui][qml][kit]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name (QML through `editorconfig-checker`):
  `git commit -m "contour: show config reload errors as a banner, and copies and refused pastes as toasts" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 7: The permission Dialog

> **Depends on Step 2:** `SessionChrome` sits in Step 2's `PaneCard`; the Dialog is parented to the chrome item as
> today. Locate `permissionDialog` by `objectName`.

**Files:**
- Modify: `src/contour/qml/KitDialog.qml` — a body slot, `acceptEnabled`, a default button, plain text, a description.
- Create: `src/contour/qml/PermissionDialog.qml`; add it to `_qml_files` (`src/contour/CMakeLists.txt:385-428`).
- Modify: `src/contour/qml/SessionChrome.qml` — Task 3's `permissionDialog` becomes a `PermissionDialog`.
- Test: `src/contour/test/QmlComponents_test.cpp`, `src/contour/test/Accessibility_qml_test.cpp`.

**Interfaces:**
- Consumes: Task 3 (`pendingPermission`'s keys, `answerPermission`), Task 1 (`Permission.*`).
- Produces (`KitDialog.qml`, every addition defaulting to today's behaviour):
  - `property alias bodyData: bodySlot.data` — a `ColumnLayout { id: bodySlot; objectName: "kitDialogBody" }` between
    the message and the buttons;
  - `property bool acceptEnabled: true` (the accept button's `enabled`; Return does nothing while false);
  - `enum DefaultButton { Accept, Reject }` and `property int defaultButton: KitDialog.Accept` — that button takes the
    initial focus and Return/Enter;
  - `kitDialogHeading` and `kitDialogMessage` set `textFormat: Text.PlainText`; the content item gains
    `Accessible.description: root.message`.
- Produces (`PermissionDialog.qml`, `objectName: "permissionDialog"`):
  - `required property var prompt` (the map), `signal answered(int decision, int memory)`;
  - heading = `prompt.title`; `message` empty; body: a `Repeater` of `permissionLine` Labels (`textFormat:
    Text.PlainText`, `font.bold: modelData.strong`, `verbatim` → `wrapMode: Text.NoWrap` and `elide: Text.ElideRight`,
    else `Text.Wrap`; colour `sys.windowText`), then a `CheckBox` `permissionRemember` (`text: prompt.rememberText`,
    `checked` initially `prompt.initialMemory !== Permission.Once`, `Accessible.role: Accessible.CheckBox`,
    `Accessible.name: text`);
  - `acceptText`/`rejectText` from the map; `acceptEnabled: !(prompt.allowNeedsRemember && !permissionRemember.checked)`;
    `defaultButton` from `prompt.defaultDecision`;
  - Allow → `answered(Permission.Allow, checked ? prompt.rememberScope : Permission.Once)`; Deny likewise with
    `Permission.Deny`; `closePolicy: Popup.NoAutoClose` and an Escape handler on the content → `answered(Permission.Deny,
    Permission.Once)` (R-g); every way it closes hands focus back to `chrome.displayItem`;
  - content `Accessible.name: prompt.title`, `Accessible.description`: the lines joined by newlines.
- SessionChrome: `PermissionDialog { prompt: chrome.session ? chrome.session.pendingPermission : ({}) ; onAnswered:
  (decision, memory) => chrome.session.answerPermission(decision, memory) }`, opened whenever `pendingPermission` has
  a title — on its change signal and on `onSessionChanged`/`Component.onCompleted` (a rebound session shows what it
  owes), closed when it empties.

- [ ] **Step 1: Write the failing tests** (`QmlComponents_test.cpp`, `[contour][gui][qml][permission]`, a
  `SessionChrome` over a `MockSession` whose `pendingPermission` is set from the Task 3 builders' output for a paste
  and for a capture):
  - "the dialog shows the session's question": heading `kitDialogHeading.text` is the title; two `permissionLine`s are
    Verbatim with `elide == Text.ElideRight`; the buttons read "Cancel" and "Paste".
  - (Review Focus 5) "keyboard-only answering": paste prompt → `QTest::keyClick(window, Qt::Key_Return)` → the mock
    records `(Allow, Once)`; capture prompt → Return → `(Deny, Once)`; capture prompt, Tab until
    `permissionRemember.activeFocus`, Space, then focus Deny and Space → `(Deny, Session)`; capture prompt with the box
    ticked, Escape → `(Deny, Once)`; after each, the dialog is closed and `displayItem.activeFocus`.
  - (Review Focus 1) "the dialog reopens for the next question": answering the first prompt while the mock then sets a
    second → after one event-loop turn the dialog is open with the second title.
  - "a rebound session shows the question it owes": a `SessionChrome` created for a mock whose `pendingPermission`
    is already set opens the dialog without any signal.
  - "a program-chosen title renders as text": title `"Allow “<img src=x>” to read the screen?"` → the heading's text
    is that string and `textFormat == Text.PlainText`.
  - "Terminal row: the dialog's lines read on its surface": under the Terminal row, for contour-dark and contour-light,
    `contrastRatio(permissionLine color, the KitDialog surface) >= 4.5` (as 1a's KitDialog Terminal-row case does).
  - `Accessibility_qml_test.cpp`, `[contour][gui][qml][a11y]` (Review Focus 5): "the permission dialog's accessible
    tree": the content item is `Accessible.Dialog` named by the title with the lines as description;
    `permissionRemember` is `Accessible.CheckBox` named "Don't ask again in this session"; both buttons are named by
    their text.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][permission]"`

  Expected: FAIL, `permissionDialog` has no `kitDialogHeading` (it is still Task 3's `RequestPermission`).
- [ ] **Step 3: Implement** the KitDialog additions, the component and the SessionChrome wiring.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][permission],[contour][gui][qml][kit],[contour][gui][qml][a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][permission],[contour][gui][qml][kit],[contour][gui][qml][a11y]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Commit** (`editorconfig-checker` the QML and `CMakeLists.txt`):

```bash
git add src/contour/qml/KitDialog.qml src/contour/qml/PermissionDialog.qml src/contour/qml/SessionChrome.qml src/contour/CMakeLists.txt src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
git commit -m "contour: one kit dialog for every permission wall, titled by its action" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Extension and control consent on the same pair

**Files:**
- Modify: `src/vtcontrol/Catalog.hpp:41-76` — a weight column on `PermissionPhrases`. Test `src/vtcontrol/Catalog_test.cpp`.
- Modify: `src/vtextension/ExtensionConsent.hpp:57-63`, `ExtensionConsent.cpp:114-141` — `consentText` gives way to
  `consentQuestion`. Test `src/vtextension/ExtensionConsent_test.cpp:97-224` (the `consentText` cases move onto it).
- Modify: `src/contour/session/PermissionPrompt.hpp/.cpp` — the consent prompt and `consentVerdictFor`.
- Modify: `src/contour/session/TerminalSession.hpp:840-853`, `:1330`; `TerminalSession.cpp:1037-1067`,
  `:1224-1245` — consent joins the queue; the three consent invokables, `pendingExtensionPermissionText` and
  `requestPermissionForExtension` go.
- Modify: `src/contour/qml/SessionChrome.qml` — `requestExtensionCommandsDialog` (`:345-357`) and its handler go; drop
  `import Qt.labs.platform` if nothing else in the file uses it. Delete `src/contour/qml/RequestPermission.qml` and its
  `_qml_files` line (`src/contour/CMakeLists.txt:390`).
- Modify: `src/contour/test/QmlMocks.hpp:169-176`, `:199` (the extension stubs go).
- Test: `src/contour/session/TerminalSessionExtensions_test.cpp:371-470`, `:630-640`;
  `src/contour/test/QmlComponents_test.cpp:1864-1912` (rewritten onto `permissionDialog`);
  `src/contour/session/PermissionPrompt_test.cpp`.

**Interfaces:**
- Produces (`vtcontrol`): `enum class ConsentWeight : uint8_t { Observes = 0, CanRunCommands };`; `PermissionPhrase`
  gains `ConsentWeight weight = ConsentWeight::Observes;` (Input and Write are `CanRunCommands`; every row names it);
  `[[nodiscard]] constexpr ConsentWeight consentWeightOf(Permission permission) noexcept;`.
- Produces (`vtextension`): `struct ConsentLine { std::string text {}; vtcontrol::ConsentWeight weight = vtcontrol::ConsentWeight::Observes; };`,
  `struct ConsentQuestion { std::string title {}; std::vector<ConsentLine> lines {}; };`,
  `[[nodiscard]] ConsentQuestion consentQuestion(ExtensionConsentRequest const& request);` — title
  `“{name}” wants to control Contour` when the manifest declares control permissions, else `“{name}” wants to run
  commands`; lines `Module: {hostId}`, then `{command name}: {displayedCommandLine}` per declared command (with
  today's "It may run only these, exactly as written ({name} is filled in by the extension):" before them), then
  `May {phrase}.` per control permission, a `CanRunCommands` one as `May {phrase} (can run commands).`; every guest
  text through `neutralizedForDisplay`.
- Produces (`session/PermissionPrompt.hpp`): `inline constexpr std::string_view ExtensionRememberText = "Remember for
  this extension";`, `[[nodiscard]] PermissionPrompt consentPrompt(std::string title, std::vector<PromptLine> lines);`
  (deny "Deny", allow "Allow", `rememberScope = initialMemory = Persistent`, `defaultDecision = Deny`, `allowOnce =
  Withheld`; a `CanRunCommands` line is `Strong`), `enum class ConsentVerdict : uint8_t { Dismiss = 0, Deny, Allow };`,
  `enum class ConsentAnswerError : uint8_t { AllowMustBeRemembered = 0, ScopeNotOffered };`,
  `[[nodiscard]] std::expected<ConsentVerdict, ConsentAnswerError> consentVerdictFor(PermissionDecision decision, PermissionMemory memory) noexcept;`
  — `(Allow, Persistent)` Allow, `(Deny, Persistent)` Deny, `(Deny, Once)` Dismiss, `(Allow, Once)`
  AllowMustBeRemembered, any `Session` ScopeNotOffered (R-f).
- `TerminalSession`: on the consent queue's Due, `_permissionQueue.offer(ExtensionConsentAsk {})`; `pendingPermission`
  for that ask is the consent prompt of `_extensionState->consent->current()`; `answerPermission` for it maps through
  `consentVerdictFor` to `vtextension::ConsentAnswer` (Dismiss → Dismissed, Deny → Denied, Allow → Allowed), logs and
  keeps the prompt on an error, and re-offers when the consent queue is Due again. Without `CONTOUR_EXTENSIONS` the
  ask never arises.

- [ ] **Step 1: Write the failing tests.**
  - `Catalog_test.cpp`, `[vtcontrol][catalog]`: `consentWeightOf(Permission::Input) == CanRunCommands`, `Write` too,
    `Read` and `Screen` `Observes`; every row of `PermissionPhrases` is in enumerator order (the existing
    `static_assert` still holds).
  - `ExtensionConsent_test.cpp`, `[vtextension][consent]`: a manifest named `"git-stats"` with `read` and `input` →
    title `"“git-stats” wants to control Contour"`, the `input` line `"May type into panes (can run commands)."` with
    `CanRunCommands`, the `read` line `Observes`; commands only → `"… wants to run commands"`; a name with `‮`
    and `<b>` shows `<U+202E>` and keeps `<b>` as text (the old cases' assertions, moved).
  - `PermissionPrompt_test.cpp`, `[contour][permission][prompt]`: the `consentVerdictFor` table (all six pairs);
    `consentPrompt` defaults as above, and a `CanRunCommands` line comes out `Strong`.
  - `TerminalSessionExtensions_test.cpp`, `[contour][session][extensions]`: the allow/deny/dismiss cases drive
    `answerPermission(Allow, Persistent)`, `(Deny, Persistent)`, `(Deny, Once)` with today's recorded outcomes;
    `(Allow, Once)` records nothing and leaves the prompt pending. (Review Focus 1) "an extension waits behind a guarded
    role": a capture ask, then an extension ask → `pendingPermissionAsk()` is the capture; after answering it, the
    consent; the daemon case (`:448`) answers through `answerPermission` and reaches the daemon's `IGrantStore`.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][extensions]`: the consent prompt shows a bold
    `permissionLine` for the `input` grant, `permissionRemember` ticked with "Remember for this extension"; unticking
    disables `kitDialogAccept`; Allow with it ticked records `(Allow, Persistent)`; Deny unticked `(Deny, Once)`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtcontrol_test vtextension_test contour_gui_test`

  Expected: compile error, no `vtcontrol::ConsentWeight`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vtcontrol/vtcontrol_test "[vtcontrol][catalog]"
out/appleclang-debug/src/vtextension/vtextension_test "[vtextension]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][permission],[contour][session][extensions],[contour][gui][qml][extensions],[contour][gui][qml][permission]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][extensions]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
grep -rnE "RequestPermission|allowPendingExtension|denyPendingExtension|dismissPendingExtension|requestPermissionFor|consentText\(" src
```

  Expected: PASS; the grep prints nothing.
- [ ] **Step 5: Lint, format and commit** the files above by name (with `git rm src/contour/qml/RequestPermission.qml`):
  `git commit -m "contour: extension and control consent answer through the permission dialog" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 9: Save Layout as a kit sheet

**Files:**
- Create: `src/contour/window/SaveLayoutCheck.hpp` (Qt-free); test `src/contour/window/SaveLayoutCheck_test.cpp` in
  `contour_test`.
- Modify: `src/contour/window/WindowController.hpp:255-262`, `WindowController.cpp:749-767`.
- Rewrite: `src/contour/qml/SaveLayoutDialog.qml` over `KitDialog`; delete its line in
  `scripts/qml-size-literal-baseline.txt`.
- Modify: `src/contour/qml/Main.qml:242-249` (the instance keeps its id and inputs).
- Modify: `src/contour/test/MockMainController.hpp:222-223`, `src/contour/test/MainWindowControllerMock.hpp`, and the
  stand-in at `QmlComponents_test.cpp:4063-4082`.
- Test: `src/contour/test/QmlComponents_test.cpp:4161-4210`, `src/contour/window/WindowMenus_test.cpp`.

**Interfaces:**
- Consumes: Task 5/6 (`LayoutSaved`, `LayoutSaveFailed`, `notices()`), Task 7 (`acceptEnabled`).
- Produces (`window/SaveLayoutCheck.hpp`): `enum class LayoutNameState : uint8_t { Empty = 0, Available, Taken };`,
  `enum class LayoutReplace : uint8_t { Keep = 0, Replace };`, `enum class SaveLayoutButton : uint8_t { Disabled = 0, Save, Replace };`,
  `struct SaveLayoutCheck { LayoutNameState name = LayoutNameState::Empty; SaveLayoutButton button = SaveLayoutButton::Disabled; constexpr bool operator==(SaveLayoutCheck const&) const noexcept = default; };`,
  `[[nodiscard]] inline SaveLayoutCheck checkLayoutName(std::string_view trimmed, std::span<std::string const> existing, LayoutReplace replace);`
  (`@pre trimmed` is already trimmed), `[[nodiscard]] inline std::string layoutSummaryText(int tabs, int panes);` —
  `"{n} tab(s) · {m} pane(s) · working directories and commands"` with singular forms for 1.
- Produces (`WindowController`): `[[nodiscard]] Q_INVOKABLE QVariantMap checkLayoutName(QString const& name, bool replace) const;`
  (carve-out; `name`, `button` as ints of the enums, `trimmed`), `[[nodiscard]] Q_INVOKABLE QString layoutSummary() const;`
  (tabs and `Tab::paneCount()` of this window), `Q_INVOKABLE void saveLayoutAs(QString const& name);` (refuses a taken
  or empty name), `Q_INVOKABLE void replaceLayout(QString const& name);`; both call `_manager.saveWindowLayout(_windowId,
  …)` and post `layoutSavedNoticeText` or `layoutSaveFailedNoticeText`.
- QML (`SaveLayoutDialog.qml`): a `KitDialog` with `heading: qsTr("Save Layout")`; body: a `Label` "Name", the
  `TextField` `saveLayoutNameField`, `saveLayoutSummary` (footnote), and `saveLayoutTaken` (visible while Taken and not
  confirmed: `"“%1” already exists."` plus a flat `Button` `saveLayoutReplace` "Replace?"); accept "Save", or
  "Replace" with `acceptRole: chromeStyle.statusDanger` once confirmed; `acceptEnabled` from the check; reject
  "Cancel"; Return in the field accepts only when enabled; `onClosed` restores terminal focus as today.

- [ ] **Step 1: Write the failing tests.**
  - `SaveLayoutCheck_test.cpp`, `[layout][savecheck]`: existing `{"dev", "ops"}`: `""` → Empty/Disabled; `"web"` →
    Available/Save; `"dev"` + Keep → Taken/Disabled; `"dev"` + Replace → Taken/Replace; `"Dev"` → Available/Save;
    `"web"` + Replace → Available/Save. `layoutSummaryText(1, 1) == "1 tab · 1 pane · working directories and
    commands"`, `(3, 5)` the canvas's text.
  - `WindowMenus_test.cpp`, `[contour][windowmenus][layout]` with an `InMemoryLayoutStore`: `checkLayoutName("  dev ",
    false)` on a store holding `dev` gives `Taken` and `trimmed == "dev"`; `saveLayoutAs("dev")` leaves the store
    unchanged (Also pinned: a name taken between check and click); `replaceLayout("dev")` rewrites it and
    `notices().toastText() == "Saved layout “dev”"`; a store whose `load` fails → `toastText()` ends with
    `describe(LayoutSaveError::StoreUnreadable)` + `"."`.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][layout]` (the two existing cases kept): Save is disabled for an
    empty and an all-space name; enabled for `"web"`; for `"dev"` `saveLayoutTaken` shows and Save is disabled;
    clicking `saveLayoutReplace` makes the accept read "Replace" with `acceptRole == chromeStyle.statusDanger`;
    typing a character withdraws it; Return on a disabled Save leaves the dialog open; on the MacOS row the dialog's
    `y == 0` (sheet). Under the Terminal row, for contour-dark and contour-light, the Replace button's label meets
    `contrastRatio(onStatusColor(danger), statusColor(danger)) >= 4.5`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `window/SaveLayoutCheck.hpp` not found.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[layout][savecheck]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][windowmenus],[contour][gui][qml][layout],[contour][gui][qml][kit]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][layout]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit** the files above by name:
  `git commit -m "contour: Save Layout says what it saves and asks before replacing a layout" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"`.

---

### Task 10: Snapshot rows, and the Terminal row's pass

> **Depends on Steps 4a, 1c, 1d and 2** (and 3a/3b where they landed first): every existing row names their fields
> (`SnapshotSurface`, `.drift`, `.frame`, `.mode`, `.density`, `.paneEnd`, `.find`, …); the new rows name them too, at
> their defaults.

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` (`SnapshotScript :35-49`, `ScriptAction :66-74`, `SnapshotBanner
  :84-89`, `SnapshotScriptRow :97-107`, `SnapshotScripts`), `src/contour/test/ChromeSnapshotScene.hpp` (driving the
  window's `notices`, the mock session's `pendingPermission`, the Save Layout dialog).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Produces: `enum class SnapshotPermission : uint8_t { None = 0, Paste, Consent };`,
  `enum class SnapshotLayoutName : uint8_t { None = 0, Available, Taken };`; `SnapshotScriptRow` gains
  `SnapshotPermission permission = SnapshotPermission::None;` and `SnapshotLayoutName layoutName = SnapshotLayoutName::None;`;
  `ScriptAction` gains `OpenPermission` and `OpenSaveLayout`; `SnapshotBanner` gains `ConfigReload`. Rows appended in
  this order: `PermissionPaste` (`"permission-paste"`, `OpenPermission`, `Paste`: the Task 3 builder's prompt for a
  2.4 MB paste into "zsh" with two preview lines), `PermissionConsent` (`"permission-consent"`, `Consent`: "git-stats"
  with `read`, `screen`, `input`), `SaveLayout` (`"save-layout"`, `OpenSaveLayout`, `Available`, "dev" typed, three
  tabs), `SaveLayoutTaken` (`"save-layout-taken"`, `Taken`), `ConfigReloadBanner` (`"config-reload-banner"`, banner
  `ConfigReload` with `configReloadFailedNoticeText("yaml-cpp: error at line 3, column 5: end of map not found")`). The
  existing `Toast` script posts Copied through the window's `notices` instead of the scene's centred pill.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): `scriptRow(SnapshotScript::PermissionConsent).permission ==
    SnapshotPermission::Consent`, `scriptRow(SnapshotScript::SaveLayoutTaken).layoutName == SnapshotLayoutName::Taken`;
    every row names both fields; the enumerator-order `static_assert` holds with the new last enumerator; counts from
    `SnapshotScripts.size()`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): each new script builds on the MacOS and Terminal rows
    without a QML warning; `permission-paste` finds `permissionDialog` open with "Paste"; `permission-consent` finds a
    bold `permissionLine`; `save-layout-taken` finds `saveLayoutTaken` visible; `config-reload-banner` finds
    `noticeBanner` visible; `toast` finds `noticeToast` inside `titleBar` on the MacOS row.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `SnapshotScript::PermissionPaste`.
- [ ] **Step 3: Implement** the rows and the scene's handling.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
```

  Expected: PASS.
- [ ] **Step 5: Format and commit:**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git add src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git commit -m "contour: snapshot the permission dialog, consent, Save Layout and the reload banner" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, so the plan's own
  commit is left out. Apply its findings, re-run the filters of every task it touches, and commit once with the
  Signed-off-by line, staging only the files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[pastepreview],[notices],[toastplacement],[layout][savecheck],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][permission],[contour][session][permission],[contour][session][clipboard],[contour][session][extensions],[contour][session][manager],[contour][notices],[contour][windowmenus],[contour][session],[contour][display],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][permission],[contour][gui][qml][extensions],[contour][gui][qml][layout],[contour][gui][qml][mainwindow][notices],[contour][gui][qml][kit],[contour][gui][qml][a11y],[contour][gui][qml][snapshot]"
out/appleclang-debug/src/vtbackend/vtbackend_test "[screen]"
out/appleclang-debug/src/vtcontrol/vtcontrol_test "[vtcontrol]"
out/appleclang-debug/src/vtextension/vtextension_test "[vtextension]"
out/appleclang-debug/src/vthost/vthost_test "[vthost][host]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
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

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, the size read from
  `ChromeSnapshotPlan.hpp`. Look at `permission-paste.*` (a sheet on macOS, two preview lines, "Don't ask again in this
  session", Cancel · Paste), `permission-consent.*` (the `input` grant in bold, the box ticked), `save-layout.*` and
  `save-layout-taken.*` ("Replace?" inline), `config-reload-banner.*` (the banner under the band, "Show in Editor"),
  `toast.*` (the pill in the band); on the Terminal row each is boxed with the hard shadow, `[x]`, bracketed buttons.
- [ ] **Step 4: Owner manual checks.** Batched per phase (ruling): list them in the ledger; do not wait on them. On this
  Mac, `out/appleclang-debug/src/contour/contour.app`:
  - `printf '\e[>0,t'` with `capture_buffer: ask`: a sheet titled "Allow “zsh” to read the screen?", Deny focused;
    Return denies; VoiceOver reads the title and "Don't ask again in this session, checkbox".
  - Copy 600 KB of text and paste: "Paste 600.0 kB into “zsh”?" with two lines and the rest counted; Return pastes.
    Copy 50 MB (`yes | head -c 50000000 | pbcopy`) and paste: a toast, no dialog, nothing typed.
  - Font change and capture in one `printf`: two sheets, one after the other.
  - Break `contour.yml` and save: a banner in every window; "Show in Editor" opens the file; fix and save: it goes.
  - Select three lines and ⌘C: "Copied 3 lines" in the band; hover keeps it.
  - Save Layout with an empty, a new and an existing name; "Replace?" then Replace.
  - An extension with `run_command: ask` and a `control: input` grant: "… wants to control Contour", the input line in
    bold, Allow disabled while "Remember for this extension" is unticked.
  - In a daemon-attached window, `printf '\e[>0,t'` returns at once (an empty reply).
  - `ui_style: terminal`: the same sheets boxed in cells.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Asks every permission (font change, large paste, buffer capture, host status line, rerun) in one dialog that names
    the action, with a preview of a large paste and a checkbox to stop asking for the session; questions are asked one
    at a time."
  - "Shows configuration reload errors in the window, with a button that opens the file, and confirms copies, refused
    pastes and saved layouts with a short notice that screen readers announce."
  - "Asks extension and control consent in the same dialog, with the permissions that can type shown in bold."
  - "Save Layout says what it saves, stays disabled for an empty name, and asks before replacing an existing layout."
  - "Refuses a buffer capture in a daemon-hosted pane instead of leaving the program waiting for the reply."
- [ ] **Step 6:** Mark Step 3c done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-3c.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the terminal paths; a large paste counts its lines once when it is held (O(n) over at most
    1 MiB) and copies at most two 160-byte prefixes; a notice is one `QTimer` per window.
  - **Risk:** medium. The guarded-role API, the dialog and the consent flow change together; the queue changes when the
    second of two questions appears; a daemon capture now replies. Mitigated by the per-role table test, the queue and
    reload cases, the keyboard tests and the daemon test.
  - **Coverage:** `[pastepreview]`, `[notices]`, `[toastplacement]`, `[layout][savecheck]` tables; the permission
    prompt, queue, reload and announcement cases; the QML dialog, keyboard and accessibility cases on the native and
    Terminal rows; the daemon refusal; five new snapshot scripts.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Consent without "Remember for this extension" (R-f).** An extension's grant can only be recorded. *Default: the box
   starts ticked; unticked, Allow is disabled and Deny means "not now" (asked again in a new pane).* The alternative
   is a session-scoped grant layer in `vtextension`'s runtime, so Allow could hold for one pane.
2. **Where a toast goes with no title band (R-j).** *Default: top centre of the content, or bottom centre when the
   cursor is in the top two rows.* The alternatives reveal the band over the top rows for the toast's lifetime, or
   send such notices to the tray.
3. **A daemon-hosted pane's capture (R-h).** *Default: the daemon refuses it; nothing is asked.* The alternative relays
   the question to the attached client over a new PDU pair, as extension consent does through `DaemonGrantReplies`.
4. **The default button (R-d).** *Default: Deny for what a program asks and for a rerun, Allow for the user's own large
   paste.* The alternative is the macOS convention, the action button everywhere, which lets a stray Return grant a
   capture.
5. **A reload that changes a role's setting (R-e).** *Default: it forgets that role's "don't ask again" answer.* The
   alternative keeps every remembered answer until the session ends.

## Spec gaps found

- §3.9's citations are stale (current lines in the *Spec* paragraph); the guarded-role API has five `(bool, bool)`
  executors because `applyPendingRerun` joined; two of the three reload-error sites it cites no longer log.
- "Both are `Q_ENUM`" puts the enums in a Qt header, so the prompt decisions that use them test in `contour_gui_test`,
  not the Qt-free `contour_test` (R-a).
- "Restore summaries": no restore event reaches the client; the row ships here, Step 6.2 posts it (R-l).
- "Deny and Allow" against the canvas's verbs ("Cancel" · "Paste"): the buttons answer Deny/Allow and carry the
  canvas's labels (R-m).
- The canvas's "Paste 2.4 MB" is above today's 1 MiB hard limit; the limits are unchanged (R-n).
- Allow-once for consent has no representation in `IGrantStore` (owner 1); Escape's meaning is unspecified (R-g).
- Daemon-hosted panes never ask any guarded role: capture was dropped (fixed here), the host-writable status line is
  always shown, a font change is ignored; profile permissions do not reach the daemon.
- On the `daemon` board but not in §3.9: the "Opened by contour new (agent)" Toast (no window origin reaches the client)
  and the failed-attach Dialog with Retry · Copy Details. The canvas's "5 key bindings were skipped" banner needs load
  warnings the loader does not return.
- Toast placement without a band is unspecified (owner 2); the canvas says only "in the title band … never over the
  prompt row".
