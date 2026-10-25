# Native Redesign — Step 2 (Panes: Cards, Headers, Gap and Hit Area, Densities, Pane States) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead of Steps 4a, 1c and 1d (pipelining ruling). Line numbers are at HEAD `d8053e4f`, before any of
them lands. Where Step 2 touches code they add or change, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference once 1d has landed.

**Goal:** A split tab becomes cards with headers, a 6-pt gap that is the divider, a 10-pt hit area and a
2 × 32 grabber; a single pane stays edge to edge; focus is shown by dimming and header emphasis, never an
outline; a global `chrome_density: comfortable | minimal` switches the size sub-row; a pane that is
reconnecting shows a pill, and a pane whose process ended shows `ExitedFooter.qml` in its bottom chrome.

**Architecture:**
- **Pure decisions first, Qt-free, tested in `contour_test`.** `window/PaneHeaderPolicy.hpp` decides
  whether a header shows, what it holds, where, how emphasised, and whether its actions show.
  `window/HeldModifierReveal.hpp` decides when a held ⌥/Alt reveals headers in Minimal.
  `window/SplitChrome.hpp` turns the resolved tokens into the size solver's metrics.
  `window/PaneState.hpp` decides what an ended pane's footer says and offers.
- **Density is a token sub-row.** `config::ChromeDensity { Comfortable = 0, Minimal }` is a new global
  config key and a new `ChromeInputs` field; `resolveChromeTokens` overlays the row's Minimal sub-row. A
  change rebuilds the provider (§2.1), never mutates it.
- **Per-pane data lives on `TerminalSession`** as notifying `Q_PROPERTY`s that QML reads through the
  leaf's `PaneProxy::session`. The foreground-process poll that today exists only for extensions becomes
  the session's, armed by a follower bitmask (extension, header); the window controller decides which
  sessions have a visible header.
- **One component tree.** `PaneNode.qml` keeps the `SplitView`; its handle becomes the gap with a wider
  `containmentMask`. A new `PaneCard.qml` wraps each leaf: `PaneHeader.qml`, the unchanged
  `TerminalPane.qml` (minus its focus frame), and `ExitedFooter.qml`. The Terminal row draws the same
  components in cells.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), QML, Catch2 v3, Python 3 (lint), CMake build tree
`out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.6 (`:469-519`)
binds, with what it cites: §1 (principles, `:28-64`), §2.1 (token rows and the density group, `:68-126`;
contrast overrides `:120-126`), §2.3 (Reduce Motion, `:139-167`), §2.6 (testing, `:206-226`), §2.7 (the
edition gate, `:228-284`), §3.16 row 6.3 (`ExitedFooter` is Step 2's, `:821`), §4 ("Per-pane properties
cost redraws", `:836`). The spec's code citations have moved: the handle constant is still
`PaneLayout.hpp:37`; the solver call is `WindowController.cpp:1826-1835`; the dim layer is
`TerminalPane.qml:181-203` and the reconnecting overlay `:205-223`; the extension poll is
`TerminalSession.cpp:1163-1177` (`armForegroundProcessPoll`) and `:1243-1261` (`pollForegroundProcess`).
The design record is `docs/superpowers/specs/2026-10-23-native-redesign-design.md` ("Pane cards with headers
appear only once a tab is split, and focus is shown by dimming, not by an outline"; "There are two:
Comfortable and Minimal"; "The pane reconnecting overlay dims by the pane-dim token and shows a toast-style
pill"; `chrome_density: comfortable`). The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>;
its local copies `mac-dark`, `mac-density`, `kit-components`, `kit-tokens`, `tui`, `moments` (5),
`daemon` (2), `win` and `linux` are in `.superpowers/design/canvas/project/`. The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Steps 4a, 1c and 1d
(`2026-10-25-native-redesign-step-4a.md`, `-1c.md`, `-1d.md`) are assumed done.

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

- **The spec, verbatim (§3.6):**
  - "`displayWorkingDirectory`: notifies on OSC 7 and OSC 3008 changes."
  - "`foregroundProcessName`: generalises the polling that today runs only for extensions … It runs only
    while a header is visible, every 1 s."
  - "`programStatus`: per pane. The tab aggregate stays."
  - "The visual gap is `space.paneGap`: 6 in Comfortable, 1 in Minimal. The size solver uses it."
  - "The handle becomes a transparent item of `space.splitHit` width (10 / 8), with a centred grabber,
    2 × 32, shown on hover. The hit area overlaps the cards and never changes the layout."
  - Header policy inputs "all enums with zero meaning none": `ChromeDensity`, `HeaderReveal { None = 0,
    HeldModifier }`, `PointerPresence { Away = 0, Hovering }`, `PaneFocus { Unfocused = 0, Focused,
    KeyboardFocused }`, the visible pane count, the width in points. Outputs `PaneHeaderContent { Hidden = 0,
    Name, NameAndCwd }`, "where the cwd appears from 360 pt wide", and `HeaderActions { Hidden = 0, Shown }`.
  - "A new global key, `chrome_density: comfortable | minimal`. Changing it rebuilds the provider."
  - "Reconnecting: the pane dims by the token, plus a static `Toast` pill (from 1a)."
  - "Disconnected or exited: `ExitedFooter.qml` sits in the pane's bottom chrome, never over the cells.
    Return restarts the session."
  - TUI row: "The header is one row of cells: name, cwd, and `⊞ ⊟ Z ×`. Splits are `│` and `─` cell lines.
    Inactive panes are drawn at 0.62 opacity. In Minimal, the bar row is hidden while there is one tab, and
    there are no headers."
  - §2.1: `ChromeDensity { Comfortable = 0, Minimal }` "selects a size sub-row"; the provider is built
    "from Step 2, [with] the density". `ContrastPreference::Increased`: "the active pane card gets a 1.5-pt
    accent ring; the pane dim drops to 5 %".
- **Canvas values, exact.** Comfortable · Minimal where two are given (`mac-density`, `kit-tokens`,
  `kit-components`, `mac-dark`, `win`, `linux`, `tui`, `moments`):

  | Token | MacOS | Fluent | ContourLinux | Terminal (cells) |
  |---|---|---|---|---|
  | `size.paneHeader` (exists) | 22 · 0 | 28 · 0 | 28 · 0 | 1 · 0 |
  | `space.paneGap` (exists), new `space.paneGapRows` | 6 · 1 | 4 · 1 | 6 · 1 | 1 · 1 |
  | `space.splitHit` (exists) | 10 · 8 | 10 · 8 | 10 · 8 | 1 · 1 |
  | `space.paneInset` (exists; the cards' inset from the content edge) | 6 · 0 | 4 · 0 | 6 · 0 | 0 · 0 |
  | `radius.paneCard` (exists) | 10 · 0 | 4 · 0 | 6 · 0 | 0 · 0 |
  | `size.bareBand` (a band with neither tabs nor title) | 28 | 40 | 46 | 1 |
  | `size.grabber` × `size.grabberLength` | 2 × 32 | 2 × 32 | 2 × 32 | 0 × 0 |
  | `size.paneHeaderAction` (hit; glyph 15) | 22 | 22 | 22 | 1 |
  | `space.paneHeaderGap` | 6 | 8 | 8 | 1 |
  | `type.paneHeader` / `type.paneHeaderWeight` | 11 / 500 | 12 / 600 | 12.5 / 600 | 0 / 400 |
  | `size.paneFooter` / `size.footerButton` | 34 / 24 | 34 / 24 | 34 / 24 | 1 / 1 |
  | `space.footerLeading` | 14 | 14 | 14 | 1 |
  | `motion.headerRevealMs` | 500 | 500 | 500 | 500 |
  | `dim.darkPercent` / `dim.lightPercent` (`kit-tokens` "pane.dim") | 15 / 10 | 15 / 10 | 15 / 10 | 15 / 10 |

  - Header padding: leading `s5` (10), trailing `s3` (6). Footer: trailing `s5` (10), gap `s4` (8). Footer
    text `type.tab`; buttons `radius.control`.
  - Card: fill = the pane's own terminal background; a one-device-pixel border in the separator colour
    ("0.5 pt on Retina, 1 px elsewhere"); `elevation.e1` on the active card only.
  - Header text: active `label.primary`, inactive `label.secondary` ("drops to secondary (60 %), never
    below"); cwd `label.secondary`, middle-elided. Actions fade in over `motion.fast` (100 ms).
  - Minimal: no headers ("⌥-hold shows them"), "hold ⌥ for half a second", `macOS` band 28 "painted in the
    terminal background — traffic lights only, no title, no tabs".
  - Footer (`moments` 5): "Process exited with code 1." · "Restart ↩" (accent) · "Close ⌘W".
  - Pill (`daemon` 2): "Reconnecting to starfive…"; the grid dims.
  - Glyphs: native header actions `⊞ ⊟ ⤢ ✕` (`win`, `linux`); Terminal row `⊞ ⊟ Z ×` and `│` / `─`.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e; the 4a, 1c and 1d SDD ledgers):
  - Status colours are tokens; Running is the accent, never green or yellow.
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`); a new file's baseline
    is 0 and a baseline only goes down.
  - Every visual surface is a snapshot row, and every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - Reduce Motion: every transition becomes the 100 ms cross-fade (`motion.crossFade`, 1c R-j).
  - Text contrast at least 4.5:1, checked against contour-dark and contour-light.
  - `enum class` over `bool`, zero the default; QML-facing `Q_PROPERTY`/`QVariantMap` booleans are the
    documented carve-out.
  - Any new default goes through `DefaultsEdition`'s table (`EditionDefaults`, `Config.hpp:1035-1121`).
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`; Classic's `dim_unfocused` is 0.
  - 1c D3: a transient, user-driven overlay of the top rows (full-screen reveal) is accepted; Step 2's
    Minimal reveal follows it.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven tables (header rules per density, header actions, footer rows,
  the Minimal sub-rows); dependency injection (the foreground process is read through `vtpty::Pty`, which
  `MockPty` fakes); configuration at construction (the provider is rebuilt per density; no setters on it);
  Doxygen on every new public declaration; `.hpp` with `#pragma once`; zero warnings and no `NOLINT`;
  `slots`, `signals` and `emit` are Qt macros, never identifiers. This step touches
  `_terminatedAndWaitingForKeyPress` (a `bool` state that has four states now), so it becomes an enum.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in
  `out/appleclang-debug/CMakeCache.txt`): `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`.
  Never run two builds at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows. A
  task touching `vtworkspace` also runs `ctest --test-dir out/appleclang-debug -L daemon`.
- **Format** every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`. Check every new
  or touched `.qml`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`. **Never clang-format a
  `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a
  value, and every table row names every field (`ChromeTokenRows`, the Minimal sub-rows, `DensityHeaderRules`,
  `PaneHeaderActions`, `PaneFooterRows`, `EditionDefaultsTable`, `SnapshotScripts`, `TitleBandRules`,
  `TitleBandSurfaceRows`).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
  Counts in tests are derived from tables (`ChromeDensityCount`, `PaneHeaderActions.size()`,
  `SnapshotScripts.size()`), never literals.
- **QML:** enums are unreachable through `chromeStyle`; QML passes and compares `int`s or the name strings
  the provider publishes. Every new control sets `Accessible.role` and `Accessible.name`.
- **Qt-free targets stay Qt-free:** the four new `window/*.hpp` and `config/ChromeDensity.hpp` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files by name (other agents share the tree; never
  `git add -A` or a directory). Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, and never carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 13).

## Review Focus

1. **A deep split tree in a small window.** Eight nested splits in a 320 × 200 window must not give any
   pane a negative extent, overlap headers, or emit QML warnings. Expected: a pane too short for a header
   plus one cell row shows no header (its terminal keeps the room); gaps and the inset still add up; the
   solver never undershoots. Pinned in Task 1 (`paneHeader.tooShortHasNoHeader`), Task 6
   (`PaneLayout: a deep tree counts a gap per level and the header and inset once`) and Task 9 ("a deep tree
   in a small window").
2. **Dragging a split where the hit area overlaps a card's scrollbar.** The 10-pt hit area reaches 2 pt into
   each card (3.5 pt in Minimal), where the left card's right-edge scrollbar sits. Expected: a press within
   the overlap drags the divider; a press on the scrollbar's centre scrolls and never starts a drag. Pinned in
   Task 8 ("a press inside a card's edge drags the divider" and "the scrollbar's centre still scrolls", in
   both densities).
3. **A header's cwd on a very long path.** A 4 096-character directory (or one holding a bidi override) must
   not widen the header, push the actions out, or elide the name. Expected: the cwd is home-abbreviated,
   sanitized, middle-elided, the name stays whole, the actions stay reachable, and the full path is in
   `Accessible.name`. Pinned in Task 4 (the property is sanitized and abbreviated) and Task 9 ("a long cwd
   elides in the middle").
4. **Foreground-process polling with many panes.** Twelve panes in four tabs must not run twelve timers.
   Expected: only sessions whose header is visible poll, once a second; switching tabs moves the poll; a
   poll that finds the same name emits nothing; a single pane and Minimal poll nothing. Pinned in Task 5
   (`[manager][processname]`).
5. **Minimal density with a single tab.** Expected: on macOS a bare 28-pt band in the terminal background
   with the traffic lights and no title; no pane header, even with ⌥ held (one visible pane); the terminal
   never covered; a second tab brings back the 38-pt band with the well; a user's `tab_bar_visibility:
   always` still shows the well; Fluent, Linux and the Terminal row unchanged. Pinned in Task 1
   (`paneHeader.singlePaneNeverHasAHeader`), Task 7 (`titleBand.minimalBareBand`, "Minimal with one tab is
   a bare 28-pt band") and Task 12 (`single-tab-minimal`).

Also pinned, beyond the five:
- A pane whose profile background is light under a dark chrome keeps its header legible (Task 3,
  `colorOn`; Task 9, the contrast case).
- An ended pane swallows typed text instead of writing to a dead PTY, and ⌘W still closes it (Task 10).
- Increased Contrast: the active card's 1.5-pt accent ring and the 5 % dim (Tasks 3 and 9).

---

## Spec resolutions (read before Task 1)

These settle where the spec is silent, or where the canvas or the code disagrees with it. Those marked
**(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Canvas values win over the brief's paraphrase.** The canvas draws the macOS pane header at 22 pt
  and the dim at 15 % (dark) / 10 % (light); `ChromeTokenGroups.hpp:329` already holds 22. Those are used.
- **R-b. Cards reach every row (owner, 1).** §3.6 does not restrict panes to macOS, and `win`/`linux` draw
  their cards (radius 4 / 6, gap 4 / 6, header 28). §2.1's "Fluent and ContourLinux keep today's geometry"
  is read as the band and tab geometry 1c's acceptance test pins; that test passes unedited.
- **R-c. Both editions default to `comfortable` (owner, 2).** The key is new, so no prior value exists to
  keep; pane cards are a restyle like 1c's pills, which no edition gates. The value is still a column of
  `EditionDefaults`, so a later edition changes one cell.
- **R-d. What the header policy also reads.** Beyond §3.6's inputs it takes the pane's height, the header
  height and the minimum content height (one cell row) so a short pane drops its header (Review Focus 1),
  and `PaneActionsReveal` (a layout token: `OnHover` on native rows, `OnHoverOrFocus` on the Terminal row,
  `tui`: "actions … on hover or active"). It also outputs `HeaderPlacement { Inline = 0, Overlay }` and
  `HeaderEmphasis { Secondary = 0, Primary }`. `KeyboardFocused` (the pane was reached by a `FocusPane*`
  action) shows the actions, as hover does.
- **R-e. Minimal's ⌥-hold reveal is a transient overlay (owner, 4).** `mac-density` says "hold ⌥ for half a
  second to show headers in the band". Drawing every pane's header in one band has no layout; resizing the
  panes on a modifier hold would reflow every grid. So, as 1c D3 accepted for the full-screen band, the
  headers draw over each pane's top row while ⌥/Alt alone is held for `motion.headerRevealMs` (500); any
  other key or click, or the release, hides them. Their fill is the card's own background, opaque.
- **R-f. Minimal and the tab bar.** Minimal changes the band only where 1c's band would show a centred title:
  `Pill`, client band, strip gate hidden. There the band is bare (`size.bareBand`, fill = terminal
  background, centre empty). `tab_bar_visibility: always` keeps the well. The Terminal row under `Multiple`
  already hides its bar row with one tab, which is §3.6's "In Minimal, the bar row is hidden".
- **R-g. Focus stays visible in Minimal (owner, 5).** Minimal has no headers, so with `dim_unfocused: 0`
  (Classic's default) no pane would look focused. Under Minimal, the effective unfocused dim is
  `max(dim_unfocused, pane.dim)`; under Comfortable it is `dim_unfocused` (the header emphasis carries
  focus). Under Increased Contrast either is capped at 5 %.
- **R-h. `space.paneInset` is the cards' inset from the content edge** (`mac-density`: "padding 6 · card
  radius window − 6"), not "content inside its card" as `ChromeTokenGroups.hpp:138` says. The comment is
  corrected.
- **R-i. The dim keeps today's blend.** `TerminalPane.dimOverlayColor` (`:184-188`) blends toward the pane's
  own background; the canvas draws a black scrim. §3.6 does not ask to change it, and its tests stay. The
  dim layer covers the terminal only, so the header above it is never dimmed (`mac-dark` 6).
- **R-j. The footer replaces the three "press any key" notices (owner, 3).** Today a pane waits for a key
  only after an early exit (`TerminalSession.cpp:1739-1750`), a lost daemon connection (`:1720-1734`) or a
  start failure (`:708-728`); every other exit closes the pane, and still does. Those three stop writing into
  the grid and set the session's end instead. Return restarts (same profile, page size, launched command and
  spawn cwd, in the same leaf); the `ClosePane` binding and the footer's Close button close; any other key is
  swallowed. A remote-hosted or Disconnected pane offers no Restart (the daemon owns it; 1c D2: no dead
  buttons). Restart needs a model call §3.6 does not name: `SessionModel::replaceLeafSession`.
- **R-k. The reconnecting pill is 1a's `Toast`.** Its tokens (36 / 18) stand, not the `daemon` board's 32 /
  16, and it reads "Reconnecting to %1…" with `win.hostLabel` (1c). "attempt 3" has no source. The pill sits
  over the dimmed, frozen grid: a pane-state indicator, as the board draws it.
- **R-k2. The dim token.** `paneDimColor` (`UiStyleProvider.cpp:287-290`, black at a fixed 60 %) becomes the
  kit's `pane.dim`: black at `dim.darkPercent` on a dark chrome, `dim.lightPercent` on a light one,
  `contrast.paneDimPercent` (5) under Increased Contrast. `ChromeTokens::PaneDimPercent` (`UiStyle.hpp:270`)
  and the unused `paneDimTextColor` go.
- **R-l. Header contents.** Name = the foreground process name, else the session title (`paneHeaderName`).
  No icon (1c D4: no icon source). Trailing status = the pane's badge (`chromeStyle.badgeLook`, 1c) when not
  `None`. Action glyphs are text tokens; SF Symbols are later (1c D4).
- **R-m. Colours.** New roles resolve as 1c's do; pane roles are resolved against the card's own background
  by `colorOn(role, background)`, so a light-profile pane under a dark chrome stays legible. The gap fill is
  the chrome scheme's background moved 4 L points darker (`mac-dark` `#101216` under `#1C1E24`), the same
  ±4 convention as 1d's tint. The canvas's 8 % footer fill and 28 % grabber are not in the kit's alpha
  tables; the nearest steps `fill(3)` and `fill(1)` are used.
- **R-n. Snapshots.** New `SnapshotScripts` rows (Task 12): `split-minimal`, `split-header-hover`,
  `pane-exited`, `single-tab-minimal`. `split-three` and `remote-reconnecting` change picture.

### Non-goals

- Per-process header icons and SF Symbols (1c D4); the header buttons' shortcut tooltips.
- The find bar in the TUI header row (3b), the sticky header and link pill in the header (3e, 3f).
- Reconnect from the footer (Moments 6.3, once a reconnect API exists).
- Pane zoom and split-appear animations (`motion.emphasized`): later, with Reduce Motion fallbacks.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/ChromeDensity.hpp` (create) | `ChromeDensity`, its spellings, `chromeDensityName` | 1 |
| `src/contour/window/PaneHeaderPolicy.hpp` (create) | `resolvePaneHeader`, `DensityHeaderRules`, `PaneHeaderActions`, `paneHeaderName` | 1 |
| `src/contour/window/HeldModifierReveal.hpp` (create) | The ⌥-hold state machine | 1 |
| `src/contour/config/Config.hpp`, `Config.cpp`, `GlobalOverrides.cpp`, `ConfigDocumentation.hpp` (modify) | `chrome_density` | 2 |
| `src/contour/window/SettingsController.cpp` (modify) | The Density row | 2 |
| `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp` (modify) | Minimal sub-rows, new tokens, roles, glyphs | 3 |
| `src/contour/window/UiStyleProvider.{hpp,cpp}`, `ChromeTokenObjects.{hpp,cpp}`, `ContourGuiApp.cpp`, `test/QmlChromeStyle.hpp` (modify) | Density input, invokables, QML groups | 3 |
| `src/contour/session/TerminalSession.{hpp,cpp}`, `TerminalSessionManager.{hpp,cpp}`, `PaneProxy.{hpp,cpp}`, `window/WindowController.{hpp,cpp}` (modify) | Per-pane properties, focus, visible pane count | 4, 5, 10 |
| `src/vtpty/Pty.hpp`, `Process.hpp`, `MockPty.{hpp,cpp}` (modify) | `foregroundProcessName()` through the PTY | 5 |
| `src/vtworkspace/PaneLayout.hpp` (modify), `src/contour/window/SplitChrome.hpp` (create) | The solver's `SplitChrome` | 6 |
| `src/contour/window/TitleBandLayout.hpp`, `TitleBandSurface.hpp` (modify, 1c/1d's) | Minimal's bare band | 7 |
| `src/contour/qml/PaneNode.qml` (modify) | The gap, hit area, grabber, Terminal-row lines | 8 |
| `src/contour/qml/PaneCard.qml`, `PaneHeader.qml` (create); `TerminalPane.qml` (modify) | Cards and headers | 9 |
| `src/contour/window/PaneState.hpp` (create); `src/vtworkspace/Pane.hpp`, `SessionModel.{hpp,cpp}`, `ModelEvents.hpp` (modify) | Session end, footer rows, restart | 10 |
| `src/contour/qml/ExitedFooter.qml` (create) | The footer; the reconnecting pill in `PaneCard.qml` | 11 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp`, `QmlMocks.hpp`, `MockMainController.hpp` (modify) | Mocks and snapshot rows | 4, 9, 11, 12 |

---
### Task 1: `PaneHeaderPolicy.hpp`, `HeldModifierReveal.hpp` and `ChromeDensity`

**Files:**
- Create: `src/contour/config/ChromeDensity.hpp`, `src/contour/window/PaneHeaderPolicy.hpp`,
  `src/contour/window/HeldModifierReveal.hpp`.
- Test: `src/contour/window/PaneHeaderPolicy_test.cpp`, `src/contour/window/HeldModifierReveal_test.cpp`, added to
  `contour_test` beside 1c's `window/TabBadge_test.cpp` (`src/contour/CMakeLists.txt`, the `contour_test` source list).

**Interfaces:**
- Consumes: `config::ConfigEnumInfo` (`ConfigEnumInfo.hpp`), `actions::fromString` and `actions::name`
  (`Actions.hpp:423`, `:953`).
- Produces (`config/ChromeDensity.hpp`, namespace `contour::config`):
  - `enum class ChromeDensity : uint8_t { Comfortable = 0, Minimal };` and
    `inline constexpr size_t ChromeDensityCount` derived from `Minimal`.
  - `detail::ChromeDensityTable` of `ConfigEnumInfo<ChromeDensity>`: `{ Comfortable, "comfortable", "Comfortable" }`,
    `{ Minimal, "minimal", "Minimal" }`, and the `configEnumValues<ChromeDensity>()` specialization (the
    `DefaultsEdition.hpp:31-43` shape).
  - `[[nodiscard]] constexpr std::string_view chromeDensityName(ChromeDensity) noexcept;` → `"comfortable"` / `"minimal"`.
  - `enum class PaneActionsReveal : uint8_t { OnHover = 0, OnHoverOrFocus };` (a layout token's type, R-d).
- Produces (`window/PaneHeaderPolicy.hpp`, namespace `contour::window`):
  - `enum class HeaderReveal : uint8_t { None = 0, HeldModifier };`
  - `enum class PointerPresence : uint8_t { Away = 0, Hovering };`
  - `enum class PaneFocus : uint8_t { Unfocused = 0, Focused, KeyboardFocused };`
  - `config::PaneActionsReveal` (Task 1's, in `config/ChromeDensity.hpp`, so the token rows of Task 3 can name it).
  - `enum class PaneHeaderContent : uint8_t { Hidden = 0, Name, NameAndCwd };`
  - `enum class HeaderActions : uint8_t { Hidden = 0, Shown };`
  - `enum class HeaderPlacement : uint8_t { Inline = 0, Overlay };`
  - `enum class HeaderEmphasis : uint8_t { Secondary = 0, Primary };`
  - `inline constexpr double CwdMinPaneWidth = 360.0;`
  - `struct PaneHeaderInputs { config::ChromeDensity density = config::ChromeDensity::Comfortable; HeaderReveal reveal = HeaderReveal::None; PointerPresence pointer = PointerPresence::Away; PaneFocus focus = PaneFocus::Unfocused; config::PaneActionsReveal actionsReveal = config::PaneActionsReveal::OnHover; int visiblePanes = 1; double width = 0; double height = 0; double headerHeight = 0; double minimumContentHeight = 0; };`
  - `struct PaneHeader { PaneHeaderContent content = PaneHeaderContent::Hidden; HeaderActions actions = HeaderActions::Hidden; HeaderPlacement placement = HeaderPlacement::Inline; HeaderEmphasis emphasis = HeaderEmphasis::Secondary; bool operator==(PaneHeader const&) const = default; };`
  - `struct DensityHeaderRule { config::ChromeDensity density; HeaderReveal needs; HeaderPlacement placement; };`
    and `inline constexpr std::array<DensityHeaderRule, config::ChromeDensityCount> DensityHeaderRules`:
    `{ Comfortable, None, Inline }`, `{ Minimal, HeldModifier, Overlay }` (a rule's `needs == None` means any).
  - `[[nodiscard]] constexpr PaneHeader resolvePaneHeader(PaneHeaderInputs const& inputs) noexcept;`, in order:
    1. `visiblePanes < 2` → `PaneHeader {}`;
    2. the density's rule; its `needs` not met → `{}`;
    3. `Inline` and `height < headerHeight + minimumContentHeight` → `{}`;
    4. `content = width >= CwdMinPaneWidth ? NameAndCwd : Name`; `emphasis = focus != Unfocused ? Primary : Secondary`;
    5. `actions = Shown` when `pointer == Hovering`, or `focus == KeyboardFocused`, or
       `actionsReveal == OnHoverOrFocus && focus != Unfocused`; else `Hidden`.
  - `enum class PaneHeaderAction : uint8_t { SplitRight = 0, SplitDown, Zoom, Close };`
  - `struct PaneHeaderActionRow { PaneHeaderAction action; std::string_view commandName; std::string_view accessibleName; };`
    and `inline constexpr auto PaneHeaderActions = std::array { … }`, in enumerator order:
    `{ SplitRight, "SplitVertical", "Split right" }`, `{ SplitDown, "SplitHorizontal", "Split down" }`,
    `{ Zoom, "TogglePaneZoom", "Zoom pane" }`, `{ Close, "ClosePane", "Close pane" }` (`mac-dark` aria labels).
  - `[[nodiscard]] inline std::string paneHeaderName(std::string_view processName, std::string_view title);`
    → `processName` unless empty, else `title`.
  - Name functions for QML, each covering every enumerator: `paneHeaderContentName` (`"hidden"`, `"name"`,
    `"nameAndCwd"`), `headerActionsName` (`"hidden"`, `"shown"`), `headerPlacementName` (`"inline"`,
    `"overlay"`), `headerEmphasisName` (`"secondary"`, `"primary"`).
- Produces (`window/HeldModifierReveal.hpp`):
  - `class HeldModifierReveal` with `explicit constexpr HeldModifierReveal(std::chrono::milliseconds delay) noexcept;`,
    `void modifierPressed(std::chrono::steady_clock::time_point now) noexcept;` (only a lone press arms it),
    `void otherInput() noexcept;` (disarms until the next press), `void modifierReleased() noexcept;`,
    `[[nodiscard]] HeaderReveal at(std::chrono::steady_clock::time_point now) const noexcept;` (`HeldModifier`
    once armed for at least the delay), `[[nodiscard]] std::optional<std::chrono::steady_clock::time_point> dueAt() const noexcept;`
    (when `at()` will next change; the caller arms its timer for it). Not thread-safe; GUI thread.

- [ ] **Step 1: Write the failing tests.**
  - `PaneHeaderPolicy_test.cpp` (`[paneheader]`), with `base = { .visiblePanes = 2, .width = 400, .height = 300, .headerHeight = 22, .minimumContentHeight = 17 }`:
    - `paneHeader.singlePaneNeverHasAHeader` (Review Focus 5): for both densities, every reveal, pointer and
      focus (`iota` over each enum), `visiblePanes = 1` gives `PaneHeader {}`.
    - `paneHeader.comfortableSplit`: `base` → `{ NameAndCwd, Hidden, Inline, Secondary }`; with `.focus = Focused`
      → emphasis `Primary`; with `.width = 359.5` → `Name`; `.width = 360` → `NameAndCwd`.
    - `paneHeader.actionsOnHover`: `.pointer = Hovering` → `Shown`; `.focus = KeyboardFocused` → `Shown`;
      `.focus = Focused` → `Hidden`; `.focus = Focused, .actionsReveal = OnHoverOrFocus` → `Shown`.
    - `paneHeader.minimalNeedsTheHeldModifier`: `.density = Minimal` → `{}`; with `.reveal = HeldModifier` →
      placement `Overlay`, content `NameAndCwd`.
    - `paneHeader.tooShortHasNoHeader` (Review Focus 1): `.height = 38.9` → `{}`; `.height = 39` → shown;
      Minimal with `HeldModifier` and `.height = 10` → still shown (an overlay takes no room).
    - `paneHeader.rulesCoverEveryDensity`: `DensityHeaderRules[i].density == ChromeDensity(i)` over `iota`.
    - `paneHeader.actionsResolve`: every row of `PaneHeaderActions` is in enumerator order, and
      `actions::fromString(std::string(row.commandName))` has a value whose `actions::name` is `row.commandName`.
    - `paneHeader.name`: `paneHeaderName("vim", "zsh") == "vim"`, `paneHeaderName("", "zsh") == "zsh"`.
    - `paneHeader.names`: every name function covers every enumerator; `chromeDensityName(Minimal) == "minimal"`,
      and `configEnumValues<ChromeDensity>()` lists both, in order.
  - `HeldModifierReveal_test.cpp` (`[heldmodifier]`), delay 500 ms, `t0` any time point:
    - pressed at `t0`: `at(t0 + 499ms) == None`, `at(t0 + 500ms) == HeldModifier`, `dueAt() == t0 + 500ms`;
    - `otherInput()` before the delay: `at(t0 + 600ms) == None` and `dueAt() == nullopt`;
    - `modifierReleased()` after the reveal: `None`;
    - a second press re-arms from its own time.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `window/PaneHeaderPolicy.hpp` not found.
- [ ] **Step 3: Implement** the three headers as the Interfaces block states. The density step is a lookup in
  `DensityHeaderRules`; `static_assert` that it and `PaneHeaderActions` are in enumerator order.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[paneheader],[heldmodifier]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeDensity.hpp src/contour/window/PaneHeaderPolicy.hpp src/contour/window/PaneHeaderPolicy_test.cpp src/contour/window/HeldModifierReveal.hpp src/contour/window/HeldModifierReveal_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/config/ChromeDensity.hpp src/contour/window/PaneHeaderPolicy.hpp src/contour/window/PaneHeaderPolicy_test.cpp src/contour/window/HeldModifierReveal.hpp src/contour/window/HeldModifierReveal_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: decide a pane header and the held-modifier reveal from tables" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The `chrome_density` key

**Files:**
- Modify: `src/contour/config/Config.hpp`: `EditionDefaults` (`:1035-1056`) gains `ChromeDensity chromeDensity;`
  after `theme`; both rows of `EditionDefaultsTable` (`:1085-1121`) name it; a global entry after
  `windowControlStyle` (`:1814-1816`).
- Modify: `src/contour/config/Config.cpp:1601` (load), `src/contour/config/GlobalOverrides.cpp:63` (the
  `settings.yml` row), `src/contour/config/ConfigDocumentation.hpp` (a `ChromeDensityConfig` beside
  `WindowControlStyleConfig` at `:306-318`, a `ChromeDensityWeb` beside `:2477`, and the `using` beside `:3404`).
- Modify: `src/contour/window/SettingsController.cpp:1653-1658`: a row after `window_control_style`.
- Test: `src/contour/config/Config_test.cpp` (beside "Config: window_control_style parses each value", `:3650`),
  `src/contour/config/GlobalOverrides_test.cpp`, `src/contour/window/SettingsController_test.cpp`.

**Interfaces:**
- Consumes: Task 1 (`ChromeDensity`, its table).
- Produces:
  - `ConfigEntry<ChromeDensity, documentation::ChromeDensity> chromeDensity { editionDefaults(defaultsEdition).chromeDensity };`
    on `Config`. Both edition rows are `ChromeDensity::Comfortable` (R-c).
  - The generated configuration carries, after `window_control_style`:

    ```
    {comment} How much room the window chrome takes (ignore-case).
    {comment}   comfortable = once a tab is split, pane cards with headers and a 6 pt gap (default).
    {comment}   minimal     = 1 pt split lines and no pane headers; hold Alt (Option) for half a
    {comment}                 second to show them. On macOS a single tab gets a bare 28 pt band.
    {comment} Applied live.
    chrome_density: {}
    ```
  - The Settings row: `globalEnumFieldFromTable<config::ChromeDensity>("chrome_density", "Density", "How much room the window chrome takes: pane cards with headers, or bare split lines.", [](config::Config const& c) { return c.chromeDensity.value(); })`,
    with no restart note (the provider is rebuilt on reload, Task 3).

- [ ] **Step 1: Write the failing tests.**
  - `Config_test.cpp` (`[config]`): "Config: chrome_density parses each value (ignore-case)" — `comfortable`,
    `Minimal`, `MINIMAL` load; `dense` keeps the default and logs a warning, as the `window_control_style` case
    checks. "Config: chrome_density defaults to comfortable in both editions" —
    `editionDefaults(Classic).chromeDensity == editionDefaults(Native2026).chromeDensity == Comfortable`; a
    partial `contour.yml` loaded through the path overload gives `Comfortable`; a `settings.yml` with
    `chrome_density: minimal` overrides it. "the generated config states chrome_density" —
    `defaultConfigString()` contains `"chrome_density: comfortable"` once.
  - `GlobalOverrides_test.cpp`: `chrome_density` is a scalar row covering its own key alone (the `:12` shape).
  - `SettingsController_test.cpp`: the field `chrome_density` exists, offers both options, and writing
    `minimal` round-trips through `settings.yml`, as the `window_control_style` field's case does.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `Config` has no member `chromeDensity`.
- [ ] **Step 3: Implement** the entry, the column, the load, the override row, the documentation and the
  Settings row.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[config]" && out/appleclang-debug/src/contour/contour_gui_test "[settings]"`

  Expected: PASS. The existing documentation round-trip cases pass with the new key.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/GlobalOverrides.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/config/GlobalOverrides_test.cpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
git add src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/GlobalOverrides.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/config/GlobalOverrides_test.cpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
git commit -m "config: add chrome_density, comfortable or minimal" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: The density sub-row, the pane tokens and colours, and the provider's answers

> **Depends on Steps 4a, 1c and 1d:** 4a appended the four `settings*` fields to `SizeTokens`; 1c added
> `ColorSourceKind::{Fill, Label, Surface, SchemeOnBar}`, `ChromeColorSource::level`, the `detail::fill/label/schemeOnBar`
> factories, `ChromeColorRole` up to `ChipTextStrong`, `MotionStyle`, `badgeLook` and the glyph tokens beside
> `tabSeparator`; 1d added `BandFill` and `bandTintOf`. Add this task's fields after theirs, by name.

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp`:
  - `SizeTokens` (`:114-131`) gains `bareBand`, `grabber`, `grabberLength`, `paneHeaderAction`, `paneFooter`,
    `footerButton`; `SpaceTokens` (`:134-142`) gains `paneGapRows`, `paneHeaderGap`, `footerLeading`, and the
    `paneInset` comment is corrected (R-h); `TypeTokens` gains `paneHeader` and `int paneHeaderWeight`;
    `MotionTokens` gains `headerRevealMs`; a new `struct DimTokens { int darkPercent = 0; int lightPercent = 0; };`;
    `LayoutTokens` gains `PaneActionsReveal paneActions = PaneActionsReveal::OnHover;` (Task 1's enum).
  - `struct MinimalDensityTokens { int paneHeader = 0; int paneGap = 0; int splitHit = 0; int paneInset = 0; int paneCardRadius = 0; };`
    and `ChromeTokenRow` gains `DimTokens dim {}` and `MinimalDensityTokens minimal {}`.
  - `ChromeColorRole` gains, after `ChipTextStrong`: `PaneGapFill`, `PaneCardBorder`, `PaneHeaderFill`,
    `PaneHeaderText`, `PaneHeaderTextDimmed`, `PaneSplitLine`, `PaneGrabber`, `PaneFooterFill`, `PaneFooterText`;
    `ChromeColorRoleCount` derived from `PaneFooterText`.
  - `ColorSourceKind` gains `SchemeShade` (the source's `slot` moved by `level` HSL lightness points, clamped)
    and `Separator` (black on a light ground, white on a dark one, at `contrast.separatorPercent`); factories
    `detail::schemeShade(slot, level)` and `detail::separator()`.
  - `BandFill` (1d) gains `Background` (the terminal background exactly), used by Task 7.
  - The four rows get every value of Global Constraints' table; `resolveChromeTokens` gains a density parameter.
- Modify: `src/contour/config/UiStyle.hpp`: `UiStyleTokens` gains `std::array<std::string_view, 4> paneActionGlyphs`
  (`{"⊞","⊟","⤢","✕"}` native, `{"⊞","⊟","Z","×"}` Terminal), `splitLineGlyphs` (`{"",""}` native, `{"│","─"}`
  Terminal, side-by-side then stacked) and `restartGlyph` (`"↩"` everywhere); `ChromeTokens::PaneDimPercent`
  (`:270`) goes (R-k2).
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}` (`ChromeInputs` `:88-97`; `paneDimColor` `.cpp:282-290`),
  `src/contour/window/ChromeTokenObjects.{hpp,cpp}`, `src/contour/ContourGuiApp.cpp` (`chromeInputs`, `:1796-1806`),
  `src/contour/test/QmlChromeStyle.hpp` (`ChromeSetup`, `:28-37`).
- Test: `src/contour/config/ChromeTokenGroups_test.cpp`, `src/contour/config/UiStyle_test.cpp`,
  `src/contour/window/UiStyleProvider_test.cpp`.

**Interfaces:**
- Consumes: Task 1 (`ChromeDensity`, `resolvePaneHeader`, `paneHeaderName`, the name functions); Task 2
  (`Config::chromeDensity`); 1c (`statusInk`, `badgeLook`, `legibleOnBar`).
- Produces (`config`):
  - `[[nodiscard]] constexpr ChromeTokenRow resolveChromeTokens(ChromeStyle row, AccessibilityPreferences preferences, ChromeDensity density = ChromeDensity::Comfortable) noexcept;`
    Under `Minimal` it copies the row's `minimal` sub-row into `size.paneHeader`, `space.paneGap`, `space.paneGapRows`,
    `space.splitHit`, `space.paneInset` and `radii.paneCard`, before the accessibility steps.
  - Minimal sub-rows: MacOS/Fluent/ContourLinux `{ 0, 1, 8, 0, 0 }`; Terminal `{ 0, 1, 1, 0, 0 }`.
  - `layout.paneActions`: `OnHover` on the native rows, `OnHoverOrFocus` on Terminal.
  - Native colours: `PaneGapFill = schemeShade(Background, -4)`, `PaneCardBorder = separator()`,
    `PaneHeaderFill = Fixed` with `alphaPercent = 0`, `PaneHeaderText = label(1)`, `PaneHeaderTextDimmed = label(2)`,
    `PaneSplitLine = separator()`, `PaneGrabber = fill(1)`, `PaneFooterFill = fill(3)`, `PaneFooterText = label(1)`.
    Terminal: `Background`, `Background`, `Bar`, `BarText`, `schemeOnBar(Secondary)`, `Secondary`, `Accent`,
    `Bar`, `BarText`.
- Produces (`window`):
  - `ChromeInputs` gains `config::ChromeDensity density = config::ChromeDensity::Comfortable;`, and
    `ContourGuiApp::chromeInputs()` sets it from `_config.chromeDensity.value()`. `onConfigurationReloaded`
    (`:1884`) already rebuilds the provider.
  - `ChromeSetup` gains `config::ChromeDensity density {}`.
  - QML groups: `chromeStyle.size.bareBand/grabber/grabberLength/paneHeaderAction/paneFooter/footerButton`;
    `chromeStyle.space.paneGapRows` (vertical units: one cell height on the Terminal row) `/paneHeaderGap/footerLeading`;
    `chromeStyle.type.paneHeader`, `paneHeaderWeight`; `chromeStyle.motion.headerReveal`;
    `chromeStyle.layout.paneActions`; `chromeStyle.colors.paneGapFill … paneFooterText`.
  - `UiStyleProvider` gains `Q_PROPERTY(QString density READ densityName CONSTANT)`,
    `Q_PROPERTY(QStringList paneActionGlyphs …)`, `Q_PROPERTY(QStringList splitLineGlyphs …)`,
    `Q_PROPERTY(QString restartGlyph …)`, and these `[[nodiscard]] Q_INVOKABLE`s:
    - `QVariantMap paneHeader(int reveal, int pointer, int focus, int visiblePanes, qreal width, qreal height) const;`
      runs `resolvePaneHeader` with the provider's density, `size.paneHeader`, `cellHeight` as the minimum
      content height and `layout.paneActions`; gives `{ content, actions, placement, emphasis }` as names.
    - `QString paneHeaderName(QString const& processName, QString const& title) const;`
    - `QColor colorOn(int role, QColor const& background) const;` resolves `Fill`, `Label`, `Surface` and
      `Separator` sources by @p background's tone (1d's `toneOf`) instead of the palette window's; every other
      kind as `colors` does. Out-of-range roles give transparent.
    - `qreal hairline(qreal devicePixelRatio) const;` → one device pixel in logical pixels (`1 / dpr`, 1 for dpr ≤ 0).
    - `qreal unfocusedDim(qreal profileDim) const;` → R-g: `max(profileDim, dim of the chrome tone)` under
      Minimal, `profileDim` under Comfortable, then capped at `contrast.paneDimPercent / 100` when set.
  - `paneDimColor` becomes black at `contrast.paneDimPercent.value_or(dark ? dim.darkPercent : dim.lightPercent)`,
    "dark" decided as `statusColor` decides it; `paneDimTextColor` is removed (its one QML reader goes in Task 11).

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`):
    - `chromeTokens.paneTokens`: every Comfortable value of Global Constraints' table, one `CHECK` each per row;
      extend `numbersOf` and `everyRowMatchesTheDesignKit` with the new fields in declaration order.
    - `chromeTokens.minimalSubRow`: `resolveChromeTokens(MacOS, {}, Minimal)` has `size.paneHeader == 0`,
      `space.paneGap == 1`, `space.splitHit == 8`, `space.paneInset == 0`, `radii.paneCard == 0`, and an unchanged
      `size.titleBar == 38`; the Terminal row's Minimal `splitHit == 1`. Comfortable equals the two-argument
      resolution for every row.
    - `chromeTokens.increasedContrastCapsTheDim`: under `{ .contrast = Increased }`, `contrast.paneDimPercent == 5`
      and `activeCardRing == 1.5` in both densities.
    - `chromeTokens.gapShade`: the `SchemeShade` of contour-dark's `#1C1E24` by −4 is darker and within
      ΔL 4 ± 0.5; for every bundled scheme it differs from the background.
  - `UiStyle_test.cpp` (`[uistyle]`): the glyph arrays per row (`paneActionGlyphs[2] == "Z"` on Terminal,
    `"⤢"` on MacOS; `splitLineGlyphs[0] == "│"` on Terminal and empty on MacOS).
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`):
    - "the provider takes the density from its inputs": `density == "minimal"` under `ChromeInputs { .density = Minimal }`;
      `size.paneHeader == 0`; two inputs differing only in density compare unequal.
    - "paneHeader answers the pure policy": Comfortable, `paneHeader(0, 1, 1, 2, 400, 300)` gives
      `{ "nameAndCwd", "shown", "inline", "primary" }`; Minimal, `paneHeader(1, 0, 0, 2, 400, 300)["placement"] == "overlay"`.
    - "colorOn picks the label by the pane's own background" (Also pinned): on a dark chrome,
      `colorOn(PaneHeaderText, #FAFAF8)` is black at 88 % and `colorOn(PaneHeaderText, #1C1E24)` white at 92 %;
      for contour-dark's and contour-light's backgrounds, `PaneHeaderText` and `PaneHeaderTextDimmed` composited
      over the background reach 4.5:1, and `PaneFooterText` over `PaneFooterFill` over the background does too.
    - "the pane dim is the kit's token": `paneDimColor` alpha is 0.15 on a `#1E1E1E` window palette, 0.10 on
      `#F5F5F7`, 0.05 under Increased Contrast.
    - "unfocusedDim floors Minimal and caps Increased Contrast": Comfortable `unfocusedDim(0) == 0`; Minimal on a
      dark chrome `unfocusedDim(0) == 0.15` and `unfocusedDim(0.3) == 0.3`; Increased `unfocusedDim(0.3) == 0.05`.
    - "hairline is one device pixel": `hairline(2) == 0.5`, `hairline(1) == 1`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on the new fields.
- [ ] **Step 3: Implement** the fields, sub-rows, roles, the two colour kinds in `ChromeColorTokens::color`, the
  groups' properties and the invokables. `paneDimColor`'s readers (`TerminalPane.qml:213`) keep working.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[uistyle],[config],[paneheader]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[contour][gui][qml]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS; 1c's `UiStyle.nativeRowsKeepTodaysGeometry` passes unedited.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/config/UiStyle.hpp src/contour/config/UiStyle_test.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/ContourGuiApp.cpp src/contour/test/QmlChromeStyle.hpp
git add src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/config/UiStyle.hpp src/contour/config/UiStyle_test.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/ContourGuiApp.cpp src/contour/test/QmlChromeStyle.hpp
git commit -m "contour: the density sub-row, pane tokens and colours, and the provider's pane answers" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 4: Per-pane working directory and program status; pane focus and the visible pane count

> **Depends on Step 1c:** 1c renames `refreshTabProgramStatusForSession` (`TerminalSessionManager.cpp:1521`) to
> `refreshTabBadgeForSession(SessionId, OutputOrigin)` and maps `BadgeKind` to `TabBadgeKind` through
> `resolveTabBadge`. Locate both by name.

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp` (the `Q_PROPERTY` block `:200-252`, the signals beside
  `reconnectingChanged` `:1303`) and `.cpp`: overrides of `vtbackend::Terminal::Events::workingDirectoryChanged()`
  and `contextChanged(ContextId)` (`Terminal.hpp:595`, `:602`, unhandled today).
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}`: `refreshTabBadgeForSession` also calls the
  session's `refreshPaneProgramStatus()`; the `FocusPane*` handlers record a keyboard activation.
- Modify: `src/contour/session/PaneProxy.{hpp,cpp}` (`:25-103`): `focus`.
- Modify: `src/contour/window/WindowController.{hpp,cpp}`: `visiblePaneCount` beside `activeTabRootPane` (`.hpp:111`, `:449`),
  notified from `rebuildActiveTabPaneProxies` (`:711`) and zoom changes.
- Modify: `src/contour/test/QmlMocks.hpp` (`MockSession` `:22-…`, `MockPaneProxy` `:225-260`),
  `src/contour/test/MockMainController.hpp`, `src/contour/test/MainWindowControllerMock.hpp`: the mirrors.
- Test: `src/contour/session/TerminalSessionManager_test.cpp` (new `[manager][panes]` cases; it is in
  `contour_gui_test_heavy_sources`, `src/contour/CMakeLists.txt:865-893`).

**Interfaces:**
- Consumes: `TerminalSession::displayWorkingDirectory()` (`.hpp:786`), `vtbackend::abbreviateHomePath`
  (`WorkingDirectory.hpp:109`), `TerminalSessionManager::homeDirectory()`, `window::badgeFor` and
  `resolvedRecords` (`ProgramStatusBadge.hpp`), 1c's `resolveTabBadge`, Task 1's `PaneFocus`.
- Produces (`TerminalSession`), all GUI-thread, notifying only on change:
  - `Q_PROPERTY(QString displayWorkingDirectory READ getDisplayWorkingDirectory NOTIFY displayWorkingDirectoryChanged)`:
    the cached `displayWorkingDirectory()`, home-abbreviated. The two terminal events run on the parser thread
    under the terminal lock, so each only posts `refreshDisplayWorkingDirectory()` to the GUI thread
    (`_postTarget->post`, as `extensionFramePublished` does), which recomputes and compares. Also refreshed once
    when the display attaches.
  - `Q_PROPERTY(int programStatus READ getProgramStatus NOTIFY paneProgramStatusChanged)`: this pane's
    `TabBadgeKind` from `badgeFor(resolvedRecords(terminal.programStatus()), TabFocus::Foreground)` through
    `resolveTabBadge` (a visible header is in front, so `Done` stays the tab's), and
    `Q_PROPERTY(QString programStatusText READ getProgramStatusText NOTIFY paneProgramStatusChanged)`.
    `void refreshPaneProgramStatus();` recomputes both under one terminal lock.
  - Signals `displayWorkingDirectoryChanged()` and `paneProgramStatusChanged()` (not `programStatusChanged`,
    which is the `Events` override at `:954`).
- Produces (`PaneProxy`): `Q_PROPERTY(int focus READ focus NOTIFY activeChanged)` giving `PaneFocus`:
  `Unfocused` when not active, `KeyboardFocused` when the tab's last activation came from a `FocusPane*` action,
  else `Focused`. `enum class PaneActivation : uint8_t { Pointer = 0, Keyboard };` is recorded per tab by the
  manager: `PaneProxy::activate()` records `Pointer`, the four `FocusPane*` handlers `Keyboard`.
  `[[nodiscard]] PaneActivation lastPaneActivation(vtworkspace::TabId) const noexcept;` on the manager.
- Produces (`WindowController`): `Q_PROPERTY(int visiblePaneCount READ visiblePaneCount NOTIFY visiblePaneCountChanged)`:
  the leaves under the active tab's `layoutRoot()` (1 while zoomed or unsplit, 0 without a tab).
- Mocks: `MockSession` gains settable `displayWorkingDirectory`, `foregroundProcessName`, `title`,
  `programStatus`, `programStatusText` with their signals; `MockPaneProxy` gains `focus` (settable) and records
  `lastRatio` in `setRatio`; both controller mocks gain `visiblePaneCount` with a setter.

- [ ] **Step 1: Write the failing tests** (`[manager][panes]`; build as the badge cases do: `TestApp`,
  `MockPtySessionFactory`, `ScopedControllerWithOsWindow`; drain with
  `QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall)`).
  - "OSC 7 notifies the pane's working directory once": write `"\033]7;file://localhost/tmp/x\033\\"` to the
    session's terminal (the MockPty stdout path the manager tests use), drain; a `QSignalSpy` on
    `displayWorkingDirectoryChanged` counts 1 and the property is `"/tmp/x"`. The same report again counts no more.
  - "OSC 3008 notifies too": a context start carrying a cwd (the sequence `Terminal_test` uses for
    `contextChanged`) gives one more notification.
  - "the cwd is abbreviated and sanitized" (Review Focus 3): OSC 7 for `<home>/repo` reads `"~/repo"`; a path
    holding U+202E reads without it (`SanitizePurpose::Display` replaced it).
  - "a pane's program status is its own": two panes; OSC 7501 working in pane 0 only. Pane 0's `programStatus ==
    int(TabBadgeKind::Working)`, pane 1's `None`, and the tab's `BadgeRole` is still `Working` (the aggregate stays).
    Clearing it notifies once.
  - "focus says how the pane was reached": after `activate()` on the right proxy, its `focus == Focused`;
    after `FocusPaneLeft`, the left proxy's `focus == KeyboardFocused` and the right one's `Unfocused`.
  - "visiblePaneCount follows splits and zoom": 1 → split → 2 → split → 3 → `TogglePaneZoom` → 1 → again → 3,
    one notification per change.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`

  Expected: compile error, no `getDisplayWorkingDirectory`.
- [ ] **Step 3: Implement** as the Interfaces block states.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[manager],[contour][gui][qml],[contour][multiwindow]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/session/PaneProxy.hpp src/contour/session/PaneProxy.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/test/QmlMocks.hpp src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/session/PaneProxy.hpp src/contour/session/PaneProxy.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/test/QmlMocks.hpp src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp
git commit -m "contour: a pane's working directory and program status as notifying properties; pane focus and the visible pane count" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: The foreground-process poll, for extensions and for visible headers

> **Depends on Step 1c:** 1c adds `TerminalSession::foregroundProcessName()` (a one-shot GUI-thread read for the
> tab-label fallback, via `dynamic_cast<vtpty::Process const*>`). This task routes it through the PTY.

**Files:**
- Modify: `src/vtpty/Pty.hpp` (`:100-161`), `src/vtpty/Process.hpp:160` (the existing method becomes the
  override), `src/vtpty/MockPty.{hpp,cpp}` (`:24-80`).
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}`: the timer leaves `ExtensionState`
  (`.cpp:342-353`) for the session; `armForegroundProcessPoll` (`:1163-1177`), `pollForegroundProcess`
  (`:1243-1261`), `pollsForegroundProcess` (`:1078-1084`), `statusLineExtensionsReplaced` (`:1197-1203`).
- Modify: `src/contour/window/WindowController.{hpp,cpp}`: `refreshHeaderFollowers()`.
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}`, `src/contour/ContourGuiApp.cpp` (`refreshChromeStyle`,
  `:1808-1814`): a density change refreshes every controller's followers.
- Test: `src/vtpty/MockPty_test.cpp` (create; add to `vtpty_test`), `src/contour/session/TerminalSessionManager_test.cpp`,
  `src/contour/session/TerminalSessionExtensions_test.cpp` (unchanged; must pass).

**Interfaces:**
- Produces (`vtpty`): `[[nodiscard]] virtual std::optional<std::string> foregroundProcessName() const { return std::nullopt; }`
  on `Pty`; `Process` overrides it; `MockPty` overrides it and gains
  `void setForegroundProcessName(std::optional<std::string> name);` (runtime state a test flips).
- Produces (`TerminalSession`):
  - `enum class ProcessNameFollower : uint8_t { Extension = 0b01, Header = 0b10 };` — a bitmask: the two
    combine freely. `void startFollowingForegroundProcess(ProcessNameFollower who);` and
    `void stopFollowingForegroundProcess(ProcessNameFollower who);`. The 1-s `QTimer` exists while any follower
    is set; starting the first polls at once, so a header never waits a second for its name.
  - `void pollForegroundProcess();` (public: its timer's slot, and the tests' tick) reads
    `_terminal.device().foregroundProcessName()`; on a change it stores the name, emits
    `foregroundProcessNameChanged()`, and tells the terminal (`foregroundProcessChanged`) only while
    `Extension` follows. `pollsForegroundProcess()` reports the timer.
  - `Q_PROPERTY(QString foregroundProcessName READ getForegroundProcessName NOTIFY foregroundProcessNameChanged)`.
  - 1c's `foregroundProcessName()` reads `_terminal.device().foregroundProcessName().value_or("")`.
  - The extension path calls start/stop with `Extension` where it armed and reset the timer.
- Produces (`WindowController`): `void refreshHeaderFollowers();` and a private `window::HeaderReveal _headerReveal`
  (always `None` until Task 9 drives it). A session follows with `Header` iff it is a
  leaf under the active tab's `layoutRoot()` and `resolvePaneHeader({ .density, .reveal = _headerReveal, .visiblePanes, .width = CwdMinPaneWidth, .height = <large> })`
  is not `Hidden` (geometry is QML's; the rule here is density, reveal and count). Every other session of this
  window stops following with `Header`. Called on active-tab, structure, zoom, density and reveal changes.

- [ ] **Step 1: Write the failing tests.**
  - `MockPty_test.cpp` (`[vtpty][mockpty]`): a new `MockPty` gives `nullopt`; after
    `setForegroundProcessName("vim")` it gives `"vim"`, through a `Pty const&`.
  - `TerminalSessionManager_test.cpp`, `[manager][processname]` (Review Focus 4; the factory's `createdPtys`
    are the panes' `MockPty`s):
    - "a single pane polls nothing": one tab, one pane: `pollsForegroundProcess()` is false.
    - "a split's visible panes poll, and only on change": split; both poll. Set pane 0's name `"vim"` and call
      `pollForegroundProcess()`: one `foregroundProcessNameChanged`, value `"vim"`; poll again: none.
    - "only the active tab's headers poll": four tabs of three panes; exactly the active tab's three sessions poll;
      `activateTab(2)` moves it to tab 2's three, and tab 0's stop.
    - "Minimal polls nothing": with `chrome_density: minimal`, no session of a split polls. (That a held
      modifier makes them poll is Task 9's `[contour][panes][reveal]` case, once the reveal exists.)
    - "closing back to one pane stops the poll".
    - "a remote-hosted pane has no process name": `reportedHosting = Remote`; the name stays empty and nothing
      is emitted.
  - The `[extensions]` cases in `TerminalSessionExtensions_test.cpp` pass unedited (extensions build only with
    `CONTOUR_EXTENSIONS`; run them where it is on).
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtpty_test contour_gui_test`

  Expected: compile error, `MockPty` has no `setForegroundProcessName`.
- [ ] **Step 3: Implement** as stated. The controller reads the density as `_manager.app().config().chromeDensity.value()`.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vtpty/vtpty_test "[vtpty]"
out/appleclang-debug/src/contour/contour_gui_test "[manager],[contour][tablabel],[extensions]"
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtpty/Pty.hpp src/vtpty/Process.hpp src/vtpty/Process_unix.cpp src/vtpty/Process_win32.cpp src/vtpty/MockPty.hpp src/vtpty/MockPty.cpp src/vtpty/MockPty_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/ContourGuiApp.cpp
editorconfig-checker src/vtpty/CMakeLists.txt
git add src/vtpty/Pty.hpp src/vtpty/Process.hpp src/vtpty/Process_unix.cpp src/vtpty/Process_win32.cpp src/vtpty/MockPty.hpp src/vtpty/MockPty.cpp src/vtpty/MockPty_test.cpp src/vtpty/CMakeLists.txt src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/ContourGuiApp.cpp
git commit -m "contour: one foreground-process poll per session, for extensions and visible pane headers" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

  (Stage `Process_unix.cpp`/`Process_win32.cpp` only if the `override` needed a definition change there.)

---

### Task 6: The size solver counts the gap per axis, the header and the inset

**Files:**
- Modify: `src/vtworkspace/PaneLayout.hpp` (`:28-37`, `contentSizeForLeaf` `:47-100`).
- Create: `src/contour/window/SplitChrome.hpp`.
- Modify: `src/contour/window/WindowController.cpp:1826-1835`, `src/contour/ContourGuiApp.{hpp,cpp}`.
- Test: `src/vtworkspace/PaneLayout_test.cpp`, new `src/contour/window/SplitChrome_test.cpp` (in `contour_test`),
  `src/contour/display/DisplayRendering_test.cpp:2223` (the content-driven resize case, `[display]`).

**Interfaces:**
- Produces (`vtworkspace`):
  - `struct SplitChrome { int sideBySideGap = DefaultSplitHandleThickness; int stackedGap = DefaultSplitHandleThickness; int leafHeader = 0; int rootInset = 0; [[nodiscard]] constexpr bool operator==(SplitChrome const&) const noexcept = default; };`
  - `[[nodiscard]] inline LayoutSize contentSizeForLeaf(Pane const& leaf, LayoutSize requiredLeafSize, SplitChrome const& chrome, Pane const& layoutRoot) noexcept;`
    replaces the `int handleThickness` overload (callers: `WindowController.cpp:1830`, `PaneLayout_test.cpp`).
    When `&leaf != &layoutRoot` it first adds `leafHeader` to the height, walks up using `sideBySideGap` on a
    `Vertical` split and `stackedGap` on a `Horizontal` one, then adds `2 · rootInset` to both axes. The identity
    case is unchanged. Every step ceils; the contract stays "never undershoot".
- Produces (`window/SplitChrome.hpp`, Qt-free):
  `[[nodiscard]] constexpr vtworkspace::SplitChrome splitChromeOf(config::ChromeTokenRow const& tokens, double unitWidth, double unitHeight) noexcept;`
  → `{ ceil(paneGap · unitWidth), ceil(paneGapRows · unitHeight), ceil(paneHeader · unitHeight), ceil(paneInset · unitWidth) }`.
- Produces (`ContourGuiApp`): `[[nodiscard]] vtworkspace::SplitChrome splitChrome() const;` — from the
  published provider's resolved tokens and its cell units when `_chromeStyleHost` exists, else from
  `resolveChromeTokens(_chromeStyle, accessibilityPreferences(), _config.chromeDensity.value())` with unit 1
  (headless). The controller's solver call passes it.

- [ ] **Step 1: Write the failing tests.**
  - `PaneLayout_test.cpp` (`[vtworkspace][layout]`), ported to `SplitChrome { 6, 6, 0, 0 }` (the old behaviour),
    plus:
    - "a split leaf adds the header and the root inset": side-by-side at 0.5, `{ 6, 6, 22, 6 }`, required
      `{ 400, 300 }` for the first leaf → `{ 2·400 + 12, 300 + 22 + 12 }`; the second leaf → `{ ceil((400+6)/0.5)+12, … }`.
    - "the stacked gap is the stacked axis's": `{ 8, 17, 17, 0 }` (Terminal row, 8 × 17 cells) on a stacked split
      uses 17, on a side-by-side one 8.
    - "an unsplit or zoomed leaf is the identity": the leaf as its own layout root gives the requirement unchanged
      with any chrome.
    - "a deep tree counts a gap per level and the header and inset once" (Review Focus 1): eight nested
      alternating splits; the result equals a hand-composed fold, and the leaf's solved share is ≥ the requirement
      for every leaf.
  - `SplitChrome_test.cpp` (`[splitchrome]`): MacOS Comfortable `{ 6, 6, 22, 6 }`; Minimal `{ 1, 1, 0, 0 }`;
    Fluent `{ 4, 4, 28, 4 }`; Terminal with units 8 × 17 `{ 8, 17, 17, 0 }`.
  - `DisplayRendering_test.cpp` (`[display]`): the content-driven resize case gains a split section: the window's
    content grows by the gap, the header and twice the inset over the leaf's request.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtworkspace_test contour_test`

  Expected: compile error, no `SplitChrome`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vtworkspace/vtworkspace_test "[vtworkspace][layout]"
out/appleclang-debug/src/contour/contour_test "[splitchrome]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][sizing]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS. The `[display]` case runs on weston in CI.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtworkspace/PaneLayout.hpp src/vtworkspace/PaneLayout_test.cpp src/contour/window/SplitChrome.hpp src/contour/window/SplitChrome_test.cpp src/contour/window/WindowController.cpp src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp src/contour/display/DisplayRendering_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/vtworkspace/PaneLayout.hpp src/vtworkspace/PaneLayout_test.cpp src/contour/window/SplitChrome.hpp src/contour/window/SplitChrome_test.cpp src/contour/window/WindowController.cpp src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp src/contour/display/DisplayRendering_test.cpp src/contour/CMakeLists.txt
git commit -m "vtworkspace: the size solver counts the gap per axis, the pane header and the cards' inset" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 7: Minimal's bare band

> **Depends on Steps 1c and 1d:** `TitleBandInputs`, `TitleBandRules`, `resolveTitleBand` and `BarCentre`
> (1c Task 3); `TitleBandSurfaceInputs`, `TitleBandSurfaceRows`, `resolveTitleBandSurface` (1d Task 2);
> `UiStyleProvider::titleBand` and `bandFill` (1c Task 4, 1d Task 7); `WindowController::refreshTitleBand` and
> `titleBandHeight` (1d Task 6); Main.qml's `band` and `TitleBar.qml`'s `bandTitle` (1c Task 7). Locate each by name.

**Files:**
- Modify: `src/contour/window/TitleBandLayout.hpp`, `src/contour/window/TitleBandSurface.hpp`,
  `src/contour/window/UiStyleProvider.{hpp,cpp}`, `src/contour/window/WindowController.cpp` (`refreshTitleBand`),
  `src/contour/qml/TitleBar.qml`.
- Test: `src/contour/window/TitleBandLayout_test.cpp`, `src/contour/window/TitleBandSurface_test.cpp`,
  `src/contour/test/MainWindowQml_test.cpp`.

**Interfaces:**
- Consumes: Task 3 (`size.bareBand`, `BandFill::Background`, `ChromeInputs::density`).
- Produces:
  - `TitleBandInputs` gains `config::ChromeDensity density = config::ChromeDensity::Comfortable;`.
    `TitleBandRules` gains a density column (any, or `Minimal`). The new row, ahead of 1c's `Pill`/hidden-gate
    rows: `{ Pill, ClientBand, Minimal, gate Hidden, Top }` → `{ Shown, Top, Empty, Hidden }` (R-f). With the gate
    shown, Minimal resolves as Comfortable.
  - `TitleBandSurfaceInputs` gains `BarCentre centre = BarCentre::Empty;` and `config::ChromeDensity density`.
    After the row match and before the full-screen step: `Minimal` with `centre == Empty` on a client band gives
    `{ Client, Background, <the row's growth> }`.
  - `refreshTitleBand` passes `tokens.height = band.centre == Empty ? size.bareBand : size.titleBar` to
    `resolveTitleBandGeometry`. `bandFill(frame, mode)` answers the chrome scheme's `Background` for
    `BandFill::Background`. The provider's `titleBand(…)` passes its own density; QML calls are unchanged.
  - `TitleBar.qml`: `bandTitle` is invisible when `centre === "empty"`.

- [ ] **Step 1: Write the failing tests.**
  - `TitleBandLayout_test.cpp` (`[titleband]`): `titleBand.minimalBareBand` (Review Focus 5):
    `{ Pill, ClientBand, Windowed, Hidden, Top, Minimal }` → `{ Shown, Top, Empty, Hidden }`; the same with the gate
    `Shown` equals the Comfortable answer; every `Flat` row and every `NativeFrame` row gives the same answer in both
    densities (`iota` over `ChromeDensity`).
  - `TitleBandSurface_test.cpp` (`[titlebandsurface]`): `surface.minimalBareBandIsTheBackground`: MacOS, the
    row's material, client band, windowed, `Minimal`, centre `Empty` → backdrop `Client`, fill `Background`;
    centre `Tabs` → the Comfortable row; full screen keeps `Client`.
  - `MainWindowQml_test.cpp` (`[contour][gui][qml][mainwindow][band][density]`):
    - "Minimal with one tab is a bare 28-pt band" (Review Focus 5): `ChromeSetup { .row = MacOS, .density = Minimal }`,
      one tab under `Multiple`: `titleBar.height == chromeStyle.size.bareBand` (28), `bandTitle` and `tabWell`
      invisible, `controller.chromeHeight() == 28`; after `setTabBarShouldShow(true)` the height is 38 with the
      well visible.
    - "Minimal keeps a user's always-shown tab bar": `TabBarVisibility::Always`, one tab: the well is visible at 38.
    - "Minimal leaves Fluent, Linux and the Terminal row as Comfortable has them": `chromeHeight()` and the bar's
      geometry are equal across densities for those three rows with one and with three tabs.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `TitleBandInputs` has no `density`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[titleband],[titlebandsurface],[titlebandgeometry]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][titleband][controller]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS; 1c's §2.1 acceptance test passes unedited.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/TitleBandLayout.hpp src/contour/window/TitleBandLayout_test.cpp src/contour/window/TitleBandSurface.hpp src/contour/window/TitleBandSurface_test.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/window/WindowController.cpp src/contour/test/MainWindowQml_test.cpp
editorconfig-checker src/contour/qml/TitleBar.qml
git add src/contour/window/TitleBandLayout.hpp src/contour/window/TitleBandLayout_test.cpp src/contour/window/TitleBandSurface.hpp src/contour/window/TitleBandSurface_test.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/window/WindowController.cpp src/contour/test/MainWindowQml_test.cpp src/contour/qml/TitleBar.qml
git commit -m "contour: Minimal gives a single macOS tab a bare band in the terminal background" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: The split handle is the gap, with a wider hit area and a grabber

**Files:**
- Modify: `src/contour/qml/PaneNode.qml` (the `handle:` delegate `:99-117`; the root's backdrop and inset).
- Modify: `src/contour/session/TerminalSessionManager.hpp:126`, `:158-164` (`splitHandleThickness` goes),
  `src/contour/test/MockMainController.hpp:63`, `:154-157`, `src/contour/test/QmlComponents_test.cpp:102-120`,
  `src/contour/session/KeyboardRouting_test.cpp:210`, `:248-251` (the mirrors go).
- Test: `src/contour/test/QmlComponents_test.cpp` (new `[contour][gui][qml][split][handle]` cases).

**Interfaces:**
- Consumes: Task 3 (`space.paneGap`, `paneGapRows`, `splitHit`, `paneInset`, `size.grabber`, `grabberLength`,
  `colors.paneGapFill`, `paneSplitLine`, `paneGrabber`, `chromeStyle.splitLineGlyphs`, `density`).
- Produces (QML, `PaneNode.qml`):
  - The handle is an `Item`, `objectName: "splitHandle"`, whose thickness is `space.paneGap` (side by side) or
    `space.paneGapRows` (stacked), snapped to `dprSnap` as today (#2040). It fills with `colors.paneGapFill` under
    Comfortable and `colors.paneSplitLine` under Minimal (`chromeStyle.density`).
  - `containmentMask: hitArea`, where `hitArea` (`objectName: "splitHitArea"`) is centred on the handle and
    `space.splitHit` thick along the split axis (Qt's documented way to widen a `SplitView` handle without
    changing the layout). A `HoverHandler` on it sets `Qt.SplitHCursor` / `Qt.SplitVCursor`.
  - `splitGrabber` (`size.grabber` × `size.grabberLength`, transposed for stacked splits, radius half its
    thickness, `colors.paneGrabber`) is centred and visible while `SplitHandle.hovered || SplitHandle.pressed`;
    its opacity animates over `motion.fast` under `motion.full`, and cross-fades otherwise.
  - Terminal row: `chromeStyle.splitLineGlyphs` is non-empty, so a `Repeater` of `Text` draws `│` per cell row (or
    `─` per cell column) in `colors.paneSplitLine`, in the accent while hovered or pressed; no grabber
    (`size.grabber == 0`).
  - The root `PaneNode` (`inSplit == false`) of a split tree draws a `colors.paneGapFill` backdrop and insets its
    `SplitView` by `space.paneInset`; a leaf root is edge to edge.
- The manager's `splitHandleThickness` property and its three mirrors are removed; nothing else reads them.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][split][handle]`, MacOS row unless stated; a
  side-by-side split of two `MockPaneProxy` leaves in a 400 × 300 window):
  - "the handle is the gap, and its hit area is wider": `splitHandle.width == chromeStyle.space.paneGap` (6),
    `splitHitArea.width == chromeStyle.space.splitHit` (10) and `splitHitArea.x == -2`; stacked:
    `splitHandle.height == paneGapRows`. Minimal: 1 and 8. The second card's x is `paneInset + first.width + 6`
    (the layout ignores the hit area).
  - "the grabber shows on hover": `splitGrabber` invisible; after `QTest::mouseMove` onto the gap's centre,
    visible with size 2 × 32.
  - "a press inside a card's edge drags the divider" (Review Focus 2): press 1 pt left of the gap (inside the
    first card), move 40 pt, release: the `SplitView` was `resizing` and the left proxy's `lastRatio` changed. In
    Minimal, 3 pt inside.
  - "the scrollbar's centre still scrolls" (Review Focus 2): with the left session's scrollbar visible on the
    right (`isScrollbarRight`, `isScrollbarVisible`), a press on `verticalScrollBar`'s centre
    (`SessionChrome.qml:137`) presses the bar and does not resize; in both densities.
  - "the root inset and backdrop appear only when split": a leaf root fills the window; a split root's
    `SplitView` sits at `paneInset` from each edge over a backdrop coloured `colors.paneGapFill`.
  - Terminal row (run under `ContourTui` with `ChromeSetup { .row = Terminal }`): the handle is one cell wide, its
    first glyph is `"│"`, a stacked handle one cell high with `"─"`; no grabber.
  - The `#2040` snapping case keeps passing at device-pixel ratios 1.25 and 1.75, with the token thickness.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][split]"`

  Expected: FAIL, `splitHitArea` not found.
- [ ] **Step 3: Implement** as stated, and remove `splitHandleThickness` with its mirrors.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][split],[contour][gui][qml],[keyboard]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][split]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS. `PaneNode.qml`'s size and colour baselines go down (the handle's palette colours go).
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSessionManager.hpp src/contour/test/MockMainController.hpp src/contour/test/QmlComponents_test.cpp src/contour/session/KeyboardRouting_test.cpp
editorconfig-checker src/contour/qml/PaneNode.qml scripts/qml-size-literal-baseline.txt
git add src/contour/qml/PaneNode.qml src/contour/session/TerminalSessionManager.hpp src/contour/test/MockMainController.hpp src/contour/test/QmlComponents_test.cpp src/contour/session/KeyboardRouting_test.cpp scripts/qml-size-literal-baseline.txt
git commit -m "contour: the split handle is the pane gap, with a wider hit area and a grabber on hover" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: Pane cards and headers, and the held-modifier reveal

> **Depends on Step 1c:** `ToolbarButton.qml` (1c Task 7) and `chromeStyle.badgeLook` (1c Task 4).

**Files:**
- Create: `src/contour/qml/PaneCard.qml`, `src/contour/qml/PaneHeader.qml`; add both to `_qml_files`
  (`src/contour/CMakeLists.txt:383-…`).
- Modify: `src/contour/qml/PaneNode.qml` (the leaf `Loader`, `:55-64`; `inSplit` `:41-43` becomes
  `visiblePanes`), `src/contour/qml/TerminalPane.qml` (`showFocusFrame` `:41-42` and `focusFrame` `:225-235` go;
  the dim at `:194-203` reads `chromeStyle.unfocusedDim`), `src/contour/qml/ToolbarButton.qml` (an `extent`
  property defaulting to `size.toolbarButton`), `src/contour/qml/Main.qml` (passes `win.visiblePaneCount` and
  `win.headerReveal` to the root `PaneNode`).
- Modify: `src/contour/session/PaneProxy.{hpp,cpp}`, `src/contour/session/TerminalSessionManager.{hpp,cpp}`:
  `runHeaderAction`.
- Modify: `src/contour/window/WindowController.{hpp,cpp}`: `headerReveal`, its event filter and timer.
- Modify: `src/contour/test/QmlMocks.hpp` (`MockPaneProxy::runHeaderAction` records), `MockMainController.hpp`,
  `MainWindowControllerMock.hpp` (`headerReveal` with a setter).
- Test: `src/contour/test/QmlComponents_test.cpp` (new `[contour][gui][qml][panecard]`; the focus-frame cases at
  `:3456-3504` and `:6719-6760` are replaced), `src/contour/test/Accessibility_qml_test.cpp`,
  `src/contour/window/MultiWindow_test.cpp` or `TerminalSessionManager_test.cpp` (`[contour][panes][reveal]`).

**Interfaces:**
- Consumes: Task 1 (`HeldModifierReveal`, `PaneHeaderAction`, `PaneHeaderActions`), Task 3 (`paneHeader`,
  `paneHeaderName`, `colorOn`, `hairline`, `unfocusedDim`, the tokens and glyphs), Task 4 (the session properties,
  `PaneProxy.focus`, `visiblePaneCount`), Task 5 (`refreshHeaderFollowers`), Task 8 (the root inset).
- Produces (C++):
  - `PaneProxy`: `Q_INVOKABLE void runHeaderAction(int action);` → the manager's
    `void runPaneHeaderAction(vtworkspace::TabId tab, vtworkspace::PaneId leaf, window::PaneHeaderAction action);`,
    which activates the leaf (`PaneActivation::Pointer`) and runs `actions::fromString(row.commandName)` on its
    session. Out-of-range ints do nothing.
  - `WindowController`: `Q_PROPERTY(int headerReveal READ headerReveal NOTIFY headerRevealChanged)` (a
    `HeaderReveal`). `bindWindow` installs an event filter on the `QQuickWindow` that never consumes: a
    `KeyPress` of `Qt::Key_Alt` with no other modifier calls `modifierPressed(now)` and arms a single-shot
    `QTimer` for `dueAt()`; any other `KeyPress` or a `MouseButtonPress` calls `otherInput()`; a `KeyRelease` of
    `Qt::Key_Alt` calls `modifierReleased()`. On each change it emits and calls `refreshHeaderFollowers()`. The
    delay is `motion.headerRevealMs` of the app's resolved row. A window deactivation counts as a release.
- Produces (QML):
  - `PaneCard.qml`, `objectName: "paneCard"`: `property var node`, `property int visiblePanes`,
    `property int headerReveal`. Its policy is `chromeStyle.paneHeader(headerReveal, hover ? 1 : 0, node.focus,
    visiblePanes, width, height)`. When `visiblePanes ≥ 2`: a card (`objectName: "paneCardSurface"`) of radius
    `radii.paneCard`, fill = `session.backgroundColor`, a `chromeStyle.hairline(Screen.devicePixelRatio)` border
    in `colorOn(PaneCardBorder, fill)`, `elevation.e1` drawn as Toast draws `e2` when `node.active`, and under
    Increased Contrast a `contrast.activeCardRing` ring in the accent when active (`objectName: "activeCardRing"`).
    One pane: none of these, edge to edge. Holds `PaneHeader` (inline: anchored top at `size.paneHeader`;
    overlay: `z` above the terminal, same height, the terminal not moved), then `TerminalPane` filling the rest.
    `opacity: node.active ? 1 : chromeStyle.tui.inactivePaneOpacity` (0.62 on the Terminal row, 1 elsewhere).
  - `PaneHeader.qml`, `objectName: "paneHeader"`: `required property var session`, `required property var node`,
    `property var policy`. Children: `paneHeaderName` (`chromeStyle.paneHeaderName(session.foregroundProcessName,
    session.title)`, weight `type.paneHeaderWeight`, never elided before the cwd), `paneHeaderCwd`
    (`session.displayWorkingDirectory`, `Text.ElideMiddle`, visible when `policy.content === "nameAndCwd"`),
    `paneHeaderStatus` (the `badgeLook(session.programStatus)` glyph in its ink, when not None), and
    `paneHeaderActions`: four `ToolbarButton`s (`extent: size.paneHeaderAction`, glyphs `paneActionGlyphs[i]`,
    `accessibleName` from `PaneHeaderActions` via a provider list, `objectName: "paneHeaderAction" + i`) that call
    `node.runHeaderAction(i)`; their opacity follows `policy.actions` over `motion.fast` (cross-fade under Reduced).
    Text sizes `type.paneHeader`; colours `colorOn(PaneHeaderText|PaneHeaderTextDimmed, session.backgroundColor)`
    by `policy.emphasis`; the cwd always `PaneHeaderTextDimmed`. Overlay fill is the card's background, opaque.
    `Accessible.role: Accessible.StaticText`, `Accessible.name: qsTr("%1, %2").arg(name, full cwd)`.
    Terminal row: one cell row, fill `colors.paneHeaderFill` (the bar), glyphs `⊞ ⊟ Z ×`.
  - Every `TerminalPane` keeps `objectName`s and behaviour; it no longer has `focusFrame`.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][panecard]`, MacOS row, Comfortable, unless stated):
  - "a single pane is edge to edge": `paneHeader` invisible, no `paneCardSurface`, the `TerminalPane` geometry
    equals the window's content.
  - "a split's cards have headers": two leaves, 800 × 400: each `paneHeader.height == 22`; each terminal's
    `y == 22`; `paneHeaderName.text == "vim"` from the mock's `foregroundProcessName`, `"zsh"` (the title) when it
    is empty; `paneHeaderCwd` visible (each card ≥ 360 wide); at 600 wide (≈ 291 each), invisible.
  - "focus is emphasis and dim, never an outline": no item named `focusFrame` exists; the active header's name
    colour is `colorOn(PaneHeaderText, bg)` and the inactive one's `colorOn(PaneHeaderTextDimmed, bg)`; the
    inactive pane's `dimOverlay` starts at the terminal's top (`y ≥` the header's bottom).
  - "header text reads at 4.5:1 on its card" (Also pinned): for contour-dark's and contour-light's backgrounds as
    the session background, under a dark and a light chrome palette, both header colours composited over the
    background reach 4.5:1.
  - "the actions show on hover and act on their pane": `paneHeaderActions` opacity 0; after hovering the header, 1;
    clicking `paneHeaderAction0` records `runHeaderAction(0)` on that pane's proxy.
  - "a long cwd elides in the middle" (Review Focus 3): a 4 096-character cwd: `paneHeaderCwd.elide == Text.ElideMiddle`,
    the header's height is still 22, `paneHeaderCwd.x + paneHeaderCwd.width ≤ paneHeaderActions.x`,
    `paneHeaderName.truncated` is false, and the header's `Accessible.name` contains the whole path.
  - "a deep tree in a small window" (Review Focus 1): eight nested alternating splits in 320 × 200: no QML warning
    (`QmlMessageCapture`), every `paneCard` has non-negative width and height, and every header whose card is
    shorter than `22 + cellHeight` is invisible.
  - "Minimal shows no headers until the modifier is held": `ChromeSetup { .density = Minimal }`: headers invisible
    and terminals at `y == 0`; with `headerReveal = 1`, headers visible as overlays, terminals still at `y == 0`;
    a single pane with `headerReveal = 1` shows none (Review Focus 5).
  - "Increased Contrast rings the active card": `activeCardRing` visible on the active card only, `border.width == 1.5`.
  - "the Terminal row draws a header of one cell row" (under `ContourTui`, `ChromeSetup { .row = Terminal }`): header
    height `== chromeStyle.cellHeight`, action glyphs `"⊞" "⊟" "Z" "×"`, the inactive card's opacity 0.62, actions
    shown on the active pane without hover.
  - `Accessibility_qml_test.cpp`: a kit case for `PaneHeader.qml` (`StaticText`, name `"vim, ~/repo"`) and its
    four buttons (`Button`, names "Split right", "Split down", "Zoom pane", "Close pane").
  - `[contour][panes][reveal]` (a `TestApp` window): `QTest::keyPress(window, Qt::Key_Alt)`, then
    `QTest::qWaitFor` up to 1 s for `headerReveal == 1`; `QTest::keyClick(window, Qt::Key_X)` back to 0;
    pressing Alt and releasing before 500 ms never reveals; the terminal still received the Alt press (the filter
    consumes nothing). Under `chrome_density: minimal` with a split, the reveal makes the active tab's sessions
    `pollsForegroundProcess()`, and the release stops them (Review Focus 4).
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][panecard]"`

  Expected: FAIL, `paneHeader` not found.
- [ ] **Step 3: Implement** the two components, the leaf wiring, the controller's reveal and `runHeaderAction`.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][panecard],[contour][gui][qml][split],[contour][gui][qml][a11y],[contour][panes][reveal],[manager],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][panecard],[contour][gui][qml][split]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS; `PaneCard.qml` and `PaneHeader.qml` at baseline 0.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/PaneProxy.hpp src/contour/session/PaneProxy.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/test/QmlMocks.hpp src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp src/contour/session/TerminalSessionManager_test.cpp
editorconfig-checker src/contour/qml/PaneCard.qml src/contour/qml/PaneHeader.qml src/contour/qml/PaneNode.qml src/contour/qml/TerminalPane.qml src/contour/qml/ToolbarButton.qml src/contour/qml/Main.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt
git add src/contour/qml/PaneCard.qml src/contour/qml/PaneHeader.qml src/contour/qml/PaneNode.qml src/contour/qml/TerminalPane.qml src/contour/qml/ToolbarButton.qml src/contour/qml/Main.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt src/contour/session/PaneProxy.hpp src/contour/session/PaneProxy.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/test/QmlMocks.hpp src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
git commit -m "contour: pane cards with headers once a tab is split, focus by emphasis and dimming, and the held-modifier reveal" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 10: A session's end, and Restart in the same pane

**Files:**
- Create: `src/contour/window/PaneState.hpp`, test `src/contour/window/PaneState_test.cpp` (in `contour_test`).
- Modify: `src/vtworkspace/Pane.hpp` (beside `swapSession`, `:109-116`) and `Pane.cpp`,
  `src/vtworkspace/SessionModel.{hpp,cpp}` (beside `toggleActivePaneZoom`, `.hpp:198`),
  `src/vtworkspace/ModelEvents.hpp` (beside `paneSwapped`, `:129`).
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}`: `_terminatedAndWaitingForKeyPress` (`.hpp:1843`) and its
  readers in `reportDeviceStartFailure` (`.cpp:708-728`), `terminate` (`:780-795`), `onClosed` (`:1686-1785`),
  `sendKeyEvent` (`:2838-2849`) and `sendCharEvent` (`:2901-…`).
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}`: `restartSession`, the `paneSessionReplaced` event,
  and the proxy refresh in the hosting controller.
- Test: `src/vtworkspace/SessionModel_test.cpp`, `src/contour/session/TerminalSession_test.cpp` (the two cases at
  `:3666-3718` are rewritten), `src/contour/session/TerminalSessionManager_test.cpp` (`[manager][paneend]`).

**Interfaces:**
- Produces (`window/PaneState.hpp`, Qt-free):
  - `enum class SessionEnd : uint8_t { None = 0, Exited, Signalled, StartFailed, Disconnected };`
  - `enum class PaneState : uint8_t { Live = 0, Reconnecting, Ended };`
  - `enum class FooterRestart : uint8_t { Unavailable = 0, Offered };`
  - `enum class RestartSupport : uint8_t { Unsupported = 0, Supported };` — the caller's hosting, converted at
    the boundary (`SessionHosting::Local` → `Supported`).
  - `struct PaneFooterRow { SessionEnd end; FooterRestart restart; std::string_view message; std::string_view messageWithDetail; };`
    and `inline constexpr auto PaneFooterRows = std::array { … }` in enumerator order:
    - `None`: `Unavailable`, `""`, `""`;
    - `Exited`: `Offered`, `"Process exited."`, `"Process exited with code {}."` (`moments` 5);
    - `Signalled`: `Offered`, `"Process ended."`, `"Process ended by signal {}."`;
    - `StartFailed`: `Offered`, `"The terminal could not be started."`, `"The terminal could not be started: {}."`;
    - `Disconnected`: `Unavailable`, `"Disconnected from the Contour daemon. Its sessions are still running."`, same.
  - `[[nodiscard]] std::string paneFooterMessage(SessionEnd end, std::string_view detail);` — the detail form when
    `detail` is non-empty.
  - `[[nodiscard]] constexpr FooterRestart restartOf(SessionEnd end, RestartSupport host) noexcept;` — the row's
    offer, `Unavailable` when `host == Unsupported`.
  - `[[nodiscard]] constexpr PaneState paneStateOf(SessionEnd end, vthost::client::ConnectionState connection) noexcept;`
    — `Ended` when `end != None`, `Reconnecting` when the connection is `Reconnecting`, else `Live`.
- Produces (`vtworkspace`): `SessionId Pane::replaceSession(SessionId session) noexcept;` (`@pre isLeaf()`,
  returns the old one); `SessionId SessionModel::replaceLeafSession(TabId tab, PaneId leaf, SessionId session);`
  keeps the `PaneId`, the active pane and zoom, and raises
  `virtual void paneSessionReplaced(TabId tab, PaneId leaf) { (void) tab, (void) leaf; }` on `ModelEvents`.
- Produces (`TerminalSession`):
  - `_terminatedAndWaitingForKeyPress` becomes `window::SessionEnd _end = SessionEnd::None;` plus
    `std::string _endDetail;`, under `_onClosedMutex` as before; "waiting" is `_end != None`.
  - The three notices stop writing into the grid (R-j): the start failure sets `StartFailed` with
    `std::format("{}", failure)`; the transport-lost branch `Disconnected`; the early-exit branch `Exited` with
    the exit code, or `Signalled` with the signal, from `sessionExitStatusOf`. `writeNotice` stays for its other
    callers.
  - `Q_PROPERTY(int sessionEnd READ getSessionEnd NOTIFY sessionEndChanged)`,
    `Q_PROPERTY(QString endMessage READ getEndMessage NOTIFY sessionEndChanged)`,
    `Q_PROPERTY(int restartOffer READ getRestartOffer NOTIFY sessionEndChanged)`; `sessionEndChanged()` is emitted
    once per end, after `_onClosedMutex` is released.
  - `Q_INVOKABLE void restart();` → `_manager->restartSession(*this)` when the offer is `Offered` and a manager
    exists; `Q_INVOKABLE void dismiss();` → the old acknowledge path (`emit sessionClosed(*this)`, then
    `closeDisplay()`).
  - Keys while ended: a `Return`/`Enter` press restarts when offered; every other press goes through the input
    mappings only, so `ClosePane` still closes (through `terminate()`'s already-closed branch, which reads `_end`);
    text never reaches the PTY.
- Produces (`TerminalSessionManager`): `void restartSession(TerminalSession& ended);` — finds the hosting tab and
  leaf, mints the next id (the `_pendingSessionId` pattern `splitActivePane` uses, `.cpp:1852-1860`), calls
  `createBackingSession(id, cwd, ended.terminal().pageSize(), ended.launchedCommand(), ended.profileName())` with
  `cwd` = `ended.resolveWorkingDirectory(CwdPurpose::Spawn)`'s path, else `ended.workingDirectory()`, then
  `_model->replaceLeafSession(…)`, makes the new session the active pane's when it was, and retires `ended`
  without pruning the leaf. **Pre-flight check:** whether `removeSession` (`:1159`) tolerates a session no leaf
  carries; if not, factor its teardown out of the model-pruning half and call that.
- The `paneSessionReplaced` override notifies the leaf's `PaneProxy::changed`, so QML rebinds the pane's session
  (the rebind-through-null path `QmlComponents_test.cpp:3175` covers).

- [ ] **Step 1: Write the failing tests.**
  - `PaneState_test.cpp` (`[panestate]`): rows in enumerator order; `paneFooterMessage(Exited, "1") == "Process exited with code 1."`,
    `(Exited, "") == "Process exited."`; `restartOf(Exited, Supported) == Offered`, `(Exited, Unsupported) == Unavailable`,
    `(Disconnected, Supported) == Unavailable`; `paneStateOf` over every `SessionEnd` × `ConnectionState`.
  - `SessionModel_test.cpp` (`[vtworkspace][model]`): "replacing a leaf's session keeps its pane": the `PaneId`,
    the active leaf and a zoomed leaf survive; the old id is returned; one `paneSessionReplaced` event.
  - `TerminalSession_test.cpp` (`[contour][session][close]`), the two rewritten cases:
    - "a spontaneous early exit ends the session; typing does not close it": `pty.close(); session->onClosed();`
      → `sessionEnd == int(SessionEnd::Exited)`, `sessionClosed` not emitted, nothing new on screen
      (`countOnScreen(…, "Shell terminated too quickly.") == 0`); `sendCharEvent(U'x', …)` emits nothing and
      writes nothing to the PTY's stdin; `dismiss()` emits `sessionClosed`.
    - "a second onClosed() reports the end once": a spy on `sessionEndChanged` counts 1.
    - "a lost daemon connection ends as Disconnected, with no Restart": `noteTransportLost()`, close →
      `Disconnected`, `restartOffer == Unavailable`; a Return press does nothing.
    - "a start failure ends as StartFailed and names the failure".
  - `TerminalSessionManager_test.cpp` (`[manager][paneend]`):
    - "Return restarts the pane in place": a split; end pane 1 early; a Return press → the leaf's `PaneId` is
      unchanged, its session id is new, the factory's last `requestedProfileNames` and `requestedCwds` are the
      ended session's, the old session is gone (`sessionForId(old) == nullptr`), the new one is Live and active.
    - "ClosePane closes an ended pane": the binding's action on the ended session collapses the split.
    - "a remote-hosted pane offers no Restart": `reportedHosting = Remote` → `restartOffer == Unavailable`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test vtworkspace_test contour_gui_test`

  Expected: compile errors on `PaneState.hpp` and `replaceLeafSession`.
- [ ] **Step 3: Implement** as stated.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[panestate]"
out/appleclang-debug/src/vtworkspace/vtworkspace_test "[vtworkspace]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][session],[manager],[contour][multiwindow]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/PaneState.hpp src/contour/window/PaneState_test.cpp src/vtworkspace/Pane.hpp src/vtworkspace/Pane.cpp src/vtworkspace/SessionModel.hpp src/vtworkspace/SessionModel.cpp src/vtworkspace/ModelEvents.hpp src/vtworkspace/SessionModel_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/window/PaneState.hpp src/contour/window/PaneState_test.cpp src/contour/CMakeLists.txt src/vtworkspace/Pane.hpp src/vtworkspace/Pane.cpp src/vtworkspace/SessionModel.hpp src/vtworkspace/SessionModel.cpp src/vtworkspace/ModelEvents.hpp src/vtworkspace/SessionModel_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp src/contour/window/WindowController.cpp
git commit -m "contour: an ended pane waits with a named end instead of a notice in the grid; Return restarts it in place" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```


---

### Task 11: `ExitedFooter.qml` and the reconnecting pill

> **Depends on Step 1c:** `win.hostLabel` and the `chipOpenGlyph`/`chipCloseGlyph` tokens (1c Tasks 4 and 5).

**Files:**
- Create: `src/contour/qml/ExitedFooter.qml`; add it to `_qml_files`.
- Modify: `src/contour/qml/PaneCard.qml` (the footer at the bottom, the terminal above it; the reconnecting dim and
  pill), `src/contour/qml/TerminalPane.qml` (the `reconnectingOverlay` at `:205-223` goes).
- Modify: `src/contour/window/WindowController.{hpp,cpp}`: `Q_INVOKABLE [[nodiscard]] QString shortcutText(QString const& commandName) const;`
  — the chord `command::chordIndex(_manager.app().config())` (1b) holds for that command, in the host's
  `shortcutStyleFor` style; empty without one.
- Modify: `src/contour/test/QmlMocks.hpp` (`MockSession`: `sessionEnd`, `endMessage`, `restartOffer`, `restart()`
  and `dismiss()` counting calls), `MockMainController.hpp` (`shortcutText`), `src/contour/test/ChromeSnapshot_test.cpp:105`
  (the object it finds keeps the name `reconnectingOverlay`).
- Test: `src/contour/test/QmlComponents_test.cpp` (new `[contour][gui][qml][paneend]`; the case at `:3038` is
  rewritten), `src/contour/test/Accessibility_qml_test.cpp`, `src/contour/window/WindowMenus_test.cpp` (`shortcutText`).

**Interfaces:**
- Consumes: Task 3 (`size.paneFooter`, `footerButton`, `space.footerLeading`, `colors.paneFooterFill/Text`,
  `restartGlyph`, `paneDimColor`), Task 10 (the session properties).
- Produces (QML):
  - `ExitedFooter.qml`, `objectName: "exitedFooter"`: `required property var session`; visible while
    `session.sessionEnd !== 0`; height `size.paneFooter`; fill `colorOn(PaneFooterFill, session.backgroundColor)`
    with a top hairline in `PaneCardBorder`; leading `space.footerLeading`, trailing `s5`, gap `s4`.
    `exitedFooterMessage` (`session.endMessage`, `type.tab`), `exitedFooterRestart` (accent fill,
    `onStatusColor(Running)` text, `qsTr("Restart %1").arg(chromeStyle.restartGlyph)`, visible when
    `session.restartOffer === 1`, calls `session.restart()`), `exitedFooterClose` (`fill` level 3,
    `qsTr("Close %1").arg(win.shortcutText("ClosePane"))` trimmed, calls `session.dismiss()`); buttons
    `size.footerButton` high at `radii.control`. `Accessible.role: Accessible.AlertMessage` with the message as
    name; the buttons are `Accessible.Button`. Terminal row: one cell row on the bar, the buttons bracketed with
    `chipOpenGlyph`/`chipCloseGlyph`.
  - `PaneCard.qml`: the terminal's bottom anchors to the footer's top while it shows, so the footer is never over
    cells. While `session.isReconnecting`: a layer `objectName: "reconnectingOverlay"` over the terminal only,
    colour `chromeStyle.paneDimColor`, and a `Toast` (`objectName: "reconnectingPill"`, `shown: true`,
    `text: qsTr("Reconnecting to %1…").arg(win.hostLabel)`, `glyph: chromeStyle.badgeLook(3).glyph`)
    centred on it. The pill never animates its glyph.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][paneend]`):
  - "an ended pane shows the footer below its cells": `MockSession.sessionEnd = Exited`, `endMessage = "Process exited
    with code 1."`, `restartOffer = 1`: `exitedFooter` visible, height 34 on MacOS; the terminal's bottom equals the
    footer's top; `exitedFooterMessage.text` is the message; clicking `exitedFooterRestart` counts one `restart()`;
    `exitedFooterClose`'s text starts with "Close" and a click counts one `dismiss()`.
  - "a Disconnected pane offers no Restart": `restartOffer = 0` → `exitedFooterRestart` invisible.
  - "the footer reads at 4.5:1": message and button texts over their fills over contour-dark's and contour-light's
    backgrounds.
  - "a reconnecting pane dims below its header and shows the pill": in a split, `isReconnecting = true`:
    `reconnectingOverlay` visible with `color == chromeStyle.paneDimColor`, its `y` at the header's bottom;
    `reconnectingPill` visible, text `"Reconnecting to home…"` with the mock's `hostLabel = "home"`; false hides
    both. Under Increased Contrast the dim's alpha is 0.05.
  - Terminal row (under `ContourTui`): the footer is `cellHeight` high and the Restart button's text is
    bracketed.
  - `Accessibility_qml_test.cpp`: `ExitedFooter.qml` is an `AlertMessage` named by the message, with two buttons.
  - `WindowMenus_test.cpp`: `shortcutText("ClosePane")` is the Classic default's chord in the host's style, and
    `shortcutText("NoSuchCommand")` is empty.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][paneend]"`

  Expected: FAIL, `exitedFooter` not found.
- [ ] **Step 3: Implement** as stated; remove `TerminalPane.qml`'s overlay and the provider's `paneDimTextColor`.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][paneend],[contour][gui][qml][panecard],[contour][gui][qml][a11y],[windowmenus],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][paneend]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS; `TerminalPane.qml`'s baselines go down, `ExitedFooter.qml` is at 0.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/test/QmlMocks.hpp src/contour/test/MockMainController.hpp src/contour/test/ChromeSnapshot_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp src/contour/window/WindowMenus_test.cpp
editorconfig-checker src/contour/qml/ExitedFooter.qml src/contour/qml/PaneCard.qml src/contour/qml/TerminalPane.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt
git add src/contour/qml/ExitedFooter.qml src/contour/qml/PaneCard.qml src/contour/qml/TerminalPane.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt src/contour/window/WindowController.hpp src/contour/window/WindowController.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/test/QmlMocks.hpp src/contour/test/MockMainController.hpp src/contour/test/ChromeSnapshot_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp src/contour/window/WindowMenus_test.cpp
git commit -m "contour: an ended pane shows a footer with Restart and Close; a reconnecting pane dims and shows a pill" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---
### Task 12: Snapshot rows, and the Terminal row's pass

> **Depends on Steps 4a, 1c and 1d:** 4a's `SnapshotSurface` replaced `ContentPage`; 1c's rows name `.drift`,
> `.frame` and `.mode`, and `HostConnection` gained `Connected`/`Disconnected`; 1d added `band-solid`. Every
> existing row names this task's new fields too.

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` (`SnapshotScript` `:28-42`, `ScriptAction` `:57-65`,
  `SnapshotScriptRow` `:86-98`, `SnapshotScripts`), `src/contour/test/ChromeSnapshotScene.hpp` (the scene builds
  `ChromeSetup.density`, the mock session's end and the hover).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Produces:
  - `SnapshotScriptRow` gains `config::ChromeDensity density = config::ChromeDensity::Comfortable;` and
    `SnapshotPaneEnd paneEnd = SnapshotPaneEnd::None;` with `enum class SnapshotPaneEnd : uint8_t { None = 0, Exited };`;
    `ScriptAction` gains `HoverPaneHeader` (the pointer over the active pane's header and then the gap, so the
    actions and the grabber show).
  - New rows, appended in this order: `SplitMinimal` (`"split-minimal"`, `PaneShape::SplitThree`, `Minimal`),
    `SplitHeaderHover` (`"split-header-hover"`, `SplitThree`, `HoverPaneHeader`), `PaneExited` (`"pane-exited"`,
    `SplitThree`, `paneEnd = Exited`, message `"Process exited with code 1."`), `SingleTabMinimal`
    (`"single-tab-minimal"`, one tab, `Minimal`).
  - `split-three` (cards and headers) and `remote-reconnecting` (the pill) change picture; their rows are unchanged.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): the four rows' fields, e.g.
    `scriptRow(SnapshotScript::SplitMinimal).density == ChromeDensity::Minimal`,
    `scriptRow(SnapshotScript::PaneExited).paneEnd == SnapshotPaneEnd::Exited`; every row names a density; the
    enumerator-order `static_assert` holds; counts come from `SnapshotScripts.size()`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): every new script builds on the MacOS and Terminal
    rows without a QML warning; `split-header-hover` finds `paneHeaderActions` at opacity 1 and `splitGrabber`
    visible; `pane-exited` finds `exitedFooter`; `single-tab-minimal` on MacOS finds `titleBar.height == 28`;
    `remote-reconnecting` finds `reconnectingPill`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, no `SnapshotScript::SplitMinimal`.
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
git commit -m "contour: snapshot the pane cards, Minimal, the header on hover and an ended pane" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, so the plan's
  own commit is left out. Apply its findings, re-run the filters of every task it touches, and commit once with the
  Signed-off-by line, staging only the files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[paneheader],[heldmodifier],[config],[chrometokens],[uistyle],[splitchrome],[titleband],[titlebandsurface],[titlebandgeometry],[panestate],[snapshot]"
out/appleclang-debug/src/vtworkspace/vtworkspace_test "[vtworkspace]"
out/appleclang-debug/src/vtpty/vtpty_test "[vtpty]"
out/appleclang-debug/src/contour/contour_gui_test "[settings],[contour][uistyle],[manager],[contour][session],[contour][tablabel],[extensions],[contour][sizing],[contour][multiwindow],[keyboard],[windowmenus],[contour][panes][reveal],[contour][titleband][controller],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][split],[contour][gui][qml][panecard],[contour][gui][qml][paneend],[contour][gui][qml][mainwindow],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L daemon --output-on-failure
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots, on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot],[display]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, with the size read from
  `ChromeSnapshotPlan.hpp`. Look at `split-three.*` (cards, headers, the 6-pt gap on the darker backdrop, the dimmed
  inactive panes under undimmed headers), `split-minimal.*` (1-pt lines, no headers), `split-header-hover.*`,
  `pane-exited.*`, `single-tab-minimal.macos.*` (a 28-pt band in the terminal background) and
  `remote-reconnecting.*` (the pill); on the Terminal row, `│`/`─` lines and one-row headers. `three-tabs.*` and
  `single-tab.*` on every row look as they did after 1d.
- [ ] **Step 4: Owner manual checks.** Batched per phase (ruling): list them in the ledger for the owner; do not wait
  on them. On this Mac, `out/appleclang-debug/src/contour/contour.app`:
  - ⌘D (or the default split binding): two cards with headers appear; one pane again is edge to edge.
  - The header shows `nvim` and its cwd while `nvim` runs, and the cwd follows `cd` within a second.
  - Hover a header: split right, split down, zoom and close fade in and act on that pane.
  - Drag the gap from its edge and from 2 pt inside a card; the grabber shows on hover; the scrollbar still scrolls.
  - `chrome_density: minimal` (Settings › Density, applied live): 1-pt lines, no headers; hold ⌥ half a second and the
    headers overlay, then vanish on release; a single tab has a bare 28-pt band.
  - Increased Contrast on: the active card's accent ring; the dim lighter.
  - Reduce Motion on: the actions and the grabber cross-fade.
  - `exit 1` right after opening a split pane (within `early_exit_threshold`): the footer "Process exited with code 1."
    with Restart and Close; Return restarts in the same pane and directory; ⌘W closes.
  - `contour client --host`, kill the daemon connection: the panes dim with "Reconnecting to …"; when the supervisor
    gives up, the footer says Disconnected with Close only.
  - `ui_style: terminal`: one-row headers with `⊞ ⊟ Z ×`, `│`/`─` lines, inactive panes at 0.62.
  - Twelve panes over four tabs: Activity Monitor shows no extra CPU over one tab's worth.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Adds pane cards with headers once a tab is split: the program, its working directory, its status, and split,
    zoom and close buttons on hover. A single pane stays edge to edge."
  - "Shows the focused pane by dimming the others instead of an outline."
  - "Makes the gap between panes the divider, with a wider grab area and a grabber on hover."
  - "Adds `chrome_density: comfortable | minimal`; Minimal draws 1-pt split lines without headers (hold Option or Alt
    to show them) and a bare band for a single tab on macOS."
  - "Replaces the 'press any key' notice of a pane whose shell ended early, failed to start or lost the daemon with a
    footer offering Restart and Close; Return restarts the pane in place."
  - "Shows a reconnecting pane as dimmed with a 'Reconnecting to …' pill."
- [ ] **Step 6:** Mark Step 2 done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-2.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** one 1-s poll per session with a visible header (none for a single pane, Minimal or background
    tabs); per-pane properties notify only on change; the working directory is recomputed on OSC 7 / 3008 only.
  - **Risk:** medium. The pane tree's QML is restructured (cards wrap every leaf); the handle's hit area overlaps the
    cards; the three wait-for-key exits now wait on a footer and Return restarts in place; `Pty` gains a virtual.
  - **Coverage:** the four pure tables, the `[manager][panes]`, `[manager][processname]` and `[manager][paneend]`
    mock-PTY cases, the solver's `[vtworkspace][layout]` cases, the `[split][handle]`, `[panecard]` and `[paneend]`
    property tests on both the native and the Terminal row, and four new snapshot scripts.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Pane cards on every row now (R-b).** `win` and `linux` draw their cards, and §3.6 does not limit panes to macOS.
   *Default: every row, with its canvas values.* The alternative keeps Fluent and ContourLinux on today's handle until
   5a / 5b, through a layout token.
2. **`chrome_density` defaults to `comfortable` in both editions (R-c).** Classic users' splits gain cards and headers,
   as 1c's pills reached them ungated. *Default: Comfortable for both.* The alternative is Classic = Minimal, closer to
   today's thin divider.
3. **Which exits show the footer (R-j).** *Default: the three that wait for a key today (early exit, start failure,
   lost daemon); a normal exit still closes the pane; Return restarts, ⌘W closes, other keys are swallowed.* The
   alternative keeps every ended pane open with the footer, which changes how `exit` behaves for everyone.
4. **Minimal's ⌥-hold shows the headers as a transient overlay of each pane's top row (R-e)**, as 1c D3's full-screen
   band does. *Default: overlay, no reflow.* The canvas's "in the band" has no layout for several panes.
5. **Focus stays visible in Minimal (R-g).** *Default: under Minimal the unfocused dim is at least the kit's pane dim
   (15 % / 10 %), even with `dim_unfocused: 0`.* The alternative honours 0 and leaves Minimal with no focus cue.

## Spec gaps found

- §3.6's code citations are stale (current lines in the Spec paragraph above); the brief's "header 26, dim 18 %"
  contradicts the canvas (22, 15 % / 10 %), which is used.
- The header type size: `kit-tokens` says 12 medium on macOS, `kit-components` and `mac-dark` say footnote 11; 11 is
  used. Windows and Linux use 12 and 12.5 semibold.
- `space.paneInset` is documented in code as the content's inset inside its card; the canvas uses it as the cards'
  inset from the content edge (R-h).
- The header policy needs inputs §3.6 does not list: the pane's height (a deep tree in a small window) and when actions
  reveal on the Terminal row (R-d).
- Minimal's "⌥-hold shows them in the band" has no layout for several panes (R-e), and Minimal with one tab and no
  headers leaves no focus cue under a zero dim (R-g).
- "Return restarts the session" needs a model call (`replaceLeafSession`) and a decision about which exits keep the
  pane (R-j); a Disconnected pane has no reconnect API, so it offers Close only.
- The reconnecting pill: the board draws it 32 / 16 with "attempt 3", 1a's Toast is 36 / 18 and no attempt count
  exists (R-k). "Dims by the pane-dim token" re-tokenizes `paneDimColor` from 60 % to the kit's 15 % / 10 % (R-k2).
- The canvas's footer fill (8 %), grabber (28 %) and dim scrim (black over the cells) are not in the kit's tables or
  today's blend; the nearest kit steps and today's blend are used (R-i, R-m).
- The header's status word: the canvas says "Running", 1c's badge look says "Working".
