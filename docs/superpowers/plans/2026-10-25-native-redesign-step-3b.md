# Native Redesign — Step 3b (Find Bar: Regex, Whole Word, Chunked Search, Placement) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead of Steps 4a, 1c, 1d and 2 (pipelining ruling). Line numbers are at HEAD `30cdc3f0`, before any
of them lands. Where Step 3b touches code they add or change, the task names it as their plans do and carries
a **Depends on Step <id>** note. A pre-flight scan re-verifies every reference once Step 2 has landed.

**Goal:** The find bar gains regular-expression and whole-word toggles beside the case toggle, an invalid or
too-complex pattern says so in the count slot, the count is walked in 500-line steps that release the terminal
lock and yield to the event loop between them, matches paint in search yellow, and the bar sits at the active
pane's top right under its header (moving to the bottom right when the current match is under it), in the pane
header row on the Terminal row, with ⌘F / ⌘G / ⇧⌘G / ⌘E on macOS.

**Architecture:**
- **One compiled query, one subject, one matcher.** `vtbackend::SearchQuery` is compiled once per pattern and
  option change (`std::expected<SearchQuery, SearchPatternError>`). It matches a *segment* (consecutive rows of
  one logical line, at most `MaxScanCells` cells) through one subject string built from the segment's cells,
  each cluster NFC-normalised, so the literal and the regex syntax agree on wraps, wide characters, padding and
  combining marks. Stepping, the probe, the tally and the per-frame highlight all ask the same query, so the
  count can never disagree with what stepping visits or what is painted.
- **The tally is a resumable walk.** `vtbackend::SearchTallyWalk` keeps its place by stable row id, reads at
  most 500 rows of whole segments per `step()`, and survives output that scrolls, evicts or appends between
  steps. `TerminalSession` runs one step under the lock, releases it, publishes the partial count and posts the
  next step to its own event queue. The per-keystroke "nearest match behind the cursor" search is bounded to
  one step; the walk finds the rest and jumps there when it ends.
- **The bar's rules are headless.** `session/SearchStatus.hpp` gains the toggle table and the outcomes
  `Pending` and `Invalid`; `window/FindBarPlacement.hpp` decides where the bar goes. QML renders their answers
  from tokens only.

**Tech Stack:** C++23 (`std::wregex`, ECMAScript grammar), libunicode (NFC, simple lowercase), Qt 6.11 (Quick,
Quick Controls), QML, Catch2 v3, Python 3 (lint), CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.8 (`:551-575`) binds,
with what it cites and what cites it: §1 (principles, `:28-64`), §2.1 (token rows, `:68-126`), §2.5 and §2.6
(snapshots and testing, `:187-226`), §4's risk row "Regex search on 10 000 lines blocks the GUI thread | Chunked
walk with the lock released between chunks (3b), and a compiled regex" (`:836`) and §6's round-1 correction
"Search ran on the GUI thread | 3b chunks the walk" (`:868`). §3.8's code citations have moved: the case toggle's
rules are `session/SearchStatus.hpp:114-165` (`CasePinned`, `describeSearchCase`, `nextSearchCase`); the search on
the GUI thread under the state lock is `TerminalSession.cpp:4443-4489` (the two focus actions), with the
synchronous whole-grid tally in `refreshSearchStatusLocked` (`:2725-2757`) and the per-keystroke backward walk in
`setSearchPattern` (`:2527-2573`). The design record is `docs/superpowers/specs/2026-10-23-native-redesign-design.md`
("yellow means search", `:22`; the TUI row's "the find bar lives in the pane header row", `:42`). The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `mac-find-menu`, `tui`, `kit-tokens` and
`kit-components` are in `.superpowers/design/canvas/project/`. The plans index is
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

- **The spec, verbatim (§3.8):**
  - "Add `SearchSyntax { Literal = 0, Regex }` and `SearchWordBoundary { Any = 0, WholeWord }`."
  - "An invalid regex is reported as `std::expected` and shown as "Invalid pattern" in the count slot."
  - "The walk becomes chunked: 500 lines per step, releasing the lock between steps, and yielding to the event
    loop." "The count updates as the walk progresses." "A regex is compiled once per query."
  - "Pure decision: `window/FindBarPlacement.hpp`." "The bar sits under the pane header, or at the top right of a
    single pane. It moves to the bottom right when the current match is under it." "The count text reads "2 of 5"."
  - Tests: "Regex and whole-word matching in `vtbackend_test`, including an invalid regex." "Chunked search equals
    unchunked search." "The placement table."
- **Canvas values, exact** (`mac-find-menu`, `kit-components`, `kit-tokens`, `tui`):

  | Token | MacOS | Fluent | ContourLinux | Terminal (cells) |
  |---|---|---|---|---|
  | `size.findBar` (height) | 32 | 32 | 32 | 1 |
  | `size.findBarWidth` | 380 | 380 | 380 | 48 |
  | `size.findToggleWidth` × `size.findToggleHeight` | 24 × 22 | 24 × 22 | 24 × 22 | 4 × 1 |
  | `size.findButton` (previous, next, close; square) | 22 | 22 | 22 | 2 |
  | `size.findIcon` (the magnifier) | 14 | 14 | 14 | 0 |
  | `space.findBarInset` (from the pane's right and top edge) | 8 | 8 | 8 | 1 |
  | `radius.findBar` | 8 | 8 | 8 | 0 |
  | `layout.findBar` (`FindBarHost`) | `Overlay` | `Overlay` | `Overlay` | `PaneHeaderRow` |

  - Bar padding: leading `s5` (10), trailing `s2` (4); gap between items `s2` (4). Toggles at `radii.control`.
    Field text `type.body`; count and toggle glyphs `type.footnote`, the count in tabular digits
    (`font.features: { "tnum": 1 }`). Shadow `elevation.e2`, drawn as `Toast.qml` draws it.
  - Toggles, in order: `Aa` (case; `aa` when pinned insensitive, as today), `.*` (regex), `ab` (whole word). An
    "on" toggle is accent-tinted: fill = the accent at 22 %, glyph = the accent; "off" glyph = `label(2)`. The
    Terminal row brackets them `[Aa] [.*] [ab]`, on = the scheme's selection with `SelectionText`, off =
    `Secondary`; its previous/next glyphs are `↑ ↓`, close `×`.
  - Matches: yellow `#FFD60A` at 35 %, the current one at 85 % (the canvas's "1 pt ring" is owner decision 3).
    A miss says "No results" in the count slot and tints the field red (`chromeStyle.errorColor`).
  - ⌘G / ⇧⌘G cycle, ⌘E uses the selection, ⎋ closes. ⌘F opens.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e; the 4a, 1c, 1d and 2 SDD ledgers):
  - **Yellow means search, everywhere,** and nothing else is yellow-filled; Running is the accent.
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`); a new file's baseline is 0
    and a baseline only goes down (`SearchBar.qml` is at 2 today, `scripts/qml-size-literal-baseline.txt:13`, and
    goes to 0).
  - Every visual surface is a snapshot row, and every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - `enum class` over `bool`, zero the default; QML-facing `Q_PROPERTY`/`QVariantMap`/`Q_INVOKABLE` booleans are
    the documented carve-out. `std::expected` for every fallible call (`SearchPatternError`, `InvalidSyntax = 0`).
  - Text contrast at least 4.5:1, checked against contour-dark and contour-light.
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **Threading (§3.8, §4, §6):** the search runs on the GUI thread, under the terminal's state lock, **one step at a
  time**: a step reads at most `SearchWalkStepLines` (500) rows of whole segments, the lock is released after it,
  and the next step is posted to the session's event queue (`platform::postToObject`), never run in a loop. No
  search work moves to another thread. A step never holds the lock across a post, and the per-keystroke probe is
  itself bounded to one step.
- **AGENT.md applies in full:** data-driven tables (the toggle table, the placement rules, the find chords, the
  token rows); dependency injection (no clock or thread; the event loop is the session's own queue);
  configuration at construction (`SearchQuery` is immutable once compiled, `SearchTallyWalk` takes its query and
  limit in its constructor); Doxygen on every new public declaration; `.hpp` with `#pragma once`; zero warnings
  and no `NOLINT`; `slots`, `signals` and `emit` are Qt macros, never identifiers. This step touches
  `Terminal::setNewSearchTerm` and `setSearchCaseSensitivity` (both `bool` returns, `Terminal.hpp:3664`, `:3669`),
  so they become `std::expected<SearchChange, SearchPatternError>` in the same change.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in
  `out/appleclang-debug/CMakeCache.txt`): `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`.
  Never run two builds in one tree at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit, run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding. Format every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`;
  check every new or touched `.qml`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`. **Never
  clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a
  value, and every table row names every field (`ChromeTokenRows`, `SearchToggleRows`, `SnapshotScripts`, the
  `ContourScheme` rows).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type. Counts
  in tests are derived from tables (`SearchToggleRows.size()`, `SnapshotScripts.size()`), never literals.
- **libstdc++'s regex runs on its breadth-first executor** (`__polynomial`), as the link patterns do; no
  back-references on any platform. A `std::regex_error` thrown while matching is `SearchPatternError::TooComplex`,
  never an escaping exception.
- **QML:** enums are unreachable through `chromeStyle`; QML passes and compares `int`s or the name strings the
  provider publishes. Every new control sets `Accessible.role` and `Accessible.name`.
- **Qt-free targets stay Qt-free:** `window/FindBarPlacement.hpp` and `session/SearchStatus.hpp` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files by name (other agents share the tree; never
  `git add -A` or a directory). Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, and never carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 13).

## Review Focus

1. **A catastrophic regex on a 10 000-line scrollback.** `(a+)+$` over 10 000 rows of sixty `a`s and a `b` is
   exponential in a backtracking engine (libc++ on macOS). Expected: no step blocks the GUI thread for long; the
   walk ends with "Pattern too complex" (libc++ and MSVC give up through their own complexity limits, which the
   matcher catches) or with "No results" (libstdc++'s breadth-first executor is polynomial); typing or ⎋ still
   work. Pinned in Task 4 (`searchWalk.aCatastrophicPatternEndsTheWalk`) and Task 7 ("a catastrophic pattern ends
   in a word, not a hang").
2. **A match spanning a wrapped line.** `de 3` across a soft wrap, a whole word whose neighbour is on the next
   row, and a match across the 500-row step boundary. Expected: matched, highlighted on both rows, counted once,
   and a hard line break is never crossed. Pinned in Task 2 (`searchQuery.aRegexSpansASoftWrapButNotAHardBreak`,
   `searchQuery.wholeWordLooksAcrossASoftWrap`), Task 3 (`search.regexHighlightPaintsBothRowsOfAWrappedMatch`),
   Task 4 (`searchWalk.aMatchAcrossTheStepBoundaryCountsOnce`) and Task 7 (`searchFocusedRows` spans two rows).
3. **Searching while output streams in.** Output arriving between steps scrolls rows, evicts the oldest and
   appends new ones. Expected: the walk finishes, never counts a row twice, skips rows evicted before it reached
   them, restarts on a reflow, and the lock is free between steps so the parser thread is never starved. Pinned
   in Task 4 (`searchWalk.finishesWhileOutputStreamsIn`, `searchWalk.restartsWhenTheGridReflows`) and Task 7
   ("the count is walked in steps and the lock is free between them").
4. **A wide or combining character in the pattern.** `本語` over two wide cells, `日.` where `.` must take a whole
   wide character, and `café` typed precomposed (U+00E9) against a decomposed `e` + U+0301 printed by `ls` on
   macOS. Expected: all match, highlights cover whole cells (both columns of a wide character), and no match
   begins or ends inside a cell's cluster. Pinned in Task 2 (`searchQuery.wideAndCombiningCharacters`) and Task 3
   (`search.regexHighlightCoversBothHalvesOfAWideCharacter`).
5. **⌘G with no prior search.** Expected: the find bar opens, nothing is written to the PTY, the cursor does not
   move; with a pattern and the bar closed, ⌘G steps and wraps without opening it. Pinned in Task 8 ("Find Next with
   no search opens the find bar", "Find Next wraps").

Also pinned, beyond the five: an invalid regex (Tasks 1, 3, 7); a double-clicked word stays literal while the
regex toggle is on (Task 3); a match far above the cursor is still reached after the bounded probe (Task 7); ⎋
closes and returns focus (Task 10).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. One subject for both syntaxes.** A segment's subject is its cells in order: each cluster's codepoints
  NFC-normalised (`unicode::to_nfc`), an empty cell a space, a no-text cell (`NoTextCellFlags`: wide-character
  continuation, wrap padding, a tall block's lower part) nothing, a blank row (`Line::isBlank()`) one `\n` that no
  pattern can match across, the segment's trailing empty cells dropped. Under `CaseComparison::Folded` the subject
  and the pattern are lowered with `unicode::simple_lowercase` (a regex's escaped letters excepted, so `\D` stays
  `\D`). A match must begin on a cell's first unit and end on a cell's last unit; otherwise it is dropped and the
  search resumes one unit after its start. Zero-length matches are dropped. Literal matches overlap (every start,
  as `Screen::tallyMatches` counts today); regex matches do not (the engine's own iteration). This replaces
  `Grid::textMatchesAt`/`matchedTextEnd` (`Grid.hpp:934-953`, `Grid.cpp:2503-2557`), whose callers all move.
  Empty cells now match a space in the pattern: "foo bar" finds an on-screen "foo bar" however the gap was made.
- **R-b. Regex flavour.** ECMAScript through `std::wregex` (`wchar_t` is UTF-32 on macOS and Linux, so `.` takes one
  codepoint; Windows' UTF-16 makes a non-BMP codepoint two units, which the cluster-boundary rule of R-a keeps
  whole). `^` and `$` anchor at the segment's ends. Back-references (`\1`…) are refused with
  `SearchPatternError::BackReference`, as link patterns refuse them (`LinkPatterns.hpp:87-96`). Smart case reads
  the regex's unescaped characters only.
- **R-c. Whole word.** A match is a whole word when the unit before it and the unit after it are each a space,
  `\n`, one of the terminal's `wordDelimiters` (the double-click selection's set), or the segment's end. The
  delimiters are read when the query is compiled.
- **R-d. A segment** is at most `MaxScanCells` (36 864, `links/ScanText.hpp:35`) cells of one logical line, cut
  from the line's head every `max(1, MaxScanCells / columns)` rows, so the walk and the highlighter cut a long line
  at the same rows. A regex match across a cut is not found (the link scanner's rule); a literal one is, since a
  literal never spans more cells than it has codepoints.
- **R-e. The keystroke probe is bounded.** `setSearchPattern` searches backwards from the cursor for at most one
  step (500 rows). If that finds nothing, the bar shows `Pending` (neither red nor navigable), and the tally walk,
  which reads the whole grid anyway, remembers the nearest match at or before the cursor and the last match
  overall; when it ends with the cursor where it started, the session jumps there (wrapping to the last match
  when none lies behind). Stepping (Enter, ⌘G, F3) stays one synchronous search, as today: it is one explicit
  action, not one per keystroke.
- **R-f. A pattern that blows the engine's budget** (`std::regex_error` while matching: libc++'s
  `error_complexity`, MSVC's `error_stack`/`error_complexity`) ends the query: `Search::problem` becomes
  `TooComplex`, the compiled query is dropped (so nothing highlights and no further step runs), and the count slot
  reads "Pattern too complex". `InvalidSyntax` and `BackReference` read "Invalid pattern" (§3.8's words).
- **R-g. A double-clicked word is always literal.** `SearchOrigin::DoubleClick` compiles `SearchSyntax::Literal`
  whatever the toggles say; the stored options are untouched.
- **R-h. Find Next and Find Previous wrap (owner, 5).** `FocusNextSearchMatch`/`FocusPreviousSearchMatch` go through
  the session's wrapping `stepSearch` (today only the bar's buttons and Enter do), so F3 and ⌘G wrap like Enter. vi's
  `n`/`N` (`ViCommands.cpp:199-215`) keep stopping at the ends. With no pattern they open the find bar.
- **R-i. Where the bar goes.** Native rows parent the bar to the pane's terminal item, which Step 2's card places
  below the header, so "under the pane header" is the terminal's top plus `space.findBarInset` (the canvas's top 30
  = header 22 + 8). The Terminal row hosts it in the pane header row when the pane has a header; a pane without one
  (a single pane, Minimal) gets the floating one-row bar at its top right **(owner, 2)**.
- **R-j. Chords (owner, 4 and 6).** On macOS, both editions: ⌘F `SearchReverse`, ⌘G `FocusNextSearchMatch`, ⇧⌘G
  `FocusPreviousSearchMatch`, ⌘E the new `SearchSelection`, all in every mode (so ⌘G with no search reaches the
  session). Elsewhere the existing Ctrl+Shift+F and F3/Shift+F3 (Search mode only, so F3 still reaches `mc` with no
  search) stay, and "Use Selection for Find" has no default chord (Ctrl+Shift+E is `SplitVertical`,
  `Config.hpp:1490-1493`). Enter / Shift+Enter in the field step everywhere.
- **R-k. Use Selection for Find** takes the selection's first line; under `Regex` it is escaped
  (`escapeSearchRegex`). No selection does nothing. It opens the bar with the pattern in the field.
- **R-l. Match colours are a scheme's.** `contour-dark` and `contour-light` gain `searchHighlight` = yellow
  `#FFD60A` at 35 % over the cell's own text, and `searchHighlightFocused` = yellow at 85 % with dark ink (the
  scheme's background on contour-dark, its foreground on contour-light). Every other scheme, `contour-classic`
  included, keeps its colours (§2.7). A scheme's yellow `#8A6100` (contour-light) fails 4.5:1 as a fill, hence the
  canvas's system yellow.
- **R-m. The toggles' state is the session's.** Regex and whole word start `Literal`/`Any` in every session and are
  not configuration keys; the case policy keeps its `search_case_sensitivity` profile key.
- **R-n. The bar's surface** is `PopupSurface.qml` at `radius.findBar`: opaque, as every popup here is. No surface in
  the tree renders `material.popover` yet, so "popover glass" is not drawn by this step (a spec gap).
- **R-o. Snapshots.** New rows `find-bar`, `find-bar-split` and `find-bar-invalid` (Task 12). The snapshot terminal
  is the stub, so matches are not in the pictures; their colours are pinned by Task 5's palette test.

### Non-goals

- The current match's 1-pt ring (owner decision 3), search-match scrollbar ticks (3f's), search history, replace.
- Moving any search work off the GUI thread.
- The status line's `{SearchPrompt}` placeholder (unchanged).

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/vtbackend/core/Search.hpp` (modify) | `SearchSyntax`, `SearchWordBoundary`, `SearchOptions`, `SearchPatternError`, `SearchChange` | 1 |
| `src/vtbackend/core/RegexEngine.hpp` (create) | The engine flags and the back-reference scan, shared with the link patterns | 1 |
| `src/vtbackend/links/LinkPatterns.cpp` (modify) | Uses `RegexEngine.hpp` | 1 |
| `src/vtbackend/screen/SearchQuery.{hpp,cpp}` (create) | Compile, segments, the subject, matching | 1, 2 |
| `src/vtbackend/screen/Screen.{hpp,cpp}`, `Terminal.{hpp,cpp}`, `RenderBufferBuilder.{hpp,cpp}`, `grid/Grid.{hpp,cpp}`, `input/vi/ViCommands.cpp` (modify) | Everything searches through the query | 3, 4 |
| `src/vtbackend/screen/SearchTallyWalk.{hpp,cpp}` (create) | The resumable, chunked tally | 4 |
| `src/vtbackend/core/ColorPalette.cpp` (modify) | Search yellow in contour-dark/-light | 5 |
| `src/contour/session/SearchStatus.hpp` (modify) | Toggle table, `Pending`, `Invalid`, progress | 6 |
| `src/contour/session/TerminalSession.{hpp,cpp}` (modify) | Toggles, stepped tally, probe, focused rows, actions | 7, 8 |
| `src/contour/config/Actions.hpp`, `Config.hpp`, `ConfigDocumentation.hpp`, `command/MenuBarTable.cpp` (modify) | `SearchSelection`, the macOS find chords, the Edit row | 8 |
| `src/contour/window/FindBarPlacement.hpp` (create) | Where the bar goes | 9 |
| `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `window/UiStyleProvider.{hpp,cpp}`, `window/ChromeTokenObjects.{hpp,cpp}`, `display/TerminalDisplay.{hpp,cpp}` (modify) | Tokens, glyphs, the placement invokable, `rowBand` | 9 |
| `src/contour/qml/SearchBar.qml`, `PopupSurface.qml`, `SessionChrome.qml`, `src/contour/test/QmlMocks.hpp` (modify) | The bar | 10 |
| `src/contour/qml/PaneHeader.qml`, `PaneCard.qml`, `TerminalPane.qml` (modify, Step 2's) | The Terminal row's header-row bar | 11 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | Snapshot rows | 12 |

### Shared files

Files another Phase 3 step's spec section also names, or that every Phase 3 step edits. A parallel worktree that
touches one rebases before its task commits, and the merge order is the board's.

- `src/contour/session/TerminalSession.{hpp,cpp}`: §3.9 (3c, the guarded-role API `:823-833`, `:1261`, and config
  reload errors `.cpp:4812-4816`). 3b's hunks are the find-bar block (`.hpp:238-339`, `:1779-1787`; `.cpp:2457-2758`,
  `:4443-4489`, `:6063`) and one new action handler.
- `src/contour/qml/SessionChrome.qml`: §3.9 (3c, `RequestPermission` and the consent dialogs, `:353-357`), §3.11 (3e,
  the scrollbar `:135` and the hyperlink tooltip `:64-90`), §3.12 (3f, the block tooltip `:97-128`). 3b touches only
  the `SearchBar` instance (`:360-367`) and `onSearchBarRequested` (`:479-482`).
- `src/contour/qml/TerminalPane.qml`: §3.11 (3e, the size indicator `:130-166`). 3b adds one forwarded property.
- `src/vtbackend/screen/Terminal.{hpp,cpp}`: §3.7 (3a, `currentWorkingDirectory()` `:3496`, the `FileSystem` `:826`),
  §3.11 (3e, `applyHintOverlay` `.cpp:5395`), §3.12 (3f, `fillGutter` `.cpp:3937`, `fillStickyHeader` `:4361`). 3b's
  hunks are `Search` (`.hpp:297-313`) and the search block (`.hpp:3647-3686`, `.cpp:8486-8615`).
- `src/vtbackend/core/ColorPalette.{hpp,cpp}`: §3.12 (3f, `blockStatus*`, `.hpp:371-425`). 3b edits `ContourScheme`
  and `applyContourScheme` only (`.cpp:44-133`).
- `src/contour/command/CommandCatalog.hpp` and `config/Actions.hpp`: §3.10 (3d adds a `CommandCategory` column to the
  catalogue). Whichever lands second gives `SearchSelection` its category (Tab/Pane/…: "Find" has no category in
  §3.10's list; use the one 3d gives `SearchReverse`).
- `src/contour/qml/PaneHeader.qml` (Step 2's): §3.11 (3e's link pill "in the pane header's trailing area") and §3.12
  (3f's sticky header "moves into the pane header"). 3b adds the find slot only.
- Edited by every Phase 3 step: `src/contour/config/ChromeTokenGroups.hpp` (append rows' fields; `ChromeColorRole`
  appends after whatever is last at execution), `src/contour/test/ChromeSnapshotPlan.hpp` (append scripts),
  `src/contour/test/QmlMocks.hpp`, `src/contour/CMakeLists.txt`, `scripts/qml-size-literal-baseline.txt`,
  `metainfo.xml`.

---

### Task 1: The search vocabulary and `SearchQuery::compile`

**Files:**
- Modify: `src/vtbackend/core/Search.hpp` (after `CaseComparison`, `:85-110`).
- Create: `src/vtbackend/core/RegexEngine.hpp`; list it in `src/vtbackend/CMakeLists.txt` beside `core/Search.hpp`
  (`:87`).
- Modify: `src/vtbackend/links/LinkPatterns.cpp:50-62` (the engine flags) and `:105-151` (`anySourceCharacter`,
  `hasBackReference`) to use the shared header; `hasNonAsciiInBracket` stays there on the shared scanner.
- Create: `src/vtbackend/screen/SearchQuery.hpp`, `SearchQuery.cpp`; list them at `:92-95` and `:178-215`.
- Test: create `src/vtbackend/screen/SearchQuery_test.cpp` (add to `vtbackend_test`, `:284-381`);
  `src/vtbackend/core/Search_test.cpp`.

**Interfaces:**
- Produces (namespace `vtbackend`, `core/Search.hpp`):
  - `enum class SearchSyntax : uint8_t { Literal = 0, Regex };`
  - `enum class SearchWordBoundary : uint8_t { Any = 0, WholeWord };`
  - `struct SearchOptions { SearchCaseSensitivity caseSensitivity = SearchCaseSensitivity::Smart; SearchSyntax syntax = SearchSyntax::Literal; SearchWordBoundary wordBoundary = SearchWordBoundary::Any; constexpr bool operator==(SearchOptions const&) const noexcept = default; };`
  - `enum class SearchPatternError : uint8_t { InvalidSyntax = 0, BackReference, TooComplex };`
  - `enum class SearchChange : uint8_t { Unchanged = 0, Changed };`
- Produces (`core/RegexEngine.hpp`, header-only, std only):
  - `inline constexpr std::regex_constants::syntax_option_type RegexEngineFlags` — `ECMAScript | optimize`, plus
    `__polynomial` under `__GLIBCXX__` (the comment moves with it).
  - `enum class RegexSourceRole : uint8_t { Plain = 0, Escaped, InClass, EscapedInClass };` and
    `template <typename Char, typename Test> [[nodiscard]] constexpr bool anyRegexSourceCharacter(std::basic_string_view<Char> source, Test test)`
    (today's `anySourceCharacter`, generic over the character type).
  - `template <typename Char> [[nodiscard]] constexpr bool hasRegexBackReference(std::basic_string_view<Char> source) noexcept`.
- Produces (`screen/SearchQuery.hpp`):
  - `class SearchQuery` with
    `[[nodiscard]] static std::expected<SearchQuery, SearchPatternError> compile(std::u32string_view pattern, SearchOptions options, std::u32string_view wordDelimiters);`
    (`@pre !pattern.empty()`), `[[nodiscard]] std::u32string const& pattern() const noexcept;`,
    `[[nodiscard]] SearchOptions options() const noexcept;`, `[[nodiscard]] CaseComparison comparison() const noexcept;`.
    Members (private): the pattern as given, the options, the resolved comparison, the delimiters, the folded NFC
    needle (`std::wstring`, Literal) or `std::optional<std::wregex>` (Regex).
  - `[[nodiscard]] std::u32string escapeSearchRegex(std::u32string_view text);` — backslash before each of
    `\^$.|?*+()[]{}`.
- `compile`: the comparison is `caseComparisonFor` over the pattern for Literal, and over its `Plain`/`InClass`
  characters only for Regex (R-b). Regex: `hasRegexBackReference` → `BackReference`; the pattern NFC-normalised,
  folded outside escapes when `Folded` (R-a), widened (one `wchar_t` per codepoint where `sizeof(wchar_t) == 4`, a
  surrogate pair above U+FFFF otherwise), compiled with `RegexEngineFlags`; `std::regex_error` → `InvalidSyntax`.

- [ ] **Step 1: Write the failing tests** (`[searchquery]` unless stated):
  - `searchQuery.literalAlwaysCompiles`: `compile(U"a(b", {}, U" ")` has a value; `pattern() == U"a(b"`.
  - `searchQuery.invalidRegexIsAnError`: with `.syntax = Regex`, each of `U"a(b"`, `U"[z-a]"`, `U"*x"` gives
    `error() == SearchPatternError::InvalidSyntax`.
  - `searchQuery.backReferenceIsRefused`: `U"(a)\\1"` → `BackReference`; `U"\\\\1"` (an escaped backslash, then
    `1`) compiles.
  - `searchQuery.smartCaseReadsTheRegexsOwnLetters`: Regex + Smart: `U"\\d+"` → `Folded`, `U"\\dA"` → `Exact`;
    Literal + Smart: `U"Foo"` → `Exact`, `U"foo"` → `Folded`; `Insensitive` + `U"Foo"` → `Folded`.
  - `searchQuery.escapeSearchRegex`: `escapeSearchRegex(U"a.b*(c)")` compiles as a regex, and equals
    `U"a\\.b\\*\\(c\\)"`.
  - `Search_test.cpp` (`[search]`): `SearchOptions.defaultsAreZero` — each field of `SearchOptions {}` casts to 0,
    and `SearchPatternError::InvalidSyntax`, `SearchChange::Unchanged` are 0.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test`

  Expected: compile error, `SearchQuery` not declared.
- [ ] **Step 3: Implement** the vocabulary, `RegexEngine.hpp`, the `LinkPatterns.cpp` switch-over and `compile`.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[searchquery],[search],[links]"`

  Expected: PASS; the `[links]` cases (back-reference and bracket diagnostics) unchanged.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/screen/SearchQuery.cpp src/vtbackend/links/LinkPatterns.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/core/Search.hpp src/vtbackend/core/RegexEngine.hpp src/vtbackend/links/LinkPatterns.cpp src/vtbackend/screen/SearchQuery.hpp src/vtbackend/screen/SearchQuery.cpp src/vtbackend/screen/SearchQuery_test.cpp src/vtbackend/core/Search_test.cpp
editorconfig-checker src/vtbackend/CMakeLists.txt
git add src/vtbackend/core/Search.hpp src/vtbackend/core/RegexEngine.hpp src/vtbackend/links/LinkPatterns.cpp src/vtbackend/screen/SearchQuery.hpp src/vtbackend/screen/SearchQuery.cpp src/vtbackend/screen/SearchQuery_test.cpp src/vtbackend/core/Search_test.cpp src/vtbackend/CMakeLists.txt
git commit -m "vtbackend: search options and a query compiled once, with an invalid regex as an error" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: Matching a segment: literal, regex, whole word, wraps and clusters

**Files:**
- Modify: `src/vtbackend/screen/SearchQuery.{hpp,cpp}`.
- Test: `src/vtbackend/screen/SearchQuery_test.cpp`.

**Interfaces:**
- Consumes: Task 1.
- Produces (`screen/SearchQuery.hpp`):
  - `struct SearchSegment { LineOffset first {}; LineOffset last {}; bool operator==(SearchSegment const&) const = default; };`
  - `[[nodiscard]] SearchSegment searchSegmentAt(Grid const& grid, LineOffset row);` — R-d, from
    `Grid::logicalLineHead` (`Grid.cpp:1183`) and `logicalLineEnd(head, lastPageLine)` (`:1208`).
  - `[[nodiscard]] std::optional<SearchSegment> nextSearchSegment(Grid const& grid, SearchSegment segment);` and
    `previousSearchSegment(...)`: the adjacent segment, or none at the grid's bottom / the history's top.
  - `[[nodiscard]] std::expected<std::vector<CellLocationRange>, SearchPatternError> SearchQuery::matchesIn(Grid const& grid, SearchSegment segment) const;`
    — every match whose first cell is in @p segment, sorted by first cell; `.first` is its first cell, `.second`
    its last column (a wide character's second half where it ends on one). `std::regex_error` → `TooComplex`.
- The subject (private to `SearchQuery.cpp`): `std::wstring units`, and per cell its first unit and its
  `CellLocation`s, so a unit maps to a cell and a cell to its last column (R-a). Literal: compare the folded needle at
  every cell-first unit. Regex: `std::regex_search` in a loop from a moving position, with `match_prev_avail` past the
  start; drop a zero-length or off-boundary match and resume one unit after its start; accept and resume at its end.
  Whole word: R-c, on the units.

- [ ] **Step 1: Write the failing tests** (`[searchquery]`, `MockTerm` grids; delimiters `U" ./"`):
  - `searchQuery.literalCountsOverlappingStarts`: `"aaa"`, literal `aa` → `{(0,0)-(0,1)}`, `{(0,1)-(0,2)}`.
  - `searchQuery.regexMatchesWithinASegment`: page 2 × 10, `"abc 12 de 345"` (wraps after `de `), regex `[0-9]+` →
    `{(0,4)-(0,5)}`, `{(1,0)-(1,2)}`.
  - `searchQuery.aRegexSpansASoftWrapButNotAHardBreak` (Review Focus 2): the same grid, regex `de 3` →
    `{(0,7)-(1,0)}`, one segment `{0, 1}`; written instead as `"abc 12 de\r\n345"`, `de.*3` finds nothing in either
    row's segment, and `searchSegmentAt(grid, 1) == {1, 1}`.
  - `searchQuery.wholeWordUsesTheWordDelimiters`: page 1 × 30, `"foo foobar barfoo foo."`, whole-word `foo` → starts
    at columns 0 and 18 only, for literal `foo` and regex `fo+` alike; `.wordBoundary = Any` → four.
  - `searchQuery.wholeWordLooksAcrossASoftWrap` (Review Focus 2): page 2 × 4, `"ab foo"` → whole-word `foo` is
    `{(0,3)-(1,1)}`; `"ab foox"` → none.
  - `searchQuery.wideAndCombiningCharacters` (Review Focus 4): page 1 × 12, `U"日本語 café"` (cells 日 0–1,
    本 2–3, 語 4–5, space 6, c 7, a 8, f 9, é 10):
    - literal `本語` → `{(0,2)-(0,5)}`; regex `日.` → `{(0,0)-(0,3)}`; regex `語 c` → `{(0,4)-(0,7)}`;
    - literal `café` typed precomposed (`U"café"`) → `{(0,7)-(0,10)}`, and decomposed likewise; regex `caf.` →
      `{(0,7)-(0,10)}`;
    - literal `cafe` and regex `cafe` → none (the cell is é, not e); regex `e` → none.
  - `searchQuery.blankRowsAndPaddingAreNotText`: a wide character pushed to the next row leaves wrap padding at the
    row's end; literal `ab` across that padding matches with `.second` on the next row; a blank continuation row (the
    `\n` unit) stops `a.b`.
  - `searchQuery.zeroLengthAndAnchors`: regex `a*` over `"bab"` → `{(0,1)-(0,1)}` only; `^b` → column 0; `b$` →
    column 2 (trailing empty cells dropped, page 1 × 10).
  - `searchQuery.foldedRegexIsUnicodeAware`: Smart, regex `привет` matches `"ПРИВЕТ"`; `\D+` over `"12ab"` →
    `{(0,2)-(0,3)}`.
  - `searchQuery.segmentsCutALongLineAtFixedRows`: a 200-column page holding one logical line of 400 rows:
    `searchSegmentAt` of rows 0, 183, 184 gives `{0,183}`, `{0,183}`, `{184,367}` (36 864 / 200 = 184), and
    `nextSearchSegment({0,183}) == {184,367}`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[searchquery]"`

  Expected: compile error, `matchesIn` not declared.
- [ ] **Step 3: Implement** segments, the subject and `matchesIn`.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[searchquery],[search]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/screen/SearchQuery.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/screen/SearchQuery.hpp src/vtbackend/screen/SearchQuery.cpp src/vtbackend/screen/SearchQuery_test.cpp
git add src/vtbackend/screen/SearchQuery.hpp src/vtbackend/screen/SearchQuery.cpp src/vtbackend/screen/SearchQuery_test.cpp
git commit -m "vtbackend: match a logical line's segment, literal or regex, by whole word, across wraps and clusters" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: Stepping, the terminal's search and the highlight go through the query

**Files:**
- Modify: `src/vtbackend/screen/Screen.hpp:865-918`, `Screen.cpp:8715-8841` (`search`, `searchFrom`,
  `searchReverse`, `searchReverseFrom`; `tallyMatches` keeps its signature and walks `searchFrom(query, …)` over a
  compiled literal query until Task 4 replaces it).
- Modify: `src/vtbackend/screen/Terminal.hpp:297-313` (`Search`), `:3395-3398`, `:3647-3686`;
  `Terminal.cpp:8486-8615`.
- Modify: `src/vtbackend/screen/RenderBufferBuilder.hpp:264-275`, `:366-370`; `RenderBufferBuilder.cpp:706-779`.
- Modify: `src/vtbackend/grid/Grid.hpp:934-953`, `Grid.cpp:2503-2557` (remove `textMatchesAt`, `matchedTextEnd`).
- Modify: `src/vtbackend/input/vi/ViCommands.cpp` (`:228`, `:1099`: the new returns), and the callers that drop a
  `setNewSearchTerm` result unannotated: `ViCommands_test.cpp:497`, `Terminal_selection_test.cpp:173`, `:214`,
  `src/contour/session/TerminalSession_test.cpp:4001` (`(void)`).
- Modify: `src/contour/session/TerminalSession.cpp:2544`, `:2594`, `:2728-2729`, `:6063` — compile-only adaptation
  (`setSearchOptions` with the toggled case; `search().options.caseSensitivity`); Task 7 reworks these paths.
- Test: `src/vtbackend/screen/Screen_test.cpp`, `src/vtbackend/screen/RenderBufferBuilder_test.cpp`.

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces (`Screen`): `[[nodiscard]] std::expected<std::optional<CellLocation>, SearchPatternError> searchFrom(SearchQuery const& query, CellLocation start) const;`,
  the same for `searchReverseFrom`, and
  `[[nodiscard]] std::expected<std::optional<CellLocationRange>, SearchPatternError> matchStartingAt(SearchQuery const& query, CellLocation start) const;`.
  `search(std::u32string_view, CellLocation, SearchCaseSensitivity)` and `searchReverse(…)` stay as literal
  conveniences over a compiled literal query (the `[screen]` cases use them). The `CaseComparison` overloads go.
- Produces (`Terminal`):
  - `struct Search { std::u32string pattern; SearchOptions options {}; SearchOrigin origin = SearchOrigin::Typed; std::shared_ptr<SearchQuery const> query {}; std::optional<SearchPatternError> problem {}; };`
    — `caseSensitivity` moves into `options`; the `comparison` cache goes (the query carries it).
  - `[[nodiscard]] std::expected<SearchChange, SearchPatternError> setNewSearchTerm(std::u32string text, SearchOrigin origin);`
    and `[[nodiscard]] std::expected<SearchChange, SearchPatternError> setSearchOptions(SearchOptions options);`
    (replaces `setSearchCaseSensitivity`). Both recompile when the text or the effective options change (R-g), with
    `_settings.wordDelimiters`; on error the pattern stays, `query` is null and `problem` is set; on success
    `problem` is cleared. An empty pattern has no query and no problem.
  - `search(CellLocation)`, `searchReverse(CellLocation)`, `searchNextMatch`, `searchPrevMatch` keep their
    signatures; a `TooComplex` from the screen sets `problem`, drops `query`, and returns nullopt (R-f).
  - `[[nodiscard]] std::optional<CellLocationRange> focusedSearchMatch() const;` — the match starting at the
    normal-mode cursor on the page on display.
- `RenderBufferBuilder::prepareSearchHighlights`: the segment of each rendered row (`searchSegmentAt`), its matches
  from `search.query->matchesIn` computed once per segment per pass (cached beside `_searchMatches`, replacing
  `_searchScannedThrough`), an error painting nothing; the painting loop (`:762-778`) is unchanged.

- [ ] **Step 1: Write the failing tests:**
  - `Screen_test.cpp` (`[screen]`):
    - `search.regexStepsAndWholeWordSkipsPartialWords`: `"cat concat cat"`, `setSearchOptions({.wordBoundary = WholeWord})`,
      `setNewSearchTerm(U"cat", Typed)`; `searchNextMatch` from `(0,0)` lands on `(0,11)`, never `(0,7)`.
    - `search.invalidRegexHighlightsNothingAndSaysWhy`: Regex, `setNewSearchTerm(U"a(", Typed).error() == InvalidSyntax`;
      `search().problem == InvalidSyntax`, `search().query == nullptr`, `search().pattern == U"a("`, `search(pos)` is
      nullopt, and no rendered cell carries the search highlight; `setNewSearchTerm(U"a", Typed)` clears `problem`.
    - `search.optionsReportWhetherTheyChanged`: the same options twice → `Unchanged`, then a different syntax → `Changed`.
    - `search.doubleClickWordIsLiteralUnderRegex` (R-g): Regex on; `setNewSearchTerm(U"a+b", DoubleClick)` has a value
      and matches the text `a+b`, not `aab`; `search().options.syntax` is still `Regex`.
    - The existing `searchReverse`, `search.smartCaseIsCodepointAware`, `search.walksTextCellsAcrossWideCharactersAndPadding`,
      `search.highlightPaintsExactlyTheCellsTheSearchMatches`, `search.highlightReachesAcrossRowsWithoutText` and
      `tallyMatches` cases pass **unedited**: they are this rewrite's regression net.
  - `RenderBufferBuilder_test.cpp` (`[renderbuffer][search]`):
    - `search.regexHighlightPaintsTheMatchedCells`: `"id 42 and 7"`, regex `[0-9]+` → columns 3, 4 and 10 carry
      `searchHighlight` colours, the cursor's match carries `searchHighlightFocused`, columns 5–9 neither.
    - `search.regexHighlightPaintsBothRowsOfAWrappedMatch` (Review Focus 2): page 2 × 10, `"abc 12 de 345"`, regex
      `de 3` → `(0,7)`, `(0,8)`, `(0,9)`, `(1,0)` highlighted.
    - `search.regexHighlightCoversBothHalvesOfAWideCharacter` (Review Focus 4): `U"x日y"`, regex `日` → columns 1 and 2.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test`

  Expected: compile error, `setSearchOptions` not declared.
- [ ] **Step 3: Implement** the screen, terminal and builder changes; remove the two grid functions; adapt the callers.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[screen],[terminal],[renderbuffer],[vi],[search],[searchquery]"
cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][search],[contour][session][actions]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/RenderBufferBuilder.cpp src/vtbackend/grid/Grid.cpp src/vtbackend/input/vi/ViCommands.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/RenderBufferBuilder.hpp src/vtbackend/screen/RenderBufferBuilder.cpp src/vtbackend/grid/Grid.hpp src/vtbackend/grid/Grid.cpp src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/input/vi/ViCommands_test.cpp src/vtbackend/screen/Terminal_selection_test.cpp src/vtbackend/screen/Screen_test.cpp src/vtbackend/screen/RenderBufferBuilder_test.cpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git add src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/RenderBufferBuilder.hpp src/vtbackend/screen/RenderBufferBuilder.cpp src/vtbackend/grid/Grid.hpp src/vtbackend/grid/Grid.cpp src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/input/vi/ViCommands_test.cpp src/vtbackend/screen/Terminal_selection_test.cpp src/vtbackend/screen/Screen_test.cpp src/vtbackend/screen/RenderBufferBuilder_test.cpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git commit -m "vtbackend: step, search and highlight through the compiled query; setters report std::expected" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The chunked tally walk and the bounded probe

**Files:**
- Create: `src/vtbackend/screen/SearchTallyWalk.hpp`, `SearchTallyWalk.cpp`; list them in `src/vtbackend/CMakeLists.txt`.
- Modify: `src/vtbackend/screen/Screen.{hpp,cpp}` (`tallyMatches` runs a walk to completion; the budgeted reverse search),
  `src/vtbackend/screen/Terminal.{hpp,cpp}` (`:3672-3684` and the new calls).
- Test: create `src/vtbackend/screen/SearchTallyWalk_test.cpp` (add to `vtbackend_test`).

**Interfaces:**
- Consumes: Tasks 1–3; `Grid::stableLineIdOf`, `lineOffsetOf`, `stableRangeFloor`, `stableIdGeneration`
  (`Grid.hpp:766-787`).
- Produces (`screen/SearchTallyWalk.hpp`):
  - `inline constexpr auto SearchWalkStepLines = LineCount(500);`
  - `enum class WalkProgress : uint8_t { Running = 0, Finished };`
  - `class SearchTallyWalk`:
    - `SearchTallyWalk(std::shared_ptr<SearchQuery const> query, Grid const& grid, CellLocation position, size_t limit = DefaultSearchMatchLimit);`
      — records the stable ids of the top row, of the bottom row **at construction** (the walk's end: rows appended
      later are the next walk's), of @p position, and the grid's `stableIdGeneration()`.
    - `[[nodiscard]] std::expected<WalkProgress, SearchPatternError> step(Grid const& grid, LineCount budget = SearchWalkStepLines);`
      — reads whole segments until at least @p budget rows are read or the end is passed (a segment is never split
      across steps); a next row below the stable floor resumes at the floor; a changed `stableIdGeneration()` restarts
      the walk from the top with an empty tally; the tally caps at the limit (`TallyExactness::Capped`) and finishes.
    - `[[nodiscard]] SearchMatchTally tally() const noexcept;` — so far; `ordinal` set once the walk passes a match
      starting at the position.
    - `[[nodiscard]] std::optional<CellLocation> nearestAtOrBefore(Grid const& grid) const;` and
      `[[nodiscard]] std::optional<CellLocation> lastMatch(Grid const& grid) const;` — kept as stable ids, mapped back
      (nullopt once evicted).
    - `[[nodiscard]] LineCount rowsLastStep() const noexcept;` and `[[nodiscard]] std::shared_ptr<SearchQuery const> const& query() const noexcept;`.
- Produces (`Screen`): `[[nodiscard]] std::expected<std::optional<CellLocation>, SearchPatternError> searchReverseFrom(SearchQuery const& query, CellLocation start, LineCount budget) const;`
  — stops after @p budget rows of whole segments.
- Produces (`Terminal`):
  - `struct TerminalSearchTally { SearchTallyWalk walk; PageIndex page; };`
  - `[[nodiscard]] std::optional<TerminalSearchTally> beginSearchTally(CellLocation position, size_t limit = DefaultSearchMatchLimit) const;`
    — nullopt without a query.
  - `[[nodiscard]] std::expected<WalkProgress, SearchPatternError> continueSearchTally(TerminalSearchTally& tally, LineCount budget = SearchWalkStepLines);`
    — restarts the walk on the page on display when `displayedPageIndex()` changed; a `TooComplex` sets `problem` and
    drops the query (R-f).
  - `[[nodiscard]] std::optional<CellLocation> probeSearchReverse(CellLocation from, LineCount budget = SearchWalkStepLines);`
    — `searchReverse(from)` bounded to @p budget (reveals a found match as `searchReverse` does).
  - `tallySearchMatches(position, limit)` keeps its signature: begin, then continue to `Finished`.

- [ ] **Step 1: Write the failing tests** (`[searchwalk]`; `drain(walk, grid, budget)` runs `step` to `Finished` and
  counts steps; `oracle(grid, query, position)` counts `matchesIn` over every segment directly):
  - `searchWalk.chunkedEqualsUnchunked` (§3.8): a 2 000-row history (page 10 × 20, history 3 000), row *i* reading
    `"needle {i}"` when `i % 7 == 0`, else `"hay {i}"`; for literal `needle`, regex `ne+dle [0-9]*5\b`, literal `e`
    (overlapping), each at budgets 1, 7 and 500: `drain(...).tally()` equals `oracle`, and the ordinal at the tenth
    needle row's start is 10.
  - `searchWalk.aStepReadsWholeSegmentsWithinItsBudget`: on the same grid every step's `rowsLastStep() <= 500`; one
    logical line of 600 wrapped rows (page 3 × 100) is read whole in a single step at budget 500, and counted once.
  - `searchWalk.aMatchAcrossTheStepBoundaryCountsOnce` (Review Focus 2): `"nee"` ends row 499 of the walk and `"dle"`
    begins row 500 on its soft wrap; budget 500 → total 1.
  - `searchWalk.finishesWhileOutputStreamsIn` (Review Focus 3): 1 200 rows holding 12 matches, history 10 000; after
    each step write 600 rows of `"hay"`: the walk finishes within `ceil(1200 / 500) + 1` steps with total 12. With
    history 1 500, rows evicted before the walk reached them are skipped: the total equals the matches in the rows
    that survived, and no step fails.
  - `searchWalk.restartsWhenTheGridReflows`: resize the columns between two steps (`stableIdGeneration()` moves): the
    walk restarts and its final tally equals `oracle` on the reflowed grid.
  - `searchWalk.aCatastrophicPatternEndsTheWalk` (Review Focus 1): 10 000 rows of sixty `a` and one `b`, regex
    `(a+)+$`. Under `__GLIBCXX__`: `Finished` with total 0 within `ceil(10000 / 500)` steps. Otherwise (libc++, MSVC):
    the first `step` returns `SearchPatternError::TooComplex`. Through the terminal, `continueSearchTally` then leaves
    `search().problem == TooComplex` and `search().query == nullptr`.
  - `searchWalk.capsAtTheLimit`: limit 3 over 5 matches → `{ .total = 3, .exactness = Capped }`, `Finished`.
  - `searchWalk.remembersTheNearestMatchBehindThePosition`: matches at rows −800, −300 and 5, position row 0:
    `nearestAtOrBefore == row −300`, `lastMatch == row 5`.
  - `probe.stopsAtItsBudget` (R-e): the only match 800 rows above the cursor; `probeSearchReverse(cursor, 500)` is
    nullopt, `probeSearchReverse(cursor, LineCount(1000))` finds it.
  - The `[screen]` `tallyMatches` cases and `MultiPage.DECPCCM_reset.theSearchFindsWhatIsOnThePageOnDisplay` pass
    unedited.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test`

  Expected: compile error, `SearchTallyWalk` not declared.
- [ ] **Step 3: Implement** the walk, the budgeted reverse search and the terminal calls; `Screen::tallyMatches` becomes
  a literal query drained by a walk.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[searchwalk],[searchquery],[screen],[terminal],[search]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/screen/SearchTallyWalk.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/screen/SearchTallyWalk.hpp src/vtbackend/screen/SearchTallyWalk.cpp src/vtbackend/screen/SearchTallyWalk_test.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp
editorconfig-checker src/vtbackend/CMakeLists.txt
git add src/vtbackend/screen/SearchTallyWalk.hpp src/vtbackend/screen/SearchTallyWalk.cpp src/vtbackend/screen/SearchTallyWalk_test.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/CMakeLists.txt
git commit -m "vtbackend: tally search matches in resumable 500-row steps that survive streaming output" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: Search yellow in the Contour schemes

**Files:**
- Modify: `src/vtbackend/core/ColorPalette.cpp:44-133` (`ContourScheme`, `ContourDark`, `ContourLight`,
  `applyContourScheme`).
- Test: `src/vtbackend/core/ColorPalette_test.cpp`.

**Interfaces:**
- `ContourScheme` gains `RGBColor searchMatch;` and `RGBColor searchMatchInk;`, declared after `idleBackground`:
  ContourDark `0xFFD60A_rgb` / `0x1C1E24_rgb` (its background), ContourLight `0xFFD60A_rgb` / `0x2B2E35_rgb` (its
  foreground). `applyContourScheme` sets
  `palette.searchHighlight = { .foreground = CellForegroundColor {}, .foregroundAlpha = 1.0f, .background = scheme.searchMatch, .backgroundAlpha = 0.35f }`
  and `palette.searchHighlightFocused = { .foreground = scheme.searchMatchInk, .foregroundAlpha = 1.0f, .background = scheme.searchMatch, .backgroundAlpha = 0.85f }`
  (the 35 / 85 % of `kit-tokens` "search.match · current").

- [ ] **Step 1: Write the failing tests** (`[ColorPalette]`):
  - `ColorPalette.searchMatchIsYellowInTheContourSchemes`: for `contour-dark` and `contour-light`,
    `searchHighlight.background == 0xFFD60A_rgb` at alpha 0.35 and `searchHighlightFocused.background` the same at
    0.85.
  - `ColorPalette.searchMatchTextReadsAtFourAndAHalfToOne`: composite the yellow over `defaultBackground` at each
    alpha; `contrastRatio(defaultForeground, match composite) >= 4.5` and
    `contrastRatio(searchMatchInk, current composite) >= 4.5` for both schemes.
  - `ColorPalette.otherSchemesKeepTheirSearchColours` (§2.7): `contour-classic` and `monokai` have
    `searchHighlight == ColorPalette {}.searchHighlight`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[ColorPalette]"`

  Expected: FAIL, the background is `CellForegroundColor`.
- [ ] **Step 3: Implement** the two fields and the two assignments.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[ColorPalette]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/core/ColorPalette.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/core/ColorPalette.cpp src/vtbackend/core/ColorPalette_test.cpp
git add src/vtbackend/core/ColorPalette.cpp src/vtbackend/core/ColorPalette_test.cpp
git commit -m "vtbackend: contour-dark and contour-light paint search matches yellow, the current one stronger" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: The find bar's toggles and outcomes, as tables

**Files:**
- Modify: `src/contour/session/SearchStatus.hpp` (`SearchOutcome :19-24`, `MatchPresence :40-44`, `SearchStatus :56-62`,
  `describeSearchCounting :97-108`, after `nextSearchCase :154-165`).
- Test: `src/contour/session/SearchStatus_test.cpp` (in `contour_test`, `src/contour/CMakeLists.txt:1191`).

**Interfaces:**
- Consumes: Task 1 (`SearchOptions`, `SearchPatternError`).
- Produces (namespace `contour::session`):
  - `SearchOutcome` gains `Pending` (a walk is under way and has found nothing yet: neither red nor navigable) and
    `Invalid` (the pattern is refused), after `Matched`.
  - `MatchPresence`'s `None` becomes `Unknown = 0` ("not known before the walk"), keeping `Some`.
    `describeSearchCounting(Unknown)` → `{ Pending, Unavailable, Counting }`.
  - `SearchStatus` gains `std::optional<vtbackend::SearchPatternError> error {};`.
  - `[[nodiscard]] inline SearchStatus describeSearchProgress(vtbackend::SearchMatchTally partial);` — total 0 →
    `{ Pending, Unavailable, Counting }`; else `{ Matched, Available, Counting, partial }`.
  - `[[nodiscard]] inline SearchStatus describeSearchError(vtbackend::SearchPatternError error);` →
    `{ Invalid, Unavailable, Settled, {}, error }`.
  - `enum class SearchToggle : uint8_t { Case = 0, Regex, WholeWord };`, `enum class ToggleState : uint8_t { Off = 0, On };`
  - `struct SearchToggleRow { SearchToggle toggle = SearchToggle::Case; std::string_view objectName; };` and
    `inline constexpr auto SearchToggleRows = std::array { SearchToggleRow { .toggle = SearchToggle::Case, .objectName = "searchBarCaseToggle" }, SearchToggleRow { .toggle = SearchToggle::Regex, .objectName = "searchBarRegexToggle" }, SearchToggleRow { .toggle = SearchToggle::WholeWord, .objectName = "searchBarWordToggle" } };`
  - `struct SearchToggleAffordance { std::string_view glyph; ToggleState state = ToggleState::Off; };`
  - `[[nodiscard]] inline SearchToggleAffordance describeSearchToggle(SearchToggle toggle, vtbackend::SearchOptions options);`
    — Case: `describeSearchCase`'s glyph, `On` when pinned; Regex: `".*"`, `On` under `Regex`; WholeWord: `"ab"`,
    `On` under `WholeWord`.
  - `[[nodiscard]] inline vtbackend::SearchOptions nextSearchOptions(SearchToggle toggle, vtbackend::SearchOptions options);`
    — Case: `nextSearchCase`; the other two flip their own field. A `switch`, so a fourth toggle is a compile error.

- [ ] **Step 1: Write the failing tests** (`[searchstatus]`):
  - `describeSearchCounting`: rewritten for `Unknown` → `Pending`, not navigable; `Some` unchanged.
  - `describeSearchProgress`: `{}` → `Pending`; `{ .total = 5, .ordinal = 2 }` → `Matched`, `Available`, `Counting`,
    the tally kept.
  - `describeSearchError`: each `SearchPatternError` → `Invalid`, `Unavailable`, `error` equal to it.
  - `searchToggles.rowsAreInEnumeratorOrder`: `SearchToggleRows[i].toggle == SearchToggle(i)` over
    `iota(0, SearchToggleRows.size())`, and the object names are distinct.
  - `searchToggles.eachFlipsOnlyItsOwnField`: for every row, `nextSearchOptions(row.toggle, {})` differs from `{}` in
    exactly that toggle's field; twice for Regex and WholeWord is the identity; three times for Case is.
  - `searchToggles.glyphsAndState`: `describeSearchToggle(Regex, { .syntax = Regex })` is `{ ".*", On }`;
    `describeSearchToggle(WholeWord, {})` is `{ "ab", Off }`; Case keeps `describeSearchCase`'s answers.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test && out/appleclang-debug/src/contour/contour_test "[searchstatus]"`

  Expected: compile error, `SearchToggle` not declared.
- [ ] **Step 3: Implement** the additions.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/contour/contour_test "[searchstatus]"`. Expected: PASS.
- [ ] **Step 5: Format and commit** (`SearchStatus.hpp` is header-only; clang-tidy runs on `SearchStatus_test.cpp`).

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/SearchStatus_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/SearchStatus.hpp src/contour/session/SearchStatus_test.cpp
git add src/contour/session/SearchStatus.hpp src/contour/session/SearchStatus_test.cpp
git commit -m "contour: the find bar's toggles, pending and invalid outcomes as pure tables" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The session walks the count in steps, probes per keystroke, and publishes the toggles

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp:238-247`, `:268-339`, `:1779-1787`;
  `TerminalSession.cpp:415-424` (the debounce), `:2457-2758` (the find bar block), `:2760-…` (`onScrollOffsetChanged`),
  `:6063`.
- Test: `src/contour/session/TerminalSession_test.cpp` (`:4009-4175`, `:5877-…` and new cases).

**Interfaces:**
- Consumes: Tasks 3, 4, 6.
- Produces (`TerminalSession`):
  - Removed: `searchCaseGlyph`, `searchCaseTooltip`, `searchCasePinned`, `cycleSearchCaseSensitivity()`.
  - `Q_PROPERTY(QVariantList searchToggles READ searchToggles NOTIFY searchStateChanged)` — one `QVariantMap` per
    `SearchToggleRows` row: `toggle` (int), `glyph`, `checked` (bool, QML carve-out), `tooltip`, `accessibleName`
    ("Match case", "Regular expression", "Whole word"), `objectName`. Tooltips: the case ones as today
    (`renderSearchCaseTooltip`), `tr("Regular expression: on")`/`tr("Regular expression: off")`,
    `tr("Whole word: on")`/`tr("Whole word: off")`.
  - `Q_INVOKABLE void toggleSearchOption(int toggle);` — out-of-range ints do nothing; `setSearchOptions(nextSearchOptions(…))`,
    then the probe and the walk as for a new pattern.
  - `Q_PROPERTY(QVariantMap searchFocusedRows READ searchFocusedRows NOTIFY searchFocusedRowsChanged)` — `{ first, last }`
    viewport rows of `focusedSearchMatch()` (`line + scrollOffset`), empty when none or off screen; recomputed by
    `refreshSearchStatusLocked` and `onScrollOffsetChanged`.
  - `searchHasMatches()` is `outcome != NoMatch && outcome != Invalid`.
  - `void runSearchTallyStep();` (public, so a test can drive it without an event loop, as `refreshSearchStatus()`)
    and `[[nodiscard]] bool searchTallyRunning() const noexcept;`.
  - Private: `void startSearchTallyLocked();`, `std::optional<vtbackend::TerminalSearchTally> _searchWalk;`,
    `uint64_t _searchWalkTicket = 0;`, `vtbackend::CellLocation _searchWalkOrigin {};`,
    `enum class PendingSearchJump : uint8_t { None = 0, ToNearest };` and `PendingSearchJump _pendingSearchJump = PendingSearchJump::None;`.
- Behaviour:
  - `setSearchPattern`: under the lock, `setNewSearchTerm`; an error → `describeSearchError`. Otherwise
    `probeSearchReverse(cursor)`: a match moves the vi cursor (`_pendingSearchJump = None`, `MatchPresence::Some`), none
    leaves it (`ToNearest`, `Unknown`). Then the debounce (typing), as today.
  - `refreshSearchStatusLocked` no longer tallies: with a `problem`, `describeSearchError`; otherwise
    `startSearchTallyLocked()` (bar open only): `beginSearchTally(cursor)`, `++_searchWalkTicket`, and one posted
    `runSearchTallyStep` guarded by the ticket and `_isSearchBarOpen`.
  - `runSearchTallyStep`: lock; `continueSearchTally`; read the tally; unlock; `Running` → `describeSearchProgress`
    and post the next step; `Finished` → `describeSearch`, and with `ToNearest` and the cursor still at
    `_searchWalkOrigin`, move it to `nearestAtOrBefore` else `lastMatch` (revealing it); an error →
    `describeSearchError`. Each step emits `searchStateChanged`.
  - `renderSearchSummary`: `Pending` → `tr("…")`; `Invalid` → `tr("Pattern too complex")` for `TooComplex`, else
    `tr("Invalid pattern")`; `Counting` with a tally → `tr("%1 of %2…")` with an ordinal, else
    `tr("%n match(es)…", nullptr, n)`.
  - `:6063`: `setSearchOptions` with the profile's case policy and the other two options kept.

- [ ] **Step 1: Write the failing tests** (`[contour][session][search]`; `drainSearchTally(session)` calls
  `runSearchTallyStep()` while `searchTallyRunning()`):
  - The existing find-bar cases (`:4009`, `:4142`, `:5877`) drain the walk where they read a count; their assertions
    stand.
  - "the count is walked in steps and the lock is free between them" (Review Focus 3): 2 000 rows, every fifth holding
    `needle`; `searchBarOpened()`, `setSearchPattern("needle")`, `refreshSearchStatus()`, one `runSearchTallyStep()`:
    `searchTallyRunning()` is true and the summary ends in `…`; `std::async` taking `std::scoped_lock { terminal() }`
    is ready within 1 s; `terminal().writeToScreen("hay\r\n")` between two steps succeeds; after draining, the summary
    equals what `terminal().tallySearchMatches(cursor)` gives, rendered.
  - "an invalid regex says so in the count slot": `toggleSearchOption(1)`, `setSearchPattern("a(")` → summary
    `"Invalid pattern"`, `searchHasMatches()` and `searchNavigable()` false, nothing tallied; `setSearchPattern("a")`
    recovers.
  - "a catastrophic pattern ends in a word, not a hang" (Review Focus 1): 10 000 rows of sixty `a` and a `b`, regex
    `(a+)+$`; draining takes at most `ceil(10000 / 500)` steps and ends with `"Pattern too complex"` (libc++, MSVC) or
    `"No results"` (`__GLIBCXX__`).
  - "a match far above the cursor is reached after the bounded probe" (R-e): the only `needle` 2 000 rows above the
    cursor: right after `setSearchPattern`, `searchHasMatches()` is true, `searchNavigable()` false and the cursor
    unmoved; after draining, the cursor is on that match and the summary is `"1 of 1"`.
  - "toggles cycle and re-run the search": `searchToggles` has `SearchToggleRows.size()` maps with the table's object
    names; `toggleSearchOption(2)` turns `search().options.wordBoundary` to `WholeWord` and that map's `checked` true;
    `toggleSearchOption(99)` changes nothing.
  - "searchFocusedRows names the current match's rows" (Review Focus 2): a match wrapped over viewport rows 3 and 4 →
    `{ first: 3, last: 4 }`; scrolled off screen → empty, with `searchFocusedRowsChanged` emitted.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][search]"`

  Expected: compile error, `runSearchTallyStep` not declared.
- [ ] **Step 3: Implement** the properties, the stepped tally, the probe and the jump.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][session][search],[contour][session][actions],[contour][session]"`

  Expected: PASS. Also once under TSan if the tree exists (`ctest --preset=clang-tsan -R contour_gui_test`), since the
  step releases and retakes the parser thread's lock.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSession.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git commit -m "contour: walk the find bar's count in steps off the lock, bound the keystroke probe, publish the toggles" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Find Next, Find Previous and Use Selection for Find, with their chords per platform

**Files:**
- Modify: `src/contour/config/Actions.hpp` (`:60-61`, `:97`, the variant `:185-222`, documentation `:456-545`, the
  catalogue `:782-830`): `SearchSelection`.
- Modify: `src/contour/config/ConfigDocumentation.hpp:1378` (the action list).
- Modify: `src/contour/config/Config.hpp:1283-1712` (`makeDefaultInputMappings`): the macOS find chords.
- Modify: `src/contour/command/MenuBarTable.cpp:211-214` (the Edit menu row).
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` (`:1125-1126`, `:1160`; `.cpp:2610-2675`, `:4443-4489`):
  the new handler; the two focus actions wrap and open the bar with no pattern.
- Test: `src/contour/config/Config_test.cpp`, `src/contour/config/Actions_test.cpp`,
  `src/contour/command/MenuBarTable_test.cpp`, `src/contour/session/TerminalSession_test.cpp`.

**Interfaces:**
- Consumes: Tasks 1 (`escapeSearchRegex`), 3, 7.
- Produces:
  - `actions::SearchSelection {}`, catalogue name `"SearchSelection"`, documentation "Searches for the selected text:
    its first line becomes the find bar's pattern, escaped when the bar searches by regular expression."
  - `TerminalSession::operator()(actions::SearchSelection)` (R-k): returns true; posts `searchBarRequested`.
  - `FocusNextSearchMatch`/`FocusPreviousSearchMatch` → `stepSearch(Top|Bottom)`; their old bodies become the private
    `[[nodiscard]] bool focusAdjacentSearchMatch(SearchWrapEdge edge);` that `stepSearch` calls (no recursion). With
    no pattern they post `searchBarRequested` and return true (Review Focus 5).
  - `inline std::array<CharInputMapping, 4> macFindChords()` in `Config.hpp`, appended by `makeDefaultInputMappings`
    for every edition under `#ifdef __APPLE__`, each with `.modes {}`: Super `'F'` `SearchReverse`, Super `'G'`
    `FocusNextSearchMatch`, Shift+Super `'G'` `FocusPreviousSearchMatch`, Super `'E'` `SearchSelection` (R-j).
  - `MenuBarTable` Edit: `Table::command(SearchSelection {}, "Use Selection for Find").enabledWhen(hasSelection)` after
    "Find Previous".

- [ ] **Step 1: Write the failing tests:**
  - `Config_test.cpp` (`[config][input-mapping]`): "the find chords follow the platform": on `__APPLE__`, both
    `defaultInputMappings(DefaultsEdition::Classic)` and `(Native2026)` hold the four `macFindChords()` rows with empty
    modes; elsewhere no default mapping carries `Modifier::Super` with those inputs; Ctrl+Shift+F and the two F3 rows
    are unchanged.
  - `Actions_test.cpp` (`[actions]`): `actions::fromString("SearchSelection")` gives `SearchSelection`, and its
    catalogue entry has documentation.
  - `MenuBarTable_test.cpp` (`[menubar]`): `menuBar.everyCatalogueActionIsReachable` passes with the new row; the Edit
    menu's row after "Find Previous" is "Use Selection for Find", disabled without a selection.
  - `TerminalSession_test.cpp` (`[contour][session][actions]`):
    - "Find Next with no search opens the find bar" (Review Focus 5): no pattern; `FocusNextSearchMatch` returns true;
      after `processEvents`, a `QSignalSpy` on `searchBarRequested` has 1; the mock PTY received no bytes; the vi
      cursor is unchanged. The same for `FocusPreviousSearchMatch`.
    - "Find Next wraps": two matches, the cursor on the second; `FocusNextSearchMatch` lands on the first, and with the
      bar closed `searchBarRequested` is not emitted.
    - "Use Selection for Find takes the first line, escaped under regex": select `foo.bar` (then a second line);
      `toggleSearchOption(1)`; the action makes `search().pattern == U"foo\\.bar"` and requests the bar; without a
      selection the pattern is unchanged and nothing is requested.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `actions::SearchSelection` not declared.
- [ ] **Step 3: Implement** the action, its handler, the wrap, the chords and the menu row.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[config][input-mapping],[actions],[menubar],[command]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][actions],[contour][session][search],[windowmenus]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSession.cpp src/contour/command/MenuBarTable.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/Actions.hpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config.hpp src/contour/command/MenuBarTable.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/config/Config_test.cpp src/contour/config/Actions_test.cpp src/contour/command/MenuBarTable_test.cpp src/contour/session/TerminalSession_test.cpp
git add src/contour/config/Actions.hpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config.hpp src/contour/command/MenuBarTable.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/config/Config_test.cpp src/contour/config/Actions_test.cpp src/contour/command/MenuBarTable_test.cpp src/contour/session/TerminalSession_test.cpp
git commit -m "contour: Find Next/Previous wrap and open the bar, Use Selection for Find, and the macOS find chords" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: `FindBarPlacement.hpp`, the find bar's tokens and the provider's answer

> **Depends on Steps 1c and 2:** 1c's colour sources (`detail::label(level)`, `SchemeSlot::SelectionText`) and roles;
> Step 2's `ChromeTokenRow` fields and `paneHeader(...)` invokable, whose shape `findBarPlacement` copies.

**Files:**
- Create: `src/contour/window/FindBarPlacement.hpp`; test `src/contour/window/FindBarPlacement_test.cpp` (add to
  `contour_test`, `src/contour/CMakeLists.txt:1153-1202`).
- Modify: `src/contour/config/ChromeTokenGroups.hpp` (`SizeTokens :114-131`, `SpaceTokens :134-141`, `RadiusTokens
  :145-153`, `LayoutTokens :227-233`, `ChromeColorRole :50-57` and its count, the four rows `:318-440`).
- Modify: `src/contour/config/UiStyle.hpp:218-225`, `:356-362`, `:431-437` (glyphs).
- Modify: `src/contour/window/ChromeTokenObjects.{hpp,cpp}`, `src/contour/window/UiStyleProvider.{hpp,cpp}`.
- Modify: `src/contour/display/TerminalDisplay.{hpp,cpp}` (`rowBand`, beside `gridMetrics()` `:553-556`).
- Test: `src/contour/config/ChromeTokenGroups_test.cpp`, `src/contour/window/UiStyleProvider_test.cpp`.

**Interfaces:**
- Produces (`config/ChromeTokenGroups.hpp`): `enum class FindBarHost : uint8_t { Overlay = 0, PaneHeaderRow };`;
  `SizeTokens` gains `findBar`, `findBarWidth`, `findToggleWidth`, `findToggleHeight`, `findButton`, `findIcon`;
  `SpaceTokens` gains `findBarInset`; `RadiusTokens` gains `findBar`; `LayoutTokens` gains
  `FindBarHost findBar = FindBarHost::Overlay;` — values in Global Constraints. `ChromeColorRole` appends
  `FindToggleOnFill`, `FindToggleOnText`, `FindToggleText`, `FindCountText` after the last role at execution, the count
  derived from `FindCountText`. Native rows: `Status`/`Running` at 22 %, `Status`/`Running` at 100 %, `label(2)`,
  `label(2)`; Terminal row: `Scheme Selection`, `Scheme SelectionText`, `Scheme Secondary`, `Scheme Foreground`.
- Produces (`config/UiStyle.hpp` glyphs): `findPreviousGlyph` (`"▴"` / `"↑"`), `findNextGlyph` (`"▾"` / `"↓"`),
  `toggleOpenGlyph` / `toggleCloseGlyph` (`""` / `"["`, `"]"`).
- Produces (`window/FindBarPlacement.hpp`, namespace `contour::window`, Qt-free):
  - `enum class FindBarAnchor : uint8_t { TopRight = 0, BottomRight, HeaderRow };`
  - `enum class PaneHeaderPresence : uint8_t { Absent = 0, Present };`
  - `struct RowBand { double top = 0; double bottom = 0; bool operator==(RowBand const&) const = default; };`
  - `struct FindBarInputs { config::FindBarHost host = config::FindBarHost::Overlay; PaneHeaderPresence header = PaneHeaderPresence::Absent; double paneWidth = 0; double paneHeight = 0; double barWidth = 0; double barHeight = 0; double inset = 0; std::optional<RowBand> currentMatch {}; };`
  - `struct FindBarPlacement { FindBarAnchor anchor = FindBarAnchor::TopRight; double x = 0; double y = 0; double width = 0; bool operator==(FindBarPlacement const&) const = default; };`
  - `[[nodiscard]] constexpr FindBarPlacement placeFindBar(FindBarInputs const& in) noexcept;` — rules, in order:
    1. `PaneHeaderRow` and `Present` → `{ HeaderRow, 0, 0, paneWidth }`;
    2. `width = clamp(paneWidth − 2·inset, 0, barWidth)`, `x = paneWidth − inset − width`, top `y = inset`;
    3. when `currentMatch` overlaps `[inset, inset + barHeight)` and the bottom position
       `yb = paneHeight − inset − barHeight` is below the top one and does not overlap it → `{ BottomRight, x, yb, width }`;
    4. otherwise `{ TopRight, x, inset, width }`.
  - `[[nodiscard]] constexpr RowBand rowBandOf(int first, int last, double marginTop, double rowHeight) noexcept;`
    → `{ marginTop + first·rowHeight, marginTop + (last + 1)·rowHeight }`.
  - `[[nodiscard]] constexpr std::string_view findBarAnchorName(FindBarAnchor anchor) noexcept;` → `"topRight"`,
    `"bottomRight"`, `"headerRow"`.
- Produces (QML-facing):
  - `chromeStyle.size.findBar`, `findBarWidth`, `findToggleWidth`, `findToggleHeight`, `findButton`, `findIcon`,
    `chromeStyle.space.findBarInset`, `chromeStyle.radii.findBar`, `chromeStyle.colors.findToggleOnFill`,
    `findToggleOnText`, `findToggleText`, `findCountText`, and the four glyphs — scaled by the units as every size is.
  - `UiStyleProvider`: `Q_INVOKABLE QVariantMap findBarPlacement(int header, qreal paneWidth, qreal paneHeight, qreal matchTop, qreal matchBottom) const;`
    — runs `placeFindBar` with the row's tokens; `matchBottom <= matchTop` means no current match; returns
    `{ anchor: findBarAnchorName(…), x, y, width }`.
  - `TerminalDisplay`: `Q_INVOKABLE [[nodiscard]] QVariantMap rowBand(int first, int last) const;` — `rowBandOf` with the
    page margin and the cell height from `gridMetrics()`, in item coordinates; `{ top, bottom }`.

- [ ] **Step 1: Write the failing tests:**
  - `FindBarPlacement_test.cpp` (`[findbar]`), with `base = { .paneWidth = 800, .paneHeight = 400, .barWidth = 380, .barHeight = 32, .inset = 8 }`:
    - `findBar.topRightOfAPane`: `base` → `{ TopRight, 412, 8, 380 }`.
    - `findBar.underTheHeaderIsTheTerminalsTop` (R-i): `base` with `.header = Present` → the same; the card's 22-pt
      header above the terminal makes it 30 in card coordinates, the canvas's value.
    - `findBar.movesBelowWhenTheCurrentMatchIsUnderIt`: `.currentMatch = RowBand { 10, 30 }` → `{ BottomRight, 412, 360, 380 }`;
      `{ 50, 70 }` → `TopRight`; `.paneHeight = 60` with `{ 10, 30 }` → `TopRight` (nowhere better).
    - `findBar.aNarrowPaneShrinksTheBar`: `.paneWidth = 200` → `{ TopRight, 8, 8, 184 }`; `.paneWidth = 10` → width 0,
      never negative.
    - `findBar.theTerminalRowUsesTheHeaderRow`: `.host = PaneHeaderRow, .header = Present` → `{ HeaderRow, 0, 0, 800 }`;
      `.header = Absent` → `TopRight` (owner decision 2).
    - `findBar.rowBand`: `rowBandOf(2, 3, 6, 20) == RowBand { 46, 86 }`.
    - `findBar.names`: every `FindBarAnchor` has a distinct non-empty name.
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`, `everyRowMatchesTheDesignKit`): the new tokens per row from Global
    Constraints, the four colour sources per row, `layout.findBar`.
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`): MacOS publishes `size.findBar == 32`, `radii.findBar == 8`;
    `findBarPlacement(0, 800, 400, 10, 30)` is `{ "bottomRight", 412, 360, 380 }`; under Terminal,
    `size.findBar == cellHeight`, `findBarWidth == 48 * cellWidth`, `findBarPlacement(1, …)["anchor"] == "headerRow"`,
    and the glyphs are `↑`, `↓`, `[`, `]`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `FindBarPlacement.hpp` not found.
- [ ] **Step 3: Implement** the header, the tokens, the glyphs, the provider's invokable and `rowBand`.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[findbar],[chrometokens]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/FindBarPlacement_test.cpp src/contour/window/UiStyleProvider.cpp src/contour/window/ChromeTokenObjects.cpp src/contour/display/TerminalDisplay.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/FindBarPlacement.hpp src/contour/window/FindBarPlacement_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/display/TerminalDisplay.hpp src/contour/display/TerminalDisplay.cpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/UiStyleProvider_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/window/FindBarPlacement.hpp src/contour/window/FindBarPlacement_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/display/TerminalDisplay.hpp src/contour/display/TerminalDisplay.cpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: decide where the find bar goes from one table, with its tokens on every row" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: The find bar, native rows and the Terminal row's floating bar

**Files:**
- Modify: `src/contour/qml/SearchBar.qml` (rewritten layout; its focus, hand-over and seeding logic `:83-188` stays).
- Modify: `src/contour/qml/PopupSurface.qml:75`, `:88` (the shadow corner follows `root.radius`, not
  `chromeStyle.radius`, so an override of `radius` keeps the shadow true; the default is unchanged).
- Modify: `src/contour/qml/SessionChrome.qml:360-367` (`SearchBar { header: 0 }`, wired properly in Task 11).
- Modify: `src/contour/test/QmlMocks.hpp` — `MockSession` (`:22`) gains the search surface that `MockSearchSession`
  (`QmlComponents_test.cpp:5576-5670`) has today, updated to Task 7's (`searchToggles`, `toggleSearchOption`,
  `searchFocusedRows`, the pattern, summary and flags, `searchBarOpened/Closed`, `searchNext/Previous`, setters for
  tests); `MockSearchSession` goes. `StubContourTerminal` (`:324`) gains `rowBand(first, last)` (`rowBandOf` at
  margin 0, row height 20).
- Modify: `scripts/qml-size-literal-baseline.txt:13` (the `SearchBar.qml` line goes: 2 → 0).
- Test: `src/contour/test/QmlComponents_test.cpp` (`:5671-5745` host, `:5746`, `:5939`, new cases),
  `src/contour/test/Accessibility_qml_test.cpp`.

**Interfaces:**
- Consumes: Tasks 7 (properties), 9 (tokens, `findBarPlacement`, `rowBand`).
- Produces (QML, `SearchBar.qml`, `objectName: "searchBar"`):
  - `property int header: 0` (a `PaneHeaderPresence`), `property Item headerSlot: null` (Task 11).
  - `readonly property var placement: chromeStyle.findBarPlacement(root.header, displayItem.width, displayItem.height, band.top, band.bottom)`
    where `band` is `displayItem.rowBand(rows.first, rows.last)` when `session.searchFocusedRows` has `first`, else
    `{ top: 0, bottom: 0 }`. `x`, `y`, `width` from it; `height: chromeStyle.size.findBar`.
  - `background: PopupSurface { radius: chromeStyle.radii.findBar }`, with `elevation.e2` (R-n).
  - `contentItem: Row` (spacing `space.s2`, leading `space.s5`, trailing `space.s2`):
    `searchBarIcon` (a `Shape` magnifier, `size.findIcon` square, `colors.findToggleText`; not visible at 0),
    `searchBarField` (`TextField`, flexible width, placeholder `qsTr("Find")`, `type.body`, its `palette.highlight` the
    error colour on a miss or an invalid pattern, as today), `searchBarCount` (`Label`, `type.footnote`,
    `font.features: { "tnum": 1 }`, `colors.findCountText`, or `chromeStyle.errorColor` when `!searchHasMatches`),
    a `Repeater` over `session.searchToggles` of `ToolButton`s (`objectName: modelData.objectName`,
    `text: toggleOpenGlyph + modelData.glyph + toggleCloseGlyph`, `checked: modelData.checked`, not `checkable`, as today
    and for the same reason, `size.findToggleWidth` × `findToggleHeight`, background `radii.control` in
    `colors.findToggleOnFill` when checked, `tabHoverFill` on hover, `"transparent"` otherwise, glyph in
    `findToggleOnText`/`findToggleText`, `onClicked: session.toggleSearchOption(modelData.toggle)`,
    `ToolTip.text: modelData.tooltip`), then `searchBarPrevious`, `searchBarNext` (`findPreviousGlyph`, `findNextGlyph`)
    and `searchBarClose` (`closeGlyph`), each `size.findButton` square.
  - Accessibility: field `Accessible.EditableText` "Find"; count `StaticText` (its text); each toggle `Button`,
    `Accessible.name: modelData.accessibleName`, `Accessible.checkable: true`, `Accessible.checked: modelData.checked`;
    the three buttons "Previous match", "Next match", "Close find".
- **TUI row:** the same file under ContourTui: one cell row, radius 0, the hard one-cell shadow from `e2`, toggles
  `[Aa] [.*] [ab]`, `↑ ↓ ×`, the field on `Bar`, the floating 48-cell bar for a pane without a header.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][search]`, MacOS row unless stated; the two existing cases
  use `MockSession` and find the case toggle as `searchBarCaseToggle`, their assertions otherwise unchanged):
  - "the find bar sits at its pane's top right with the design's extents": an 800 × 400 stub terminal: the bar's
    `x == 412`, `y == 8`, `width == 380`, `height == 32`; its background `radius == 8`; each toggle 24 × 22; the
    count's `font.features.tnum == 1`; no QML warning (`QmlMessageCapture`).
  - "the find bar moves to the bottom when the current match is under it": `searchFocusedRows = { first: 0, last: 0 }` →
    `y == 360`; `{ first: 10, last: 10 }` → `y == 8`.
  - "the toggles render the session's table and light the active one": three buttons named from `SearchToggleRows`, in
    order, texts `Aa`, `.*`, `ab`; the regex one checked → its background colour equals `colors.findToggleOnFill`;
    clicking `searchBarWordToggle` records `toggleSearchOption(2)`.
  - "a miss and an invalid pattern both tint the field and say so": summary `"No results"` and `"Invalid pattern"` with
    `searchHasMatches` false → the count's colour and the field's `palette.highlight` equal `chromeStyle.errorColor`.
  - "Escape closes the bar and gives the terminal the keyboard back": as the existing case.
  - "the Terminal row draws the bar in cells" (`ContourTui`, `ChromeSetup { .row = Terminal }`): height `cellHeight`,
    radius 0, toggle texts `[Aa]`, `[.*]`, `[ab]`, buttons `↑`, `↓`, `×`, width `48 * cellWidth`.
  - `Accessibility_qml_test.cpp` (`[contour][gui][qml][a11y]`): a `SearchBar.qml` case: the field, the count, the three
    toggles (role `Button`, names "Match case", "Regular expression", "Whole word", checkable) and the three buttons each
    declare a role and a name.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][search]"`

  Expected: FAIL, the bar is 26 cells wide and has one toggle.
- [ ] **Step 3: Implement** the bar, the mock's surface and the `PopupSurface` corner.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][search],[contour][gui][qml][a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][search]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS; `SearchBar.qml` at size baseline 0, no colour literal.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlMocks.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/SearchBar.qml src/contour/qml/PopupSurface.qml src/contour/qml/SessionChrome.qml scripts/qml-size-literal-baseline.txt
git add src/contour/qml/SearchBar.qml src/contour/qml/PopupSurface.qml src/contour/qml/SessionChrome.qml scripts/qml-size-literal-baseline.txt src/contour/test/QmlMocks.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
git commit -m "contour: the find bar draws the canvas's layout from tokens, three toggles, a tabular count, and moves off the match" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: The Terminal row's find bar lives in the pane header row

> **Depends on Step 2:** `PaneCard.qml`, `PaneHeader.qml` (Step 2 Task 9) and the card's leaf wiring.

**Files:**
- Modify: `src/contour/qml/PaneHeader.qml` (a find slot), `src/contour/qml/PaneCard.qml` (passes it down),
  `src/contour/qml/TerminalPane.qml` (forwards `headerFindSlot` and `headerPresent` to `SessionChrome`),
  `src/contour/qml/SessionChrome.qml:360-367` (passes them to `SearchBar`), `src/contour/qml/SearchBar.qml`.
- Test: `src/contour/test/QmlComponents_test.cpp` (`[contour][gui][qml][panecard]`).

**Interfaces:**
- Consumes: Task 10; Step 2's `PaneHeader` children (`paneHeaderName`, `paneHeaderCwd`, `paneHeaderActions`).
- Produces (QML):
  - `PaneHeader.qml`: `readonly property alias findSlot: findSlotItem`, an `Item` (`objectName: "paneHeaderFindSlot"`)
    from after `paneHeaderName` to the trailing edge; `property bool findActive: false` (QML carve-out): while true,
    `paneHeaderCwd` and `paneHeaderActions` are not visible and the name stays.
  - `PaneCard.qml` → `TerminalPane { headerFindSlot: paneHeader.findSlot; headerPresent: paneHeader.visible ? 1 : 0 }`;
    `TerminalPane` and `SessionChrome` forward `property Item headerFindSlot` and `property int headerPresent`.
  - `SearchBar.qml`: `header: headerPresent`; when `placement.anchor === "headerRow"`, `parent: headerSlot`, `x: 0`,
    `y: 0`, `width: headerSlot.width`, `height: headerSlot.height`, no background (the header's fill), and the slot's
    header `findActive` follows the bar's `visible`.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][panecard]`):
  - "the Terminal row's find bar takes the header row and no grid cells" (`ContourTui`, `ChromeSetup { .row = Terminal }`,
    a two-pane split): opening pane 0's bar parents it to that card's `paneHeaderFindSlot`; its height is `cellHeight`;
    the pane's `TerminalPane` `y` and `height` equal their values with the bar closed; `paneHeaderCwd` and
    `paneHeaderActions` are invisible and `paneHeaderName` visible while it is open, all back after `close()`; pane 1's
    header is unchanged.
  - "a Terminal-row pane without a header gets the floating bar" (one pane): the bar's parent is the terminal item, at
    the top right, one cell high.
  - "the native bar sits under the header": MacOS row, a split: the bar's parent is the terminal item and its `y` in
    the card's coordinates is `size.paneHeader + space.findBarInset` (30).
- [ ] **Step 2: Run them and see them fail.**

  Run: `CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][panecard]"`

  Expected: FAIL, `paneHeaderFindSlot` not found.
- [ ] **Step 3: Implement** the slot and the forwarding.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][panecard],[contour][gui][qml][search],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][panecard],[contour][gui][qml][search]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/PaneHeader.qml src/contour/qml/PaneCard.qml src/contour/qml/TerminalPane.qml src/contour/qml/SessionChrome.qml src/contour/qml/SearchBar.qml
git add src/contour/qml/PaneHeader.qml src/contour/qml/PaneCard.qml src/contour/qml/TerminalPane.qml src/contour/qml/SessionChrome.qml src/contour/qml/SearchBar.qml src/contour/test/QmlComponents_test.cpp
git commit -m "contour: the Terminal row's find bar takes the pane header row, never a grid row" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: Snapshot rows, and the Terminal row's pass

> **Depends on Steps 4a, 1c, 1d and 2:** every existing row names their fields (`SnapshotSurface`, `.drift`,
> `.frame`, `.mode`, `.density`, `.paneEnd`); the new rows name them too, at their defaults.

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` (`SnapshotScript :35-48`, `ScriptAction :66-74`, `SnapshotScriptRow
  :97-107`, `SnapshotScripts`), `src/contour/test/ChromeSnapshotScene.hpp` (the mock session's search state, opening
  the bar).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Produces: `enum class SnapshotFind : uint8_t { None = 0, Matches, Invalid };`, `SnapshotScriptRow` gains
  `SnapshotFind find = SnapshotFind::None;`; `ScriptAction` gains `OpenFindBar`. Rows appended in this order:
  `FindBar` (`"find-bar"`, one tab, `SingleLeaf`, `find = Matches`), `FindBarSplit` (`"find-bar-split"`, `SplitThree`,
  `Matches`), `FindBarInvalid` (`"find-bar-invalid"`, `SingleLeaf`, `Invalid`), each `.action = OpenFindBar`. The scene:
  `Matches` → pattern `"config"`, summary `"2 of 5"`, the regex toggle checked; `Invalid` → pattern `"a("`, summary
  `"Invalid pattern"`, `searchHasMatches` false. Existing rows name `.find = SnapshotFind::None`.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): `scriptRow(SnapshotScript::FindBarSplit).panes == PaneShape::SplitThree`,
    `scriptRow(SnapshotScript::FindBarInvalid).find == SnapshotFind::Invalid`; every row names a `find`; the
    enumerator-order `static_assert` holds; counts from `SnapshotScripts.size()`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): each new script builds on the MacOS and Terminal rows
    without a QML warning; `searchBar` is opened; `find-bar-invalid` finds `searchBarCount.text == "Invalid pattern"`;
    on the Terminal row `find-bar-split` finds the bar inside `paneHeaderFindSlot`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `SnapshotScript::FindBar`.
- [ ] **Step 3: Implement** the rows and the scene's handling.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git add src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git commit -m "contour: snapshot the find bar on a pane, under a split's header and with an invalid pattern" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, so the plan's own
  commit is left out. Apply its findings, re-run the filters of every task it touches, and commit once with the
  Signed-off-by line, staging only the files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[searchquery],[searchwalk],[search],[screen],[terminal],[renderbuffer],[vi],[links],[ColorPalette]"
out/appleclang-debug/src/contour/contour_test "[searchstatus],[findbar],[chrometokens],[config][input-mapping],[actions],[menubar],[command],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session][search],[contour][session][actions],[contour][session],[contour][uistyle],[windowmenus],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][search],[contour][gui][qml][panecard],[contour][gui][qml][snapshot]"
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
  `ChromeSnapshotPlan.hpp`. Look at `find-bar.*` (380 × 32 at the top right, three toggles with the regex one tinted,
  "2 of 5"), `find-bar-split.*` (under the active card's header; on the Terminal row inside the header row) and
  `find-bar-invalid.*` (the red field and "Invalid pattern"); `split-three.*` and `single-tab.*` look as they did after
  Step 2.
- [ ] **Step 4: Owner manual checks.** Batched per phase (ruling): list them in the ledger for the owner; do not wait on
  them. On this Mac, `out/appleclang-debug/src/contour/contour.app`:
  - ⌘F opens the bar at the pane's top right; typing `config` lights matches yellow, the current one stronger, and
    reads "2 of 5" in steady digits.
  - `.*` on, `conf.g` matches; `a(` reads "Invalid pattern" in red; `ab` on, `con` stops matching `config`.
  - `seq 1 200000` then search `9999$` with `.*` on: the bar stays responsive while the count fills in; ⎋ closes at
    once. `(a+)+$` over a scrollback of `python3 -c 'print(("a"*60+"b\n")*10000)'`: "Pattern too complex", no beach ball.
  - `while :; do date; done` running, search `:`: the count settles, the terminal keeps scrolling.
  - `ls` a directory holding `café` (NFD on APFS) and search `café`: found.
  - ⌘G with no search opens the bar; with a search and the bar closed, ⌘G / ⇧⌘G step and wrap; select a word and ⌘E
    fills the bar with it.
  - Split the tab: the bar sits under the active card's header; scrolling the current match under the bar moves it to
    the bottom right.
  - `ui_style: terminal`: in a split the bar is the header row (`[Aa] [.*] [ab] ↑ ↓ ×`), the grid keeps its rows; a
    single pane gets the one-row floating bar.
  - VoiceOver reads "Match case, toggle button", "Regular expression", "Whole word", and the count.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Adds regular-expression and whole-word search to the find bar, beside match case; an invalid pattern says so in
    the count."
  - "Counts search matches in steps, so a long scrollback or a heavy pattern no longer freezes the window, and the count
    fills in as it goes."
  - "Searches match across wrapped lines, wide characters and accented letters however they were composed."
  - "Paints search matches yellow in contour-dark and contour-light, the current one stronger."
  - "Places the find bar at the active pane's top right under its header, moving it out of the way of the current
    match; under `ui_style: terminal` it takes the pane header row."
  - "Adds ⌘F, ⌘G, ⇧⌘G and ⌘E (Use Selection for Find) on macOS; Find Next and Find Previous wrap and open the bar when
    nothing is being searched for. Adds the SearchSelection action."
- [ ] **Step 6:** Mark Step 3b done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-3b.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** a keystroke reads at most 500 rows under the lock instead of the scrollback; the count is one
    500-row step per event-loop turn; a regex compiles once per pattern or toggle; the highlight builds one subject per
    visible segment per frame (a regex runs only over visible rows there). Callgrind on `bench-headless` with a pattern
    set, before and after, recorded in the ledger.
  - **Risk:** medium-high. The literal matcher is rewritten onto the shared subject (Task 3's untouched `[screen]`
    cases are its net); the tally changes from synchronous to stepped (stale-ticket and closed-bar paths); the GUI
    thread still runs each step, and a catastrophic pattern is bounded by the engines' own limits (owner decision 1).
  - **Coverage:** the `[searchquery]` and `[searchwalk]` tables (chunked equals unchunked at three budgets, streaming,
    reflow, eviction, the catastrophic pattern), `[searchstatus]`, `[findbar]`, the session's stepped-tally cases, the
    QML property tests on the native and Terminal rows, and three snapshot scripts.

---

## Owner decisions

The run proceeds on each recommended default.

1. **How a catastrophic regex is contained (R-f).** Chunking bounds rows per step, not time per row. *Default: the
   engines' own limits (libc++'s and MSVC's complexity errors become "Pattern too complex", libstdc++'s breadth-first
   executor is polynomial) plus refusing back-references; no thread.* The alternative copies each step's subject under
   the lock and matches it on a worker thread that a new keystroke abandons, which costs a thread, a copy per step, and
   the spec's "on the GUI thread" model.
2. **The Terminal row with no pane header (R-i).** A single pane or Minimal has no header row to host the bar. *Default:
   the floating one-row bar at the pane's top right, over the top row's cells like the native bar.* The alternative
   reserves a header row while the bar is open, which resizes the PTY (SIGWINCH) on every open and close.
3. **The current match's 1-pt ring.** The canvas rings the current match; the renderer has no outline for a run of
   cells. *Default: defer it; 85 % against 35 % marks the current match.* The alternative adds a ring decoration to
   `RenderCell` and `vtrasterizer`'s decoration renderer in this step.
4. **The macOS find chords in both editions (R-j).** ⌘ chords collide with nothing either edition binds (issue #2175).
   *Default: Classic and Native2026 alike.* The alternative keeps Classic chord-free until #2175.
5. **F3 and ⌘G wrap like the bar's buttons (R-h).** *Default: wrap; vi's `n`/`N` keep stopping at the ends.* The
   alternative keeps the actions non-wrapping and gives ⌘G its own wrapping action.
6. **Use Selection for Find off macOS (R-j).** Ctrl+Shift+E is `SplitVertical`. *Default: no default chord; the Edit
   menu and the palette reach it.*

## Spec gaps found

- §3.8's citations are stale (current lines in the Spec paragraph): `SearchStatus.hpp:136-160` is now `:114-165`, and
  `TerminalSession.cpp:4408-4420` is now `:4443-4489`; the synchronous tally and the per-keystroke walk it does not cite
  (`:2725-2757`, `:2527-2573`) are where the GUI thread actually blocked.
- "500 lines per step" bounds rows, not the time one row takes: a catastrophic regex on a single row is bounded only by
  the engine (owner decision 1). The per-keystroke nearest-match search and stepping are also grid walks §3.8 does not
  chunk; the plan bounds the first to one step (R-e) and leaves stepping synchronous.
- The regex flavour, Unicode handling (NFC, `.` over a wide character, case folding) and the whole-word rule are
  unspecified (R-a to R-c).
- The canvas's match colours, the ring and "⌘G / ⇧⌘G / ⌘E" are not in §3.8 (R-j, R-l, owner decision 3); the chords
  overlap issue #2175.
- The canvas has no Windows or Linux find bar; their rows reuse the macOS extents with their own radii and colours.
- "Popover glass" is drawn by no surface yet (R-n). The Terminal row's canvas reads the count "2/5", §3.8 "2 of 5";
  §3.8 is used on every row. The Terminal row's "Find" label is the field's placeholder.
- §3.8 does not say where the bar goes on the Terminal row without a header (owner decision 2), nor what "under the
  current match" means for a match wrapped over two rows (the band covers both).
- Search-match scrollbar ticks are §3.12's (3f), not 3b's.
