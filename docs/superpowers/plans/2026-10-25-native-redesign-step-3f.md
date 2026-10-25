# Native Redesign — Step 3f (Command Blocks: Rail, Status Chip, Block Menu, Fold Chip, Sticky Header as Chrome, Scrollbar Ticks) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25) at HEAD `0382e4ca`, before Steps 1c (in progress), 1d, 2 (in progress
in `../contour-step2`) and 3e land, and beside the written plans of 3a–3d. Step 3f depends on 1b, 2 and 3e (plans
index). Where it touches code those steps add or change, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference before Task 1; line numbers are at
`0382e4ca` and a later task finds code an earlier one moved by its name.

**Goal:** Command blocks become first-class chrome that never covers a non-blank cell: a 3-pt pixel rail in a fixed
22-pt block gutter (neutral, danger with ✗, accent with ▸; ✓ on hover), a status chip right of the command only over
blank cells (else a mark on the rail), a ⋯ button and a native block menu (Copy Output, Copy Command, Rerun, Open in
Pager, Collapse Output) whose header carries what the tooltip said, keyboard focus and announcements, an
"80 lines hidden ⌄" fold chip, the sticky header moved into the pane header or the title band, and scrollbar ticks
(failures solid, running hollow, search matches yellow).

**Architecture:**
- **Every block decision is a pure function in `vtbackend/shell/`.** `BlockRail.hpp` (which segment and mark a row
  draws), `BlockChipPlacement.hpp` (where the chip fits over blank cells, or that it collapses), `BlockStatusText.hpp`
  (the chip's, the menu header's and VoiceOver's words), and `ScrollbarMarks.hpp`'s tick table. `Terminal` gathers
  facts per frame and fills the `RenderBuffer`; nothing in it decides.
- **The gutter has a form.** `vtbackend::BlockColumnForm { Glyphs = 0, Rail }` comes from the chrome row
  (`layout.blockColumn`): the native rows draw the rail in a pixel strip of `space.blockGutter`, placed after the
  timestamp and line-number cells; the Terminal row keeps today's private-use glyph column in cells. The strip's
  width depends on configuration only, and one function (`session::gutterExtentFor`) gives it to the page fit, the
  renderer and the hit-test.
- **Menus are tables; presentation is 1b's.** `command/BlockMenu.hpp` builds the block menu (a header, then five
  rows pinned to a block id); the pane context menu puts the same group first when the click lands in a block. A new
  `ContextMenuEntryKind::Header` flows through `MenuTree`, the in-scene model and the Cocoa presenter. The menu opens
  through `WindowController::presentSurface` with an anchor.
- **The sticky header stays the data source.** `RenderStickyHeader` is still filled by `Terminal::fillStickyHeader`;
  `window/StickyHeaderPlacement.hpp` decides its host (pane header, title band, or the grid as today), and
  `window/StickyHeaderChrome.hpp` binds it to the strings QML shows. Under a chrome host the renderer draws nothing
  over row 0.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), Objective-C++ (AppKit, the 1b presenter), QML, Catch2 v3,
Python 3 (lint), CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.12 (`:673-717`) binds, with
what it cites: §1 (principles, `:28-64`, principle 5 "Nothing goes over non-blank cells"), §2.1 (component tokens
`size.rail` 3, `space.blockGutter` 22, `size.foldChipHit` 24, `:84-94`), §2.5/§2.6 (snapshots, testing,
`:187-226`), §5 (`:844-854`). §3.12's citations at this HEAD: `Terminal::fillGutter` `Terminal.cpp:3937`,
`fillStickyHeader` `:4361`, `Renderer::renderGutter` `Renderer.cpp:1010` (cited `:1006`), `renderStickyHeader` `:1068`
(cited `:1064`), the private-use glyphs `BoxDrawingRenderer.cpp:433-440` and `:2789-2806`, `blockStatus*`
`ColorPalette.hpp:370-426`, the block tooltip `SessionChrome.qml:93-128`. Design record:
`docs/superpowers/specs/2026-10-23-native-redesign-design.md` ("Command blocks never draw UI over non-blank terminal
cells. The status colour for "running" is the accent colour, never yellow (yellow means search)", `:22-23`). Canvas
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `blocks`, `moments` (8), `mac-find-menu`
(context menu, Shell › Command) and `tui` are in `.superpowers/design/canvas/project/`. Plans index:
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Steps 1b and 4a are done; 1c, 1d, 2, 3a–3e are
assumed done (`2026-10-25-native-redesign-step-1c.md`, `-1d.md`, `-2.md`, `-3a.md` … `-3d.md`, and 3e's plan).

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

- **The spec, verbatim (§3.12):**
  - "`RenderGutterCell` gains a `RailSegment { None = 0, Head, Body, Tail }` and an outcome."
  - "`Renderer::renderGutter` draws a 3-pt rounded bar, plus a head glyph: ✗ for a failure, ▸ while running, ✓ on
    hover." "The rail sits in `space.blockGutter` (22 pt), after line numbers and timestamps." "The private-use glyphs
    remain for the TUI row."
  - "Status chip (pure, `vtbackend/shell/BlockChipPlacement.hpp`)." "The chip is drawn only over blank cells to the
    right of the prompt." "If it does not fit, it collapses to the rail's head glyph."
  - "Hovering a block lights its rail and shows a `⋯` button." "The menu opens through the 1b presenter, from a
    `command/BlockMenu` table: Copy Output, Copy Command, Rerun, Open in Pager, Collapse Output." "The block tooltip
    (`SessionChrome.qml:97-128`) moves into the menu's header **in this PR**."
  - "⌘↑ / ⌘↓ focus a block." "⌃↩ or ⇧F10 opens the block's menu." "Changes are announced through `Announcer`."
  - "Fold chip. "80 lines hidden ⌄", with a 24-pt hit area."
  - "Sticky header. It moves into the pane header, or into the title band for a single pane. `RenderStickyHeader`
    stays as the data source."
  - "Scrollbar ticks. Failures are solid ticks, running blocks hollow ones, and search matches yellow."
  - Tests: "Building rail segments." "The chip placement table: with an RPROMPT, without one, and in a narrow pane."
    "The block menu table." "Binding the sticky header data."
- **Canvas values, exact** (`blocks`, `mac-find-menu`, `moments` 8, `tui`). Native rows in points; the Terminal row in
  cells:

  | Token | MacOS / Fluent / ContourLinux | Terminal |
  |---|---|---|
  | `space.blockGutter` (the strip) | 22 | 0 (the glyph column's `gutter.block_column_width` cells) |
  | `size.rail` (bar width) | 3 | 0 |
  | `space.railInset` (bar's left edge in the strip) | 8 | 0 |
  | `radius.rail` | 2 | 0 |
  | `size.railMark` (the ✗ disc, the ▸ and ✓ box) | 13 | 1 |
  | `size.blockActions` (the ⋯ button, square) | 22 | 1 |
  | `radius.blockActions` | 6 | 0 |
  | `size.foldChip` (drawn height, clamped to the row) | 20 | 1 |
  | `size.foldChipHit` (minimum hit width and height) | 24 | 1 |
  | `radius.chip` (status and fold chips) | 10 | 0 |
  | `size.scrollTick` (tick height) | 3 | 1 |
  | `layout.blockColumn` | `Rail` | `Glyphs` |

  - The ✗ is drawn on a `status.danger` disc in the page's background ink; ▸ in the accent (the scheme's
    `block_status` running colour); ✓ in the neutral rail colour, on hover only. Rail colours: success neutral
    (`blockStatusColor(Success)`), failure `blockStatusColor(Failure)`, running `blockStatusColor(Running)`; hovered or
    focused, `blockStatusHoverColor`. Static: no pulse, no animation at all (Reduce Motion has nothing to stop).
  - Chip copy: "Failed · exit 8 · 12.4 s", "Running · 8 s", a quiet duration on success ("4 ms"); its fill is the
    outcome's colour at 16 %, text in the outcome's colour (success: `gutterTextColor()`, no fill).
  - Fold chip copy "80 lines hidden ⌄" (thousands grouped, "1 line hidden ⌄"); fill the text colour at 8 %.
  - VoiceOver: "Failed, exit 8, 12.4 seconds".
  - Sticky header in chrome: "✓ cmake --build … · 2m 14s · Jump to Command ↑".
  - Ticks: failures solid, running a 1-pt outline in the running colour, search matches `searchHighlight` yellow
    (Step 3b's R-l colour).
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the 1c, 1d, 2, 3a–3d SDD ledgers):
  - **Running is the accent, never yellow; yellow is search, and only search is yellow.** `block_status` running is
    blue in contour-dark/-light (Phase 0 Task 4).
  - **Nothing goes over non-blank cells** (principle 5): every chip and button sits over blank cells, the gutter, a
    pane header or the title band. A test pins each.
  - **Menus are data** (1b): the block menu is a `detail::MenuTable` table; a pick runs after the menu closes, against
    the pinned `QPointer<TerminalSession>` and block id, as a no-op when either has gone (1b R-m). No chords in
    context menus (1b R-i).
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`); a new file's baseline is 0 and a
    baseline only goes down.
  - Every visual surface is a snapshot row; every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`). TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - Text contrast at least 4.5:1 on contour-dark and contour-light (chip, fold chip, menu header).
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
  - Shortcuts: macOS ⌘ chords in both editions (3b D4: Classic binds nothing to ⌘); none new off macOS where a chord
    would collide (3b D6).
- **AGENT.md applies in full:** data-driven tables (`BlockRailMarkRows`, `BlockStatusWordingRows`,
  `ScrollbarMarkKindTable`'s new column, the block menu, `StickyHeaderHostRules`, the token rows); dependency
  injection (the notifier and announcer the session is given, the terminal's injected clocks — never
  `system_clock::now()`); configuration at construction (`GutterChrome` is fixed per session and rebuilt on reload;
  block focus and hover are domain state); `enum class` over `bool` with zero the default; `std::expected` for
  fallible calls; Doxygen on every new public declaration; `.hpp` with `#pragma once`; zero warnings, no `NOLINT`;
  `slots`, `signals` and `emit` are Qt macros, never identifiers. QML-facing `Q_PROPERTY`/`QVariantMap` booleans are
  the documented carve-out.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`):
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds in one tree.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding. Format touched `.cpp`/`.hpp`/`.mm` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`;
  check new or touched `.qml`, `.txt`, `CMakeLists.txt` with `editorconfig-checker <files>`. **Never clang-format a
  `.qml`.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value,
  and every table row names every field (`ChromeTokenRows`, `ScrollbarMarkKindTable`, `SnapshotScripts`, the new
  tables).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type. Counts in
  tests come from tables (`BlockMenuRowCount`, `ScrollbarMarkKindTable.size()`, `SnapshotScripts.size()`).
- **Qt-free stays Qt-free:** everything new in `vtbackend/`, `command/` and the `window/*.hpp` decisions includes no
  Qt header.
- **Never edit `vendor/core-cpp`.** The working tree carries other agents' edits (`ChromeTokenGroups_test.cpp`,
  `UiStyle_test.cpp`, `TabWellLayout_test.cpp` at plan time): stage only the task's own files, by name, never
  `git add -A` or a directory.
- **Commits:** one per task, message ending with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`; never `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 15).

## Review Focus

1. **A prompt with an RPROMPT or powerlevel10k filling the line.** zsh's RPROMPT sits at the right edge with blanks
   between it and the command; p10k can fill the whole row. Expected: the chip lands in the blank run before the
   RPROMPT with two blank columns either side, never over it; with no run wide enough it collapses to the rail's mark
   and the details reach the user through the menu header and VoiceOver. Pinned in Task 5 (`blockChip.rpromptKeepsItsCells`,
   `blockChip.p10kFullRowCollapses`, `Gutter.terminal.aChipNeverCoversAnRprompt`).
2. **A block of 100 000 lines, folded and scrolled.** Expected: collapsed, its head row draws one `Single` rail
   segment and "100,000 lines hidden ⌄"; expanded and scrolled into its middle, every drawn row is `Body`, the chip
   and ⋯ anchor follow the first visible row, the sticky header names it, and a frame does not walk the block's rows.
   Pinned in Task 3 (`Gutter.terminal.aHundredThousandLineBlockDrawsItsRail`) and Task 6
   (`Gutter.terminal.aHundredThousandLineFoldIsOneChip`).
3. **A block still running when the pane is closed or restarted** (Step 2's Restart). Expected: on the session's end
   the running record is closed (`CommandBlockEnd::Abandoned`), so the ended pane shows no ▸, no hollow tick, no
   ticking chip, raises no finish notification, and a block menu or notification click aimed at it does nothing; a
   restarted pane starts with no blocks. Pinned in Task 12.
4. **Nested shells without shell integration.** `ssh host` or a bare `bash` from an integrated zsh leaves the outer
   block running for as long as the nested shell lives. Expected: one Running rail over all its rows, one hollow tick,
   one chip on the `ssh` row ticking, no chip or mark on the nested shell's own prompt rows, ⌘↓ from the block goes to
   the next real block or nowhere, and key input clears block focus. Pinned in Task 10
   (`TerminalSession: a nested shell without integration stays one running block`).
5. **The gutter with line numbers and timestamps on.** Expected: lead-in, timestamps, line numbers, then the 22-pt
   rail strip against column 0; the page loses exactly that width; a press on a line number does what it did, a press
   on the strip acts on the block; the Terminal row keeps the glyph column after the numbers as today. Pinned in Task 2
   (`gutterGeometry.railSitsAfterTheCells`, "the window reserves the cells and the strip") and Task 4
   (`RailGeometry.theCellsMoveLeftOfTheStrip`).

Also pinned, beyond the five: a block menu pick after the block was evicted is a no-op (Task 9); the Terminal row
draws exactly today's gutter cells (Task 3); a hidden success tick stays hidden under `scrollbar.marks` (Task 7).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Which rows get the rail (owner, 1).** `layout.blockColumn` is `Rail` on the MacOS, Fluent and ContourLinux
  rows and `Glyphs` on the Terminal row, in both editions: it is chrome, not a §2.7 default. `gutter.block_column_width`
  applies to the glyph column only (documented). The strip is reserved exactly when the block column is today
  (`folding.show_markers`, `gutter.exit_status` or `gutter.user_marks`).
- **R-b. Strip geometry.** Left to right: the lead-in, the timestamp cells, the line-number cells, the strip
  (`round(space.blockGutter × dpr)` device px), column 0. The bar is `size.rail` wide at `space.railInset`, from 2 pt
  below its first row's top to 2 pt above its last row's bottom, its ends rounded by `radius.rail` (drawn as the
  bar plus two one-device-pixel-narrower cap rectangles: `RenderTarget` has rectangles only). The mark is centred on
  the bar, top-aligned with the head row.
- **R-c. Rail segments.** Rows are the fold projection's rows (a collapsed fold is its head), on screen or not. A
  block's first such row is `Head`, its last `Tail`, between them `Body`, so a block continuing above the viewport
  draws `Body` on the top row; a block with one visible row is `Single`, an enumerator §3.12 does not list
  (a bar rounded at both ends). A `Prompting` block (the live prompt) draws none. Rows of no block draw none.
- **R-d. The mark.** On the `Head`/`Single` row: Failure → ✗ always; Running → ▸ always; Success → ✓ only while the
  block is hovered or focused; a user mark (vi `mm`) on a row that heads nothing → ◆, as today. The Glyphs form keeps
  `blockColumnGlyph()` unchanged.
- **R-e. The chip** sits on the block's command row (the row the sticky header copies, `stickyHeaderInput()`'s input
  line). Its run is the LAST maximal blank run starting at or after the command's start column that holds the chip
  plus `BlockChipGap` (2) columns on each side, or plus `StickyHeaderChipInset` at the page's edge. It is
  right-aligned in that run. Without one it collapses (R-d's mark stays the only sign; a collapsed Success shows
  nothing). Drawn by the renderer as annotation cells in the terminal font over a `radius.chip` band clamped to the
  row (owner, 2). Running time refreshes once a second while a running chip is on screen.
- **R-f. Copy.** One wording row per `vtbackend::CommandEnding` plus Running: Succeeded and Unknown → the duration
  alone; Failed → "Failed · exit {code}"; Killed → "Killed · {signal}"; Crashed → "Crashed"; Interrupted →
  "Interrupted"; Running → "Running". Then " · {duration}". Durations (`formatChipDuration`): under 1 s "N ms"; finished
  under 60 s one decimal ("12.4 s"); running whole seconds ("8 s"); then "2m 14s", "1h 02m". Spoken
  (`spokenBlockStatus`): commas for " · ", "seconds"/"minutes"/"milliseconds" spelled out ("Failed, exit 8, 12.4
  seconds"; "Running, 8 seconds"; "Succeeded, 4 milliseconds").
- **R-g. Hover.** A block is hovered while the pointer is over its rows or the strip beside them
  (`Terminal::setHoveredCommandBlock`). The ⋯ button shows for the hovered block, else the focused one, in the strip at
  its first visible row. The tooltip goes (its anchor property becomes `blockActionsAnchor`), and its text becomes the
  menu header's rows.
- **R-h. The menu.** `buildBlockMenu` gives: the header (one `Header` row per tooltip line: the command, the status in
  R-f's words, the directory, the start time), a separator, then "Copy Output" (`CopyCommandBlock`), "Copy Command"
  (`CopyCommandLine`), "Rerun…" (`RerunCommand`; it still asks, Step 3c), "Open in Pager" (`OpenCommandOutput`),
  "Collapse Output"/"Expand Output" (the new `ToggleCommandFold`), each pinned by `CommandBlockId` with
  `CommandBlockTarget::Pointer`. The pane context menu, right-clicked inside a block, starts with the same group and a
  separator (canvas: "puts that block's actions first, under a header naming it"); the old mid-menu block rows go,
  "Select Output" with them (owner, 5).
- **R-i. Header rows.** `ContextMenuEntryKind::Header` is a disabled label: in-scene a `type.caption` secondary row
  (the first in `type.footnote` primary), natively `+[NSMenuItem sectionHeaderWithTitle:]` on macOS 14+ and a
  disabled item before that. `buildMenuTree` gives it no action and no chord; the menu bar never holds one (asserted).
- **R-j. Block focus.** `Terminal` holds `std::optional<CommandBlockId> _focusedBlock` (domain state). ⌘↑/⌘↓
  (`FocusPreviousCommand`/`FocusNextCommand`) move it to the previous/next block with a command, scroll its head into
  view (`scrollToCommandBlockHead`), light its rail, and announce R-f's spoken words, Polite. ⇧⌘↑/⇧⌘↓
  (`SelectPreviousCommandOutput`/`SelectNextCommandOutput`) do the same and select the output. While a block is
  focused the input mode carries the new `MatchModes::BlockFocus` (`0x80`, the last free bit): ⌃↩ and ⇧F10 run
  `OpenBlockMenu`, Return runs `ToggleFold` on it, ⎋ runs `ClearCommandBlockFocus`; without focus those keys reach the
  application. Focus clears on any key that reaches the PTY, on a click in the grid, and when its record goes.
  (owner, 4)
- **R-k. Fold chip.** `collapsedFoldLabel` reads "80 lines hidden ⌄"; a band behind it (`radius.chip`, height
  `min(size.foldChip, row)`); its hit extent is the label widened to at least `size.foldChipHit` and the row's height
  (§3.12's 24 pt cannot be met vertically on a row shorter than 24 pt without covering the next row; a spec gap).
- **R-l. Sticky header host.** `stickyHeaderHostFor`: the pane shows a header (Step 2's `PaneHeaderContent` not
  `Hidden`) → `PaneHeader`; else one visible pane and a title band shown (Step 1c) → `TitleBand`; else `Grid` (today's
  in-grid band: Minimal splits, `show_title_bar: true`, full screen with the band hidden). Under `PaneHeader` and
  `TitleBand` the terminal fills `RenderStickyHeader` with `presentation = Chrome` and the renderer and the row-0 press
  ignore it; the session publishes the binding.
- **R-m. Ticks (owner, 3).** `ScrollbarMarkKind` gains `SearchMatch`, sorted above `UserMark` and below `Running`
  (a search is what the user is doing now, but a failure or a running block on the same pixel still wins), and the
  table a `ScrollbarTickStyle { Solid = 0, Hollow }` column (Running is `Hollow`). Sources gain `Running` and `SearchMatches`; the YAML token `commands` still switches on Running too, so
  existing configs keep their ticks. Native2026's default becomes `[failures, running, search_matches]`; Classic's
  stays `[failures, commands, user_marks]` (which, through `commands`, includes Running).
- **R-n. The ended session.** When Step 2's `_end` becomes non-`None`, the session calls
  `Terminal::abandonRunningCommand()`: the running record becomes `Finished` with the new
  `CommandBlockEnd::Abandoned`, no exit code, no duration, outcome `Success` (nothing known), and no finish summary.
- **R-o. The finish notification** (moments 8) exists (`TerminalSession::deliverFinishNotice`, 10 s, `unfocused`);
  3f adds that its click scrolls the pane to the block (`scrollToCommandBlockHead`) after `revealSession`, and flashes
  it. The tab's ✓/✗ badge is Step 1c's (`TabAttention::noteFinished`); the "Blocks" palette group is Step 3d's: every
  new action gets `CommandCategory::Blocks`.
- **R-p. Snapshots.** The snapshot terminal is a stub, so the rail, the chips and the ticks are not in the pictures;
  their geometry and colours are pinned by Tasks 3–7. New scripts picture the chrome: `block-menu`, `block-actions`,
  `sticky-pane-header`, `sticky-title-band`.

### Non-goals

- The canvas's hovered-block background tint and its 1-px ring (not in §3.12; they would tint non-blank cells).
- Chords in the block menu (⌥⌘C, ⌘R, ⌃⇧G, ⌥⌘[ on the canvas): 1b R-i keeps context menus chord-free, and the ⌘ chords
  are issue #2175's.
- The macOS `UNUserNotificationCenter` backend (Step 6.5), the shell-integration card (6.1).

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `window/ChromeTokenObjects.{hpp,cpp}`, `window/UiStyleProvider.{hpp,cpp}` (modify) | The block tokens and `layout.blockColumn` | 1 |
| `src/vtbackend/screen/GutterSettings.{hpp,cpp}`, `src/contour/geometry/WindowGeometry.hpp`, `src/contour/session/FontControl.{hpp,cpp}`, `SessionInput.cpp`, `TerminalSession.{hpp,cpp}`, `display/TerminalDisplay.cpp`, `window/WindowController.cpp`, `config/Config.{hpp,cpp}`, `ContourGuiApp.{hpp,cpp}` (modify) | The form, the strip in the page fit and the hit-test | 2 |
| `src/vtbackend/shell/BlockRail.{hpp,cpp}` (create); `render/RenderBuffer.hpp`, `screen/Gutter.{hpp,cpp}`, `screen/Terminal.{hpp,cpp}` (modify) | Rail segments and marks into the frame | 3 |
| `src/vtrasterizer/RailGeometry.hpp` (create); `GridMetrics.hpp`, `Renderer.{hpp,cpp}` (modify) | Drawing the strip | 4 |
| `src/vtbackend/shell/BlockChipPlacement.hpp`, `BlockStatusText.{hpp,cpp}` (create); `Terminal.{hpp,cpp}`, `RenderBuffer.hpp`, `Renderer.cpp` (modify) | The status chip | 5 |
| `src/vtbackend/screen/Gutter.{hpp,cpp}`, `Terminal.cpp`, `src/contour/session/TerminalSession.cpp` (modify) | The fold chip | 6 |
| `src/vtbackend/shell/ScrollbarMarks.{hpp,cpp}`, `screen/Terminal.cpp`, `screen/SearchTallyWalk.{hpp,cpp}` (3b's), `config/Config.{hpp,cpp}`, `TerminalSession.cpp` (modify) | The tick model | 7 |
| `src/contour/command/BlockMenu.{hpp,cpp}` (create); `ContextMenu.{hpp,cpp}`, `MenuTree.cpp`, `config/Actions.hpp`, `window/ContextMenuModel.cpp`, `window/AppMenuBar.cpp`, `platform/CocoaNativeMenuPresenter.mm` (modify) | The block menu table and header rows | 8 |
| `src/contour/window/WindowController.{hpp,cpp}`, `session/TerminalSession.{hpp,cpp}`, `session/TerminalSessionManager.{hpp,cpp}` (modify) | Opening the block menu, the ⋯ anchor | 9 |
| `src/vtbackend/input/MatchModes.hpp`, `screen/Terminal.{hpp,cpp}`, `config/Actions.hpp`, `Config.hpp`, `command/MenuBarTable.cpp`, `TerminalSession.{hpp,cpp}` (modify) | Block focus, chords, announcements | 10 |
| `src/contour/window/StickyHeaderPlacement.hpp`, `StickyHeaderChrome.hpp` (create); `vtbackend/shell/StickyHeader.hpp`, `RenderBuffer.hpp`, `Terminal.cpp`, `Renderer.cpp`, `TerminalSession.{hpp,cpp}` (modify) | The sticky header as chrome | 11 |
| `src/vtbackend/shell/CommandBlock.{hpp,cpp}`, `screen/Terminal.{hpp,cpp}`, `TerminalSession.cpp` (modify) | The ended session; the notification click | 12 |
| `src/contour/qml/BlockActionsButton.qml` (create); `SessionChrome.qml`, `ActionContextMenu.qml`, `PaneHeader.qml` (Step 2's), `TitleBar.qml` (Step 1c's), `test/QmlMocks.hpp`, `styles/ContourTui/*` as needed (modify) | The QML | 13 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | Snapshot rows | 14 |

### Shared files

Files another Phase 3 step's spec or plan also names. A parallel worktree that touches one rebases before its task
commits; the merge order is the board's.

- `src/contour/qml/SessionChrome.qml` — §3.11/3e (the scrollbar `:135`, `ScrollBarMode` and its visibility table;
  the hyperlink tooltip `:64-90` goes in 3e), §3.8/3b (the `SearchBar` instance), §3.9/3c (the permission dialogs).
  3f removes the block tooltip (`:93-128`), adds the ⋯ button, and changes the tick delegate (`:239-270`). **Depends on
  Step 3e:** the tick layer stays inside 3e's bar, and 3e's "marks keep it visible" rule counts the new kinds.
- `src/vtbackend/shell/ScrollbarMarks.{hpp,cpp}` and `Terminal::scrollbarMarkInputs()` — 3e reads marks for its
  overlay visibility; 3b's deferred search-match ticks land here (Task 7).
- `src/vtbackend/screen/SearchTallyWalk.{hpp,cpp}` — 3b's; 3f adds the matched rows' collection (Task 7).
  **Depends on Step 3b.**
- `src/vtbackend/shell/CommandBlock.{hpp,cpp}` — 3a's `{LastCommand}` reads `lastFinished()`/`current()`
  (`CommandBlock.hpp:558-568`) and `formatCommandDuration`. 3f adds `CommandBlockEnd::Abandoned` (3a's D5 "empty while
  a command runs" then also shows nothing for an abandoned block, which is right) and leaves `formatCommandDuration`
  untouched (the chip's format is a new function).
- `src/vtbackend/screen/Terminal.{hpp,cpp}` — §3.7/3a (cwd, `FileSystem`), §3.8/3b (the search block), §3.11/3e
  (`applyHintOverlay`). 3f's hunks: `fillGutter`, `fillFoldLabels`, `fillStickyHeader`, `scrollbarMarkInputs`, the new
  block focus and hover members.
- `src/vtbackend/core/ColorPalette.{hpp,cpp}` — 3b edits `ContourScheme` (search yellow); 3f reads `blockStatus*`,
  `searchHighlight` and adds nothing.
- `src/contour/qml/PaneHeader.qml` (Step 2's) — 3b's find slot, 3e's link pill (trailing area). 3f swaps the name/cwd
  for the sticky header. `src/contour/qml/TitleBar.qml` (Step 1c's `bandTitle`) — 3f swaps the title.
- `src/contour/config/Actions.hpp`, `command/CommandCatalog.hpp` — 3b (`SearchSelection`), 3d (`CommandCategory`).
- `src/contour/command/MenuBarTable.cpp` — 3b's Edit rows; 3f's Shell › Commands rows.
- `src/contour/config/Config.hpp` `makeDefaultInputMappings` — 3b's `#ifdef __APPLE__` find chords; 3f's block chords
  join that block.
- Edited by every Phase 3 step: `config/ChromeTokenGroups.hpp`, `test/ChromeSnapshotPlan.hpp`, `test/QmlMocks.hpp`,
  `src/contour/CMakeLists.txt`, `src/vtbackend/CMakeLists.txt`, `scripts/qml-size-literal-baseline.txt`, `metainfo.xml`.

---

### Task 1: The block tokens and the block column form

**Files:**
- Modify: `src/vtbackend/screen/GutterSettings.hpp` — `BlockColumnForm` (here, so `config` and `vtbackend` share it).
- Modify: `src/contour/config/ChromeTokenGroups.hpp` — `SizeTokens` (`:196-224`), `SpaceTokens` (`:227-240`),
  `RadiusTokens` (`:243-261`), `LayoutTokens` (`:346-354`), the four rows (`MacRow :547`, `FluentRow :598`,
  `LinuxRow :649`, `TerminalRow :704`); and wherever Steps 1c/2/3a–3e appended their fields.
- Modify: `src/contour/window/ChromeTokenObjects.{hpp,cpp}`, `src/contour/window/UiStyleProvider.{hpp,cpp}` — publish
  the tokens QML reads (`size.blockActions`, `radius.blockActions`, `size.scrollTick`, `space.blockGutter`).
- Test: `src/contour/config/ChromeTokenGroups_test.cpp` (the row pin), `src/contour/window/UiStyleProvider_test.cpp`.

**Interfaces:**
- Produces (`vtbackend`, `GutterSettings.hpp`): `enum class BlockColumnForm : uint8_t { Glyphs = 0, Rail };` —
  Glyphs: the private-use glyph column in cells; Rail: the pixel strip.
- Produces (`config`): `SizeTokens::rail`, `railMark`, `blockActions`, `foldChip`, `foldChipHit`, `scrollTick`;
  `SpaceTokens::blockGutter`, `railInset`; `RadiusTokens::rail`, `blockActions`, `chip`;
  `LayoutTokens::blockColumn = vtbackend::BlockColumnForm::Glyphs`; values per Global Constraints' table.
- Produces (QML, through the provider): `chromeStyle.size.blockActions`, `chromeStyle.radii.blockActions`,
  `chromeStyle.size.scrollTick`, `chromeStyle.space.blockGutter`, and `chromeStyle.blockColumnRail` (`bool`, the
  QML carve-out: `layout.blockColumn == Rail`).

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp`, the existing `chromeTokens.everyRowMatchesTheDesignKit`: extend each row's expected
    values with the table's columns (MacOS/Fluent/ContourLinux identical; Terminal in cells).
  - New `chromeTokens.blockColumnFormFollowsTheRow` (`[chrometokens]`): `ChromeTokenRows[i].layout.blockColumn` is
    `Rail` for every native row and `Glyphs` for Terminal, over `iota(0uz, ChromeTokenRows.size())`.
  - `UiStyleProvider_test.cpp` (`[uistyle]`): the MacOS provider reports `size.blockActions == 22`,
    `space.blockGutter == 22`, `blockColumnRail == true`; the Terminal provider `1`, `0`, `false`.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test contour_gui_test`. Expected: compile failure on
  `SizeTokens::rail`.
- [ ] **Step 3: Implement** the enum, the fields, every row (naming every field), and the provider properties.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[chrometokens]" && out/appleclang-debug/src/contour/contour_gui_test "[uistyle]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (only the files above), message
  `config: the command block tokens and the block column's form per chrome row`.

---

### Task 2: The strip in the gutter's layout, the page fit and the hit-test

> **Depends on Step 1c/2:** none of their code; the call sites below are at `0382e4ca`. Pre-flight re-greps
> `configuredGutterWidth`/`gutterExtentFor` callers.

**Files:**
- Modify: `src/vtbackend/screen/GutterSettings.{hpp,cpp}` — `GutterSettings::blockColumnForm`, `GutterLayout::rail`,
  `gutterLayoutFor`.
- Modify: `src/contour/geometry/WindowGeometry.hpp:557-608` — `GutterExtent::rail`, `isInGutter`, `gutterPointAt`.
- Modify: `src/contour/session/FontControl.{hpp,cpp}:45-160` — `GutterChrome`, the extent and width functions.
- Modify: `src/contour/config/Config.{hpp,cpp}` — `gutterSettingsFor(Config const&, vtbackend::BlockColumnForm)`
  (`.hpp:3227`, `.cpp:799`); `emulationSettings` passes `Glyphs` (the daemon draws nothing), `sessionSettings` takes
  the form.
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}` — `[[nodiscard]] config::ChromeStyle chromeStyle() const noexcept`
  (if 1c/2 have not added it).
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — `_gutterChrome` built at construction from
  `app.chromeStyle()` and rebuilt on reload; `sessionSettings` calls (`.cpp:389`, `:1306`, `:5997`); the
  `gutterColumns` reader (`:5874-5889`).
- Modify: `src/contour/session/SessionInput.cpp:153-189` (`gutterHitAt`), `src/contour/display/TerminalDisplay.cpp:942`,
  `:1648`, `src/contour/window/WindowController.cpp:1352`, `:1526`, `:1709` — read the session's `GutterChrome`.
- Test: `src/vtbackend/screen/Gutter_test.cpp`, `src/contour/geometry/WindowGeometry_test.cpp` (or the file that holds
  `isInGutter`'s cases), `src/contour/session/FontControl_test.cpp`, `src/contour/session/TerminalSession_test.cpp`.

**Interfaces:**
- Produces (`vtbackend`):
  - `GutterSettings::blockColumnForm = BlockColumnForm::Glyphs;` (declared last).
  - `enum class BlockRailPresence : uint8_t { Absent = 0, Present };` and `GutterLayout::rail = BlockRailPresence::Absent;`.
    Under `Rail`, `gutterLayoutFor` appends no `BlockColumn` cell segment and sets `rail = Present` exactly when any of
    the block column's three jobs is on; under `Glyphs` it is today's layout.
- Produces (`geometry`):
  - `GutterExtent::rail = 0;` (device px, after `columns`).
  - `isInGutter` covers `[pageMarginLeft - rail - columns - leadIn, pageMarginLeft)`.
  - `enum class GutterPart : uint8_t { Cells = 0, Rail };`, `struct GutterPoint { GutterPart part = GutterPart::Cells; int column = 0; bool operator==(GutterPoint const&) const = default; };`
  - `[[nodiscard]] constexpr std::optional<GutterPoint> gutterPointAt(int xDevicePx, int pageMarginLeft, GutterExtent extent, int cellWidthPx) noexcept;`
    — the lead-in counts as cell column 0 (or as the rail when there are no cells); `gutterColumnAt` is kept, built on it,
    returning `nullopt` over the rail.
- Produces (`session`, `FontControl.hpp`):
  - `struct GutterChrome { vtbackend::BlockColumnForm form = vtbackend::BlockColumnForm::Glyphs; int stripPoints = 0; int barPoints = 0; int barInsetPoints = 0; int barRadiusPoints = 0; int markPoints = 0; bool operator==(GutterChrome const&) const = default; };`
  - `[[nodiscard]] GutterChrome gutterChromeFor(config::ChromeStyle style) noexcept;` — from the row's tokens.
  - `configuredGutterLayout(config::Config const&, GutterChrome const&)`,
    `gutterExtentFor(vtbackend::GutterLayout const&, vtbackend::ImageSize cellSize, double scale, GutterChrome const&)`
    (`rail = layout.rail == Present ? ceil(stripPoints × scale) : 0`), `configuredGutterExtent(…, GutterChrome const&)`,
    `configuredGutterWidth(…, GutterChrome const&)` — the old overloads go; every caller passes the session's.
  - `TerminalSession`: `[[nodiscard]] session::GutterChrome const& gutterChrome() const noexcept;`
  - A rail hit reports `GutterHit::segment == GutterSegmentKind::BlockColumn` (no new kind).

- [ ] **Step 1: Write the failing tests.**
  - `Gutter_test.cpp` (`[gutter]`): `Gutter.layout.railTakesNoCells`: `{ .exitStatus = true, .lineNumbers = Absolute, .blockColumnForm = Rail }`
    → segments `{LineNumber}` only, `totalColumns == 6`, `rail == Present`; the same with `Glyphs` → today's two
    segments, `rail == Absent`; all three jobs off under `Rail` → `rail == Absent`, `totalColumns == 0`.
  - `WindowGeometry_test` (`[geometry][gutter]`), Review Focus 5: `gutterGeometry.railSitsAfterTheCells`:
    `extent = { .leadIn = 12, .columns = 3 * 16, .rail = 44 }`, `pageMarginLeft = 200`, `cellWidth = 16`:
    `gutterPointAt(200 - 1) == GutterPoint { Rail }`, `(200 - 44) == Rail`, `(200 - 45) == { Cells, 2 }`,
    `(200 - 44 - 48) == { Cells, 0 }`, `(200 - 44 - 48 - 12) == { Cells, 0 }` (lead-in), one pixel further `nullopt`,
    `(200)` `nullopt`; `gutterColumnAt` is `nullopt` over the rail; with `columns = 0`, the lead-in is the rail's.
  - `FontControl_test.cpp` (`[fontcontrol]`): `gutterChromeFor(MacOS) == { Rail, 22, 3, 8, 2, 13 }`,
    `gutterChromeFor(Terminal).form == Glyphs`; with line numbers and timestamps on, `configuredGutterWidth` at
    scale 2 equals `gutterLeadIn(2) + (8 + 6) × cellWidth + 44` under MacOS, and today's value under Terminal.
  - `TerminalSession_test.cpp` (`[contour][session][gutter]`), "the window reserves the cells and the strip": a
    `TestApp` session's `gutterChrome()` equals `gutterChromeFor(app.chromeStyle())`, and with that row's `Rail`
    form `gutterHitAt` over the strip's middle gives `segment == BlockColumn` for the row beside it.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test contour_test contour_gui_test`. Expected: compile
  failure on `GutterSettings::blockColumnForm`.
- [ ] **Step 3: Implement.** The renderer still draws nothing in the strip (Task 4); the page fit reserves it now.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[gutter]" && out/appleclang-debug/src/contour/contour_test "[geometry],[fontcontrol],[config]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][gutter],[windowsizing]"`
  Expected: PASS; the existing `[windowsizing]` and gutter cases pass unedited on the Terminal row and with the
  strip's width added on the native rows (update only expectations that compute a native gutter width, citing R-b).
- [ ] **Step 5: Tidy, format, commit**, message
  `contour: a fixed block strip after the gutter's cells, reserved by the page fit and found by the hit-test`.

---

### Task 3: Rail segments and marks

**Files:**
- Create: `src/vtbackend/shell/BlockRail.hpp`, `BlockRail.cpp`; list them in `src/vtbackend/CMakeLists.txt`.
- Modify: `src/vtbackend/render/RenderBuffer.hpp:107-117` (`RenderGutterCell`).
- Modify: `src/vtbackend/screen/Gutter.{hpp,cpp}` (`GutterRow`, `appendGutterCells`, `:301-323`, `.cpp:546-580`).
- Modify: `src/vtbackend/screen/Terminal.{hpp,cpp}` — `fillGutter` (`.cpp:3937`), `setHoveredCommandBlock`.
- Test: new `src/vtbackend/shell/BlockRail_test.cpp` (add to `vtbackend_test`), `src/vtbackend/screen/Terminal_gutter_test.cpp`.

**Interfaces:**
- Produces (`vtbackend`, `BlockRail.hpp`):
  - `enum class RailSegment : uint8_t { None = 0, Head, Body, Tail, Single };`
  - `enum class RailMark : uint8_t { None = 0, Check, Cross, Running, UserMark };`
  - `enum class BlockAttention : uint8_t { Rest = 0, Hovered, Focused };`
  - `struct RailRowFacts { std::optional<CommandBlockId> block {}; std::optional<CommandBlockId> above {}; std::optional<CommandBlockId> below {}; CommandBlockState state = CommandBlockState::Prompting; CommandBlockOutcome outcome = CommandBlockOutcome::Success; UserMarkPresence mark = UserMarkPresence::Absent; BlockAttention attention = BlockAttention::Rest; bool operator==(RailRowFacts const&) const = default; };`
    — `above`/`below` are the blocks of the projection's rows directly above and below, looked up one row beyond
    the drawn rows at the page's edges (nullopt where there is no such row).
  - `[[nodiscard]] constexpr RailSegment railSegmentOf(RailRowFacts const& facts) noexcept;` (R-c).
  - `struct RailMarkRow { CommandBlockOutcome outcome; RailMark atRest; RailMark attended; };` and
    `inline constexpr auto BlockRailMarkRows = std::array { … }` in outcome order: Success `None`/`Check`, Failure
    `Cross`/`Cross`, Running `Running`/`Running`.
  - `[[nodiscard]] constexpr RailMark railMarkOf(RailRowFacts const& facts, RailSegment segment) noexcept;` — the
    table on `Head`/`Single`; `UserMark` for a marked row whose segment is `None`/`Body`/`Tail` and that heads nothing.
  - `[[nodiscard]] RGBColor railColor(ColorPalette const& palette, CommandBlockOutcome outcome, BlockAttention attention) noexcept;`
    — `blockStatusColor` at rest, `blockStatusHoverColor` otherwise.
- Produces (`RenderGutterCell`): `RailSegment rail = RailSegment::None; RailMark mark = RailMark::None; std::optional<CommandBlockOutcome> outcome {}; RGBColor railColor {};`
  — a rail cell has `codepoint == 0` and `column == ColumnOffset(0)` (it is the strip, not a cell column).
- Produces (`Terminal`): `void setHoveredCommandBlock(std::optional<CommandBlockId> block);` (refreshes only on a
  change, as `setGutterHoverLine` does); `fillGutter` emits one rail cell per drawn row with `rail != None` or
  `mark != None` when `layout.rail == Present`, and today's glyph cells otherwise.

- [ ] **Step 1: Write the failing tests.**
  - `BlockRail_test.cpp` (`[blockrail]`):
    - `blockRail.segments`: a table over `{block, above, below}`: `{7, —, 7} → Head`, `{7, 7, 7} → Body`,
      `{7, 7, 8} → Tail`, `{7, 6, 8} → Single`, `{—, 7, 7} → None`; a `Prompting` block → `None` whatever its
      neighbours.
    - `blockRail.marksFollowTheTable`: `BlockRailMarkRows[i].outcome == CommandBlockOutcome(i)` over
      `CommandBlockOutcomeCount`; Success Head at `Rest` → `None`, `Hovered` → `Check`, `Focused` → `Check`; Failure
      `Cross` always; Running `Running` always; a `Body` row of a failed block → `None`; a marked row heading nothing →
      `UserMark`.
    - `blockRail.runningIsNeverYellow`: under contour-dark and contour-light, `railColor(…, Running, Rest)` equals
      `blockStatusColor(Running)`, and its hue is not within 30° of yellow (the design record's rule as a test).
  - `Terminal_gutter_test.cpp` (`[gutter]`, `MockTerm` with `GutterSettings { .exitStatus = true, .blockColumnForm = Rail }`):
    - `Gutter.terminal.aFailedBlockDrawsAHeadAndATail`: `ls` (exit 0, 2 output rows), `false` (exit 1, 1 output
      row), then a live prompt: the drawn rows' rails read `Head, Body, Tail, Head, Tail, None`, marks
      `None, None, None, Cross, None, None`, the second block's `outcome == Failure`.
    - `Gutter.terminal.hoveringASuccessShowsItsCheck`: `setHoveredCommandBlock(first)` → its head mark `Check`, its
      `railColor` the hover colour; `nullopt` → `None`.
    - `Gutter.terminal.aRunningBlockShowsItsTriangle`: ;C with no ;D → head `Running`, every row of it the running colour.
    - Review Focus 2: `Gutter.terminal.aHundredThousandLineBlockDrawsItsRail`: history 120 000; a block printing
      100 000 lines, finished; `CollapseLastFold` → its head row's rail is `Single`; expand, scroll so the viewport's
      top is its row 50 000 → every drawn row is `Body`, no mark; scroll to its last row → that row is `Tail`.
    - `Gutter.terminal.theGlyphFormDrawsTodaysCells`: the same session under `Glyphs` gives exactly the
      `gutterRows()` strings today's cases expect (`TestHelpers.hpp:346`), and no rail cell.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test`. Expected: compile failure on `BlockRail.hpp`.
- [ ] **Step 3: Implement.** `fillGutter` reads the block of drawn row `y` and of rows `y - 1` and `y + 1` once each
  (the neighbour beyond the page through `_viewport.translateScreenToGridLine` of `-1` and `drawnRowLimit()`), never
  walking a block's rows: a frame stays O(drawn rows × log blocks).
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[blockrail],[gutter]"`
  Expected: PASS, the existing `[gutter]` cases unedited.
- [ ] **Step 5: Tidy, format, commit**, message `vtbackend: command blocks draw a rail of segments and marks`.

---

### Task 4: The renderer draws the strip

**Files:**
- Create: `src/vtrasterizer/RailGeometry.hpp` (header-only); list it in `src/vtrasterizer/CMakeLists.txt`.
- Modify: `src/vtrasterizer/GridMetrics.hpp:39-60` (`GutterRailMetrics`), `src/vtrasterizer/Renderer.{hpp,cpp}`
  (`renderGutter :1010`, `applyResize`), `src/contour/session/FontControl.cpp` (`applyResize` passes the strip),
  `src/vtrasterizer/BoxDrawingRenderer.cpp` (a `Check` status mark, U+10F010, beside `:2789-2806`),
  `docs/vt-extensions/private-use-glyphs.md` (the new codepoint).
- Test: new `src/vtrasterizer/RailGeometry_test.cpp` (add to `vtrasterizer_test`), `BoxDrawingRenderer_test.cpp`.

**Interfaces:**
- Produces (`vtrasterizer`):
  - `struct GutterRailMetrics { int strip = 0; int bar = 0; int barInset = 0; int radius = 0; int mark = 0; bool operator==(GutterRailMetrics const&) const = default; };`
    in device px; `GridMetrics::rail` of that type.
  - `Renderer::applyResize(vtbackend::ImageSize, vtbackend::PageSize, PageMargin, GutterRailMetrics rail)`.
  - `RailGeometry.hpp`: `struct RailRect { int x = 0; int y = 0; int width = 0; int height = 0; bool operator==(RailRect const&) const = default; };`
    `[[nodiscard]] inline std::vector<RailRect> railRects(vtbackend::RenderGutterCell const& cell, GridMetrics const& metrics, int smoothScrollOffset);`
    — the bar (and its two cap rectangles for `Head`/`Tail`/`Single`, R-b); `[[nodiscard]] inline vtbackend::CellLocation railMarkOrigin(…)`
    and the grid-origin shift that centres the mark's one-cell glyph on the bar; `[[nodiscard]] inline int cellGutterShift(GridMetrics const&)`
    `== metrics.rail.strip` — the timestamp and line-number cells are drawn that far further left.
  - `blockColumnCodepoint`-style mapping `railMarkCodepoint(RailMark)`: Cross U+10F005, Running U+10F002, UserMark
    U+10F003, Check U+10F010 (new: a light check drawn by `StatusMark`).
- `Renderer::renderGutter`: rail cells through `railRects` (`renderRectangle` in `cell.railColor`, opaque) and their
  mark as a standalone cell (the ✗ on a danger disc: the disc is a `StatusMark` shape in the danger colour, the ✗ the
  page background, as one glyph); text cells with the grid origin shifted by `cellGutterShift`, as `renderStatusRows`
  shifts it (`Renderer.cpp:806-815`).
- **TUI row:** `rail.strip == 0`, so nothing moves and the glyph cells draw as today.

- [ ] **Step 1: Write the failing tests** (`[renderer][rail]`):
  - `RailGeometry.aHeadIsRoundedAtTheTop`: metrics `cellSize 16×32`, `pageMargin.left 200`, `rail { 44, 6, 16, 4, 26 }`;
    a `Head` cell on line 3 → the bar `{ x = 200 - 44 + 16, y = 3 × 32 + 4, width 6, height 28 }` plus one cap row
    one pixel narrower each side at its top; `Body` → `{ …, y = 96, height 32 }` with no caps; `Single` → both caps,
    `height 24`.
  - `RailGeometry.theCellsMoveLeftOfTheStrip` (Review Focus 5): `cellGutterShift == 44`, and a line-number cell at
    column -1 maps to x `200 - 44 - 16`.
  - `RailGeometry.theMarkIsCentredOnTheBar`: the mark's cell box is centred at x `200 - 44 + 16 + 3`, top-aligned
    with the row.
  - `BoxDrawingRenderer_test.cpp`: U+10F010 is claimed and draws non-empty pixels; U+2713 is not claimed.
  - `RailGeometry.noStripNoChange`: `rail.strip == 0` → `cellGutterShift == 0`.
- [ ] **Step 2: Run them and see them fail.** Build `vtrasterizer_test`. Expected: compile failure on `RailGeometry.hpp`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtrasterizer/vtrasterizer_test "[renderer]" && out/appleclang-debug/src/contour/contour_test "[fontcontrol]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message `vtrasterizer: draw the block rail in its strip, the gutter's cells beside it`.

---

### Task 5: The status chip

**Files:**
- Create: `src/vtbackend/shell/BlockChipPlacement.hpp` (header-only), `src/vtbackend/shell/BlockStatusText.{hpp,cpp}`;
  list them in `src/vtbackend/CMakeLists.txt`.
- Modify: `src/vtbackend/render/RenderBuffer.hpp` — `RenderChipBand`, `RenderBuffer::chipBands` (cleared in `clear()`).
- Modify: `src/vtbackend/screen/Terminal.{hpp,cpp}` — `fillBlockChips` (called beside `fillFoldLabels` from the frame
  build, `Terminal.cpp:1298-1303`), `hasVisibleRunningChip()`.
- Modify: `src/vtrasterizer/Renderer.cpp` — `renderChipBands` before `renderAnnotations`.
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — `_runningChipTimer` (one single-shot `QTimer`, armed while
  `hasVisibleRunningChip()`, refreshing the render buffer each second). **Pre-flight:** if the sticky header's
  `running 12s` already has a refresh path, the chip joins it instead.
- Test: new `src/vtbackend/shell/BlockChipPlacement_test.cpp`, `BlockStatusText_test.cpp`; `Terminal_gutter_test.cpp`.

**Interfaces:**
- Produces (`BlockChipPlacement.hpp`):
  - `inline constexpr auto BlockChipGap = ColumnCount(2);`
  - `struct BlockChipRowFacts { ColumnCount pageColumns {}; ColumnOffset commandStart {}; std::span<ColumnRange const> blankRuns {}; };`
    — `blankRuns` the row's maximal blank runs, ascending.
  - `enum class BlockChipPlacementKind : uint8_t { CollapsedToRail = 0, InRow };`
  - `struct BlockChipPlacement { BlockChipPlacementKind kind = BlockChipPlacementKind::CollapsedToRail; ColumnRange columns {}; bool operator==(BlockChipPlacement const&) const = default; };`
  - `[[nodiscard]] constexpr BlockChipPlacement placeBlockChip(BlockChipRowFacts const& facts, ColumnCount chipColumns) noexcept;` (R-e).
  - `[[nodiscard]] std::vector<ColumnRange> blankRunsOf(Line const& line, ColumnCount pageColumns);`, declared in
    `screen/Gutter.hpp`, defined in `Gutter.cpp` (a blank cell is an empty or space cell on the default background).
- Produces (`BlockStatusText.hpp`):
  - `struct BlockStatusWording { std::optional<CommandEnding> ending {}; std::string_view label; FinishDetail detail; std::string_view spokenLabel; };`
    and `inline constexpr auto BlockStatusWordingRows = std::array { … }` — one row per `CommandEnding` plus a
    Running row (`ending = nullopt`), R-f.
  - `[[nodiscard]] std::string formatChipDuration(std::chrono::steady_clock::duration duration, CommandBlockState state);`
  - `[[nodiscard]] std::string blockStatusText(CommandBlockRecord const& record, std::chrono::system_clock::time_point now);`
    — "Failed · exit 8 · 12.4 s"; empty for `Prompting`.
  - `[[nodiscard]] std::string spokenBlockStatus(CommandBlockRecord const& record, std::chrono::system_clock::time_point now);`
    — "Failed, exit 8, 12.4 seconds".
- Produces (`RenderBuffer`): `struct RenderChipBand { LineOffset lineOffset {}; ColumnRange columns {}; RGBAColor fill {}; };`
  `std::vector<RenderChipBand> chipBands {};` — drawn under the chip's annotation cells, `radius.chip` approximated by
  one-pixel caps as the rail is; height the row's.

- [ ] **Step 1: Write the failing tests.**
  - `BlockChipPlacement_test.cpp` (`[blockchip]`), with a 20-column chip on an 80-column page, `commandStart = 16`:
    - `blockChip.withoutAnRprompt`: runs `{[39, 80)}` → `InRow`, right-aligned at `80 - StickyHeaderChipInset`:
      `[58, 78)`.
    - `blockChip.rpromptKeepsItsCells` (Review Focus 1): runs `{[39, 70)}` (an RPROMPT at 70..79) → `InRow`,
      `[48, 68)`; the chip's last column + `BlockChipGap` ≤ 70.
    - `blockChip.p10kFullRowCollapses` (Review Focus 1): runs `{[30, 31), [45, 46)}` → `CollapsedToRail`.
    - `blockChip.narrowPaneCollapses`: a 30-column page, runs `{[20, 30)}` → `CollapsedToRail`.
    - `blockChip.neverLeftOfTheCommand`: a wide blank run at `[2, 14)` (inside the prompt) and none after → collapsed.
    - `blockChip.theLastWideRunWins`: runs `{[20, 50), [52, 80)}` → placed in `[52, 80)`.
  - `BlockStatusText_test.cpp` (`[blockstatus]`): rows cover every `CommandEnding` once plus Running; records built
    with an injected wall clock: exit 8 after 12.4 s → "Failed · exit 8 · 12.4 s" and "Failed, exit 8, 12.4 seconds";
    running 8.7 s → "Running · 8 s" / "Running, 8 seconds"; exit 0 after 4 ms → "4 ms" / "Succeeded, 4 milliseconds";
    134 s → "2m 14s"; SIGKILL → "Killed · SIGKILL · 3.0 s"; a `Prompting` record → "".
  - `Terminal_gutter_test.cpp` (`[gutter][blockchip]`):
    - `Gutter.terminal.aChipNeverCoversAnRprompt` (Review Focus 1): a prompt row `"~ ❯ make"` with `"[main] 14:02"` at
      the right edge, then exit 2: the frame's annotation cells for the chip lie in blank cells only (each cell's
      column is blank in the row), end ≥ 2 columns before `[main]`, and read "Failed · exit 2 · …".
    - `Gutter.terminal.aCollapsedChipLeavesTheMark`: the same with the row filled to the edge → no chip cells, the
      rail head mark `Cross`.
    - `Gutter.terminal.aRunningChipTicks`: running at `t0`, the wall clock advanced 3 s → "Running · 3 s";
      `hasVisibleRunningChip()` true; after ;D false.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test`. Expected: compile failure on
  `BlockChipPlacement.hpp`.
- [ ] **Step 3: Implement.** Chip colours: text `blockStatusColor(outcome)` (success: `gutterTextColor()`), band the
  outcome colour at 16 % (none for success). The TUI row draws the same cells and band (the band is a cell fill there).
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[blockchip],[blockstatus],[gutter]" && out/appleclang-debug/src/vtrasterizer/vtrasterizer_test "[renderer]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (with the session timer), message
  `vtbackend: a status chip right of the command, over blank cells only`.

---

### Task 6: The fold chip

**Files:**
- Modify: `src/vtbackend/screen/Gutter.{hpp,cpp}` — `collapsedFoldLabel`, `foldLabelExtent` (`.hpp:330-362`),
  `layoutFoldLabel`.
- Modify: `src/vtbackend/screen/Terminal.cpp` — `fillFoldLabels` adds a `RenderChipBand`; `collapsedFoldLabelAt`
  (`:4006`) uses the widened hit extent.
- Modify: `src/contour/session/TerminalSession.cpp` — the hit-test converts `size.foldChipHit` to columns for the session's
  cell width (the extent takes `ColumnCount minimumHitColumns`).
- Test: `src/vtbackend/screen/Gutter_test.cpp`, `Terminal_gutter_test.cpp`, `src/contour/session/TerminalSession_test.cpp`.

**Interfaces:**
- `collapsedFoldLabel(uint64_t hiddenLines)` → `"80 lines hidden ⌄"`, `"1 line hidden ⌄"`, `"100,000 lines hidden ⌄"`.
- `foldLabelExtent(LineOffset, ColumnCount textColumns, ColumnCount pageColumns, std::size_t labelColumns, ColumnCount minimumHitColumns)`
  → the drawn extent widened symmetrically to `minimumHitColumns`, clipped to blank columns of the row and the page.
- `TerminalSession`: `minimumHitColumns = ceil(size.foldChipHit × dpr / cellWidth)` from the session's `GutterChrome`
  row (add `foldChipHitPoints` to `GutterChrome`, Task 2's struct, declared last).

- [ ] **Step 1: Write the failing tests.**
  - `Gutter_test.cpp` (`[gutter]`): `Gutter.foldLabel.words`: the three strings above; `Gutter.foldLabel.hitIsWidened`:
    a 10-column label drawn at `[9, 19)` after 8 text columns on an 80-column page, `minimumHitColumns = 14` → widened
    to `[7, 21)`, clipped to the blank columns → `[8, 21)`; with `minimumHitColumns = 4` → the drawn `[9, 19)`.
  - `Terminal_gutter_test.cpp`, Review Focus 2: `Gutter.terminal.aHundredThousandLineFoldIsOneChip`: the 100 000-line
    block collapsed → exactly one chip band and one label "100,000 lines hidden ⌄" in the frame, on the head row,
    over blank cells.
  - `TerminalSession_test.cpp` (`[contour][session][gutter]`), "a click beside the fold chip still opens it": a press
    one column right of the label's last cell expands the fold; a press three columns beyond does nothing.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test contour_gui_test`. Expected: the label still reads `⋯ 80 lines`.
- [ ] **Step 3: Implement.** Update the existing label expectations (`⋯ 1,234 lines`) in the same commit, citing the canvas.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[gutter],[folding]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][gutter]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message `vtbackend: a fold chip, "80 lines hidden ⌄", with a wider hit`.

---

### Task 7: The scrollbar's tick model

> **Depends on Step 3b:** `SearchTallyWalk` (`screen/SearchTallyWalk.{hpp,cpp}`) and the `searchHighlight` palette
> colour. **Depends on Step 3e:** its overlay-visibility table reads `scrollbarMarkInputs()`; the new kinds count.

**Files:**
- Modify: `src/vtbackend/shell/ScrollbarMarks.{hpp,cpp}` (`:24-130`, `scrollbarMarkColor`).
- Modify: `src/vtbackend/screen/SearchTallyWalk.{hpp,cpp}` — keep the matched rows' stable ids (one per row).
- Modify: `src/vtbackend/screen/Terminal.{hpp,cpp}` — `scrollbarMarkInputs()` (`.hpp:2050`) adds `SearchMatch` inputs
  from the finished or partial tally; `scrollbarMarksRevision()` moves with the tally.
- Modify: `src/contour/config/Config.{hpp,cpp}` — `ScrollBarConfig::marks` per edition (the edition default table),
  the `scrollbar.marks` tokens `running` and `search_matches` (`Config.cpp:4270` neighbourhood), `commands` mapping
  to `{Commands, Running}`; `ConfigDocumentation.hpp`.
- Modify: `src/contour/session/TerminalSession.cpp` — each published mark gains `style` (an `int`).
- Test: `src/vtbackend/shell/ScrollbarMarks_test.cpp`, `src/vtbackend/screen/SearchTallyWalk_test.cpp`,
  `src/contour/config/Config_test.cpp`, `src/contour/session/TerminalSession_test.cpp`.

**Interfaces:**
- `enum class ScrollbarMarkKind : uint8_t { Command = 0, UserMark, SearchMatch, Running, Failure };` (R-m's order).
- `enum class ScrollbarTickStyle : uint8_t { Solid = 0, Hollow };`, `ScrollbarMarkKindRow::style`.
- `ScrollbarMarkSource` gains `Running = 1 << 3`, `SearchMatches = 1 << 4`; `Running`'s row source becomes `Running`.
- `scrollbarMarkColor(palette, SearchMatch)` → `palette.searchHighlight` colour (3b's), opaque.
- `[[nodiscard]] std::span<int64_t const> SearchTallyWalk::matchedRows() const noexcept;` (stable ids, ascending).
- The published mark map gains `style` (`0` solid, `1` hollow).

- [ ] **Step 1: Write the failing tests.**
  - `ScrollbarMarks_test.cpp` (`[scrollbarmarks]`): the table's static asserts still hold with five rows;
    `scrollbarMarks.runningIsHollow`: `scrollbarMarkKindRow(Running).style == Hollow`, every other `Solid`;
    `scrollbarMarks.priority`: Failure > Running > SearchMatch > UserMark > Command on one pixel;
    `scrollbarMarks.searchIsYellowOnlyThere`: under contour-dark, `scrollbarMarkColor(SearchMatch)` is
    `searchHighlight`'s colour and no other kind's colour equals it.
  - `SearchTallyWalk_test.cpp` (`[searchwalk]`): a walk over three matching rows reports their three stable ids, a row
    matched twice once.
  - `Config_test.cpp` (`[config]`): the Native2026 default `marks` is `{Failures, Running, SearchMatches}`, Classic's
    `{Failures, Commands, Running, UserMarks}`; `scrollbar.marks: [commands]` yields `{Commands, Running}`;
    `[search_matches]` yields `{SearchMatches}`; an unknown token is still refused as today.
  - `TerminalSession_test.cpp`, extending "scrollbar marks follow the command blocks" (`:1712`): a running block's
    mark carries `style == 1`; with the find bar on `foo` matching two rows, two `SearchMatch` marks appear, and
    clearing the search removes them; a Hidden success tick stays hidden under Native2026's default (Also pinned).
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test contour_test contour_gui_test`. Expected: compile
  failure on `ScrollbarTickStyle`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[scrollbarmarks],[searchwalk]" && out/appleclang-debug/src/contour/contour_test "[config]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][session]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message
  `vtbackend: scrollbar ticks for running blocks (hollow) and search matches (yellow)`.

---

### Task 8: The block menu as a table, and header rows

**Files:**
- Create: `src/contour/command/BlockMenu.hpp`, `BlockMenu.cpp`; list them in `src/contour/command/CMakeLists.txt`.
- Modify: `src/contour/config/Actions.hpp` — `ToggleCommandFold` (struct, variant, `CommandBlockActionConcept :304`,
  `NonRepeatableActionConcept`, documentation, catalogue row, 3d's `CommandCategory::Blocks`).
- Modify: `src/contour/command/ContextMenu.{hpp,cpp}` — `ContextMenuEntryKind::Header`; `PointedBlock` (`.hpp:41-46`)
  is replaced by `BlockMenuState`; `contextMenuTemplate` (`:161-245`) starts with the block group and loses its
  mid-menu block rows (`:204-210`).
- Modify: `src/contour/command/MenuTree.cpp:41-43`, `src/contour/window/ContextMenuModel.cpp:40-62` (`kind: "header"`),
  `src/contour/window/AppMenuBar.cpp` (a header in a menu-bar tree is a programming error: `assert`), 
  `src/contour/platform/CocoaNativeMenuPresenter.mm:90-140` (R-i, and the outline test's kind mapping).
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — `operator()(actions::ToggleCommandFold)`;
  `contextMenuState()` fills the header facts from `commandBlockTooltipText`.
- Test: new `src/contour/command/BlockMenu_test.cpp` (add to `contour_test`); `ContextMenu_test.cpp`,
  `MenuTree_test.cpp`, `window/ContextMenuModel_test.cpp`, `platform/CocoaNativeMenuPresenter_test.mm`,
  `config/Actions_test.cpp`.

**Interfaces:**
- Produces (`config`): `struct ToggleCommandFold { vtbackend::CommandBlockTarget target = vtbackend::CommandBlockTarget::Last; vtbackend::CommandBlockId block {}; bool operator==(ToggleCommandFold const&) const = default; };`
  catalogue name `"ToggleCommandFold"`, documentation "Collapses or expands a command block's output."
- Produces (`command`, `BlockMenu.hpp`):
  - `enum class FoldPresence : std::uint8_t { None = 0, Expanded, Collapsed };`
  - `struct BlockMenuState { vtbackend::CommandBlockId id {}; std::vector<std::string> header {}; OutputPresence output = OutputPresence::Absent; CommandLinePresence commandLine = CommandLinePresence::Absent; PromptPresence livePrompt = PromptPresence::Absent; FoldPresence fold = FoldPresence::None; };`
    — `header` is the tooltip text's lines, the status line in `blockStatusText`'s words.
  - `[[nodiscard]] std::vector<ContextMenuEntry> buildBlockMenu(BlockMenuState const& state);` — R-h's rows, the
    enabling rules of today's `blockCommand` rows (`ContextMenu.cpp:86-110`), "Collapse Output" when `Expanded`,
    "Expand Output" when `Collapsed`, disabled when `None`.
  - `inline constexpr std::size_t BlockMenuRowCount = 5;` (asserted against the table).
  - `ContextMenuState::blockUnderCursor` becomes `std::optional<BlockMenuState>`; `buildContextMenu` puts
    `buildBlockMenu(*blockUnderCursor)` and a separator first.

- [ ] **Step 1: Write the failing tests.**
  - `BlockMenu_test.cpp` (`[blockmenu]`):
    - `blockMenu.headerThenFiveRows`: header `{"ctest -R contour_gui_test", "Failed · exit 8 · 12.4 s", "~/repo/contour", "Started 14:02:11"}`
      → four `Header` entries with those titles, a separator, then titles
      `{"Copy Output", "Copy Command", "Rerun…", "Open in Pager", "Collapse Output"}` (`BlockMenuRowCount` rows).
    - `blockMenu.rowsArePinned`: every command row's action holds `target == Pointer` and `block == state.id`
      (`std::visit` over the five types).
    - `blockMenu.availability`: no output → Copy Output, Open in Pager, Collapse disabled; no command line → Copy
      Command and Rerun disabled; no live prompt → Rerun disabled; `Collapsed` → "Expand Output" enabled.
  - `ContextMenu_test.cpp` (`[contextmenu]`): with `blockUnderCursor`, the first entries equal `buildBlockMenu`'s
    followed by a separator; without, the first entry is "Copy"; no row titled "Select Output" remains.
  - `MenuTree_test.cpp` (`[menutree]`): a `Header` entry gives a `Header` node with no action and no chord, and the
    actions' indices skip it.
  - `ContextMenuModel_test.cpp` (`[contextmenumodel]`): a header row is `{ kind: "header", title }` with no `actionId`.
  - `CocoaNativeMenuPresenter_test.mm` (`[cocoa]`): the outline of a tree with a header has a disabled item (macOS
    14+: `isSectionHeader`) titled as the header, and picks map past it.
  - `Actions_test.cpp` (`[actions]`): `fromString("ToggleCommandFold")` holds the alternative; it is non-repeatable.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test contour_gui_test`. Expected: compile failure on
  `BlockMenu.hpp`.
- [ ] **Step 3: Implement.** `TerminalSession::operator()(ToggleCommandFold)` resolves the block like the other pinned
  block actions and toggles the fold at its head through `withScrollableChange`.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[blockmenu],[contextmenu],[menutree],[actions],[menubar]" && out/appleclang-debug/src/contour/contour_gui_test "[contextmenumodel],[cocoa],[contour][session]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message `command: the block menu is a table, with its header rows`.

---

### Task 9: Opening the block menu, and the ⋯ anchor

**Files:**
- Modify: `src/contour/window/WindowController.{hpp,cpp}` — `openBlockMenu`, `presentSurface`'s anchor (`.cpp:180-220`).
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — `openBlockMenu(CommandBlockId)` (`.cpp:3346`) goes to the
  manager; `commandBlockTooltipAnchor` → `blockActionsAnchor` (`.hpp:224-226`, `:353-357`, `:1775-1777`);
  `commandBlockTooltip()` is removed; `Q_INVOKABLE void openBlockActions();`.
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}` — `openBlockMenu(TerminalSession*, CommandBlockId, std::optional<QPointF>)`.
- Modify: `src/contour/session/CommandBlockTooltip.{hpp,cpp}` — `BlockTooltipHint` and `blockTooltipHintText` go;
  `commandBlockTooltipText` stays as the header's source (renamed `commandBlockHeaderLines`, returning lines).
- Test: `src/contour/window/WindowMenus_test.cpp`, `src/contour/session/TerminalSession_test.cpp` (the tooltip cases at
  `:6934-7060`, `:7977` are rewritten for the anchor), `CommandBlockTooltip_test.cpp`.

**Interfaces:**
- `WindowController::openBlockMenu(session::TerminalSession& session, vtbackend::CommandBlockId block, std::optional<QPointF> anchor);`
  — pins `_contextMenuSession`, builds `BlockMenuState` under the terminal lock, and
  `presentSurface(MenuSurface::Pane, resolveMenu(command::buildBlockMenu(state)), anchor)`.
- `presentSurface(MenuSurface, command::MenuTree, std::optional<QPointF> anchor = std::nullopt)` passes it as
  `MenuAnchor::position`. (A defaulted parameter is a position, not a `bool`.)
- `Q_PROPERTY(QRectF blockActionsAnchor READ blockActionsAnchor NOTIFY blockActionsAnchorChanged)` — the strip × the
  attended block's first visible row, in logical px; empty when nothing is hovered or focused, on the alternate
  screen, or under `Glyphs` (the TUI's ⋯ is the cell at the block column's head: the anchor is that cell).
- `Q_INVOKABLE void openBlockActions();` — opens the attended block's menu anchored at `blockActionsAnchor`'s bottom left.
- ⌃↩/⇧F10 (Task 10) and a press on the rail of a block with nothing to fold (`BlockColumnPress::OpenBlockMenu`) call
  the same path; a right press keeps opening the pane menu with the block group first.

- [ ] **Step 1: Write the failing tests.**
  - `WindowMenus_test.cpp` (`[contour][windowmenus][blockmenu]`), in a `TestApp` (in-scene route):
    - "the block menu opens on the pane surface with its header": after a failed `make` (`runAndDeliverShellCommand`),
      `openBlockMenu(session, id, QPointF{4, 40})` publishes a pane menu whose first row is a header titled `make`, and
      `contextMenuRequested` fires once.
    - "a pick after the pane closed does nothing": open, close the tab, trigger "Copy Output": no crash, clipboard
      unchanged (1b R-m).
    - "a pick after the block was evicted does nothing" (Also pinned): `max_records = 1`, open on block 1, run a second
      command, trigger "Copy Output": the clipboard is unchanged.
  - `TerminalSession_test.cpp` (`[contour][session][gutter]`): "hovering a block's rows anchors its actions":
    `blockActionsAnchor` is non-empty over the block's output and over its strip, at its first visible row; empty
    after the pointer leaves, after a scroll moves another block under it it names that block, and empty on the
    alternate screen; "a press on the rail of a block with nothing to fold opens its menu" (the existing case at
    `:6371` now through `openBlockMenu`).
  - `CommandBlockTooltip_test.cpp` (`[blockheader]`): the header lines are the old tooltip's facts, one per line, the
    status in `blockStatusText`'s words, with no hint line.
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: compile failure on `openBlockMenu`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[windowmenus],[contour][session][gutter],[blockheader],[menus]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message
  `contour: the block menu opens from the rail, the ⋯ button and the keyboard; the tooltip becomes its header`.

---

### Task 10: Block focus, its chords, and announcements

**Files:**
- Modify: `src/vtbackend/input/MatchModes.hpp:13-23` (`BlockFocus = 0x80`, its name in the formatter at `:120-135`),
  `src/contour/config/Config.cpp:4260-4280` (the YAML mode name `BlockFocus`), `window/SettingsController.cpp:1877`
  (the mode enumeration's upper bound becomes `BlockFocus`).
- Modify: `src/vtbackend/screen/Terminal.{hpp,cpp}` — `_focusedBlock`, `focusCommandBlock`, `focusedCommandBlock`,
  `clearCommandBlockFocus`; the match modes report `BlockFocus` while set; the rail reads `BlockAttention::Focused`.
- Modify: `src/contour/config/Actions.hpp` — six actions (below), each `CommandCategory::Blocks`.
- Modify: `src/contour/config/Config.hpp` `makeDefaultInputMappings` (`:1283-…`) — the chords (R-j); the macOS ones in
  3b's `#ifdef __APPLE__` block. **Depends on Step 3b.**
- Modify: `src/contour/command/MenuBarTable.cpp:133-170` — the Shell › "Commands" submenu gains "Previous Command",
  "Next Command", "Select Previous Output", "Select Next Output", "Block Actions…".
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — the handlers; key input to the PTY and a grid press clear
  focus; the announcement.
- Test: `src/vtbackend/screen/Terminal_gutter_test.cpp` (or a new `Terminal_blockfocus_test.cpp`),
  `src/contour/config/Actions_test.cpp`, `Config_test.cpp`, `command/MenuBarTable_test.cpp`,
  `session/TerminalSession_test.cpp`.

**Interfaces:**
- Produces (`config`): `struct FocusPreviousCommand {};`, `struct FocusNextCommand {};`,
  `struct SelectPreviousCommandOutput {};`, `struct SelectNextCommandOutput {};`, `struct OpenBlockMenu {};`,
  `struct ClearCommandBlockFocus {};` — catalogue names as spelled; non-repeatable except the four steps (they
  repeat).
- Produces (`vtbackend::Terminal`):
  - `enum class BlockStep : uint8_t { Previous = 0, Next };`
  - `[[nodiscard]] std::optional<CommandBlockId> focusCommandBlock(BlockStep step);` — from the focused block, else from
    the viewport's bottom (Previous) or top (Next); skips `Prompting` records; scrolls the head into view; returns the
    new focus, `nullopt` (focus unchanged) at the ends.
  - `[[nodiscard]] std::optional<CommandBlockId> focusedCommandBlock() const noexcept;`
  - `void clearCommandBlockFocus();` — also run when the focused record is retired.
- `TerminalSession`: each step handler announces `spokenBlockStatus(record, now)` prefixed by the command's short name
  (`commandShortName`), Polite: "make, Failed, exit 2, 3.1 seconds". `OpenBlockMenu` → `openBlockActions()`.
  `ToggleFold` acts on the focused block first when there is one.
- Default chords, both editions: Shift+F10 and Ctrl+Enter → `OpenBlockMenu`, Enter → `ToggleFold`, Escape →
  `ClearCommandBlockFocus`, all with `.modes` requiring `BlockFocus`; on `__APPLE__` also Super+Up/Down →
  `FocusPreviousCommand`/`FocusNextCommand`, Shift+Super+Up/Down → the two selects, every mode except
  `AlternateScreen` (vim keeps ⌘↑ if it reads it).

- [ ] **Step 1: Write the failing tests.**
  - Terminal (`[blockfocus]`): three finished blocks and a live prompt: `focusCommandBlock(Previous)` thrice gives
    blocks 3, 2, 1 and a fourth `nullopt` with focus still 1; `Next` from 1 gives 2; the match modes include
    `BlockFocus` exactly while focused; the head row's rail mark under focus is `Check` for a success; retiring
    block 2 while focused clears focus.
  - `Actions_test.cpp` (`[actions]`): the six round-trip and sit at their variant index; every one has category
    `Blocks` (3d's test covers "every action has a category").
  - `Config_test.cpp` (`[config][input-mapping]`): "the block chords need block focus": Ctrl+Enter, Shift+F10, Enter,
    Escape mappings to the block actions all require `BlockFocus`; Enter without it is unbound by them; on
    `__APPLE__` Super+Up is `FocusPreviousCommand` outside the alternate screen; elsewhere no default mapping carries
    Super with Up/Down.
  - `MenuBarTable_test.cpp` (`[menubar]`): the Commands submenu holds the five new rows, in that order; 1b's
    reachability test passes (every action in a menu).
  - `TerminalSession_test.cpp` (`[contour][session][blockfocus]`), with a recording announcer (`setAnnouncer`):
    - "⌘↑ focuses the last block and says how it went": after a failed `make`, `FocusPreviousCommand` → announcer got
      "make, Failed, exit 2, …", Polite; the head is in view.
    - "Ctrl+Enter opens the focused block's menu, and reaches the shell without focus": with focus, a Ctrl+Enter key
      event opens the pane menu with the block header; after `ClearCommandBlockFocus`, the same event writes to the PTY.
    - "typing clears the focus": a key that reaches the PTY clears `focusedCommandBlock()`.
    - Review Focus 4, "a nested shell without integration stays one running block": `ssh` started (;C), then the
      remote prints a prompt and output with no OSC 133 for 50 rows: one block, `Running`, every drawn row of it a
      rail `Body`/`Head` in the running colour, its chip on the `ssh` row only, one `Running` (hollow) mark;
      `FocusNextCommand` from it returns `nullopt`; a typed key clears focus.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test contour_test contour_gui_test`. Expected: compile
  failure on `MatchModes::BlockFocus`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[blockfocus],[gutter],[input]" && out/appleclang-debug/src/contour/contour_test "[actions],[config],[menubar],[command]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][blockfocus],[settings]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message
  `contour: ⌘↑/⌘↓ focus a command block, ⌃↩ or ⇧F10 open its menu, and VoiceOver says how it went`.

---

### Task 11: The sticky header as chrome

> **Depends on Step 2:** `window/PaneHeaderPolicy.hpp` (`PaneHeaderContent`, `resolvePaneHeader`), `PaneHeader.qml`.
> **Depends on Step 1c:** the title band's presence (`TitleBar.qml`'s `bandTitle`; 1c's band visibility in
> `WindowController`). Pre-flight names the exact accessor.

**Files:**
- Create: `src/contour/window/StickyHeaderPlacement.hpp`, `StickyHeaderChrome.hpp` (header-only, Qt-free); list them
  in `src/contour/CMakeLists.txt`'s `_header_files`.
- Modify: `src/vtbackend/shell/StickyHeader.hpp:36-43` — `StickyHeaderPresentation`, `StickyHeaderSettings::presentation`.
- Modify: `src/vtbackend/render/RenderBuffer.hpp:128-137` — `RenderStickyHeader::presentation`, `outcome`, `commandLine`.
- Modify: `src/vtbackend/screen/Terminal.{hpp,cpp}` — `fillStickyHeader` (`:4361`) fills the new fields;
  `[[nodiscard]] std::optional<RenderStickyHeader> stickyHeaderSnapshot() const;` (the same fill into a local).
- Modify: `src/vtrasterizer/Renderer.cpp:773-777` — skip a `Chrome` header; the row-0 press in
  `TerminalSession`/`SessionInput` ignores it.
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` — `stickyHeaderHost`, the published binding, `jumpToStickyHeader()`.
- Modify: `src/contour/window/WindowController.{hpp,cpp}` — a `Q_PROPERTY(QVariantMap activeStickyHeader …)` for the
  title band, from the active session.
- Test: new `src/contour/window/StickyHeaderPlacement_test.cpp`, `StickyHeaderChrome_test.cpp` (add to `contour_test`);
  `src/vtbackend/shell/StickyHeader_test.cpp`; `TerminalSession_test.cpp`.

**Interfaces:**
- Produces (`vtbackend`): `enum class StickyHeaderPresentation : uint8_t { InGrid = 0, Chrome };`,
  `StickyHeaderSettings::presentation = StickyHeaderPresentation::InGrid;` (declared last, kept by `operator==`);
  `RenderStickyHeader::presentation`, `CommandBlockOutcome outcome = Success`, `std::string commandLine {}` (the
  record's display form, `CommandLineText`'s display accessor, at most 256 bytes).
- Produces (`window/StickyHeaderPlacement.hpp`):
  - `enum class StickyHeaderHost : uint8_t { Grid = 0, PaneHeader, TitleBand };`
  - `enum class TitleBandPresence : uint8_t { Absent = 0, Shown };`
  - `struct StickyHeaderHostInputs { PaneHeaderContent header = PaneHeaderContent::Hidden; int visiblePanes = 1; TitleBandPresence band = TitleBandPresence::Absent; };`
  - `[[nodiscard]] constexpr StickyHeaderHost stickyHeaderHostFor(StickyHeaderHostInputs const& inputs) noexcept;` (R-l)
  - `[[nodiscard]] constexpr vtbackend::StickyHeaderPresentation presentationOf(StickyHeaderHost host) noexcept;`
- Produces (`window/StickyHeaderChrome.hpp`):
  - `struct StickyHeaderChrome { vtbackend::StickyHeaderKind kind = vtbackend::StickyHeaderKind::Command; vtbackend::CommandBlockId block {}; vtbackend::CommandBlockOutcome outcome = vtbackend::CommandBlockOutcome::Success; std::string glyph {}; std::string command {}; std::string detail {}; bool operator==(StickyHeaderChrome const&) const = default; };`
  - `[[nodiscard]] StickyHeaderChrome bindStickyHeader(vtbackend::RenderStickyHeader const& header);` — `glyph` ✓/✗/▸
    by outcome; `command` = `commandLine`, else the `cells`' text trimmed (a recovered prompt row); `detail` = the
    `chip` cells' text without its leading mark; for `Evicted`, `command` = the placeholder text, `glyph` = "⋯".
- `TerminalSession`: `Q_PROPERTY(QVariantMap stickyHeader READ stickyHeader NOTIFY stickyHeaderChanged)` —
  `{ glyph, command, detail, outcome: int, jumpLabel: "Jump to Command ↑" }`, empty with none or under `Grid`;
  re-bound on viewport changes, block events and the running-chip tick, emitted only when the binding changed.
  `void setStickyHeaderHost(window::StickyHeaderHost host);` (domain state: the pane's card decides it) sets the
  terminal's presentation. `Q_INVOKABLE void jumpToStickyHeader();` → `scrollToCommandBlockHead(block)`.
- **TUI row:** the host rules are the same; a pane header row shows `✓ cmake --build … · 2m 14s ↑`, and a pane with no
  header keeps today's in-grid band.

- [ ] **Step 1: Write the failing tests.**
  - `StickyHeaderPlacement_test.cpp` (`[stickyheader]`): `{Name, 2, Absent} → PaneHeader`; `{Hidden, 1, Shown} →
    TitleBand`; `{Hidden, 1, Absent} → Grid` (no title bar, full screen hidden); `{Hidden, 3, Shown} → Grid` (Minimal
    splits); `presentationOf(Grid) == InGrid`, the other two `Chrome`.
  - `StickyHeaderChrome_test.cpp` (`[stickyheader]`), "binding the sticky header data": a header with
    `outcome = Success`, `commandLine = "cmake --build --preset appleclang-debug"`, chip cells `"✓ 2m 14s"` →
    `{ glyph "✓", command "cmake --build --preset appleclang-debug", detail "2m 14s" }`; a failure → `"✗"`, detail
    `"2 1m 04s"` as the chip spells it; no command line → the cells' text; `Evicted` → `"⋯"` and the placeholder.
  - `StickyHeader_test.cpp` (`[stickyheader]`): `Chrome` presentation decides exactly what `InGrid` does
    (`stickyHeaderFor` over the existing table with both values).
  - `TerminalSession_test.cpp` (`[contour][session][stickyheader]`): with `setStickyHeaderHost(PaneHeader)` and the view
    scrolled into a block's output, `stickyHeader` holds its command and `stickyHeaderChanged` fired once; the render
    buffer's header has `presentation == Chrome`; a press on row 0 reaches the grid (selects), not the header;
    `jumpToStickyHeader()` brings the head to the top; back to `Grid`, the press jumps as today (the cases at
    `:6220-6890` pass unedited under `Grid`).
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test contour_test contour_gui_test`. Expected: compile
  failure on `StickyHeaderPlacement.hpp`.
- [ ] **Step 3: Implement.** `PaneCard.qml` (Task 13) calls `session.setStickyHeaderHost` from its header policy; until
  then every session is `Grid`, which is today.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[stickyheader]" && out/appleclang-debug/src/contour/contour_test "[stickyheader]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][stickyheader],[contour][session][gutter]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message
  `contour: the sticky command header can live in the pane header or the title band, taking no cells`.

---

### Task 12: A running block when the session ends, and the notification click

> **Depends on Step 2:** `TerminalSession::_end` (`window::SessionEnd`), `restartSession`, `paneSessionReplaced`.

**Files:**
- Modify: `src/vtbackend/shell/CommandBlock.{hpp,cpp}` — `CommandBlockEnd::Abandoned`; `CommandBlockStore::abandonRunning()`.
- Modify: `src/vtbackend/screen/Terminal.{hpp,cpp}` — `void abandonRunningCommand();` (under the lock; refreshes).
- Modify: `src/contour/session/TerminalSession.cpp` — call it where Step 2 sets `_end`; the notification's
  `actionInvoked` handler (`:2355-2362`) scrolls to the block.
- Test: `src/vtbackend/shell/CommandBlock_test.cpp`, `src/contour/session/TerminalSession_test.cpp`,
  `TerminalSessionManager_test.cpp`.

**Interfaces:**
- `enum class CommandBlockEnd : uint8_t { Reported = 0, Implicit, Abandoned };` — every `switch` over it handles the new
  enumerator; `outcomeOf` of an abandoned record with no exit code is `Success`.
- `void CommandBlockStore::abandonRunning();` — the running record becomes `Finished`/`Abandoned`, its duration and exit
  code stay empty, the change listener is told, no `CommandBlockSummary` is produced.
- The `actionInvoked` handler: `_manager->revealSession(*this)`, then `scrollToCommandBlockHead(_finishNotice->block)`
  under the lock when the record still exists (taken before `forgetFinishNotice()`).

- [ ] **Step 1: Write the failing tests.**
  - `CommandBlock_test.cpp` (`[commandblock]`): `abandonRunning()` on a running record → `Finished`, `Abandoned`, no
    exit code, `outcomeOf == Success`; on a `Prompting` record it does nothing; the listener is told once.
  - `TerminalSession_test.cpp` (`[contour][session][paneend]`), Review Focus 3:
    - "a block running when the shell exits stops running": `;C` for `sleep 100`, then the PTY closes: the gutter's
      head mark is not `Running`, `scrollbarMarks` holds no `style == 1` mark, `hasVisibleRunningChip()` is false, and
      the recording notifier raised nothing.
    - "a restarted pane starts with no blocks": end it, `restart()` (Step 2) → the new session's
      `commandBlocks().size() == 0`, `stickyHeader` empty, `blockActionsAnchor` empty.
    - "closing the pane while its block runs leaves nothing behind": close the tab with the block running; the
      notifier raised and withdrew nothing; a posted block menu pick for it is a no-op (Task 9's guard).
  - `TerminalSession_test.cpp` (`[contour][session][notification][manager]`), extending "a click on a finish notice
    brings its session's tab forward" (`:9277`): the background session was scrolled to its top first; after
    `fireActivated`, the block's head row is in the viewport and the line flash is on it.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test contour_gui_test`. Expected: compile failure on
  `CommandBlockEnd::Abandoned`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[commandblock],[gutter]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][paneend],[contour][session][notification],[manager]"`
  Expected: PASS. A task touching `vthost` record replication (`adopt`) also runs `ctest --test-dir out/appleclang-debug -L daemon`.
- [ ] **Step 5: Tidy, format, commit**, message
  `contour: a command still running when its shell ends stops running; a finish notice scrolls to its block`.

---

### Task 13: The QML — the ⋯ button, header rows, the sticky header in chrome, hollow ticks

> **Depends on Step 2:** `PaneCard.qml`, `PaneHeader.qml` (`paneHeaderName`, `paneHeaderCwd`). **Depends on Step 1c:**
> `TitleBar.qml`'s `bandTitle`. **Depends on Step 3e:** the scrollbar in `SessionChrome.qml`.

**Files:**
- Create: `src/contour/qml/BlockActionsButton.qml`; add it to `_qml_files`.
- Modify: `src/contour/qml/SessionChrome.qml` — the block tooltip (`:93-128`) goes; a `BlockActionsButton` at
  `session.blockActionsAnchor`; the tick delegate (`:243-270`) draws `style` 1 as a border-only rectangle and takes its
  height from `size.scrollTick`.
- Modify: `src/contour/qml/ActionContextMenu.qml:100-125` — a `"header"` row delegate.
- Modify: `src/contour/qml/PaneHeader.qml`, `PaneCard.qml` (Step 2's) — the swap and `setStickyHeaderHost`.
- Modify: `src/contour/qml/TitleBar.qml` (Step 1c's) — `bandTitle` swaps to `win.activeStickyHeader` when set.
- Modify: `src/contour/test/QmlMocks.hpp` — `MockSession`: `blockActionsAnchor`, `openBlockActions()` (counted),
  `stickyHeader`, `jumpToStickyHeader()` (counted), `setStickyHeaderHost` (recorded), mark `style`; `MockMainController.hpp`:
  `activeStickyHeader`.
- Modify: `scripts/qml-size-literal-baseline.txt` (SessionChrome's count goes down by the tick literals replaced).
- Test: `src/contour/test/QmlComponents_test.cpp`, `MainWindowQml_test.cpp`, `Accessibility_qml_test.cpp`.

**Interfaces:**
- `BlockActionsButton.qml`, `objectName: "blockActionsButton"`: `required property var session`; `x/y/width/height`
  from the anchor (square `size.blockActions`, centred on the strip, top at the anchor's top, never beyond the strip:
  `x + width ≤ ` the grid's left edge); fill `colors` popover surface at `radii.blockActions`, glyph `⋯` in `type.body`;
  visible while the anchor is non-empty; click → `session.openBlockActions()`; `Accessible.role: Accessible.Button`,
  `Accessible.name: qsTr("Block actions")`. Shows and hides at once (no fade; nothing to stop under Reduce Motion).
  Terminal row: a one-cell `⋯` in `colors.tabText` on the bar fill.
- `ActionContextMenu.qml`: `row.kind === "header"` → a disabled `MenuItem` (`objectName: "contextMenuHeader"`) in
  `type.caption`, secondary text; the first header `type.footnote`, primary.
- `PaneHeader.qml`: while `session.stickyHeader.command` is non-empty, `paneHeaderName`/`paneHeaderCwd` hide and
  `paneHeaderSticky` shows `glyph command · detail` (glyph in `statusColor(outcome)`; `Text.ElideMiddle` on the
  command) and `paneHeaderJump` (`jumpLabel`, accent text, click → `session.jumpToStickyHeader()`,
  `Accessible.Button`). `PaneCard.qml` calls `session.setStickyHeaderHost(…)` whenever its header policy or
  `visiblePanes` changes, with `stickyHeaderHostFor` exposed through the provider as an invokable taking ints.
- `TitleBar.qml`: `bandTitle.text` is the sticky header's `glyph command · detail` while
  `win.activeStickyHeader.command` is set and the host is `TitleBand`; a `bandJump` link as in the pane header.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][blocks]`):
  - "the ⋯ button sits in the strip": `MockSession.blockActionsAnchor = Qt.rect(6, 40, 22, 17)` → `blockActionsButton`
    visible, `width == 22`, its right edge ≤ the terminal item's grid left; a click counts one `openBlockActions()`;
    an empty anchor hides it. No `commandBlockTooltip` object exists.
  - "a header row is a disabled label": a model `[ {kind: "header", title: "make"}, {kind: "separator"}, {kind:
    "command", title: "Copy Output", …} ]` → `contextMenuHeader` with text `make`, `enabled == false`.
  - "the sticky header swaps into the pane header": in a split, `stickyHeader = { glyph: "✓", command: "cmake --build",
    detail: "2m 14s", outcome: 0, jumpLabel: "Jump to Command ↑" }` → `paneHeaderSticky` visible reading
    `"✓ cmake --build · 2m 14s"`, `paneHeaderName` hidden; `paneHeaderJump` click counts one `jumpToStickyHeader()`;
    the card recorded `setStickyHeaderHost(PaneHeader)`; the single-pane case recorded `TitleBand` and `bandTitle`
    reads the same text.
  - "running is the accent in the header": `outcome: 2` → the glyph's colour equals `chromeStyle.statusColor(Running)`,
    never yellow.
  - "a running tick is hollow": a mark `{ position: 0.5, kind: 3, style: 1 }` → its delegate's `color` transparent and
    `border.width ≥ 1`; height `chromeStyle.size.scrollTick`.
  - Terminal row (under `ContourTui`): the ⋯ button is one cell; the pane header row shows the sticky text.
  - `Accessibility_qml_test.cpp`: the button is a `Button` named "Block actions"; the jump link a `Button`.
  - `MainWindowQml_test.cpp`: the MacOS row's ⋯ is 22 × 22 at radius 6 (canvas `blocks`).
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: FAIL, `blockActionsButton` not found.
- [ ] **Step 3: Implement.** No colour or size literal; every new element has an `objectName`.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][blocks]" && ctest --test-dir out/appleclang-debug -L lint --output-on-failure`
  Expected: PASS.
- [ ] **Step 5: Commit** (`editorconfig-checker` on the QML and the baseline; never clang-format QML), message
  `contour: a ⋯ button beside each block, header rows in menus, the sticky header in the pane header and title band`.

---

### Task 14: Snapshot rows, the Terminal row included

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` — `SnapshotScript` gains `BlockMenu`, `BlockActions`,
  `StickyPaneHeader`, `StickyTitleBand` (after whatever is last at execution); `ScriptAction` gains `OpenBlockMenu`,
  `HoverBlock`; a `SnapshotBlocks { None = 0, Sample }` column the four rows set.
- Modify: `src/contour/test/ChromeSnapshotScene.hpp` (the scene drives `MockSession` per the new columns).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp` (or the file pinning the table), `ChromeSnapshot_test.cpp`.

**Interfaces:**
- Rows: `block-menu` (one tab, single pane, `OpenBlockMenu`: the in-scene menu with four header rows and five
  commands); `block-actions` (`HoverBlock`: the ⋯ button at a block's head); `sticky-pane-header` (`SplitThree`, the
  active pane's header showing a failed block's sticky text); `sticky-title-band` (one pane, the band showing a
  succeeded block's sticky text).

- [ ] **Step 1: Write the failing tests.** The plan test's "one row per script, in enumerator order" and "names are
  unique" pass with the four rows; a new case checks the four names and that each sets `SnapshotBlocks::Sample`.
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: compile failure on `SnapshotScript::BlockMenu`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[snapshot]" && CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]" && CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"`
  Expected: PASS, no QML warnings.
- [ ] **Step 5: Tidy, format, commit**, message `test: snapshot the block menu, the ⋯ button and the sticky header in chrome`.

---

### Task 15: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts CMakeLists.txt`) and
  apply its findings; re-run the filters of every task it touches.
- [ ] **Step 2:** Every task's filter once more:

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[blockrail],[blockchip],[blockstatus],[gutter],[folding],[scrollbarmarks],[searchwalk],[blockfocus],[stickyheader],[commandblock],[input]"
out/appleclang-debug/src/vtrasterizer/vtrasterizer_test "[renderer]"
out/appleclang-debug/src/contour/contour_test "[chrometokens],[geometry],[fontcontrol],[config],[blockmenu],[contextmenu],[menutree],[actions],[menubar],[command],[stickyheader]"
out/appleclang-debug/src/contour/contour_gui_test "[uistyle],[contour][session],[windowmenus],[blockheader],[menus],[contextmenumodel],[cocoa],[manager],[windowsizing],[contour][gui][qml],[snapshot],[a11y]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][blocks],[snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, read from
  `ChromeSnapshotPlan.hpp`. Look at `block-menu.*` (header rows above five commands), `block-actions.*` (⋯ in the strip,
  never over a cell), `sticky-pane-header.*`, `sticky-title-band.*`, and `split-three.*`/`single-tab.*` (as after 3e).
- [ ] **Step 4: Owner manual checks**, batched per phase: listed in the ledger, not awaited. On this Mac,
  `out/appleclang-debug/src/contour/contour.app` with zsh and shell integration:
  - `ls; false; sleep 20` → rails: neutral, red with ✗, blue with ▸ (never yellow); hovering `ls` shows ✓ and ⋯;
    "Failed · exit 1 · 0.0 s" sits right of `false` in blank cells; "Running · N s" ticks.
  - powerlevel10k with an RPROMPT: the chip never covers it; a prompt filling the row shows only the rail mark.
  - ⋯ or right-click on a block → an NSMenu whose header names the command; "Collapse Output" shows
    "N lines hidden ⌄", a click on the chip (or Return while focused) expands it.
  - ⌘↑/⌘↓ walk the blocks, VoiceOver reading "false, Failed, exit 1, …"; ⌃↩ and ⇧F10 open the menu; ⎋ leaves focus;
    typing reaches the shell.
  - Scrolled back in a split: the pane header shows "✓ ls · 4 ms · Jump to Command ↑"; in a single pane the title band
    does; `show_title_bar: true` keeps the in-grid band.
  - Line numbers and timestamps on: numbers, then the strip, then the text; resizing keeps the page fit.
  - Scrollbar: a red tick for `false`, a hollow blue one while `sleep` runs, yellow ticks for a search.
  - `exit` while `sleep 100` runs in a split: no ▸ stays behind; Restart gives a clean pane.
  - A 10 s command in a background tab: a notification; clicking it brings the tab and scrolls to the block.
  - `ui_style: terminal`: the glyph column as before, the ⋯ a cell, menus in-scene, the sticky header in the header row.
  - `ssh` into a host without integration: one running block for the whole session.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Draws command blocks as a rail beside the output: neutral for success (✓ on hover), red with ✗ for a failure, the
    accent with ▸ while running, in a fixed strip after the line numbers and timestamps."
  - "Shows how a command went right of its prompt, only where those cells are blank, so an RPROMPT is never covered."
  - "Adds a ⋯ button and a native block menu (Copy Output, Copy Command, Rerun, Open in Pager, Collapse Output) whose
    header names the command and how it ended; the block tooltip is gone."
  - "Adds ⌘↑/⌘↓ to step through command blocks, ⇧⌘↑/⇧⌘↓ to select their output, and ⌃↩ or ⇧F10 to open a focused
    block's menu; VoiceOver says how each command went. Adds the FocusPreviousCommand, FocusNextCommand,
    SelectPreviousCommandOutput, SelectNextCommandOutput, OpenBlockMenu, ClearCommandBlockFocus and ToggleCommandFold
    actions and the BlockFocus input mode."
  - "Folded output reads "80 lines hidden ⌄"; the sticky command header moves into the pane header or the title band."
  - "Scrollbar ticks: failures solid, running commands hollow, search matches yellow; `scrollbar.marks` gains
    `running` and `search_matches`."
- [ ] **Step 6:** Mark Step 3f done in the plans index's step table and *Last update*, and on the board with
  `/status-board update`. Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-3f.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** `fillGutter` reads two neighbour rows' blocks per drawn row (O(rows × log blocks)), never a block's
    rows; one chip and its blank-run scan per visible block head; one redraw a second only while a running chip or
    sticky header is on screen; ticks bucketed per pixel as before. Callgrind on `bench-headless` before and after,
    recorded in the ledger.
  - **Risk:** medium-high. The gutter geometry changes on every native row (Task 2: page fit, hit-test, renderer
    agree through one function); a new input mode bit; the sticky header's press path changes under a chrome host; a
    menu entry kind reaches the Cocoa presenter.
  - **Coverage:** the `[blockrail]`, `[blockchip]`, `[blockstatus]`, `[blockmenu]`, `[stickyheader]` tables; the
    terminal's gutter, focus and abandon cases; the renderer's rail geometry; the session cases for focus, the
    notification, the ended pane; QML property tests on the native and Terminal rows; four snapshot scripts.

---

## Owner decisions

The run proceeds on each default.

1. **The rail on Windows and Linux too (R-a).** *Default: all three native rows draw the rail; only the Terminal row
   keeps the glyph column.* The alternative is the MacOS row only until Phase 5, as §2.1 keeps the Fluent and
   ContourLinux band geometry.
2. **The chips are terminal text (R-e, R-k).** *Default: the status and fold chips are drawn by the renderer in the
   terminal font over a rounded band, so they scroll with the cells and look the same in the TUI.* The canvas draws
   them as 11-pt system-font pills, which needs QML overlays tracking every visible block through smooth scrolling.
3. **Which ticks show by default (R-m).** *Default: Native2026 `[failures, running, search_matches]` (the canvas's
   "failures and running only", plus 3b's search); Classic keeps today's ticks; `commands` keeps implying running.*
4. **Block chords (R-j).** *Default: ⌘↑/⌘↓ and ⇧⌘↑/⇧⌘↓ on macOS in both editions; ⌃↩, ⇧F10, Return and ⎋ act only
   while a block is focused, on every OS; no new focus chords off macOS (the palette and Shell › Commands reach them).*
5. **The pane menu's block group (R-h).** *Default: the five block-menu rows under the header lead the menu, and
   "Select Output" leaves it (⇧⌘↑/⇧⌘↓, the palette and Shell › Commands keep it).*

## Spec gaps found

- §3.12's `RailSegment` has no value for a one-row block: `Single` is added (R-c).
- "24-pt hit area" cannot be met vertically on a row shorter than 24 pt without covering the next row: the hit is the
  row's height and at least 24 pt wide (R-k).
- Where the sticky header goes in Minimal splits, with `show_title_bar: true`, or in full screen with the band hidden:
  the grid, as today (R-l). The canvas's "In Minimal it uses the reserved 28 pt band" names no band for splits.
- The chip's wording for killed, crashed, interrupted and unknown endings, and VoiceOver's beyond one example (R-f).
- The ✓ on success is drawn only on hover; §3.12 and the canvas agree, but give no colour: the neutral rail's (R-d).
- How the block menu's header holds the tooltip's several lines: one header row each (R-h, R-i).
- Search-match ticks need the matched rows, which 3b's tally walk does not keep (Task 7, Depends on Step 3b).
- A daemon-hosted pane's rail, chip and focus read mirrored records (`CommandBlockStore::adopt`); §3.12 is silent.
  Pre-flight checks that the mirror's records carry what `BlockRail` reads; otherwise they draw no rail.
- The canvas's hovered-block tint and the block menu's chords are not in §3.12 (Non-goals).
