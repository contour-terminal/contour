# Pre-flight scan — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-3a.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.7 (binds), with the plans index's
rules and AGENT.md. The plan was written at 30cdc3f0. This scan checks it against the committed head of
`work/native-redesign-step-2`, 159f3fb2, which holds 1c, 1d (Tasks 1–6) and Step 2 (Tasks 1–6 and 8–10). Step 2's
Tasks 7, 11, 12 and 13 are still open, and `../contour-step2` has an uncommitted `src/contour/CMakeLists.txt`.
**Re-check the cites below at 3a's real BASE once the main line fast-forwards.**

How it was checked: every cite was read from `git archive work/native-redesign-step-2`.
`StatusLineBuilder.{hpp,cpp}`, `StatusLineBuilder_test.cpp`, `Selector.*`, `MockTerm.hpp`, `CommandBlock.hpp`,
`ColorPalette.*` and the four status-line QML files are unchanged since 30cdc3f0, so their line numbers still hold.
These files did move: `Terminal.{hpp,cpp}`, `Config.hpp`, `Config_test.cpp`, `src/contour/CMakeLists.txt`,
`SettingsController.cpp`/`_test.cpp` and `QmlComponents_test.cpp` (see §3). 3a uses no 1c, 1d or Step 2 identifier:
no `BarText`/`BarInk`, `StatusColor*`, `BarPresence`, density or pane-header names. So no renamed name breaks it.

## 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → 4, 7 | `ItemColor`, `makeTextColor/makeBackgroundColor(std::optional<ItemColor> const&)`, `resolveItemColor` | 4: the success/danger/muted runs; 7: the chip's RGB mode pair | Consistent. One type covers both roles and RGB. Task 7 wraps `RGBColorPair` members as `ItemColor{RGBColor}`. |
| 1 → 9 | `ColorRoleNames`, `parseItemColor`/`formatItemColor`; bridge `writeColorPair`/`writeStyles`/`optionalColor` on `ItemColor` | `indicatorColorRoles()`, the QML `roleNames` | Consistent. `optionalColor` (Tabs' active pair) must go through `parseItemColor` too, which Task 1 says. **F9**: Task 1's bridge test uses a template that names `{Cwd}`/`{Git}` before they exist. |
| 2 → 3 | `Terminal::homeDirectory()`; `elideMiddle` | Task 3 places `gitBranch()` "beside `homeDirectory()`" | The order is consistent. **F7**: `{Cwd}` abbreviates `~` for foreign paths, while Step 2's pane header does not. |
| 2 → 4, 9 | the generic `MaxWidth` (`requires t.maxWidth`), written only off its default | `LastCommand::maxWidth {32}`; the bridge's generic `maxWidth` key | Consistent. Write the generic `MaxWidth` *after* the type-specific block, so `Context` keeps `Verbosity,Separator,MaxWidth` (`StatusLineBuilder.cpp:962-972`). Between Tasks 2 and 9, the editor drops `{Cwd:MaxWidth=…}` on save. No test catches that, and it is acceptable within the PR. |
| 3 → 10 | `MockTerm(PageSize, FileSystem const&)`; `refreshGitHead()` only when `containsItem<Git>` | `IndicatorTemplate_test` builds a plain `MockTerm` with the Native2026 template, which contains `{Git}` | **F6**: the template cases then stat the real filesystem. |
| 4 ↔ existing `vtbackend` | `formatCommandDuration(steady_clock::duration)` | — | **F2** (blocking): the name and signature already exist in `vtbackend` (`shell/CommandBlock.hpp:368`), with another format. |
| 5 → 6 | `groupDigits(uint64_t)` | `{Scroll}`'s visit and test | Consistent; the order is right. |
| 7 → 8 | `InputMode` with `hiddenIn`/`look`; `showsModeChip`; `indicatorStatusLineColorsFor` | measuring items with `Styling::Disabled` (a hidden chip is zero width) | Consistent. Zero-width items "never count" in collapse. |
| 7 → 9 | `inputModeFrom`/`inputModeName`, `InputModeLook` | the bridge's `hide`/`chip` keys | Consistent. `chip` is a QVariantMap boolean, the documented QML carve-out. |
| 7 → 10 | `Chip`/`Hide=` written "after the styles" | R-j's template `{InputMode:Bold,Chip,Hide=Insert,Left= ,Right= }` | **F3** (blocking): that text is not the canonical order (`appendStyles` writes `Left`/`Right` as styles). |
| 8 ↔ existing `Terminal_test` | `collapseStatusLine` + R-h's tie order | `theMiddleSegmentLeavesTheLeftOneWhole` row 4 (`Terminal_test.cpp:4398-4416`) | **F1** (blocking): the plan says "unedited", but this row changes. |
| 1–8 → 10 | everything | `drawsTheCanvasStates` and `eachPaneDrawsItsOwnLine` | Names agree. **F5**: on contour-dark the Insert and Inactive pairs are identical. **F2**: the canvas durations. |
| 1, 9 → 11 | the step's filters | Task 11 Step 2's union | The union covers every tag (`[statusline]` includes `[collapse]`, `[selection]` and the rest). **F13**: the `/simplify` and clang-tidy base. |
| 3a ↔ Step 2 (open tasks) | — | Step 2 Task 11 (`QmlComponents_test.cpp`, `ExitedFooter.qml`), Task 12 (`ChromeSnapshotPlan.hpp` adds scripts) | Append-only overlaps. Task 11 Step 3's PNG count is derived from `SnapshotScripts.size()`, so Step 2's new scripts change the number but not the rule. **F18**. |

## 2. Per-task self-consistency

| Task | Agrees with itself? | References on the Step 2 branch |
|---|---|---|
| 1 | Mostly | `Styles :25-32`, `Tabs :125-130`, `CellFlagNames :171-185`, `tryParseColorAttribute :69-76`, `makeTextColor/makeBackgroundColor :266-281`, `addColor :824-831` and `StatusLineBuilder_test.cpp:589-590` are all exact. The only other users of the colour fields are `StatusLineBuilder_test.cpp:204-205` (optional == optional, still compiles) and the bridge. Bridge cites have moved: `writeColorPair :2619`, `writeStyles :2644`, `optionalColor :2664-2671`, `hexToRgb`'s other caller `:2490`, the indicator tests `SettingsController_test.cpp:1548-1736`. The contrast claim holds. Recomputed against the idle bar, the non-muted roles' lowest values are 4.94 on dark (danger) and 4.58 on light (success). Muted is 4.14 on dark and **4.11** on light (the plan says 4.14), still ≥ 3. **F9**. |
| 2 | Yes | `makeItemOfType`'s Context branch `:158-169`, `elide :657-716` (a `static` member of `VTSerializer`; the move to a free function is fine), the `MaxWidth` write `:970-972`. `currentWorkingDirectory()` is `Terminal.hpp:3518`, `resolveWorkingDirectory` `:3086`, `_homeDirectory` `:4394`. `abbreviateHomePath` (`core/WorkingDirectory.hpp:109`) and `sanitizeCommandLine` (`shell/CommandLineSanitizer.hpp:46`; it turns bad UTF-8 and C0 into U+FFFD) exist. The elision examples recompute: `"/…/d59"`, and `"…aaa"` for one 300-byte component. **F7**. |
| 3 | Mostly | The `FileSystem` API matches (`isDirectory`, `isRegularFile`, `fileSize`, `readFile`, `lastWriteTime`). `InMemoryFileSystem::lastWriteTime` returns `now()` at `InMemoryFileSystem.cpp:784` (R-d is right). `core::pathFromUtf8` is in `<core/Utf8Path.hpp>`. `_fileSystem` is `Terminal.hpp:4397`, the constructor `Terminal.cpp:280`, `endCommand` `:6631-6636`, `setCurrentWorkingDirectory` `:6659-6674`, the mirror and take-over returns `:1641-1669`. OSC 7 with an empty host resolves Local (`WorkingDirectory.cpp:35-38`), so `file:///r/src` reaches `OpenLocally`. vtbackend CMake anchors `:108`, `:204`, `:362` are exact, and `vtbackend_test` already uses `InMemoryFileSystem`. **F14** (MockTerm), **F15**. |
| 4 | **No** | **F2** (blocking). `lastFinished()` (`CommandBlock.hpp:564`), `current()` (`:558`), `CommandBlockEnd::Reported` (`:51`), `exitCode`/`duration`/`commandLine` (`:145-152`), `typeAtPrompt` (`testing/TestHelpers.hpp:199`) and `ManualClock::advance` all exist. R-c's states check out: `current()` is non-null while Prompting *or* Running, so the visit must test `state == Running`, not only non-null. The Tabs SGR pattern is at `:763-776`. |
| 5 | Yes | `Selection` `:54-117`, `state()` `:98`, `ranges()` `:104`, `SelectionHelper::pageSize/cellEmpty` `:24-27`, `TestSelectionHelper` `Selector_test.cpp:15`, tag `[selector]`. |
| 6 | Mostly | `visit(HistoryLineCount)` `:408-430`; `scrollingDisabled`/`scrollableLineCount`/`scrollUp` exist. **F10**: the fixture it reuses holds 100 lines of history, and it is a DECPCCM case. |
| 7 | **No** | **F3** (blocking): `"{InputMode:Bold,Chip,Hide=Insert,Left= ,Right= }"` is not a fixed point. **F4**: the chip test is vacuous on MockTerm's palette, and `backgroundAt` is file-local to `Screen_context_test.cpp:1003`. `InputMode :57`, `visit(InputMode) :440-443`, `indicatorStatusLineColors()` `Terminal.cpp:1566-1581` (private, declared `Terminal.hpp:4123`); `Settings::focused` defaults to true. **F16**. |
| 8 | **No** | **F1** (blocking), **F8**. The placement table rows recompute against `Terminal.cpp:1676-1713`, and `widthOf` is `:1678-1681`. There are 24 priorities, one per `Item` alternative after Tasks 2–6 (19 + 5). The test names `anOverflowingIndicatorShowsItsHead :4292`, `theRightSegmentEndsInTheLastColumn :4321` and `aSegmentIsMeasuredAsTheGridLaysItOut :4339` exist. |
| 9 | Mostly | `StatusLineItemDialog.qml` `edit :90-120`, `apply :123-160`, the colour fields `:303-316`; `ColorSlotField.qml` validation `:49-56`, swatch `:59-62`; `StatusLinePreviewItem.qml:48`; the dialog in `StatusLineIndicatorEditor.qml:304-311`. The neighbouring QML case moved to `QmlComponents_test.cpp:2043` (tag `[contour][gui][qml][extensions]`). Bridge `parseIndicatorSegment :2674-2745`, `serializeIndicatorSegment :2747-2813`. **F11**. |
| 10 | Mostly | `Native2026IndicatorTemplate` is `Config.hpp:660-670`, `Native2026Expectation`'s strings `Config_test.cpp:4080-4082`, `GuiStateStore_test.cpp` `src/contour/CMakeLists.txt:1177`. `contour_test` links `vtbackend` and `core::testing`, so `MockTerm` builds there. No other copy of the interim template exists in `src/` or `docs/`. **F5**, **F6**. The claim that "the indicator bridge round-trips a segment unchanged" covers the new default does not hold: that test's `BasicConfig` profile is not the Native2026 template. Add an explicit Native2026 bridge case (with F3's canonical text). |
| 11 | Mostly | The snapshot formula `SnapshotScripts.size() × 4 × 2 × 2` is still derived (`ChromeSnapshotPlan.hpp:156`, `:465-492`). The lint gates `check_qml_colors`/`check_qml_sizes` exist (root `CMakeLists.txt:278-285`). **F13**, **F17**. |

## 3. Findings

**F1 — blocking (Task 8).** R-h: "drops while some non-empty segment would not be placed … Order: lowest `Priority`
first; among equals, the right segment's last item, then the middle's, then the left's"; Step 4: "the existing
`[terminal]` indicator cases pass unedited".
*Why it fails:* `Terminal.indicatorStatusLine.theMiddleSegmentLeavesTheLeftOneWhole` (`Terminal_test.cpp:4398`) has the
row `{ "0123456789", "XYZW", "RIGHT", "0123456789     RIGHT" }`. All three items are literal `Text`, so all are Normal
priority. The middle is not placed (left 10 > (20−4)/2 = 8). R-h first drops the right segment's `RIGHT`, which was
placed, and then `XYZW`. The line becomes `"0123456789          "`, so the test fails. The plan's own
`dropsByPriorityRightToLeft` example needlessly loses the right High item for the same reason.
*Resolution:* break ties among equal priorities by taking an item of a segment that would **not** be placed first
(right, then middle, then left), and fall back to right-to-left only after that. Every row of the five existing layout
cases then keeps its expected line; this was checked by hand. Write the tie rule into R-h and Review Focus 3.

**F2 — blocking (Task 4).** "Produces: `[[nodiscard]] std::string formatCommandDuration(std::chrono::steady_clock::duration
duration);` — under 1 s `"<ms> ms"`; under 60 s `"<s>.<tenths> s"` …".
*Why it fails:* `vtbackend::formatCommandDuration` with exactly that signature already exists
(`shell/CommandBlock.hpp:368`, `.cpp:158-173`, tests `CommandBlock_test.cpp:1822-1850`). Its doc says it is the format
"every surface prints it": `850ms`, `12s`, `3m 05s`, `1h 02m`. If the new function is declared in `vtbackend`, the
build has a duplicate definition. If it is declared in `StatusLineDefinitions` (R-a), `StatusLineBuilder_test.cpp`
(`using namespace vtbackend;`) and `VTSerializer`'s unqualified call still bind to the old function. Then
`formatCommandDuration.table` fails (`"4ms" ≠ "4 ms"`), and so do `lastCommandSaysHowItWent` and Task 10's
`"12.4 s"`.
*Resolution (default):* drop the new function and reuse the existing one, so the status line, the gutter tooltip, the
finish notification and the sticky chip agree. Canvas texts become `✗ exit 8 ctest -R gui · 12s` and `✓ make · 4ms`,
and Tasks 4 and 10 change their expected strings. If the owner wants the canvas's tenths: rename the new function
`formatStatusLineDuration` and give its test a qualified call (owner question 1).

**F3 — blocking (Tasks 7 and 10).** Task 7: "`Hide=<mode>` and the bare flag `Chip`; written after the styles as `Chip` then
`Hide=<name>`"; its test says `"{InputMode:Bold,Chip,Hide=Insert,Left= ,Right= }"` is a canonical fixed point. R-j's
template uses the same text.
*Why it fails:* `appendStyles` (`StatusLineBuilder.cpp:887-901`) writes flags, `Color`, `BackgroundColor`, then `Left`
and `Right`. Writing `Chip`/`Hide` after the styles therefore yields `{InputMode:Bold,Left= ,Right= ,Chip,Hide=Insert}`,
so Task 7's assertion fails. The default template is also not canonical: one Apply in the chip editor rewrites
`contour.yml`, which breaks Review Focus 4's "round-trips byte for byte".
*Resolution:* keep "after the styles" (the same place `Context`'s `Verbosity`/`MaxWidth` go). Write R-j's left segment,
Task 7's fixed-point string, Task 10's `Native2026Expectation` and the docs as
`{InputMode:Bold,Left= ,Right= ,Chip,Hide=Insert}`. Add a Task 10 assertion that each Native2026 segment satisfies
`serializeStatusLineSegment(parseStatusLineSegment(s)) == s`.

**F4 — non-blocking (Task 7).** "`aChipKeepsTheLineQuiet` … columns 0–7 have background
`indicatorStatusLineNormalMode.background` and column 9 has `indicatorStatusLineInsertMode.background`."
*Why:* MockTerm's default palette gives all four indicator pairs `0x0270c0` (`core/ColorPalette.hpp:303-306`). Every
assertion in this case, including "without `Chip` column 9 takes the Normal pair", passes even if the chip is never
drawn. `backgroundAt` is in an anonymous namespace in `Screen_context_test.cpp:1003`.
*Resolution:* load `contour-dark` (Normal = page on blue, ≠ Insert), or set four distinct pairs first. Copy the
`backgroundAt` helper, or move it to `testing/TestHelpers.hpp`.

**F5 — non-blocking (Task 10, Review Focus 5).** "B's column 0 background is `indicatorStatusLineInactive.background` …
`sendFocusInEvent()` → B's column 0 background is the Insert pair's."
*Why:* `applyContourScheme` sets `indicatorStatusLineInsertMode = indicatorStatusLineInactive`
(`ColorPalette.cpp:127`). On contour-dark the two assertions cannot tell focused from unfocused.
*Resolution:* give B's palette a distinct Inactive pair before the focus change, or assert on a scheme where they
differ. See also owner question 2 (the chip in an unfocused pane).

**F6 — non-blocking (Tasks 3 and 10).** "each case parses the Native2026 template into a 100-column `MockTerm`".
*Why:* the template contains `{Git}`, and a plain `MockTerm` gives the terminal `NativeFileSystem::instance()`
(`MockTerm.hpp:488`). The cases then walk and stat real directories up from `/srv/app`, `/srv/a` and `/srv/b`. That is
real I/O in a unit test, which breaks the DI rule, and the result depends on the host (a runner image with `/.git`
would show a branch).
*Resolution:* build these cases with Task 3's `MockTerm { size, files }` and an empty `InMemoryFileSystem`. Do the
same for every later test whose template holds `{Git}`.

**F7 — non-blocking (Task 2).** "`sanitizeCommandLine(abbreviateHomePath(path, homeDirectory()), …)`", applied to every
resolved directory, foreign ones included.
*Why:* Step 2's pane header computes the same text in `TerminalSession::shownWorkingDirectory`
(`session/TerminalSession.cpp:2394-2406`), and abbreviates **only when `locality == Local`**. An ssh path that
happens to equal the local `$HOME` would read `~/x` in the status line but `/home/bob/x` in the pane header above it.
*Resolution:* abbreviate only Local paths. Better, lift that rule into `vtbackend` (`core/WorkingDirectory`) as one
function, and have both the pane header and `{Cwd}` call it. Add the test row
`file://elsewhere.example<home>/x` → the full path.

**F8 — non-blocking (Task 8).** R-h drops "while some non-empty segment would not be placed"; `placeStatusLineSegments`
places only the middle and right.
*Why:* a left segment wider than the line never triggers a drop, so it is cut instead. In a narrow pane the Native2026
left segment (chip, Context, Cwd ≤ 40, Git) is cut inside the path rather than losing `{Git}` (Normal) first, which
contradicts Review Focus 3 and the canvas ("segments drop right to left, by priority"). The plan's
`dropsByPriorityRightToLeft` ("until it fits") also needs the left to count, and it does not state the expected
fates.
*Resolution:* treat "left wider than `columns`" as not placed. The one-item rule is unchanged, so
`anOverflowingIndicatorShowsItsHead` still passes. Write the expected fates into the plan. With F1 and F8 applied, the
example `[L: E8, H15, N24][M: H22][R: H17, Low6]` in 40 columns gives Shown, Shown, Dropped | Dropped | Shown, Dropped.

**F9 — non-blocking (Task 1).** The `[settings]` case uses "`parseIndicatorSegment` of the Review Focus 4 template".
*Why:* that template names `{Cwd}` and `{Git}`, which arrive in Tasks 2 and 3. At Task 1 they come back as literal
`Text` rows with no colour, so the case cannot pass.
*Resolution:* use the substitute template that the vtbackend case already uses
(`{VTType:Color=#C0C030}{Clock:Color=muted}{Title:Color=#000000,BackgroundColor=accent}`).

**F10 — non-blocking (Task 6).** "the `historyItemCountsThePageOnDisplay` fixture (`:537-551`) with 1 300 lines of history".
*Why:* that fixture has `MaxHistoryLineCount` 100, and it switches pages with DECPCCM, so its cursor writes to a page
with no history.
*Resolution:* use a fresh `MockTerm` with at least 1 300 lines of history and no page switch.

**F11 — non-blocking (Task 9).** "the foreground `colorSlotField`'s text is `"accent"`"; "`StatusLinePreviewItem` uses the
item's colour only when it starts with `#`" (`:48`).
*Why:* the dialog holds up to four `ColorSlotField`s, all with the objectName `colorSlotField`, so `findChild` is
ambiguous. `StatusLinePreviewItem.qml:43` also binds `bgColor` straight into a `color`, so `BackgroundColor=accent`
(Review Focus 4) produces an "Unable to assign" QML warning.
*Resolution:* give each field a distinct objectName (or select it by `label`), and apply the same `#` gate at `:43`.
Add no new `"transparent"` literal: reuse the existing ternaries, so the `check_qml_colors` count does not rise.

**F12 — non-blocking (all).** The stale line numbers are listed in §2. Locate by name; nothing else in the plan's
cites is missing.

**F13 — non-blocking (Task 11).** "`git diff <the plan commit>..HEAD -- src docs/configuration`", and clang-tidy over
`git diff --name-only <the plan commit>..HEAD -- 'src/*.cpp'`.
*Why:* 3a builds on top of 1c, 1d and Step 2. A diff from the plan commit sweeps all of their sources into
`/simplify` and clang-tidy.
*Resolution:* record 3a's BASE (the commit Task 1 starts from) in the ledger, and diff from it.

**F14 — non-blocking (Task 3).** "`MockTerm(PageSize, FileSystem const&)` … threads the reference into the `Settings`
constructor's `terminal { … }` … existing constructors are unchanged."
*Why:* the `Settings` constructor (`MockTerm.hpp:481-497`) picks
`linkServices ? linkServices->fileSystem : NativeFileSystem::instance()` and has no filesystem parameter. "Unchanged"
and "threads into" contradict each other.
*Resolution:* add one private delegating constructor that takes the `FileSystem const&`. Both public paths call it,
the `Settings` constructor passing its current choice. That keeps `Terminal()`'s rule that the link services'
filesystem is the terminal's.

**F15 — non-blocking (Task 3).** "`containsItem<T>`".
*Why:* `Terminal::indicatorStatusLineDrivenByExtensions` (`Terminal.cpp:846-853`) already hand-writes this as
`namesAnExtension`. Separately, `InMemoryFileSystem::writeFile` is `[[nodiscard]]` and returns `std::expected`.
*Resolution:* replace the lambda with `containsItem<Extension>`. In the tests, `REQUIRE(files.writeFile(…))`, or use
`addFile`/`createDirectories`.

**F16 — non-blocking (Task 7).** "`InputModeNames` with one row per `ViMode`".
*Why:* no `ViModeCount` constant exists. Without one, a new mode escapes the table, as the Step 2 ledger parked for its
own enums.
*Resolution:* add the count beside `ViMode`, plus a `static_assert` that row *i* is enumerator *i*, as Task 1 does for
roles.

**F17 — non-blocking (Task 11 Step 6).** The plans index's Plan column is still shifted one row (1d → `-1c.md`,
2 → `-1d.md`, 3a–3e → `-2.md`). Correct it when 3a is marked done.

**F18 — non-blocking (sequencing).** Step 2's Tasks 7, 11, 12 and 13 are still open on the branch 3a will build on, and
its worktree has an uncommitted `src/contour/CMakeLists.txt`. Start 3a only after the fast-forward, and re-run this
scan's §2 cites at that BASE. The overlaps (`QmlComponents_test.cpp`, `ChromeSnapshotPlan.hpp`, `CMakeLists.txt`)
are append-only.

### Flag checks

- **Real sessions:** none. Every new test uses `MockTerm` (no PTY child) or an offscreen dialog, and none constructs a
  `TerminalSession`, so `MockPtySessionFactory` is not needed. Task 11's `[display][snapshot]` runs are pre-existing.
- **QML literals:** none new as planned. Watch F11's swatch and preview gates.
- **`bool` parameters:** none added. The surviving `bool`s are question-returns (`showsModeChip`, `containsItem`),
  defaulted `operator==`, and the QVariantMap `chip` key (the documented carve-out).
- **Step 1c, 1d and Step 2 names:** no reliance. The only overlap in meaning is F7 (Step 2's pane-header cwd).

## 4. What §3.7 asks that no task delivers

Nothing. All five placeholders, `Hide=Insert` (generalised to `Hide=<mode>`), the role colours with their
`tryParseColorAttribute` branch, the pure collapse, the full template and the chip editor are covered. So are the five
test bullets (parse and serialise each placeholder, the Git cache with invalidation, the selection threshold, the
collapse table, role resolution). The departures (`resolveWorkingDirectory` instead of `currentWorkingDirectory()`,
`lastFinished()` instead of `finishedBlocks`, `scrollableLineCount` as the bound) are justified in R-b, R-c and
Task 6.

## 5. Owner questions left open (proposed defaults)

1. **Duration format (F2).** The canvas's `12.4 s` / `4 ms`, or the house `12s` / `4ms` that every other surface
   prints? *Default: the house format, by reusing `vtbackend::formatCommandDuration`.*
2. **The chip in an unfocused pane.** Task 7 resolves the line to Inactive when unfocused, but says nothing about the
   chip. An unfocused pane left in vi Normal mode would still show an accent `NORMAL` chip. *Default: an unfocused
   pane draws the chip text without its mode pair, so only the focused pane carries a coloured chip.*
3. **Tie order in collapse (F1).** *Default: an item of a segment that would not be placed goes first, then right to
   left.*
4. **Left-segment overflow (F8).** *Default: an overflowing left segment drops items by priority; a single item is
   still cut.*
5. **`~` for a foreign path (F7).** *Default: only Local paths are abbreviated, through one shared function with the
   pane header.*
6. **Daemon-hosted panes.** The host terminal resolves roles against its own palette, and the client mirrors the row
   as RGB. Whether a client-side light/dark switch reaches the daemon's palette was not verified here. *Default:
   accept; add it to the owner's manual checks ("daemon session: switch scheme, the status line recolours").*
7. **`{Cwd}` alongside Step 2's pane header.** In a split, each pane now shows its directory twice: in its header and in
   its status line. *Default: keep both; the template is per profile, and the pane header goes with the split.*
