# Pre-flight scan — Step 3e (size indicator, overlay scrollbar, link pill, hint labels)

Plan: `docs/superpowers/plans/2026-10-25-native-redesign-step-3e.md` (written at `0382e4ca`, before 1c, 1d and 2).
Verified against `work/native-redesign-step-2` at `ebbb09d67` (= main line `6f71105fe` + Step 2's `5d042135`,
`159f3fb2`, `ebbb09d6`), read from git objects. Spec §3.11 (`:643-671`), the plans index, AGENT.md, the Step 2 SDD
ledger and the 3a/3b/3c pre-flights and rulings (execution order 3a → 3b → 3c → 3d → 3e).

**Summary.** 37 findings: **10 blocking** (F1–F10), 27 non-blocking (F11–F37). 7 owner questions. Every symbol the
plan cites still exists under the name it uses, except the PaneCard header id (`header`, not `paneHeader`) and
`headerPresent`'s meaning (ruled by 3b's pre-flight). Almost every line number moved (remap in §2). The plan is
sound in shape. What blocks it is how it meets the code around it: where `applyResize` takes its lock, the indicator
status line, Step 2's inline-vs-overlay header, 3b's find bar, QML mocks the plan does not list, and the two thread
contexts of `bufferChanged`/`screenUpdated`.

---

## 1. Shared files and interfaces (pairs of tasks)

| Pair | Shared file / interface | Verdict |
|---|---|---|
| T1 → T2, T3, T4 | `config/ScrollBarMode.hpp` (`ScrollBarMode`, `resolveScrollBarMode`) | Consistent. T2's `ScrollbarVisibilityInputs` also needs `config::ScrollBarPosition`, which is in `Config.hpp:103` (**F25**). |
| T1 ↔ T11 | `Config.cpp`, `ConfigDocumentation.hpp`, `Config_test.cpp` | Different hunks (scrollbar reader `:2959` vs scheme reader `:2124` / writer `:5160`). Fine. |
| T2 → T4 | `vtbackend::marksAbove`, `OverlayHold` | T4 computes it only when `refreshScrollbarMarks` re-reads its inputs (**F11**). |
| T3 → T4 | `applySystemScrollBars` (no-op in T3, filled in T4) | Fine. T3 stages `TerminalSession.{hpp,cpp}`. |
| T4 ↔ T6 ↔ T10 | `TerminalSession.{hpp,cpp}` | Three hunks in one file, run in sequence. T4's `bufferChanged` hook is a thread problem (**F8**). |
| T5 → T6 → T12 | `SizeIndicator.hpp` → `requestSizeIndicator`/`sizeIndicatorSettled` → QML | The text is read before the resize applies (**F1**). The plural announcement fails its test (**F2**). |
| T6 ↔ FontControl | `applyResize` (`FontControl.cpp:112-176`) | The request must come after `resizeScreen` and outside the lock (**F1**). |
| T7 → T10 | `linkPillText`, `LinkTargetReveal`, `linkPillVisibility`, `HoverChange`, `linkOpenHint` | The tracker re-processes the text it is handed (**F17**). Mouse tracking is ignored (**F19**). |
| T8 → T10 → T9 → T14 | `Terminal::overlayClearance()` → `TerminalSession::overlayClearance()` map → `placeLinkPill` → `LinkPill.qml` | The status line is missing from the clearance (**F3**). The keys (`trailingBlank`, `avoidRows`, margins, cell size) agree across T9, T10 and T14. |
| T9 → T12, T13, T14, T15 | tokens (`size.linkPill/scrollbar`, `space.scrollbarInset`, `radii.*`, `motion.linger`), 4 roles, `scrollThumbGlyph` | Consistent. The alpha sources need care (**F23**). |
| T10 → T14 | `QmlMocks.hpp` `MockSession` (tooltip props swapped in T10) | T10 breaks `QmlComponents_test.cpp:5490` until T14 (**F6**). |
| T12 ↔ T13 ↔ T14 ↔ T15 | `TerminalPane.qml`, `SessionChrome.qml`, `Toast.qml`, `LinkPill.qml`, `scripts/qml-size-literal-baseline.txt` | One file per task, in order. The baseline is regenerated in every task: rebase onto the previous one. |
| T12, T14 ↔ `KeyboardRouting_test.cpp` | `RoutingMockSession` binds "every property TerminalPane/SessionChrome binds" | Not listed in the plan (**F7**). |
| T13 ↔ SessionChrome `markColor()` | `vbar.handleColor` (`SessionChrome.qml:410`) | T13 deletes `handleColor`, and `markColor()` still reads it (**F9**). |
| T16 ↔ Step 2 T12 | `ChromeSnapshotPlan.hpp` `SnapshotScriptRow` (`.density`, `.paneEnd`, Minimal scripts) | Not on the branch yet (**F10**). |
| **3e T14 ↔ 3b T10/T11** | `PaneHeader.qml` trailing slot (3b `findSlot` + `findActive`), `TerminalPane.headerPresent`, the grid's top right (3b's native find bar) | Collision (**F4**, **F5**, **F22**). |
| 3e T12/T14 ↔ 3c T6 | `Toast.qml` (3c `textFormat: Text.PlainText` on `toastText`) | Keep 3c's line, and add the same to the new `toastDetail` (**F13**). Toast is always `visible` (3c's F14, **F12**). |
| 3e T10/T14 ↔ 3c T6 | "never over the prompt row" (3c `ToastPlacementInputs.cursorRow`) vs 3e `avoidRows` (live prompt span + cursor) | Two definitions of one rule (**F21**). |
| 3e T9 ↔ 3b T9, 3d (tokens) | `ChromeColorRole` (append after the last at execution), `SizeTokens`, `MotionTokens` (3d `popoverScalePercent`), `UiStyleTokens` glyphs, `UiStyleProvider` invokables (3b `findBarPlacement`) | Append by name. Every row that uses designated initialisers names the new fields. |
| 3e T11 ↔ 3b T5 | `ColorPalette.cpp` `ContourScheme` (3b adds `searchMatch`, `searchMatchInk`) / `applyContourScheme` | 3e adds no scheme field, only three lines in `applyContourScheme`. Rebase. |
| 3e T7 ↔ 3d | `command/Shortcut.{hpp,cpp}` | 3e appends `linkOpenHint` after `describeLinkGesture` (`.hpp:208`, `.cpp:157-168`). Low risk. |
| 3e T2 ↔ 3f | `ScrollbarMarkKindTable` | 3e adds the `overlay` column, and 3f's ticks read the table. 3f follows 3e (index). |
| 3e T1 ↔ Step 2 / 3a | `EditionDefaults` (Step 2 added `chromeDensity`; 3a edits a constant only) | Append after `chromeDensity`. `ExpectedEditionDefaults` is now `Config_test.cpp:4005`. |

## 2. Per task: self-consistency and whether the references still exist

| Task | Self-consistent | References on `ebbb09d67` (plan → actual) |
|---|---|---|
| 1 | Yes | `ScrollBarConfig :586-602`→`:587-603`; `EditionDefaults :1035-1057`→`:1036-1059`; table `:1085-1124`→`:1087-1136`; profile `scrollbar :1220`→`:1224`; `hyperlinkHoverTooltip :1778`→`:1782`; `format(…, ScrollBarConfig) :2828-2831`→`:2837-2840`; reader `Config.cpp:2959-2967` ✓; docs `ScrollbarConfig :525`→`:535`, `ScrollbarWeb :2752`→`:2776`, `HyperlinkHoverTooltipConfig :253` ✓, `…Web :2393`→`:2404`; `ExpectedEditionDefaults :3910-3936`→`:4005-4030`, expectations →`:4031-4105`, `checkProfileDefaults :4109`, `checkEditionDefaults :4165`, `Config.firstLaunchDefaults :4191` ✓; `DefaultsEdition.hpp` is listed in `config/CMakeLists.txt:18` (not `src/contour/CMakeLists.txt`); `contour_test` list `:1153-1202`→`:1171-1220`. `Config_test` runs in `contour_gui_test` ✓. |
| 2 | Yes | `ScrollbarMarkKindRow :86-101` ✓, table `:107-124`→`:106-123`; `[scrollbar-marks]` exists (55). `SearchStatus.hpp` is in no CMake list (**F32**). |
| 3 | Yes | `PlatformAppearance.hpp:14-20` ✓; Cocoa `:22-80` ✓ (also the early-return `:61`); `onPlatformAppearanceChanged :1916-1919`→`ContourGuiApp.cpp:1982-1985`; `updateMotionPreference .hpp:499-501`→`:543`, `.cpp:1191-1195`→`:1203-1207`. `AppearanceFixtures.hpp`/`QmlChromeStyle.hpp` name no field, so they need no change. The designated initialisers are `AppearanceSources_test.cpp:68,:90` and `CocoaPlatformAppearance.mm:61,:66` (**F33**). |
| 4 | **No** (F8, F11) | `isScrollbarVisible :215`→`:229`; `getIsScrollbarVisible :472-483`→`:505-516`; members `:1955-1966`→`:2114-2148`; ctor `:368-392`→`:367-…` (`_startTime :378`); `bufferChanged :836-842`→`:850-863`; `onScrollOffsetChanged :2760-2770`→`:2983-2993`; `announceScrollableLineCount :3554-3560`→`:3773-3779`; `refreshScrollbarMarks :3587-3654`→`:3805-3866`; profile notifiers `:5850-5859`→`:6070-6080`. `registerMarkedScrollbarProfile :1659`, `writeFiller :1677`, `numberedOutput :467` ✓. Filters `[contour][session][scrollbar-marks]` (7), `[contour][gui][qml][scrollbar]` (2) ✓. |
| 5 | Mostly (F14) | New file. `TerminalClocks::steady` ✓ (`vtbackend/screen/TerminalClocks.hpp`). |
| 6 | **No** (F1, F2, F16) | properties `:201-217`→`:215-236`; signals `:1306-1332`→`:1422-1457`; font actions `:4420`/`:4719`→`Decrease :4633`, `Increase :4930`, `Reset :5078`; `setFontSize :6174`→`:6392`; `applyResize :145-150` ✓ (the function now runs to `:176`); `showResizeIndicator :203/:517`→`:217/:550`; a11y case `:5475`→`:5552`. `_app.terminalClocks()` (`ContourGuiApp.hpp:465`), `testApp.steadyClock().advance()` ✓. |
| 7 | Mostly (F17) | `HyperlinkTooltip.hpp :62-110` ✓, `.cpp :77-124` ✓; `describeLinkGesture .hpp:208`, `.cpp:157-168` ✓; `modifiersText(…, Text)` returns `"Ctrl+"`, so `"Ctrl+Click to open"` ✓. `vtbackend::FollowModifiers` is in `screen/LinkPresentation.hpp:20` (HyperlinkTooltip.hpp must include it). |
| 8 | Yes (F3 is in the consumer) | `livePromptSpan .hpp:2881`→`:2885`; `Line::trimmedColumns()` (`grid/Line.hpp:606`) matches R-g's "blank" exactly. `Terminal_test.cpp` is in `vtbackend/CMakeLists.txt:365`, `screen/Terminal.hpp :114`. |
| 9 | Mostly (F23, F24) | `ChromeColorRole :115-153`, last `PaneHeaderActionHover`, count `:199-200`; `SizeTokens :267`, `SpaceTokens :309`, `RadiusTokens :331`, `MotionTokens :370` (it has `style` and `headerRevealMs`, the precedent for an unreduced hold), Reduced branch `:1170-1176`; `label() :618`, `schemeColor() :594`; `UiStyleTokens :79`→`:80`, `NativeTokens :312`→`:348`, `TerminalTokens :383`→`:423`; `colorOn :339`, `paneHeader(...) :322`; `chromeTokens.everyRowMatchesTheDesignKit :164` ✓. |
| 10 | **No** (F6, F18, F19, F20) | `.hpp :219-222`→`:229-236`; `:265`→`:298`; `:348`→`:381`; `:360-397`→`:395-425`; `:1294`→`:1429`; `:1756-1773`→`:1920-1937`; `.cpp` hover `:3140-3218`→`:3355-3428`; `hoveredDetectedLinkTooltip :3438-3456`→`:3656-3674`; `updateHyperlinkHover :3458-3476`→`:3676-3695`; `clearHyperlinkHover :3680-3687`→`:3898-3905`; `onHeldModifiersChanged :3734-3739`→`:3952-3957`; `screenUpdated :852`→`:865`. Tests `:6761`→`:6838`, `:7066`→`:7143`, `:7147-7200`→`:7224-7290`, `:7186`→`:7263`, `:4584`→`:4661`. `Terminal::followModifiers() :1578` ✓. |
| 11 | Yes | `ColorPalette.hpp :299-300` ✓; `applyContourScheme :114-133`→`:114-138`; `applyHintOverlay :5395-5531`→`:5409-5536`; `RenderCell::sizing` (`render/RenderBuffer.hpp:78`) ✓; reader `:2123-2124`→`:2124`; writer `:5160-5170` ✓; `HintModeHandler.LabelAssignment.TwoChar`, `LinkPatterns.userPatternIsAppendedAndHintOnly` ✓. The scheme colours match the plan's hex values exactly. |
| 12 | **No** (F7, F12) | `TerminalPane.qml :129-170`→`:153-193` (`sizeWidget :156`, `updateSizeWidget :181-192`), `Connections :240-252`→`:255-266`; Toast case `:6270`→`:7423`; `[contour][gui][qml][kit][toast]` (4) ✓. |
| 13 | **No** (F9) | `ScrollBar :135-194`→`:135-193`, `contentItem :195-205` ✓, `background :207-212` ✓, tick layer `:215-283`→`:214-284`; tests `:1958`→`:2089`, `:2056`→`:2187`. |
| 14 | **No** (F4, F5, F7) | `hyperlinkTip :59-91`→`:57-91`; tooltip test `:4374`→`:5490`; `PaneHeader.qml` names `paneHeaderStatus :128`, `paneHeaderActions :169` ✓; in `PaneCard.qml` the header's id is **`header`**, and `headerInline`/`headerShown` exist (`:52-53`). |
| 15 | Yes | `motion.fast/standard` QML names (`ChromeTokenObjects.hpp:335-336`) ✓. |
| 16 | **No** (F10) | `SnapshotScript :35`, `PaneShape :60` (`SingleLeaf`, `SplitThree`), `ScriptAction :74-83`, `SnapshotScriptRow :137-153`, which still lacks `.density` and `.paneEnd` (Step 2 T12 is open). |
| 17 | Mostly (F30, F19) | Snapshot matrix `ChromeSnapshotPlan.hpp:538` ✓. |

---

## 3. Findings

### Blocking

**F1 — blocking (Task 6).** "`applyResize` calls `session.requestSizeIndicator(SizeIndicatorKind::PageSize)` once when
either dimension changed, after its two signals"; "formats the text from the page size".
*Why:* in `FontControl.cpp` the two signals (`:145-150`) fire **before** `terminal.resizeScreen(fit.pageSize, …)`
(`:173`), which runs inside `std::scoped_lock { terminal }` (`:164`). A request placed after the signals reads the
**old** page. Today's QML has the same bug: `updateSizeWidget` reads `pageColumnsCount` synchronously from those
signals. `fit.pageSize` is the **total** page, status line included, while today's text reads `mainPageSize()`
(`TerminalSession.hpp:545-548`). Native2026 ships an indicator status line by default, so the pill would read
"100 × 31". Emitting under the terminal lock to QML that reads back into the session also invites the
non-recursive-mutex deadlock the code base guards against elsewhere.
*Resolution:* record `changed` before the lock, and call `requestSizeIndicator(PageSize)` **after** the lock scope
ends, following `resizeScreen`. Format from `terminal.mainPageSize()`. Add a test that the text equals the *new* main
page after `applyResize` (display test) or after `resizeScreen` (displayless).

**F2 — blocking (Task 6).** "`tr("%n column(s)", "", columns) + ", " + tr("%n row(s)", "", lines)` … exactly one
announcement, `"100 columns, 30 rows"` … `"1 column, 1 row"`".
*Why:* Contour loads no translation catalogue with English plural forms (the only `QTranslator` is the menu one in
`AppMenuBar.cpp:77`). Without one, `tr("%n column(s)")` yields "100 column(s)", which is what `tr("%n match(es)")`
(`TerminalSession.cpp:2933`) prints today. The test fails.
*Resolution:* add a Qt-free `sizeIndicatorAnnouncement(SizeIndicatorKind, PageSize, double points)` to
`SizeIndicator.hpp` that picks singular or plural with a two-row table, and test it in `[sizeindicator]`. The session
passes the result to `announce()`. Localisation is a later `tr()` of whole patterns.

**F3 — blocking (Tasks 8, 9, 10, 14; spec "only ever over blank cells").** "`trailingBlankColumns` — one entry per
row of the page on display"; rule 4: "rows outside the grid (the margins) are clear"; rule 5: "else
`yb = paneHeight − insetTop − pillHeight` … `BottomRight`".
*Why:* `pageSize()` and `mainPageSize()` exclude the indicator status line. Both editions default to
`StatusDisplayType::Indicator` at `Bottom` (`Config.hpp:690-691`). The status line's rows therefore fall "outside the
grid" and count as clear, so BottomRight places the pill over the status line's text. That breaks principle 5 and
§3.11. With `position: Top` the same happens at TopRight.
*Resolution:* `OverlayClearance` covers every row the display draws, status line included, and reports the status
rows' entries as 0 (they are drawn chrome). Its row origin is the display's first drawn row, so QML's `marginTop`
mapping stays one formula. Add `overlayClearance.theStatusLineIsNeverBlank` (Task 8) and a Task 9 case with a bottom
status line where BottomRight is refused.

**F4 — blocking (Task 14; 3b's ruled F5).** "`PaneCard.qml` → `TerminalPane { headerLinkSlot: paneHeader.linkPillSlot;
headerPresent: paneHeader.visible ? 1 : 0 }` … (3b's name, reused if present)".
*Why:* (a) the card's header id is `header` (`PaneCard.qml:89`). (b) 3b's pre-flight F5 was ruled (3b P1):
`headerPresent: card.headerInline ? 1 : 0`, because Minimal's header is an **overlay** while ⌥ is held
(`policy.placement === "overlay"`). `visible ? 1 : 0` would re-host the pill on every ⌥ press, and would place it in
an overlay header that covers cells. When 3e lands, `headerPresent` already exists with the ruled meaning, so 3e
must not redefine it.
*Resolution:* bind `headerLinkSlot: header.linkPillSlot` and reuse 3b's `headerPresent` as it stands
(`card.headerInline`). Add a Minimal case: holding ⌥ with the follow modifier held never moves the pill into the
overlay header.

**F5 — blocking (Task 14 vs 3b Tasks 10–11).** "`PaneHeader.qml`: `linkPillSlot` (at the trailing edge, full header
height) and `property bool linkPillActive` … `paneHeaderStatus` and `paneHeaderActions` are hidden"; single pane:
"at the top right (`s4` from the top, `s5` from the right)".
*Why:* 3b, which lands first, puts its find bar in the same places. On the Terminal row in a split it takes a
`findSlot` that runs "from after `paneHeaderName` to the trailing edge" (with `findActive` hiding the same two
items). On the native rows it sits at the grid's top right under the header (3b plan `:12`, `:1080-1085`). With the
find bar open and ⌘ held over a link, the pill and the bar overlap in both places, and two flags fight over one
header's status and actions.
*Resolution:* while the find bar is shown, the bar owns the header slot and the top right. The pill goes to the
grid, with the bar's rows passed as avoided rows (BottomRight or nowhere). `placeLinkPill` gains a `blockedTop`
input (or `findBarRows`), with a test. See **F22** for the header's state and **Q2** for the owner.

**F6 — blocking (Task 10, every commit builds).** "`QmlMocks.hpp`'s `MockSession` swaps its two tooltip properties
for the four new ones".
*Why:* `QmlComponents_test.cpp:5490` ("SessionChrome shows the hyperlink tooltip …") calls
`session->setHyperlinkHover(...)` five times, and that test is rewritten only in Task 14. After Task 10 the GUI test
target no longer compiles.
*Resolution:* in Task 10, **add** the four properties and `overlayClearance()` to `MockSession` and keep
`setHyperlinkHover`/`hyperlinkTooltip*`. Task 14 removes them together with the tooltip and its test.

**F7 — blocking (Tasks 12, 14).** Task 12 lists only `QmlMocks.hpp` as the mock to change. Task 14 binds
`session.linkTargetText/Hint/Shown`, `session.overlayClearanceRevision` and calls `session.overlayClearance()`.
*Why:* `session/KeyboardRouting_test.cpp:90-125` has `RoutingMockSession`, documented as "exposing every property the
TerminalPane/SessionChrome QML binds", and it drives the real `TerminalPane.qml`. Without the new properties, typed
bindings get `undefined` and the call throws a `TypeError`. Both are QML warnings, and `test_main.cpp`'s run-wide gate
fails the suite on them. `QmlComponents_test.cpp`'s own mocks (`MockSearchSession`, which 3b folds into `MockSession`)
need the same check.
*Resolution:* Tasks 12 and 14 add the properties to `RoutingMockSession` (constants: empty text, not shown,
revision 0, an empty `overlayClearance()` map), and drop its `showResizeIndicator`/`upTime` in Task 12. Also guard
the QML reads as the tooltip guards them today (`!== undefined`, `SessionChrome.qml:68-76`). Grep every
`Q_PROPERTY(.*pageLineCount` mock before each commit.

**F8 — blocking (Task 4).** "`refreshScrollbarVisibility()` (private, GUI thread) … `bufferChanged`, the posted half of
`onScrollOffsetChanged`, `announceScrollableLineCount` and the profile notifiers call it too"; it "stores
`_scrollbarVisibility`".
*Why:* `bufferChanged` (`TerminalSession.cpp:850`) runs on the **terminal thread**, possibly with the state mutex
held, and returns early without a display (`:852`). A GUI-thread function called there races the GUI and may
self-lock. In a displayless test the cached value is never refreshed on `\033[?1049h`, so the plan's "alternate
screen stays hidden" case reads a stale `Shown`. Today's `getIsScrollbarVisible()` is computed on read, which is why
it works without a display.
*Resolution:* keep `getIsScrollbarVisible()` a pure read: `resolveScrollbarVisibility(inputs())`, where the inputs
come from the profile, `_systemScrollBars`, the displayed screen type, `_lastHistoryLineCount`, the scroll offset and
the cached `_marksAbove`. Make the change notification the posted part: `bufferChanged` posts
(`platform::postToObject`) a GUI-thread `notifyScrollbarVisibility()`, which compares and emits. Test the alternate
screen on a displayless session.

**F9 — blocking (Task 13).** "The luminance-picked white or black `handleColor` and its literals go."
*Why:* `SessionChrome.qml:405-411` `markColor()` falls back to `vbar.handleColor` when the session has published no
tick colours (a mock, or before the first refresh). Removing the property makes that fallback `undefined`, a QML
warning in every tick test that runs on a mock.
*Resolution:* in Task 13, change the fallback to the thumb colour
(`chromeStyle.colorOn(chromeStyle.roles.scrollbarThumb, session.backgroundColor)`, or `colors.scrollbarThumb` on the
Terminal row), and extend the existing marks test (`:2366`) to a mock with no colours.

**F10 — blocking (sequencing; Tasks 9, 12–16).** "Steps 4a, 1c, 1d and 2 … are assumed done"; Task 16 "every
existing row names their fields".
*Why:* Step 2 is not finished. Task 7 (band), Task 11's fix round, Task 12 (`.density`, `.paneEnd`, Minimal
snapshot scripts) and Task 13 are open, and `SnapshotScriptRow` (`ChromeSnapshotPlan.hpp:137-153`) has neither
field. 3b, 3c and 3d also land first and touch 9 of 3e's files (§1).
*Resolution:* start 3e on the main line once Step 2's Finish, 3b, 3c and 3d have landed and fast-forwarded. Record
that commit as BASE and re-run §2's line remap there. Tasks 1, 2, 5, 7 and 8 are Qt-free or vtbackend-only and
could start earlier in a worktree, if the owner wants the overlap.

### Non-blocking

**F11 (Task 4).** "`refreshScrollbarMarks` computes `_marksAbove = vtbackend::marksAbove(*inputs, unbox(scrollable),
key.sources)` whenever it reads the inputs." `refreshScrollbarMarks` reads the inputs only when the key changed **and**
`trackPixels > 0` **and** sources are non-empty (`:3836-3840`). With no track published yet, `_marksAbove` stays
`None`. *Resolution:* compute `marksAbove` from `_terminal.scrollbarMarkInputs()` whenever `marksRevision` or
`scrollable` moved, independent of `trackPixels`. Add a case with `trackPixels == 0`.

**F12 (Tasks 12, 14, 16).** Tests assert "not visible" or "shows none" ("a pane 60 px wide shows none",
"`linkTargetShown` false … invisible", "every `trailingBlank` 0 → not visible"). `Toast.qml` is always `visible`, and
only `opacity` follows `shown` (`:31-34`). The same issue is 3c's F14. *Resolution:* assert `shown`/`opacity == 0`, or
give `Toast` `visible: opacity > 0` (Task 15 does exactly this for the bar). If 3c has already changed `Toast`, build
on that.

**F13 (Tasks 12, 14; security).** The link pill shows OSC 8 URIs the application chose. 3c sets
`textFormat: Text.PlainText` on `toastText` only. *Resolution:* set it on the new `toastDetail` too, and add a test
that a target `"<b>x</b>"` renders literally.

**F14 (Task 5; AGENT.md).** "`sizeIndicatorGate(std::chrono::steady_clock::duration uptime, bool sizeIndicatorOnResize)`
(the profile `bool` is a YAML-boundary value, documented)". This is a new pure function, not the YAML boundary.
*Resolution:* convert at the session (`SizeIndicatorSetting { Off = 0, On }`), or let the gate take only the uptime
and have the session test the profile key.

**F15 (Task 6; DI).** "the start read from the same clock at construction". `TerminalSession` already has
`_startTime { steady_clock::now() }` (`:378`), read by `getUptime()` and `startTime()`. *Resolution:* one start time.
Either move `_startTime` onto `TerminalClocks::steady` (checking its readers, which compare against the system
clock), or name the new member and document why there are two.

**F16 (Task 6).** "the three font-size actions and `setFontSize` call it". The actions call `setFontSize`, so every
zoom requests twice. *Resolution:* call only in `setFontSize`, after the size is persisted (both the display and the
displayless branch).

**F17 (Tasks 7, 10).** The tracker's `update(uri, …)` "returns `linkPillText(uri, …)`", while Task 10 passes a detected
link's already-processed `linkPillText(link, …)` through the same `uri` slot, as today's code passes
`hoveredDetectedLinkTooltip()`. That is double processing: a breadcrumb or path is re-parsed as a URI.
*Resolution:* the tracker takes `std::variant<OscLink, DetectedText>`, or the session formats before `update`
and the tracker stores opaque text.

**F18 (Task 10).** The pill is "re-decided in `onHeldModifiersChanged`". `sendMouseMoveEvent` also applies a new chord
(`updateHeldModifiers` inside the lock, `:3393-3395`) without going through `onHeldModifiersChanged`. A ⌘ pressed
while another window had focus would leave the pill stale. *Resolution:* re-decide on `chordChanged` there too, and
add a test.

**F19 (Tasks 7, 10, Review Focus 3, manual checks).** `linkPillVisibility(…, terminal.followModifiers(), …)` ignores
mouse tracking. `htop` turns mouse tracking on, and then `Terminal::hoverPointer()` is `Elsewhere` and a ⌘-click goes
to the application unless the bypass modifier is held (`Terminal.cpp:5838-5841`). The pill would say "⌘-click to
open" when that does not open the link. Review Focus 3 and the htop manual check ("⌘-click still opens it") are
likely wrong. *Resolution:* `Shown` only when `hoverPointer() == Over` (as the pointer hand already does). The hint
spells the bypass chord while tracking is on (reuse `describeLinkGesture`'s bypass text). Fix the two claims (**Q3**).

**F20 (Task 10; threading).** "While `linkTargetShown`, `screenUpdated()` asks for a clearance refresh." `screenUpdated`
runs on the parser thread, and `linkTargetShown` is GUI-thread state. *Resolution:* mirror it into a
`std::atomic<LinkPillVisibility>`, or always post (the atomic flag coalesces) and test `shown` on the GUI side. Add a
`[threading]` case.

**F21 (Tasks 8–10 vs 3c).** 3c's toast avoids "the prompt row" as `cursorRow` only, while 3e avoids the live prompt
span plus the cursor row. *Resolution:* one definition. 3e's `OverlayClearance::avoidRows` is the richer one; record
the divergence and align 3c in 3e's `/simplify` if cheap (**Q5**).

**F22 (Task 14 + 3b; AGENT.md "two bool members").** With 3b, `PaneHeader` carries `findActive` and `linkPillActive`, two
flags over one trailing area, a state machine in booleans. *Resolution:* one `property int trailingOccupant`
(`none`/`find`/`linkPill`) published as names by the provider, chosen by precedence (find first, **F5**).

**F23 (Task 9).** "`ScrollbarThumb` (native `label(1)` at 30 %)". A `Label` source's `alphaPercent` scales the kit's
`LabelAlphas` (88 light / 92 dark, `ChromeTokenObjects.cpp:154-160`), so "label(1) at 30 %" draws at about 26 / 28 %.
*Resolution:* state the source exactly (for example `ChromeColorSource { .kind = Label, .level = 1, .alphaPercent =
33 }` for about 30 %), and pin the resolved alpha in `ChromeTokenGroups_test`.

**F24 (Task 9).** "`[[nodiscard]] constexpr LinkPillPlacement placeLinkPill(…)` … `floor(…)`". `std::floor` is not
reliably `constexpr` in AppleClang's libc++. *Resolution:* `inline` (it is never needed at compile time), or integer
cell math.

**F25 (Task 2).** `ScrollbarVisibilityInputs` uses `config::ScrollBarPosition` from `Config.hpp:103`, which pulls the
whole configuration header into a "pure" decision header in `contour_test`. *Resolution:* move `ScrollBarPosition`
into `ScrollBarMode.hpp` (`Config.hpp` includes it), or accept the include knowingly.

**F26 (Tasks 12, 14).** The canvas gap between target and hint is `s3` (6). `Toast`'s `RowLayout` spacing is `s5`
(`Toast.qml:62`). *Resolution:* a `spacing` property on `Toast`, defaulting to `s5`, which the link pill sets to
`s3`.

**F27 (Task 14).** "`text: session.linkTargetText` (elided in the middle to the placement's width)", while `placement`
reads `implicitWidth`. A `Label` in a `RowLayout` needs `Layout.maximumWidth`/`fillWidth` and `elide:
Text.ElideMiddle`. If elision fed `implicitWidth`, the placement binding would loop. *Resolution:* take the
placement from the unelided implicit width, set the width from `placement.width`, and test for no binding-loop warning
(`QmlMessageCapture`).

**F28 (Tasks 12–14).** `TerminalPane`'s dim overlay is `z: 5` (`TerminalPane.qml:231-236`), above `SessionChrome`
(z 0), and the whole pane has `opacity: session.opacity` (`:94`). The link pill and the scrollbar are therefore dimmed
in an inactive pane and translucent on a translucent profile. The plan raises only the size pill above the dim.
*Resolution:* give the link pill the same `z` as the size pill, and record the profile-opacity inheritance as
accepted (or **Q6**).

**F29 (Tasks 12, 14; Step 2 Task 11 ruling M2).** "No hard shadow on the in-pane pill on the Terminal row — only real
popovers cast one" (Step 2 ledger) applies to the reconnecting pill and is still in Step 2's fix round. The size pill
and the link pill are in-pane pills too. *Resolution:* reuse whatever switch Step 2's fix adds to `Toast`, and assert
it on the Terminal row in both tasks.

**F30 (Task 17).** Manual check: "after `false` exits, it stays visible at the bottom". By R-b the failure's row must
be **above the viewport**. Right after `false` it is on screen, so the bar hides. *Resolution:* "after `false`, print
a page of output: the bar stays at the bottom".

**F31 (all).** Line numbers drifted (§2). Dispatch with §2's remap, never with the plan's numbers.

**F32 (Tasks 2, 5).** "list it in the session sources beside `SearchStatus.hpp`". No CMake list names
`SearchStatus.hpp`. Pure session headers are either unlisted or in `_header_files`
(`src/contour/CMakeLists.txt:243-247`, `session/HyperlinkTooltip.hpp`). *Resolution:* add the new headers to
`_header_files`. Tests go into `contour_test` beside `session/SearchStatus_test.cpp` (`:1209`).

**F33 (Task 3).** "`AppearanceFixtures.hpp` and `QmlChromeStyle.hpp` (name the new field)". Neither spells a
`PlatformAppearance` field. The initialisers that do are in `AppearanceSources_test.cpp:68,:90` and the Cocoa
`current()` at `:61` (the no-accent early return) and `:66`. *Resolution:* stage those instead. The early return must
still report `scrollBars`.

**F34 (Task 11; binding ruling).** The contour-dark label background is the cursor gold `#E8C26E`, a yellow fill,
against the plan's own Global Constraint "yellow means search and nothing else is yellow-filled". The spec says
"cursor colour" (**Q1**).

**F35 (Task 10; spec principle 7).** Task 10 rewrites `sendMouseMoveEvent`'s `Hover` struct, which holds
`bool decidesPointerShape` (`:3389`), and keeps passing `UiHandledHint = false` into
`Terminal::sendMouseMoveEvent(…, bool)`. *Resolution:* turn the local member into an enum in the same change. The
vtbackend signature is not itself touched, so leave it, noted.

**F36 (Task 3).** The Cocoa case compares `current().scrollBars` to the mapping of the live
`NSScroller.preferredScrollerStyle`, which is ambient OS state. That is acceptable, since the accent case does the
same, but the notification half is the real test. Post the notification with no `userInfo`, as AppKit does.

**F37 (index).** The plans index row for 3e (`plans-index.md:91`) names `2026-10-25-native-redesign-step-2.md` as 3e's
plan. It should be `…-step-3e.md`. The index also says "3a to 3e may run in any order", against the ruled
3a → 3b → 3c → 3d → 3e. Correct both at 3e's Finish.

**Flags requested.**
- *Tests spawning real sessions:* none. Session cases must use `makeDisplaylessSession` (MockPty,
  `TerminalSession_test.cpp:124`) or `MockPtySessionFactory`. Name this in the Task 4, 6 and 10 dispatches; Task 6's
  "two sessions" is two displayless ones. QML cases use mocks.
- *Real I/O in unit tests:* none new beyond Task 3's OS read (F36). Task 1's round trip uses the in-memory writer.
- *QML colour/size literals:* none planned. The removed box, tooltip and handle lower `TerminalPane.qml` (2) and
  `SessionChrome.qml` (11) in the size baseline. `"white"`/`"black"` are named colours, which the colour gate does
  not count. They go anyway.
- *bool parameters:* F14 (new pure function). The documented carve-outs are `linkTargetRevealOf(bool)` (boundary),
  `sizeIndicatorSettled(bool)` and `linkTargetShown` (QML), `linkPillActive` (QML; F22) and
  `EditionDefaults::hyperlinkHoverTooltip` (YAML).
- *Step 1c/2 names that changed:* `paneHeader` → `header` (PaneCard id), `headerPresent` means `headerInline` (F4),
  `MotionTokens.style/headerRevealMs`, `EditionDefaults.chromeDensity`, `SchemeSlot::BarInk`, `colorOn`,
  `paneCardContentInset`, `overlayPaneHeader`, and `RoutingMockSession` (F7).
- *Geometry Step 2 changed:* the grid (`TerminalPane`, and so `SessionChrome`) sits `contentInset` =
  `ceil(r·(1−1/√2))` (3 px at macOS r 10) in from a card, below an inline header, and above an ended pane's footer
  (`PaneCard.qml:66-79`). So: (a) the scrollbar's `scrollbarInset` 4 is measured from the grid, which makes 7 px from
  the card edge in a split and 4 in a single pane (**Q4**); (b) the size pill is centred on the grid below the header,
  not on the card (as Step 2's reconnecting pill is); (c) the link pill's s4/s5 insets are from the grid; (d)
  Minimal's overlay header covers cells, so the pill must not treat it as a header (F4); (e) the dim overlay is above
  the chrome (F28); (f) the overlay scrollbar never reaches a card's corner arc, because the inset keeps the grid
  inside it, so it needs no clip.

## 4. Spec asks no task delivers

1. **"Only ever over blank cells"** is not met: the BottomRight and TopRight fallbacks can cover the indicator status
   line (F3), and the pill can cover 3b's find bar (F5).
2. Delivered with ruled deviations, not gaps: `hyperlink_hover_tooltip: false` is the Native2026 default, while
   Classic keeps `true` (D1). The overlay-by-default `ScrollBarMode` sits behind the `system` preference, with
   Classic `always` (D3). The 45 % dim and the new label colours apply to contour-dark and contour-light only (D4).
   Everything else in §3.11 (Toast pill text, the visibility table, the tooltip's removal, hint colours, the three
   test areas) has a task.

## 5. Owner questions (proposed defaults)

1. **Hint labels in contour-dark's gold cursor colour vs "nothing else is yellow-filled".** *Default:* the spec's
   "cursor colour" stands. Hint mode and search are separate modes, and `#E8C26E` is not the search yellow
   `#FFD60A`. Record it as the ruling's one named exception.
2. **Link pill vs the find bar (3b).** *Default:* the find bar owns the header slot and the grid's top right while it
   is open. The pill falls back to the grid with the bar's rows avoided (BottomRight or nowhere).
3. **Pill under mouse tracking (htop, mc).** *Default:* show it only when a click would follow
   (`hoverPointer() == Over`); with tracking on, the hint names the bypass chord ("⇧⌘-click to open"). Correct
   Review Focus 3 and the htop manual check.
4. **Scrollbar inset in a card.** *Default:* measured from the grid edge, as planned (7 px from the card edge in a
   split, 4 in a single pane).
5. **One definition of "the prompt row" (3c's toast vs 3e's pill).** *Default:* 3e's (live prompt span plus cursor
   row) for the pill. 3c's toast keeps `cursorRow` and is aligned in 3e's `/simplify` if that is a one-line change.
6. **Pills in an inactive or translucent pane.** *Default:* the size pill and the link pill sit above the unfocused
   dim. Both inherit the profile's opacity like the rest of the pane, which is accepted.
7. **The pill's width in a header.** *Default:* it takes the area from after the pane name to the trailing edge
   (hiding the working directory too), the same extent as 3b's `findSlot`. The target is middle-elided to fit.
