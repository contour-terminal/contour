# Semantic blocks: finishing #1010

THIS IS A DRAFT DOCUMENT

Design for the rest of the semantic-blocks feature set, so that
[#1010](https://github.com/contour-terminal/contour/issues/1010) can be closed. Output folding
(#2077), block-atomic eviction (#836, #2085), prompt navigation, "copy last command" and the OSC 3008
context model already shipped; this document covers everything that is still missing, plus the
features other terminals have converged on and that were judged worth adding.

- [1. Goal and scope](#1-goal-and-scope)
- [2. Vocabulary](#2-vocabulary)
- [3. Architecture overview](#3-architecture-overview)
- [4. Foundation: block identity and the record store](#4-foundation-block-identity-and-the-record-store)
- [5. The gutter](#5-the-gutter)
- [6. Scrollbar marks, sticky header, evicted-output placeholder](#6-scrollbar-marks-sticky-header-evicted-output-placeholder)
- [7. Acting on a block](#7-acting-on-a-block)
- [8. Finish notifications](#8-finish-notifications)
- [9. Recent-commands picker](#9-recent-commands-picker)
- [10. Shell integration scripts](#10-shell-integration-scripts)
- [11. Configuration and the settings page](#11-configuration-and-the-settings-page)
- [12. Daemon mode](#12-daemon-mode)
- [13. Cross-cutting concerns](#13-cross-cutting-concerns)
- [14. Testing strategy](#14-testing-strategy)
- [15. Delivery](#15-delivery)
- [16. Verification items and follow-ups](#16-verification-items-and-follow-ups)

---

## 1. Goal and scope

### 1.1 Goal

Every item of #1010's wishlist ships, together with the block features other terminals have proven
useful, so that #1010 is closed by one pull request.

### 1.2 In scope

| #1010 item / addition | Where |
|---|---|
| Blocks carry their command, exit status, times and working directory | §4 |
| Per-block working directory used for clickable relative paths | §7.4 |
| Line numbers in the gutter: absolute, relative, hybrid | §5.3 |
| Fold markers (exist) — plus a collapsed-size label and vi `z` keys | §5.2, §5.5 |
| A block whose prompt was evicted: keep its command with an "output evicted" placeholder (at the very top always, elsewhere wherever the header shows) | §6.3 |
| Line-mark indicator in the gutter | §5.2 |
| Per-line timestamps in the gutter | §5.4 |
| Exit-status colouring of blocks (gutter) | §5.2 |
| Exit-status marks on the scrollbar | §6.1 |
| Sticky command header | §6.2 |
| Select / copy / open a block's output, clear back to the prompt | §7 |
| Notifications when a long command finishes | §8 |
| Recent-commands picker across all open sessions | §9 |
| Shell scripts emit `133;B` and the command line; PowerShell integration | §10 |
| Every new key editable on the GUI settings page | §11 |
| Everything works in daemon-attached `contour client` sessions | §12 |

### 1.3 Out of scope

- Persisting evicted output to disk (#1010 mentions it; rejected — a second store of scrollback to
  secure and bound, for little gain).
- Sharing a block as a web link, AI features on blocks (Warp, Wave): need a hosted service.
- Click-to-move-cursor in the prompt (`133;A;click_events`, `cl=`) and erasing the prompt on resize
  (`redraw=`): prompt editing, not blocks — a separate ticket (§16).
- OSC 633 (VS Code's own sequences) and `133;A;aid=` for nested shells: follow-ups (§16).
- Persisting the recent-commands list across restarts: the shell's own history covers it.
- Editing key bindings on the settings page (the page stays a viewer for bindings).

---

## 2. Vocabulary

- **Block** — one shell command cycle: the prompt the user typed at, and the output the command
  printed. Delimited on the grid by the OSC 133 marks (`A` prompt start, `B` prompt end / input
  start, `C` output start, `D` command finished) or, when no shell integration is installed, by the
  OSC 3008 `type=shell`/`type=command` contexts (arbitrated by `MarkArbiter`).
- **Head** — the logical line carrying `LineFlag::Marked` where a block's prompt begins.
- **Block id** — `CommandBlockId`, the terminal's own monotonic `uint32` handle for one block.
  Zero means "no block".
- **Record** — `CommandBlockRecord`, everything the terminal knows about one block that the grid
  cannot hold: command line, exit status, times, working directory.
- **Visible row** — a grid row a folded viewport actually shows; a collapsed fold counts as its one
  head row. All row arithmetic users see (scrollbar, relative line numbers, vi motions) counts
  visible rows.

---

## 3. Architecture overview

```
 shell ──OSC 133 / OSC 3008 / OSC 7──▶ Screen (parser thread)
                                          │ stamps LineFlags (as today)
                                          │ stamps CommandBlockId + bornAt on rows   (§4.2, §5.4)
                                          ▼
                                 Terminal ── CommandBlockStore (records, revision)    (§4.3)
                                    │   │        ▲ adopt()  ◀── ScreenMirror (daemon)   (§12)
                     fillRenderBuffer   │ Events::commandBlockFinished(summary)        (§4.5)
                                    ▼   ▼
   RenderBuffer { cells, gutter[col], annotations, stickyHeader }   TerminalSession (GUI thread)
        │                                                           ├─ finish notifications (§8)
        ▼                                                           ├─ scrollbar marks model (§6.1)
   vtrasterizer::Renderer                                           ├─ block actions (§7)
   (gutter columns, annotations, sticky header)                     └─ picker snapshot (§9)
```

Layering is unchanged: everything that decides lives in pure, dependency-free units with their own
tests; Terminal and TerminalSession wire them.

| Unit | Module / directory | Pure? |
|---|---|---|
| `CommandBlockStore`, `CommandBlockRecord` | `vtbackend/shell/` | yes (clocks injected) |
| `GutterLayout`, gutter glyph/precedence table, line-number arithmetic | `vtbackend/screen/Gutter.hpp` | yes |
| `stickyHeaderFor(...)` decision | `vtbackend/shell/` | yes |
| `scrollbarMarks(...)` bucketing | `vtbackend/shell/` | yes |
| `sanitizeCommandLine(...)` | `vtbackend/shell/` | yes |
| `finishNotificationFor(...)` | `contour/platform/` | yes |
| `rankRecentCommands(...)` | `contour/command/` (Qt-free) | yes |
| Click counting (`ClickCounter`) | `vtbackend/input/` | yes (clock injected) |

---

## 4. Foundation: block identity and the record store

### 4.1 Why a per-row id plus a record table

Block metadata has to survive scrolling, window resizes (a column change reflows and destroys stable
row ids) and eviction. Three shapes were weighed:

1. **Chosen: a block id on every row + a record table.** Rows carry `CommandBlockId`; the table maps
   ids to records. Reflow already carries per-row data onto every wrapped chunk (as it does for the
   OSC 3008 `ContextId`), so ids survive resizes; row → record is O(1).
2. Id on the head only + a sorted index: same memory, but row → block needs a search and every
   resize invalidates the index.
3. A side table keyed by stable row id (the `FoldState` shape): a column resize destroys stable ids,
   so every window resize would erase exit codes and timestamps. Rejected.

### 4.2 Stamping rows

- `Line` gains `CommandBlockId _blockId` (boxed `uint32_t`). It goes into the padding after
  `Line::_dirty`: the tail stays 32 bytes and the `sizeof(Line) == sizeof(LineSoA) + 32`
  static_assert keeps holding (layout in §5.4). **Zero memory cost per line.**
- Stamped exactly where the context id is: `Screen::updateCursorIterator()` adopts the active block
  id onto the line the cursor arrives on; a `133;A` explicitly adopts the new id onto the current
  line (the line a `;D` closes is routinely the line the next prompt starts on — last writer wins,
  as for contexts). Not stamped per write: one store per cursor-line change.
- Like `ContextId`, the id is **not** head-only: `LogicalLineAttributes` in `Grid.cpp` gains
  `blockId`, applied to every chunk reflow emits. `Line::reset()` clears it.
- Not dirtying: like `adoptContext`, adopting a block id does not mark the line dirty (derived
  state; the daemon carries it with the line's next real change and in snapshots).
- Primary screen only. The alternate screen's rows keep id 0.

### 4.3 The record and the store

```cpp
// vtbackend/shell/CommandBlock.hpp
using CommandBlockId = boxed::boxed<uint32_t, detail::CommandBlockIdTag>; // 0 = none

enum class CommandBlockState : uint8_t { Prompting = 0, Running, Finished };
enum class CommandLineSource : uint8_t { None = 0, Reported, Recovered };
enum class CommandBlockEnd : uint8_t { Reported = 0, Implicit };    // ;D / 3008 end vs. superseded

struct CommandBlockRecord
{
    CommandBlockId id;
    CommandBlockState state = CommandBlockState::Prompting;
    CommandBlockEnd end = CommandBlockEnd::Reported;   // meaningful once Finished
    std::string commandLine;                    // RAW, as reported/recovered; sanitised per use (§10.4)
    CommandLineSource commandLineSource = CommandLineSource::None;
    std::optional<int> exitCode;                // from ;D, or ContextOutcome::asShellExitCode()
    ContextOutcome outcome;                     // signal / crash / interrupt when OSC 3008 told us
    std::chrono::system_clock::time_point promptStartedAt;
    std::optional<std::chrono::system_clock::time_point> commandStartedAt;
    std::optional<std::chrono::steady_clock::duration> duration;   // ;C → ;D
    std::string workingDirectory;               // snapshot at ;C (§4.4)
    ContextLocality locality = ContextLocality::Unknown;
    int64_t headStableId {};                    // position cache (§6.1), valid for headIdGeneration
    uint64_t headIdGeneration {};
};
```

`CommandBlockStore` (owned by `Terminal`, primary screen):

- Constructed with `CommandBlockStoreLimits { size_t maxRecords = 1000; }`, a
  `core::platform::IClock const&` (durations) and a `core::platform::WallClockRef` (displayed
  times). Tests inject `ManualClock`/`ManualWallClock`. Configuration at construction; no setters.
- Transitions (all total — the protocol is lenient, a malformed order is an outcome, not an error):

| Event | Effect |
|---|---|
| `A` (prompt start) | A `Prompting` record that never reached `C` is **discarded** (empty Enter, Ctrl-C at the prompt); a `Running` record is closed as `Finished` with `end = Implicit` and no exit code (a nested shell's prompt — `ssh`, `bash` — or a missing `;D`). An implicit close raises **no** finish event: the `ssh` the user is still inside has not finished. Then a new record is minted with `promptStartedAt = now`. |
| `B` (prompt end) | Nothing in the store (the column lives on the line, as today). |
| `C` (output start) | Current record → `Running`; `commandStartedAt`; command line from `cmdline_url=` (Reported) or recovered from the grid between the `;B` column and the output start (Recovered, only when `;B` was seen); working-directory snapshot (§4.4). `C` with no current record mints one (integration sourced mid-session). |
| `D` (finished) | `Running` record → `Finished`, exit code, duration. `D` with no `Running` record (tcsh's unconditional `D`, a nested shell's exit) is ignored. |
| OSC 3008 `type=command` end | Same as `D`, with `ContextOutcome` (signal, crash) — only when `MarkArbiter::contextMayNotify()`. When OSC 133 owns the session, a 3008 end may still **enrich** the matching `Finished` record's outcome (signal names), never re-close it. |

- Bounded by `maxRecords`: the oldest records are dropped first. Records deliberately **outlive their
  rows** (the picker and the evicted-output placeholder need commands whose rows are gone); a row
  whose id no longer resolves is treated as "no block".
- `revision()` bumps on every change; caches (gutter, scrollbar marks, sticky header) key on it.
- `adopt(record, AdoptMode::{Live, Snapshot})` for the daemon mirror (§12).
- `clear()` on RIS/hard reset only.

### 4.4 Working-directory snapshot

At `C`, the store records the directory through the existing OSC 3008-aware resolver
(`vtbackend::resolveWorkingDirectory(contexts, osc7, self, purpose)` in `core/WorkingDirectory.hpp`):
OSC 3008 `effectiveWorkingDirectory` first, then OSC 7, together with its `ContextLocality`.
Snapshot at `C` rather than `A` because OSC 7 is emitted from the shell's precmd and the command runs
in the directory in effect when the user pressed Enter.

### 4.5 Events and threading

- `Terminal::Events` gains `commandBlockFinished(CommandBlockSummary const&)` — a value copy of the
  record fields a frontend needs (id, command line, exit code, outcome, duration, directory). Raised
  only for a **reported** end (`;D`, or an OSC 3008 `end=` while 3008 owns the marks), never for an
  implicit close. Raised on the parser thread with `Terminal::_stateMutex` held, exactly like
  `progressChanged`: an implementation must not read terminal state and defers to the GUI thread.
- No other new event: the GUI-side views (scrollbar marks, sticky header state) compare
  `CommandBlockStore::revision()` on the existing render/update path.
- **Removed:** `ShellIntegration` / `NullShellIntegration` / `Terminal::setShellIntegration` — the
  interface has no production implementation; its tests move to the store.
- **Rebased:** `SemanticBlockTracker` (DEC mode 2034) keeps its session token and query encoding but
  reads blocks from the store instead of keeping its own deque. Mode 2034 still gates only the query
  protocol.

### 4.6 User marks are not prompt marks

- New `LineFlag::UserMark` (head-only, in `HeadOnlyLineFlags`, one row in `VTBACKEND_LINE_FLAGS`).
- Vi `mm` (`ViCommands.cpp` `toggleLineMark`) toggles `UserMark` instead of `Marked`. Consequence:
  a manual mark no longer counts as a block start for block-atomic eviction (`Grid::clampHistory`)
  or folding.
- `[m`/`]m`, the `im`/`am` text objects and `ScrollMarkUp`/`ScrollMarkDown` keep visiting **both**
  kinds of mark, so no navigation behaviour is lost.
- The deprecated `SETMARK` sequence keeps setting `Marked` (it is a prompt mark by definition).

---

## 5. The gutter

### 5.1 Layout

The strip left of the grid becomes an ordered list of **segments**, described by one table:

| Segment | Columns | Default | Content |
|---|---|---|---|
| timestamps | width of a formatted sample | off | per-line time (§5.4) |
| line numbers | `line_number_width` | off | §5.3 |
| block column | 1 | on | fold controls + exit status + user marks (§5.2) |

- One function, `gutterLayoutFor(GutterSettings, FoldingSettings)`, returns the segments with their
  first column and the total column count. It replaces `gutterWidthFor` (FontControl.hpp); the
  window geometry, the hit-test (`isInGutter`), the renderer and `Terminal::fillGutter` all read it,
  so the reserved width and what is drawn can never disagree.
- The block column is reserved when fold markers, exit status or user marks are enabled — all three
  default to on in the configuration, and fold markers already reserve one column today, so **no
  user's column count changes on upgrade**. The engine's own `vtbackend::GutterSettings` defaults
  every switch to off, so an embedder or test that constructs default `Settings` reserves no gutter,
  as before; the configuration supplies the user-facing defaults.
- The width is constant for a given configuration. It never grows with content: a changing gutter
  would resize the page and reflow mid-session.
- `RenderGutterCell` gains `ColumnOffset column` (negative: `-total .. -1`); `Renderer::renderGutter`
  draws at that column instead of the hard-coded `-1`.

### 5.2 Block column

What one row shows, by precedence:

1. Fold head of an expanded / collapsed block (existing glyphs U+10F000 / U+10F001).
2. User mark (`LineFlag::UserMark`) on a non-head row — glyph ◆ in the scheme's mark colour. Over a
   fold body it replaces `│` for that row. On a head row the fold control wins (a prompt is already
   a navigation target).
3. Fold body / body end (existing `│` / `┕`).
4. Running block's head: ▸.
5. Finished block with no foldable output: ● on its head.

Colouring: every glyph of a block takes the block's outcome colour from the colour scheme's new
`block_status` slots — `success` (defaults to the fold-marker colour, i.e. neutral), `failure`
(defaults to the palette's red), `running` (defaults to the palette's yellow). A block with no exit
code known uses `success`. Hover highlighting keeps working as today. With `gutter.exit_status: false`
every glyph keeps today's fold-marker colour and the ▸ / ● status glyphs are not drawn; with
`gutter.user_marks: false` the ◆ is not drawn.

Hovering the block column shows a tooltip with exit code (and signal), duration, start time,
working directory and command line — a QML `ToolTip` fed by a `TerminalSession` invokable that reads
the record of the hovered row.

### 5.3 Line numbers

`gutter.line_numbers: off | absolute | relative | hybrid`, matching vim:

| Value | vim | Cursor row | Other rows |
|---|---|---|---|
| `off` | `nonu nornu` | — | — |
| `absolute` | `nu` | its absolute number | absolute numbers |
| `relative` | `rnu` | `0` | distance from the cursor row |
| `hybrid` | `nu rnu` | its absolute number | distance from the cursor row |

- **Absolute numbering** counts physical rows from the session's first row, evicted rows included:
  `number(row) = evictedRowCount + (row - addressableTop) + 1`. `Grid` gains `uint64_t
  _evictedRowCount`, advanced by line-wise eviction, block-atomic trims, ED 3, `ClearToPrompt` (§7.3)
  and rows dropped by a reflow that overflows capacity; reset by RIS. Numbers are stable while
  history scrolls away; a column resize renumbers the rows below the (anchored) top, which is the
  honest answer since reflow changes the physical rows.
- **Relative distance** counts **visible** rows (`visibleDistance` from `Folding.hpp`), so a collapsed
  fold counts as one row — exactly what `j`/`k` count, so `12k` lands on the row labelled 12.
- **Which cursor**: the vi cursor in vi mode, the terminal cursor otherwise. Distances keep counting
  when the cursor is scrolled off screen.
- Every physical row is numbered, wrapped continuations included (vim leaves those blank, but
  Contour's `j`/`k` step over them).
- Fixed width (`line_number_width`, 3–10, default 6); a number wider than that shows its last
  `width - 1` digits after `…`. Right-aligned, in the scheme's `gutter_text` colour.

### 5.4 Per-line timestamps

- `Line` gains a 24-bit "seconds since this session started" field, in the last three free padding
  bytes. Layout of the tail after `_revision`:

  ```
  +24 bool _dirty | +25..+27 uint8_t _bornAt[3] | +28..+31 CommandBlockId _blockId   → still 32 bytes
  ```

  Zero memory cost. Horizon: 2^24 s ≈ 194 days of continuous uptime; a line stamped beyond it
  stores the saturated value and the gutter shows no time for it.
- Stamped in `Screen::updateCursorIterator()` when the arriving line's stamp is still zero ("the
  time the cursor first reached this line" — #1010's "time it was moved to via LF"). `Line::reset()`
  clears it. Carried through reflow on every chunk (`LogicalLineAttributes`).
- Unlike the block id, setting the stamp **marks the line dirty** — once per line, the moment it is
  first reached — so the daemon delta carries the stamp (and the block id adopted with it) even for a
  line that is never written to, such as a blank line in a command's output.
- The clock is read **once per PTY read batch**: `Terminal` samples the injected wall clock when a
  batch starts and the parser stamps from that value. No clock read per line.
- Display: `gutter.timestamp_format` (default `%H:%M:%S`, a `std::format` chrono spec, local time
  — the time-zone conversion is injected through `TerminalClocks` so tests are zone-independent),
  validated at load by formatting a sample (invalid → logged, default kept). A row shows its time
  only when it differs from the row above, so long outputs do not repeat the same second.

### 5.5 Folding additions

- A collapsed fold's head row gets a dim, render-only annotation after its text: `⋯ 1,234 lines`.
  Drawn through a new `RenderBuffer::annotations` vector (line, column, text, attributes) — not
  through `cells`, which selection, hit-testing and the accessibility bridge walk — so it is never
  selected or copied. Clipped at the page edge.
- Vi normal mode gains vim's fold keys: `za` toggle, `zo` open, `zc` close, `zM` close all, `zR`
  open all — rows in the `ViInputHandler.cpp` command table mapping to the existing fold actions.

---

## 6. Scrollbar marks, sticky header, evicted-output placeholder

### 6.1 Scrollbar marks

- A tick layer over the stock QML `ScrollBar` in `SessionChrome.qml`, fed by a `TerminalSession`
  property (a list of `{ position: 0..1, kind, targetLine }`).
- One pure function builds it: `scrollbarMarks(heads, userMarks, visibleRowCount, trackPixels,
  kinds)`:
  - positions are in **visible** rows (fold-aware), matching the scrollbar's own range
    (`Viewport::scrollableLineCount`);
  - ticks that fall on the same pixel merge, so thousands of commands cost at most the track's
    pixel height; within a pixel `failure` wins, then `running`, then `user_mark`, then `command`.
- Positions come from each record's cached `headStableId`: a recompute is O(records), not
  O(scrollback). A stable-id generation bump (column resize) triggers one rescan of the heads —
  reflow already walks the whole scrollback, so this adds no new order of cost. User-mark positions
  come from a small set of stable ids maintained by `toggleLineMark`, rescanned the same way.
- Recomputed when the store revision, the fold revision, the history size or the track height
  changes; coalesced to at most once per frame.
- Clicking a tick scrolls the viewport so that block's head is at the top (expanding it if folded).
- Config (profile): `scrollbar.marks: [failures, commands, user_marks]` (default all; `[]` disables).
  Hidden with the scrollbar in the alternate screen, as today.
- **The scrollbar is shown by default** (owner decision): `scrollbar.position` defaults to `right`
  instead of `hidden`, so the marks are visible out of the box. `position: hidden` (or the settings
  page) turns it off; the release notes say so, because every user's window gains a scrollbar on
  upgrade.

### 6.2 Sticky command header

- **When**: primary screen; the top visible row belongs to a block whose head row is above the
  viewport; that block is `Running` or `Finished` with at least one output row; and the profile's
  mode allows it:
  - `sticky_header.mode: never | scrolled | always` (default `scrolled`): `scrolled` shows it only
    while the viewport is scrolled back into history; `always` also while following live output
    (e.g. `make` stays visible above its own output).
- **What**: the block's input row — the logical line carrying `PromptEnd`, else the head — copied
  from the grid with its real colours, truncated to the page width, plus a right-aligned chip in the block's status colour
  (render-only): `✓`/`✗ 2` and the duration, or `running 12s`.
- **How**: `RenderBuffer` gains `std::optional<RenderStickyHeader>` (cells, chip, block id, kind).
  The renderer draws it over screen row 0 with the scheme's `sticky_header.background` and a
  one-pixel separator in `sticky_header.separator`. The decision is the pure
  `stickyHeaderFor(topRowBlock, headVisible, state, mode, scrolledIntoHistory, screenType)`.
- **Input**: a press on row 0 while the header is shown is consumed (no selection starts there) and
  scrolls the viewport to the block's head, expanding a collapsed fold. Exception (Task 8.6a): while
  the scrollbar is shown, a press inside its strip reaches the QML ScrollBar, not the header.
- Not exposed to the accessibility bridge (it duplicates a row that exists).

### 6.3 Evicted-output placeholder

- When the top visible row belongs to a block whose head was evicted (the record survives, its
  `headStableId` is below the grid's stable floor), the same overlay shows
  `⋯ <command> — earlier output evicted` instead of the input row it can no longer copy (owner
  decision: the header must not vanish just because the prompt left the scrollback):
  - at the very top of the scrollback, always (independent of `sticky_header.mode`);
  - anywhere else, wherever the mode would show a header for that viewport position (`scrolled`:
    while scrolled into history; `always`: always; `never`: not at all).
- The command is known even without `cmdline_url`, because the store recovers it from the grid at
  `C`, while the text still exists (§4.3).
- `sticky_header.show_evicted: true` (profile), independent of `mode` — `never` does not hide it.
- Whole blocks evicted by block-atomic eviction leave no placeholder (they are gone; the picker still
  lists them while their record lives).

---

## 7. Acting on a block

### 7.1 Selecting and copying

- `SelectCommandBlock { part: output | input | all, target: pointer | cursor | last }` — one
  parameterised action. `CopyLastCommandPrompt/Output/Block` stay.
- **Ctrl+triple-click selects the clicked block's output** (Ghostty, Konsole). Mouse bindings gain an
  optional click count:
  ```yaml
  - { mouse: Left, clicks: 3, mods: [Control], action: SelectCommandBlock, part: output, target: pointer }
  ```
  `MouseInputMapping` gains `std::optional<uint8_t> clickCount` (absent = any count, today's
  meaning). Click counting is extracted from `Terminal::handleMouseSelection` into a pure
  `ClickCounter` (injected clock) so bindings and selection read **one** count; a binding that names
  a click count is consulted before the terminal's multi-click selection, otherwise the third click
  would extend a word selection.
  Known interaction: the first click of a Ctrl+triple-click on a hyperlink still follows it (the
  default `Ctrl+Left → FollowHyperlink`), as in Ghostty.
- `CopySelection { format, fallback: none | last_command_output }` — **off by default**, so an
  accidental copy on an empty selection never replaces the clipboard with a huge build log.
- Context menu rows for the block under the pointer: *Select output*, *Copy output*,
  *Copy command*, *Open output in pager* — next to the existing *Toggle Output Fold*. The block
  actions (`SelectCommandBlock`, `OpenCommandOutput`, and the new `CopyCommandBlock` /
  `CopyCommandLine`) carry an optional block id, so a menu row acts on the block that was
  right-clicked even if the pointer moves before the row is chosen.

### 7.2 Opening output elsewhere

`OpenCommandOutput { target, program, placement: split | tab | detached, format: plain | sgr }`:

- `program` empty → `command_blocks.pager` (default `less -R` on Unix, `more` on Windows).
- `split` / `tab`: the output (with SGR when `format: sgr`, via `Line::toUtf8WithSgr`) is written to
  a private temporary file (owner-only permissions, unique name under a per-process directory) and
  opened in a new pane running `program <file>`, through
  `TerminalSessionManager::createBackingSession`'s existing command override. The file is deleted
  when that pane's session closes; the per-process directory is removed at exit.
- `detached`: plain text piped into the program's stdin with no pane (`wl-copy`, a notes script) —
  through a new `ExternalLauncher::runWithStdin(program, arguments, input) ->
  std::expected<void, SpawnError>` behind the existing DI interface (the error enum every other
  program-launching method already uses; the Qt adapter owns its `QProcess` until it finishes).
- Split and tab placements are refused in a daemon-attached session (the pager would run on the
  client while the output lives on the host); `detached` works there.
- Default binding: `Ctrl+Shift+G` → last output in the pager (kitty's binding), if the plan confirms it
  is free.

### 7.3 Clearing back to the prompt

`ClearToPrompt`: drops every row above the current prompt's head — scrollback and page alike — and
moves the head to the top of the page (kitty `to_cursor`, iTerm2 "Clear to Last Mark"). A `Grid`
operation; dropped rows count as evicted (§5.3), and records, folds and marks fall away as on
eviction. No-op when no prompt is known.

### 7.4 Paths relative to the command's directory

`Terminal::localPathAtMousePosition` and the hint-mode `filepath` resolution resolve relative paths
against the **clicked row's block** working directory, through `resolveWorkingDirectory`, instead of
today's OSC 7 value. A record whose locality is `Foreign` (ssh, container, VM) refuses local opening
exactly as OSC 3008 already does. No record for the row → today's behaviour.

---

## 8. Finish notifications

- `Events::commandBlockFinished(summary)` → `TerminalSession` (deferred to the GUI thread) → the pure
  decision `finishNotificationFor(summary, policy, visibility) -> std::optional<NotificationRequest>`
  in `contour/platform`.
- **Visibility** is a new `SessionVisibility { Focused, VisibleUnfocused, Hidden }` computed by
  `TerminalSessionManager`: focused session in the active window; its tab shown but focus elsewhere;
  another tab active or the window minimised.
- **Policy** (profile):

  ```yaml
  notify_on_command_finish:
    when: unfocused        # never | unfocused | hidden | always
    min_duration: 10       # seconds; shorter commands never notify
    outcome: any           # any | failure
    action: notify         # notify | bell | notify_bell
    clear_on: focus        # focus | next | never
  ```

  **On by default** (`unfocused`, 10 s): the case it serves — a build in a background tab — is when
  nobody thinks to turn it on; the 10 s floor keeps ordinary commands quiet.
- **Delivery** through `Notifier` on Linux (FreeDesktop or portal) and the tray message elsewhere —
  routed through the **window**, not the pane: only the active tab's `SessionChrome` listens to a
  session's notification and bell signals today, so a background tab's notification (the case this
  feature exists for) would otherwise be dropped on Windows and macOS. Clicking a notification
  focuses its tab on Linux; a tray message does not say which message was clicked, so elsewhere the
  click only raises the window. Title: `✓ make finished` / `✗ make failed (exit 2)` / `✗ make killed (SIGSEGV)`; body:
  `took 3m 12s · ~/src/contour · <tab name>`. One notification id per session (a newer finish
  replaces the older one); clicking focuses the tab where the platform supports activation;
  `clear_on: focus` withdraws it via `discardDesktopNotification`. `bell` reuses the profile's `bell:`
  sound and taskbar alert.
- Text shown is sanitised (§10.4).

---

## 9. Recent-commands picker

- **Source**: on open, `TerminalSessionManager` snapshots every open session's store (command,
  exit code, directory, finish time, session) — one short read per session under its lock, at most
  `max_records` each. No separate history to maintain; closing a tab takes its commands with it.
  Daemon-attached sessions take part through their mirrored stores.
- **Ranking**: pure, Qt-free `rankRecentCommands(rows, currentSession, query)` in `contour/command`:
  identical commands collapse into one row with a count and their newest run; the current session's
  commands first (newest first), then the others by recency; filtered with the palette's existing
  `fuzzyMatch`/`fuzzyScore`, match positions highlighted.
- **UI**: the command palette's QML shell, generalised with a row-source seam (a source + an
  "on accept" callback) so it has two modes: actions (today) and recent commands. A row shows the
  command, an ✓/✗ dot, the shortened directory (`~/src/contour`), age (`3m ago`) and, for other
  sessions, the tab name. Opened by a new `OpenRecentCommands` action (default `Ctrl+Alt+R`, VS Code's,
  if free) and an *Open Recent Commands* palette entry.
- **Accept**: Enter **inserts** at the prompt; Shift+Enter inserts and runs (a `\r` after the paste).
  Insertion goes through the paste path, so shells with bracketed paste do not run a multi-line command
  early. Before insertion the text is stripped of ESC and other control characters (§10.4).
- **Guard**: acts only when the focused session's current block is `Prompting`. In vim, `less` or
  while a command runs, the picker shows "not at a shell prompt" and inserts nothing.

---

## 10. Shell integration scripts

### 10.1 `133;B` — prompt end

Appended to the prompt so the terminal knows where typed input begins:
bash `\[\e]133;B\e\\\]` at the end of `PS1` (guarded against double insertion when re-sourced or when
a prompt framework rewrites `PS1`), zsh `%{…%}` at the end of `PROMPT`, fish after `fish_prompt`,
tcsh `%{…%}` in `prompt`.

### 10.2 The command line

kitty's `133;C;cmdline_url=<percent-encoded>`, which Contour already parses:

- bash: the vendored bash-preexec passes the command line to `preexec` as `$1`; encoded in pure bash.
- zsh: `preexec`'s `$1`. fish: `fish_preexec`'s `argv`.
- tcsh: no clean hook — sends `B` only; the store recovers the command from the grid (§4.3).

### 10.3 Native integrations and PowerShell

- fish integrates natively: it marks prompts (`A`, `C`, `D`) from 4.0.0, sends the command line
  from 4.0.1 and `B` from 4.3.0 (`no-mark-prompt` turns it off). The script detects the version and
  adds only what that fish leaves out, never a second `A`/`C`/`D`.
- Nushell (≥ 0.111) integrates natively: documented, no script. It never sends `cmdline_url` (the
  store recovers the command from the grid), and it emits `133;A;k=s` before every continuation
  line. Contour therefore parses the prompt kind: an `A` with `k=s`, `k=c` or `k=r` (secondary,
  continuation, right prompt) does **not** start a block.
- **PowerShell** (new; installed like the others with
  `contour generate integration shell pwsh to <FILE>`): wraps `prompt` — reports `$?` /
  `$LASTEXITCODE` as `D`, emits OSC 7 when the location is a filesystem path, then `A`, the original
  prompt, `B` — and wraps `PSConsoleHostReadLine` to emit `C;cmdline_url=` once the line is accepted
  (VS Code's `shellIntegration.ps1` approach; a key handler on Enter would emit `C` before the
  newline). One more row in the embedded-script table.
- **Risk**: OSC 133 must survive ConPTY to reach Contour on Windows. Contour ships its own
  `conpty.dll`/OpenConsole, which passes VT through since Windows Terminal 1.22; verified first by an
  automated probe (§16), and the PowerShell script is held back if it fails.

### 10.4 Command lines are untrusted

A command line arrives from whatever wrote to the pty. `sanitizeCommandLine(text, purpose)`:

- `Display` (notification, picker row, sticky-header chip, tooltip): C0/C1 controls and bidi
  overrides are replaced with visible placeholders.
- `Insert` (picker): additionally strips ESC entirely — a crafted `cmdline_url` carrying `\e[201~`
  would otherwise end bracketed paste early and run the remainder instead of only inserting it.

---

## 11. Configuration and the settings page

### 11.1 Keys

| Scope | Key | Type / values | Default | Settings page |
|---|---|---|---|---|
| global | `gutter.exit_status` | bool | `true` | checkbox |
| global | `gutter.user_marks` | bool | `true` | checkbox |
| global | `gutter.line_numbers` | `off \| absolute \| relative \| hybrid` | `off` | dropdown |
| global | `gutter.line_number_width` | int 3–10 | `6` | number |
| global | `gutter.timestamps` | bool | `false` | checkbox |
| global | `gutter.timestamp_format` | chrono format | `%H:%M:%S` | text (validated) |
| global | `command_blocks.max_records` | int ≥ 1 | `1000` | number |
| global | `command_blocks.pager` | command line | `less -R` / `more` | text |
| global | `folding.enabled` / `show_markers` / `auto_collapse_on_new_command` | bool | existing | checkboxes |
| global | `folding.on_jump_into_fold` | `expand \| skip` | existing | dropdown |
| profile | `scrollbar.marks` | list of `failures`, `commands`, `user_marks` | all | three checkboxes |
| profile | `scrollbar.position` (existing key) | `hidden \| left \| right` | **`right`** (was `hidden`) | dropdown (exists) |
| profile | `sticky_header.mode` | `never \| scrolled \| always` | `scrolled` | dropdown |
| profile | `sticky_header.show_evicted` | bool | `true` | checkbox |
| profile | `notify_on_command_finish.when` | `never \| unfocused \| hidden \| always` | `unfocused` | dropdown |
| profile | `notify_on_command_finish.min_duration` | seconds | `10` | number |
| profile | `notify_on_command_finish.outcome` | `any \| failure` | `any` | dropdown |
| profile | `notify_on_command_finish.action` | `notify \| bell \| notify_bell` | `notify` | dropdown |
| profile | `notify_on_command_finish.clear_on` | `focus \| next \| never` | `focus` | dropdown |
| colour scheme | `block_status.success` / `failure` / `running` | colour | derived (§5.2) | colour slots |
| colour scheme | `gutter_text` | colour | dimmed foreground | colour slot |
| colour scheme | `sticky_header.background` / `separator` | colour | derived | colour slots |

`gutter` and `command_blocks` are global for the reason `folding` is: they shape the page geometry
or describe how the terminal reads a protocol, not how one pane looks. The `folding.*` keys predate
this work but join the settings page with it, because the block column now merges fold markers with
exit status.

Config structs (`contour/config/Config.hpp`): `GutterConfig`, `CommandBlocksConfig` (global),
`StickyHeaderConfig`, `FinishNotificationConfig` and `ScrollBarConfig::marks` (profile). Plain bools
are YAML fields converted at the boundary (AGENT.md's documented carve-out); enums use the existing
`ConfigEnum` token tables; each struct gets a `ConfigDocumentation.hpp` entry, so
`contour generate config` and the website's configuration reference list every key. Mapping into
vtbackend settings happens where `folding` is mapped today.

### 11.2 The settings page

- Profile keys: a new **"Command blocks"** group in `SettingsController`'s profile field table, using
  the existing `boolField` / `enumField` / int / string descriptors. Enum options read the loader's
  token tables, so the page cannot offer a value the loader rejects.
- Global keys: `settings.yml` overrides are flat top-level keys today. Global descriptors gain
  **dotted keys** (`gutter.line_numbers`); `GuiConfigStore` writes them as nested YAML merged with
  sibling keys, and `mergeGuiManagedSideFiles` applies them **one leaf at a time**, so overriding
  `gutter.line_numbers` in the GUI does not reset a `gutter.timestamps` set in `contour.yml`, and
  `contour.yml` still wins wherever the GUI set nothing.
- Colour slots: new rows in the scheme editor's slot table.
- New default bindings appear in the page's read-only bindings viewer automatically.
- Saving goes through the existing live reload; a change of gutter width resizes and reflows the
  page, as toggling fold markers does today.

---

## 12. Daemon mode

- `WireLine` (`vthost/proto/Pdu.hpp`) gains `blockId` and `bornAt`. The protocol is unreleased and
  its codec version pinned, so structs change in place.
- `Delta` gains the block records changed since the connection last saw them, using the
  per-connection, revision-gated pattern the OSC 3008 contexts use (`FollowState`,
  `collectContextState`).
- `ScreenMirror` adopts them into the client terminal's store: `AdoptMode::Snapshot` for the first
  replay after attaching (no events), `AdoptMode::Live` afterwards (a record the mirror never held
  that is older than the newest it holds is history, adopted as `Snapshot`) — including a resync snapshot
  after a resize, so a command that finished during the resize still notifies. Host ids are used
  as-is: the client store only holds mirrored records.
- Head positions are host stable ids and never cross the wire; the mirror records its own from the
  `Marked` rows it writes. The host's session epoch rides `SessionState`, so per-line birth times
  name the same instant on both ends.
- Consequently an attached client has the gutter, scrollbar marks, sticky header, picker entries and
  finish notifications — and reattaching never replays notifications for blocks that finished while
  detached.
- tmux control mode is unaffected (its panes have their own grids and no OSC 133 passthrough).

---

## 13. Cross-cutting concerns

### 13.1 Performance

| Path | Cost added |
|---|---|
| Memory per line | 0 bytes (block id and timestamp live in existing padding) |
| Per cursor-line change | two stores (block id; timestamp when zero, which also dirties the line once) |
| Per PTY batch | one wall-clock read |
| Per OSC 133 sequence | a record update (a few per command) |
| Per frame | gutter: O(visible rows) with O(1) lookups; sticky header: O(1) lookups + one row copy when shown |
| Per store/fold revision | scrollbar marks: O(records), at most once per frame |

No regression is accepted in parser throughput: the plan measures `cat` of a large file under
Callgrind before and after (AGENT.md workflow).

### 13.2 Security

- Command lines are sanitised for display and stripped of ESC for insertion (§10.4).
- Temporary output files are owner-only, uniquely named, deleted with their pane and at exit.
- The picker inserts only at a shell prompt and never runs without the explicit Shift+Enter.
- Working directories keep OSC 3008's locality rules: a foreign path is never opened locally.

### 13.3 Accessibility

The gutter, annotations and sticky header sit outside the cell grid the accessibility bridge walks,
so screen readers see exactly what they see today. Finish notifications are ordinary desktop
notifications and reach assistive technology through the platform.

### 13.4 Error handling

Protocol input is lenient and total: malformed or out-of-order OSC 133 is an outcome (§4.3), never an
error. Fallible operations return `std::expected`: `ExternalLauncher::runWithStdin`, temporary-file
creation (`OpenCommandOutput` reports failure through the session's existing error/notification
path), timestamp-format validation at config load.

---

## 14. Testing strategy

Every pure unit has its own Catch2 tests; integration runs headless (`MockTerm`, `TestApp`,
`Qt6::Test`). Highlights:

| Area | Tests |
|---|---|
| Store | `ManualClock`: every A/B/C/D order, missing `D`, empty prompts discarded, nested prompts closing implicitly without a finish event, `C` without `A`, the cap, OSC 3008 enrichment, `adopt` live vs snapshot |
| Rows | block id + timestamp survive reflow on every chunk; reset clears them; `sizeof(Line)` unchanged |
| User marks | `mm` sets `UserMark`; eviction and folding ignore it; `[m`/`]m` still visit it |
| Gutter | layout table; glyph precedence; colours by outcome; absolute numbers across eviction/ED 3/reflow; relative/hybrid against folds and vi cursor; timestamp formatting with `ManualWallClock`; annotation not in `cells` |
| Scrollbar | bucketing, failure priority, fold-aware positions, rescan after generation bump |
| Sticky header | decision table (modes × head visible × folded × alt screen); render buffer content; click consumed; placeholder after eviction |
| Actions | `SelectCommandBlock` targets; click-count binding parse + match order; `CopySelection` fallback; `OpenCommandOutput` with `InMemoryFileSystem` + fake launcher; `ClearToPrompt`; path resolution local/foreign/missing |
| Notifications | `finishNotificationFor` table (when × visibility × duration × outcome); formatting; replace/clear with a fake notifier; no replay on attach |
| Picker | ranking/dedup; fuzzy filter through the row source; QML model; insert vs run via mock PTY incl. bracketed paste; refusal off-prompt |
| Shell scripts | embedded-script table (incl. pwsh); per shell available on CI, run in a real PTY via the `vtpty` harness and assert A/B/C/D + encoded command line (spaces, quotes, Unicode); sanitiser incl. `\e[201~` |
| Settings | every new row round-trips; nested override keeps siblings; enum options equal tokens; bad timestamp format reported; docs/generated config list every key |
| Daemon | wire round-trip of the new fields; mirror adopts records; client raises finish events live only |

Coverage is reported with the `clang-coverage` preset; data-race checks for the event path with
`clang-tsan` (`[threading]`).

---

## 15. Delivery

One branch, one pull request, closing #1010. Implemented in this order, each phase building and
passing its tests before the next starts, each followed by `/simplify` and an xhigh code review:

1. Foundation — row stamping, record store, events, `UserMark`, ShellIntegration removal,
   SemanticBlockTracker rebase (§4).
2. Daemon replication (§12).
3. Shell integration scripts and the sanitiser (§10).
4. Gutter (§5).
5. Block actions, click-count bindings, `runWithStdin`, path resolution (§7).
6. Finish notifications (§8).
7. Sticky header and evicted-output placeholder (§6.2, §6.3).
8. Scrollbar marks (§6.1).
9. Recent-commands picker (§9).
10. Settings page, documentation (website: shell integration, configuration reference, features),
    release notes in `metainfo.xml` (§11).

---

## 16. Verification items and follow-ups

### 16.1 To verify during implementation (before relying on them)

- Does the ConPTY Contour uses pass OSC 133 through? Likely (Contour ships its own `conpty.dll`,
  which passes VT through since Windows Terminal 1.22), but measured by an automated probe first; if
  it fails, the PowerShell script is held back and the limitation raised, rather than shipped
  silently broken.
- What fish and Nushell emit natively was answered during planning from their sources and release
  notes (§10.3); the fish ≥ 4 path is still checked by hand, because CI ships fish 3.7.
- `Ctrl+Shift+G` and `Ctrl+Alt+R` are free in the default bindings on every platform.
- Every place rows leave the grid advances `_evictedRowCount` (line-wise eviction, block trims, ED 3,
  `ClearToPrompt`, reflow overflow).
- `YAMLConfigReader` can apply a single nested leaf without resetting its siblings.

### 16.2 Follow-ups (not in this PR)

- Click-to-move in the prompt (`click_events`, `cl=`) and prompt redraw on resize (`redraw=`).
- `133;A;aid=` to keep nested shells' blocks apart; OSC 633 support.
- OSC 777 and OSC 9 notifications are never focus-gated (only OSC 99 is) — a separate defect.
- Status-line placeholders for the last exit code and duration.
