# Pre-flight scan — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-3b.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.8 (`:551-575`, binds), with the
plans index's rules and AGENT.md. The plan was written at 30cdc3f0. This scan checks it against the committed head of
`work/native-redesign-step-2`, **159f3fb2**, read from `git archive` (the `../contour-step2` worktree is dirty: uncommitted
edits to `src/contour/CMakeLists.txt`, `PaneCard.qml`, `TerminalPane.qml`, `TerminalSession.hpp`,
`TerminalSession_test.cpp`, `QmlMocks.hpp`, `QmlComponents_test.cpp`, `Accessibility_qml_test.cpp`,
`MockMainController.hpp`, `WindowController.cpp` — Step 2 Task 11 in flight). That head holds 1c, 1d Tasks 1–6 and Step 2
Tasks 1–6 and 8–10; Step 2 Tasks 7, 11, 12, 13 are open (ledger `progress.md`, last entry "Task 9: complete").
**Re-check §2's cites at 3b's real BASE once the main line fast-forwards.**

Unchanged since 30cdc3f0 (cites hold): `core/Search.hpp`, `grid/Grid.{hpp,cpp}` (`logicalLineHead :1183`,
`logicalLineEnd :1208`, `stableIdGeneration/stableLineIdOf/lineOffsetOf/stableRangeFloor :766-787`,
`textMatchesAt/matchedTextEnd .hpp:942-953 .cpp:2503-2557`), `Screen.{hpp,cpp}` search block (`.hpp:865-918`,
`.cpp:8715-8841`), `RenderBufferBuilder.{hpp,cpp}` (`:264-275`, `:366-370`, `.cpp:706-779`), `links/ScanText.hpp:35`,
`session/SearchStatus.hpp` (all cites exact), `MenuBarTable.cpp:211-214`, `Actions.hpp` (`:60`, `:97`, `:185`, `:222`,
`:456`, `:545`, `:782`, `:830`), the existing `[screen]` regression cases (`Screen_test.cpp:3365-3800`) and
`Screen_multipage_test.cpp:903`. Moved: see §2.

Names from 1c/1d/2 that 3b uses all exist: `SchemeSlot::SelectionText`, `Secondary`, `Foreground`, `BarInk` (1c's
rename of `BarText`; 3b never names `BarText`), `detail::label(level)`, `StatusColorRole::Running`,
`ColorSourceKind::StatusMark` (1c's text/mark split), `UiStyleProvider::paneHeader(int,int,int,int,qreal,qreal)`
(`UiStyleProvider.hpp:322`), `paneHeaderName/Cwd/Actions` object names, `ChromeSetup`, `QmlMessageCapture`,
`nullAppearanceServices`, `MockPtySessionFactory`. `TerminalPane.qml` *is* a `ContourTerminal`, so
`displayItem.rowBand(...)` reaches `TerminalDisplay` as planned.

## 1. Cross-task pairs (shared file or interface)

| Tasks | Producer | Consumer | Finding |
|---|---|---|---|
| 1 → 2, 3, 4 | `SearchOptions`, `SearchPatternError`, `SearchChange`, `SearchQuery::compile`, `RegexEngine.hpp` | `matchesIn`, `Terminal::Search`, the walk | Consistent. `RegexEngine.hpp` in `core/` meets AGENT.md's rule (named by `links/` and `screen/`, includes only std). |
| 1 ↔ `LinkPatterns.cpp` | `anyRegexSourceCharacter<Char>`, `hasRegexBackReference<Char>`, `RegexEngineFlags` | `hasNonAsciiInBracket`, `hasBackReference` (`LinkPatterns.cpp:112-158`), `RegexFlags` (`:51-62`) | Consistent; cites drifted (`:105-151` → `:112-158`). |
| 2 → 3 | `searchSegmentAt`, `matchesIn` | `RenderBufferBuilder::prepareSearchHighlights` (per frame) | **F15**: a `TooComplex` on the render path cannot set `problem` and recurs every frame. |
| 2 → 4 | segments, `matchesIn` | `SearchTallyWalk::step` | **F14**: "≤ 500 rows per step" and "whole segments" contradict on narrow panes (a segment is `MaxScanCells / columns` rows: 1 843 at 20 columns). |
| 3 → 7, 8 | `setNewSearchTerm`/`setSearchOptions` → `std::expected`, `Search::options`, `focusedSearchMatch` | `TerminalSession` find-bar block | Consistent. Task 3's compile-only cites moved (`:2544/:2594/:2728/:6063` → `:2767/:2817/:2951-2952/:6281`). |
| 3 ↔ vi | `Terminal::searchReverse(text, pos)` unchanged signature | `ViCommands::updateSearchTerm` (`:226-230`) for `*`/`#` | **F17**: a vi word search goes through `SearchOrigin::Typed`, so the Regex toggle turns `*` on `a.b` into a regex. Task 3's "ViCommands.cpp `:228`, `:1099`: the new returns" needs no edit there. |
| 4 → 7 | `beginSearchTally`, `continueSearchTally`, `probeSearchReverse`, `nearestAtOrBefore`, `lastMatch` | `startSearchTallyLocked`, `runSearchTallyStep`, `setSearchPattern` | Interface consistent. **F8** (blocking): the bounded probe loses the "no match anywhere" answer the existing tests read. **F10/F11**: restart policy. |
| 5 ↔ 3 | `searchHighlight{,Focused}` on contour-dark/-light | the builder's `makeRGBColorPair` mix (`RenderBufferBuilder.cpp:52-62`) | Consistent: the alpha is mixed over the cell's own background, so the 35/85 % composites the test computes are what is painted. Contrast checked by hand: 4.7:1 (dark), 11.5:1 (light), 8.7:1 (focused, dark). |
| 6 → 7 | `SearchOutcome::{Pending,Invalid}`, `MatchPresence::Unknown`, `describeSearchProgress/Error`, toggle table | session | **F1** (blocking): Task 6's enum edits break `TerminalSession.cpp` before Task 7 adapts it. |
| 6 → 10 | `SearchToggleRows` object names | QML Repeater, the QML tests | Consistent. **F23**: glyph/tooltip/accessible name live outside the row. |
| 7 → 8 | `stepSearch`, `focusAdjacentSearchMatch`, `toggleSearchOption` | Find Next/Previous wrap; Use Selection for Find | Consistent. **F2** (blocking): the existing `[actions]` case asserts the old no-pattern behaviour. |
| 7 → 10 | `searchToggles`, `toggleSearchOption`, `searchFocusedRows` | `SearchBar.qml`, `MockSession` | Consistent. **F20**: between Tasks 7 and 10 `SearchBar.qml` reads removed properties. |
| 7 ↔ `onScrollOffsetChanged` | — | `searchFocusedRows` refresh | **F4** (blocking): that callback runs under the held state lock, possibly on the parser thread. |
| 8 ↔ `Config` | `macFindChords()` appended to `makeDefaultInputMappings` | existing users' `contour.yml` | **F12**: the tree's rule is that a new default chord goes in `builtinFallback*Mappings` (`Config.hpp:219-227`), or it never reaches an existing config. |
| 9 → 10, 11 | tokens, `findBarPlacement`, `rowBand`, glyphs | `SearchBar.qml`, `PaneHeader.qml` | Consistent. **F13** (Terminal-row colour sources), **F16** (token shape), **F22** (duplicate glyph fields). |
| 9/10 → 11 | `PaneHeaderPresence`, placement | `PaneCard` → `TerminalPane` → `SessionChrome` → `SearchBar` | **F5** (blocking): the native bar's card-space `y` is 33, not 30 (Step 2's corner inset); `headerPresent` must be "inline header", not "visible". |
| 10 ↔ existing `SearchBar.qml` | — | Popup `margins: chromeStyle.shadowMargin` (`:53`) | **F7** (blocking): 12 px margins clamp the popup off the 412/8 the test expects. |
| 12 ↔ Step 2 Task 12 | Step 2's `SnapshotScriptRow::density`, `paneEnd`, new scripts | 3b's rows "name `.density`, `.paneEnd`" | **F6** (blocking): not on the branch yet. |
| 3b ↔ 3a | — | `Terminal.{hpp,cpp}` (3a: constructor, status line, `_gitHead`, `homeDirectory()`; 3b: `Search`, search block), `vtbackend/CMakeLists.txt` (both append in `screen/`), `Config.hpp`/`Config_test.cpp` (3a: `Native2026IndicatorTemplate`; 3b: input mappings or fallbacks), `QmlComponents_test.cpp`, `metainfo.xml`, plans index | Different functions; append-only. No interface dependency either way: 3a's `{SearchPrompt}` reads only `search().pattern` (`StatusLineBuilder.cpp:493-508`), which 3b keeps. 3a's `MockTerm(PageSize, FileSystem const&)` is additive; 3b's tests use the existing constructors. Whichever lands second rebases and re-runs its filters. Same Finish-diff defect as 3a's F13 (**F21**). |

## 2. Per-task self-consistency

| Task | Agrees with itself? | References on the Step 2 branch |
|---|---|---|
| 1 | Yes | `Search.hpp` `CaseComparison :88-92` (plan `:85-110` ok); CMake `core/Search.hpp :87`, headers `:60-130`, sources `:178-216`, `vtbackend_test :284-392` (`Search_test.cpp :359`). `LinkPatterns.cpp` flags `:51-62`, scanner `:112-158`. `SearchCaseSensitivity::Smart` is 0 (`Search_test.cpp:59`), so `SearchOptions{}` is all-zero. |
| 2 | Mostly | Grid cites exact. Test arithmetic checked: wraps, whole word (0/18 vs four), wide/combining columns, `segmentsCutALongLineAtFixedRows` (36 864 / 200 = 184). **F18** (`isBlank()` is storage, not content); **F24** (no decomposed-on-screen case). |
| 3 | Mostly | `Terminal.hpp` `Search :297-313` ✓, `search() :3404` (plan `:3395-3398`), search block `:3647-3692` (`setNewSearchTerm :3670`, `setSearchCaseSensitivity :3675`); `Terminal.cpp :8501-8630` (plan `:8486-8615`). Unannotated `setNewSearchTerm` callers: `ViCommands_test.cpp:497`, `Terminal_selection_test.cpp:173`, `:214` ✓, `TerminalSession_test.cpp:4077` (plan `:4001`); the `(void)` callers (`Screen_test:3538`, `Screen_multipage_test:910/930`, `Terminal_test:1668`, `Terminal_input_test:797`, `TerminalSession_test:10140`) compile unchanged. No test uses the `CaseComparison` overloads that go. No other module reads `Search` (vthost, vtworkspace, vtrasterizer: none). **F17**. |
| 4 | Mostly | `DefaultSearchMatchLimit` `Search.hpp:63`; `displayedPageIndex()` `Terminal.hpp:2569`; `tallySearchMatches :3689`. libc++'s `error_complexity` (`_LIBCPP_REGEX_COMPLEXITY_FACTOR`) makes the catastrophic case throw on the first row, as the plan assumes. **F14** (budget wording vs `aStepReadsWholeSegmentsWithinItsBudget`). |
| 5 | Yes | `ContourScheme :48-57`, `ContourDark :59`, `ContourLight :82`, `applyContourScheme :115-138` (plan `:44-133`); `contrastRatio` `core/Color.hpp:116`; tag `[ColorPalette]` ✓. |
| 6 | **No** | Cites exact. **F1** (blocking): the switch at `TerminalSession.cpp:2911` and `MatchPresence::None` at `:2787`. `SearchStatus_test.cpp` is in `contour_test` at `CMakeLists.txt:1208` (plan `:1191`). |
| 7 | **No** | Moved: `.hpp:251-371` (plan `:238-339`), members `:1940-1964` (plan `:1779-1787`); `.cpp` find-bar block `:2672-2982` (plan `:2457-2758`), debounce `:430-439` (plan `:415-424`), output re-arm `:906-924` (not cited), `onScrollOffsetChanged :2983`, profile `:6281`; tests `:4085`, `:4218`, `:5953` (plan `:4009`, `:4142`, `:5877`). **F3**, **F4**, **F8** (blocking); **F10**, **F11**, **F19**, **F20**. |
| 8 | **No** | `Config.hpp` `makeDefaultInputMappings :1287-1722`, F3 rows `:1328-1343`, Ctrl+Shift+F `:1657-1661`, `SplitVertical :1497` (plan `:1490-1493`); `ConfigDocumentation.hpp:1388` (plan `:1378`); `TerminalSession.hpp :1251-1252`, `:1286`; `.cpp :4661-4710` (plan `:4443-4489`), `stepSearch :2843`. `MenuBarTable_test` `[menubar]` ✓ (in `contour_test`). **F2** (blocking); **F9**, **F12**. |
| 9 | Mostly | `ChromeTokenGroups.hpp` moved: `ChromeColorRole :115-153`, count `:199-200`, `SizeTokens :267-306`, `SpaceTokens :309-328`, `RadiusTokens :331-351`, `LayoutTokens :444-455`, native colours `:707-764`, Terminal row colours `:1088-1120`. `UiStyle.hpp` glyph struct `:205-235`, rows `:402-412` / `:482-489` (plan `:218-225`, `:356-362`, `:431-437`). `gridMetrics() TerminalDisplay.hpp:553` ✓. `contour_test` list `:1170-1240`. **F13**, **F16**, **F22**. |
| 10 | **No** | `SearchBar.qml` logic now `:86-198`; `PopupSurface.qml :75`, `:88` ✓; `SessionChrome.qml` instance `:363-367`, `onSearchBarRequested :479-482`; baseline line is `:11` (plan `:13`); `MockSession` `QmlMocks.hpp:25` (plan `:22`), `StubContourTerminal :397` (plan `:324`); `MockSearchSession` `QmlComponents_test.cpp:6742` (plan `:5576-5670`), cases `:6907`, `:7100`. **F7** (blocking). |
| 11 | **No** | Object names exist. `PaneCard.qml` places the terminal at `header.height + contentInset` (`:67-72`). **F5**, **F6** (blocking). |
| 12 | **No** | `SnapshotScript :35-56`, `ScriptAction :74-83`, `SnapshotScriptRow :137-153`: **no `density` or `paneEnd` field yet** (Step 2 Task 12). **F6** (blocking). |
| 13 | Mostly | **F21** (diff base), **F25** (bench hook), plans index row (3b → `-2.md`, shifted: 3a's F17). |

## 3. Findings

**F1 — blocking (Task 6).** "`SearchOutcome` gains `Pending` … and `Invalid` …, after `Matched`." "`MatchPresence`'s
`None` becomes `Unknown = 0`." Step 2 builds and runs only `contour_test`.
*Why it fails:* `TerminalSession::renderSearchSummary` switches over `SearchOutcome` without a default
(`TerminalSession.cpp:2911-2916`): `-Wswitch` under `-Werror` breaks `contour_gui_test` and `contour`. `:2787` names
`MatchPresence::None`, which no longer exists. Task 6's commit therefore leaves the app unbuildable until Task 7.
*Resolution:* Task 6 also adds the two `case`s (`Pending` → `tr("…")`, `Invalid` → `tr("Invalid pattern")`) and adapts
`:2787`, builds `contour_gui_test`, and stages `TerminalSession.cpp`. Or fold Task 6 into Task 7's commit. See also F8,
which changes what `MatchPresence` should hold.

**F2 — blocking (Task 8).** R-h: "With no pattern they open the find bar"; "`FocusNextSearchMatch` returns true".
*Why it fails:* `TerminalSession_test.cpp:4059` ("search-match focus actions move the vi cursor onto a found match",
`[contour][session][actions]`, run by Task 8 Step 4) asserts `CHECK_FALSE((*session)(actions::FocusNextSearchMatch {}))`
with no search. Task 8 does not list it.
*Resolution:* Task 8 rewrites that line to the new contract (returns true, `searchBarRequested` posted, no PTY bytes),
which is what its own "Find Next with no search opens the find bar" case pins.

**F3 — blocking (Task 7).** "Removed: `searchCaseGlyph`, `searchCaseTooltip`, `searchCasePinned`,
`cycleSearchCaseSensitivity()`." "The existing find-bar cases … their assertions stand."
*Why it fails:* the section "cycling the case policy re-runs the search" (`TerminalSession_test.cpp:4131-4160`) calls
all four. The selection-report table has the row `.site = "cycleSearchCaseSensitivity"` (`:10635-10641`). Neither is
listed, so the test file does not compile.
*Resolution:* port the section to `searchToggles[0]` (`glyph`, `checked`) and `toggleSearchOption(0)`, with the same
three-step assertions. Rename the table row to `toggleSearchOption(Case)`.

**F4 — blocking (Task 7).** "`searchFocusedRows` … recomputed by `refreshSearchStatusLocked` and
`onScrollOffsetChanged`."
*Why it fails:* `onScrollOffsetChanged` is called from `Terminal::onViewportChanged` (`Terminal.cpp:456`) **with the
state mutex held**, and for output that scrolls a scrolled-back view **on the parser thread**
(`TerminalSession.cpp:2985-2990` says so and posts `clearHovers` for that reason). Computing `focusedSearchMatch()`
there under a fresh `scoped_lock` self-deadlocks (a non-recursive mutex). Computing it without the lock writes a
GUI-thread `Q_PROPERTY` from the parser thread.
*Resolution:* post, as `clearHovers` is posted: `platform::postToObject(this, [this] { refreshSearchFocusedRows(); })`,
where the GUI-thread function takes the lock, reads `focusedSearchMatch()` and emits on change. Coalesce with an atomic
flag, as `_searchTallyPostPending` does. Add a `[threading]` case that scrolls from a second thread with the bar open.

**F5 — blocking (Task 11; Task 9's comment).** "the bar's parent is the terminal item and its `y` in the card's
coordinates is `size.paneHeader + space.findBarInset` (30)"; `PaneCard.qml → TerminalPane { … headerPresent:
paneHeader.visible ? 1 : 0 }`.
*Why it fails:* (a) Step 2's ruling I2 places the terminal at `header.height + card.contentInset`
(`PaneCard.qml:67-72`). `contentInset` is `ceil(r·(1−1/√2))`, which is 3 at macOS's `radii.paneCard` 10, so the bar's
card-space `y` is 22 + 3 + 8 = 33. The assertion fails. (b) Under Minimal the header is an *overlay* while ⌥ is held
(`policy.placement === "overlay"`). `visible ? 1 : 0` would hand the Terminal row's bar to the header row and back on
every ⌥ press.
*Resolution:* assert `size.paneHeader + card.contentInset + space.findBarInset`, read from the card, and fix
`findBar.underTheHeaderIsTheTerminalsTop`'s comment. Pass `headerPresent: card.headerInline ? 1 : 0`, and add a
Minimal case: holding ⌥ leaves the bar where it was.

**F6 — blocking (Tasks 11, 12; sequencing).** "Depends on Steps 4a, 1c, 1d and 2: every existing row names their
fields (`SnapshotSurface`, `.drift`, `.frame`, `.mode`, `.density`, `.paneEnd`)."
*Why it fails:* on 159f3fb2, `SnapshotScriptRow` (`ChromeSnapshotPlan.hpp:137-153`) has no `density` or `paneEnd`, and
the `SplitMinimal`/`PaneExited`/`SingleTabMinimal` scripts do not exist. Those come from Step 2 Task 12, which is open.
Step 2 Task 11 has uncommitted edits to `PaneCard.qml`, `TerminalPane.qml`, `QmlMocks.hpp` and
`QmlComponents_test.cpp`, which are 3b Task 10/11's files.
*Resolution:* start 3b only after Step 2 Tasks 7, 11, 12 and 13 land and the main line fast-forwards. Record that
commit as 3b's BASE, and re-run this scan's §2 there. Tasks 1–6 are vtbackend- or header-only and could start on the
branch head, if the owner wants the overlap.

**F7 — blocking (Task 10).** "`x`, `y`, `width` from [placement]"; test: "`x == 412`, `y == 8`, `width == 380`".
*Why it fails:* `SearchBar.qml:53` keeps `margins: chromeStyle.shadowMargin`, and `shadowMarginUnits` is 12 on the native
rows (`UiStyle.hpp:365`). A Qt Quick `Popup` keeps itself inside its parent minus its margins, so it lands at x ≤ 800 −
12 − 380 = 408 and y ≥ 12. The plan keeps only `:83-188` and does not mention `:49-53`.
*Resolution:* replace `:49-53` with the placement binding and `margins: 0`, or `space.findBarInset`; the placement
already keeps the bar inside the pane. Assert that the background's shadow still fits (PopupSurface draws outside its
surface by `_margin`).

**F8 — blocking (Task 7; Task 6's `MatchPresence`).** R-e: "If that finds nothing, the bar shows `Pending`"; "The
existing find-bar cases … drain the walk where they read a count; their assertions stand."
*Why it fails:* the section "a pattern that matches nothing says so" (`TerminalSession_test.cpp:4123-4129`) asserts
`CHECK_FALSE(searchHasMatches())` and `"No results"` **right after** `setSearchPattern("haystack")`, with no
`refreshSearchStatus()`. Under R-e that is `Pending` (`searchHasMatches()` true, summary `…`), and no walk is running
to drain (it starts only from `refreshSearchStatusLocked`). It is also a UX regression: on a scrollback under 500 rows,
the probe has read the whole grid and *knows* there is no match, yet the field stays un-red until a posted walk ends.
*Resolution:* make the probe tri-state: `Found`, `NoneInGrid` (the budget covered every row) and `Undecided`. Keep
`MatchPresence { None = 0, Some, Unknown }`, so `None` still means "proved absent" and goes straight to `NoMatch`.
Only `Unknown` gives `Pending`. Then the existing section passes as written, and the "far above the cursor" case still
covers `Pending`.

**F9 — non-blocking (Task 8).** "Run: `out/appleclang-debug/src/contour/contour_test "[config][input-mapping],…"`".
*Why:* `Config_test.cpp` is compiled into `contour_gui_test` (`src/contour/CMakeLists.txt:882`), not `contour_test`
(Step 2's F6 too). The new "find chords follow the platform" case never runs locally.
*Resolution:* run `[config][input-mapping]` from `contour_gui_test` in Task 8 Step 4 and in Task 13 Step 2.

**F10 — non-blocking (Task 7).** "`refreshSearchStatusLocked` … `startSearchTallyLocked()`: `beginSearchTally(cursor)`,
`++_searchWalkTicket`, and one posted `runSearchTallyStep`".
*Why:* output re-arms the 250 ms timer while the bar is open (`TerminalSession.cpp:906-924`), and the timer calls
`refreshSearchStatus()` (`:435-439`). Each firing starts a new walk and stales the running one. A walk of more than
250 ms (a 200 000-line scrollback is 400 posted steps) under `while :; do date; done` never finishes. That fails the
owner check "the count settles, the terminal keeps scrolling". `SearchTallyWalk` was built to survive streaming output,
so restarting it throws that property away.
*Resolution:* split "re-tally" from "restart". An output-driven refresh does nothing while a walk is `Running`, and
starts a new walk only after `Finished`. Pattern, option, page or `stableIdGeneration` changes restart at once. Pin it:
a 2 000-row walk with `refreshSearchStatus()` between every step still ends with the right total.

**F11 — non-blocking (Tasks 7, 8).** `stepSearch` ends in `refreshSearchStatus()` (`:2867`), which under Task 7 restarts
the walk and posts its first step.
*Why:* every Enter, ⌘G or F3 resets the count to `…` for at least one event-loop turn, even on a ten-line scrollback:
"2 of 5" → "…" → "3 of 5", where today it is instant. The canvas and the owner check ask for "steady digits".
*Resolution:* run the first step inline in `startSearchTallyLocked` (the lock is already held, and the step is
bounded), and publish its result. A scrollback within one step settles synchronously, as today.

**F12 — non-blocking (Task 8).** "`inline std::array<CharInputMapping, 4> macFindChords()` in `Config.hpp`, appended by
`makeDefaultInputMappings` for every edition".
*Why:* `Config.hpp:219-227` documents why new default chords are *fallbacks*: a written `contour.yml` replaces
`input_mapping` wholesale, so a new default never reaches an existing config. The Classic edition (owner decision 4) is
exactly the existing users. `chordIndex(Config)` already indexes fallbacks (`Shortcut.cpp:125-131`), so the menu still
shows ⌘F/⌘G.
*Resolution:* add the four rows to `builtinFallbackCharMappings()` (`Config.cpp:193`) under `#ifdef __APPLE__` (or an
`.enabled` predicate), with the same empty modes. The test then asserts them via
`applyBuiltinFallback(config, U'F', Super, …)`. Add a `menuBar` assertion that "Find…" shows `⌘F` on macOS.

**F13 — non-blocking (Task 9).** "Terminal row: `Scheme Selection`, `Scheme SelectionText`, `Scheme Secondary`,
`Scheme Foreground`"; "Native rows: `Status`/`Running` at 22 %, `Status`/`Running` at 100 %".
*Why:* 1c's ruling is one ink on the bar. `PaneHeaderTextDimmed` is `schemeOnBar(Secondary)`, because "ANSI 8 on a
light scheme's dark bar fails 4.5:1" (`ChromeTokenGroups.hpp:1100-1112`). A toggle glyph in raw `Secondary` and a count
in raw `Foreground` on `Bar` reintroduce both problems. On the native rows, a 22 % fill is a mark, so after 1c's
text/mark split it should be `StatusMark`. Accent text on its own 22 % wash is near the line on light: `#007AFF` on
white is about 4.0:1 before the wash.
*Resolution:* Terminal: `FindToggleText = schemeOnBar(Secondary)`, `FindCountText = schemeColor(BarInk)`. Native:
`FindToggleOnFill = statusMarkSource(Running)` at 22 %. Add a contrast test of `FindToggleOnText` over the
`FindToggleOnFill`-on-surface composite on contour-dark and contour-light, and on the Terminal row over every built-in
scheme, as the pane-header roles have.

**F14 — non-blocking (Task 4, Global Constraints).** "a step reads at most `SearchWalkStepLines` (500) rows of whole
segments" against "reads whole segments until at least @p budget rows are read … (a segment is never split across
steps)".
*Why:* a segment is `max(1, MaxScanCells / columns)` rows: 184 at 200 columns, 1 843 at 20, 36 864 at 1. A step can read
`budget + segmentRows − 1` rows, so the "at most" does not hold, and a narrow pane's step reads far more than 500
rows. `aStepReadsWholeSegmentsWithinItsBudget` ("every step's `rowsLastStep() <= 500`") passes only because that grid's
lines are hard-broken.
*Resolution:* state the budget in cells (`500 × columns`, which equals the spec's "500 lines" at any width that can hold
them) and keep "never split a segment". Reword the constraint to "at least one segment, then whole segments until the
budget", and make the test's bound `budget + segmentRows − 1`.

**F15 — non-blocking (Task 3).** "`prepareSearchHighlights` … its matches from `search.query->matchesIn` …, an error
painting nothing".
*Why:* the builder holds a `const` terminal on the render path, so a `TooComplex` cannot set `problem` or drop the query
there (R-f). Every frame re-runs the catastrophic regex until a walk or a step trips it, and with the bar closed (F3
stepping) no walk runs at all.
*Resolution:* cache the failure per `query` pointer in the builder, so one throw per query paints nothing thereafter. Or
let the builder report it once through the terminal's event listener, which posts. Add a `[renderbuffer]` case: two
renders of a `TooComplex` query run the matcher once.

**F16 — non-blocking (Task 9).** "`size.findButton` (previous, next, close; square) … Terminal 2"; "`space.findBarInset`
(from the pane's right and top edge) … 1"; `FindBarInputs { … double inset … }`.
*Why:* a 2-cell "square" button on a 1-row bar is 2 rows tall. One `inset` used for both x and y is 1 cell horizontally
but 1 row vertically, which are different pixel extents on the Terminal row, and the units differ per axis there.
*Resolution:* `findButtonWidth`/`findButtonHeight` (2×1 cells on Terminal, 22×22 elsewhere), and `insetX`/`insetY` in
`FindBarInputs`, each scaled by its own unit.

**F17 — non-blocking (Task 3, R-g).** "`SearchOrigin::DoubleClick` compiles `SearchSyntax::Literal` whatever the toggles
say".
*Why:* vi `*`/`#` search the word under the cursor through `ViCommands::updateSearchTerm` → `Terminal::searchReverse(text,
pos)` → `setNewSearchTerm(…, Typed)` (`Terminal.cpp:8530-8536`). With the regex toggle left on, `*` over `a.b` or
`f(x)` becomes a regex or an `InvalidSyntax`.
*Resolution:* add `SearchOrigin::WordUnderCursor` (or a `SearchSyntax` override parameter on that overload), compiled
literal like `DoubleClick`, and a `[vi]` case. Drop "`ViCommands.cpp` (`:228`, `:1099`: the new returns)", which needs
no edit.

**F18 — non-blocking (Task 2, R-a).** "a blank row (`Line::isBlank()`) one `\n` that no pattern can match across".
*Why:* `Line::isBlank()` means *unmaterialised storage* (`LineSoA::codepoints.empty()`, `Line.hpp:251`,
`LineSoA.hpp:237`), not "shows nothing". A materialised all-empty continuation row reads as N spaces (empty cell →
space), and an unmaterialised one reads as `\n`. Whether `a.b` matches then depends on an invisible storage state.
Task 2's "a blank continuation row" case gives no recipe for building one.
*Resolution:* decide on content. A row whose every cell is empty (or `isBlank()`) is one `\n`. State the recipe in the
test (e.g. `EL` on a wrapped row's continuation), and add the materialised twin with the same expectation.

**F19 — non-blocking (Task 7 test; flag: time in unit tests).** "`std::async` taking `std::scoped_lock { terminal() }` is
ready within 1 s".
*Why:* a wall-clock wait in a unit test (flaky under ASan/TSan). It is also vacuous: `runSearchTallyStep()` has
returned on the test thread, so nothing holds the lock.
*Resolution:* drop it. The `writeToScreen` between two steps already proves the lock is free. For the "released
between steps" property, assert on the step's structure: one `runSearchTallyStep()` returns with `searchTallyRunning()`
true and a step posted (count the posts with a test seam, or `QCoreApplication::sendPostedEvents` once).

**F20 — non-blocking (Tasks 7→10).** Task 7 removes `searchCaseGlyph`/`Tooltip`/`Pinned`. `SearchBar.qml:78-80` still binds
them until Task 10.
*Why:* with a real session the bindings read `undefined` into typed `string`/`bool` properties. That is a QML warning,
and the run-wide gate in `test_main.cpp` fails any suite that instantiates `SessionChrome` with a real session, at
Task 7's commit.
*Resolution:* keep the three read-only properties, derived from `searchToggles[Case]`, until Task 10 removes their last
reader. Or move `SearchBar.qml`'s three bindings into Task 7.

**F21 — non-blocking (Task 13).** "`git diff <the plan commit>..HEAD -- src scripts`".
*Why:* 3b builds on 1c, 1d and Step 2, so this diff sweeps all of their sources into `/simplify` (3a's F13).
*Resolution:* record 3b's BASE in the ledger and diff from it.

**F22 — non-blocking (Task 9).** "`toggleOpenGlyph` / `toggleCloseGlyph` (`""` / `"["`, `"]"`)".
*Why:* `chipOpenGlyph`/`chipCloseGlyph` already exist with exactly those values (`UiStyle.hpp:219-220`, `:405-406`,
`:482-483`). Two fields for one bracket pair is duplication, and AGENT.md's data-driven rule applies.
*Resolution:* reuse the chip glyphs, or rename them once to `bracketOpenGlyph`/`bracketCloseGlyph` for both.

**F23 — non-blocking (Tasks 6, 7).** `SearchToggleRow { toggle, objectName }`. The glyph lives in `describeSearchToggle`'s
switch, the tooltip and accessible name in `TerminalSession`.
*Why:* a fourth toggle edits the enum, the row, two switches and two session tables.
*Resolution:* put `glyph` (for Regex and WholeWord) and an accessible-name key (`QT_TR_NOOP` source text) in the row.
Keep the switch only for Case's three-state glyph.

**F24 — non-blocking (Task 2).** Review Focus 4 is "`café` typed precomposed … against a decomposed `e` + U+0301
**printed** by `ls`". `wideAndCombiningCharacters` writes `U"日本語 café"` precomposed on screen and varies only the
pattern.
*Resolution:* add the row with the screen text written as `café`, which is the APFS case.

**F25 — non-blocking (Task 13 Step 6).** "Callgrind on `bench-headless` with a pattern set, before and after".
*Why:* `bench-headless.cpp` has no search hook and no task adds one.
*Resolution:* add a `Workloads` row (or a `--search <pattern>` flag) in Task 4, or drop the claim and report
`[searchwalk]` timings instead.

**F26 — non-blocking (all).** Stale line numbers are listed in §2. Locate by name; every cited symbol exists. The plans
index's 3b row still points at `-2.md` (3a's F17); fix it when Step 3b is marked done.

### Flag checks

- **Real sessions:** none. Session cases use `makeDisplaylessSession` over `MockPty` or `TestApp`'s
  `MockPtySessionFactory`. QML cases use `MockSession`/`StubContourTerminal`. Snapshots use the scene's mock session.
- **Real filesystem / time I/O in unit tests:** the 1 s `std::async` wait (F19). No filesystem. The walk injects
  nothing time-based, which is right: it is driven by posts, not a clock.
- **QML colour/size literals:** none planned. `"transparent"` passes `check-qml-colors.py` (hex and numeric `Qt.rgba`
  only), as in `PaneHeader.qml`. The magnifier `Shape` must take its extents from `size.findIcon`, since `SearchBar.qml`'s
  baseline goes to 0. `font.features: { "tnum": 1 }` is not a size property.
- **`bool` parameters:** none added. Surviving `bool`s are question returns (`searchTallyRunning`), defaulted
  `operator==`, and QML carve-outs (`checked`, `findActive`). `focusAdjacentSearchMatch` returns success as `bool`; it
  inherits the action-handler contract, so document that at the declaration.
- **Step 1c / 2 names that changed:** none used under an old name. F13 is about *which* 1c source to use, not a rename.

## 4. What §3.8 asks that no task delivers

Nothing. Both enums (Task 1), the `std::expected` invalid regex shown as "Invalid pattern" (Tasks 1, 3, 7, 10), the
chunked 500-line walk that releases the lock and yields (Tasks 4, 7), the progressive count (Task 7), one compile per
query (Tasks 1, 3), `window/FindBarPlacement.hpp` with under-header / top-right / bottom-right placement (Tasks 9–11),
"2 of 5" (unchanged rendering), and the three test bullets (Tasks 2/3, 4, 9) are all covered. F14 is the only place
where "500 lines per step" is not literally what the walk does, and F10 decides whether "the count updates as the walk
progresses" survives streaming output.

## 5. Owner questions left open (proposed defaults)

1. **Instant "No results" (F8).** When the keystroke probe has read the whole grid and found nothing, should the bar say
   so at once? *Default: yes. `MatchPresence::None` means "proved absent", and only an exhausted budget gives
   `Pending`.*
2. **Find chords as fallbacks (F12).** *Default: macOS ⌘F/⌘G/⇧⌘G/⌘E go in `builtinFallbackCharMappings`, so existing
   Classic configs get them, and an explicit user binding still wins.*
3. **Step budget unit (F14).** *Default: 500 × columns cells per step, never splitting a segment, so a narrow pane does
   not read thousands of rows per step.*
4. **Re-walk policy under streaming output (F10).** *Default: output never restarts a running walk; a finished walk
   re-runs on the 250 ms debounce. Pattern, option, page and reflow changes restart at once.*
5. **vi `*`/`#` under the regex toggle (F17).** *Default: always literal, like a double-click.*
6. **Terminal-row button and inset extents (F16).** *Default: buttons 2 cells × 1 row; the inset 1 cell and 1 row, as
   two values.*
7. **Starting before Step 2 finishes (F6).** *Default: no. 3b starts at the fast-forwarded main line, and Tasks 1–6
   may start early only if the owner wants the overlap.*
