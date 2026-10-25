# Native Redesign — Step 3e (Size Indicator, Overlay Scrollbar, Link Pill, Hint Labels) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead of Steps 1c, 1d and 2 (pipelining ruling; 4a is done). Line numbers are at HEAD `0382e4ca`, before
1c, 1d and 2 land. Where Step 3e touches code they add or change, the task names it as their plans do and carries a
**Depends on Step <id>** note. A pre-flight scan re-verifies every reference once Step 2 has landed. Phase 3 steps
may run in parallel worktrees: see *Shared files*.

**Goal:** The resize box becomes a Toast pill centred on each resized pane ("100 × 30", or "13 pt" on a font zoom)
that fades 600 ms after the last change and is announced once; the scrollbar becomes an overlay that follows the
system's scroller style and hides at the bottom of the scrollback unless a mark that matters sits above the viewport;
the hyperlink tooltip is replaced by a target pill shown while the follow modifier is held (in the pane header's
trailing area, or at the top right of a single pane, only ever over blank cells and never on the prompt row), with
`hyperlink_hover_tooltip: false` as the Native2026 default; hint labels take the cursor colour, dim the rest to 45 %,
and turn to the selection colour once a prefix is typed.

**Architecture:**
- **Every decision is a table.** `config/ScrollBarMode.hpp` resolves the profile's preference against the system's
  scroller style; `session/ScrollbarVisibility.hpp` decides shown or hidden from position, screen, history, offset and
  the marks above the viewport (a new `OverlayHold` column of vtbackend's `ScrollbarMarkKindTable` says which marks
  count); `session/SizeIndicator.hpp` says what the pill reads and keeps a font zoom's text through the resize it
  causes; `session/HyperlinkTooltip.hpp` gains the pill's words and when it shows; `window/LinkPillPlacement.hpp`
  places the pill from per-row blank runs that `vtbackend::Terminal::overlayClearance()` reports.
- **The OS is an interface.** The system's scroller style is a new optional field of `platform::PlatformAppearance`,
  read by the Cocoa backend from `NSScroller.preferredScrollerStyle` and reported on
  `NSPreferredScrollerStyleDidChangeNotification`; the null backend reports none (Overlay). Sessions read it at
  construction and follow changes through the manager, as the motion preference does.
- **QML renders answers.** `Toast.qml` grows a size, a font and a secondary text; the size indicator and `LinkPill.qml`
  are Toasts; the scrollbar draws from tokens (the Terminal row's `┃` column). The hyperlink tooltip goes.
- **Hint labels stay in cells** (§3.11's one exception to principle 5): `applyHintOverlay` reads two new palette fields
  that only contour-dark and contour-light set, so every other scheme keeps its look.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), QML, Objective-C++ (AppKit), Catch2 v3, Python 3 (lint), CMake
build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.11 (`:643-671`) binds, with
what it cites and what cites it: §1 (principles, `:28-64`; 5 "Nothing goes over non-blank cells", 7 "When this design
touches an API that takes `bool`s, it fixes the API in the same change"), §2.1 (token rows, `:68-126`), §2.3 (Reduce
Motion, `:139-167`), §2.4 (platform appearance, `:169-185`), §2.5–§2.6 (snapshots and testing, `:187-226`), §2.7 (the
`DefaultsEdition` gate, `:228-284`) and §3.0 item 3 (`:308-311`: "The `hyperlink_hover_tooltip: false` default moves
to 3e. It ships in the same PR as the link target pill that replaces the tooltip"). §3.11's citations have moved: the
size indicator is `TerminalPane.qml:129-170` (`sizeWidget` `:134`, `updateSizeWidget` `:158-169`, its `Connections`
`:240-252`); the hyperlink tooltip is `SessionChrome.qml:59-91`; the `ScrollBar` is still `:135` (to `:285`);
`Terminal::applyHintOverlay` is still `Terminal.cpp:5395` (to `:5531`). The design record is
`docs/superpowers/specs/2026-10-23-native-redesign-design.md`. The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `surfaces` (Size indicator, Scrollbar, Tooltips,
Hint mode labels), `moments` (6 · Links and paths), `defaults-all`, `tui` and `kit-components` are in
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

- **The spec, verbatim (§3.11):**
  - "It becomes a `Toast` pill: "100 × 30", or "13 pt" on a font zoom."
  - "A new `ScrollBarMode { Overlay = 0, Always }`, with Overlay as the default (zero means default)." "An overlay
    scrollbar hides at the bottom of the scrollback, **unless marks exist above the viewport**. Marks keep it visible."
    "This is a pure decision with its own table."
  - "While the follow modifier is held, the link target shows in a pill. It sits in the pane header's trailing area,
    or at the top right of a single pane, and only ever over blank cells." "`hyperlink_hover_tooltip: false` becomes
    the default in this same PR." "The hyperlink tooltip (`SessionChrome.qml:64-90`) goes."
  - "the label uses the cursor colour, non-matches dim to 45 %, and a typed prefix switches the label to the
    selection colour." "They stay in cells … the only allowed exception to principle 5."
  - Tests: "The scrollbar visibility table." "The pill placement, which must only ever be over blank cells." "The hint
    label colours."
- **Canvas values, exact** (`surfaces`, `moments` 6, `defaults-all`; Fluent and ContourLinux have no board of their
  own and take the macOS extents, a spec gap):

  | Token | MacOS | Fluent | ContourLinux | Terminal (cells) |
  |---|---|---|---|---|
  | `size.linkPill` (height) | 22 | 22 | 22 | 1 |
  | `radii.linkPill` | 11 | 11 | 11 | 0 |
  | `size.scrollbar` (track width) | 8 | 8 | 8 | 1 |
  | `space.scrollbarInset` (gap to the pane edge, inside the hit area) | 4 | 4 | 4 | 0 |
  | `radii.scrollbar` (handle) | 4 | 4 | 4 | 0 |
  | `motion.lingerMs` (size indicator hold after the last change) | 600 | 600 | 600 | 600 |

  - Size indicator: 1a's Toast extents (`size.toast` 36, `radii.toast` 18, padding `s7`), centred on its pane; text
    `type.body` at `Font.DemiBold` with tabular digits (`font.features: { "tnum": 1 }`). The board draws 32 / 16; 1a's
    Toast wins, as Step 2's ruling did for the reconnecting pill.
  - Link pill: `size.linkPill` high, padding `s5` (10) each side, gap `s3` (6), text `type.footnote`; target in
    `colors.toastText`, hint in the new `colors.toastTextSecondary`; inset from the pane `s4` (8) top, `s5` (10)
    right. Toast fill and `e2` as Toast draws them.
  - Text: `"100 × 30"` (U+00D7 between spaces), `"13 pt"` / `"13.5 pt"`; announcement `"100 columns, 30 rows"`
    (`"1 column, 1 row"`) and, for a zoom, `"13 points"`. Pill target `"github.com › contour-terminal › contour › issues › 2175"`
    (U+203A between spaces); hint `"⌘-click to open"` under `ShortcutStyle::MacGlyphs`, `"Ctrl+Click to open"` under
    `Text`.
  - New colour roles, appended after the last `ChromeColorRole` at execution, the count derived from the last:
    `ToastTextSecondary` (native `Fixed 0xFFFFFF` at 60 %; Terminal `schemeColor(Background)`), `ScrollbarThumb`
    (native `label(1)` at 30 %; Terminal `schemeColor(Secondary)`), `ScrollbarThumbActive` (native `label(1)` at 50 %;
    Terminal `schemeColor(Accent)`), `ScrollbarTrack` (native `label(1)` at 10 %; Terminal `Fixed` at 0 %). The board's
    hint is 50 %; over a light pane that reads 4.3:1, so 60 % (R-h). Native scrollbar roles are resolved with Step 2's
    `colorOn(role, session.backgroundColor)`.
  - Glyph: `UiStyleTokens::scrollThumbGlyph` — `""` native, `"┃"` Terminal.
  - Hint colours (contour-dark / contour-light): label `{ fg #1C1E24, bg #E8C26E }` / `{ fg #FAFAF8, bg #1F5FBF }`
    (the scheme's background on its cursor); typed `{ fg #D9DCE3, bg #3A4A6B }` / `{ fg #2B2E35, bg #C8D8F5 }` (its
    foreground on its selection); unmatched cells keep 45 % of their colour. Every other scheme keeps 50 % and today's
    half-dimmed typed characters.
  - Defaults (`EditionDefaultsTable`): Classic `hyperlink_hover_tooltip: true`, `scrollbar.mode: always`; Native2026
    `false`, `system`.
  - The size indicator stays quiet for the session's first second (today's `upTime > 1.0`), read from the injected
    `TerminalClocks::steady`.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the 1c, 1d, 2 SDD ledgers):
  - Toast from 1a is the pill shape; Reduce Motion turns every transition into a 100 ms cross-fade
    (`ReducedMotionCrossFadeMs`) with no travel, and never shortens a hold (`lingerMs` stays 600).
  - Status colours: Danger, Warning, Success, Running (= accent); yellow means search and nothing else is yellow-filled.
  - No colour or size literal in chrome QML (`check-qml-colors`, `check-qml-sizes`): a new file's baseline is 0 and a
    baseline only goes down. `TerminalPane.qml` and `SessionChrome.qml` lose the literals of the removed box, tooltip
    and scrollbar; regenerate their lines with `python3 scripts/check-qml-sizes.py --print-baseline`.
  - Every visual surface is a snapshot row, and every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`).
  - `enum class` over `bool`, zero the default; QML-facing `Q_PROPERTY`/`Q_INVOKABLE` booleans and `Config`'s public
    YAML `bool`s are the documented carve-outs. Data-driven tables; counts in tests are derived from tables.
  - The macOS follow modifier is Command in Native2026 (ruling R5); the pill's hint is rendered from the configured
    `FollowHyperlink` binding, never from a literal.
  - Text contrast at least 4.5:1 (`MinimumTextContrast`), checked against contour-dark and contour-light.
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`; a Native2026 case sets the config field it needs.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** dependency injection (the scroller style through `IPlatformAppearance`, time through
  `TerminalClocks`); configuration at construction (a session reads the system's scroller style when it is made;
  `applySystemScrollBars` is live reconfiguration, documented as `applyMotionPreference` is); Doxygen on every new
  public declaration; `.hpp` with `#pragma once`; zero warnings and no `NOLINT`; `slots`, `signals` and `emit` are
  never identifiers. This step touches `HyperlinkHoverTracker::Change::changed` (a `bool`), which becomes
  `HoverChange` in the same change.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`):
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds in one tree at
  once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit, run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding. Format every touched `.cpp`/`.hpp`/`.mm` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`;
  check every new or touched `.qml`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`. **Never
  clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value,
  and every table row names every field (`ChromeTokenRows`, `EditionDefaultsTable`, `ScrollbarMarkKindTable`,
  `SnapshotScripts`, the `ContourScheme` rows, the null and test `PlatformAppearance`s).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **QML:** enums are unreachable through `chromeStyle`; QML passes and compares `int`s or the name strings the
  provider publishes. Every new control sets `Accessible.role` and `Accessible.name`.
- **Qt-free stays Qt-free:** `config/ScrollBarMode.hpp`, `session/ScrollbarVisibility.hpp`, `session/SizeIndicator.hpp`,
  `session/HyperlinkTooltip.hpp`, `window/LinkPillPlacement.hpp` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files by name (other agents share the tree; never `git add -A` or
  a directory). Each message ends with exactly `Signed-off-by: Christian Parpart <christian@parpart.family>`, and never
  carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 17).

## Review Focus

1. **Resizing a split with several panes at once.** A window resize changes every pane's page in one event. Expected:
   each pane shows its own pill centred on itself with its own size; a pane narrower than the pill shows none; only
   the active pane is announced, once, after the burst settles; a font zoom keeps "13 pt" through the resize it causes.
   Pinned in Task 5 (`sizeIndicator.aFontZoomKeepsItsTextThroughTheResize`), Task 6 ("only the active pane is announced,
   once per burst") and Task 12 ("every pane of a split shows its own size indicator").
2. **A link under the cursor on the prompt row.** A URL typed at the prompt, hovered with ⌘ held, on a single pane
   cleared to row 0. Expected: the pill never covers the prompt or cursor row; it goes to the bottom right, or nowhere.
   Pinned in Task 8 (`overlayClearance.avoidsTheCursorAndTheLivePrompt`), Task 9 (`linkPill.neverOnThePromptRow`) and
   Task 10 ("a link at the prompt places the pill off the prompt row").
3. **A full-screen TUI with no blank cells.** `htop` or `mc` paints every cell, often with a coloured background.
   Expected: no pill at all (a coloured space is not blank); the link still lights up and ⌘-click still opens it; no
   QML warning. Pinned in Task 8 (`overlayClearance.aColouredSpaceIsNotBlank`), Task 9 (`linkPill.noBlankCellsNoPill`),
   Task 10 ("the target is still followed when the pill has nowhere to go") and Task 14.
4. **The scrollbar on a trackpad versus a mouse.** macOS "Show scroll bars: Automatically" flips between overlay and
   legacy scrollers when a mouse is plugged in or removed. Expected: the bar follows live in every session; at the
   bottom it shows under legacy scrollers and hides under overlay ones; `scrollbar.mode: always|overlay` overrides the
   system; Classic stays always-shown. Pinned in Task 1 (`scrollBarMode.resolves`), Task 3 (the Cocoa notification
   case) and Task 4 ("the system's scroller style switches the mode live").
5. **Hint labels over wide characters.** A match beginning on `日本`, a label landing on a wide glyph's head, and a
   kitty-scaled cell. Expected: every label character is one narrow cell (`width == 1`, default `GlyphSizing`), the
   wide glyph it replaced is not drawn half, and no label character is lost. Pinned in Task 11
   (`Terminal.hintLabels.aLabelOverAWideCharacterIsNarrowCells`).

Also pinned, beyond the five: Classic keeps its scrollbar, hint colours and a hover-shown target (Tasks 1, 4, 10, 11);
Reduce Motion keeps the 600 ms hold (Task 15); a scroll withdraws the pill as it withdrew the tooltip (Task 10).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Where the mode comes from (owner, 3).** §3.11 names `ScrollBarMode { Overlay = 0, Always }`; the canvas says
  the bar "follows the system". So the profile gains `scrollbar.mode: system | overlay | always`
  (`ScrollBarModePreference { System = 0, Overlay, Always }`), resolved against `PlatformAppearance::scrollBars` into
  §3.11's `ScrollBarMode`; no system answer (the null backend, Windows and Linux until Phase 5) is `Overlay`. §2.7: a
  Classic file keeps today's look, so Classic's default is `always`, Native2026's `system`.
- **R-b. Which marks keep the bar (owner, 2).** Every finished command leaves a `Command` tick, so "marks keep it
  visible" read literally shows the bar at the bottom of every shell-integrated session, against the canvas's "hidden
  at the bottom". A mark holds the overlay bar when its kind's new `OverlayHold` column says `KeepsShown`: `Failure`,
  `Running` and `UserMark`; `Command` is `Quiet`. Only kinds `scrollbar.marks` draws count. "Above the viewport" is a
  visible row below the viewport's first row; the rule only matters at the bottom, where that row is the scrollable
  count.
- **R-c. The visibility rules, in order:** position `Hidden` → hidden; the alternate screen with
  `hide_in_alt_screen` → hidden; no history → hidden; `Always` → shown; `Overlay` → shown when scrolled back or a held
  mark is above, else hidden. The bar keeps its tick layer and every interaction it has today.
- **R-d. The pure headers live in `session/`** beside `SearchStatus.hpp` and `HyperlinkTooltip.hpp`, because
  `TerminalSession` consumes them and `session/` may not include `window/`. The pill placement is QML-facing geometry
  and lives in `window/`, answered through the provider as 3b's `findBarPlacement` is.
- **R-e. What `hyperlink_hover_tooltip` means without a tooltip (owner, 1).** The tooltip goes (§3.11). The key now
  says whether the target pill shows on plain hover (`true`) or only while the follow modifier is held (`false`).
  It stays a YAML `bool` (Config's documented exception) and becomes `LinkTargetReveal { HeldModifier = 0, Hover }`
  inside the session. Classic keeps `true`: its users still see a hovered link's target, in the pill.
- **R-f. What the pill says.** A URL reads as its host and its non-empty path segments joined by `" › "` (scheme,
  user info, port, query and fragment dropped; percent-decoding as the tooltip does); a local `file://` URI or a
  detected path reads as the path (plus `:line:column` where detection found one); anything else as written. It is
  sanitised for display and capped at 100 codepoints (`MaxTooltipLength`) with `elideMiddle`. Unlike the tooltip, a
  detected link printed as it opens still shows its target: the canvas's pill names a printed URL. The hint after it is
  `linkOpenHint` of the configured follow modifiers; empty when no binding follows links.
- **R-g. Where the pill goes (owner, 5).** In a pane with a header (Step 2's `PaneHeader` visible): the header's
  trailing slot, over no cells, with the header's status and actions hidden while it shows. Otherwise at the top right
  (`s4` from the top, `s5` from the right) when every cell under it is blank and none is on an avoided row; else at the
  bottom right under the same test; else nowhere. "Blank" is `Line::trimmedColumns()`'s default cell (no text or a
  plain space, default rendition, no hyperlink, no image). Avoided rows are the live prompt's (`livePromptSpan()`) and
  the cursor's, on the page on display. The pill never moves to dodge the link itself; a link's cells are not blank.
- **R-h. Contrast over the board.** The board's 50 % hint over the Toast fill reads 4.3:1 on a light pane; 60 % reads
  5.3:1, so the hint is 60 %. The Toast fill stays 1a's.
- **R-i. Hint colours are a scheme's (owner, 4).** `ColorPalette` gains `std::optional<CellRGBColorAndAlphaPair>
  hintLabelTyped` (empty: today's half-dimmed typed characters) and `float hintUnmatchedOpacity = 0.5f`. Only
  `applyContourScheme` sets them (and sets `hintLabel` from the scheme's background and cursor), so `contour`,
  `contour-classic` and every other scheme draw as today, as 3b's search yellow did. The scheme file key
  `hint_label_typed` reads and writes the new pair beside `hint_label`.
- **R-j. "A typed prefix switches the label".** The handler keeps only matches whose label starts with the prefix
  (`HintModeHandler::matches()`), so once anything is typed every label on screen is drawn whole in `hintLabelTyped`,
  where the scheme sets one.
- **R-k. A label is narrow cells.** Each label character's cell gets `width = 1` and `sizing = GlyphSizing {}`; today
  only `width` is reset, so a label over a wide or kitty-scaled head keeps `sizing.columns = 2` and draws over its
  neighbour.
- **R-l. The size indicator's burst.** A page resize or a font change starts or extends a burst; the pill shows the
  burst's text; a font zoom's "13 pt" survives the page resize it causes for as long as the burst lasts; the burst ends
  when the pill has been still for `motion.lingerMs`, and only then is it announced, Polite, for the active pane alone.
  The gate moves from QML (`upTime > 1.0 && showResizeIndicator`) into the session; the page resize is reported once
  per change by `FontControl.cpp`'s `applyResize` (`:112-150`), not once per dimension.
- **R-m. Accessibility.** The size pill is `Accessible.ignored` (the session announces it once); the link pill is
  `Accessible.ToolTip` named by its target and hint; the scrollbar keeps `Accessible.ScrollBar`.
- **R-n. Snapshots.** New rows `size-indicator` (a split, every pane resized), `link-pill` (one pane),
  `link-pill-split` (in the active card's header) and `scrollbar-overlay` (scrolled back with a failed block above).
  The snapshot terminal is the stub, so hint labels are pinned by Task 11's render-buffer tests, not by pictures.

### Non-goals

- ⇧⌘-click "reveal in Finder" and "paths resolve against the block's cwd" from Moments 6 (owner decision 6).
- One shared Tooltip component: `TabItem.qml`'s tooltip stays, the block tooltip is 3f's.
- Scrollbar tick restyling and search-match ticks (3f); the status line's "HINT · Open".
- Windows' and Linux's scroller-style backends (Phase 5); a Settings control for `scrollbar.mode` (4b).

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/ScrollBarMode.hpp` (create) | `ScrollBarMode`, `ScrollBarModePreference`, its table, `resolveScrollBarMode` | 1 |
| `src/contour/config/Config.{hpp,cpp}`, `ConfigDocumentation.hpp` (modify) | `scrollbar.mode`, two `EditionDefaults` columns, `hint_label_typed` | 1, 11 |
| `src/vtbackend/shell/ScrollbarMarks.{hpp,cpp}` (modify) | `OverlayHold` column, `MarksAbove`, `marksAbove` | 2 |
| `src/contour/session/ScrollbarVisibility.hpp` (create) | The visibility table | 2 |
| `src/contour/platform/PlatformAppearance.hpp`, `CocoaPlatformAppearance.mm` (modify) | The system's scroller style | 3 |
| `src/contour/ContourGuiApp.cpp`, `session/TerminalSessionManager.{hpp,cpp}` (modify) | Forward a change to every session | 3 |
| `src/contour/session/TerminalSession.{hpp,cpp}` (modify) | Scrollbar, size indicator, link target | 4, 6, 10 |
| `src/contour/session/SizeIndicator.hpp` (create), `session/FontControl.cpp` (modify) | The indicator's words and burst | 5, 6 |
| `src/contour/session/HyperlinkTooltip.{hpp,cpp}`, `command/Shortcut.{hpp,cpp}` (modify) | Pill words, reveal, hint | 7 |
| `src/vtbackend/screen/OverlayClearance.hpp` (create), `screen/Terminal.{hpp,cpp}` (modify) | Blank runs and avoided rows | 8 |
| `src/contour/window/LinkPillPlacement.hpp` (create) | Where the pill goes | 9 |
| `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `window/UiStyleProvider.{hpp,cpp}`, `window/ChromeTokenObjects.{hpp,cpp}` (modify) | Tokens, roles, glyph, the placement invokable | 9 |
| `src/vtbackend/core/ColorPalette.{hpp,cpp}`, `screen/Terminal.cpp` (modify) | Hint colours and narrow label cells | 11 |
| `src/contour/qml/Toast.qml`, `TerminalPane.qml` (modify) | The size indicator pill | 12 |
| `src/contour/qml/SessionChrome.qml` (modify) | The overlay scrollbar; the tooltip goes; the pill's host | 13, 14 |
| `src/contour/qml/LinkPill.qml` (create), `PaneHeader.qml`, `PaneCard.qml` (modify, Step 2's) | The pill and its header slot | 14 |
| `src/contour/test/QmlMocks.hpp`, `ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | Mocks, snapshot rows | 12–16 |

### Shared files

Files another Phase 3 step's spec section or plan also names, or that every Phase 3 step edits. A parallel worktree
that touches one rebases before its task commits and re-runs that task's filter; the merge order is the board's.

- `src/contour/qml/SessionChrome.qml`: 3b (the `SearchBar` instance and `onSearchBarRequested`), 3c (the
  `RequestPermission` dialogs and consent), 3f (the block tooltip `:93-128` and the tick layer `:215-283`, which 3f
  restyles: failures solid, running hollow, search matches yellow). 3e's hunks: `hyperlinkTip` (removed), the
  `ScrollBar`'s own properties, `contentItem` and `background` (not the tick layer), and one `LinkPill`.
- `src/contour/qml/TerminalPane.qml`: 3b (forwards `headerFindSlot`, `headerPresent`), Step 2. 3e replaces
  `sizeWidget`/`updateSizeWidget`/the size `Connections` and forwards `headerLinkSlot`. **`headerPresent`** is 3b's
  name too: whichever of 3b and 3e lands second reuses the property the first added.
- `src/contour/qml/PaneHeader.qml`, `PaneCard.qml` (Step 2's): 3b adds a find slot, 3f moves the sticky header into
  the header. 3e adds `linkPillSlot` at the trailing edge.
- `src/contour/qml/Toast.qml`: 3c adds `textFormat: Text.PlainText` on `toastText`. 3e adds `extent`, `cornerRadius`,
  `inset`, `textFont`, `detail` and `toastDetail`; keep 3c's line.
- `src/vtbackend/shell/ScrollbarMarks.hpp`: 3f's ticks read `ScrollbarMarkKindTable`; 3e appends the `overlay` column.
- `src/vtbackend/screen/Terminal.{hpp,cpp}`: 3a (`currentWorkingDirectory`, the `FileSystem`), 3b (the search block),
  3f (`fillGutter`, `fillStickyHeader`). 3e's hunks: `applyHintOverlay` and one new `overlayClearance()`.
- `src/vtbackend/core/ColorPalette.{hpp,cpp}`: 3b (`ContourScheme`, `applyContourScheme`: search yellow), 3f
  (`blockStatus*`). 3e adds two hint fields and three lines to `applyContourScheme`.
- `src/contour/session/TerminalSession.{hpp,cpp}`: 3b (the find-bar block), 3c (the guarded-role API, reload errors).
  3e's hunks: the scrollbar property (`.hpp:215`, `:472-483`), the hyperlink-hover block (`.hpp:221-222`, `:265`,
  `:348-397`, `:1294`, `:1756-1773`; `.cpp:3140-3218`, `:3438-3476`, `:3680-3687`, `:3734-3739`), `refreshScrollbarMarks`
  (`.cpp:3587-3654`), `bufferChanged` (`:836-842`), `onScrollOffsetChanged` (`:2760-2770`), the font-size actions
  (`:4420`, `:4719`), and new members.
- `src/contour/config/Config.hpp` `EditionDefaults`/`EditionDefaultsTable` (`:1035-1124`): Step 2 and 3a add columns;
  append after theirs. `Config_test.cpp`'s `ExpectedEditionDefaults` likewise.
- `src/contour/command/Shortcut.{hpp,cpp}`: 3d (shortcut text in the palette). 3e adds one function after
  `describeLinkGesture`.
- Edited by every Phase 3 step: `config/ChromeTokenGroups.hpp` (append fields; `ChromeColorRole` appends after
  whatever is last at execution), `config/UiStyle.hpp` glyphs, `window/UiStyleProvider.{hpp,cpp}`,
  `window/ChromeTokenObjects.{hpp,cpp}`, `test/ChromeSnapshotPlan.hpp`, `test/QmlMocks.hpp`, `src/contour/CMakeLists.txt`,
  `scripts/qml-size-literal-baseline.txt`, `metainfo.xml`.

---
### Task 1: `scrollbar.mode` and the two new edition defaults

> **Depends on Steps 2 and 3a (if landed):** their `EditionDefaults` columns; append after them, by name.

**Files:**
- Create: `src/contour/config/ScrollBarMode.hpp` (list it in `contour_config`'s sources beside `DefaultsEdition.hpp`);
  test `src/contour/config/ScrollBarMode_test.cpp` (add to `contour_test`, `src/contour/CMakeLists.txt:1153-1202`).
- Modify: `src/contour/config/Config.hpp` (`ScrollBarConfig` `:586-602`, `EditionDefaults` `:1035-1057`,
  `EditionDefaultsTable` `:1085-1124`, the profile's `scrollbar` `:1220`, `Config::hyperlinkHoverTooltip` `:1778`,
  the writer's `format(…, ScrollBarConfig)` `:2828-2831`); `src/contour/config/Config.cpp` (reader `:2958-2967`);
  `src/contour/config/ConfigDocumentation.hpp` (`ScrollbarConfig` `:525`, `ScrollbarWeb` `:2752`,
  `HyperlinkHoverTooltipConfig` `:253-257`, `HyperlinkHoverTooltipWeb` `:2393-2399`).
- Test: `src/contour/config/Config_test.cpp` (`ExpectedEditionDefaults` `:3910-3936`, both expectations
  `:3938-4004`, `checkProfileDefaults`, `checkEditionDefaults`; a new case).

**Interfaces:**
- Produces (`config/ScrollBarMode.hpp`, namespace `contour::config`, Qt-free):
  - `enum class ScrollBarMode : uint8_t { Overlay = 0, Always };` (§3.11's type)
  - `enum class ScrollBarModePreference : uint8_t { System = 0, Overlay, Always };`
  - `detail::ScrollBarModePreferenceTable` of `ConfigEnumInfo<ScrollBarModePreference>`: `{ System, "system", "Follow the system" }`,
    `{ Overlay, "overlay", "Overlay" }`, `{ Always, "always", "Always" }`, and `configEnumValues<ScrollBarModePreference>()`
    (the `DefaultsEdition.hpp:31-43` shape), so the generic reader and formatter take it.
  - `[[nodiscard]] constexpr ScrollBarMode resolveScrollBarMode(ScrollBarModePreference preference, std::optional<ScrollBarMode> system) noexcept;`
    → `System` gives `system.value_or(ScrollBarMode::Overlay)`; the other two give themselves.
- Produces (`Config.hpp`): `ScrollBarConfig::mode` (`ScrollBarModePreference mode { ScrollBarModePreference::System };`,
  documented); `EditionDefaults` gains `ScrollBarModePreference scrollBarMode;` and `bool hyperlinkHoverTooltip;`
  (Config's YAML-boundary carve-out, documented) — Classic `Always`, `true`; Native2026 `System`, `false`; the profile's
  `scrollbar { ScrollBarConfig { .mode = editionDefaults(defaultsEdition).scrollBarMode } }`;
  `hyperlinkHoverTooltip { editionDefaults(defaultsEdition).hyperlinkHoverTooltip }`.
- YAML: `scrollbar: { position, mode, hide_in_alt_screen, marks }`; the doc comment for `mode`: "system follows the
  operating system's scroll bar setting; overlay hides the bar at the bottom of the scrollback; always shows it while
  there is scrollback." `hyperlink_hover_tooltip`'s comment: "Whether hovering a link shows its target without holding
  the follow modifier. While the modifier is held the target always shows."

- [ ] **Step 1: Write the failing tests.**
  - `ScrollBarMode_test.cpp` (`[scrollbarmode]`): `scrollBarMode.resolves` (Review Focus 4) — `System` with `nullopt`
    is `Overlay`, with `Always` is `Always`, with `Overlay` is `Overlay`; `Overlay` and `Always` ignore every system
    value (`iota` over the preferences and `{nullopt, Overlay, Always}`); `scrollBarMode.table` — the table lists every
    enumerator once, in order, and `configEnumFromToken<ScrollBarModePreference>("ALWAYS") == Always`.
  - `Config_test.cpp` (`[config]`): `ExpectedEditionDefaults` gains `.hyperlinkHoverTooltip` and `.scrollBarMode`
    (Classic `true`, `Always`; Native2026 `false`, `System`); `checkProfileDefaults` checks
    `profile.scrollbar.value().mode`, `checkEditionDefaults` checks `config.hyperlinkHoverTooltip.value()`, so
    `Config.firstLaunchDefaults` covers a partial `contour.yml` (Classic), a `settings.yml` stating `native2026`, and the
    generated config.
  - New "Config: scrollbar.mode reads system, overlay and always, and keeps the default on anything else": each token
    loads; `mode: sideways` keeps the edition's default and logs; the generated config round-trips `mode`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile error, `config/ScrollBarMode.hpp` not found.
- [ ] **Step 3: Implement** the header, the columns, the reader and writer lines and the documentation.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[scrollbarmode]"
out/appleclang-debug/src/contour/contour_gui_test "[config]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/config/ScrollBarMode_test.cpp src/contour/config/Config.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ScrollBarMode.hpp src/contour/config/ScrollBarMode_test.cpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/config/ScrollBarMode.hpp src/contour/config/ScrollBarMode_test.cpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/CMakeLists.txt
git commit -m "config: add scrollbar.mode, and gate the overlay scrollbar and the hover target behind the 2026 defaults" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The overlay scrollbar's visibility table

**Files:**
- Modify: `src/vtbackend/shell/ScrollbarMarks.hpp` (`ScrollbarMarkKindRow` `:86-101`, `ScrollbarMarkKindTable`
  `:107-124`), `src/vtbackend/shell/ScrollbarMarks.cpp`; test `src/vtbackend/shell/ScrollbarMarks_test.cpp`.
- Create: `src/contour/session/ScrollbarVisibility.hpp` (list it in the session sources beside `SearchStatus.hpp`);
  test `src/contour/session/ScrollbarVisibility_test.cpp` (add to `contour_test` beside `session/SearchStatus_test.cpp`).

**Interfaces:**
- Consumes: Task 1 (`ScrollBarMode`).
- Produces (`vtbackend`, `ScrollbarMarks.hpp`):
  - `enum class OverlayHold : uint8_t { Quiet = 0, KeepsShown };` and `ScrollbarMarkKindRow::overlay`
    (`OverlayHold overlay = OverlayHold::Quiet;`): `Command` `Quiet`; `UserMark`, `Running`, `Failure` `KeepsShown` (R-b).
  - `enum class MarksAbove : uint8_t { None = 0, Some };`
  - `[[nodiscard]] MarksAbove marksAbove(std::span<ScrollbarMarkInput const> marks, int64_t firstVisibleRow, ScrollbarMarkSources sources) noexcept;`
    → `Some` when a mark has `visibleRow < firstVisibleRow`, its kind's `source` is in `sources`, and its kind's
    `overlay` is `KeepsShown`.
- Produces (`session/ScrollbarVisibility.hpp`, namespace `contour::session`, Qt-free):
  - `enum class ScrollbarVisibility : uint8_t { Hidden = 0, Shown };`
  - `enum class AltScreenScrollbar : uint8_t { Hidden = 0, Shown };` (from `hide_in_alt_screen`, converted at the boundary)
  - `struct ScrollbarVisibilityInputs { config::ScrollBarPosition position = config::ScrollBarPosition::Right; config::ScrollBarMode mode = config::ScrollBarMode::Overlay; vtbackend::ScreenType screen = vtbackend::ScreenType::Primary; AltScreenScrollbar altScreen = AltScreenScrollbar::Hidden; int64_t historyLines = 0; int64_t scrollOffset = 0; vtbackend::MarksAbove marksAbove = vtbackend::MarksAbove::None; };`
  - `[[nodiscard]] constexpr ScrollbarVisibility resolveScrollbarVisibility(ScrollbarVisibilityInputs const& in) noexcept;`
    — R-c's five rules, in order.

- [ ] **Step 1: Write the failing tests.**
  - `ScrollbarMarks_test.cpp` (`[scrollbar-marks]`): `ScrollbarMarks.overlayHold.column` — `Command` is `Quiet`, the other
    three `KeepsShown`, read from the table; `ScrollbarMarks.marksAbove.onlyHeldDrawnKindsAboveTheRowCount` — marks
    `{ {5, Command}, {7, Failure}, {40, Failure} }`: `marksAbove(…, 30, All) == Some`; at 6 `None` (only a `Command`
    above); at 30 with sources `{Commands, UserMarks}` `None` (failures are not drawn); empty marks `None`.
  - `ScrollbarVisibility_test.cpp` (`[scrollbarvisibility]`): `scrollbarVisibility.table` — a table of rows
    `{ inputs, expected }` covering each rule and its boundary: `Hidden` position wins over everything; alternate screen
    with `AltScreenScrollbar::Hidden` hidden, with `Shown` follows the rest; `historyLines == 0` hidden even under
    `Always`; `Always` at the bottom shown; `Overlay` at the bottom hidden, at `scrollOffset == 1` shown, at the bottom
    with `MarksAbove::Some` shown. `scrollbarVisibility.everyCombination` — `iota` over every enum input and
    `historyLines ∈ {0, 10}`, `scrollOffset ∈ {0, 3}`: the result equals the five rules written as one expression in the
    test, and is never `Shown` with `historyLines == 0`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test contour_test`

  Expected: compile error, no `OverlayHold`.
- [ ] **Step 3: Implement** the column (every row names it), `marksAbove` in the `.cpp`, and the header.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[scrollbar-marks]"
out/appleclang-debug/src/contour/contour_test "[scrollbarvisibility]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/shell/ScrollbarMarks.cpp src/vtbackend/shell/ScrollbarMarks_test.cpp src/contour/session/ScrollbarVisibility_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/shell/ScrollbarMarks.hpp src/vtbackend/shell/ScrollbarMarks.cpp src/vtbackend/shell/ScrollbarMarks_test.cpp src/contour/session/ScrollbarVisibility.hpp src/contour/session/ScrollbarVisibility_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/vtbackend/shell/ScrollbarMarks.hpp src/vtbackend/shell/ScrollbarMarks.cpp src/vtbackend/shell/ScrollbarMarks_test.cpp src/contour/session/ScrollbarVisibility.hpp src/contour/session/ScrollbarVisibility_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: decide whether the scrollbar shows from one table, and which marks hold an overlay bar" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: The system's scroller style

**Files:**
- Modify: `src/contour/platform/PlatformAppearance.hpp` (`PlatformAppearance`), `src/contour/platform/CocoaPlatformAppearance.mm`
  (`:22-80`), `src/contour/ContourGuiApp.cpp` (`onPlatformAppearanceChanged` `:1916-1919`),
  `src/contour/session/TerminalSessionManager.{hpp,cpp}` (beside `updateMotionPreference`, `.hpp:499-501`, `.cpp:1191-1195`),
  `src/contour/test/AppearanceFixtures.hpp` and `QmlChromeStyle.hpp` (name the new field).
- Test: `src/contour/platform/CocoaAppearanceSources_test.mm`, `src/contour/platform/AppearanceSources_test.cpp`.

**Interfaces:**
- Consumes: Task 1 (`ScrollBarMode`).
- Produces:
  - `PlatformAppearance` gains `std::optional<config::ScrollBarMode> scrollBars {};` — the system's scroller style, if
    it publishes one. `NullPlatformAppearance` reports none.
  - Cocoa: `current()` sets `scrollBars` to `Overlay` for `NSScrollerStyleOverlay` and `Always` for
    `NSScrollerStyleLegacy` (`NSScroller.preferredScrollerStyle`, @pre main thread); a second observer on
    `NSPreferredScrollerStyleDidChangeNotification` reports `current()` through the same callback; the destructor
    removes both.
  - `TerminalSessionManager::updateSystemScrollBars(std::optional<config::ScrollBarMode> system)` → each session's
    `applySystemScrollBars(system)` (Task 4). `ContourGuiApp::onPlatformAppearanceChanged(appearance)` calls it with
    `appearance.scrollBars`, beside `refreshChromeStyle()`.

- [ ] **Step 1: Write the failing tests.**
  - `CocoaAppearanceSources_test.mm` (`[appearance-source][cocoa]`), "Cocoa appearance reads the scroller style and
    reports its change" (Review Focus 4): `current().scrollBars` equals the mapping of `NSScroller.preferredScrollerStyle`;
    posting `NSPreferredScrollerStyleDidChangeNotification` calls the callback once with `current()`; after the source
    is destroyed a post calls nothing. The existing accent case still holds.
  - `AppearanceSources_test.cpp`: the null appearance's `scrollBars` is `nullopt`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[appearance-source]"`

  Expected: compile error, no member `scrollBars`.
- [ ] **Step 3: Implement** the field, the Cocoa reading and observer, and the forwarding (`applySystemScrollBars` is
  declared in Task 4; land both declarations here as a no-op body that Task 4 fills, so this task builds).
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[appearance-source]"`

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/ContourGuiApp.cpp src/contour/session/TerminalSessionManager.cpp src/contour/platform/AppearanceSources_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/PlatformAppearance.hpp src/contour/platform/CocoaPlatformAppearance.mm src/contour/platform/CocoaAppearanceSources_test.mm src/contour/platform/AppearanceSources_test.cpp src/contour/ContourGuiApp.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/test/AppearanceFixtures.hpp src/contour/test/QmlChromeStyle.hpp
git add src/contour/platform/PlatformAppearance.hpp src/contour/platform/CocoaPlatformAppearance.mm src/contour/platform/CocoaAppearanceSources_test.mm src/contour/platform/AppearanceSources_test.cpp src/contour/ContourGuiApp.cpp src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/test/AppearanceFixtures.hpp src/contour/test/QmlChromeStyle.hpp
git commit -m "contour: read the system's scroller style on macOS and hand its changes to every session" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The session decides the scrollbar through the table

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp` (`isScrollbarVisible` `:215`, `getIsScrollbarVisible` `:472-483`,
  members near `:1955-1966`), `src/contour/session/TerminalSession.cpp` (constructor `:368-392`, `bufferChanged`
  `:836-842`, `onScrollOffsetChanged` `:2760-2770`, `announceScrollableLineCount` `:3554-3560`, `refreshScrollbarMarks`
  `:3587-3654`, the profile notifiers `:5850-5859`).
- Test: `src/contour/session/TerminalSession_test.cpp` (new `[contour][session][scrollbar]` cases).

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces (`TerminalSession`):
  - `void applySystemScrollBars(std::optional<config::ScrollBarMode> system);` — live reconfiguration (documented as
    `applyMotionPreference` is): stores it and refreshes. The constructor reads `_app.platformAppearance().scrollBars`.
  - `void refreshScrollbarVisibility();` (private, GUI thread) — builds `ScrollbarVisibilityInputs` from the profile
    (`position`, `resolveScrollBarMode(mode, _systemScrollBars)`, `hideScrollbarInAltScreen ? Hidden : Shown`), the
    displayed screen type, the published history count, the scroll offset and `_marksAbove`; stores
    `_scrollbarVisibility`; emits `isScrollbarVisibleChanged` only on a change.
  - `getIsScrollbarVisible()` returns `_scrollbarVisibility == ScrollbarVisibility::Shown` (the QML-facing `bool` stays).
  - `refreshScrollbarMarks` computes `_marksAbove = vtbackend::marksAbove(*inputs, unbox(scrollable), key.sources)`
    whenever it reads the inputs, then calls `refreshScrollbarVisibility()`. `bufferChanged`, the posted half of
    `onScrollOffsetChanged`, `announceScrollableLineCount` and the profile notifiers call it too.

- [ ] **Step 1: Write the failing tests** (`[contour][session][scrollbar]`; a helper `registerScrollbarModeProfile(app, name, mode)`
  beside `registerMarkedScrollbarProfile`; history from `writeFiller`, blocks from `vtbackend::test::commandRun`):
  - "the overlay scrollbar hides at the bottom and shows when scrolled back": profile `overlay`, 60 filler lines: at the
    bottom `isScrollbarVisible()` is false; `setScrollOffset(5)` → true and one `isScrollbarVisibleChanged`; back to 0 →
    false; no history at all → false.
  - "a failed command above the viewport keeps the overlay scrollbar": `commandRun("make", numberedOutput(3), 2)` then
    filler past a page, `setScrollbarMarkTrackPixels(200)`, `refreshScrollbarMarks()` → visible at the bottom; with
    exit code 0 instead → hidden; with `scrollbar.marks: []` and the failure → hidden.
  - "the system's scroller style switches the mode live" (Review Focus 4): profile `system`, at the bottom with history:
    `applySystemScrollBars(Always)` → visible; `(Overlay)` → hidden; `(nullopt)` → hidden; profile `always` ignores
    `Overlay`; a Classic `TestApp` session (no profile override) is visible at the bottom, as today.
  - "position hidden and the alternate screen stay hidden": `position: Hidden` under `always` → hidden; `\033[?1049h`
    with `hide_in_alt_screen: true` → hidden.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][scrollbar]"`

  Expected: FAIL, the overlay bar is visible at the bottom.
- [ ] **Step 3: Implement** as the Interfaces block states.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][session][scrollbar],[contour][session][scrollbar-marks],[contour][gui][qml][scrollbar]"`

  Expected: PASS; the existing marks and QML scrollbar cases are unchanged (TestApp is Classic: `always`).
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git commit -m "contour: an overlay scrollbar hides at the bottom unless a failure, a running command or a mark is above" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: The size indicator's words and its burst

**Files:**
- Create: `src/contour/session/SizeIndicator.hpp` (session sources); test `src/contour/session/SizeIndicator_test.cpp`
  (add to `contour_test`).

**Interfaces:**
- Produces (`session/SizeIndicator.hpp`, namespace `contour::session`, Qt-free):
  - `enum class SizeIndicatorKind : uint8_t { PageSize = 0, FontSize };`
  - `enum class SizeIndicatorBurst : uint8_t { Idle = 0, Showing };`
  - `inline constexpr auto SizeIndicatorStartupQuiet = std::chrono::seconds { 1 };`
  - `[[nodiscard]] constexpr SizeIndicatorKind nextSizeIndicatorKind(SizeIndicatorBurst burst, SizeIndicatorKind showing, SizeIndicatorKind event) noexcept;`
    → `FontSize` when `burst == Showing && showing == FontSize`, else `event` (R-l).
  - `[[nodiscard]] std::string sizeIndicatorText(SizeIndicatorKind kind, vtbackend::PageSize page, double points);`
    → `"{columns} × {lines}"` or `"{points} pt"` with the shortest decimal form (`13` → `"13"`, `13.5` → `"13.5"`).
  - The gate: `enum class SizeIndicatorGate : uint8_t { Quiet = 0, Open };` and
    `[[nodiscard]] constexpr SizeIndicatorGate sizeIndicatorGate(std::chrono::steady_clock::duration uptime, bool sizeIndicatorOnResize) noexcept;`
    (the profile `bool` is a YAML-boundary value, documented) → `Open` when `uptime > SizeIndicatorStartupQuiet` and the
    profile key is on.

- [ ] **Step 1: Write the failing tests** (`[sizeindicator]`):
  - `sizeIndicator.text`: `{PageSize, 30×100}` → `"100 × 30"` (with U+00D7); `{FontSize, 13.0}` → `"13 pt"`; `13.5` →
    `"13.5 pt"`.
  - `sizeIndicator.aFontZoomKeepsItsTextThroughTheResize` (Review Focus 1): `nextSizeIndicatorKind(Showing, FontSize,
    PageSize) == FontSize`; `(Idle, FontSize, PageSize) == PageSize`; `(Showing, PageSize, FontSize) == FontSize`.
  - `sizeIndicator.gate`: `1 s` → `Quiet`; `1 s + 1 ms` → `Open`; key off → `Quiet` at any uptime.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `session/SizeIndicator.hpp` not found.
- [ ] **Step 3: Implement** the header.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/contour/contour_test "[sizeindicator]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/SizeIndicator_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/SizeIndicator.hpp src/contour/session/SizeIndicator_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/session/SizeIndicator.hpp src/contour/session/SizeIndicator_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: say what the size indicator reads, and keep a font zoom's size through its resize" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: The session raises and announces the size indicator

**Files:**
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}` (properties near `:201-217`, signals near `:1306-1332`, the
  font actions `:4420`, `:4719`, `setFontSize` `:6174`), `src/contour/session/FontControl.cpp` (`applyResize`
  `:145-150`).
- Test: `src/contour/session/TerminalSession_test.cpp` (`[contour][session][sizeindicator]`).

**Interfaces:**
- Consumes: Task 5.
- Produces (`TerminalSession`):
  - `Q_PROPERTY(QString sizeIndicatorText READ sizeIndicatorText NOTIFY sizeIndicatorRequested)` and the signal
    `void sizeIndicatorRequested();` — emitted on every request the gate lets through, so QML restarts its hold.
  - `void requestSizeIndicator(SizeIndicatorKind event);` — public, GUI thread. Asks `sizeIndicatorGate(steady.now() -
    _startedAt, profile().sizeIndicatorOnResize)` with `_app.terminalClocks().steady` (the start read from the same
    clock at construction); picks the kind with `nextSizeIndicatorKind`; sets the burst `Showing`; formats the text from
    the page size or the profile's font size.
  - `Q_INVOKABLE void sizeIndicatorSettled(bool paneActive);` (QML carve-out) — ends the burst; when `paneActive`
    announces, Polite: `tr("%n column(s)", "", columns) + ", " + tr("%n row(s)", "", lines)`, or `tr("%1 points")`.
  - `applyResize` calls `session.requestSizeIndicator(SizeIndicatorKind::PageSize)` once when either dimension
    changed, after its two signals; the three font-size actions and `setFontSize` call it with `FontSize`.
  - `showResizeIndicator` (`:203`, `:517`) and the QML `upTime` read go with Task 12's QML; the property stays until then.

- [ ] **Step 1: Write the failing tests** (`[contour][session][sizeindicator]`, a `RecordingAnnouncer` as the a11y
  case at `:5475` installs it; time from `testApp.steadyClock().advance(…)`):
  - "the indicator stays quiet for the first second": `requestSizeIndicator(PageSize)` emits nothing; after
    `advance(1001ms)` it emits once and `sizeIndicatorText() == "100 × 30"` for a 100 × 30 page.
  - "a font zoom reads in points and keeps its text through its resize": `IncreaseFontSize` → `"13 pt"` (from 12), then
    `requestSizeIndicator(PageSize)` → still `"13 pt"`; after `sizeIndicatorSettled(true)` a page request reads the page.
  - "only the active pane is announced, once per burst" (Review Focus 1): two sessions, three page requests each, then
    `sizeIndicatorSettled(true)` on one and `(false)` on the other: exactly one announcement, `"100 columns, 30 rows"`,
    Polite; a 1 × 1 page says `"1 column, 1 row"`; a zoom says `"13 points"`.
  - "size_indicator_on_resize: false raises nothing".
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][sizeindicator]"`. Expected: compile error, no `requestSizeIndicator`.
- [ ] **Step 3: Implement** as the Interfaces block states.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][session][sizeindicator],[contour][session][a11y],[contour][session][view]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSession.cpp src/contour/session/FontControl.cpp src/contour/session/TerminalSession_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/FontControl.cpp src/contour/session/TerminalSession_test.cpp
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/FontControl.cpp src/contour/session/TerminalSession_test.cpp
git commit -m "contour: a session raises its size indicator once per change and announces the settled size for the active pane" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The link target's words, its hint, and when it shows

**Files:**
- Modify: `src/contour/session/HyperlinkTooltip.{hpp,cpp}` (`:62-110`, `.cpp:77-124`), `src/contour/command/Shortcut.{hpp,cpp}`
  (after `describeLinkGesture`, `.hpp:208`, `.cpp:157-168`).
- Test: `src/contour/session/HyperlinkTooltip_test.cpp` (`[contour][hyperlink]`), `src/contour/command/Shortcut_test.cpp`.

**Interfaces:**
- Produces (`session/HyperlinkTooltip.hpp`):
  - `[[nodiscard]] std::string linkPillText(std::string_view uri, std::optional<std::string_view> localHost, size_t maxLength);`
    — R-f for an OSC 8 URI (a local `file://` is its decoded path, as `hyperlinkTooltipText` does).
  - `[[nodiscard]] std::string linkPillText(vtbackend::DetectedLink const& link, size_t maxLength);` — R-f for a detected
    link; always its target, never empty for a link. Replaces `detectedLinkTooltip`, whose one caller moves in Task 10.
  - `enum class LinkTargetReveal : uint8_t { HeldModifier = 0, Hover };`
    `[[nodiscard]] constexpr LinkTargetReveal linkTargetRevealOf(bool hoverTooltip) noexcept;` (the YAML boundary)
  - `enum class LinkPillVisibility : uint8_t { Hidden = 0, Shown };`
    `[[nodiscard]] constexpr LinkPillVisibility linkPillVisibility(LinkTargetReveal reveal, vtbackend::FollowModifiers held, std::string_view target) noexcept;`
    → `Shown` when `target` is non-empty and (`held == Held` or `reveal == Hover`).
  - `HyperlinkHoverTracker::Change` becomes `{ HoverChange change = HoverChange::Unchanged; std::string text; vtbackend::CellLocation anchor {}; }`
    with `enum class HoverChange : uint8_t { Unchanged = 0, Changed };`; its `update(uri, cell, maxLength)` returns
    `linkPillText(uri, …)` as `text`.
- Produces (`command/Shortcut.hpp`): `[[nodiscard]] std::string linkOpenHint(std::optional<vtbackend::Modifiers> follow, ShortcutStyle style);`
  → `""` for none; `modifiersText(*follow, style) + (style == MacGlyphs ? "-click" : "Click") + " to open"`.

- [ ] **Step 1: Write the failing tests.**
  - `HyperlinkTooltip_test.cpp` (`[contour][hyperlink]`): "linkPillText reads a URL as a breadcrumb":
    `"https://github.com/contour-terminal/contour/issues/2175"` → `"github.com › contour-terminal › contour › issues › 2175"`;
    `"https://user@host:8443/a//b/?q=1#f"` → `"host › a › b"`; `"https://example.com"` → `"example.com"`;
    `"file:///home/user/my%20file.txt"` → `"/home/user/my file.txt"`; `"mailto:someone@example.com"` as written; a
    right-to-left override is replaced, as the tooltip's case at `:132` checks; a 300-character URL is at most 100
    codepoints. "linkPillText names a detected link's target even where it is printed" (a URL detected as printed is
    its breadcrumb; a path with a location gets `":3:1"`). "linkPillVisibility": every combination of reveal, held and
    `{"", "x"}` (`iota`), `Shown` exactly where the rule says. The tracker cases now read `change == HoverChange::Changed`.
  - `Shortcut_test.cpp` (`[contour][palette]`): `linkOpenHint(Modifiers{Super}, MacGlyphs) == "⌘-click to open"`,
    `linkOpenHint(Modifiers{Control}, Text) == "Ctrl+Click to open"`, `linkOpenHint(nullopt, Text) == ""`.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test contour_test`. Expected: compile error, no `linkPillText`.
- [ ] **Step 3: Implement** the functions; the breadcrumb parses with the URL helpers `hyperlinkTooltipText` already
  uses (`localFilePath`, `sanitizeCommandLine`), splitting the authority and path by hand rather than through Qt.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][hyperlink]"
out/appleclang-debug/src/contour/contour_test "[contour][palette]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/HyperlinkTooltip.cpp src/contour/session/HyperlinkTooltip_test.cpp src/contour/command/Shortcut.cpp src/contour/command/Shortcut_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/HyperlinkTooltip.hpp src/contour/session/HyperlinkTooltip.cpp src/contour/session/HyperlinkTooltip_test.cpp src/contour/command/Shortcut.hpp src/contour/command/Shortcut.cpp src/contour/command/Shortcut_test.cpp
git add src/contour/session/HyperlinkTooltip.hpp src/contour/session/HyperlinkTooltip.cpp src/contour/session/HyperlinkTooltip_test.cpp src/contour/command/Shortcut.hpp src/contour/command/Shortcut.cpp src/contour/command/Shortcut_test.cpp
git commit -m "contour: say a link's target as a breadcrumb, how to open it, and when its pill shows" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Overlay clearance: blank runs and avoided rows

**Files:**
- Create: `src/vtbackend/screen/OverlayClearance.hpp` (list it in `src/vtbackend/CMakeLists.txt` beside `screen/Terminal.hpp`).
- Modify: `src/vtbackend/screen/Terminal.{hpp,cpp}` (beside `livePromptSpan()`, `.hpp:2881`).
- Test: `src/vtbackend/screen/Terminal_test.cpp` (`[terminal][overlayclearance]`).

**Interfaces:**
- Produces (`vtbackend`):
  - `struct OverlayClearance { int columns = 0; std::vector<int> trailingBlankColumns; std::vector<int> avoidRows; bool operator==(OverlayClearance const&) const = default; };`
    — one entry per row of the page on display, top first: the count of default cells at its right end
    (`pageColumns − Line::trimmedColumns()`); `avoidRows` sorted, distinct: the live prompt's rows (`livePromptSpan()`,
    translated through the viewport) and the cursor's row, each only where it is on the page on display.
  - `[[nodiscard]] OverlayClearance Terminal::overlayClearance() const;` — @pre the caller holds the lock. Rows hidden by
    a collapsed fold are read as the row standing in their place, as the hint overlay projects them.

- [ ] **Step 1: Write the failing tests** (`[terminal][overlayclearance]`, `MockTerm` 4 × 20):
  - `overlayClearance.blankRuns`: `"hello"` on row 0, nothing else: `trailingBlankColumns == {15, 20, 20, 20}`,
    `columns == 20`.
  - `overlayClearance.aColouredSpaceIsNotBlank` (Review Focus 3): `"\033[44m" + 20 spaces + "\033[m"` on row 1 → its
    entry is 0; a full-screen fill of `\033[44m\033[2J` → every entry 0.
  - `overlayClearance.avoidsTheCursorAndTheLivePrompt` (Review Focus 2): `"$ https://x.y"` with the cursor on row 0
    → `avoidRows == {0}`; with an OSC 133 prompt spanning rows 2–3 and the cursor on row 3 → `{2, 3}`; scrolled back so
    the cursor row is off the page → it is absent.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test`. Expected: compile error, no `overlayClearance`.
- [ ] **Step 3: Implement** the struct and the method.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[terminal][overlayclearance]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/screen/OverlayClearance.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_test.cpp
editorconfig-checker src/vtbackend/CMakeLists.txt
git add src/vtbackend/screen/OverlayClearance.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_test.cpp src/vtbackend/CMakeLists.txt
git commit -m "vtbackend: report each displayed row's blank run and the prompt and cursor rows an overlay must avoid" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 9: Where the link pill goes, the overlay tokens, and the provider's answer

> **Depends on Steps 1c and 2:** 1c's colour sources (`detail::label(level)`, `schemeColor`) and roles; Step 2's
> `ChromeTokenRow` fields, `colorOn` and `paneHeader(...)` invokable, whose shape `linkPillPlacement` copies.

**Files:**
- Create: `src/contour/window/LinkPillPlacement.hpp`; test `src/contour/window/LinkPillPlacement_test.cpp` (add to
  `contour_test`).
- Modify: `src/contour/config/ChromeTokenGroups.hpp` (`SizeTokens`, `SpaceTokens`, `RadiusTokens`, `MotionTokens`,
  `ChromeColorRole` and its count, the four rows, `resolveChromeTokens`' Reduced branch), `src/contour/config/UiStyle.hpp`
  (`UiStyleTokens` `:79`, `NativeTokens` `:312`, `TerminalTokens` `:383`), `src/contour/window/ChromeTokenObjects.{hpp,cpp}`,
  `src/contour/window/UiStyleProvider.{hpp,cpp}`.
- Test: `src/contour/config/ChromeTokenGroups_test.cpp`, `src/contour/config/UiStyle_test.cpp`,
  `src/contour/window/UiStyleProvider_test.cpp`.

**Interfaces:**
- Produces (`config`): `SizeTokens` gains `linkPill`, `scrollbar`; `SpaceTokens` gains `scrollbarInset`; `RadiusTokens`
  gains `linkPill`, `scrollbar`; `MotionTokens` gains `lingerMs` — values in Global Constraints. The Reduced branch
  leaves `lingerMs` alone. `ChromeColorRole` appends `ToastTextSecondary`, `ScrollbarThumb`, `ScrollbarThumbActive`,
  `ScrollbarTrack` with the sources in Global Constraints. `UiStyleTokens::scrollThumbGlyph`.
- Produces (`window/LinkPillPlacement.hpp`, namespace `contour::window`, Qt-free):
  - `enum class LinkPillHost : uint8_t { Grid = 0, PaneHeader };`
  - `enum class LinkPillAnchor : uint8_t { Hidden = 0, HeaderTrailing, TopRight, BottomRight };`
  - `struct LinkPillGrid { double marginLeft = 0; double marginTop = 0; double cellWidth = 0; double cellHeight = 0; int columns = 0; std::span<int const> trailingBlankColumns {}; std::span<int const> avoidRows {}; };`
  - `struct LinkPillInputs { LinkPillHost host = LinkPillHost::Grid; double paneWidth = 0; double paneHeight = 0; double pillWidth = 0; double pillHeight = 0; double insetTop = 0; double insetRight = 0; LinkPillGrid grid {}; };`
  - `struct LinkPillPlacement { LinkPillAnchor anchor = LinkPillAnchor::Hidden; double x = 0; double y = 0; double width = 0; bool operator==(LinkPillPlacement const&) const = default; };`
  - `[[nodiscard]] constexpr LinkPillPlacement placeLinkPill(LinkPillInputs const& in) noexcept;` — rules, in order:
    1. `pillWidth <= 0 || pillHeight <= 0` → `{}` (Hidden);
    2. `PaneHeader` → `{ HeaderTrailing, 0, 0, pillWidth }`;
    3. `width = min(pillWidth, paneWidth − 2·insetRight)`; `width <= 0` → `{}`; `x = paneWidth − insetRight − width`;
       `firstColumn = max(0, floor((x − marginLeft) / cellWidth))`;
    4. a band `[y, y + pillHeight)` is clear when every grid row it touches
       (`floor((y − marginTop) / cellHeight)` to `floor((y + pillHeight − marginTop) / cellHeight − ε)`, clipped to the
       rows) has `trailingBlankColumns[r] >= columns − firstColumn` and is not in `avoidRows`; rows outside the grid
       (the margins) are clear;
    5. `y = insetTop` clear → `{ TopRight, x, insetTop, width }`; else `yb = paneHeight − insetTop − pillHeight`, if
       `yb > insetTop` and clear → `{ BottomRight, x, yb, width }`; else `{}`.
  - `[[nodiscard]] constexpr std::string_view linkPillAnchorName(LinkPillAnchor) noexcept;` → `"hidden"`,
    `"headerTrailing"`, `"topRight"`, `"bottomRight"`.
- Produces (QML-facing): `chromeStyle.size.linkPill/scrollbar`, `space.scrollbarInset`, `radii.linkPill/scrollbar`,
  `motion.linger`, `colors.toastTextSecondary/scrollbarThumb/scrollbarThumbActive/scrollbarTrack`,
  `scrollThumbGlyph` — scaled by the units as every size is. `UiStyleProvider`:
  `Q_INVOKABLE [[nodiscard]] QVariantMap linkPillPlacement(int host, qreal paneWidth, qreal paneHeight, qreal pillWidth, qreal pillHeight, QVariantMap const& clearance) const;`
  — `insetTop = space.s4`, `insetRight = space.s5`; `clearance` is Task 10's map (`marginLeft`, `marginTop`, `cellWidth`,
  `cellHeight`, `columns`, `trailingBlank`, `avoidRows`); returns `{ anchor, x, y, width }`.

- [ ] **Step 1: Write the failing tests.**
  - `LinkPillPlacement_test.cpp` (`[linkpill]`), `base`: pane 800 × 400, cell 8 × 20, margins 0, 100 columns, 20 rows
    all blank (`trailingBlank` 100 each), pill 300 × 22, insets 8 / 10:
    - `linkPill.topRightOverBlankCells`: → `{ TopRight, 490, 8, 300 }`.
    - `linkPill.movesDownWhenTheTopIsNotBlank`: row 1 has 38 → `{ BottomRight, 490, 370, 300 }`; 39 → `TopRight`.
    - `linkPill.neverOnThePromptRow` (Review Focus 2): `avoidRows = {0}` → `BottomRight`; `{0, 19}` → Hidden.
    - `linkPill.noBlankCellsNoPill` (Review Focus 3): every entry 0 → Hidden.
    - `linkPill.inThePaneHeader`: `host = PaneHeader` → `{ HeaderTrailing, 0, 0, 300 }` whatever the grid holds.
    - `linkPill.aNarrowPaneShrinksThePill`: pane 200 wide → width 180, x 10; pane 10 wide → Hidden.
    - `linkPill.marginsAreClear`: `marginTop = 30` (the pill sits in the top margin) with row 0 not blank → `TopRight`.
    - `linkPill.names`: distinct non-empty names for every anchor.
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`, `everyRowMatchesTheDesignKit`): the new tokens and roles per row;
    `resolveChromeTokens(MacOS, { .motion = Reduced })` keeps `motion.lingerMs == 600` while `standardMs == 100`.
    "the link pill's hint reads at 4.5:1": `ToastTextSecondary` composited over `ToastFill` over contour-dark's and
    contour-light's backgrounds reaches `MinimumTextContrast`.
  - `UiStyle_test.cpp`: `scrollThumbGlyph` is `""` native, `"┃"` Terminal.
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`): MacOS publishes `size.linkPill == 22`, `radii.linkPill == 11`,
    `motion.linger == 600`; `linkPillPlacement(0, 800, 400, 300, 22, base map)` is `{ "topRight", 490, 8, 300 }`; under
    Terminal `size.linkPill == cellHeight`, `size.scrollbar == cellWidth`.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected: compile error, `LinkPillPlacement.hpp` not found.
- [ ] **Step 3: Implement** the header, the tokens, the glyph and the invokable.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_test "[linkpill],[chrometokens],[uistyle]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/window/LinkPillPlacement_test.cpp src/contour/window/UiStyleProvider.cpp src/contour/window/ChromeTokenObjects.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/LinkPillPlacement.hpp src/contour/window/LinkPillPlacement_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/config/UiStyle_test.cpp src/contour/window/UiStyleProvider_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/window/LinkPillPlacement.hpp src/contour/window/LinkPillPlacement_test.cpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/UiStyle.hpp src/contour/window/ChromeTokenObjects.hpp src/contour/window/ChromeTokenObjects.cpp src/contour/window/UiStyleProvider.hpp src/contour/window/UiStyleProvider.cpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/config/UiStyle_test.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: place the link pill only over blank cells off the prompt row, with the overlay tokens on every row" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 10: The session publishes the link target instead of a tooltip

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp` (`:219-222`, `:265`, `:348`, `:360-397`, `:1294`, `:1756-1773`),
  `src/contour/session/TerminalSession.cpp` (`sendMouseMoveEvent`'s hover `:3140-3218`, `hoveredDetectedLinkTooltip`
  `:3438-3456`, `updateHyperlinkHover` `:3458-3476`, `clearHyperlinkHover` `:3680-3687`, `onHeldModifiersChanged`
  `:3734-3739`, `screenUpdated` `:852`).
- Test: `src/contour/session/TerminalSession_test.cpp` (the hover cases `:6761`, `:7066`, `:7147-7200` and the new ones).

**Interfaces:**
- Consumes: Tasks 7, 8.
- Produces (`TerminalSession`, replacing `hyperlinkTooltipText`, `hyperlinkTooltipAnchor` and `hyperlinkHoverChanged`):
  - `Q_PROPERTY(QString linkTargetText READ linkTargetText NOTIFY linkTargetChanged)`,
    `Q_PROPERTY(QString linkTargetHint READ linkTargetHint NOTIFY linkTargetChanged)`,
    `Q_PROPERTY(bool linkTargetShown READ linkTargetShown NOTIFY linkTargetChanged)` (QML carve-out), and
    `Q_PROPERTY(int overlayClearanceRevision READ overlayClearanceRevision NOTIFY overlayClearanceChanged)`.
  - `Q_INVOKABLE [[nodiscard]] QVariantMap overlayClearance() const;` — under the lock, `Terminal::overlayClearance()`
    plus the display's page margin and cell size in logical pixels (`gridMetrics()` over `devicePixelRatio()`), as the
    keys Task 9 reads; an empty map without a display.
  - The hover always tracks (the `hyperlinkHoverTooltip` early return goes): a new cell over an OSC 8 link takes
    `linkPillText(uri, localHost, MaxTooltipLength)`, over a detected link `linkPillText(link, MaxTooltipLength)`;
    `_linkTargetReveal = linkTargetRevealOf(_config.hyperlinkHoverTooltip.value())` (re-read on reload); `shown` is
    `linkPillVisibility(reveal, terminal.followModifiers(), text)`, re-decided in `onHeldModifiersChanged` when it
    reports `LinkRevealChange::Changed`. The hint is `linkOpenHint(config::followHyperlinkModifiers(_config), platformShortcutStyle())`.
  - While `linkTargetShown`, `screenUpdated()` asks for a clearance refresh: one coalesced post per frame (an atomic
    flag, as `requestScrollbarMarksRefresh` does) that bumps the revision and emits `overlayClearanceChanged`.
  - `clearHyperlinkHover` (a scroll, leaving the grid) clears the target and emits.

- [ ] **Step 1: Write the failing tests** (`[contour][session][links]` and `[contour][session][hyperlink]`; the
  existing hover cases read `linkTargetText()` instead of `hyperlinkTooltipText()`):
  - "the target pill shows while the follow modifier is held over a link": Native2026 reveal
    (`config().hyperlinkHoverTooltip = false` before the session): over an OSC 8 link with no modifier,
    `linkTargetText()` is the breadcrumb and `linkTargetShown()` false; `onHeldModifiersChanged(follow)` → true and
    `linkTargetHint()` non-empty; released → false; each change one `linkTargetChanged`.
  - "Classic shows the target on hover": TestApp's default config → shown without a modifier.
  - "a detected link printed as it opens names its target in the pill": the `:7186` case now expects
    `"example.com › page"`.
  - "a link at the prompt places the pill off the prompt row" (Review Focus 2): `"$ https://x.y"` with the cursor on
    row 0 → `overlayClearance()["avoidRows"]` holds 0 and `trailingBlank[0] == columns − 13`.
  - "the target is still followed when the pill has nowhere to go" (Review Focus 3): a page filled with
    `\033[44m\033[2J` and an OSC 8 link, follow modifier held → `linkTargetShown()` true, every `trailingBlank` 0, and a
    follow click opens the link (the launcher records it, as `:4584` checks).
  - "a scroll withdraws the target" (the `:7066` case, renamed): output scrolling the page clears it on the GUI thread.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][session][links],[contour][session][hyperlink]"`. Expected: compile error, no `linkTargetText`.
- [ ] **Step 3: Implement** as the Interfaces block states; `QmlMocks.hpp`'s `MockSession` swaps its two tooltip
  properties for the four new ones (with setters for tests) and an `overlayClearance()` returning a settable map.
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][session][links],[contour][session][hyperlink],[contour][hyperlink],[contour][session][gutter]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/test/QmlMocks.hpp
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/test/QmlMocks.hpp
git commit -m "contour: a session publishes the hovered link's target and where an overlay may go, in place of a tooltip" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 11: Hint labels in the cursor colour, the rest at 45 %, a typed prefix in the selection

**Files:**
- Modify: `src/vtbackend/core/ColorPalette.hpp` (`:299-300`), `src/vtbackend/core/ColorPalette.cpp`
  (`applyContourScheme` `:114-133`), `src/vtbackend/screen/Terminal.cpp` (`applyHintOverlay` `:5395-5531`),
  `src/contour/config/Config.cpp` (scheme reader `:2123-2124`, writer `:5160-5170`), `src/contour/config/ConfigDocumentation.hpp`
  (a `HintLabelTyped` entry beside `HintLabel`).
- Test: `src/vtbackend/screen/Terminal_test.cpp` (`[terminal][hintmode][hintlabels]`), `src/vtbackend/core/ColorPalette_test.cpp`
  (`[ColorPalette]`), `src/contour/config/Config_test.cpp`.

**Interfaces:**
- Produces (`vtbackend::ColorPalette`): `std::optional<CellRGBColorAndAlphaPair> hintLabelTyped {};` and
  `float hintUnmatchedOpacity = 0.5f;`, documented. `applyContourScheme` sets `hintLabel = { scheme.background, 1,
  scheme.cursor, 1 }`, `hintLabelTyped = { scheme.foreground, 1, scheme.selection, 1 }`, `hintUnmatchedOpacity = 0.45f`.
- `applyHintOverlay`: with a non-empty filter and `hintLabelTyped` set, every label character takes it; with no
  `hintLabelTyped`, today's rule (typed characters mixed halfway to the background). Unmatched cells blend by
  `1 − hintUnmatchedOpacity`. Each label cell gets `width = 1` and `sizing = GlyphSizing {}` (R-k).
- Scheme key `hint_label_typed` (`foreground`, `foreground_alpha`, `background`, `background_alpha`, as `hint_label`),
  written only when set.

- [ ] **Step 1: Write the failing tests.**
  - `Terminal_test.cpp` (`[terminal][hintmode][hintlabels]`), with contour-dark set on the terminal
    (`defaultColorPalettes("contour-dark", palette)`) unless stated:
    - `Terminal.hintLabels.useTheCursorColourAndDimTheRest`: `"see https://example.com now"`, hint mode on: the label
      cell's background `0xE8C26E`, foreground `0x1C1E24`; the `s` of "see" has foreground
      `mixColor(0xD9DCE3, 0x1C1E24, 0.55f)`.
    - `Terminal.hintLabels.aTypedPrefixTurnsTheLabelsToTheSelection`: enough URLs for two-character labels (as
      `HintModeHandler.LabelAssignment.TwoChar` writes them), the first label character typed: every label cell on
      screen has background `0x3A4A6B` and foreground `0xD9DCE3`, both characters.
    - `Terminal.hintLabels.otherSchemesKeepTheirLook`: the default palette: label `0xFFCC00`, a typed character
      `mixColor(0xFFCC00, bg, 0.5f)`, an unmatched cell blended by 0.5 — today's values.
    - `Terminal.hintLabels.aLabelOverAWideCharacterIsNarrowCells` (Review Focus 5): a hint-only user pattern `日本\S*`
      (built as `LinkPatterns.userPatternIsAppendedAndHintOnly` builds one) over `"日本/notes.txt"`, and a label over a
      `OSC 66` scaled cell: every cell carrying a label character has `width == 1` and `sizing == GlyphSizing {}`; the
      wide head's continuation column draws no glyph; the label reads whole across the cells.
  - `ColorPalette_test.cpp` (`[ColorPalette]`): `ColorPalette.hintLabels.contourSchemesReadAtAA` — label and typed pairs
    of contour-dark and contour-light reach 4.5:1; `contour` and `contour-classic` leave `hintLabelTyped` empty and the
    opacity 0.5.
  - `Config_test.cpp` (`[config]`): a scheme with `hint_label_typed` loads it; one without leaves it empty; the writer
    round-trips it.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test contour_gui_test`. Expected: compile error, no `hintLabelTyped`.
- [ ] **Step 3: Implement** the fields, the scheme lines, the overlay changes and the key.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[hintmode],[ColorPalette],[screen]"
out/appleclang-debug/src/contour/contour_gui_test "[config]"
```

  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/core/ColorPalette.cpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_test.cpp src/vtbackend/core/ColorPalette_test.cpp src/contour/config/Config.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/core/ColorPalette.hpp src/vtbackend/core/ColorPalette.cpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_test.cpp src/vtbackend/core/ColorPalette_test.cpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp
git add src/vtbackend/core/ColorPalette.hpp src/vtbackend/core/ColorPalette.cpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_test.cpp src/vtbackend/core/ColorPalette_test.cpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp
git commit -m "vtbackend: hint labels take the cursor colour, dim the rest to 45 % and turn to the selection once typed" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 12: The size indicator is a Toast pill on each pane

**Files:**
- Modify: `src/contour/qml/Toast.qml` (keep 3c's `textFormat` line if it has landed), `src/contour/qml/TerminalPane.qml`
  (`sizeWidget`, `updateSizeWidget` and the size `Connections`, `:129-170`, `:240-252`, go), `src/contour/session/TerminalSession.hpp`
  (`showResizeIndicator` `:203`, `:517` go), `src/contour/test/QmlMocks.hpp` (`MockSession`: `sizeIndicatorText`,
  `sizeIndicatorRequested`, `sizeIndicatorSettled` recorded; `showResizeIndicator` and `upTime` go),
  `scripts/qml-size-literal-baseline.txt` (`TerminalPane.qml`).
- Test: `src/contour/test/QmlComponents_test.cpp` (the Toast case `:6270`; new `[contour][gui][qml][sizeindicator]`),
  `src/contour/test/Accessibility_qml_test.cpp`.

**Interfaces:**
- Consumes: Tasks 6, 9.
- Produces (QML):
  - `Toast.qml` gains `property real extent: chromeStyle.size.toast`, `property real cornerRadius: chromeStyle.radii.toast`,
    `property real inset: chromeStyle.space.s7`, `property font textFont: chromeStyle.type.footnote` and
    `property string detail` (a trailing `Label`, `objectName: "toastDetail"`, `colors.toastTextSecondary`, visible when
    non-empty). Its height, radius, paddings and both labels' fonts read them; every existing use is unchanged.
  - `TerminalPane.qml`: a `Toast` (`objectName: "sizeIndicator"`, `anchors.centerIn: parent`, `z` above the dim, `text:
    session.sizeIndicatorText`, `textFont` = `type.body` at `Font.DemiBold` with `features: { "tnum": 1 }`,
    `Accessible.ignored: true`) shown while `held && fits`, where `fits` is `implicitWidth <= pane.width − 2 · space.s4`;
    a `Timer` (`interval: chromeStyle.motion.linger`) restarted on `sizeIndicatorRequested` sets `held` false and calls
    `session.sizeIndicatorSettled(pane.paneActive)`.
- **TUI row:** the same file under ContourTui: Toast's Inverse style draws a one-row strip of the scheme's foreground,
  centred on the pane.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][sizeindicator]`, MacOS row unless stated):
  - "a resize shows the pill centred on its pane and settles after the hold": emitting `sizeIndicatorRequested` with
    `"100 × 30"` → `sizeIndicator.shown`, centred (`x + width / 2 == pane.width / 2`), height 36, radius 18, its font
    `tnum`; after `motion.linger` (`QTest::qWait`) not shown and `sizeIndicatorSettled(true)` recorded once; a second
    request before the hold ends restarts it (one settle).
  - "every pane of a split shows its own size indicator" (Review Focus 1): a three-pane split, all three sessions
    requested: three pills, each centred on its own pane with its own text; only the active one settles with `true`;
    a pane 60 px wide shows none.
  - "the Terminal row draws the indicator as one inverse row" (`ContourTui`, `ChromeSetup { .row = Terminal }`):
    height `cellHeight`, radius 0.
  - The Toast case at `:6270`: `extent`, `cornerRadius` and `detail` override the defaults; `toastDetail` hidden when
    empty.
  - `Accessibility_qml_test.cpp`: the size indicator is ignored; the Toast's detail label is part of its name.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][sizeindicator]"`. Expected: FAIL, `sizeIndicator` not found.
- [ ] **Step 3: Implement** the Toast properties, the pill and the mock; regenerate the baseline line.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][sizeindicator],[contour][gui][qml][kit][toast],[contour][gui][qml][a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][sizeindicator],[contour][gui][qml][kit][toast]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/session/TerminalSession.hpp src/contour/test/QmlMocks.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/Toast.qml src/contour/qml/TerminalPane.qml scripts/qml-size-literal-baseline.txt
git add src/contour/qml/Toast.qml src/contour/qml/TerminalPane.qml src/contour/session/TerminalSession.hpp src/contour/test/QmlMocks.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp scripts/qml-size-literal-baseline.txt
git commit -m "contour: the size indicator is a Toast pill on each resized pane, reading 100 × 30 or 13 pt" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 13: The overlay scrollbar draws from tokens, and the Terminal row's `┃` column

**Files:**
- Modify: `src/contour/qml/SessionChrome.qml` (the `ScrollBar`'s own properties `:135-194`, `contentItem` `:195-205`,
  `background` `:207-212`; the tick layer `:215-283` stays), `scripts/qml-size-literal-baseline.txt` (`SessionChrome.qml`).
- Test: `src/contour/test/QmlComponents_test.cpp` (`:1958`, `:2056`, `[contour][gui][qml][scrollbar]`).

**Interfaces:**
- Consumes: Tasks 4, 9; Step 2's `colorOn`.
- Produces (QML, `verticalScrollBar`):
  - `implicitWidth: size.scrollbar + space.scrollbarInset`, the inset as padding on the pane-edge side (so the hit area
    reaches the edge), `padding` 0 elsewhere; `visible` as today (`session.isScrollbarVisible && hasScrollback`).
  - Native `contentItem`: radius `radii.scrollbar`, colour `colorOn(ScrollbarThumb, session.backgroundColor)`, or
    `ScrollbarThumbActive` while hovered or pressed; `background`: `colorOn(ScrollbarTrack, …)` while hovered or
    pressed, else transparent. The luminance-picked white or black `handleColor` and its literals go.
  - Terminal row (`chromeStyle.scrollThumbGlyph !== ""`): the `contentItem` is a column of that glyph, one per cell row
    of its height, in `colors.scrollbarThumb` (`scrollbarThumbActive` while pressed); no track.
- **TUI row:** one cell wide, `┃` in the scheme's bright black, accent while dragged.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][scrollbar]`):
  - "the scrollbar takes its extents and colours from the tokens": MacOS row over contour-dark's background: the bar's
    width is `12`, its handle 8 wide with radius 4, the handle's colour equals `colorOn(ScrollbarThumb, bg)` and, hovered,
    `colorOn(ScrollbarThumbActive, bg)`; over contour-light's background the thumb is dark.
  - "an overlay bar hidden at the bottom takes no presses": the mock's `isScrollbarVisible` false → the bar invisible and
    a press at the pane's right edge reaches the item below.
  - "the Terminal row draws the bar as a ┃ column" (`ContourTui`, `ChromeSetup { .row = Terminal }`): width `cellWidth`,
    the handle's children are `"┃"` texts, one per cell row of the handle.
  - The existing `:1958` and `:2056` cases pass unchanged (`implicitWidth >= 12`, the single-`x` placement).
- [ ] **Step 2: Run them and see them fail.** Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][scrollbar]"`. Expected: FAIL, the handle is white.
- [ ] **Step 3: Implement** the bar; regenerate the baseline line.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][scrollbar],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][scrollbar]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/SessionChrome.qml scripts/qml-size-literal-baseline.txt
git add src/contour/qml/SessionChrome.qml scripts/qml-size-literal-baseline.txt src/contour/test/QmlComponents_test.cpp
git commit -m "contour: the overlay scrollbar draws from the tokens, and as a ┃ column on the Terminal row" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 14: The link pill replaces the hyperlink tooltip

> **Depends on Step 2:** `PaneHeader.qml`, `PaneCard.qml` (Step 2 Task 9) and the card's leaf wiring.

**Files:**
- Create: `src/contour/qml/LinkPill.qml` (add to `_qml_files`, `src/contour/CMakeLists.txt`).
- Modify: `src/contour/qml/SessionChrome.qml` (`hyperlinkTip` `:59-91` goes; a `LinkPill`), `src/contour/qml/TerminalPane.qml`
  (forwards `headerLinkSlot` and `headerPresent`), `src/contour/qml/PaneCard.qml`, `src/contour/qml/PaneHeader.qml`
  (`linkPillSlot`), `scripts/qml-size-literal-baseline.txt`.
- Test: `src/contour/test/QmlComponents_test.cpp` (`:4374` becomes the pill's case; new `[contour][gui][qml][linkpill]`),
  `src/contour/test/Accessibility_qml_test.cpp`.

**Interfaces:**
- Consumes: Tasks 9, 10, 12.
- Produces (QML):
  - `LinkPill.qml`, `objectName: "linkPill"`: a `Toast` with `extent: size.linkPill`, `cornerRadius: radii.linkPill`,
    `inset: space.s5`, `text: session.linkTargetText` (elided in the middle to the placement's width), `detail:
    session.linkTargetHint`, `shown: session.linkTargetShown && placement.anchor !== "hidden"`;
    `Accessible.role: Accessible.ToolTip`, `Accessible.name` the target and the hint.
    `readonly property var placement: { session.overlayClearanceRevision; return chromeStyle.linkPillPlacement(header, chrome.width, chrome.height, implicitWidth, height, session.overlayClearance()) }`;
    `x`, `y`, `width` from it on the grid; with `"headerTrailing"`, `parent: headerSlot`, anchored to its trailing edge.
  - `PaneHeader.qml`: `readonly property alias linkPillSlot: linkPillSlotItem` (`objectName: "paneHeaderLinkSlot"`, at
    the trailing edge, full header height) and `property bool linkPillActive: false` (QML carve-out): while true,
    `paneHeaderStatus` and `paneHeaderActions` are hidden.
  - `PaneCard.qml` → `TerminalPane { headerLinkSlot: paneHeader.linkPillSlot; headerPresent: paneHeader.visible ? 1 : 0 }`;
    `TerminalPane` and `SessionChrome` forward `property Item headerLinkSlot` and `property int headerPresent` (3b's
    name, reused if present).
- **TUI row:** one inverse cell row, in the header row of a split, or top right over blank cells of a single pane.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][linkpill]`):
  - "there is no hyperlink tooltip": no item named `hyperlinkTooltip` exists.
  - "a single pane's pill sits top right over blank cells": an 800 × 400 chrome, the mock's target
    `"github.com › contour-terminal"`, hint `"⌘-click to open"`, shown, an all-blank clearance: `linkPill` visible at the
    top right (`y == 8`, `x + width == 790`), height 22, radius 11, `toastDetail.text == "⌘-click to open"`.
  - "the pill moves off a prompt row and hides over a full-screen app" (Review Focus 2, 3): clearance `avoidRows [0]`
    → `y > chrome.height / 2`; every `trailingBlank` 0 → not visible, no QML warning (`QmlMessageCapture`).
  - "a split's pill sits in the pane header": MacOS row, two panes: the pill's parent is that card's
    `paneHeaderLinkSlot`, the terminal's geometry unchanged, `paneHeaderActions` hidden while it shows, back after.
  - "not shown means not visible": `linkTargetShown` false with a target → invisible.
  - "the Terminal row draws the pill in cells" (`ContourTui`): height `cellHeight`, radius 0.
  - `Accessibility_qml_test.cpp`: `LinkPill.qml` declares `ToolTip` and a name holding the target.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][linkpill]"`. Expected: FAIL, `linkPill` not found.
- [ ] **Step 3: Implement** the pill, the slot and the forwarding; regenerate the baseline line.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][linkpill],[contour][gui][qml][panecard],[contour][gui][qml][a11y],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][linkpill],[contour][gui][qml][panecard]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/LinkPill.qml src/contour/qml/SessionChrome.qml src/contour/qml/TerminalPane.qml src/contour/qml/PaneCard.qml src/contour/qml/PaneHeader.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt
git add src/contour/qml/LinkPill.qml src/contour/qml/SessionChrome.qml src/contour/qml/TerminalPane.qml src/contour/qml/PaneCard.qml src/contour/qml/PaneHeader.qml src/contour/CMakeLists.txt scripts/qml-size-literal-baseline.txt src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
git commit -m "contour: a held follow modifier shows the link's target in a pill in the pane header or over blank cells" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 15: Motion: the hold, the fades, and Reduce Motion

**Files:**
- Modify: `src/contour/qml/SessionChrome.qml` (the bar's fades), `src/contour/qml/TerminalPane.qml`, `src/contour/qml/LinkPill.qml`.
- Test: `src/contour/test/QmlComponents_test.cpp` (`[contour][gui][qml][motion]`).

**Interfaces:**
- Consumes: Tasks 12–14.
- Produces (QML): the scrollbar shows and hides by `opacity` over `motion.standard` (`visible` follows `opacity > 0`, so
  a hidden bar takes no press); its thumb changes colour over `motion.fast` (today's two `duration: 120` go); the size
  pill and the link pill fade and slide as `Toast` does; the hold stays `motion.linger` under every preference.

- [ ] **Step 1: Write the failing tests** (`[contour][gui][qml][motion]`):
  - "the overlay bar fades out over the standard duration": after `isScrollbarVisible` goes false the bar's opacity
    animates (`Behavior` duration `== motion.standard`) and `visible` is false once it reaches 0.
  - "Reduce Motion cross-fades and keeps the hold" (`ChromeSetup { .accessibility = { .motion = Reduced } }`): the size
    pill's `enterOffset == 0`, the bar's fade 100 ms, the size pill's `Timer.interval == 600`.
  - "no animation duration is a literal": `SessionChrome.qml`, `TerminalPane.qml`, `LinkPill.qml` and `Toast.qml` are at
    their regenerated size baselines, which count `duration`.
- [ ] **Step 2: Run them and see them fail.** Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][motion]"`. Expected: FAIL, the bar hides at once.
- [ ] **Step 3: Implement** the Behaviors.
- [ ] **Step 4: Run them and see them pass.**

```bash
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][motion],[contour][gui][qml][scrollbar],[contour][gui][qml][sizeindicator],[contour][gui][qml][linkpill]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/SessionChrome.qml src/contour/qml/TerminalPane.qml src/contour/qml/LinkPill.qml scripts/qml-size-literal-baseline.txt
git add src/contour/qml/SessionChrome.qml src/contour/qml/TerminalPane.qml src/contour/qml/LinkPill.qml scripts/qml-size-literal-baseline.txt src/contour/test/QmlComponents_test.cpp
git commit -m "contour: the overlays fade on the motion tokens, cross-fading under Reduce Motion without shortening the hold" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 16: Snapshot rows, and the Terminal row's pass

> **Depends on Steps 4a, 1c, 1d and 2:** every existing row names their fields; the new rows name them too, at their
> defaults. If 3b or 3c landed first, their rows and fields (`.find`, …) are named as well.

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` (`SnapshotScript`, `ScriptAction`, `SnapshotScriptRow`,
  `SnapshotScripts`), `src/contour/test/ChromeSnapshotScene.hpp` (the mock session's state per action).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp`, `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Produces: `ScriptAction` gains `ShowSizeIndicator`, `HoldLinkModifier`, `ScrollBack`. Rows appended in this order:
  `SizeIndicator` (`"size-indicator"`, `SplitThree`, `ShowSizeIndicator`: every pane `"100 × 30"`-style text of its own
  size), `LinkPill` (`"link-pill"`, `SingleLeaf`, `HoldLinkModifier`: target `"github.com › contour-terminal › contour › issues › 2175"`,
  hint from the row's shortcut style, an all-blank clearance), `LinkPillSplit` (`"link-pill-split"`, `SplitThree`,
  `HoldLinkModifier`), `ScrollbarOverlay` (`"scrollbar-overlay"`, `SingleLeaf`, `ScrollBack`: history, offset 20, a
  `Failure` and a `Running` tick).

- [ ] **Step 1: Write the failing tests.**
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`): `scriptRow(SnapshotScript::LinkPillSplit).panes == PaneShape::SplitThree`;
    every row names an action; the enumerator-order `static_assert` holds; counts from `SnapshotScripts.size()`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): each new script builds on the MacOS and Terminal rows
    without a QML warning; `size-indicator` finds three shown `sizeIndicator`s; `link-pill-split` finds `linkPill` inside
    a `paneHeaderLinkSlot`; `scrollbar-overlay` finds `verticalScrollBar` visible.
- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`. Expected: compile error, no `SnapshotScript::SizeIndicator`.
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
git commit -m "contour: snapshot the size indicator, the link pill on a pane and in a header, and the overlay scrollbar" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 17: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, so the plan's own
  commit is left out. Apply its findings, re-run the filters of every task it touches, and commit once with the
  Signed-off-by line, staging only the files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[scrollbar-marks],[terminal][overlayclearance],[hintmode],[ColorPalette],[screen]"
out/appleclang-debug/src/contour/contour_test "[scrollbarmode],[scrollbarvisibility],[sizeindicator],[linkpill],[chrometokens],[uistyle],[contour][palette],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[config],[appearance-source],[contour][session][scrollbar],[contour][session][scrollbar-marks],[contour][session][sizeindicator],[contour][session][links],[contour][session][hyperlink],[contour][session][a11y],[contour][hyperlink],[contour][uistyle],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][sizeindicator],[contour][gui][qml][scrollbar],[contour][gui][qml][linkpill],[contour][gui][qml][panecard],[contour][gui][qml][kit][toast],[contour][gui][qml][snapshot]"
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
  `ChromeSnapshotPlan.hpp`. Look at `size-indicator.*` (three pills, each centred on its pane; one inverse row under
  ContourTui), `link-pill.*` (22 pt pill top right with the dimmer hint), `link-pill-split.*` (in the active card's
  header, its actions hidden), `scrollbar-overlay.*` (8 pt bar, inset 4, ticks visible; `┃` column under ContourTui);
  `single-tab.*` and `split-three.*` look as they did after Step 2.
- [ ] **Step 4: Owner manual checks.** Batched per phase (ruling): list them in the ledger for the owner; do not wait on
  them. On this Mac, `out/appleclang-debug/src/contour/contour.app`, with a fresh (Native2026) config:
  - Drag the window edge: "100 × 30"-style pills appear on every pane of a split and fade 600 ms after the drag stops;
    VoiceOver says "100 columns, 30 rows" once, for the active pane. ⌘+ shows "13 pt" and keeps it through the resize.
  - System Settings › Appearance › Show scroll bars: "When scrolling" — the bar is hidden at the bottom and appears when
    scrolled back; after `false` exits, it stays visible at the bottom; "Always" — it shows at the bottom at once, without
    restarting Contour. Plug in or unplug a mouse under "Automatically" and watch it follow.
  - `echo https://github.com/contour-terminal/contour/issues/2175`, hover with ⌘ held: links light up and the pill
    reads "github.com › contour-terminal › contour › issues › 2175 ⌘-click to open" at the top right; `clear` then type
    that URL at the prompt and hover it: the pill is never on the prompt row. In `htop`: no pill, ⌘-click still opens.
    In a split: the pill sits in the pane header.
  - A Classic config (`defaults_edition: classic`): the scrollbar shows at the bottom as before; hovering a link shows
    the pill without ⌘; hint mode colours are unchanged.
  - Hint mode (⇧⌃U) under contour-dark: labels in the cursor's gold, everything else dimmed; type a first letter of a
    two-letter label: the labels turn to the selection blue. A path starting with a CJK name labels cleanly.
  - `ui_style: terminal`: the size pill is one inverse row, the scrollbar is a `┃` column, the pill one inverse row.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Shows the size indicator as a pill centred on each resized pane (\"100 × 30\", or \"13 pt\" while zooming the
    font), announced once to screen readers when it settles."
  - "Makes the scrollbar an overlay that follows the system's scroll bar setting and hides at the bottom of the
    scrollback unless a failed or running command or a mark lies above; adds `scrollbar.mode` (`system`, `overlay`,
    `always`)."
  - "Replaces the hyperlink tooltip with a target pill shown while the follow modifier is held, placed in the pane
    header or over blank cells and never on the prompt row; `hyperlink_hover_tooltip` now shows it on plain hover and
    is off in the new defaults."
  - "Draws hint-mode labels in the cursor colour under contour-dark and contour-light, dims everything else, and turns
    the labels to the selection colour once a prefix is typed; adds the `hint_label_typed` scheme key."
- [ ] **Step 6:** Mark Step 3e done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-3e.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the parser or render paths beyond the hint overlay's two extra field reads per label cell;
    the overlay clearance reads the page's rows (`trimmedColumns`, O(columns) each) once per frame and only while the
    pill shows; the scrollbar decision is a table lookup on existing refreshes. Callgrind on `bench-headless`, before and
    after, recorded in the ledger.
  - **Risk:** medium. The tooltip's behaviour changes for every user (it becomes the pill); the scrollbar's visibility
    has a new rule set and a live OS input; Classic keeps its look through the edition table. Mitigated by the
    visibility table, the edition fixtures, the session cases and the QML property tests on the native and Terminal rows.
  - **Coverage:** `[scrollbarmode]`, `[scrollbarvisibility]`, `[sizeindicator]`, `[linkpill]`, `[overlayclearance]`,
    `[hintlabels]` tables; the session scrollbar, size-indicator and link-target cases; the Cocoa scroller notification;
    the QML cases on both rows; four new snapshot scripts.

---

## Owner decisions

The run proceeds on each recommended default.

1. **What `hyperlink_hover_tooltip` means once the tooltip is gone (R-e).** *Default: `true` shows the target pill on
   plain hover, `false` only while the follow modifier is held; Classic `true`, Native2026 `false`.* The alternative
   retires the key and keeps a tooltip for Classic, which leaves two components for one job.
2. **Which marks keep the overlay scrollbar at the bottom (R-b).** *Default: failures, running commands and vi marks;
   a successful command's tick does not.* Read literally, any tick holds it, and a shell-integrated session never hides
   its bar.
3. **Where the mode comes from, and Classic's mode (R-a).** *Default: a `scrollbar.mode: system | overlay | always`
   profile key, `system` in Native2026 and `always` in Classic.* The alternative has no key: Overlay everywhere, the
   system ignored, and Classic's look changing.
4. **Hint colours only in contour-dark and contour-light (R-i).** *Default: yes, as 3b did for search yellow; every
   other scheme keeps its labels and its 50 % dim.* The alternative restyles every scheme from its own cursor and
   selection colours, changing Classic's look.
5. **The pill with nowhere blank to go (R-g).** *Default: no pill; the link still lights up and opens.* The
   alternatives put the target in the status line or the title band.
6. **⇧⌘-click "reveal in Finder" and block-relative paths (Moments 6).** *Default: deferred; not in §3.11.* The
   alternative adds a `RevealLink` action and an `IFileRevealer` in this step.

## Spec gaps found

- §3.11's citations moved slightly (current lines in the Spec paragraph); the tooltip is `:59-91`, not `:64-90`.
- §3.11 gives no source for the scrollbar mode while the canvas says "follows the system", and no Classic value
  (owner decision 3); Windows' auto-hide and Linux's settings have no backend until Phase 5.
- "Marks keep it visible" conflicts with the canvas's "hidden at the bottom" for ordinary command ticks (owner
  decision 2).
- The size indicator's announcement, 600 ms hold, cross-fade and per-pane behaviour are on the canvas only; nothing
  says what a font zoom announces ("13 points" chosen) or what happens in a pane narrower than the pill (none shown).
  The board draws the pill 32 / 16; 1a's Toast is 36 / 18.
- The link pill's 22 pt size, breadcrumb format, hint text, "never on the prompt row", ⇧⌘-click and block-relative
  paths are canvas-only; §3.11 does not define "prompt row", say what happens without blank cells (owner decision 5),
  or what `hyperlink_hover_tooltip` means afterwards (owner decision 1). The board's 50 % hint fails 4.5:1 over a light
  pane (R-h).
- "Non-matches dim to 45 %" changes today's 50 %, and "a typed prefix switches the label" replaces today's dimmed typed
  characters; §2.7 needs both scoped to the Contour schemes (owner decision 4). The canvas's "HINT · Open" status-line
  text is not 3e's.
- The canvas's "one Tooltip component, 600 ms, never over the prompt row" is not in §3.11; `TabItem.qml`'s tooltip
  stays and the block tooltip is 3f's.
- Fluent and ContourLinux have no boards for any of the four surfaces; their rows reuse the macOS extents.
- Search-match scrollbar ticks and the tick restyle are §3.12's (3f).
