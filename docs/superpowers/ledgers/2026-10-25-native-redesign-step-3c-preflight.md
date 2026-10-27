# Pre-flight scan — Step 3c (notices, permission dialogs, consent, Save Layout)

Plan: `docs/superpowers/plans/2026-10-25-native-redesign-step-3c.md` (commit `0382e4ca`, written at `fdb54a64`).
Verified against `work/native-redesign-step-2` at `159f3fb2` (Step 1c done, Step 1d Tasks 2–6, Step 2 Tasks 1–6, 8–10;
Step 2 Task 7 dispatched, Task 11 uncommitted in `../contour-step2`, Tasks 12–13 open). Spec §3.9 (`:577-619`), §1
(`:28-64`), plans index, 3a plan + pre-flight, 3b plan. Read-only; nothing built.

**Counts: 3 blocking, 21 non-blocking. 7 owner questions.**

Every symbol, signature and signal the plan names exists on the Step 2 branch with the signature the plan assumes
(`GuardedRole`, `PermissionCache = std::map<GuardedRole, bool>`, the five `(bool allow, bool remember)` executors,
`executeRole(GuardedRole, bool, bool)`, `requestCaptureBuffer(LineCount, bool logical)` on `Terminal::Events`,
`Terminal`, `Screen::captureBuffer`, `MockTerm`, `TerminalSession`; `HostedSession::Events` with
`onShowHostWritableStatusLine`; `ExtensionConsentQueue`, `consentText`, `PermissionPhrases`, `LayoutSaveError` +
`describe`, `InMemoryLayoutStore`, `RecordingExternalLauncher::openedUrls`, `KitDialog`, `Toast`, `Banner`,
`statusDanger`, `size.toast`, `ManualClock`). Only line numbers moved (§2). Step 1c's renames (`BarText` → `BarInk`,
the status text/mark split) do not touch anything 3c cites: `statusColor(role)` is the text ink and
`onStatusColor(role)` still exists. What did change under the plan is the band model (F1).

---

## 1. Cross-task pairs (shared file or interface)

| Producer → consumer | Shared | Finding |
|---|---|---|
| T1 → T2, T3, T7, T8 | `Permission.hpp` enums, `PermissionCache`, QML `Permission.*` | OK. First `Q_ENUM`/`Q_NAMESPACE` in the tree (`UiStyleProvider.hpp:364`, `ChromeTokenObjects.hpp:445` avoid `Q_ENUM` on purpose); R-a's stop-for-ruling covers it. See F15. |
| T2 → T3 | executors' visibility | T3 drops `Q_INVOKABLE`; T2's table test and `DisplayRendering_test` call the executors directly — they must stay public (F16). |
| T2 → T3 | `SessionChrome.qml:291-358` handlers | T2 rewrites five handlers T3 deletes; harmless churn. |
| T3 → T7 | `pendingPermission` map keys | Keys match exactly (`title`, `lines{text,strong,verbatim}`, `denyText`, `allowText`, `rememberText`, `rememberScope`, `initialMemory`, `defaultDecision`, `allowNeedsRemember`). Enum values inside a `QVariantMap` — F15. |
| T3 → T5 | `session::formatByteCount` | T5's `pasteTooLargeNoticeText` uses it, but T5's *Consumes* does not name T3. Order is fine (3 < 5); note it. |
| T3 → T8 | `PermissionQueue`/`ExtensionConsentAsk`, `PermissionPrompt::allowOnce` | OK. |
| T3 ↔ T4 | `CaptureBufferRequest::logical` → `mode` | T3 builds the capture prompt from "the largest pending capture"; T4 retypes the struct (`TerminalSession.hpp:2147-2151`). Disjoint fields. OK. |
| T5 → T6 | `NoticeModel(clock, announcer, onAction)` | `WindowController` already takes `IClock const& clock` (Step 2 Task 9, `WindowController.hpp:193-198`); pass `_clock` (F9). |
| T5 → T9 | `layoutSaveFailedNoticeText(name, LayoutSaveError)` | Needs a `NameTaken` row in `describe()` if F2's resolution is taken. |
| T6 → T10 | `noticeToast` in `titleBar` | Depends on F1's placement contract; existing `ChromeSnapshot_test.cpp:134-140` looks for `objectName == "toast"` (F20). |
| T7 → T8, T9 | KitDialog `bodyData`, `acceptEnabled`, `defaultButton` | OK; Escape needs one more KitDialog hook (F11). |
| T8 → T10 | `consentPrompt` for the snapshot | OK (gui target only; `ChromeSnapshotPlan.hpp` stays enum-only for `contour_test`). |
| 3c T6, T9 ↔ Step 2 T11 (uncommitted) | `WindowController.{hpp,cpp}`, `MainWindowControllerMock.hpp`, `QmlMocks.hpp`, `QmlComponents_test.cpp`, `Accessibility_qml_test.cpp`, `WindowMenus_test.cpp`, `TerminalSession.hpp`, `CMakeLists.txt` | F3: 9 of 3c's files are dirty in `../contour-step2`. |
| 3c T6 ↔ Step 2 T11 | `Toast.qml` (the reconnecting pill is a `Toast`), `PaneCard` footer | T6's `textFormat` is harmless to the pill; ContentBottom can sit on an ended pane's footer (F14). |
| 3c T6 ↔ Step 1c / Step 2 T7 | band model (`BarPresence`, `BarEdge`, bare band) | F1 (blocking). |
| 3c T10 ↔ Step 2 T12, 3a, 3b | `ChromeSnapshotPlan.hpp` rows | `.density`/`.paneEnd` are Step 2 Task 12's and not on the branch yet (F3). Append-only otherwise. |
| 3c T3, T7, T8 ↔ 3b T10 | `SessionChrome.qml`, `QmlMocks.hpp` `MockSession` | Different hunks (3b: `SearchBar`, `MockSearchSession` folded into `MockSession`; 3c: dialogs `:291-358`, `Connections` `:484-497`, permission stubs `:210-250`). Rebase. |
| 3c T7 ↔ 3b T10 | `PopupSurface.qml` (KitDialog's `background`) | 3b makes the shadow corner follow `root.radius`; KitDialog passes no radius — re-run `[contour][gui][qml][kit]` after whichever lands second. |
| 3c T2–T8 ↔ 3b T7/T8 | `TerminalSession.{hpp,cpp}` | Disjoint blocks (find bar vs permission gate). |
| 3c T4 ↔ 3a T3 | `testing/MockTerm.hpp`, `vtbackend/CMakeLists.txt`, `Terminal.hpp` | 3a adds a `FileSystem` ctor (`:52-88`); 3c edits `requestCaptureBuffer` (`:460-463`) and adds `core/CaptureLineMode.hpp`. Disjoint. |
| 3c T4 ↔ 3b T3/T4 | `Screen.{hpp,cpp}`, `Terminal.{hpp,cpp}` | Capture vs search blocks; disjoint. |
| all ↔ 3a, 3b | `metainfo.xml`, `scripts/qml-size-literal-baseline.txt`, `src/contour/CMakeLists.txt` | Append/shrink only. |

## 2. Per task (self-consistency; references on the Step 2 branch)

Line map, plan → branch (all symbols present):
`TerminalSession.hpp` `:94-110`→`:102-115`; `:823-833`→`:941-951`; `:835-837`→`:953-955`; `:840-853`→`:957-971`;
`:895`→`:1013`; `:1274`→`:1400`; `:1281`→`:1407`; `:1324-1330`→`:1459-1465`; `:1979-1983`→`:2147-2151`;
`:2015`→`:2183`; `:2110-2127`→`:2297-2314`. `TerminalSession.cpp` `:952-964`→`:1011-1023`;
`:966-1032`→`:1025-1092`; `:1037-1067`→`:1097-1126`; `:1214-1245`→`:1296-1335` (`offerExtensionConsent` now takes an
`IGrantStore&`, and `askForHostedExtension` feeds it); `:1318-1340`→`:1427-1450`; `:1343`→`:1452`; `:1389`→`:1498`;
`:1440`→`:1549`; `:1806-1835`→`:1976-2002`; `:1862`→`:2030`; `:1921`→`:2089`; `:1948-1951`→`:2116-2119`;
`:4122-4145`→`:4340-4371`; `:4197`→`:4415`; `:4364-4395`→`:4582-4610`; `:5893-5911`→`:6111-6130`.
`SessionChrome.qml` `:288-357`→`:291-358`, `:467-478`→`:484-497`. `Main.qml` `:149-186`→`:273-318`
(`bannerStack` now holds `hostingNoticeBar` + `newDefaultsBanner`), `:242-249`→`:378-386`. `WindowController.hpp`
`:255-262`→`:283-291`, `.cpp` `:749-767`→`:780-798`, ctor `:49-85`→`:56-~90`. `TerminalSessionManager.cpp`
`:85-89`→`:94-95`, `:744`→`:753`. `ContourGuiApp.cpp` `:2388-2395`→`:2415-2422`, `:2798-2800`→`:2870-2871`.
`QmlMocks.hpp` `:163-167`→`:210-214`, `:169-176`→`:216-224`, `:199`→`:246-250`. `test_main.cpp` `:116`→`~:144`.
`CMakeLists.txt` `:278`→`:285`, `:327`→`:334`, `:390`→`:397`, `_qml_files :385-428`→`:392-440`,
`contour_gui_test :769-866`→`:779-…`, `contour_test :1153-1205`→`:1170-1230`. `Terminal.hpp` `:757`→`:761`,
`:3161`→`:3165`. `SessionHost.hpp :434-491`→`:403-…`. `Announcer.hpp :52-72`→`:46-70`.
`TerminalSession_test.cpp` `RecordingAnnouncer :5457`→`:5533`, `captureWholePage :4373`→`:4453`.
`QmlComponents_test.cpp` consent `:1864-1912`→`:1994-2042`; Save Layout stand-in `:4063-4082`→`:5188-5207`, cases
`:4161-4210`→`:5286-5400`. `TerminalSessionExtensions_test.cpp` `:371-470`→`:371-503` (`:497` too), `:630-640`
(unchanged, bundled-only).

| Task | Self-consistent | References on Step 2 branch |
|---|---|---|
| 1 | Yes. | All present; registration list moved (`ContourGuiApp.cpp:2415-2422`). |
| 2 | Yes; test-site list is one short in each file (28 in `TerminalSession_test.cpp`, 9 in `DisplayRendering_test.cpp`: `:794,820,828,835,934,2538,2551,2556,2618`) — the Step 4 greps catch them. Filter `[contour][display]` misses the display tests (F5). | Present. |
| 3 | Yes; F12, F13, F16, F18. Spies now at `:1300, :1388, :4480, :6654`; `DisplayRendering_test.cpp:787, :808`. MockSession has no rerun stub or signal (nothing to remove). | Present. |
| 4 | Yes. `ExitingPty` (`SessionHost_test.cpp:214`) records no writes — extending it is allowed by the plan; mind that it closes right after its output (the pump flushes inside the loop, `SessionHost.cpp:~183-186`). | Present; `Screen.cpp:4556-4562` exact. |
| 5 | Yes; F9 applies at T6. | Present. |
| 6 | **No** — F1 (placement contract), F6 (mock factory), F9, F10, F14, F19. | `bannerStack`, `titleBar`, `content` present by `objectName`; `band` is a JS object of strings. |
| 7 | Mostly; F11 (Escape). | Present; `SessionChrome` is hosted by `TerminalPane` inside Step 2's `PaneCard`. |
| 8 | Missing a call site (F7) and a fixture detail (F8). | Present. |
| 9 | **No** — F2. | `saveWindowLayout` present; Save Layout tests live in `MultiWindow_test.cpp:940-990`, not `WindowMenus_test.cpp`. |
| 10 | Depends on Step 2 Task 12 (F3); F20. | Rows currently end at `.mode`; no `.density`/`.paneEnd` yet. |
| 11 | F17 (diff base). | — |

## 3. Findings

**F1 — blocking (Tasks 6, 10).** "`enum class BandPresence : uint8_t { Hidden = 0, Shown };` … `Q_INVOKABLE int
toastHost(bool bandShown)`" and R-j "With no band (a single tab under `Multiple` on Fluent/ContourLinux, full screen
before reveal, Minimal)". Step 1c already owns this vocabulary: `window::BarPresence { Hidden, Shown, RevealOnEdge,
Pinned }` and `BarEdge { Top, Bottom }` (`TitleBandLayout.hpp:56-76`), surfaced to QML as `appWindow.band.band` /
`.bandEdge` strings. The plan's two-state copy cannot say Pinned (full screen, remote interrupted: the band overlays the
top rows), RevealOnEdge (concealed or revealed), or a bottom-edge band (rules at `:167`, `:185`) — "vertically centred
on `titleBar`" would then put a "top centre" toast at the bottom. And Step 2 Task 7 gives Minimal a *bare* band
(`bareBand` 32/28/28, `ChromeTokenGroups.hpp:807/881/952`) shorter than `size.toast` 36, so R-j's "Minimal has no band"
is stale and a 36 pt toast centred on a 28 pt band covers the first content row. Resolution: `ToastPlacementInputs`
takes `BarPresence band; BarEdge edge; int bandHeight; int toastHeight; int cursorRow; int coveredRows`; TitleBand only
for `edge == Top` and `band ∈ {Shown, Pinned}` and `bandHeight ≥ toastHeight`, else ContentTop/ContentBottom by cursor
row; QML passes the enumerators' ints as it already does to `chromeStyle.titleBand` (no new `bool`). Delete
`BandPresence`. Task 6's "Fluent row with one tab (no band)" case must be re-derived from `TitleBandRules` (and the
TestApp's `tab_bar_visibility`). Owner Q1 decides the bottom-edge case.

**F2 — blocking (Task 9).** R-o: "'Taken' means a key of the app's merged `Config::layouts`", `saveLayoutAs` "refuses
a taken name"; test: "`checkLayoutName("  dev ", false)` on a store holding `dev` gives `Taken`" and "Save on a name
taken between the check and the click does not overwrite". Three problems. (a) Seeding `InMemoryLayoutStore` does not
put `dev` into `Config::layouts`, so the test as written reads Available. (b) `saveWindowLayout` re-reads the store and
writes `(*storedLayouts)[name] = layout` unconditionally (`TerminalSessionManager.cpp:902-913`); a check against the
in-memory config cannot stop a name written to `layouts.yml` since the last reload — the "does not overwrite" guarantee
needs the check where the store is read. (c) The saveLayoutAs/InMemoryLayoutStore tests live in
`window/MultiWindow_test.cpp` (`[contour][multiwindow]`), which already uses `MockPtySessionFactory`; `WindowMenus_test`
has none of this. Resolution: `saveWindowLayout(WindowId, std::string const& name, LayoutReplace replace)` refusing
with a new `LayoutSaveError::NameTaken` (plus its `describe` row) when `replace == Keep` and the freshly loaded store
*or* `Config::layouts` holds the name; `saveLayoutAs` → Keep, `replaceLayout` → Replace; `checkLayoutName` keeps
reading `Config::layouts` for the live UI. Tests in `MultiWindow_test.cpp` with `MockPtySessionFactory` +
`InMemoryLayoutStore`, seeding the store and asserting `NameTaken`; the existing "saveLayoutAs persists under the typed
name" section (`:978-986`) stays green. Filter `[contour][multiwindow]`.

**F3 — blocking (sequencing; Tasks 6, 9, 10).** "Steps 4a, 1c, 1d and 2 … are assumed done." On the branch, Step 2 Task
7 (Minimal's bare band — changes `TitleBandLayout.hpp`, F1's input) is in flight, Task 11 is uncommitted in
`../contour-step2` touching 13 files (9 of them 3c's: `WindowController.{hpp,cpp}`, `MainWindowControllerMock.hpp`,
`QmlMocks.hpp`, `QmlComponents_test.cpp`, `Accessibility_qml_test.cpp`, `WindowMenus_test.cpp`, `TerminalSession.hpp`,
`CMakeLists.txt`), and Tasks 12 (snapshot `.density`/`.paneEnd`) and 13 are open. Resolution: start 3c only after
Step 2 Finish (Task 13) commits; re-run the §2 line map then (Tasks 1–5 and 8 could start earlier in a separate
worktree: they touch none of Step 2's open files except `TerminalSession.hpp` and `QmlMocks.hpp`, which are rebaseable).

**F4 — non-blocking (all).** Stale line numbers, listed in §2. Locate by name; nothing referenced has disappeared.

**F5 — non-blocking (Tasks 2, 3, 4).** "`contour_gui_test "[contour][display]"`". The permission display cases are
`"[display][permissions]"` (`DisplayRendering_test.cpp:776`) and `REQUIRE_DISPLAY_OR_SKIP()`; `[contour][display]`
matches three unrelated tests. The rewritten sites at `:787-835`, `:934`, `:2538-2618` never run. Add
`CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][permissions],[display][fonts]"`
to those steps (CI's weston route runs them too).

**F6 — non-blocking (Tasks 6, 9). Real sessions.** `TestApp`'s default factory is `nullptr` (`GuiTestFixtures.hpp:493`,
`withTestNotifier` passes it through), i.e. the production PTY factory. Task 6's "with two windows … `reloadAllSessions()`"
and its `TerminalSession_test` copy/paste cases ("the hosting controller's `notices()`" — a session inside a window,
created by the manager) and Task 9's save cases create tabs. Each must construct `TestApp { std::make_unique<
MockPtySessionFactory>() }` (as `MultiWindow_test.cpp:943` does). State it in the steps.

**F7 — non-blocking (Task 8).** "`consentText` gives way to `consentQuestion`". `vthost/client/NativeClient_test.cpp:1598`
calls `vtextension::consentText` and is not in Task 8's file list, nor is `vthost_test` in its build line — the stale
`vthost_test` binary would still pass `ctest -L daemon`. Add the file, rewrite the assertion onto `consentQuestion`'s
lines, and build `vthost_test`. Also list `TerminalSessionExtensions_test.cpp:478-503` (`:497` reads
`pendingExtensionPermissionText`). The `:613-644` case is inside `#ifdef CONTOUR_TEST_SPAWNER_WASM`: with
`BUNDLED=OFF` it is not even compiled locally — only the grep and CI check it.

**F8 — non-blocking (Task 8).** "(Review Focus 1) 'an extension waits behind a guarded role': a capture ask, then an
extension ask". `TerminalSessionExtensions_test.cpp`'s `makeSession` (`:62-66`) has no display, and
`requestCaptureBuffer` refuses at once without one (`TerminalSession.cpp:1436-1441`). Use a surfaced session (the
`sessionCapturing` pattern of `TerminalSession_test.cpp:4433`) or a font change on a surfaced session.

**F9 — non-blocking (Task 6).** "the clock is `core::platform::defaultSteadyClock()` (the composition default,
documented)". `WindowController` already receives `core::platform::IClock const& clock` (held as `_clock`, used by the
held-modifier reveal). Give the `NoticeModel` that same `_clock`: one injection point per window, and a test that
drives the reveal with a `ManualClock` drives the toast too.

**F10 — non-blocking (Task 6; §1.7).** Task 6 edits `pasteFromClipboard(unsigned count, bool strip)` (the hard-limit
branch, `:1976-1986`), and Task 3 extends `BigPasteRequest` beside it. Spec §1.7: "When this design touches an API
that takes `bool`s, it fixes the API in the same change." Resolution: `enum class PasteStrip : uint8_t { Keep = 0,
Strip }` on the session API, converted from `actions::PasteClipboard::strip` (a config/action boundary `bool`) at the
handler; rewrite the ten test call sites (`TerminalSession_test.cpp:1288-1398`, `:3468`). Owner Q5 if declined.

**F11 — non-blocking (Task 7).** "`closePolicy: Popup.NoAutoClose` and an Escape handler on the content". The content
item is KitDialog's own `ColumnLayout`; a derived `PermissionDialog` cannot attach `Keys.onEscapePressed` to it
declaratively, and with `CloseOnEscape` both Escape and the Deny button arrive as `rejected`, indistinguishable. Add a
fifth KitDialog addition: `signal escaped()` raised from the content's `Keys.onEscapePressed` (closing as today when
`closePolicy` has `CloseOnEscape`), so PermissionDialog answers `(Deny, Once)` there and `(Deny, checked ? scope :
Once)` on the button.

**F12 — non-blocking (Task 3).** "each shown line cut to at most `bytesPerLine` bytes … `…` appended when cut, then
`vtbackend::sanitizeCommandLine(…)`" and the test "each at most 163 bytes". Sanitizing *after* the cut expands controls
into multi-byte replacements, so the 163-byte bound fails for a line of control bytes. Sanitize a bounded prefix first,
then cut at a UTF-8 lead byte; add a row with 200 `\x01` bytes to `pastePreview.*`.

**F13 — non-blocking (Task 3).** "an ask equal to one already waiting is not queued again (… a later font change or
paste replaces the earlier)". The prompt is built when read, but nothing re-emits `pendingPermissionChanged` when the
front's facts change (a second paste replaces `_pendingBigPaste`, a later font def, more captures), so the open dialog
shows the old size/font. Emit it when a coalesced ask updates the front. Also: after R-e's reload, if the front role's
setting became `allow`/`deny`, answer it with the configured decision and `finish()` it, rather than leaving a dialog
whose answer drains nothing.

**F14 — non-blocking (Task 6).** (a) `Toast.qml` is always `visible` (only `opacity` follows `shown`), so the plan's
`HoverHandler` reports hover over an invisible toast and suspends the *next* toast's deadline — `enabled:
noticeToast.shown`. (b) `Toast` is `Accessible.AlertMessage` named by its text, and `NoticeModel` also announces — check
VoiceOver does not read it twice (the a11y test can assert one announcement through `RecordingAnnouncer`). (c) Step 2
Task 11's `ExitedFooter` sits at an ended pane's bottom; ContentBottom covers it in a single ended pane — prefer
ContentTop when the active session's `sessionEnd != 0`.

**F15 — non-blocking (Tasks 1, 3, 7).** `pendingPermission` carries enums as `QVariant::fromValue(enum)`, and QML
compares `prompt.initialMemory !== Permission.Once`. Whether a `uint8_t`-based `Q_ENUM_NS` inside a `QVariant`
reaches JS as a number is exactly what R-a's round trip doubts; put `std::to_underlying(…)` ints in the map (the map is
already the QML carve-out) and keep the enum types for the `Q_INVOKABLE` parameters. Spell those parameters with the
declaring namespace (`contour::session::permission::PermissionDecision`) rather than the `using` alias, so moc's
normalized signature names the registered type. Extend Task 1's test to read an enum back out of a `QVariantMap`.

**F16 — non-blocking (Task 3).** "the executors lose `Q_INVOKABLE`". Keep them public: Task 2's member-pointer table
and `DisplayRendering_test` call them directly.

**F17 — non-blocking (Task 11).** "`git diff <the plan commit>..HEAD`". The plan commit `0382e4ca` is 64 commits behind
the Step 2 tip, so `/simplify` would review Steps 1c, 1d and 2. Use the commit 3c starts from (record it in the
ledger at Task 1).

**F18 — non-blocking (Tasks 3, 6; DI).** Task 3's reload case uses "`contour::test::writeConfigFile` +
`config::tryLoadConfigFromFile`" — real file I/O for something `applyReloadedConfig(config::Config const&, …)` takes
in memory: copy `app.config()`, change `permissions`, pass it. Task 6 has no choice today
(`reloadAllSessions` reads `_app.config().configFile` from disk directly, `TerminalSessionManager.cpp:757`), as the
existing manager cases do; use `TempDir` and say so.

**F19 — non-blocking (Task 6).** `Q_PROPERTY(contour::window::NoticeModel* notices …)`: follow the
`CommandPaletteModel` precedent and register `NoticeModel` uncreatable in `Contour.Terminal`
(`ContourGuiApp.cpp:2415-2422`), which Task 6 does not list.

**F20 — non-blocking (Task 10).** "The existing `Toast` script posts Copied through the window's `notices`". The existing
assertion (`ChromeSnapshot_test.cpp:134-140`) finds the item by `objectName == "toast"`; with `noticeToast`, and Step 2
Task 11's `reconnectingPill` also a `Toast`, rewrite it to `noticeToast`.

**F21 — non-blocking (Tasks 3, 6, 9; bool).** New QML-facing `bool`s: `toastHost(bool bandShown)` (removed by F1),
`checkLayoutName(QString, bool replace)`, and the map's `strong`/`verbatim`/`allowNeedsRemember`. All are within the
documented carve-out, but the tree's own precedent passes enumerator ints (`chromeStyle.titleBand(…)`); prefer
`int replace` (a `LayoutReplace`) and the enums' ints for `PromptLineWeight`/`PromptLineStyle`/`AllowOnce`. The
guarded-role pair itself is fully fixed by Task 2 (both greps are right); `setToastHovered(bool)` mirrors
`HoverHandler.hovered` and may stay.

**F22 — non-blocking (Task 11 Step 6).** The plans index row for 3c (`:89`) names `…-step-2.md` in its Plan column (the
shift 3a's pre-flight F17 noted). Fix while marking 3c done.

**F23 — non-blocking (Task 6; R-k).** `CopyPreviousMarkRange`-style copies (`TerminalSession.cpp:4128`,
`copyToClipboard(extractLastMarkRange())`) are explicit copies too and post nothing under R-k. Owner Q6.

**F24 — non-blocking (Task 4).** `docs/vt-extensions/buffer-capture.md` (cited at `TerminalSession.hpp:2160`) should say
a daemon-hosted pane refuses with the terminating chunk.

### Flag checks

- **Real sessions:** F6 (Tasks 6, 9). Tasks 2–4, 8 use `makeDisplaylessSession`/`makeSessionWithSurface`/
  `makeTestSession` over `MockPty`, and `SessionHost_test` over `ExitingPty` — fine.
- **Real I/O:** F18. Clipboard in tests is Qt's offscreen clipboard (existing practice).
- **QML literals:** none planned; `SaveLayoutDialog.qml` 5 → 0 and `RequestPermission.qml` deleted. `SessionChrome.qml`
  is at 11: if removing the dialogs drops a literal, regenerate its baseline line in the same task (the gate fails a
  file *below* its count) — Tasks 3/8 do not list `scripts/qml-size-literal-baseline.txt`.
- **bool parameters:** guarded-role pair fixed (Task 2); `requestCaptureBuffer`/`captureBuffer` fixed (Task 4);
  `pasteFromClipboard(…, bool strip)` touched but not fixed (F10); new carve-outs F21.
- **Renamed Step 1c/2 names:** none of 3c's citations were renamed; the band model is richer than the plan assumes (F1);
  `WindowController` gained an `IClock` (F9); the prompt's subject can come from the session's existing title
  (`titleChanged`), as `MockSession`'s Step 2 `title`/`setTitle` already models.

## 4. Spec asks no task delivers

- **"Restore summaries"** — only the `RestoreSummary` row and its text ship; nothing posts one (R-l defers to Step 6.2).
  Recorded as a spec gap; no further action.
- Everything else in §3.9 is delivered: NoticeModel with hover hold and Polite announcement (T5/T6), config reload
  errors (T6, through `reloadAllSessions()`, which covers both remaining call sites `TerminalSessionManager.cpp:94-95`
  and `ContourGuiApp.cpp:2870-2871`), "Copied N lines" (T6), one KitDialog with two buttons and one checkbox (T7),
  the enums as `Q_ENUM` and `PermissionCache` of `PermissionDecision` (T1/T2), checkbox labels by scope (T3/T8),
  consent migrated (T8), Save Layout (T9), and the four named tests.

## 5. Owner questions left open (proposed defaults)

1. **A bottom-edge band (`BarEdge::Bottom`).** *Default: the toast goes to the content's top centre (the canvas says
   "top center"), using the cursor rule; TitleBand only for a top band at least `size.toast` high.*
2. **A toast in the band covers the band's centre (tabs or title) and takes its clicks while shown.** *Default: accept,
   as the canvas draws it; the toast is short-lived and hover holds it only on purpose.*
3. **The hosting notice and the new-defaults offer stay their own banners** beside the NoticeModel's one banner slot.
   *Default: yes — their lifetimes are owned by `HostNotice`/`NewDefaultsNotice`; the stack sums three.*
4. **The permission sheet hangs from its pane, not the window,** in a split (it is parented to `SessionChrome`; R-c
   keeps one per pane). *Default: keep pane parenting; on macOS the sheet hangs from the pane's top edge.*
5. **`pasteFromClipboard(…, bool strip)` in scope (F10).** *Default: fix it in Task 6 with `PasteStrip`.*
6. **Other explicit copies post "Copied N lines"** (`CopyPreviousMarkRange`, vi yank). *Default: yes for every explicit
   copy action; copy-on-select stays silent.*
7. **A `SaveLayout` keybinding naming an existing layout** keeps overwriting it silently (only the dialog asks).
   *Default: keep — a name written into the configuration is the intent; F2's `LayoutReplace::Replace` is what the
   action passes.*
