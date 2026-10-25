# Native Redesign — Step 3d (Command Palette: Grouping and Drill-In) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead of Steps 4a, 1c, 1d and 2 (pipelining ruling). Line numbers are at HEAD `fdb54a64`, before any of
them lands. Where Step 3d touches code they add or change, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference once Step 2 has landed.

**Goal:** The command palette groups its rows (Pane, Tab, Window, Layouts, Blocks, Profiles, Hosts and the rest),
collapses every parameterised action into one `›` row that drills into its per-name children, keeps fuzzy matching
with highlighted characters and recently-used-first, and is redrawn at 640 × max 420, 18 % from the top, with 32-tall
single-line rows, the selected row's description in a footer, a 160 ms scale-and-fade open (fade only under Reduce
Motion), and the same structure in cells on the Terminal row.

**Architecture:**
- **Grouping is a column, the groups a table.** `command/` gains `CommandCategory`, one category row per catalogue
  action (`ActionCategories`, keyed by catalogue name), and `PaletteGroups`, the display order and titles. Adding an
  action is one row; a coverage test fails when a catalogue action has no category.
- **The palette's behaviour is pure.** `command/CommandPaletteTree` builds the tree (plain rows plus one parent per
  parameterised action, `actions::isParameterized`) and lists it for a query: grouped and recent-first when unfiltered,
  grouped and fuzzy-ranked when filtered (children match on their argument and surface as "Parent · child"), flat
  inside a drilled-in level. The ranking that lives in `CommandPaletteModel.cpp:113-204` today moves here, unchanged in
  its rules, so it is tested in `contour_test` without Qt.
- **The Qt model is an adapter.** `window/CommandPaletteModel` holds the tree, the level and the filter, publishes the
  listing as roles, and gains `enter(row)` / `back()`; its sources and hint style move into the constructor.
- **The look is tokens.** New size, type, motion, layout and colour tokens on all four rows; `window/PalettePlacement.hpp`
  decides where the popup sits; `CommandPalette.qml` is rewritten from tokens only (its 19 size literals go to 0).

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), QML, Catch2 v3, Python 3 (lint), CMake build tree
`out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.10 (`:621-641`) binds, with
what it cites: §1 (principles, `:28-64`), §2.1 (token rows, `:68-126`), §2.3 (Reduce Motion, `:139-167`), §2.5 and §2.6
(snapshots and testing, `:187-226`), Phase 0 Task 2 (`ShortcutStyle`, `command/Shortcut.hpp:20-45`). §3.10's citation
`CommandPaletteModel.cpp:125-137` (the two sections) is current. The design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md`. The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `mac-palette`, `kit-tokens`, `kit-components`
(ListRow), `tui` (TUI palette), `daemon` (Attach to Host ›) and `blocks` (a "Blocks" group) are in
`.superpowers/design/canvas/project/`. The plans index is `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.
Steps 4a, 1c, 1d and 2 (`2026-10-25-native-redesign-step-4a.md`, `-1c.md`, `-1d.md`, `-2.md`) are assumed done.

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

- **The spec, verbatim (§3.10):**
  - "A `CommandCategory` column is added to the catalogue (`command/CommandCatalog.hpp`). Rows are grouped by Pane,
    Tab, Window, Layouts, Blocks, Profiles and Hosts."
  - "Parameterised actions collapse into one row per action, marked `›`. Its children are the existing source rows."
  - "640 wide, with rows 32 high. The description shows in a footer, for the selected row only."
  - "Shortcut text comes from `ShortcutStyle` (Phase 0 Task 2). Fuzzy matching is unchanged."
  - Tests: "The grouping table." "The drill-in model." "Filtering across drill-in children."
- **Canvas values, exact** (`mac-palette`, `kit-tokens`, `kit-components`, `tui`). Native extents in logical px,
  Terminal in cells (the `size.*` and `space.*` groups scale by the units, as every size does):

  | Token | MacOS | Fluent | ContourLinux | Terminal |
  |---|---|---|---|---|
  | `size.paletteWidth` | 640 | 640 | 640 | 48 |
  | `size.paletteMaxHeight` | 420 | 420 | 420 | 20 |
  | `size.paletteField` (the query row) | 44 | 44 | 44 | 1 |
  | `size.paletteRow` (kit `size.paletteRow`) | 32 | 36 | 38 | 1 |
  | `size.paletteGroupHeader` | 24 | 24 | 24 | 1 |
  | `size.paletteFooter` | 28 | 28 | 28 | 1 |
  | `space.paletteInset` (rows' horizontal inset and the list's side padding) | 8 | 8 | 8 | 1 |
  | `layout.paletteTopPercent` | 18 | 18 | 18 | 18 |
  | `type.paletteInput` (kit) | 17 | 16 | 16 | 0 |
  | `type.shortcut` | 12 | 12 | 12.5 | 0 |
  | `motion.popoverScalePercent` (open scale start) | 97 | 97 | 97 | 100 |

  - Reused tokens: the surface radius is `radii.popover` (14 / 8 / 12 / 0, kit `radius.panel`); the selection radius
    is `radii.control` (6 / 4 / 8 / 0); the shadow is `elevation.e3`; row titles `type.body`; group headers
    `type.footnote` at `Font.DemiBold`, sentence case; footer `type.footnote`; the popup's side margin to the window is
    `space.s8` (16; 1 cell on the Terminal row).
  - Open: opacity 0 → 1 and scale `popoverScalePercent / 100` → 1 over `motion.standard` (160 ms). Close: opacity
    1 → 0 over `motion.fast` (100 ms). Reduce Motion: `resolveChromeTokens` sets `popoverScalePercent = 100` (and 1c's
    `style = CrossFade`, durations 100 ms), so the open is a fade only.
  - Glyphs (`UiStyleTokens`): `drillInGlyph` `"›"` native / `"▸"` Terminal; `palettePromptGlyph` `""` native / `"❯"`
    Terminal (drawn before the query field).
  - Copy: placeholder "Type a command or search…"; the field's accessible name "Command"; the empty list "No matching
    commands"; the recent group "Recently used"; a surfaced child "Launch Layout · split-dev" (separator `" · "`).
  - Colours (new `ChromeColorRole`s, R-h): surface, text, secondary text, selection fill, selection text, separator.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e; the 4a, 1c, 1d, 2, 3a and 3b SDD ledgers):
  - Shortcut text comes from `command/Shortcut.hpp` (`shortcutIndex`, `shortcutText`, `ShortcutKeyGlyphTable`), ⌃⌥⇧⌘
    glyph order on macOS (Phase 0), "Ctrl+Shift+P" elsewhere.
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`); a new file's baseline is 0 and a
    baseline only goes down (`CommandPalette.qml` is at 19, `scripts/qml-size-literal-baseline.txt:6`, and goes to 0).
  - Every visual surface is a snapshot row, and every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - Text contrast at least 4.5:1, checked against contour-dark and contour-light (Terminal row) and against the kit's
    light and dark surfaces (native rows). Text colours come from tokens or from `SystemPalette` as `KitDialog.qml`
    reads it, never from a popup's inherited `palette`.
  - Reduce Motion: every transition becomes the 100 ms cross-fade (1c R-j, `motion.crossFade`).
  - `enum class` over `bool`, zero the default; QML-facing `Q_PROPERTY`/`Q_INVOKABLE` booleans are the documented
    carve-out (this step needs none: `depth` and `kind` are ints, a group header is a non-empty `groupTitle`).
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven tables (`ActionCategories`, `PaletteGroups`, `PaletteHintRows`, the token
  rows, the snapshot rows); configuration at construction (`CommandPaletteModel` takes its history, sources and hint
  style in its constructor; `setSources`/`setShortcuts` go — the shortcuts are domain state passed to `refresh`);
  Doxygen on every new public declaration; `.hpp` with `#pragma once`; zero warnings and no `NOLINT`; `slots`,
  `signals` and `emit` are Qt macros, never identifiers.
- **Qt-free targets stay Qt-free:** `command/CommandPaletteTree.{hpp,cpp}` and `window/PalettePlacement.hpp` include no
  Qt header.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`):
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds in one tree at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit, run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding. Format every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`; check
  every new or touched `.qml`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`. **Never clang-format a
  `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value,
  and every table row names every field (`ChromeTokenRows`, `ActionCategories`, `PaletteGroups`, `PaletteHintRows`,
  `SnapshotScripts`).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type. Counts in
  tests are derived from tables (`actionCatalog().size()`, `PaletteGroups.size()`, `SnapshotScripts.size()`), never
  literals.
- **QML:** enums are unreachable through `chromeStyle`; QML compares `int`s. A `Popup` cannot carry `Accessible` or
  `Keys` attached properties: put them on an `Item`. Every new control sets `Accessible.role` and `Accessible.name`.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files by name (other agents share the tree; never `git add -A` or a
  directory). Each message ends with exactly `Signed-off-by: Christian Parpart <christian@parpart.family>`, and never
  carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 8).

## Review Focus

1. **A query that matches inside a drilled-in level.** Typing `dark` at the top must surface "Change Profile · dark"
   in the Profiles group (with `dark` highlighted, not "Change Profile"), and must not flood the list with every
   profile because the parent's words match; typing inside the Change Profile level filters only its children, and
   leaving the level restores the top-level query. Pinned in Task 2 (`paletteTree.aFilterSurfacesMatchingChildren`,
   `paletteTree.childrenMatchOnTheirArgumentOnly`, `paletteTree.aFilterInsideALevelRanksOnlyItsChildren`) and Task 3
   ("back() restores the filter the level was entered from").
2. **Escape in a drill-in level.** Expected: it goes back one level, with the parent row selected and the old query
   back in the field; a second Escape closes; Backspace in an empty field also goes back. Pinned in Task 3
   (`anchorRow`) and Task 5 ("Escape in a level goes back one level, then closes").
3. **200 profiles.** Expected: the top level shows one "Change Profile ›" row whatever the count; the level lists all
   200, scrolls, and the arrow keys keep the selection in view; a query finds `p199` first. Pinned in Task 2
   (`paletteTree.twoHundredProfilesAreOneRow`) and Task 5 ("a 200-row level keeps the selection in view").
4. **A palette opened while a command's modal is up** (About, Save Layout…, a confirm dialog — the macOS menu bar's
   palette row reaches it while the dialog has focus). Expected: the palette opens above it, and Escape or a pick
   gives the keyboard back to the dialog, not to the terminal behind it; a command that itself opens a modal runs only
   after the palette has closed, and its modal keeps the focus. Pinned in Task 6 ("closing the palette over a dialog
   gives the dialog its focus back", "a picked command that opens a dialog keeps that dialog focused").
5. **RTL or very long command names** (a Hebrew profile name, a 300-character tab title set by a shell). Expected:
   the title elides at the row's end, the shortcut and `›` stay inside the row, an RTL title starts at the row's
   leading edge like every other, and the bold match lands on the typed characters. Pinned in Task 2
   (`paletteTree.matchOffsetsLandOnTheChildArgument`, a Hebrew argument), Task 3 (UTF-16 offsets through the prefix)
   and Task 5 ("a long or right-to-left title elides inside its row").

Also pinned, beyond the five: reopening the palette always starts at the top level (Task 3); a recent child shows
qualified in "Recently used" (Task 2); a stale recent id disappears (Task 2); one palette per window (Task 5).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Where the column lives.** "A column of the catalogue" in `command/CommandCatalog.hpp`, as §3.10 says:
  `ActionCategories`, one `{ name, category }` row per `actions::actionCatalog()` entry, keyed by the catalogue name.
  `config/Actions.hpp` stays untouched: `config/` sits below `command/`, and the category is the palette's vocabulary,
  not the key bindings'. The coverage test (every catalogue name exactly once, no unknown name) is what makes a new
  action without a category a test failure.
- **R-b. More groups than §3.10's seven (owner, 1).** 117 catalogue actions do not fit seven groups (copy, scroll,
  font size, reset, quit). Four more groups follow the macOS menu bar's names (1b's `MenuBarTable`): Edit, View, Shell,
  Contour. Display order: Pane, Tab, Window, Layouts, Blocks, Profiles, Hosts, Edit, View, Shell, Contour. The
  assignment (Task 1's table):
  - Pane: SplitVertical, SplitHorizontal, ClosePane, FocusPane{Left,Right,Up,Down}, SwapPane{Left,Right,Up,Down},
    MovePane{Left,Right,Up,Down}, ToggleSplitOrientation, TogglePaneZoom, ResizePane.
  - Tab: CreateNewTab, CloseTab, CloseOtherTabs, CloseTabsToRight, MoveTabTo, MoveTabToLeft, MoveTabToRight,
    MoveTabToNewWindow, SwitchToTab, SwitchToPreviousTab, SwitchToTabLeft, SwitchToTabRight, SetTabTitle, SetTabColor,
    ResetTabColor, SetTabBarVisibility, SetTabBarPosition.
  - Window: NewTerminal, CloseAllTabs, ToggleFullscreen, ToggleTitleBar, MinimizeWindow, ZoomWindow.
  - Layouts: LaunchLayout, SaveLayout. Profiles: ChangeProfile. Hosts: AttachToHost.
  - Blocks: CollapseAllFolds, CollapseLastFold, ExpandAllFolds, ToggleFold, ToggleFoldAt, ToggleLastFold,
    SelectCommandBlock, CopyCommandBlock, CopyCommandLine, RerunCommand, ClearToPrompt, OpenCommandOutput,
    CopyLastCommandPrompt, CopyLastCommandOutput, CopyLastCommandBlock, CopyPreviousMarkRange, ScrollMarkUp,
    ScrollMarkDown.
  - Edit: CancelSelection, CopySelection, CreateSelection, SelectAll, PasteClipboard, PasteSelection, OpenSelection,
    CopyHyperlink, FollowHyperlink, HintMode, SearchReverse, FocusNextSearchMatch, FocusPreviousSearchMatch,
    NoSearchHighlight, SpeakSelection, StopSpeaking (and 3b's SearchSelection, if it has landed).
  - View: IncreaseFontSize, DecreaseFontSize, ResetFontSize, IncreaseOpacity, DecreaseOpacity, ScrollUp, ScrollDown,
    ScrollOneUp, ScrollOneDown, ScrollPageUp, ScrollPageDown, ScrollToTop, ScrollToBottom, ToggleStatusLine.
  - Shell: SoftReset, ClearHistoryAndReset, SendChars, WriteScreen, ViNormalMode, ToggleAllKeyMaps,
    ToggleInputMethodHandling, ToggleInputProtection, SaveScreenshot, CopyScreenshot, ScreenshotVT, OpenFileManager,
    OpenContextMenu, CreateDebugDump, TraceEnter, TraceLeave, TraceStep, TraceBreakAtEmptyQueue.
  - Contour: OpenCommandPalette, OpenConfiguration, ReloadConfig, ResetConfig, ShowAbout, Quit.
- **R-c. What drills in (owner, 2).** Every command whose action `actions::isParameterized` (`Actions.hpp:423`) is a
  child of one parent row per action name: ChangeProfile, LaunchLayout, AttachToHost and SwitchToTab from the live
  sources, and CreateSelection, HintMode, SendChars, WriteScreen, MoveTabTo, ResizePane, ToggleFoldAt when a binding
  carries them. A parent exists only with at least one child, and drills in even with one (one rule). The parent's
  title is `splitCamelCase(name)`, its description the catalogue's, its group the action's category. Levels do not
  nest.
- **R-d. A child's label.** `Command` gains `argument`: the title suffix `commandArguments` renders, without its
  leading `": "` or `" "` ("dark", "3", `"patterns (action, scope)"`). `TabCommandSource` sets it to `"3: vim"` (the
  position, and the tab's title when it has one), matching its overridden title. `HostCommandSource` sets the child's
  `description` to `config::toString(entry.url)`, so the footer reads "ssh://me@starfive" (the daemon board's
  subtitle; its session count and last-seen have no source).
- **R-e. Listing rules** (the canvas keeps the groups while filtering):
  - **Top, no filter:** first "Recently used": each id of `CommandHistory::recent()` that resolves (plain or child), a
    child shown qualified (`parent.title + " · " + argument`); then each `PaletteGroups` row in order, its plain
    commands and parents sorted by title; an empty group is omitted. A recent command also appears in its group.
  - **Top, filtered:** candidates are plain commands (title first, id as fallback, as today), parents (title, then
    name), and children (their `argument` only, never the parent's words, shown qualified, match offsets shifted past
    the prefix). Rows group by category; groups order by their best score, ties by table order; rows within a group by
    score, then recency (a parent takes its most recent child's), then title. No "Recently used" group. Row 0 is
    therefore always the best match.
  - **Level, no filter:** the parent's children titled by `argument`, the recently used first (newest first), then the
    rest by `argument`; no group headers.
  - **Level, filtered:** the children whose `argument` matches, ranked by score, recency, `argument`; no headers.
- **R-f. Navigation.** Enter or a click on a `›` row enters it (the filter empties; the top filter is kept); Enter on
  a command row runs it after the popup has closed, as today (`CommandPalette.qml:57-97`). Escape in a level goes back,
  restoring the kept filter and selecting the parent's row; Escape at the top closes. Backspace in an empty field in a
  level goes back. ↑/↓ and ⌃J/⌃K move the selection, as today. Every open (`refresh`) starts at the top level.
- **R-g. The footer.** Left: the selected row's description, elided, in secondary text; right: key hints built from
  `PaletteHintRows` and `shortcutText` (`↩ Run · ⎋ Close` on macOS, `Enter Run · Escape Close` elsewhere): a command
  row at the top Run/Close, a `›` row Open/Close, a row in a level Run/Back. The canvas's "⌥↩ Run and keep open" is
  not built **(owner, 5)**.
- **R-h. Colours (owner, 3).** Six roles, appended after the last `ChromeColorRole` at execution:
  `PaletteSurface`, `PaletteText`, `PaletteSecondaryText`, `PaletteSelectionFill`, `PaletteSelectionText`,
  `PaletteSeparator`. Native rows: `surface(1)` (kit `surface.elevated`, "palette"), `label(1)`, `label(2)`, two new
  `ColorSourceKind`s `PaletteHighlight` and `PaletteHighlightedText` (the OS's own selected-row pair: macOS's
  `selectedContentBackgroundColor`/white reads 5.3:1, where white on the accent `#007AFF` reads 4.0:1), and Step 2's
  `separator()`. Terminal row: `Bar`, `BarText`, `schemeOnBar(Secondary)`, `Selection`, `SelectionText`, `Secondary`.
  Group headers, shortcuts, descriptions and hints use `PaletteSecondaryText`, not the canvas's tertiary (black 45 %
  on white reads 3.4:1; the kit itself says "text never goes below secondary"). Matched characters are
  `Font.DemiBold` in the row's own text colour on every row, selected or not: the canvas's accent tint (`#4DA3FF` on
  dark) reads 4.0:1 on the light surface with the default accent.
- **R-i. Surface and material.** The popup sits on `PopupSurface.qml` with `color: chromeStyle.colors.paletteSurface`,
  `radius: chromeStyle.radii.popover` and a new `elevation` property set to `chromeStyle.elevation.e3`. It is opaque:
  no surface renders `material.popover` yet (3b R-n), so "popover glass" is not drawn by this step.
- **R-j. Placement.** `placePalette` (Task 4): width `min(paletteWidth, window − 2 · s8)`, centred; top
  `windowHeight · 18 %`, floored to whole rows on the Terminal row; maximum height `min(paletteMaxHeight,
  windowHeight − top − s8)`, never negative. The popup's height fits its content up to that maximum.
- **R-k. Focus hand-back (Review Focus 4).** At `aboutToShow` the palette remembers `window.activeFocusItem`. On close,
  if that item lies inside the window's `Overlay.overlay` (a dialog or popup was focused), it gets the focus back;
  otherwise `window.restoreTerminalFocus()` runs, as today. The deferred dispatch (`Qt.callLater`) is unchanged and
  runs after the hand-back.
- **R-l. RTL and long titles.** Titles set `horizontalAlignment: Text.AlignLeft` (Qt otherwise right-aligns a
  right-to-left string), `elide: Text.ElideRight`, and take the row's width minus the accessory's; the accessory
  (shortcut or `›`) never shrinks.
- **R-m. Snapshots.** The `palette` row now shows the grouped top level. New rows `palette-filtered` (query `split`,
  the canvas's state: Pane, then Layouts with "Launch Layout · split-dev") and `palette-drill-in` (inside Change
  Profile). The scene's palette gains a profile and a layout source over a scene config with profiles `work` and
  `presentation` besides the default, and layouts `split-dev` and `logs`.

### Non-goals

- "⌥↩ Run and keep open" (owner decision 5); the daemon board's "Connect to Address…" child (no such action) and its
  session counts; the canvas's titles ("Split Right ⌘D" for today's "Split Vertical"; the ⌘ chords are issue #2175).
- Popover glass (R-i). Nested drill-in levels.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/command/Command.hpp` (modify) | `CommandCategory`; `Command::category`, `Command::argument` | 1 |
| `src/contour/command/CommandCatalog.{hpp,cpp}` (modify) | `ActionCategories`, `commandCategory`; the sources fill category, argument, host description | 1 |
| `src/contour/command/CommandPaletteTree.{hpp,cpp}` (create) | The groups table, the tree, the listing, the footer hints | 2 |
| `src/contour/window/CommandPaletteModel.{hpp,cpp}` (rewrite) | The Qt adapter: roles, level, `enter`/`back` | 3 |
| `src/contour/window/WindowController.{hpp,cpp}` (modify) | Constructs the model with its sources; `refresh(shortcuts)` | 3 |
| `src/contour/qml/CommandPalette.qml` (bridge) | The delegate reads `groupTitle` and `kind` until the rewrite | 3 |
| `src/contour/window/PalettePlacement.hpp` (create) | Where the popup goes | 4 |
| `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `window/ChromeTokenObjects.{hpp,cpp}`, `window/UiStyleProvider.{hpp,cpp}` (modify) | Tokens, colour roles, glyphs, `palettePlacement` | 4 |
| `src/contour/qml/CommandPalette.qml` (rewrite), `PopupSurface.qml` (modify) | The palette; `elevation` on the surface | 5, 6 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | Snapshot rows and their scene | 7 |

### Shared files

Files another Phase 3 step's spec section also names, or that every Phase 3 step edits. A parallel worktree that
touches one rebases before its task commits, and the merge order is the board's.

- `src/contour/command/CommandCatalog.{hpp,cpp}`: §3.8's plan (3b) adds `SearchSelection` to `config/Actions.hpp` and
  names this file. Whichever lands second gives `SearchSelection` its row in `ActionCategories`: Edit, beside
  `SearchReverse`. Task 1's coverage test fails until it does.
- `src/contour/qml/PopupSurface.qml`: 3b changes the shadow corner to follow `root.radius` (`:75`, `:88`). 3d makes
  the same change plus `elevation`; whichever lands second drops the duplicate hunk on rebase.
- `src/contour/config/UiStyle.hpp` glyphs: 3b adds four beside `tabSeparator`; 3d adds two after whatever is last.
- `src/contour/window/UiStyleProvider.{hpp,cpp}`: 3b adds `findBarPlacement`; 3d adds `palettePlacement` beside it.
- Edited by every Phase 3 step: `src/contour/config/ChromeTokenGroups.hpp` (append each group's fields;
  `ChromeColorRole` and `ColorSourceKind` append after whatever is last at execution),
  `src/contour/window/ChromeTokenObjects.{hpp,cpp}`, `src/contour/test/ChromeSnapshotPlan.hpp` (append scripts and
  actions), `src/contour/test/ChromeSnapshotScene.hpp`, `src/contour/test/QmlComponents_test.cpp`,
  `src/contour/CMakeLists.txt`, `scripts/qml-size-literal-baseline.txt`, `metainfo.xml`.
- Not touched, on purpose: `Main.qml` (3c's notices; the focus hand-back lives in `CommandPalette.qml`),
  `SaveLayoutDialog.qml` and `KitDialog.qml` (3c), `config/Actions.hpp` (3b).

---

### Task 1: The category column

**Files:**
- Modify: `src/contour/command/Command.hpp:13-25` (the enum before `Command`; two members).
- Modify: `src/contour/command/CommandCatalog.hpp:11-22` (the column's declarations after `collectCommands`).
- Modify: `src/contour/command/CommandCatalog.cpp:22-34` (`makeCommand`), `:122-130` (`HostCommandSource`),
  `:132-154` (`TabCommandSource`); the table's rows after the anonymous namespace.
- Test: `src/contour/command/CommandCatalog_test.cpp`.

**Interfaces:**
- Produces (namespace `contour::command`, `Command.hpp`):
  - `enum class CommandCategory : std::uint8_t { Pane = 0, Tab, Window, Layouts, Blocks, Profiles, Hosts, Edit, View, Shell, Contour };`
  - `inline constexpr std::size_t CommandCategoryCount = static_cast<std::size_t>(CommandCategory::Contour) + 1;`
  - `Command` gains, after `action`, `CommandCategory category = CommandCategory::Pane;` (always set by `makeCommand`)
    and `std::string argument {};` (R-d).
- Produces (`CommandCatalog.hpp`):
  - `struct ActionCategory { std::string_view name {}; CommandCategory category = CommandCategory::Pane; };`
  - `[[nodiscard]] std::span<ActionCategory const> actionCategories() noexcept;` — the rows of R-b, one per catalogue
    action, in catalogue order (a `static constexpr std::array` in the `.cpp`).
  - `[[nodiscard]] CommandCategory commandCategory(actions::Action const& action) noexcept;` — the row whose `name` is
    `actions::catalogEntry(action).name`; `@pre` the row exists (the coverage test guarantees it).
- `makeCommand` sets `.category = commandCategory(action)` and `.argument` (the title suffix without one leading `": "`
  or `" "`). `TabCommandSource` sets `argument` to `std::format("{}: {}", position, title)`, or `"{position}"` for an
  untitled tab. `HostCommandSource` sets `description = config::toString(entry.url)` (`config/HostUrl.hpp:77`).

- [ ] **Step 1: Write the failing tests** (`CommandCatalog_test.cpp`, `[contour][palette][commandcategory]`):
  - `commandCategory.everyCatalogueActionHasExactlyOneRow`: the set of `actionCategories()` names equals the set of
    `actions::actionCatalog()` names, and `actionCategories().size() == actions::actionCatalog().size()` (no duplicate).
  - `commandCategory.theSpecsSevenGroups`: `SplitVertical` → Pane, `CreateNewTab` → Tab, `ToggleFullscreen` → Window,
    `LaunchLayout` → Layouts, `CopyCommandBlock` → Blocks, `ChangeProfile` → Profiles, `AttachToHost` → Hosts;
    `CopySelection` → Edit, `IncreaseFontSize` → View, `SoftReset` → Shell, `Quit` → Contour.
  - `commandCategory.everyCategoryIsUsed`: for every `CommandCategory` in `iota(0, CommandCategoryCount)`, some row has it.
  - `commandCategory.commandsCarryTheirCategoryAndArgument`: `ProfileCommandSource` over a config with profile `dark`
    gives `.category == Profiles`, `.argument == "dark"`, `.title == "Change Profile: dark"` (unchanged);
    `ActionCommandSource`'s `SplitVertical` has an empty `argument`.
  - `commandCategory.aTabChildReadsItsPositionAndTitle`: a `TabCommandSource` over titles `{"vim", ""}` gives arguments
    `"1: vim"` and `"2"`.
  - `commandCategory.aHostChildDescribesItsUrl`: `HostCommandSource` over host `starfive` at `ssh://me@starfive` gives
    `.description == "ssh://me@starfive"` and `.argument == "starfive"`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, no `actionCategories`.
- [ ] **Step 3: Implement** the enum, the two members, the table (R-b's rows, `name` spelled exactly as the catalogue
  spells it) and the source changes.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[contour][palette]"`

  Expected: PASS, the existing catalogue cases unchanged.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/command/CommandCatalog.cpp src/contour/command/CommandCatalog_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/command/Command.hpp src/contour/command/CommandCatalog.hpp src/contour/command/CommandCatalog.cpp src/contour/command/CommandCatalog_test.cpp
git add src/contour/command/Command.hpp src/contour/command/CommandCatalog.hpp src/contour/command/CommandCatalog.cpp src/contour/command/CommandCatalog_test.cpp
git commit -m "command: give every catalogue action a palette category, from one table" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The palette tree and its listing, pure

**Files:**
- Create: `src/contour/command/CommandPaletteTree.hpp`, `CommandPaletteTree.cpp`; list them in
  `src/contour/command/CMakeLists.txt:8-21`.
- Test: create `src/contour/command/CommandPaletteTree_test.cpp`; add it to `contour_test`
  (`src/contour/CMakeLists.txt:1153-1202`, beside `command/CommandCatalog_test.cpp`).

**Interfaces:**
- Consumes: Task 1 (`CommandCategory`, `Command::category`, `Command::argument`); `fuzzyMatch`, `fuzzyScore`
  (`FuzzyFilter.hpp:47`, `:73`); `shortcutText`, `ShortcutChord`, `ShortcutStyle` (`Shortcut.hpp`).
- Produces (namespace `contour::command`, Qt-free):
  - `struct PaletteGroupRow { CommandCategory category = CommandCategory::Pane; std::string_view title {}; };`
  - `inline constexpr auto PaletteGroups = std::array { … };` — R-b's display order, titles "Pane", "Tab", "Window",
    "Layouts", "Blocks", "Profiles", "Hosts", "Edit", "View", "Shell", "Contour".
    `static_assert(PaletteGroups.size() == CommandCategoryCount)`.
  - `inline constexpr std::string_view RecentGroupTitle = "Recently used";`
  - `inline constexpr std::string_view DrillInSeparator = " · ";`
  - `enum class PaletteEntryKind : std::uint8_t { Command = 0, DrillIn };`
  - `enum class PaletteDepth : std::uint8_t { Top = 0, Level };`
  - `struct PaletteParent { std::string name {}; std::string title {}; std::string description {}; CommandCategory category = CommandCategory::Pane; std::vector<std::size_t> children {}; };`
    (`children` index `PaletteTree::commands`).
  - `struct PaletteTree { std::vector<Command> commands {}; std::vector<std::size_t> plain {}; std::vector<PaletteParent> parents {}; [[nodiscard]] Command const* find(std::string_view id) const noexcept; };`
    — `find` searches every command, children included; null when none.
  - `[[nodiscard]] PaletteTree buildPaletteTree(std::vector<Command> commands);` — R-c; parents in catalogue order of
    their action, each parent's children in source order.
  - `struct PaletteEntry { PaletteEntryKind kind = PaletteEntryKind::Command; std::size_t index = 0; std::string title {}; std::vector<int> titleMatches {}; std::string_view groupTitle {}; [[nodiscard]] bool operator==(PaletteEntry const&) const = default; };`
    — `index` is into `commands` (Command) or `parents` (DrillIn); `titleMatches` are UTF-8 byte offsets into `title`,
    ascending; `groupTitle` is non-empty on the first row of a group only.
  - `struct PaletteQuery { std::string_view filter {}; std::optional<std::size_t> level {}; std::span<std::string const> recent {}; };`
    — `level` is the parent drilled into.
  - `[[nodiscard]] std::vector<PaletteEntry> listPalette(PaletteTree const& tree, PaletteQuery const& query);` — R-e.
  - `struct PaletteHintRow { PaletteEntryKind kind = PaletteEntryKind::Command; PaletteDepth depth = PaletteDepth::Top; std::string_view enterVerb {}; std::string_view escapeVerb {}; };`
  - `inline constexpr auto PaletteHintRows = std::array { {Command, Top, "Run", "Close"}, {DrillIn, Top, "Open", "Close"}, {Command, Level, "Run", "Back"} };`
  - `[[nodiscard]] std::string paletteFooterHints(PaletteEntryKind kind, PaletteDepth depth, ShortcutStyle style);` —
    `"<Enter> <enterVerb> · <Escape> <escapeVerb>"`, each key through `shortcutText(ShortcutChord { .modifiers = {}, .input = vtbackend::Key::Enter }, style)`;
    empty for a pair no row names.
- The ranking keeps today's rules (`CommandPaletteModel.cpp:143-201`): title through `fuzzyMatch`, id through
  `fuzzyScore` as a fallback without highlight, `stable_sort` by score descending, recency ascending (position in
  `recent`; absent sorts last), title. A parent's recency is its most recent child's.

- [ ] **Step 1: Write the failing tests** (`CommandPaletteTree_test.cpp`, `[palettetree]`). A helper builds commands
  through the real sources: `ActionCommandSource`, and `ProfileCommandSource`/`LayoutCommandSource`/`HostCommandSource`
  over a `config::Config` whose maps are filled with `insert_or_assign` (as `DisplayRendering_test.cpp:893` does).
  - `paletteTree.groupsFollowTheTable`: every `PaletteGroups` category appears once; the titles are R-b's, in order.
  - `paletteTree.parameterisedActionsCollapse`: with profiles `dark`, `light`: `parents` holds one `ChangeProfile`
    parent titled "Change Profile" with two children whose `argument`s are `dark`, `light`; no `plain` index is a
    `ChangeProfile` command; `find("ChangeProfile:dark")` is non-null.
  - `paletteTree.unfilteredTopIsRecentThenGroups`: `recent = {"ChangeProfile:dark", "SplitVertical"}` → row 0 is
    `{ Command, …, "Change Profile · dark", {}, "Recently used" }`, row 1 "Split Vertical" with an empty `groupTitle`;
    row 2 starts "Pane"; the groups' first rows appear in `PaletteGroups` order; within "Pane", titles ascend; the
    `ChangeProfile` parent is a `DrillIn` row in "Profiles", and no child row appears outside "Recently used".
  - `paletteTree.aStaleRecentIdDisappears`: `recent = {"ChangeProfile:gone"}` → no "Recently used" group.
  - `paletteTree.anEmptyGroupIsOmitted`: no hosts configured → no "Hosts" header and no `AttachToHost` parent.
  - `paletteTree.aFilterSurfacesMatchingChildren` (Review Focus 1): layouts `split-dev`, `logs`; filter `split` →
    the first group is "Pane" (Split Vertical/Horizontal), and a "Layouts" group holds
    `"Launch Layout · split-dev"` as a `Command` row whose `titleMatches` are `prefix + 0 … prefix + 4`, with
    `prefix = std::string_view("Launch Layout").size() + DrillInSeparator.size()` (17: `·` is two bytes), and no `logs`
    row.
  - `paletteTree.childrenMatchOnTheirArgumentOnly`: profiles `p0`…`p9`; filter `change` → one `DrillIn` row
    "Change Profile" and no child row.
  - `paletteTree.groupsOrderByTheirBestMatch`: filter `dark` with a profile `dark` → row 0 is "Change Profile · dark"
    under "Profiles" (its argument equals the query, the best score), though "Profiles" sits sixth in the table.
  - `paletteTree.aFilterInsideALevelRanksOnlyItsChildren` (Review Focus 1): `level` = the ChangeProfile parent,
    profiles `dark`, `dim`, `light`; filter `d` → rows titled `dark`, `dim` (no group titles), none from other actions;
    no filter → all three by argument, the recent one first when `recent = {"ChangeProfile:light"}`.
  - `paletteTree.recencyBreaksATie`: today's case (`CommandPaletteModel_test.cpp:303-318`) restated over `listPalette`.
  - `paletteTree.idFallbackHighlightsNothing`: filter `splitv` finds "Split Vertical" with empty `titleMatches`.
  - `paletteTree.twoHundredProfilesAreOneRow` (Review Focus 3): profiles `p000`…`p199` (`std::format("p{:03}", i)` over
    `iota(0, 200)`); unfiltered top → exactly one row whose `kind == DrillIn` and title "Change Profile", and no
    `ChangeProfile` child row; the level lists 200 rows; filter `p199` at the top → row 0 is
    "Change Profile · p199".
  - `paletteTree.matchOffsetsLandOnTheChildArgument` (Review Focus 5): profile `"שלום"`; filter `"של"` → the child row's
    `titleMatches` equal `{ prefix + 0, prefix + 1, prefix + 2, prefix + 3 }` (fuzzy matching is per byte, two bytes per
    Hebrew letter; the Qt model keeps the code-point boundaries, Task 3), `prefix` = "Change Profile" + separator.
  - `paletteTree.footerHints`: `paletteFooterHints(Command, Top, MacGlyphs) == "↩ Run · ⎋ Close"`;
    `(DrillIn, Top, MacGlyphs) == "↩ Open · ⎋ Close"`; `(Command, Level, MacGlyphs) == "↩ Run · ⎋ Back"`; under `Text`,
    the keys read as `shortcutText` spells Enter and Escape there; `(DrillIn, Level, …)` is empty.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `CommandPaletteTree.hpp` not found.
- [ ] **Step 3: Implement** `CommandPaletteTree.{hpp,cpp}`. `listPalette` collects scored candidates per R-e into one
  vector, groups them with a `std::array<…, CommandCategoryCount>` of buckets, and sets `groupTitle` on each group's
  first emitted row.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[palettetree],[contour][palette]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/command/CommandPaletteTree.cpp src/contour/command/CommandPaletteTree_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/command/CommandPaletteTree.hpp src/contour/command/CommandPaletteTree.cpp src/contour/command/CommandPaletteTree_test.cpp
editorconfig-checker src/contour/command/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/command/CommandPaletteTree.hpp src/contour/command/CommandPaletteTree.cpp src/contour/command/CommandPaletteTree_test.cpp src/contour/command/CMakeLists.txt src/contour/CMakeLists.txt
git commit -m "command: group the palette, collapse parameterised actions into drill-in rows, and rank across their children" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `CommandPaletteModel` adapts the tree

**Files:**
- Rewrite: `src/contour/window/CommandPaletteModel.hpp`, `CommandPaletteModel.cpp` (`byteOffsetsToUtf16`, `:36-69`,
  stays; the ranking `:113-204` goes to Task 2's listing).
- Modify: `src/contour/window/WindowController.cpp:64-73` (construction), `:89-101` (`openCommandPalette`);
  `WindowController.hpp:934-947` (comment only: the sources are still members, now handed to the constructor).
- Modify: `src/contour/test/ChromeSnapshotScene.hpp:304-315` (`makePaletteModel`),
  `src/contour/test/QmlComponents_test.cpp:3700-3748` (`MockPaletteController`).
- Modify: `src/contour/qml/CommandPalette.qml:209-246`, the bridge until Task 5: the delegate's `required property int
  section` and `required property bool sectionStart` become `required property string groupTitle` and `required
  property int kind`; the header shows when `row.groupTitle !== ""` and reads `row.groupTitle`. (A `required` property
  without its role is a QML error, which the run-wide gate turns into a suite failure.) The baseline count does not
  change.
- Test: `src/contour/window/CommandPaletteModel_test.cpp` (rewritten onto the new roles);
  `src/contour/test/QmlComponents_test.cpp:3842-3846`, `:3903` (the `sectioned()` reads go).

**Interfaces:**
- Consumes: Task 2 (`PaletteTree`, `buildPaletteTree`, `listPalette`, `PaletteQuery`, `PaletteEntry`,
  `paletteFooterHints`).
- Produces (`window::CommandPaletteModel`):
  - `CommandPaletteModel(command::CommandHistory const& history, std::vector<command::CommandSource const*> sources, command::ShortcutStyle hintStyle, QObject* parent = nullptr);`
    `setSources` and `setShortcuts` go.
  - `void refresh(std::unordered_map<std::string, std::string> shortcuts);` — re-collects the sources, rebuilds the
    tree, returns to the top level, clears the filter, one reset.
  - `[[nodiscard]] command::Command const* commandById(std::string_view id) const noexcept;` — `PaletteTree::find`.
  - `Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)` (as today).
  - `Q_PROPERTY(int depth READ depth NOTIFY levelChanged)` — `PaletteDepth` as an int.
  - `Q_PROPERTY(QString levelTitle READ levelTitle NOTIFY levelChanged)` — the parent's title in a level, else empty.
  - `Q_PROPERTY(int anchorRow READ anchorRow NOTIFY levelChanged)` — the row QML selects after a level change: 0 after
    `enter`, the parent's row after `back`.
  - `Q_INVOKABLE void enter(int row);` — on a `DrillIn` row: keeps the filter, enters, clears the filter; emits
    `levelChanged` then `filterChanged`. No-op on any other row or out of range.
  - `Q_INVOKABLE void back();` — in a level: restores the kept filter, returns to the top; emits both. No-op at the top.
  - `[[nodiscard]] Q_INVOKABLE QString footerHints(int row) const;` — `paletteFooterHints` for that row's kind and the
    current depth; empty out of range.
  - `Roles`: `IdRole` (a command's id; a parent's catalogue name), `TitleRole`, `DescriptionRole`, `ShortcutRole`
    (empty on a `DrillIn` row), `GroupTitleRole`, `TitleMatchesRole` (UTF-16, via `byteOffsetsToUtf16`), `KindRole`
    (int). Role names `commandId`, `title`, `description`, `shortcut`, `groupTitle`, `titleMatches`, `kind`.
    `Section`, `SectionRole`, `SectionStartRole` and `sectioned` go.
- `WindowController` builds it as `std::make_unique<CommandPaletteModel>(manager.commandHistory(), std::vector<command::CommandSource const*> { &_tabCommands, &_profileCommands, &_layoutCommands, &_hostCommands, &_boundCommands, &_actionCommands }, command::platformShortcutStyle(), this)`
  in the member-initializer list (the sources are declared before `_commandPalette`, `WindowController.hpp:940-946`),
  and `openCommandPalette` calls `refresh(command::shortcutIndex(config, command::platformShortcutStyle()))`.
  `runCommand` (`:367-388`) is unchanged: a child's id resolves through `commandById`.

- [ ] **Step 1: Write the failing tests** (`CommandPaletteModel_test.cpp`, `[contour][palette]`; every existing case is
  restated on the new roles, each keeping its assertion's meaning):
  - "With no filter, the palette groups its rows behind Recently used": row 0's `groupTitle` is "Recently used", the
    first non-recent row's is "Pane"; exactly one row per group carries a non-empty `groupTitle`.
  - "Typing ranks across groups and keeps their headers": filter `SplitVertical` → row 0 "Split Vertical", `groupTitle`
    "Pane", `shortcut` "Ctrl+Shift+E" (the existing shortcut case).
  - "A parameterised action is one drill-in row": with a `ProfileCommandSource` over profiles `dark`, `light`: one row
    with `kind == 1` titled "Change Profile", `shortcut` empty, `commandId` "ChangeProfile".
  - "enter() lists the children, back() restores the filter the level was entered from" (Review Focus 1 and 2):
    `setFilter("prof")`; `enter(rowOf("Change Profile"))` → `depth == 1`, `levelTitle == "Change Profile"`,
    `filter().isEmpty()`, `rowCount() == 2`, `anchorRow == 0`, `levelChanged` and `filterChanged` each emitted once
    (`QSignalSpy`); `setFilter("li")` → one row `light`; `back()` → `depth == 0`, `filter() == "prof"`, and
    `anchorRow` is the "Change Profile" row's index.
  - "enter() on a command row does nothing": `depth` stays 0, no signal.
  - "Reopening starts at the top": inside a level, `refresh({})` → `depth == 0`, `filter().isEmpty()`.
  - "Title-match highlight indices are UTF-16 code units through a child's prefix" (Review Focus 5): profile `"日本"`
    (three-byte characters); filter `本` at the top → the child's `titleMatches` is `{ QString("Change Profile · ").size() + 1 }`.
  - "footerHints follows the row and the depth": with `ShortcutStyle::Text`, row of a command at the top →
    `paletteFooterHints(Command, Top, Text)`; the drill-in row → the `DrillIn` text; inside a level → the `Level` text.
  - The existing cases "A remembered command that no longer exists quietly stops appearing", "Recency breaks a tie",
    "The shortcut column advertises the chord", "A zero recent_count turns the Recent section off entirely" keep their
    assertions, read through `groupTitle` instead of `section`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, no matching constructor / no `enter`.
- [ ] **Step 3: Implement** the model, then the controller, the scene's `makePaletteModel` and `MockPaletteController`
  (both construct with `{ &sources… }`, `ShortcutStyle`, then `refresh(shortcutIndex(…))`). The model keeps
  `std::optional<std::size_t> _level`, `QString _topFilter`, `std::vector<command::PaletteEntry> _rows`; a reset per
  rebuild, as today (`:115-118`).
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][palette],[contour][gui][qml][palette],[manager][palette],[contour][session][palette],[snapshot]"
```

  Expected: PASS. The QML cases pass against the bridged `CommandPalette.qml`; the two `sectioned()` reads are
  deleted, and "the palette bolds matched characters" still holds (the tint goes in Task 5).
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/CommandPaletteModel.cpp src/contour/window/CommandPaletteModel_test.cpp src/contour/window/WindowController.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/CommandPaletteModel.hpp src/contour/window/CommandPaletteModel.cpp src/contour/window/CommandPaletteModel_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/CommandPalette.qml
git add src/contour/window/CommandPaletteModel.hpp src/contour/window/CommandPaletteModel.cpp src/contour/window/CommandPaletteModel_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/QmlComponents_test.cpp src/contour/qml/CommandPalette.qml
git commit -m "contour: the palette model publishes the grouped listing and drills into parameterised actions" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The palette's tokens, colours, glyphs and placement

> **Depends on Steps 1c and 2:** 1c's `MotionStyle`, `detail::label(level)`, `detail::surface(level)`,
> `detail::schemeOnBar(slot)`, `SchemeSlot::SelectionText` and the `level` field of `ChromeColorSource`; Step 2's
> `detail::separator()`. 4a, 1c, 2 (and 3b, if landed) appended fields to `SizeTokens`, `SpaceTokens`, `TypeTokens`,
> `MotionTokens`, `LayoutTokens` and roles to `ChromeColorRole`: add this task's after theirs, by name.

**Files:**
- Create: `src/contour/window/PalettePlacement.hpp`; test `src/contour/window/PalettePlacement_test.cpp` (add to
  `contour_test`).
- Modify: `src/contour/config/ChromeTokenGroups.hpp` — `SizeTokens` (`:121-136` today), `SpaceTokens` (`:141-149`),
  `TypeTokens` (`:164-173`), `MotionTokens` (`:176-184`), `LayoutTokens` (`:234-240`), `ChromeColorRole` (`:61-69`) and
  its count, `ColorSourceKind` (`:72-79`), the four rows (`:336-437`), `resolveChromeTokens` (`:469-475`).
- Modify: `src/contour/config/UiStyle.hpp` — `UiStyleTokens` (`:77-220`), `NativeTokens` (`:295-357`), `TerminalTokens`
  (`:369-432`).
- Modify: `src/contour/window/ChromeTokenObjects.{hpp,cpp}` (`ChromeSizeTokens :35`, `ChromeSpaceTokens :97`,
  `ChromeTypeTokens :181`, `ChromeMotionTokens :216`, `ChromeLayoutTokens :390`, `ChromeColorTokens :424`; source
  resolution `.cpp:101-121`), `src/contour/window/UiStyleProvider.{hpp,cpp}`.
- Test: `src/contour/config/ChromeTokenGroups_test.cpp`, `src/contour/config/UiStyle_test.cpp`,
  `src/contour/window/UiStyleProvider_test.cpp`.

**Interfaces:**
- Produces (`config`), values as in Global Constraints:
  - `SizeTokens` gains `paletteWidth`, `paletteMaxHeight`, `paletteField`, `paletteRow`, `paletteGroupHeader`,
    `paletteFooter`; `SpaceTokens` gains `paletteInset`; `TypeTokens` gains `double paletteInput = 0.0; double shortcut = 0.0;`;
    `MotionTokens` gains `int popoverScalePercent = 0;`; `LayoutTokens` gains `int paletteTopPercent = 0;`.
  - `resolveChromeTokens` sets `motion.popoverScalePercent = 100` under `MotionPreference::Reduced`.
  - `ColorSourceKind` gains `PaletteHighlight` and `PaletteHighlightedText` (the application palette's `Highlight` and
    `HighlightedText`, as `PaletteWindow` reads `Window`).
  - `ChromeColorRole` gains `PaletteSurface`, `PaletteText`, `PaletteSecondaryText`, `PaletteSelectionFill`,
    `PaletteSelectionText`, `PaletteSeparator` after the last role; the count derives from `PaletteSeparator`. Sources
    per R-h.
- Produces (`config/UiStyle.hpp`): `UiStyleTokens` gains `std::string_view drillInGlyph; std::string_view palettePromptGlyph;`
  (`"›"`, `""` native; `"▸"`, `"❯"` Terminal).
- Produces (`window/PalettePlacement.hpp`, namespace `contour::window`, Qt-free):
  - `struct PaletteInputs { double windowWidth = 0; double windowHeight = 0; double width = 0; double maxHeight = 0; double marginX = 0; double marginY = 0; int topPercent = 0; double rowQuantum = 1; };`
  - `struct PalettePlacement { double x = 0; double y = 0; double width = 0; double maxHeight = 0; [[nodiscard]] bool operator==(PalettePlacement const&) const = default; };`
  - `[[nodiscard]] constexpr PalettePlacement placePalette(PaletteInputs const& in) noexcept;` — R-j:
    `width = clamp(windowWidth − 2·marginX, 0, in.width)`, `x = (windowWidth − width) / 2`,
    `y = floor(windowHeight · topPercent / 100 / rowQuantum) · rowQuantum`,
    `maxHeight = clamp(windowHeight − y − marginY, 0, in.maxHeight)`.
- Produces (QML-facing): `chromeStyle.size.paletteWidth` … `paletteFooter`, `chromeStyle.space.paletteInset`,
  `chromeStyle.type.paletteInput` and `shortcut` (`QFont`s, as the group's others), `chromeStyle.motion.popoverScale`
  (`qreal`, percent / 100), `chromeStyle.colors.paletteSurface` … `paletteSeparator`, `chromeStyle.drillInGlyph`,
  `chromeStyle.palettePromptGlyph` (`QString`), and
  `[[nodiscard]] Q_INVOKABLE QVariantMap palettePlacement(qreal windowWidth, qreal windowHeight) const;` on
  `UiStyleProvider`: `placePalette` with the row's tokens scaled by the units, `marginX`/`marginY` = `space.s8` in
  horizontal/vertical units, `rowQuantum` = the vertical unit on the Terminal row, else 1; returns
  `{ x, y, width, maxHeight }`.

- [ ] **Step 1: Write the failing tests:**
  - `PalettePlacement_test.cpp` (`[paletteplacement]`), `base = { .windowWidth = 1280, .windowHeight = 800, .width = 640, .maxHeight = 420, .marginX = 16, .marginY = 16, .topPercent = 18, .rowQuantum = 1 }`:
    - `palette.centredEighteenPercentDown`: `base` → `{ 320, 144, 640, 420 }`.
    - `palette.aSmallWindowShrinksIt`: `.windowWidth = 500, .windowHeight = 300` → `{ 16, 54, 468, 230 }`.
    - `palette.neverNegative`: `.windowWidth = 20, .windowHeight = 10` → `width == 0`, `maxHeight == 0`.
    - `palette.snapsToRowsOnTheTerminalRow`: `{ 960, 600, 384, 360, 8, 18, 18, 18 }` → `y == 108`, and with
      `.windowHeight = 610`: `y == 108` (109.8 floored to a row), `maxHeight == 360`.
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`, `everyRowMatchesTheDesignKit`): every new token per row from
    Global Constraints; the six colour sources per row (R-h); `chromeTokens.reducedMotion`: `popoverScalePercent == 100`
    on every row under `Reduced`, and 97 on the native rows without it.
  - `chromeTokens.paletteTextReads` (Terminal row, `defaultColorPalettes` contour-dark and contour-light): `PaletteText`
    on `PaletteSurface`, `PaletteSecondaryText` on `PaletteSurface` and `PaletteSelectionText` on `PaletteSelectionFill`
    are each ≥ 4.5:1 (`vtbackend::contrastRatio` over `chromeSchemeColorsOf`).
  - `UiStyle_test.cpp`: the two glyphs per row.
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`): MacOS publishes `size.paletteWidth == 640`, `size.paletteRow == 32`,
    `type.paletteInput.pointSizeF() == 17` (or pixel size, as the group's other fonts are read there),
    `motion.popoverScale == 0.97`; `palettePlacement(1280, 800)` is `{ 320, 144, 640, 420 }`; native `PaletteSurface`
    resolves to white under a light application palette and `#2C2C2E` under a dark one; `PaletteSelectionFill`
    equals the application palette's `Highlight`. Terminal: `size.paletteWidth == 48 * cellWidth`,
    `size.paletteRow == cellHeight`, `drillInGlyph == "▸"`, `motion.popoverScale == 1.0`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `PalettePlacement.hpp` not found.
- [ ] **Step 3: Implement** the header, the tokens and rows, the two source kinds' resolution beside `PaletteWindow`,
  the glyphs, the QML properties and the invokable.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[paletteplacement],[chrometokens],[uistyle]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/PalettePlacement_test.cpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/PalettePlacement.hpp src/contour/window/PalettePlacement_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/config/UiStyle_test.cpp src/contour/window/UiStyleProvider_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/window/PalettePlacement.hpp src/contour/window/PalettePlacement_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/config/UiStyle_test.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: tokens, colours and placement for the command palette on every row" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: `CommandPalette.qml` — grouped rows, drill-in and the footer, native and Terminal rows

**Files:**
- Rewrite: `src/contour/qml/CommandPalette.qml` (keeps `controller`, `window`, `objectName`s `commandPaletteFilter`,
  `commandPaletteList`, `commandPaletteTitle`, `commandPaletteShortcut`; the deferred dispatch `:57-97` and
  `highlightedTitle`'s escaping `:109-133` stay, `highlightedTitle` losing its colour branch per R-h).
- Modify: `src/contour/qml/PopupSurface.qml:46-88` — `property QtObject elevation: null`; when set, the shadow image
  reads `elevation.blur`/`elevation.offsetY` instead of `chromeStyle.shadowBlur`/`shadowOffsetY`, and the corner follows
  `root.radius` (see *Shared files*).
- Modify: `scripts/qml-size-literal-baseline.txt:6` (the `CommandPalette.qml` line goes: 19 → 0; `PopupSurface.qml`
  stays at 1).
- Test: `src/contour/test/QmlComponents_test.cpp` (`:3750-3820` the host; `:3827-4400` the palette cases),
  `src/contour/test/Accessibility_qml_test.cpp`.

**Interfaces:**
- Consumes: Task 3 (roles, `depth`, `levelTitle`, `anchorRow`, `enter`, `back`, `footerHints`); Task 4 (tokens,
  colours, glyphs, `palettePlacement`).
- Produces (QML structure, `objectName`s for tests):
  - `Popup` with `background: PopupSurface { color: chromeStyle.colors.paletteSurface; radius: chromeStyle.radii.popover; elevation: chromeStyle.elevation.e3 }`;
    `x`, `y`, `width` from `chromeStyle.palettePlacement(window.width, window.height)`; `height` =
    `min(placement.maxHeight, field + separator + list content + footer)`.
  - The query row (`size.paletteField`): `palettePromptGlyph` label when non-empty, a level chip `objectName:
    "commandPaletteLevel"` reading `levelTitle + " " + drillInGlyph` while `depth > 0`, then the `TextField`
    (`type.paletteInput`, `PaletteText`, placeholder copy; on an `Item` wrapper: `Accessible.role: Accessible.EditableText`,
    `Accessible.name: qsTr("Command")`).
  - A separator (`borderWidth` tall, `PaletteSeparator`) below the query row and above the footer.
  - The `ListView` (`commandPaletteList`), side padding `space.paletteInset`, delegate per row:
    - a group header (`objectName: "commandPaletteGroupHeader"`) of `size.paletteGroupHeader` when `groupTitle !== ""`:
      `type.footnote`, `Font.DemiBold`, `PaletteSecondaryText`, `Accessible.role: Accessible.Heading`;
    - the row (`size.paletteRow`): selection `Rectangle` at `radii.control` in `PaletteSelectionFill` when current;
      title (`type.body`, `PaletteText` / `PaletteSelectionText` when current; R-l alignment and elision); accessory
      (`commandPaletteShortcut` in `type.shortcut` for a command, `commandPaletteDrillIn` reading `drillInGlyph` for a
      `kind === 1` row, both `PaletteSecondaryText` / `PaletteSelectionText`); `Accessible.role: Accessible.ListItem`,
      `Accessible.name: title`, and on a drill-in row `Accessible.description: qsTr("Opens a list")`.
  - Empty list: a `Label` `objectName: "commandPaletteEmpty"` "No matching commands", `PaletteSecondaryText`, one row
    tall.
  - Footer (`size.paletteFooter`, `objectName: "commandPaletteFooter"`): description of `list.currentItem` (elided),
    and `controller.commandPalette.footerHints(list.currentIndex)` right-aligned, both `type.footnote`,
    `PaletteSecondaryText`.
  - Keys (on the field, R-f): Return/Enter → `kind === 1 ? model.enter(currentIndex) : acceptCurrent()`; Escape →
    `model.depth > 0 ? model.back() : root.close()`; Backspace with empty text and `depth > 0` → `back()`, accepted;
    ↑/↓, ⌃J/⌃K as today. A click does what Enter does. On `levelChanged`: the field's text takes `model.filter`,
    `list.currentIndex = model.anchorRow`, `list.positionViewAtIndex(anchorRow, ListView.Contain)`. Moving the
    selection calls `positionViewAtIndex(…, ListView.Contain)` (Review Focus 3).
  - No size or colour literal; every size from `chromeStyle`.
- **Terminal row:** the same items in cells: one-row query line with `❯`, one-row group headers in Secondary, one-row
  rows with the selection in the scheme's selection colours and `▸`, a one-row footer, square corners, the hard
  one-cell shadow from `e3`.

- [ ] **Step 1: Write the failing tests** (`QmlComponents_test.cpp`, `[contour][gui][qml][palette]`; the host gains a
  `ProfileCommandSource` and `LayoutCommandSource` over a config with profiles `dark`, `light` and layout `split-dev`,
  and `MockPaletteController` builds the model with them):
  - "The palette is 640 wide, 18 % down, with 32-tall rows (offscreen)": host window 1280 × 800 under the MacOS row
    (`QmlChromeStyle`): `palette.width == 640`, `palette.x == 320`, `palette.y == 144`, `palette.height <= 420`; a row's
    height is `size.paletteRow`.
  - "Rows are grouped, and only the first row of a group draws a header (offscreen)": the visible
    `commandPaletteGroupHeader` items read "Recently used", "Pane", … in `PaletteGroups` order.
  - "The footer shows the selected row's description only (offscreen)": `commandPaletteFooter`'s description equals
    the current row's `description`; after ↓ it equals the next row's; no row delegate shows a description.
  - "A drill-in row shows › and Enter opens its level (offscreen)": select "Change Profile" (filter `profile`), its
    `commandPaletteDrillIn` reads `drillInGlyph`; Return → `model.depth == 1`, `commandPaletteLevel` visible with
    "Change Profile ›", the field empty, two rows `dark` and `light`; Return on `dark` runs `ChangeProfile:dark` after
    close (`controller.ran == {"ChangeProfile:dark"}`).
  - "Escape in a level goes back one level, then closes" (Review Focus 2): in the level, Escape → palette still
    `opened`, `depth == 0`, field text `profile`, `currentIndex` on "Change Profile"; Escape again → closed, nothing run.
  - "Backspace in an empty field leaves the level (offscreen)": in the level with an empty field, Backspace →
    `depth == 0`; with text in the field, Backspace edits the text.
  - "A filter surfaces a child, which runs directly (offscreen)" (Review Focus 1): type `split` → a row titled
    "Launch Layout · split-dev" under a "Layouts" header, no `commandPaletteDrillIn` on it; selecting it and Return runs
    `LaunchLayout:split-dev`.
  - "A 200-row level keeps the selection in view (offscreen)" (Review Focus 3): profiles `p000`…`p199`; enter the level;
    press ↓ 150 times → `currentIndex == 150` and the current item lies within `[contentY, contentY + height]`.
  - "A long or right-to-left title elides inside its row (offscreen)" (Review Focus 5): a `TabCommandSource` over a
    stub `TabTitleProvider` giving one 300-character title, and a profile `שלום`; filter by each; the long row's title has `truncated == true` and its right edge plus the
    accessory's width fits the row; the Hebrew child's title has `effectiveHorizontalAlignment == Text.AlignLeft`; the
    accessory's `x + width <= row width` on both.
  - The existing cases keep their meaning: filtering through the model, accepting runs and gives the keyboard back,
    OpenCommandPalette re-opens cleanly, dismissing runs nothing, controller destruction without errors, ⌃J/⌃K.
    "The palette bolds matched characters and tints only unselected rows" becomes "Matched characters are bold, in the
    row's own colour (offscreen)": both the selected and an unselected row's StyledText contain `<b>` around the matched
    characters and no `<font color` (R-h).
  - "The Terminal row draws the palette in cells (offscreen)" (run under `CONTOUR_TEST_CONTROLS_STYLE=ContourTui` with
    the Terminal row): `width == 48 * cellWidth`, a row's height `== cellHeight`, the surface `radius == 0`, the prompt
    glyph `❯` visible, and the drill-in glyph `▸`.
  - `Accessibility_qml_test.cpp` (`[a11y]`): the field's wrapper, a row and a group header each declare a role and a
    name.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][palette]"`

  Expected: FAIL (no `commandPaletteFooter`, width 720).
- [ ] **Step 3: Implement** the QML, the `PopupSurface` change and the baseline line.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][palette],[contour][palette],[a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][palette],[a11y]"
ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$" --output-on-failure
```

  Expected: PASS. The ContourTui run is the Terminal row.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/CommandPalette.qml src/contour/qml/PopupSurface.qml scripts/qml-size-literal-baseline.txt
git add src/contour/qml/CommandPalette.qml src/contour/qml/PopupSurface.qml scripts/qml-size-literal-baseline.txt src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
git commit -m "contour: the command palette groups its rows, drills into parameterised actions and shows the description in a footer" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: Motion under Reduce Motion, and the focus hand-back over a modal

**Files:**
- Modify: `src/contour/qml/CommandPalette.qml` (`enter`/`exit` transitions; `onAboutToShow`; `onClosed`, `:72-79` today).
- Modify: `src/contour/test/MockMainController.hpp:163` (`runCommand`).
- Test: `src/contour/test/QmlComponents_test.cpp`, `src/contour/test/MainWindowQml_test.cpp`.

**Interfaces:**
- Consumes: Task 4 (`motion.standard`, `motion.fast`, `motion.popoverScale`); 1c's `motion.style`, `full`, `crossFade`.
- Produces (QML):
  - `enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: chromeStyle.motion.standard } NumberAnimation { property: "scale"; from: chromeStyle.motion.popoverScale; to: 1; duration: chromeStyle.motion.standard } }`
    (`objectName`s `commandPaletteEnterFade`, `commandPaletteEnterScale` on the animations, for the tests);
    `exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: chromeStyle.motion.fast } }`.
    Easing: `Easing.OutCubic` in, `Easing.InCubic` out (kit: "ease-out in / ease-in out"). Under Reduce Motion the
    tokens make the scale animation a no-op (from 1) and both durations 100 ms; no branch in QML.
  - `property Item _returnFocus: null`, set in `onAboutToShow` to `root.window ? root.window.activeFocusItem : null`.
  - `function _isOverOverlay(item)`: walks `item.parent` up and answers whether it meets `Overlay.overlay` (R-k).
  - `onClosed`: if `_returnFocus` is non-null and `_isOverOverlay(_returnFocus)`, `_returnFocus.forceActiveFocus()`;
    else `root.window.restoreTerminalFocus()`; then the deferred dispatch as today; `_returnFocus = null`.

- [ ] **Step 1: Write the failing tests:**
  - `QmlComponents_test.cpp` (`[contour][gui][qml][palette]`):
    - "The palette opens with a 160 ms scale and fade (offscreen)": under the MacOS row, `commandPaletteEnterScale.from == 0.97`,
      its `duration == 160`, `commandPaletteEnterFade.duration == 160`, the exit fade's `duration == 100`.
    - "Under Reduce Motion the palette only fades (offscreen)": a `QmlChromeStyle` resolved with
      `MotionPreference::Reduced` → `commandPaletteEnterScale.from == 1`, both enter durations `== 100`; after open,
      `palette.scale == 1` throughout.
  - `MainWindowQml_test.cpp` (`[contour][gui][qml][mainwindow][palette]`), Main.qml over `MockMainController` with a
    palette model:
    - "Closing the palette over a dialog gives the dialog its focus back (offscreen)" (Review Focus 4): open
      `aboutDialog` and give its accept button focus; `controller.openCommandPalette()` → the palette is `opened` above
      it; send Escape → the palette is closed, `aboutDialog.opened` is still true, and the window's `activeFocusItem`
      is inside `aboutDialog` (its ancestors include the dialog's `contentItem`), not the terminal pane.
    - "A picked command that opens a dialog keeps that dialog focused (offscreen)": `MockMainController::runCommand`
      (`MockMainController.hpp:163`, a no-op today) records the id and, for `ShowAbout`, calls the base's `showAbout()`
      (`MainWindowControllerMock.hpp:96`); pick "Show About" in the palette → after the palette has closed,
      `aboutDialog.opened` and the window's `activeFocusItem` is inside it (the terminal focus restore ran before the
      dispatch, not after).
    - "Without a dialog, closing hands the keyboard back to the terminal (offscreen)": the existing behaviour, through
      Main.qml.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][palette]"`

  Expected: FAIL (no `commandPaletteEnterScale`; focus goes to the terminal).
- [ ] **Step 3: Implement** the transitions, the hand-back, and the mock's `runCommand`. The Main.qml cases build the
  mock's palette model from an `ActionCommandSource`, as `MockPaletteController` does.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][palette],[contour][gui][qml][mainwindow]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][palette]"
ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$" --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/test/MockMainController.hpp
editorconfig-checker src/contour/qml/CommandPalette.qml
git add src/contour/qml/CommandPalette.qml src/contour/test/QmlComponents_test.cpp src/contour/test/MainWindowQml_test.cpp src/contour/test/MockMainController.hpp
git commit -m "contour: the palette scales and fades in (a fade under Reduce Motion) and gives the keyboard back to the dialog it opened over" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: Snapshot rows — grouped, filtered and drilled in

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` — `SnapshotScript` (`:35-49`, append `PaletteFiltered`,
  `PaletteDrilledIn` after the last enumerator at execution), `ScriptAction` (`:66-74`, append `FilterPalette`,
  `DrillInPalette`), `SnapshotScripts` (two rows), and the count `static_assert`.
- Modify: `src/contour/test/ChromeSnapshotScene.hpp` — `show()` (`:161-185`), `makePaletteModel` (`:304-315`), members
  (`:487`): a scene palette config and its profile and layout sources (R-m).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/test/ChromeSnapshot_test.cpp:60-123`.

**Interfaces:**
- Consumes: Tasks 3, 5 (`commandPaletteFilter`, `enter`, `depth`, `levelTitle`).
- Produces: rows `{ .script = PaletteFiltered, .name = "palette-filtered", .tabCount = 1, .panes = SingleLeaf, .surface = MainWindow, .action = ScriptAction::FilterPalette, … }`
  and `{ .script = PaletteDrilledIn, .name = "palette-drill-in", …, .action = ScriptAction::DrillInPalette, … }`, every
  other field (including those 1c, 1d, 2 and 3b added) named with the `palette` row's values.
- Scene: `FilterPalette` opens the palette as `OpenPalette` does, then sets `commandPaletteFilter`'s `text` to `split`.
  `DrillInPalette` opens it, sets the filter to `profile`, and calls the model's `enter(0)` (row 0 is "Change Profile"
  by Task 2's group ordering), then waits for `depth == 1`. The scene config holds profiles `work`, `presentation`
  beside the default, and layouts `split-dev`, `logs`; `makePaletteModel` takes the action, profile and layout sources.

- [ ] **Step 1: Write the failing tests:**
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): `scriptRow(SnapshotScript::PaletteFiltered).action == ScriptAction::FilterPalette`,
    `scriptRow(SnapshotScript::PaletteDrilledIn).action == ScriptAction::DrillInPalette`; the names are unique (the
    existing uniqueness check covers them).
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): `case PaletteFiltered:` the palette is opened, the
    first `commandPaletteGroupHeader` reads "Pane", and a row reads "Launch Layout · split-dev"; `case PaletteDrilledIn:`
    the model's `depth == 1`, `levelTitle == "Change Profile"`, and the list's count equals the scene's profile count;
    `case Palette:` the first header reads "Pane" (no history in the scene) and some row is a drill-in row.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `SnapshotScript::PaletteFiltered`.
- [ ] **Step 3: Implement** the rows, the actions and the scene.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][snapshot]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git add src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp
git commit -m "contour: snapshot the palette filtered and drilled in" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts`, so the plan's
  own commit is left out) and apply its findings. Re-run the filters of every task it touches. Commit with the
  Signed-off-by line, staging only the files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[contour][palette],[commandcategory],[palettetree],[paletteplacement],[chrometokens],[uistyle],[snapshot],[command]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][palette],[manager][palette],[contour][session][palette],[contour][uistyle],[a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][palette],[a11y],[contour][gui][qml][snapshot]"
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
  `ChromeSnapshotPlan.hpp`. Look at `palette.*` (640 wide, 18 % down, groups with sentence-case headers, one-line
  rows, the footer), `palette-filtered.*` (Pane, then Layouts with "Launch Layout · split-dev", bold matches) and
  `palette-drill-in.*` (the "Change Profile ›" chip, three profiles); on the Terminal row, cells, `❯` and `▸`.
  `single-tab.*` and `kit-dialog.*` look as they did (the `PopupSurface` change touches every popup's shadow only
  when `elevation` is set).
- [ ] **Step 4: Owner manual checks.** Batched per phase (ruling): list them in the ledger for the owner; do not wait on
  them. On this Mac, `out/appleclang-debug/src/contour/contour.app`:
  - ⇧⌘P (Ctrl+Shift+P): the palette opens with a short scale and fade; with Reduce Motion on, a fade only.
  - The rows group under Recently used, Pane, Tab, …; ⌃J/⌃K and the arrows move; the footer shows the selected row's
    description and "↩ Run · ⎋ Close".
  - "Change Profile ›" → Enter lists the profiles; typing narrows them; Escape goes back with the old query; Escape
    closes. Typing a profile's name at the top runs it directly.
  - Attach to Host › lists the saved hosts with their URL in the footer.
  - Open About from the menu bar, then the palette from the View menu (or the menu bar's palette row), Escape: the
    About dialog still has the keyboard.
  - VoiceOver reads the field as "Command", a header as a heading, and a drill-in row's "Opens a list".
  - `ui_style: terminal`: the palette is drawn in cells, `❯` before the query, `▸` on drill-in rows.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Groups the command palette's rows (Pane, Tab, Window, Layouts, Blocks, Profiles, Hosts, Edit, View, Shell,
    Contour), most recently used first."
  - "Collapses commands that take a name, such as Change Profile, Launch Layout, Attach to Host and Switch to Tab, into
    one row that opens their list; typing a name at the top still finds and runs it directly. Escape steps back."
  - "Redraws the palette at 640 points with one-line rows and the selected command's description in a footer, and
    opens it with a short scale and fade, a fade only under Reduce Motion."
- [ ] **Step 6:** Mark Step 3d done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-3d.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the render path. The tree is built once per open (some 130 commands plus the live rows);
    each keystroke lists it once, as the old ranking did, with children matched on their short argument.
  - **Risk:** low-medium. The palette model's API changes (`setSources`/`setShortcuts`/`sectioned` go); the focus
    hand-back changes where the keyboard lands after Escape when a dialog was open; Fluent and ContourLinux extents
    beyond the kit's row heights are provisional (5a/5b).
  - **Coverage:** `[commandcategory]` (the coverage of every catalogue action), `[palettetree]` (grouping, drill-in,
    filtering across children, 200 profiles, RTL offsets, hints), `[paletteplacement]`, the model's level cases, the QML
    cases on the native and Terminal rows, two Main.qml focus cases, and two new snapshot scripts.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Groups beyond §3.10's seven (R-b).** Seventy-odd actions (copy, scroll, font size, reset, quit) fit none of Pane,
   Tab, Window, Layouts, Blocks, Profiles, Hosts. *Default: four more groups named after the macOS menu bar's menus —
   Edit, View, Shell, Contour — after the seven.* The alternative is one "Other" group, which would be the palette's
   largest.
2. **What drills in (R-c).** *Default: every parameterised action, Switch to Tab included, and even with one child (one
   rule).* The alternative keeps tabs flat ("Switch To Tab 3: vim" at the top, as today) and shows a single child
   inline.
3. **Contrast over the canvas's colours (R-h).** *Default: the selected row takes the OS's own selected-row pair
   (5.3:1) instead of white on the accent (4.0:1); headers, shortcuts and the footer use secondary text instead of
   tertiary (3.4:1 on light); matched characters are bold without the accent tint.* The alternative follows the canvas
   exactly and accepts sub-4.5:1 text on the light appearance.
4. **A surfaced child has no `›` (R-e).** The canvas draws "Launch Layout · split-dev ›", but that row runs the layout.
   *Default: no `›` on it; `›` only where Enter opens a list.* The alternative draws it and makes Enter on a surfaced
   child open the parent's level with that child selected.
5. **"⌥↩ Run and keep open" (R-g).** The canvas's footer offers it; §3.10 does not. *Default: not built; the footer
   shows Run/Open and Close/Back.* The alternative runs the command without closing, which needs a decision about focus
   while the palette stays up.

## Spec gaps found

- §3.10's seven groups cover about a third of the catalogue (owner decision 1), and it does not say whether groups
  survive filtering; the canvas keeps them, so the plan does, ordered by best match (R-e).
- §3.10 does not say how a query meets a drill-in level: children matching on their argument and surfacing as
  "Parent · child", and a filter inside a level, are the plan's (R-e, Review Focus 1). Nor what Escape does in a level
  (R-f).
- Geometry beyond "640 wide, rows 32 high" comes from the canvas (max 420, 18 %, radius 14, e3, 160 ms, 0.97); the
  Fluent and ContourLinux palettes are not drawn, so their field, header and footer heights reuse the macOS values
  and only the row height follows the kit (36, 38). The Terminal row's 48 × 20 cells are read off the `tui` board.
- "Popover glass" is drawn by no surface yet (R-i, as 3b R-n).
- The canvas's tertiary text, accent tint and white-on-accent selection fall below the 4.5:1 ruling on the light
  appearance (owner decision 3).
- The daemon board's "Connect to Address…" child, session counts and last-seen have no source; the canvas's titles
  ("Split Right ⌘D") are not the catalogue's, and its ⌘ chords are issue #2175. "⌥↩ Run and keep open" is not in §3.10
  (owner decision 5).
