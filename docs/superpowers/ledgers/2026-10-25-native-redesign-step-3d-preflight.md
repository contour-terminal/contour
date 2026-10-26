# Pre-flight scan — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-3d.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.10 (`:621-641`, binds; every cited
spec range `:28-64`, `:68-126`, `:139-167`, `:187-226` still holds), the plans index and AGENT.md. The plan was written at
`fdb54a64`. This scan checks it against the committed head of `work/native-redesign-step-2`, **ebbb09d6** (main line
`6f71105f` + Step 2's `5d042135`, `159f3fb2`, `ebbb09d6`), read from `git archive`. Still to land before 3d: Step 2 Tasks
7, 12, 13 and its Finish; then 3a, 3b, 3c (order 3a → 3b → 3c → 3d). **Re-check §2's cites at 3d's real BASE.**

Unchanged since `fdb54a64` (cites hold): `CommandPaletteModel.{hpp,cpp}` (`byteOffsetsToUtf16 :36-69`, ranking
`:113-204`, `:125-137`, `:143-201`, reset `:115-118`), `CommandPalette.qml` (dispatch `:57-97`, `highlightedTitle :109-133`,
`onClosed :72-79`, delegate `:209-246`), `Command.hpp :13-25`, `CommandCatalog.hpp :11-22`, `CommandCatalog.cpp`
(`makeCommand :22-34`, `HostCommandSource :122-130`, `TabCommandSource :132-154`), `command/CMakeLists.txt :8-21`,
`FuzzyFilter.hpp :47/:73`, `Actions.hpp:423` (`isParameterized`), `HostUrl.hpp:77` (`toString`),
`CommandPaletteModel_test.cpp:303-318`, `qml-size-literal-baseline.txt:6` (19).

The 117 catalogue names (`actionCatalog()`) match R-b's assignment exactly — no name missing, none unknown, none twice;
`SearchSelection` (3b) is the 118th once 3b lands (F3).

Names from 1c/1d/2 that 3d uses and that exist: `MotionStyle::{Full,CrossFade}`, `motion.{fast,standard,style,full,
crossFade}` (QML), `detail::label/surface/schemeOnBar/separator`, `ChromeColorSource::level`, `SchemeSlot::SelectionText`,
`Secondary`, `Selection`, `Bar`, `radii.{popover,control}` (14/8/12/0, 6/4/8/0 ✓), `elevation.e3`, `type.{body,footnote}`,
`space.s1..s10`, `chromeStyle.borderWidth`, `ChromeSetup{.accessibility}` (`QmlChromeStyle.hpp:28`), `test::StubTabs`
(`CoreFixtures.hpp:122`), `defaultColorPalettes`, `vtbackend::contrastRatio` (`core/Color.hpp:116`). **Renamed:**
`SchemeSlot::BarText` → `BarInk` (F4). **New since the plan:** `ElevationShadow.qml` (Step 2 Task 9, F6), `size.paneFooter`
(no clash with `paletteFooter`), pane colour roles counted "to the end" (F5).

Sessions and I/O: no 3d test spawns a session (Tasks 3/5/6 run over `MockPaletteController` / `MockMainController`; the
re-run `[manager][palette]` cases already use `MockPtySessionFactory`); Tasks 1, 2, 4 are pure. Qt-free targets hold
(`command/CommandPaletteTree.*` in `contour_command`, `window/PalettePlacement.hpp`). No `bool` parameter is introduced;
`sectioned` (bool `Q_PROPERTY`), `SectionStartRole` and `highlightedTitle`'s `selected` go. QML literals: F9 (`kind === 1`).

## 1. Cross-task pairs (shared file or interface)

| Tasks | Producer | Consumer | Finding |
|---|---|---|---|
| 1 → 2 | `CommandCategory`, `CommandCategoryCount`, `Command::{category,argument}`, `actionCategories()` | `buildPaletteTree`, `listPalette` buckets, `PaletteGroups` static_assert | Consistent. **F10** (`iota(0, CommandCategoryCount)` mixes types). **F19** (lookup by name). |
| 1 → 2/3/5/7 | R-d: `HostCommandSource` description = URL; `TabCommandSource` argument | footer description; child titles | **F2** (blocking): the existing host case asserts the old description. |
| 2 → 3 | `PaletteTree`, `buildPaletteTree`, `listPalette`, `PaletteQuery`, `PaletteEntry`, `paletteFooterHints` | `CommandPaletteModel` roles, `enter/back`, `footerHints` | Consistent (`PaletteQuery::recent` is `std::span<std::string const>`, exactly `CommandHistory::recent()`). **F1** (blocking): child counts and order. |
| 2 ↔ 7 | `listPalette` ranking | `DrillInPalette` calls `enter(0)` | **F15**: depends on the ranking; look the row up by id. |
| 3 → 5 | roles `groupTitle`, `kind`, `depth`, `levelTitle`, `anchorRow`, `enter`, `back`, `footerHints` | `CommandPalette.qml` | Consistent. **F9** (magic `1`/`0` ints), **F13** (reset/currentIndex ordering). |
| 3 → 3 (bridge) | QML delegate `required groupTitle/kind` | `[contour][gui][qml][palette]`, `[snapshot]` | Consistent; between Tasks 3 and 5 a `›` row's Enter runs `commandById("ChangeProfile")` → null → nothing (acceptable inside the PR). |
| 3 → `WindowController` | new constructor | `WindowController.cpp:60-93` init list | Consistent: all six sources (incl. default-constructed `_actionCommands`) are declared before `_commandPalette` (`.hpp:1076-1082`), so passing their addresses in the init list is safe. `runCommand` (`.cpp:391`) unchanged. |
| 3 → test doubles | constructor, `refresh(shortcuts)` | `ChromeSnapshotScene::makePaletteModel` (`:355-368`), `MockPaletteController` (`QmlComponents_test.cpp:4816-4864`) | Consistent; the only other `setSources/setShortcuts/sectioned` users are those two plus `QmlComponents_test.cpp:4962`, `:5019`. `KeyboardRouting_test.cpp:289` returns a null palette — unaffected. |
| 4 → 5/6 | tokens, six colour roles, two glyphs, `palettePlacement`, `motion.popoverScale` | `CommandPalette.qml`, `PopupSurface.qml` | Consistent shape. **F4** (blocking, `BarText`), **F5** (blocking, role counts), **F7** (`s8` is 20), **F11**, **F12**. |
| 4 ↔ `ElevationShadow.qml` | Step 2's soft/hard elevation | Task 5's `PopupSurface.elevation` | **F6**: duplicate shadow path that cannot draw the Terminal row's hard shadow. |
| 5 → 6 | the rewritten popup | transitions, `_returnFocus`, `onClosed` | Consistent; both edit `CommandPalette.qml` in sequence. **F8** (Escape is taken by the Popup), **F21** (which popups get focus back). |
| 5 ↔ host | `openPalette` host 800×600, `installChromeStyle(engine)` default row | Task 5/6 cases need 1280×800, MacOS / Terminal / Reduced rows | **F14**. |
| 6 ↔ Main.qml | `MockMainController::runCommand` (`:272`), `showAbout()` (`MainWindowControllerMock.hpp:153`), `aboutDialog` (`Main.qml:410`), `restoreTerminalFocus` (`:457`) | Task 6 Main.qml cases | Consistent; `MainWindowScene::commandPalette` (`MockMainController.hpp:70`) carries the model. |
| 7 ↔ Step 2 Task 12 | `SnapshotScriptRow::density`, `paneEnd`, new scripts | 3d's rows "name every other field" | Consistent by construction (plan says so); not on the branch yet. |
| 3d ↔ 3b | `SearchSelection` (Actions.hpp); `PopupSurface :75/:88` radius hunk; `UiStyle.hpp` glyphs; `UiStyleProvider` `findBarPlacement`; `ChromeColorRole`/`ColorSourceKind` appends; `ChromeTokenGroups_test`/`UiStyleProvider_test` role counts | Task 1, 4, 5 | 3b lands first: Task 1 **must** add `SearchSelection` → Edit (F3); drop the duplicate `PopupSurface` hunk; append after 3b's fields/roles by name; inherit whatever 3b does about F5's counts. No name clash (`FindBarPlacement`/`FindBarInputs` vs `PalettePlacement`/`PaletteInputs`). |
| 3d ↔ 3c | `MockMainController.hpp` (3c: `notices`, Save Layout invokables; 3d: `runCommand`), `MainWindowControllerMock.hpp`, `WindowController.{hpp,cpp}` (3c: `notices`, Save Layout `:749-767`), `ChromeSnapshotPlan/Scene.hpp`, `QmlComponents_test.cpp` (3c rewrites the save-layout cases at `:5277-5395`, between 3d's palette cases), `MainWindowQml_test.cpp`, `Accessibility_qml_test.cpp`, baseline (3c drops `SaveLayoutDialog.qml`, below line 6) | Tasks 3, 5, 6, 7 | Append-only; rebase and re-run. After 3c, "Save Layout…" is a `KitDialog`: R-k's hand-back covers it unchanged. 3d leaves `Main.qml` alone (3c's toast/banners). |
| 3d ↔ 3a | `metainfo.xml`, plans index, `src/contour/CMakeLists.txt` | Task 2/4/8 | Append-only. |
| 3d → 3f | `ActionCategories` coverage test | 3f's six new block actions (`CommandCategory::Blocks`) | Consistent: 3f's plan already expects the test. |

## 2. Per-task self-consistency

| Task | Agrees with itself? | References on the Step 2 branch |
|---|---|---|
| 1 | **No** | All cites exact (see header). `Command` uses designated init only in `makeCommand`. **F2** (blocking), **F3**, **F10**, **F19**. |
| 2 | **No** | `FuzzyFilter` cites exact (byte-wise matching, no length penalty — so `split` ties "Split Vertical" with `split-dev` and table order puts Pane first, as the test expects). `contour_test` list is `CMakeLists.txt:1171-1222` (plan `:1153-1202`), `CommandCatalog_test.cpp` at `:1187`. The `insert_or_assign` precedent `DisplayRendering_test.cpp:893` is a GUI test that *keeps* `main`; the right precedent is `CommandCatalog_test.cpp:240` (whole-map assignment). Arithmetic checked: prefix 17 for "Launch Layout · " and 18 for "Change Profile · " (`·` = 2 bytes); the Hebrew per-byte offsets `prefix+0…+3`. **F1** (blocking). |
| 3 | Mostly | Moved: `WindowController.cpp` ctor `:60-93` (plan `:64-73`), `setSources` `:83-88`, `openCommandPalette :100-111` (plan `:89-101`), `runCommand :391` (plan `:367-388`); `WindowController.hpp :1070-1083` (plan `:934-947`); `ChromeSnapshotScene.hpp makePaletteModel :355-368` (plan `:304-315`); `QmlComponents_test.cpp MockPaletteController :4816-4864` (plan `:3700-3748`), `sectioned()` reads `:4958-4962`, `:5019` (plan `:3842-3846`, `:3903`). The 日本 case: `QString("Change Profile · ").size() + 1` = 18 ✓. **F13**. |
| 4 | **No** | Moved: `ChromeTokenGroups.hpp` `ChromeColorRole :115-153`, count `:199-200`, `ColorSourceKind :157-171`, `SizeTokens :267-306`, `SpaceTokens :309-328` (+ `detail::nativeSpace :558-571`), `TypeTokens :354-367`, `MotionTokens :370-386` (+ `NativeMotion :573-580`), `LayoutTokens :444-455`, rows `MacRow :783`, `FluentRow :857`, `LinuxRow :928`, `TerminalRow :1003` (plan `:336-437`), `resolveChromeTokens :1153-1194` (plan `:469-475`); `UiStyle.hpp` `UiStyleTokens :80-234`, `NativeTokens :348-421`, `TerminalTokens :423-495`; `ChromeTokenObjects.hpp` `ChromeSizeTokens :36`, `ChromeSpaceTokens :154`, `ChromeTypeTokens :285`, `ChromeMotionTokens :332`, `ChromeLayoutTokens :550`, `ChromeColorTokens :644`; source resolution `.cpp:146-193` (plan `:101-121`); `UiStyleProvider.hpp` invokables near `paneHeader :321`. `UiStyleProvider_test.cpp` is in `contour_gui_test` (`CMakeLists.txt:905`), as Step 4 runs it. Placement arithmetic re-done: all four `PalettePlacement_test` expectations hold. **F4**, **F5** (blocking); **F7**, **F11**, **F12**. |
| 5 | Mostly | Moved: palette host `QmlComponents_test.cpp:4863-4940` (plan `:3750-3820`), palette cases `:4943-5160` and `:5396-5490` (plan `:3827-4400`). `PopupSurface.qml` shadow `:53-99` (plan `:46-88`; 3b's `:75`, `:88` exact). Baseline line 6 ✓. **F6**, **F8**, **F9**, **F14**, **F16**. |
| 6 | Mostly | Moved: `MockMainController.hpp:272` (plan `:163`), `MainWindowControllerMock.hpp:153` (plan `:96`). `CommandPalette.qml onClosed :72-79` ✓. Popup has `scale`, `aboutToShow`. **F8**, **F21**. |
| 7 | Mostly | Moved: `ChromeSnapshotPlan.hpp` `SnapshotScript :35-57` (plan `:35-49`), `ScriptAction :74-85` (plan `:66-74`), `SnapshotScriptRow :137-153`, `SnapshotScripts :156`, the count static_asserts `:439-444`; `ChromeSnapshotScene.hpp show() :202` (plan `:161-185`), `makePaletteModel :355-368`, members `:610-620` (plan `:487`); `ChromeSnapshot_test.cpp` `case Palette :114-119` (in plan's `:60-123`). The scene `Config` already holds `main`, so with `work`, `presentation` the level lists three — matches Task 8's "three profiles". **F15**. |
| 8 | Mostly | **F17** (diff base, index row), **F20** (copy casing). `ctest -L lint`'s QML gates exist. |

## 3. Findings

**F1 — blocking (Tasks 2, 3, 5).** "`ProfileCommandSource`… over a `config::Config` whose maps are filled with
`insert_or_assign` (as `DisplayRendering_test.cpp:893` does)"; then "two children whose `argument`s are `dark`, `light`",
"the level lists 200 rows", "`rowCount() == 2`", "two rows `dark` and `light`"; and R-c "each parent's children in source
order".
*Why it fails:* `Config::profiles` defaults to `{ "main" }` (`Config.hpp:1856-1858`), so `insert_or_assign` yields 3 / 201
children and every count above is off by one (and "main" sorts between `dim` and `light` in `aFilterInsideALevel…`'s
unfiltered branch). `profiles` and `layouts` are `std::unordered_map` (`:1856`, `:1859`), so "source order" is hash order and
any assertion on `parents[i].children` order is flaky.
*Resolution:* tests assign the whole map (`config.profiles.value() = { … }`, as `CommandCatalog_test.cpp:240`); Task 7's
scene keeps `main` on purpose (three profiles). `buildPaletteTree` orders each parent's children by `argument` (one
deterministic rule, which is also R-e's level order), and `parameterisedActionsCollapse` asserts the sorted list.

**F2 — blocking (Task 1).** R-d: "`HostCommandSource` sets the child's `description` to `config::toString(entry.url)`";
Step 4: "PASS, the existing catalogue cases unchanged".
*Why it fails:* `CommandCatalog_test.cpp:424` asserts `home->description == actions::documentation::AttachToHost`.
*Resolution:* Task 1 updates that assertion to `config::toString(HostEntry{}.url)` (or gives the fixture URLs) in the same
commit, and says so.

**F3 — non-blocking (Task 1).** "(and 3b's SearchSelection, if it has landed)" / "Whichever lands second gives
`SearchSelection` its row".
*Why:* the order is now fixed (3b before 3d); 3b Task 8 adds it, so the coverage test fails without it.
*Resolution:* Task 1 adds `{ "SearchSelection", Edit }` unconditionally; the table has 118 rows; check at BASE whether 3b
added any other catalogue action.

**F4 — blocking (Task 4).** R-h: "Terminal row: `Bar`, `BarText`, `schemeOnBar(Secondary)`, …".
*Why it fails:* Step 2 Task 3's fix (`7a63a765`) renamed `SchemeSlot::BarText` to `BarInk` (one bar-ink chain, ruled).
*Resolution:* `PaletteText` on the Terminal row is `schemeColor(SchemeSlot::BarInk)`; `paletteTextReads` then holds by
construction on every scheme, not only contour-dark/light.

**F5 — blocking (Task 4).** "`ChromeColorRole` gains `PaletteSurface` … after the last role; the count derives from
`PaletteSeparator`."
*Why it fails:* two existing checks count the pane roles as "everything from `PaneGapFill` to the end":
`ChromeTokenGroups_test.cpp:1121` (`rows.size() == ChromeColorRoleCount - PaneGapFill`) and `UiStyleProvider_test.cpp:1223`
(`ChromePaneRoleNames`' property count). Appending six roles breaks both (3b's find-bar roles hit it first; 3b's pre-flight
did not flag it).
*Resolution:* bound the pane range by a named constant beside the enum (`PaneColorRoleEnd =
PaneHeaderActionHover`, or whatever 3b introduced at BASE) and use it in both checks; the palette roles get no entry in
`ChromePaneRoleNames`.

**F6 — non-blocking (Task 5).** "`PopupSurface.qml:46-88` — `property QtObject elevation: null`; when set, the shadow image
reads `elevation.blur`/`elevation.offsetY`"; "Terminal row: … the hard one-cell shadow from `e3`".
*Why:* Step 2 Task 9 already extracted `ElevationShadow.qml` (soft nine-patch where blur > 0, a hard offset copy
otherwise; used by `Toast.qml:53`, `PaneCard.qml:54`). PopupSurface's own `BorderImage` has no hard path, so the plan as
written cannot draw the Terminal row's shadow it promises, and adds a second soft-shadow implementation. Step 2's ruling I3
("popovers keep theirs") is exactly the hard path.
*Resolution:* `PopupSurface` gains `property var elevation: null`; when set it instantiates `ElevationShadow { level:
root.elevation; radius: root.radius; anchors.fill: parent; z: -1 }` and hides its own `BorderImage`; unset, nothing changes
for the other eight popups. Task 5's Terminal-row case asserts `elevationHardShadow` visible.

**F7 — non-blocking (Task 4, R-j, Task 5).** "the popup's side margin to the window is `space.s8` (16; 1 cell on the Terminal
row)"; "`marginX`/`marginY` = `space.s8`".
*Why:* the native scale is `{ 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 }` (`ChromeTokenGroups.hpp:562`): `s8` = 20, `s7` = 16
(Terminal: `s7` = `s8` = 1). The tests do not catch it (1280 × 800 never clamps).
*Resolution:* use `space.s7`; add one `UiStyleProvider_test` case that clamps (e.g. `palettePlacement(600, 400)` → width
568).

**F8 — non-blocking, high value (Task 5, Review Focus 2).** "Keys (on the field, R-f): … Escape → `model.depth > 0 ?
model.back() : root.close()`".
*Why:* the palette keeps Popup's default `closePolicy` (includes `CloseOnEscape`). Qt Quick's popup item grabs Escape as a
window shortcut, which wins over the focused `TextField`'s `Keys.onEscapePressed` (a TextField does not accept the
ShortcutOverride for Escape) — so Escape in a level would close the palette instead of going back. Today the difference is
invisible because both paths close.
*Resolution:* `closePolicy: Popup.CloseOnPressOutside`, and Escape handled only by the field (also via
`Keys.onShortcutOverride` if needed). Task 5's "Escape in a level goes back one level, then closes" is the test; write it
first and watch it fail for this reason, not only for the missing `back()`.

**F9 — non-blocking (Tasks 3, 5).** "`kind === 1 ? model.enter(currentIndex) : acceptCurrent()`", "`depth > 0`".
*Why:* bare enum ints in QML; Step 2's review (M6) ruled named values in QML for `EndReason`/`FooterRestart`, and
`ChromePaneRoleNames`/`ChromeBadgeKindNames` are the precedent.
*Resolution:* `CommandPaletteModel` exposes `Q_PROPERTY(int drillInKind … CONSTANT)` and `Q_PROPERTY(int topDepth …
CONSTANT)` (or one small names object); QML compares against them.

**F10 — non-blocking (Task 1).** "`for every CommandCategory in iota(0, CommandCategoryCount)`".
*Why:* `0` is `int`, the count `size_t`; the plan's own rule ("`iota` takes two bounds of one type") — it does not compile.
*Resolution:* `std::views::iota(std::size_t { 0 }, CommandCategoryCount)`.

**F11 — non-blocking (Task 4).** "`UiStyleTokens` gains `std::string_view drillInGlyph; std::string_view
palettePromptGlyph;`"; Files lists the four rows only.
*Why:* Step 2's review asked for `{}` on new `UiStyleTokens` members (global constraint too). The native `SpaceTokens` and
`MotionTokens` are built by `detail::nativeSpace(...)` (`:558`) and `detail::NativeMotion` (`:573`), not in the rows: the
new `paletteInset` and `popoverScalePercent = 97` go there (designated, every field named); the Terminal row sets its own.
`paletteWidth` must scale by the horizontal unit, the heights by the vertical (`ChromeSizeTokens`).
*Resolution:* `{}` initialisers; name the two helpers in Files; state the unit per new size token.

**F12 — non-blocking (Task 4).** "`[[nodiscard]] constexpr PalettePlacement placePalette(...)`" with `floor`.
*Why:* `std::floor` is not `constexpr` in libc++ (AppleClang) or MSVC's STL; CI builds both.
*Resolution:* `inline` (the tests are runtime), or floor via `static_cast<long long>` on the non-negative quotient.

**F13 — non-blocking (Tasks 3, 5).** "On `levelChanged`: the field's text takes `model.filter`, `list.currentIndex =
model.anchorRow`".
*Why:* `enter`/`back` reset the model, and the field's `onTextChanged` sets `currentIndex = 0`; if the handler writes
`currentIndex` before the text (or `filterChanged` triggers another reset), the anchor is lost — Review Focus 2's "the
parent's row selected".
*Resolution:* in the handler set the text first, then `currentIndex` and `positionViewAtIndex`; `setFilter` on an equal
string stays a no-op (it is, `:105`). Task 5's Escape case asserts `currentIndex` on "Change Profile" (it does).

**F14 — non-blocking (Tasks 5, 6).** "host window 1280 × 800 under the MacOS row (`QmlChromeStyle`)"; "run under
`CONTOUR_TEST_CONTROLS_STYLE=ContourTui` with the Terminal row"; "a `QmlChromeStyle` resolved with
`MotionPreference::Reduced`".
*Why:* `openPalette` hard-codes `width: 800; height: 600` and `installChromeStyle(engine)` (default setup). Also the
existing "The command palette lists commands with their shortcut and documentation" (`:4943`) asserts per-row
`commandPaletteDescription`, which Task 5 removes, and is not in the "keep their meaning" list.
*Resolution:* `openPalette(engine, controller, ChromeSetup, QSize)`; rewrite `:4943` onto the footer.

**F15 — non-blocking (Task 7).** "`DrillInPalette` … calls the model's `enter(0)` (row 0 is "Change Profile" by Task 2's
group ordering)".
*Resolution:* find the row whose `commandId` is `ChangeProfile` and `enter` it; a ranking tweak must not break a snapshot.

**F16 — non-blocking (Task 5).** "the visible `commandPaletteGroupHeader` items read "Recently used", "Pane", … in
`PaletteGroups` order".
*Why:* at max 420 tall only the first groups are instantiated. *Resolution:* assert the visible headers are a prefix of
`["Recently used"] + PaletteGroups` titles (non-empty groups only), or check the model's `groupTitle` sequence.

**F17 — non-blocking (Task 8).** "`git diff <the plan commit>..HEAD -- src scripts`"; Step 6 marks the index.
*Why:* the plan commit predates 1c, 1d, 2, 3a–3c (3a F13, 3b F21). The index's Plan column for 3d reads
`2026-10-25-native-redesign-step-2.md`. *Resolution:* record 3d's BASE in the ledger and diff from it; fix the row to `-3d.md`.

**F18 — non-blocking (all).** Line references drifted (table §2). `Shortcut.hpp:20-45` is now `:30-56` (`ShortcutStyle :30`,
`platformShortcutStyle :53`). Re-map at BASE.

**F19 — non-blocking (Task 1).** "`commandCategory` — the row whose `name` is `actions::catalogEntry(action).name`".
*Why:* a linear name scan per command (~130 × 118 per open) where the table is already "in catalogue order", which is
variant order (`catalogEntry` indexes by `action.index()`).
*Resolution:* index by `action.index()` and add one test that row *i*'s name equals `actionCatalog()[i].name` (this also
gives the coverage test its "no duplicate" for free).

**F20 — non-blocking (Task 8).** "Attach to Host › lists the saved hosts", release note "Change Profile, Launch Layout,
Attach to Host and Switch to Tab".
*Why:* parent titles are `splitCamelCase(name)`: "Attach To Host", "Switch To Tab". *Resolution:* the copy uses the
catalogue's casing (or Q3).

**F21 — non-blocking (Task 6, R-k).** "if that item lies inside the window's `Overlay.overlay` … it gets the focus back".
*Why:* the find bar (`SearchBar.qml`) and tab-colour flyout are Popups too, so a palette opened from a focused find bar hands
focus back to it even after running, e.g., Split Vertical. Probably right, but unstated. *Resolution:* Q2; add the find-bar
case to Task 6 whichever way it is ruled.

## 4. Spec asks no task delivers

None. Every §3.10 item is delivered: the `CommandCategory` column in `command/CommandCatalog.hpp` (Task 1), grouping
(Tasks 2, 5), `›` drill-in over the existing source rows (Tasks 2, 3, 5), 640 wide / 32-high rows / footer description
(Tasks 4, 5), `ShortcutStyle` shortcut text and unchanged fuzzy matching (Tasks 2, 3), and the three named tests
(`commandCategory.*`, `paletteTree.*`, `aFilterSurfacesMatchingChildren`). Two deviations are already ruled (ledger D1: four
groups beyond the seven; spec gaps: Fluent/ContourLinux rows 36/38 high per the kit).

## 5. Owner questions (proposed defaults)

1. **Window margin `s7` (16) or `s8` (20)?** (F7) *Default: `s7`, the 16 the plan meant.*
2. **Focus hand-back target:** any Overlay item (find bar, flyouts, dialogs) or only modal dialogs? (F21) *Default: any
   Overlay item, as the plan writes it, with a find-bar test.*
3. **Parent row casing:** catalogue "Attach To Host ›" or canvas "Attach to Host ›"? (F20) *Default: the catalogue's
   `splitCamelCase` everywhere; fix the copy.*
4. **Terminal-row palette shadow:** the hard one-cell `e3` copy via `ElevationShadow` (F6), consistent with Step 2's I3 "popovers
   keep theirs"? *Default: yes.*
