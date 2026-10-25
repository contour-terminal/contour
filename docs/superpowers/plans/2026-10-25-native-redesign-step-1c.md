# Native Redesign — Step 1c (Title Band, Tab Well, Chips) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead of Step 4a (pipelined). Line numbers are at HEAD `9811812d`, before 4a. Where 4a moves code, a
task locates it by name and carries a **Depends on Step 4a** note. A pre-flight scan re-verifies every
reference once 4a has landed.

**Goal:** The window's top bar becomes the canvas's title band. On macOS it holds a well of pill tabs. On
every row it gets one badge slot per tab, a HostChip, the daemon drift chip, the tab colour tint, and
correct behaviour without a title bar and in full screen. Windows and Linux keep today's geometry.

**Architecture:**
- **Pure decisions first, Qt-free, in `window/`, tested in `contour_test`.**
  - `TabBadge.hpp` decides which badge wins.
  - `HostChipState.hpp` decides what the chip says.
  - `TitleBandLayout.hpp` decides where the band, the tabs and the centred title go.
  - `TabWellLayout.hpp` decides how wide a pill is and when the labels give way.
  - `TabAttention.hpp` records unseen output and finished commands until a tab is seen.
- **One layout switch.** `layout.tabShape` ∈ `TabShape { Flat = 0, Pill }` is §1.2's "tab shape". It is
  `Pill` only on the MacOS row. `Flat` keeps today's strip, so the Fluent and ContourLinux rows keep their
  geometry (§2.1) and the Terminal row keeps its one-cell row.
- **Tokens, not literals.** New extents, radii, colour roles and motion values are rows of
  `config/ChromeTokenGroups.hpp`, copied from the canvas. The provider resolves them and exposes the
  pure decisions as `Q_INVOKABLE`s, so QML renders their answers and decides nothing.
- **Same roles, renamed.** The three OSC 7501 roles keep their numbers and become the badge slot's roles.
  The controller fills them from `resolveTabBadge`.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls, Quick Shapes), QML, Catch2 v3, Python 3 (lint),
CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.4 (`:403-444`)
binds, with what it cites:
- §1, principles (`:28-64`);
- §2.1, token rows (`:68-126`), including the native-rows acceptance test at `:107-110`;
- §2.3, accessibility (`:139-167`);
- §2.5 and §2.6, snapshots and testing (`:187-226`).

The design record is `docs/superpowers/specs/2026-10-23-native-redesign-design.md` (Coverage additions:
Daemon and remote; TUI row; "no title bar and full screen").

The canvas is <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>. Its local copies are in
`.superpowers/design/canvas/project/`: `kit-tokens`, `kit-components`, `mac-dark`, `mac-light`,
`mac-density`, `daemon`, `tui`, `surfaces`, `win`, `linux`. The plans index is
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Binding rulings come from
`docs/superpowers/ledgers/*.md`, in particular R15 in the Phase 0 ledger: "the TabLabel foreground-process
fallback moves to Step 1c".

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

- **The spec, verbatim (§3.4):**
  - "the tab colour tint (20 % on the pill, 100 % on its icon)";
  - "Working uses `StatusColorRole::Running` (the accent), replacing the green";
  - "`HostChip.qml` replaces `TabStrip.qml`'s `hostBadge` … one of `Connected = 0`, `Reconnecting` or
    `Disconnected`. The title keeps `RemoteChrome::remoteWindowTitle`";
  - "`DaemonDriftBadge` moves … into the band's trailing area. It reads the same `DaemonDriftModel`, and
    its confirmation goes through `KitDialog`";
  - "With `show_title_bar` … set to true, the native server frame is used: no band, and the tabs sit in a
    strip below the frame, as today. In full screen …, the band reveals when the pointer reaches the top
    edge". The `Config.hpp` lines are now `:1188` (`showTitleBar`) and `:1200` (`fullscreen`).
  - Acceptance: "The macOS row matches the canvas tokens. The Fluent and ContourLinux rows keep today's
    geometry. The badge priority table holds, and so does the `HostChipState` table."
  - TUI row: "The tab row is one cell high, with `│` separators. Badge glyphs replace the pixel badges.
    On macOS, AppKit's traffic lights are snapped to the first cells. Linux CSD and Windows draw `● ● ●`
    cells from the scheme."
- **Canvas token values.** These are exact; Task 4 copies them into the rows. The MacOS column is from
  `kit-components`, `kit-tokens`, `mac-dark`, `mac-light` and `daemon`. The Fluent and ContourLinux
  values are from `kit-tokens`, `win` and `linux`, recorded but unread by the `Flat` shape in 1c.

  | Token | MacOS | Fluent | ContourLinux | Terminal (cells) |
  |---|---|---|---|---|
  | `size.titleBar` / `tabWell` / `tab` (exist) | 38 / 28 / 24 | 40 / 0 / 34 | 46 / 0 / 34 | 1 / 1 / 1 |
  | `size.tabIcon` | 14 | 14 | 14 | 1 |
  | `size.tabClose` | 16 | 16 | 16 | 1 |
  | `size.badge` (disc) / `size.badgeDot` | 14 / 6 | 14 / 6 | 14 / 6 | 1 / 1 |
  | `size.progressBar` | 2 | 2 | 2 | 1 |
  | `size.hostChip` / `size.hostDot` | 24 / 7 | 24 / 7 | 24 / 7 | 1 / 1 |
  | `size.toolbarButton` | 28 | 32 | 34 | 2 |
  | `size.toolbarChip` | 22 | 22 | 22 | 1 |
  | `space.dragReserve` | 40 | 40 | 40 | 4 |
  | `space.revealEdge` | 2 | 2 | 2 | 1 |
  | `space.toolbarChipPadding` | 9 | 9 | 9 | 1 |
  | `radius.tab` / `radius.tabWell` | 12 / 14 | 6 / 0 | 8 / 0 | 0 / 0 |
  | `radius.tabIcon` / `radius.tabClose` / `radius.badge` | 3 / 8 / 7 | 3 / 8 / 7 | 3 / 8 / 7 | 0 / 0 / 0 |
  | `radius.hostChip` / `radius.toolbarChip` | 12 / 11 | 12 / 11 | 12 / 11 | 0 / 0 |
  | `motion.spinnerMs` / `motion.pulseMs` | 1200 / 700 | 1200 / 700 | 1200 / 700 | 1200 / 700 |
  | `tint.tabPercent` | 20 | 20 | 20 | 0 (the `▌` cell instead) |
  | `layout.tabShape` | `Pill` | `Flat` | `Flat` | `Flat` |
  | `layout.profileMenu` | `SecondaryClick` | `Caret` | `Caret` | `Caret` |

  - The pill's spacing uses the existing scale (`s1` = 2 … `s10` = 32):
    - well padding and gap between pills: `s1` (2);
    - icon-to-title gap: `s3` (6);
    - pill leading inset: `s5` (10), trailing inset `s4` (8);
    - band gap: `s5` (10), band trailing inset `s4` (8);
    - chip padding: `s5` (10);
    - progress bar side inset: `s5` (10), bottom inset `s1` (2).
  - The pill's width range is the existing `minTabWidth`/`maxTabWidth` (120–240, the kit's "width
    120–240").
  - The kit's alpha tables (`kit-tokens`, on a light / a dark chrome):
    - `fill.1–4`: black 20/12/8/5 % · white 24/16/10/6 %;
    - `label.primary/secondary/tertiary`: black 88/60/45 % · white 92/62/50 %;
    - `surface.elevated`: `#FFFFFF` / `#2C2C2E`, and `surface.secondary`: `#F2F2F7` / `#1C1C1E`.
  - The rest of the kit's states: rest · hover (`fill.3`) · pressed (`fill.2`); the inactive window takes
    "labels drop one level".
- **Binding rulings** (ledgers 0, 1a and 1b):
  - Status colours are tokens. Running is the accent, never green or yellow.
  - No colour or size literal in chrome QML: `check_qml_colors`, `check_qml_sizes`. A new file's baseline
    is 0, and a file's baseline only goes down.
  - Every visual surface is a snapshot row, and every visual task renders the Terminal row.
  - Reduce Motion is honoured.
  - Contrast is at least 4.5:1 for text.
  - `enum class` over `bool`, data-driven tables.
  - TestApp is Classic (R14) and passes `nullAppearanceServices()`.
- **AGENT.md applies in full.**
  - Data-driven: badge priority, badge looks, the chip states, the band rules and the
    `CommandEnding` → finished-badge map are each a table.
  - `enum class` with zero as the default; new QML-facing `Q_PROPERTY`/`QVariantMap` booleans are the
    documented carve-out.
  - Doxygen on every new public declaration; `.hpp` with `#pragma once`.
  - Zero warnings and no `NOLINT`. `slots`, `signals` and `emit` are Qt macros: never use them as
    identifiers.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already set in
  `out/appleclang-debug/CMakeCache.txt`): `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`.
  Never run two builds at once.
- **Tests:** each task runs only its own filter. The full suite runs in CI on Linux, macOS and Windows.
- **Format** every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i` (CI's version).
  Check every new or touched `.qml`, `.py`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`.
  **Never clang-format a `.qml` file.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`.** Every new struct member gets `{}`
  or a value, and every row of a table names every field. That covers `ChromeTokenRows`, the
  `ChromeColorSource` literals, `SnapshotScripts`, `TabBadgeRules`, `TabBadgeLooks`, `HostChipRows`,
  `HostChipLooks` and `TitleBandRules`.
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Counts in tests are derived from the tables** (`TabBadgeKindCount`, `SnapshotScripts.size()`,
  `HostChipRows.size()`), never written as literals.
- **QML:** enums are unreachable through the `chromeStyle` context property. QML compares the `int` or
  name-string values the provider publishes, as `windowControls.side === "leading"` does. A `Popup` cannot
  carry `Accessible`: put it on an `Item`. Every new control sets `Accessible.role` and `Accessible.name`.
- **Qt-free targets stay Qt-free:** the five new `window/*.hpp` decisions include no Qt header, and their
  tests are in `contour_test`.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, and never carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 8).

## Review Focus

1. **A long tab title in the well.** A program that sets a 200-character title must not widen the pill
   past 240, push the `+` off the band, or eat the drag region. Expected: the title elides with "…", the
   pill stays within the range, the full title stays in the tooltip and in `Accessible.name`, and at least
   `space.dragReserve` (40) of the band still drags the window. Pinned in Task 3 (`layoutTabWell` never
   exceeds `maxTab`) and Task 6 ("a long title elides inside the pill").
2. **Many tabs overflow the well.** Expected:
   - 20 tabs in a 900-pt window shrink to equal widths, then go icon-only.
   - Past icon-only width, the well scrolls with the active tab kept in view.
   - The window stays draggable, and the `+` stays reachable.

   Pinned in Task 3 (the overflow rows) and Task 6 ("twenty tabs go icon-only and keep the active one in
   view").
3. **A remote session reconnects while its tab is inactive.**
   - The mirror's resync repaints every background pane. It must not mark every background tab as having
     new output.
   - A Blocked badge earned before the drop must survive it.
   - The chip must read "Reconnecting…" and then return to connected.

   Pinned in Task 2 (the state table), Task 5 ("a resync marks no unseen output"; "the chip follows the
   connection") and Task 7 (the chip's text per state).
4. **Full screen with Reduce Motion.**
   - The band reveals from the top edge with a 100 ms cross-fade and no slide.
   - The badge spinner and the progress pulse stand still.
   - The revealed band never resizes the terminal (`chromeHeight` excludes it).

   Pinned in Task 4 (`MotionStyle::CrossFade` under `MotionPreference::Reduced`), Task 6 (the spinner and
   the pulse stop) and Task 7 ("full screen reveals the band without resizing the content").
5. **The TUI row under a light scheme.** In contour-light, `palette[0]` (the bar) is `#2B2E35`, a dark bar
   in a light scheme.
   - The selected tab's text must read on the selection colour.
   - Blue (`#1F5FBF`) or green badge glyphs on that bar fail 4.5:1. They must fall back to the bar's text
     colour, which keeps the glyph shape meaningful.

   Pinned in Task 4 (`SchemeSlot::SelectionText`; `ColorSourceKind::SchemeOnBar` against every bundled
   scheme) and Task 6 (the ContourTui pass of `tab-badges` and the contrast case).

Also pinned, beyond the five:
- The Fluent and ContourLinux rows keep today's geometry (Task 6, the §2.1 acceptance test).
- A tab colour's 20 % tint keeps the label at 4.5:1 for every swatch of the palette (Task 4).
- A finished command's ✓ or ✕ clears once its tab is seen, and so does the output dot, but only when the
  tab's window is not minimised (Task 5).
- With one tab under `Multiple`, the macOS window keeps a 38-pt band (Task 7).

---

## Spec resolutions (read before Task 1)

These settle where the spec is silent, or where the code or the canvas disagrees with it. Those marked
**(owner)** are repeated under *Owner decisions*, and the run proceeds on their default.

- **R-a. Where the decisions live.** All five pure headers are in `src/contour/window/`, Qt-free (§1.3),
  and tested in `contour_test` beside `ProgramStatusBadge_test.cpp` (`src/contour/CMakeLists.txt:1188`).
  `contour_test` already links `vthost` and `vtbackend`, which `HostChipState.hpp` (`ConnectionState`)
  and `TabBadge.hpp` (`CommandEnding`) need.
- **R-b. The badge inputs have no source today.**
  - §3.4 names `TabActivity::UnseenOutput` and `FinishedBlockBadge`, but nothing tracks unseen output or
    a finished command per tab.
  - `TabAttention` (Task 5) records them per session. It is fed by the manager on the GUI thread:
    - output arrives through the post that `TerminalSession::screenUpdated` already makes
      (`TerminalSession.cpp:893-898`);
    - finishes arrive through `handleCommandBlockFinished` (`:2266`).
  - Both are marked only while the session is `platform::SessionVisibility::Hidden`.
  - A session is *seen* when its visibility stops being `Hidden`: its tab is activated, or its window is
    restored from minimised. That clears both marks, which is "cleared when the tab is seen".
  - A remote session's output while its host connection is not `Connected` is `OutputOrigin::Resync`.
    So is its first output after it returns to `Connected`, which is the mirror's resync. Neither marks
    anything (Review Focus 3).
- **R-c. The roles are the existing ones, renamed.** `WindowController::Roles` (`WindowController.hpp:140-162`)
  keeps its numbers:
  - `ProgramStatusRole` becomes `BadgeRole` (`"badge"`, a `TabBadgeKind`);
  - `ProgramStatusTextRole` becomes `BadgeTextRole` (`"badgeText"`): the OSC 7501 tooltip when the
    program status won, else empty;
  - `ProgramStatusProgressRole` becomes `BadgeProgressRole` (`"badgeProgress"`, -1 for none).

  That honours "rendered … with the existing roles" and keeps the meaning honest. The mirrors in
  `MockMainController.hpp:87-101`, `QmlComponents_test.cpp:190-239` and `TabListModel_test.cpp:61-84` are
  renamed with it.
- **R-d. What reaches every row, and what is MacOS-only.**
  - On every row:
    - the badge decision, and its inks (Working and progress are the accent);
    - the HostChip, replacing `hostBadge`;
    - the drift chip, in the bar's trailing area;
    - the full-screen reveal;
    - the tab-label fallback.
  - Only under `TabShape::Pill`:
    - the well and the pills;
    - the tab icon and its 20 % tint;
    - close-on-hover and equal widths;
    - the band's trailing `+` toolbar button;
    - the centred single-tab title.
  - `Flat` keeps `TabStrip`'s layout, the WT-style colour fill (`TabItem.qml:104-116`) and an always-shown
    close button.
  - On the Terminal row the colour becomes a leading `▌` cell (`surfaces`: "In the TUI row the color
    becomes the tab's leading ▌ cell"). That is a glyph token, empty on the native rows, which the QML
    branches on as it does `trafficLightGlyph`.
- **R-e. The band decision (`resolveTitleBand`, Task 3).** These are rules of one table:
  - `NativeFrame` (`show_title_bar: true`): no band. The strip shows per the tab-bar gate at its
    configured edge, as today.
  - `ClientBand` with `Flat`: the band is the strip, shown per the gate, at its edge. This is today's
    behaviour.
  - `ClientBand` with `Pill` and `Top`: the band is always shown, because it carries AppKit's traffic
    lights. Its centre holds the tabs when the gate is shown, else the active session's title
    (`mac-light`: "With one tab the bar shows a centered title … 13 medium, label.secondary").
    **(owner, 1)**
  - `ClientBand` with `Pill` and `Bottom`: the band shows the title at the top, and a second bar of the
    band's height holds the well at the bottom. **(owner, 6)**
  - `FullScreen`: a band that would be shown at the top becomes `RevealOnEdge` on every row
    (`surfaces`: "Windows and Linux show it on hover too"). A bottom bar is unchanged.
- **R-f. The well's widths (`layoutTabWell`, Task 3).**
  - Pills are equal width: `min(maxTab, share)`, where `share` is the available width less padding and
    gaps, divided by the count.
  - Below `minTab` the labels give way (`TabLabelMode::IconOnly`).
  - Below the icon-only width (`s5 + tabIcon + s5` = 34), pills stay at 34 and the well scrolls
    (`TabWellOverflow::Scrolls`), keeping the active tab in view.
  - "Available" is the band's width less the leading inset, the HostChip, the drift chip, the toolbar,
    the band gaps and `space.dragReserve` (`mac-dark`: "at least 40 pt of empty bar always stays
    draggable").
  - In icon-only mode, close shows on hover only, and a badge other than `None` is drawn in place of the
    icon. The canvas is silent on this; the badge is more urgent than the icon, and the pill's tint still
    carries the colour.
- **R-g. The tab icon is the kit's tinted square (owner, 4).** `kit-components` draws it as a
  rounded square (`radius.tabIcon` 3), filled with the tab colour at 100 %, and with `label.tertiary` when
  the tab has none. Per-process icons need SF Symbols and a foreground-process poll, which exists today only
  while an extension follows it (`TerminalSession.cpp:1245-1261`).
- **R-h. Chrome colours come from the kit's tables.**
  - New `ColorSourceKind`s:
    - `Fill`, `Label` and `Surface` read a `level` from the kit tables in Global Constraints, black on a
      light chrome and white on a dark one. "Light" is decided as `statusColor` decides it: the
      application palette's window luminance (`UiStyleProvider.cpp:243-259`).
    - `SchemeOnBar` takes a scheme slot's hue where it reaches 4.5:1 on `Bar`, else `BarText` (R-i).
  - The band's own fill stays `PaletteWindow` (today's `TitleBar.qml:60-63`). The material and the
    terminal-background tint are Step 1d's (§3.5).
  - The kit's `surface.elevated` dark value is `#2C2C2E`; the `mac-dark` board draws `#3A3C43`. The token
    board wins, as the one QML reads.
- **R-i. The Terminal row stays legible on a light scheme (Review Focus 5).**
  - A new `SchemeSlot::SelectionText` takes whichever of foreground and background reads better on
    `Selection`, as `BarText` does for `Bar` (1a's `159b47dc`).
  - The row's status inks are `SchemeOnBar` sources.
- **R-j. Motion.**
  - `MotionTokens` gains `MotionStyle style` (`Full = 0, CrossFade`), which is `CrossFade` under
    `MotionPreference::Reduced`.
  - The badge spinner (today a literal 1200 ms, `TabItem.qml:377`) and the progress pulse (700 ms,
    `:208-209`) run only under `Full` (`tui`: "Reduce Motion stops the ◑ spinner").
  - The band reveal slides by its height over `motion.standard` under `Full`. Under `CrossFade` it fades
    over `motion.standard`, which is then 100 ms.
  - The tab reorder transition (`TabStrip.qml:139-141`, 120 ms) takes `motion.standard`, which is
    "palette, find bar, tab reorder".
- **R-k. Full screen (owner, 3).**
  - An `Item` of `space.revealEdge` height along the top edge reveals the band while the pointer is on it
    or on the band.
  - The band overlays the top rows and never pushes the content. A transient, pointer-driven reveal is
    the macOS behaviour, and resizing the terminal on every reveal would reflow it.
  - On macOS, the system menu bar slides over the same edge. Placing the band below it is the safe-area
    question that Step 1d's spike owns (§3.5).
- **R-l. HostChip (owner, 2).**
  - There are three states. `ConnectionState::Connecting` reads as `Reconnecting`, and `Failed`,
    `Refused` and `Stopped` read as `Disconnected`.
  - The chip is not interactive: it is `Accessible.StaticText`, as `hostBadge` is today.
  - It has no Reconnect button, because nothing can reconnect today: `NativeController` has no restart
    API. The `mac-light` "click for the session list" also has no source.
  - The `daemon` and `ssh` second words have no source either, so the chip shows the host label and the
    state text.
- **R-m. The drift chip.**
  - `DaemonDriftBadge.qml` keeps its name, its `objectName` and its model. It becomes a toolbar chip
    (`size.toolbarChip`, `radius.toolbarChip`, `space.toolbarChipPadding`, `colors.chipFill`/`chipText`).
  - It sits in the bar's trailing area, before the `+` on `Pill` and before the window controls on
    `Flat`.
  - `ConfirmDialog` gives way to `KitDialog`, with the heading `actionLabel`, the message `confirmation`,
    accept "Restart daemon" and reject "Cancel".
- **R-n. The new-tab profile menu (owner, 5).** `layout.profileMenu` is the kit's "profile dropdown"
  switch.
  - `SecondaryClick` (MacOS): the band has one `+`, and a right-click or press-and-hold on it opens the
    new-tab menu (`surfaces`: "on macOS, a secondary click or a long press on '+' (no caret)").
  - `Caret` keeps today's `▾`.
- **R-o. The tab label falls back to the process name (R15; owner, 7).**
  - An expanded label that is empty becomes the session's foreground process name. That name is read
    only then: one `vtpty::Process::foregroundProcessName()` call on the GUI thread.
  - A remote session has no `Process` and stays empty.
  - The fallback is not edition-gated: an empty tab label is a defect, not a look.
- **R-p. Snapshots.** New `SnapshotScripts` rows:
  - Task 6: `tab-badges`, `tab-overflow` and `tab-color-flyout`;
  - Task 7: `remote-connected`, `remote-disconnected`, `daemon-drift`, `native-title-bar` and
    `full-screen-reveal`.

  `single-tab` changes picture on the MacOS row (the centred title). Every row names the new fields.
- **R-q. TUI traffic lights on macOS.**
  - The horizontal inset `nativeControlsInset` is rounded up to whole cells (`chromeStyle.snapToCells`).
  - Moving the buttons vertically onto the first cell row needs `standardWindowButton` frames, which is
    Step 1d's safe-area spike. Until then, AppKit places them.

### Non-goals

- The title band's material, the terminal-background tint and the safe area (Step 1d).
- Densities: Minimal's 28-pt band (Step 2).
- The pane reconnecting overlay's restyle, which the design record lists under Daemon and remote (Step 2,
  pane states).
- Per-process tab icons, and SF Symbols (R-g).
- A Reconnect action, and the host chip's session list (R-l).
- The keyboard focus ring on toolbar buttons (Phase 6.8, keyboard to chrome).

---

### Task 1: `TabBadge.hpp` — which badge a tab shows, and how it looks

**Files:**
- Create: `src/contour/window/TabBadge.hpp`
- Test: `src/contour/window/TabBadge_test.cpp`, added to `contour_test` after `window/RemoteChrome_test.cpp`
  (`src/contour/CMakeLists.txt:1189`).

**Interfaces:**
- Consumes: `window::BadgeKind` (`ProgramStatusBadge.hpp:30-37`), `config::StatusColorRole` (`UiStyle.hpp:232-238`),
  `vtbackend::CommandEnding` (`vtbackend/shell/CommandBlock.hpp:266-274`).
- Produces (namespace `contour::window`), the spec's types verbatim plus their tables:
  - `enum class TabActivity : uint8_t { None = 0, UnseenOutput };`
  - `enum class FinishedBlockBadge : uint8_t { None = 0, Succeeded, Failed };`
  - `enum class TabBadgeKind : uint8_t { None = 0, Output, Done, Working, Error, Blocked };` and
    `inline constexpr size_t TabBadgeKindCount` derived from `Blocked`.
  - `struct TabBadgeInputs { BadgeKind programStatus = BadgeKind::None; std::optional<int> progressPercent {}; FinishedBlockBadge finishedBlock = FinishedBlockBadge::None; TabActivity activity = TabActivity::None; };`
  - `struct TabBadge { TabBadgeKind kind = TabBadgeKind::None; std::optional<int> progressPercent {}; bool operator==(TabBadge const&) const = default; };`
  - `struct TabBadgeRule { TabBadgeKind kind; BadgeKind programStatus; FinishedBlockBadge finishedBlock; TabActivity activity; };`
    A row matches when any of its three non-`None` fields equals the input's.
  - `inline constexpr auto TabBadgeRules = std::array { … };`, most urgent first:

    | Rule | `programStatus` | `finishedBlock` | `activity` |
    |---|---|---|---|
    | `Blocked` | `Blocked` | `None` | `None` |
    | `Error` | `Error` | `Failed` | `None` |
    | `Working` | `Working` | `None` | `None` |
    | `Done` | `Done` | `Succeeded` | `None` |
    | `Output` | `None` | `None` | `UnseenOutput` |

  - `[[nodiscard]] constexpr TabBadge resolveTabBadge(TabBadgeInputs const& inputs) noexcept;` returns the
    first matching rule's kind. `progressPercent` is passed through only for `Working`.
  - `enum class BadgeMark : uint8_t { None = 0, Dot, Ring, Disc, Glyph };` says how the `Pill` shape draws a
    kind. `Flat` always draws the glyph.
  - `struct TabBadgeLook { TabBadgeKind kind; std::string_view glyph; BadgeMark mark; config::StatusColorRole ink; std::string_view accessibleName; };`
  - `inline constexpr std::array<TabBadgeLook, TabBadgeKindCount> TabBadgeLooks`, indexed by kind:

    | Kind | Glyph | Mark | Ink | Name |
    |---|---|---|---|---|
    | `None` | `""` | `None` | `Running` | `""` |
    | `Output` | `"•"` | `Dot` | `Running` | `"New output"` |
    | `Done` | `"✓"` | `Glyph` | `Success` | `"Done"` |
    | `Working` | `"◑"` | `Ring` | `Running` | `"Working"` |
    | `Error` | `"✕"` | `Glyph` | `Danger` | `"Failed"` |
    | `Blocked` | `"!"` | `Disc` | `Warning` | `"Waiting for you"` |

    These come from the `daemon` and `tui` boards. The names are English source text, as the 1b menu
    tables' titles are.
  - `[[nodiscard]] constexpr TabBadgeLook const& tabBadgeLook(TabBadgeKind kind) noexcept;`
  - `[[nodiscard]] constexpr std::string_view badgeMarkName(BadgeMark mark) noexcept;` gives `"none"`,
    `"dot"`, `"ring"`, `"disc"` or `"glyph"`, for QML (Task 4).
  - `[[nodiscard]] constexpr FinishedBlockBadge finishedBlockBadgeOf(vtbackend::CommandEnding ending) noexcept;`
    reads a `FinishedBlockRows` table:
    - `Succeeded` → `Succeeded`;
    - `Failed`, `Killed` and `Crashed` → `Failed`;
    - `Unknown` and `Interrupted` → `None` (no verdict, or the user's own ⌃C).
  - `[[nodiscard]] constexpr FinishedBlockBadge strongerFinishedBlock(FinishedBlockBadge a, FinishedBlockBadge b) noexcept;`
    orders `Failed` > `Succeeded` > `None`.

- [ ] **Step 1: Write the failing tests** (`[tabbadge]`).
  - `tabBadge.priority` builds the inputs where every source competes:
    - `{ .programStatus = BadgeKind::Blocked, .finishedBlock = Failed, .activity = UnseenOutput }` →
      `Blocked`;
    - `{ BadgeKind::Working, 40, Failed, UnseenOutput }` → `Error` (a failed command outranks work);
    - `{ BadgeKind::Working, 40, None, UnseenOutput }` → `TabBadge { Working, 40 }`;
    - `{ BadgeKind::None, std::nullopt, Succeeded, UnseenOutput }` → `Done`;
    - `{ BadgeKind::Done, … }` → `Done`;
    - `{ .activity = UnseenOutput }` → `Output`;
    - `{}` → `None`.
  - `tabBadge.progressOnlyForWorking`: `{ BadgeKind::Done, 40 }` gives `progressPercent == std::nullopt`.
  - `tabBadge.everyKindHasALook`: over `std::views::iota(size_t { 0 }, TabBadgeKindCount)`, each
    `TabBadgeLooks[i].kind == TabBadgeKind(i)`. Every kind but `None` has a non-empty glyph and name.
    `tabBadgeLook(Working).ink == StatusColorRole::Running` (never green, ruling).
  - `tabBadge.rulesCoverEveryKind`: every kind but `None` appears in `TabBadgeRules` exactly once, and
    `TabBadgeRules.front().kind == Blocked`.
  - `tabBadge.finishedBlockOfEveryEnding`: one row per `CommandEnding` enumerator, with the mapping above.
    `strongerFinishedBlock(Succeeded, Failed) == Failed`, and `strongerFinishedBlock(None, Succeeded) == Succeeded`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `window/TabBadge.hpp` not found.
- [ ] **Step 3: Implement** `TabBadge.hpp` as the Interfaces block states. The rule matcher is a
  `std::ranges::find_if` over `TabBadgeRules`. `static_assert` that `TabBadgeLooks` lists the kinds in
  enumerator order, as `SnapshotScripts` does (`ChromeSnapshotPlan.hpp:222-228`).
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[tabbadge],[contour][window][programstatus]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/TabBadge.hpp src/contour/window/TabBadge_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/window/TabBadge.hpp src/contour/window/TabBadge_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: decide a tab's badge slot from one priority table" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `HostChipState.hpp` — what the host chip says

**Files:**
- Create: `src/contour/window/HostChipState.hpp`
- Test: `src/contour/window/HostChipState_test.cpp` (in `contour_test`, beside Task 1's).

**Interfaces:**
- Consumes: `vthost::client::ConnectionState` (`src/vthost/client/ConnectionState.hpp:15-23`), `config::StatusColorRole`.
- Produces (namespace `contour::window`):
  - `enum class HostChipState : uint8_t { Connected = 0, Reconnecting, Disconnected };` (§3.4 verbatim).
  - `inline constexpr size_t HostChipStateCount`, derived from `Disconnected`.
  - `struct HostChipRow { vthost::client::ConnectionState connection; HostChipState state; };`
  - `inline constexpr auto HostChipRows`, one row per `ConnectionState` in enumerator order:
    - `Connecting` → `Reconnecting`;
    - `Connected` → `Connected`;
    - `Reconnecting` → `Reconnecting`;
    - `Failed`, `Refused` and `Stopped` → `Disconnected`.
  - `[[nodiscard]] constexpr HostChipState hostChipStateOf(vthost::client::ConnectionState connection) noexcept;`
  - `enum class HostChipPresence : uint8_t { Hidden = 0, Shown };` and
    `[[nodiscard]] constexpr HostChipPresence hostChipPresence(std::string_view hostLabel) noexcept;`
    A local window, with an empty label, shows no chip.
  - `struct HostChipLook { HostChipState state; config::StatusColorRole dot; std::string_view status; std::string_view accessibleState; };`
  - `inline constexpr std::array<HostChipLook, HostChipStateCount> HostChipLooks`:
    - `Connected`: dot `Success`, status `""`, accessible state `"connected"`;
    - `Reconnecting`: dot `Warning`, status `"Reconnecting…"`, accessible state `"reconnecting"`;
    - `Disconnected`: dot `Danger`, status `"Disconnected"`, accessible state `"disconnected"`.

    This is `daemon`: "a dot plus a label: connected (success) · Reconnecting… (warning) · 'Disconnected …'".
    The button is left out (R-l).
  - `[[nodiscard]] constexpr HostChipLook const& hostChipLook(HostChipState state) noexcept;`

- [ ] **Step 1: Write the failing tests** (`[hostchip]`).
  - `hostChip.everyConnectionStateHasARow`: over `std::views::iota(size_t { 0 }, HostChipRows.size())`,
    `HostChipRows[i].connection == ConnectionState(i)`. `HostChipRows.size()` equals
    `std::to_underlying(ConnectionState::Stopped) + 1`.
  - `hostChip.stateTable` is the table above, one `CHECK` per `ConnectionState`.
  - `hostChip.reconnectRoundTrip`: `Connected` → `Reconnecting` → `Connected` reads `Connected`,
    `Reconnecting`, `Connected`; `Reconnecting` → `Refused` reads `Disconnected`.
  - `hostChip.looks`: each look's `state` is its index. `hostChipLook(Reconnecting).dot == StatusColorRole::Warning`,
    and its status is `"Reconnecting…"` (U+2026). `hostChipLook(Connected).status.empty()`.
  - `hostChip.presence`: `hostChipPresence("") == Hidden` and `hostChipPresence("build-01") == Shown`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile error, `HostChipState.hpp` not found.
- [ ] **Step 3: Implement** as stated. `static_assert` that the rows and the looks are in enumerator order.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[hostchip],[contour][hosts]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/HostChipState.hpp src/contour/window/HostChipState_test.cpp
git add src/contour/window/HostChipState.hpp src/contour/window/HostChipState_test.cpp src/contour/CMakeLists.txt
git commit -m "contour: map a host connection onto the host chip's three states" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: `TitleBandLayout.hpp` and `TabWellLayout.hpp` — where the band and the tabs go, and how wide

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp`. Add `TabShape` beside `MenuPresentation` (`:42-47`).
  The enum only; Task 4 adds it to the rows.
- Create: `src/contour/window/TitleBandLayout.hpp`, `src/contour/window/TabWellLayout.hpp`
- Test: `src/contour/window/TitleBandLayout_test.cpp`, `src/contour/window/TabWellLayout_test.cpp` (in `contour_test`).

**Interfaces:**
- Produces (`config/ChromeTokenGroups.hpp`): `enum class TabShape : uint8_t { Flat = 0, Pill };`. `Flat` is
  today's strip, and `Pill` is the well of pills.
- Produces (`window/TitleBandLayout.hpp`, namespace `contour::window`):
  - `enum class WindowFrame : uint8_t { ClientBand = 0, NativeFrame };` mirrors `show_title_bar` false and
    true.
  - `enum class WindowMode : uint8_t { Windowed = 0, FullScreen };`
  - `enum class TabStripGate : uint8_t { Hidden = 0, Shown };` is `config::isTabBarShown`'s answer.
  - `enum class BarPresence : uint8_t { Hidden = 0, Shown, RevealOnEdge };`
  - `enum class BarEdge : uint8_t { Top = 0, Bottom };`
  - `enum class BarCentre : uint8_t { Empty = 0, Tabs, Title };`
  - `struct TitleBandInputs { config::TabShape shape = config::TabShape::Flat; WindowFrame frame = WindowFrame::ClientBand; WindowMode mode = WindowMode::Windowed; TabStripGate strip = TabStripGate::Shown; config::TabBarPosition position = config::TabBarPosition::Top; };`
  - `struct TitleBand { BarPresence band = BarPresence::Hidden; BarEdge bandEdge = BarEdge::Top; BarCentre centre = BarCentre::Empty; BarPresence tabBar = BarPresence::Hidden; bool operator==(TitleBand const&) const = default; };`
    `band` is the bar holding the window's own affordances: on `Flat` and under a native frame, it is the
    strip. `tabBar` is a second bar that holds only the tabs.
  - `[[nodiscard]] constexpr TitleBand resolveTitleBand(TitleBandInputs const& inputs) noexcept;` applies
    R-e's rules. They are a table, `TitleBandRules`, matched on `(shape-or-any, frame, position)`, followed
    by one full-screen step: a `band` that is `Shown` at the `Top` becomes `RevealOnEdge`.
  - `[[nodiscard]] constexpr std::string_view barPresenceName(BarPresence)` gives `"hidden"`, `"shown"` or
    `"reveal"`. `barEdgeName` gives `"top"` or `"bottom"`, and `barCentreName` gives `"empty"`, `"tabs"` or
    `"title"`.
- Produces (`window/TabWellLayout.hpp`):
  - `enum class TabLabelMode : uint8_t { Full = 0, IconOnly };`
  - `enum class TabWellOverflow : uint8_t { Fits = 0, Scrolls };`
  - `struct TabWellMetrics { double minTab = 0; double maxTab = 0; double iconOnlyTab = 0; double gap = 0; double padding = 0; };`
  - `struct TabWellLayout { double tabWidth = 0; double wellWidth = 0; TabLabelMode label = TabLabelMode::Full; TabWellOverflow overflow = TabWellOverflow::Fits; bool operator==(TabWellLayout const&) const = default; };`
  - `[[nodiscard]] constexpr TabWellLayout layoutTabWell(double available, int tabCount, TabWellMetrics const& metrics) noexcept;`
    applies R-f:
    - `share = (max(0, available) − 2·padding − (n−1)·gap) / n`;
    - `tabWidth = min(maxTab, share)` while `share ≥ minTab`;
    - `IconOnly` at `share` while `share ≥ iconOnlyTab`;
    - else `IconOnly` at `iconOnlyTab` with `Scrolls`;
    - `wellWidth = min(max(0, available), 2·padding + n·tabWidth + (n−1)·gap)`;
    - `n == 0` gives the default value.
  - `[[nodiscard]] constexpr std::string_view tabLabelModeName(TabLabelMode)` gives `"full"` or
    `"iconOnly"`.

- [ ] **Step 1: Write the failing tests.**
  - `titleBand.rules` (`[titleband]`) is a table of `{ inputs, expected }` rows, one per R-e rule, each
    under both gates and both edges where the rule reads them:
    - `{ Pill, ClientBand, Windowed, Hidden, Top }` → `{ Shown, Top, Title, Hidden }` (one tab under
      `Multiple`);
    - `{ Pill, ClientBand, Windowed, Shown, Top }` → `{ Shown, Top, Tabs, Hidden }`;
    - `{ Pill, ClientBand, Windowed, Shown, Bottom }` → `{ Shown, Top, Title, Shown }`;
    - `{ Pill, ClientBand, Windowed, Hidden, Bottom }` → `{ Shown, Top, Title, Hidden }`;
    - `{ Flat, ClientBand, Windowed, Shown, Bottom }` → `{ Shown, Bottom, Tabs, Hidden }` (today);
    - `{ Flat, ClientBand, Windowed, Hidden, Top }` → `{ Hidden, Top, Tabs, Hidden }` (today);
    - `{ Pill, NativeFrame, Windowed, Shown, Top }` → `{ Shown, Top, Tabs, Hidden }`;
    - `{ Pill, NativeFrame, Windowed, Hidden, Top }` → `{ Hidden, Top, Tabs, Hidden }`.
  - `titleBand.fullScreenReveals` (`[titleband]`):
    - every row above with `mode = FullScreen` gives the same answer with a `Shown` band at the `Top` turned
      into `RevealOnEdge`;
    - a `Bottom` band stays `Shown`, as does `tabBar`.
  - `titleBand.names`: each name function covers every enumerator.
  - `tabWell.widths` (`[tabwell]`) uses `metrics = { .minTab = 120, .maxTab = 240, .iconOnlyTab = 34, .gap = 2, .padding = 2 }`:
    - `(1000, 3)` → `{ 240, 728, Full, Fits }`;
    - `(600, 4)` → `{ 147.5, 600, Full, Fits }`;
    - `(600, 6)` → `IconOnly` at `(600 − 4 − 10) / 6`, `Fits`;
    - `(300, 12)` → `{ 34, 300, IconOnly, Scrolls }`;
    - `(-50, 2)` → `{ 34, 0, IconOnly, Scrolls }`;
    - `(500, 0)` → `TabWellLayout {}`.
  - `tabWell.neverPastTheRange` (Review Focus 1): for `n` in `iota(1, 41)` and available widths in
    `{ 0, 120, 480, 900, 1600, 4000 }`:
    - `tabWidth ≤ maxTab`;
    - `wellWidth ≤ max(0, available)`;
    - `label == Full` implies `tabWidth ≥ minTab`.
  - `tabWell.overflow` (Review Focus 2): `(900 − 40 (drag reserve), 20)` is `IconOnly` and `Fits`, and
    `(900 − 40, 30)` is `Scrolls`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`

  Expected: compile errors on the new headers.
- [ ] **Step 3: Implement** both headers and the enum.
- [ ] **Step 4: Run them and see them pass.**

  Run: `out/appleclang-debug/src/contour/contour_test "[titleband],[tabwell],[chrometokens]"`

  Expected: PASS.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/window/TitleBandLayout*.{hpp,cpp} src/contour/window/TabWellLayout*.{hpp,cpp}
git add src/contour/config/ChromeTokenGroups.hpp src/contour/window/TitleBandLayout* src/contour/window/TabWellLayout* src/contour/CMakeLists.txt
git commit -m "contour: decide the title band, the tab bar and the well's widths from tables" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: The band's tokens, colour roles and provider answers

> **Depends on Step 4a:** 4a adds the Settings window's four `SizeTokens` rows and their entries in
> `numbersOf` (`ChromeTokenGroups_test.cpp:46-73`). Add this task's fields after them, by name.

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp`:
  - `SizeTokens` (`:114-127`), `SpaceTokens` (`:130-138`), `RadiusTokens` (`:141-150`) and `MotionTokens`
    (`:165-173`) get the new fields. A new `TintTokens` group, and `LayoutTokens` (`:223-229`) gains
    `tabShape` and `profileMenu`.
  - `ChromeColorRole` (`:50-58`), `ColorSourceKind` (`:61-68`) and `SchemeSlot` (`:71-83`) get their new
    enumerators. `ChromeColorSource` (`:92-101`) gains `level`, and `ChromeTokenRow` (`:232-246`) gains
    `tint` and `statusInk`.
  - The four rows (`:320-419`) and `resolveChromeTokens` (`:437-465`) are extended, and so is
    `chromeSchemeColorsOf` (`:470-500`).
- Modify: `src/contour/config/UiStyle.hpp`:
  - drop `progressColors` (`:202-209`, `:347-353`, `:422-428`) for a `ProgressStatusRoles` table;
  - add four glyphs beside `tabSeparator` (`:218-225`, `:357-362`, `:431-437`).
- Modify: `src/contour/window/ChromeTokenObjects.{hpp,cpp}` (the groups at `:35-46`, `:79-94`, `:131-138`,
  `:198-204`, `:372-377`, `:406-414`; colour resolution at `.cpp:101-121`).
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}`: `progressColor` (`.cpp:231-241`), plus the new
  invokables and the glyph properties.
- Test: `src/contour/config/ChromeTokenGroups_test.cpp`, `src/contour/config/UiStyle_test.cpp`,
  `src/contour/window/UiStyleProvider_test.cpp`.

**Interfaces:**
- Consumes: Tasks 1–3 (`tabBadgeLook`, `badgeMarkName`, `hostChipLook`, `resolveTitleBand` and the name
  functions, `layoutTabWell`, `TabShape`).
- Produces (`config`), with values as in Global Constraints:
  - `SizeTokens` gains `tabIcon`, `tabClose`, `badge`, `badgeDot`, `progressBar`, `hostChip`, `hostDot`,
    `toolbarButton` and `toolbarChip`, each `int … = 0;`.
  - `SpaceTokens` gains `dragReserve`, `revealEdge` and `toolbarChipPadding`.
  - `RadiusTokens` gains `tab`, `tabWell`, `tabIcon`, `tabClose`, `badge`, `hostChip` and `toolbarChip`.
  - `MotionTokens` gains `int spinnerMs = 0; int pulseMs = 0; MotionStyle style = MotionStyle::Full;`, with
    `enum class MotionStyle : uint8_t { Full = 0, CrossFade };`. `resolveChromeTokens` sets
    `style = CrossFade` under `MotionPreference::Reduced`.
  - `struct TintTokens { int tabPercent = 0; };` becomes `ChromeTokenRow::tint`.
  - `LayoutTokens` gains `TabShape tabShape = TabShape::Flat;` and
    `ProfileMenuAffordance profileMenu = ProfileMenuAffordance::Caret;`, with
    `enum class ProfileMenuAffordance : uint8_t { Caret = 0, SecondaryClick };`.
  - `ChromeColorRole` gains these roles after `SwatchRing`:
    - `TabWellFill`, `TabActiveFill`, `TabHoverFill`, `TabCloseFill`, `ControlPressedFill`;
    - `TabText`, `TabTextActive`, `TabTextDimmed`;
    - `ChipFill`, `ChipText`, `ChipTextStrong`.

    `ChromeColorRoleCount` is derived from `ChipTextStrong`.
  - `ColorSourceKind` gains `Fill`, `Label`, `Surface` and `SchemeOnBar`. `ChromeColorSource` gains
    `int level = 0;`, the 1-based step of a `Fill` (1–4), `Label` (1–3) or `Surface` (1 elevated,
    2 secondary). It is 0 for the other kinds, and every existing literal names `.level = 0`. Add the
    `detail::fill(level)`, `detail::label(level)` and `detail::surface(level)` factories beside
    `schemeColor` (`:311-318`), plus `detail::schemeOnBar(slot)`.
  - `struct AlphaPair { int light = 0; int dark = 0; };`, with
    `inline constexpr std::array<AlphaPair, 4> FillAlphas` and `std::array<AlphaPair, 3> LabelAlphas`.
    `struct SurfacePair { uint32_t light = 0; uint32_t dark = 0; };` with
    `std::array<SurfacePair, 2> SurfaceColors`. All values are from Global Constraints.
  - The native rows' new colours:
    - `TabWellFill = fill(4)`, `TabActiveFill = surface(1)`, `TabHoverFill = fill(3)`,
      `TabCloseFill = fill(3)`, `ControlPressedFill = fill(2)`;
    - `TabText = label(2)`, `TabTextActive = label(1)`, `TabTextDimmed = label(3)`;
    - `ChipFill = fill(4)`, `ChipText = label(2)`, `ChipTextStrong = label(1)`.
  - The Terminal row's new colours:
    - `TabWellFill = Bar`, `TabActiveFill = Selection`, `TabHoverFill = Bar`, `TabCloseFill = Bar`,
      `ControlPressedFill = Selection`;
    - `TabText = BarText`, `TabTextActive = SelectionText`, `TabTextDimmed = BarText` (it does not dim:
      `Secondary` on a light scheme's bar fails 4.5:1);
    - `ChipFill = Bar`, `ChipText = BarText`, `ChipTextStrong = BarText`.
  - `SchemeSlot` gains `SelectionText` after `Selection`. `chromeSchemeColorsOf` picks it the way it picks
    `BarText` (`:480-486`), against the selection colour.
  - `ChromeTokenRow` gains `std::array<ChromeColorSource, StatusColorRoleCount> statusInk {};`, indexed by
    `StatusColorRole`:
    - the native rows use `Status` sources for each role;
    - the Terminal row uses `schemeOnBar(Danger)`, `schemeOnBar(Warning)`, `schemeOnBar(Success)` and
      `schemeOnBar(Accent)`.
  - `[[nodiscard]] inline vtbackend::RGBColor legibleOnBar(ChromeSchemeColors const& colors, SchemeSlot hue) noexcept;`
    gives the hue if `contrastRatio(hue, Bar) ≥ 4.5`, else `BarText`.
- Produces (`config/UiStyle.hpp`):
  - `inline constexpr std::array<StatusColorRole, 5> ProgressStatusRoles`, indexed by
    `vtbackend::ProgressState`: `Running` (unused), `Running`, `Danger`, `Running`, `Warning`. It replaces
    `progressColors`.
  - `UiStyleTokens` gains `std::string_view tabColorGlyph, chipOpenGlyph, chipCloseGlyph, hostDotGlyph;`,
    which are `""` on `NativeTokens` and `"▌"`, `"["`, `"]"`, `"●"` on `TerminalTokens`.
- Produces (QML, through `UiStyleProvider` and `ChromeTokenObjects`):
  - The new extents are `Q_PROPERTY`s of the groups: `chromeStyle.size.tabIcon`, `chromeStyle.space.dragReserve`,
    `chromeStyle.radii.tab`, and so on.
  - `chromeStyle.motion.spinner`, `pulse`, `style`, `full` and `crossFade` are ints.
  - `chromeStyle.tint.tab` is a `qreal` in 0–1.
  - `chromeStyle.layout.tabShape`, `flat`, `pill`, `profileMenu`, `caret` and `secondaryClick` are ints.
  - `chromeStyle.colors.tabWellFill` … `chipTextStrong`.
  - `chromeStyle.tabColorGlyph`, `chipOpenGlyph`, `chipCloseGlyph` and `hostDotGlyph` are `QString`s.
- Produces (`UiStyleProvider` invokables, each `[[nodiscard]] Q_INVOKABLE`):
  - `QColor statusInk(int role) const;` is the row's `statusInk` source. Out of range gives transparent.
  - `QColor progressColor(int state) const;` keeps its signature and now reads
    `statusInk(ProgressStatusRoles[state])`.
  - `QVariantMap badgeLook(int kind) const;` gives `{ glyph, mark (badgeMarkName), ink (statusInk of the
    look), name }`.
  - `QVariantMap hostChipLook(int state) const;` gives `{ dot (statusInk), status, accessibleState }`.
  - `QVariantMap titleBand(int frame, int mode, int strip, int position) const;` runs `resolveTitleBand`
    with the row's `tabShape` and gives `{ band, bandEdge, centre, tabBar }` as their names.
  - `QVariantMap tabWellLayout(qreal available, int tabCount) const;` gives
    `{ tabWidth, wellWidth, label, scrolls }`. The metrics are `minTabWidth`, `maxTabWidth`,
    `2·s5 + size.tabIcon`, `s1` and `s1`. `scrolls` is a QML-facing bool (carve-out).
  - `qreal snapToCells(qreal length) const;` gives `widthQuantum · ceil(length / widthQuantum)` (R-q).

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp` (`[chrometokens]`):
    - `chromeTokens.titleBandTokens`: `resolveChromeTokens(MacOS, {})` has every MacOS value of the Global
      Constraints table, one `CHECK` each. Its `layout.tabShape == TabShape::Pill` and
      `layout.profileMenu == ProfileMenuAffordance::SecondaryClick`. The Terminal row gives
      `size.tabIcon == 1`, `radii.tab == 0` and `tint.tabPercent == 0`.
    - Extend `numbersOf` and `everyRowMatchesTheDesignKit` (`:46-200`) with the new numeric fields, in
      declaration order, and with every row's values.
    - `chromeTokens.onlyTheMacRowIsAPill`: over `ChromeTokenRows`, `tabShape == Pill` exactly for `MacOS`.
    - `chromeTokens.reducedMotionCrossFades`: `style == CrossFade` under `{ .motion = Reduced }`, and
      `Full` otherwise. `spinnerMs` and `pulseMs` are unchanged.
    - `chromeTokens.kitAlphaTables`: `FillAlphas[3] == AlphaPair { 5, 6 }`, `LabelAlphas[1] == AlphaPair { 60, 62 }`,
      and `SurfaceColors[0] == SurfacePair { 0xFFFFFF, 0x2C2C2E }`.
    - `chromeTokens.selectionTextReadsOnTheSelection`: the `barTextReadsOnTheBar` loop (`:339-366`) done for
      `Selection` and `SelectionText` over `defaultColorPaletteNames()`. For contour-light and contour-dark,
      the ratio is at least 4.5.
    - `chromeTokens.statusInkLegibleOnTheBar` (Review Focus 5): for every bundled scheme and each of
      `Accent`, `Danger`, `Success` and `Warning`, `contrastRatio(legibleOnBar(colors, slot), Bar) ≥
      min(4.5, contrastRatio(BarText, Bar))`. For contour-light,
      `legibleOnBar(colors, Accent) == colors BarText`. For contour-dark, it is `normalColor(4)`.
  - `UiStyle_test.cpp` (`[uistyle]`): `UiStyle: progress follows the status roles`.
    `ProgressStatusRoles[1] == StatusColorRole::Running`, `[2] == Danger` and `[4] == Warning`.
  - `UiStyleProvider_test.cpp` (`[contour][uistyle]`):
    - "UiStyleProvider resolves the tab and chip roles on a light and a dark chrome", under an application
      palette whose window is `#F5F5F7` and then `#1E1E1E`:
      - `colors.tabWellFill == platform::atOpacity(QColor(Qt::black), 5)` and then `(Qt::white, 6)`;
      - `tabTextActive` at 88 and then 92;
      - `tabActiveFill == QColor(0xFFFFFF)` and then `QColor(0x2C2C2E)`.
    - "the tab text reads at 4.5:1 on every fill it sits on": on both chromes, over the composite of each
      role on `PaletteWindow`, these are at least 4.5:1:
      - `TabTextActive` on `TabActiveFill`;
      - `TabText` on the well;
      - `ChipText` on `ChipFill`;
      - `TabTextActive` on `TabActiveFill` tinted by every `vtworkspace` tab-palette swatch at
        `tint.tab` (Also pinned, the tint).
    - "progressColor is the accent while running": with an injected accent `#007AFF`,
      `progressColor(1) == QColor(0x007AFF)` and `progressColor(2) == statusColor(Danger)`.
    - "badgeLook, hostChipLook, titleBand and tabWellLayout answer the pure tables":
      - `badgeLook(int(TabBadgeKind::Blocked))["mark"] == "disc"`;
      - `hostChipLook(int(HostChipState::Reconnecting))["status"] == "Reconnecting…"`;
      - on the MacOS row, `titleBand(0, 0, 0, 0)["centre"] == "title"`;
      - on the Fluent row, `titleBand(0, 0, 0, 0)["band"] == "hidden"`;
      - `tabWellLayout(300, 12)["scrolls"] == true`.
    - "snapToCells rounds up to whole cells on the Terminal row": with `widthQuantum` 8,
      `snapToCells(70) == 72` and `snapToCells(72) == 72`. On a native row, `snapToCells(70) == 70`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on the new fields.
- [ ] **Step 3: Implement** the fields, the rows, the factories, `legibleOnBar`, the `SelectionText` slot, the
  `ColorSourceKind` cases in `ChromeColorTokens::color`, the groups' properties and the invokables.
  `progressColor`'s QML callers (`TabItem.qml:199`, `:389`) are unchanged.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[uistyle],[config],[tabbadge],[hostchip],[titleband],[tabwell]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[contour][gui][qml]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS. `UiStyle.nativeRowsKeepTodaysGeometry` (`UiStyle_test.cpp:204`) passes unedited.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp src/contour/config/UiStyle.hpp src/contour/config/UiStyle_test.cpp src/contour/window/ChromeTokenObjects.{hpp,cpp} src/contour/window/UiStyleProvider.{hpp,cpp} src/contour/window/UiStyleProvider_test.cpp
git add src/contour/config src/contour/window
git commit -m "contour: tokens for the title band, the tab well and the chips; progress and Working take the accent" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 5: The controller feeds the badge slot, the host chip and the label fallback

> **Depends on Step 4a:** 4a removes `_settingsActive` from `IsActiveRole` and from the badge's focus
> (`WindowController.cpp:534`, `:590-592`), removes the mock's `settingsActive`/`settingsController`, and
> drops the settings tab from `TabStrip.qml`. Locate `data()`, `programStatusBadge()` and the delegate by
> name.

**Files:**
- Create: `src/contour/window/TabAttention.hpp`, and its test `src/contour/window/TabAttention_test.cpp`
  (in `contour_test`).
- Modify: `src/contour/window/WindowController.hpp`:
  - `Roles` (`:140-162`);
  - the remote chrome block (`:739-754`): the `hostChipState` property, its accessor and the
    `hostChipStateChanged` signal;
  - `cachedProgramStatusBadge` (`:830`): it gains `tabBadge(tab, row)`.
- Modify: `src/contour/window/WindowController.cpp`:
  - `roleNames` (`:495-508`) and `data` (`:552-562`);
  - `resolvedTabLabel` (`:609-634`);
  - `refreshActiveTabHighlight` (`:2043-2055`);
  - `bindWindow`, where the QWindow's `visibilityChanged` now calls the manager's
    `markVisibleSessionsSeen`.
- Modify: `src/contour/window/TabLabel.{hpp,cpp}`, adding `tabLabelWithFallback`.
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}`:
  - `setHostConnection` (`.hpp:812-817`) also stores the state and notifies every controller;
  - `refreshTabProgramStatusForSession` (`.cpp:1518-1529`) becomes `refreshTabBadgeForSession`;
  - `activeTabChanged` (`:1300-1316`) calls `markVisibleSessionsSeen`;
  - session removal forgets the session's attention.
- Modify: `src/contour/session/TerminalSession.{hpp,cpp}`:
  - the post in `screenUpdated` (`.cpp:893-898`) passes its `OutputOrigin`;
  - `handleCommandBlockFinished` (`:2266-2286`) reports the ending first, before the notice policy;
  - `setHostConnection` (`:442-450`) arms the resync grace;
  - add `foregroundProcessName()`.
- Modify: `src/contour/qml/TabStrip.qml` (the delegate's `required property`s, `:104-136`) and
  `src/contour/qml/TabItem.qml`:
  - the badge properties (`:34-59`) are renamed;
  - the glyph and ink read `chromeStyle.badgeLook(root.tabBadge)`;
  - the spinner and the pulse duration read the motion tokens and run only under `motion.full`.
- Modify: `src/contour/test/MockMainController.hpp` (`:87-101`, `:160-182`; a `hostChipState` property
  with a setter), `src/contour/test/MainWindowControllerMock.hpp`, and the mirrors in
  `src/contour/test/QmlComponents_test.cpp:190-239` and `src/contour/window/TabListModel_test.cpp:61-84`.
- Test:
  - `src/contour/window/TabLabel_test.cpp`;
  - `src/contour/session/TerminalSessionManager_test.cpp` (new `[manager][tabbadge]` and
    `[manager][hostchip]` cases);
  - the TabItem cases at `QmlComponents_test.cpp:1542-1700`, which are renamed and retagged
    `[contour][gui][qml][tabbadge]`.

**Interfaces:**
- Consumes: Task 1 (`resolveTabBadge`, `finishedBlockBadgeOf`, `strongerFinishedBlock`), Task 2
  (`hostChipStateOf`), Task 4 (`badgeLook`, `motion.*`).
- Produces (`window/TabAttention.hpp`, Qt-free):
  - `enum class OutputOrigin : uint8_t { Live = 0, Resync };`
  - `enum class SessionPlacement : uint8_t { OnScreen = 0, OffScreen };` is `Hidden` → `OffScreen`. The
    caller converts from `platform::SessionVisibility`.
  - `enum class AttentionChange : uint8_t { Unchanged = 0, Changed };`
  - `struct SessionAttention { TabActivity activity = TabActivity::None; FinishedBlockBadge finished = FinishedBlockBadge::None; bool operator==(SessionAttention const&) const = default; };`
  - `class TabAttention` with these members:
    - `[[nodiscard]] AttentionChange noteOutput(vtworkspace::SessionId, SessionPlacement, OutputOrigin)`
      marks only `OffScreen` and `Live`;
    - `[[nodiscard]] AttentionChange noteFinished(vtworkspace::SessionId, SessionPlacement, vtbackend::CommandEnding)`
      marks only `OffScreen`, keeping the stronger badge;
    - `[[nodiscard]] AttentionChange noteSeen(vtworkspace::SessionId)` clears both;
    - `void forget(vtworkspace::SessionId)`;
    - `[[nodiscard]] SessionAttention of(vtworkspace::SessionId) const`.

    It is not thread-safe, and belongs to the GUI thread alone.
  - `[[nodiscard]] SessionAttention pooledAttention(vtworkspace::Tab const& tab, TabAttention const& ledger);`
    covers every leaf's session: any `UnseenOutput`, and the strongest finished badge.
- Produces (`WindowController`):
  - `Roles::BadgeRole = Qt::UserRole + 10` (`"badge"`), `BadgeTextRole = + 11` (`"badgeText"`) and
    `BadgeProgressRole = + 12` (`"badgeProgress"`) (R-c).
  - `[[nodiscard]] TabBadge tabBadge(vtworkspace::Tab const& tab, int row) const;` gives
    `resolveTabBadge({ program kind, program progress, pooled.finished, pooled.activity })`.
  - `Q_PROPERTY(int hostChipState READ hostChipState NOTIFY hostChipStateChanged)`, with
    `[[nodiscard]] int hostChipState() const noexcept;` and `void notifyHostConnectionChanged();`.
- Produces (`TerminalSessionManager`):
  - `[[nodiscard]] vthost::client::ConnectionState hostConnection() const noexcept;` (initially `Connected`);
  - `void refreshTabBadgeForSession(vtworkspace::SessionId session, window::OutputOrigin origin);`;
  - `void noteCommandFinished(TerminalSession const& session, vtbackend::CommandEnding ending);`;
  - `void markVisibleSessionsSeen(vtworkspace::WindowId window);`.
- Produces (`TerminalSession`): `[[nodiscard]] std::string foregroundProcessName() const;` (GUI thread). It is
  empty when the device is not a `vtpty::Process`. An `enum class ResyncGrace : uint8_t { None = 0, NextOutput };`
  member is armed when the host connection returns to `Connected` from any other state.
- Produces (`TabLabel.hpp`): `[[nodiscard]] std::string tabLabelWithFallback(std::string expanded, std::string_view processName);`
  It gives `expanded` unless that is empty, else `processName`.
- Produces (QML): `TabItem` properties `tabBadge`, `tabBadgeText` and `tabBadgeProgress` replace
  `tabProgramStatus*`. Their `objectName`s are `tabBadgeSlot`, `tabBadgeGlyph` and `tabBadgeSpinner`.

- [ ] **Step 1: Write the failing tests.**
  - `TabAttention_test.cpp` (`[tabattention]`):
    - `OffScreen`/`Live` output marks `UnseenOutput`, and returns `Changed` once and `Unchanged` the second
      time;
    - `OnScreen` output marks nothing;
    - `Resync` marks nothing;
    - `noteFinished(…, OffScreen, Failed)` then `(…, Succeeded)` keeps `Failed`;
    - `noteSeen` clears both;
    - `forget` drops the entry;
    - `pooledAttention` over a two-leaf tab takes `UnseenOutput` from either leaf and `Failed` over
      `Succeeded`.
  - `TabLabel_test.cpp` (`[contour][tablabel]`): `tabLabelWithFallback("", "vim") == "vim"`,
    `tabLabelWithFallback("zsh", "vim") == "zsh"` and `tabLabelWithFallback("", "") == ""`.
  - `TerminalSessionManager_test.cpp`, `[manager][tabbadge]`. Build it as the visibility case does
    (`:799-813`): `TestApp` with `MockPtySessionFactory`, `ScopedControllerWithOsWindow`, and two tabs, the
    second active. Read `window->data(window->index(0), int(Roles::BadgeRole))`, and drain posted events
    with `QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall)`. The sections:
    - "output in a background tab shows the dot, and seeing it clears it": write `"x"` to tab 0's
      terminal and drain, and the role is `Output`. `activateTab(0)` gives `None`.
    - "output in the active tab shows nothing".
    - "a command that fails in a background tab shows ✕ until seen": write `"\033]133;C\007"` then
      `"\033]133;D;1\007"`, and the role is `Error`. After `activateTab(0)` it is `None`. With `D;0` it is
      `Done`.
    - "Blocked outranks output": OSC 7501 blocked (as `TabListModel_test.cpp:108-115` reports it) plus
      output gives `Blocked`.
    - "a minimised window's active tab is not seen" (Also pinned): with the window minimised, output on
      the active tab gives `Output`. Restoring the window (`setWindowStates(Qt::WindowNoState)`) gives
      `None`.
  - `[manager][tabbadge]`, "a resync marks no unseen output" (Review Focus 3), with a remote-hosted test
    session (`SessionHosting::Remote`) in tab 0:
    - `manager.setHostConnection(Reconnecting)`, write output, drain: `None`;
    - `setHostConnection(Connected)`, write output, drain: still `None`;
    - write output again, drain: `Output`;
    - a Blocked status set before the drop still reads `Blocked` after it.
  - `[manager][hostchip]`, "the chip follows the connection": with a `QSignalSpy` on
    `hostChipStateChanged`, `setHostConnection(Reconnecting)` gives
    `window->hostChipState() == int(HostChipState::Reconnecting)` and one signal. `Refused` gives
    `Disconnected`, and `Connected` gives `Connected`.
  - `QmlComponents_test.cpp` `[contour][gui][qml][tabbadge]`. The renamed badge case sets `tabBadge` to
    `int(TabBadgeKind::Blocked)` and expects the glyph `"!"`. Working (`3`) spins without progress and
    stands at 180° with 50. `TabBadgeKindCount + 3` draws nothing.
  - The spinner case gains "and not under Reduce Motion": with the chrome installed from
    `{ .motion = MotionPreference::Reduced }`, the spinner's `running` is false while the window is shown.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test`

  Expected: compile errors on `TabAttention.hpp`, `Roles::BadgeRole` and `hostChipState`.
- [ ] **Step 3: Implement.**
  - The manager owns one `window::TabAttention`. A session's marks follow it between windows, because
    the ledger is keyed by session.
  - `refreshTabBadgeForSession` notes output only when the attention changed or the program status may
    have changed (always, as today), then notifies the three badge roles.
  - `markVisibleSessionsSeen` notes every session of the window whose `sessionVisibility` is not `Hidden`,
    and refreshes the rows that changed.
  - `resolvedTabLabel` passes the expanded label through `tabLabelWithFallback` with the active
    session's `foregroundProcessName()`. It reads the name only when the label is empty.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
out/appleclang-debug/src/contour/contour_test "[tabattention],[tabbadge],[hostchip],[contour][window][programstatus]"
out/appleclang-debug/src/contour/contour_gui_test "[manager],[contour][tablabel],[contour][gui][model],[contour][gui][qml],[contour][multiwindow]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][tabbadge]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS. The size baseline for `TabItem.qml` goes down: the two durations are tokens now.
  Regenerate it with `python3 scripts/check-qml-sizes.py --print-baseline`.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/TabAttention*.{hpp,cpp} src/contour/window/WindowController.{hpp,cpp} src/contour/window/TabLabel.{hpp,cpp} src/contour/window/TabLabel_test.cpp src/contour/window/TabListModel_test.cpp src/contour/session/TerminalSessionManager.{hpp,cpp} src/contour/session/TerminalSessionManager_test.cpp src/contour/session/TerminalSession.{hpp,cpp} src/contour/test/MockMainController.hpp src/contour/test/MainWindowControllerMock.hpp src/contour/test/QmlComponents_test.cpp
editorconfig-checker src/contour/qml/TabItem.qml src/contour/qml/TabStrip.qml scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git add src/contour/window src/contour/session src/contour/test src/contour/qml scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git commit -m "contour: one badge slot per tab, fed by unseen output, finished commands and OSC 7501; a host chip state; a process-name label fallback" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 6: The tab well, the pills, the badge slot and the colour flyout

> **Depends on Step 4a:**
> - 4a deletes `TabStrip.qml`'s settings tab and lowers its size baseline from 3 to 2.
> - `SnapshotScriptRow::page` becomes `surface` (`SnapshotSurface`).
> - `MockMainController` loses its settings members.
>
> Edit those by name.

**Files:**
- Create: `src/contour/qml/TabBadgeSlot.qml`, the kit's badge slot: one component drawing a
  `TabBadgeKind` as a mark (`Pill`) or a glyph (`Flat`). Add it to `_qml_files` after `qml/TabItem.qml`
  (`src/contour/CMakeLists.txt:388`).
- Modify: `src/contour/qml/TabItem.qml`:
  - the tab's natural width (`:61-81`) and its fill (`:104-123`);
  - the progress bar (`:174-212`);
  - the badge (`:353-399`), which moves into `TabBadgeSlot`;
  - the close button (`:504-534`);
  - a new icon square, and the `▌` cell.
- Modify: `src/contour/qml/TabStrip.qml`: the list (`:84-209`) becomes the well under `Pill`, the
  reorder transition (`:139-141`) moves, and `hostBadge` (`:55-82`) and the new-tab buttons (`:272-321`)
  are gated. The gates are `Flat`-only for the buttons; HostChip itself is Task 7.
- Modify: `src/contour/qml/TabColorFlyout.qml`: its 12 size literals become tokens (`radii.popover`,
  `size.swatchWidth`/`swatchHeight`, `radii.control`, `colors.swatchRing`, `space.s4`).
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` (enums `:35-89`, row `:97-108`, table `:111-220`),
  `src/contour/test/ChromeSnapshotScene.hpp` and `src/contour/test/MockMainController.hpp`. The
  `MainWindowScene` gains `TabDecor decor` (`enum class TabDecor : uint8_t { Plain = 0, EveryBadge, LongTitle }`).
  Under `EveryBadge`, the rows are:
  - row 0: active, coloured `#E74856`;
  - rows 1–5: `Output`, `Working` at 40 %, `Blocked`, `Done` and `Error`.
- Modify: `scripts/qml-size-literal-baseline.txt` (`TabColorFlyout.qml`, `TabItem.qml` and `TabStrip.qml`
  drop to their new counts).
- Test:
  - `src/contour/test/MainWindowQml_test.cpp` (new `[contour][gui][qml][mainwindow][tabwell]` cases);
  - `src/contour/test/QmlComponents_test.cpp` (`[contour][gui][qml][tabbadge]`);
  - `src/contour/test/Accessibility_qml_test.cpp` (the kit table at `:180-215`);
  - `src/contour/test/ChromeSnapshotPlan_test.cpp`;
  - `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Consumes: Task 4's tokens and invokables, and Task 5's roles and `tabBadge*` properties.
- Produces (QML `objectName`s, for tests):
  - `tabWell`, the well's background `Rectangle`, visible only under `Pill`;
  - `tabIcon`, `tabColorCell` (the `▌` `Text`), `tabCloseButton` (kept), `tabLabel` (kept),
    `progressTrack` (kept) and `tabBadgeSlot`;
  - `TabBadgeSlot` has `required property int kind` and `property int progress: -1`;
  - inside it, `tabBadgeMark` (an `Item` whose `mark` string is the look's), `tabBadgeGlyph` and
    `tabBadgeSpinner`.
- Produces (`TabItem`):
  - `readonly property bool pill: chromeStyle.layout.tabShape === chromeStyle.layout.pill`, the QML-facing
    carve-out;
  - `property string labelMode: "full"`, which TabStrip assigns from `tabWellLayout(…).label`.
- Produces (`TabStrip`): `readonly property var wellLayout: chromeStyle.tabWellLayout(availableWidth, list.count)`,
  with `property real availableWidth` assigned by `TitleBar` in Task 7. Until then it is bound to the
  strip's own width.
- Produces (snapshots): `SnapshotScript::TabBadges`, `TabOverflow` and `TabColorFlyout`.
  `ScriptAction::OpenTabColorFlyout` opens the second tab's flyout. `tab-overflow` has 20 tabs and the
  20th active. Every row gains `.decor` (`TabDecor::Plain` on the existing rows).

- [ ] **Step 1: Write the failing tests.**
  - `MainWindowQml_test.cpp`, "the macOS row draws the well and pills at the canvas tokens (offscreen)".
    Load with `ChromeSetup { .row = MacOS }` and three tabs. Then:
    - `tabWell` is visible, with `height == chromeStyle.size.tabWell` (28) and
      `radius == chromeStyle.radii.tabWell` (14);
    - each TabItem has `height == size.tab` (24), and its background radius is `radii.tab` (12);
    - `tabIcon.width == size.tabIcon` (14);
    - every pill has the same width, equal to `tabWellLayout(availableWidth, 3).tabWidth`;
    - the active tab's `tabCloseButton` is visible, and an inactive tab's is not until a hover
      (`QTest::mouseMove` onto it).
  - "the Fluent and ContourLinux rows keep today's geometry (offscreen)" is §2.1's acceptance test. For
    `Fluent` and `ContourLinux`:
    - `titleBar.height == 34`, today's `chromeHeightUnits`;
    - a TabItem's `height == 32`, and its `implicitWidth == clamp(label.implicitWidth + 56, 120, 240)`;
    - `tabWell` is not visible;
    - `newTabButton.width == 32` and `newTabMenuButton` is visible;
    - `tabCloseButton` is visible on an inactive tab;
    - a coloured tab's fill equals `controller.tabBackgroundColor(…)` (WT-style).
  - "a long title elides inside the pill (offscreen)" (Review Focus 1): MacOS row, `TabDecor::LongTitle`
    (a 200-character title on row 0).
    - `tabLabel.truncated` is true and the pill's `width ≤ chromeStyle.maxTabWidth`;
    - `Accessible.name` is the full title;
    - `dragRegion.width ≥ chromeStyle.space.dragReserve`.
  - "twenty tabs go icon-only and keep the active one in view (offscreen)" (Review Focus 2): MacOS row,
    20 tabs, the 20th active, a 900-wide window.
    - every `tabLabel` is invisible and every pill is the same width;
    - the active delegate's `x` lies within the list's visible range (`contentX` to `contentX + width`);
    - `newTabButton` (Task 7's band button, or the Flat one) is visible.
  - "a coloured pill is tinted at 20 % and its icon at 100 % (offscreen)": MacOS row, tab 1 coloured
    `#E74856`, at rest.
    - its fill equals `Qt.tint(colors.tabWellFill-composited ground, rgba(#E74856, tint.tab))`, read
      through a `QQmlExpression`;
    - `tabIcon.color == QColor(0xE74856)`.
  - QmlComponents_test `[tabbadge]`, "the badge slot draws each kind from the look table". For each kind
    from `iota(size_t { 1 }, TabBadgeKindCount)`:
    - under `Pill`, `tabBadgeMark.mark == badgeLook(kind).mark`;
    - under `Flat`, `tabBadgeGlyph.text == badgeLook(kind).glyph`;
    - the ink equals `chromeStyle.statusInk(role)`;
    - `Accessible.name == badgeLook(kind).name`.

    Working with progress 40 draws a dial with a sweep of 144°.
  - The same file, "the progress pulse stops under Reduce Motion" (Review Focus 4): an indeterminate
    progress (`tabProgressState` 3) under `MotionPreference::Reduced` leaves `progressFill.opacity == 1`
    after `QTest::qWait(chromeStyle.motion.pulse)`.
  - `Accessibility_qml_test.cpp`: add a `TabBadgeSlot.qml` case (`kind` = Blocked) to the kit table. It
    expects the role `StaticText` and the name "Waiting for you".
  - `ChromeSnapshotPlan_test.cpp` (`[snapshot]`):
    - `scriptRow(SnapshotScript::TabBadges).decor == TabDecor::EveryBadge`;
    - `scriptRow(TabOverflow).tabCount == 20`;
    - `scriptRow(TabColorFlyout).action == ScriptAction::OpenTabColorFlyout`.
  - `ChromeSnapshot_test.cpp` (`[contour][gui][qml][snapshot]`): the three new scripts build on the
    MacOS and Terminal rows, and `tab-color-flyout` finds `tabColorFlyout` opened.
  - The ContourTui pass (the TUI row and Review Focus 5) adds a `MainWindowQml_test` case, "the Terminal
    row draws one cell-high tabs with glyph badges (offscreen)". Under `ChromeSetup { .row = Terminal }`
    with contour-light, `TabDecor::EveryBadge`:
    - `titleBar.height == chromeStyle.cellHeight`;
    - the separators read `"│"`;
    - row 0's `tabColorCell.text == "▌"` in the tab colour, and no `tabIcon` is visible;
    - the badge glyphs are `• ◑ ! ✓ ✕`;
    - the active label's colour on `colors.tabActiveFill` is at least 4.5:1;
    - every badge glyph's ink on `colors.tabWellFill` is at least `min(4.5, ratio(BarText, Bar))`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow][tabwell]"`

  Expected: FAIL, because `tabWell` is not found.
- [ ] **Step 3: Implement.**
  - `TabItem`:
    - under `pill`: the pill background (`radii.tab`), the icon square, the title in `type.tab`, the
      badge slot, close-on-hover-or-active (`size.tabClose`, `radii.tabClose`, `colors.tabCloseFill`),
      the progress bar inside the pill (`size.progressBar`, inset `s5`/`s1`), the 20 % tint, and labels
      one level dimmer while `!Window.active`;
    - under `Flat`: today's layout, plus the `▌` cell where `chromeStyle.tabColorGlyph` is not empty
      (then no WT fill).
  - `TabStrip`: the well (`radii.tabWell`, `colors.tabWellFill`, padding and gap `s1`). Each delegate's
    width comes from `wellLayout.tabWidth` under `Pill`. The list scrolls and calls
    `positionViewAtIndex(activeTabIndex, ListView.Contain)` on overflow. The reorder takes
    `motion.standard`.
  - `TabColorFlyout`: the tokens listed under Files. Its keyboard behaviour (`:7-9`) is unchanged.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][tabbadge],[contour][gui][qml][a11y],[contour][gui][qml][snapshot],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][tabbadge],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS. `TabBadgeSlot.qml` is at baseline 0.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshot*.{hpp,cpp} src/contour/test/MockMainController.hpp src/contour/test/MainWindowQml_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/TabBadgeSlot.qml src/contour/qml/TabItem.qml src/contour/qml/TabStrip.qml src/contour/qml/TabColorFlyout.qml scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git add src/contour/qml src/contour/test scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git commit -m "contour: a well of pill tabs on macOS, one badge slot on every row, and a token-drawn colour flyout" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 7: The title band — HostChip, drift chip, toolbar, centred title, no title bar, full screen

> **Depends on Step 4a:**
> - 4a removes `Main.qml`'s settings loader (`:260-273`), and its R-l makes Main.qml's root a
>   `ThemedWindow`.
> - 4a gives `ChromeSnapshotScene` its `subject()`.
>
> Locate `titleBar`, `bannerStack`, `content` and `DaemonDriftBadge` in Main.qml by `objectName` or type.

**Files:**
- Create: `src/contour/qml/HostChip.qml` (kit HostChip) and `src/contour/qml/ToolbarButton.qml` (kit
  ToolbarButton). Add both to `_qml_files`.
- Modify: `src/contour/qml/TitleBar.qml`:
  - the `RowLayout` (`:77-161`) gains the HostChip after the leading slot, a centre that holds either the
    `TabStrip` or the `bandTitle`, the drag region with `Layout.minimumWidth: chromeStyle.space.dragReserve`,
    and the trailing drift chip and `+` (`Pill`);
  - `Layout.preferredWidth: Math.min(implicitWidth, root.width * 0.7)` (`:103`) gives way to
    `tabWellLayout`'s `wellWidth` under `Pill`, and to the drag reserve under `Flat`;
  - the leading slot's inset goes through `chromeStyle.snapToCells` (R-q);
  - `property string centre` and `property string presence` are assigned by Main.qml;
  - `property bool revealed` and `readonly property real slideOffset`.
- Modify: `src/contour/qml/TabStrip.qml`: `hostBadge` (`:55-82`) is deleted, because HostChip replaces it.
- Modify: `src/contour/qml/DaemonDriftBadge.qml` (all of `:1-61`): it becomes a toolbar chip, and
  `ConfirmDialog` gives way to `KitDialog` (R-m). Its size baseline of 4 goes to 0: delete the line.
- Modify: `src/contour/qml/Main.qml`:
  - `readonly property var band: chromeStyle.titleBand(…)`, from `win.titleBarVisible`, `visibility`,
    `tabBarShouldShow` and `tabBarPosition`;
  - the `titleBar` (`:146-178`) is placed from `band`;
  - a `tabsBar` (`TitleBar` with `centre: "tabs"`, no window controls) at the bottom when
    `band.tabBar === "shown"`;
  - the `bandRevealEdge` `Item`;
  - `chromeHeight` (`:132`) counts the band only while it is `"shown"`;
  - the drift badge leaves `content` (`:275-281`).
- Modify: `src/contour/test/MockMainController.hpp`: `titleBarVisible` gains a setter, and the scene gains
  `hostChipState`, `tabBarPosition` and `drift` (`enum class SnapshotDrift : uint8_t { None = 0, Shown }`,
  with a `window::DaemonDriftModel` built as `DaemonDriftModel_test.cpp:13-22` builds it).
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` and `ChromeSnapshotScene.hpp`:
  - `HostConnection` gains `Connected` and `Disconnected` (`:77-81`, becoming
    `{ Local = 0, Connected, Reconnecting, Disconnected }`);
  - `enum class SnapshotFrame : uint8_t { ClientBand = 0, NativeFrame };`;
  - `enum class SnapshotWindowMode : uint8_t { Windowed = 0, FullScreenRevealed };`;
  - the new rows, each existing row naming `.drift`, `.frame` and `.mode`.
- Test:
  - `src/contour/test/MainWindowQml_test.cpp`: the remote chrome case (`:346-388`) and new
    `[contour][gui][qml][mainwindow][band]` cases;
  - `src/contour/test/Accessibility_qml_test.cpp`;
  - `src/contour/test/ChromeSnapshotPlan_test.cpp`;
  - `src/contour/test/ChromeSnapshot_test.cpp`.

**Interfaces:**
- Consumes: Task 4 (`titleBand`, `hostChipLook`, `snapToCells`, the tokens), Task 5 (`win.hostChipState`,
  `win.hostLabel`), Task 6 (`TabStrip.availableWidth`, `wellLayout`).
- Produces (QML):
  - `HostChip.qml`: `required property string hostLabel`, `required property int state`, and
    `objectName: "hostChip"`. Its children are `hostChipDot`, `hostChipLabel` and `hostChipStatus`.
    `Accessible.role: Accessible.StaticText`, and `Accessible.name: qsTr("Attached to %1, %2")` with the
    host and the look's `accessibleState`. It has zero width when `hostLabel` is empty. On the Terminal
    row it reads `[● host status]`, with the glyph tokens.
  - `ToolbarButton.qml`: `property string glyph`, `property string accessibleName`, and the signals
    `clicked()` and `secondaryClicked()`. `secondaryClicked` fires on a right-click or a press-and-hold.
    Its size is `size.toolbarButton` square, at `radii.control`. Hover is `fill.3` and pressed `fill.2`,
    through the colour roles `tabHoverFill` and `controlPressedFill`. It is `Accessible.Button`.
  - `TitleBar`: `objectName`s `bandTitle` (the centred title: `type.body` medium in `colors.tabText`),
    `bandToolbar` and `newTabButton`. On `Pill`, `newTabButton` is a `ToolbarButton`. On `Flat`, it
    stays TabStrip's.
  - Main.qml: `objectName`s `tabsBar` and `bandRevealEdge`.

- [ ] **Step 1: Write the failing tests** (`MainWindowQml_test.cpp`).
  - The remote chrome case, rewritten:
    - a remote window finds `hostChip` visible, with `hostChipLabel.text == "home"`;
    - with the mock's `hostChipState` set to each `HostChipState`, `hostChipStatus.text ==
      hostChipLook(state).status` and `hostChipDot.color == chromeStyle.statusInk(look.dot)`;
    - a local window's `hostChip` is invisible with width 0;
    - `findChild("hostBadge") == nullptr`;
    - the window title stays `"home"`, from `remoteWindowTitle`.
  - "with one tab under Multiple, the macOS band shows the centred title (offscreen)" (Also pinned): MacOS
    row, one tab, `TabBarVisibility::Multiple`.
    - `titleBar` is visible with `height == chromeStyle.size.titleBar` (38);
    - `bandTitle` is visible with the active session's title, and `tabWell` is invisible;
    - `controller.chromeHeight() == 38`;
    - after `setTabBarShouldShow(true)`, `tabWell` is visible and `bandTitle` is not.
  - "with one tab under Multiple, Fluent and Linux show no bar, as today": `chromeHeight() == 0`.
  - "the drift chip sits in the bar, not over the cells (offscreen)": with the drift shown,
    - `daemonDriftBadge` is a descendant of `titleBar` and not of `content`;
    - its `height == chromeStyle.size.toolbarChip`;
    - clicking it opens `daemonDriftConfirm`, a `KitDialog`;
    - accepting calls the model's `requestRestart`, so the model reports `isRestarting`.
  - "show_title_bar keeps the native frame and a strip (offscreen)": `setTitleBarVisible(true)` on the
    MacOS row with three tabs.
    - `titleBar` shows the tabs with no leading inset (`leadingWindowControls.width == 0`);
    - `bandTitle` is invisible;
    - there are no window controls.
  - "full screen reveals the band without resizing the content (offscreen)" (Review Focus 4): MacOS row,
    `root->showFullScreen()`.
    - `titleBar.revealed` is false and `content.y == bannerStack.y`;
    - `controller.chromeHeight()` excludes the band;
    - after `QTest::mouseMove` onto `bandRevealEdge`, `titleBar.revealed` is true and the band is
      visible, while `content.y` and `content.height` are unchanged;
    - moving away hides it again.
  - The same under `MotionPreference::Reduced`: `titleBar.slideOffset == 0` throughout, and only its
    opacity changes.
  - "the tab bar at the bottom on macOS keeps the band at the top (offscreen)": MacOS row, position 1.
    `titleBar.y == 0` with `bandTitle` visible, and `tabsBar` is at the bottom with `tabWell` inside it.
  - "the macOS + opens profiles on a secondary click (offscreen)":
    - on the MacOS row, `newTabMenuButton` is absent;
    - a right-click on `newTabButton` makes the mock record `openNewTabMenu`;
    - on Fluent, `newTabMenuButton` is visible.
  - "the Terminal row snaps the native inset to cells and brackets its chips (offscreen)":
    - under `ChromeSetup { .row = Terminal }` with `nativeControlsInset` 70, the leading slot's width
      is `snapToCells(70)`;
    - `hostChipLabel` sits between `"["` and `"]"`.
  - `Accessibility_qml_test.cpp`: add kit cases for `HostChip.qml` (`hostLabel` `"build-01"`, state
    Reconnecting; the name contains "reconnecting"), `ToolbarButton.qml` (`Button`, "New tab") and
    `DaemonDriftBadge.qml` (`Button`).
  - `ChromeSnapshotPlan_test.cpp`: the five new rows' fields, for example
    `scriptRow(SnapshotScript::FullScreenReveal).mode == SnapshotWindowMode::FullScreenRevealed`.
  - `ChromeSnapshot_test.cpp`: every new script builds on the MacOS and Terminal rows, and
    `full-screen-reveal` finds `titleBar.revealed`.
- [ ] **Step 2: Run them and see them fail.**

  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow]"`

  Expected: FAIL, because `hostChip` is not found.
- [ ] **Step 3: Implement** the components, the band layout, Main.qml's placement and reveal, the mock
  and the snapshot rows.
  - The reveal:
    - `revealed` is true while a `HoverHandler` on `bandRevealEdge` or on the band is hovered;
    - under `motion.full`, `y` slides by `-height` over `motion.standard`;
    - under `motion.crossFade`, `opacity` fades over `motion.standard`.
  - The centred title is `win.activeSession.title`, through `tabLabelWithFallback`'s role: read the
    active row's `TitleRole`.
- [ ] **Step 4: Run them and see them pass.**

  Run:

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot],[hostchip],[titleband]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][a11y],[contour][gui][qml][snapshot],[contour][hosts],[contour][drift],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS. `HostChip.qml` and `ToolbarButton.qml` are at baseline 0, and `TitleBar.qml` drops from
  1 to 0.
- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshot*.{hpp,cpp} src/contour/test/MockMainController.hpp src/contour/test/MainWindowQml_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/HostChip.qml src/contour/qml/ToolbarButton.qml src/contour/qml/TitleBar.qml src/contour/qml/TabStrip.qml src/contour/qml/DaemonDriftBadge.qml src/contour/qml/Main.qml scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git add src/contour/qml src/contour/test scripts/qml-size-literal-baseline.txt src/contour/CMakeLists.txt
git commit -m "contour: the title band holds the host chip, the drift chip and the new-tab button; a centred single-tab title; a full-screen reveal" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 8: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, so
  the plan's own commit is left out. Apply its findings, re-run the filters of every task it touches,
  and commit once with the Signed-off-by line.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[tabbadge],[hostchip],[titleband],[tabwell],[tabattention],[chrometokens],[uistyle],[config],[snapshot],[contour][window][programstatus],[contour][hosts]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle],[manager],[contour][tablabel],[contour][gui][model],[contour][multiwindow],[contour][drift],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][mainwindow],[contour][gui][qml][tabbadge],[contour][gui][qml][snapshot]"
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

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, with
  `SnapshotScripts.size()` read from `ChromeSnapshotPlan.hpp`. Look at these:
  - `tab-badges.*`: the five marks on macOS, and glyphs elsewhere;
  - `tab-badges.terminal.*.light`: the glyphs legible on the dark bar;
  - `tab-overflow.*`;
  - `single-tab.macos.*`: the centred title;
  - `remote-*.*` and `daemon-drift.*`: the chips in the band;
  - `full-screen-reveal.*`.

  On Fluent and Linux, `three-tabs.*` and `single-tab.*` must look as they did before this step.
- [ ] **Step 4: Owner manual checks.** The checks are batched per phase (ruling), so list them in the
  ledger for the owner. Do not wait on them.
  - On this Mac (`out/appleclang-debug/src/contour/contour.app`), a single tab shows a centred title, and
    ⌘T turns it into the first pill.
  - Pills are equal width, close shows on hover and on the active tab, and a coloured tab is tinted.
  - Twenty tabs go icon-only and then scroll. The window drags by the empty band.
  - Run `sleep 3; echo x` in a background tab, and it shows the accent dot. `false` in a background tab
    shows ✕ until the tab is visited.
  - A right-click on `+` opens the profiles.
  - Full screen: the band reveals at the top edge. Note where it sits relative to the system menu bar,
    which is input to Step 1d.
  - With Reduce Motion on (System Settings › Accessibility), the reveal fades and the spinner stands
    still.
  - `contour client --host`: the chip, and "Reconnecting…" after killing the daemon's connection. No
    background tab gains a dot when it comes back.
  - `show_title_bar: true`: the native frame and today's strip.
  - `ui_style: terminal`: the one-cell row, `▌`, the glyphs, and the traffic lights' inset.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release
  at `:107` (the `x.y.z` block at `:97-105` is a commented-out template):
  - "Adds macOS tabs as pills in a tab well within the title band, with a centred title while there is
    one tab."
  - "Adds one badge per tab for unseen output, finished commands and OSC 7501 program status, by
    priority: waiting, failed, working, done, new output. 'Working' and progress now use the accent
    colour."
  - "Changes the remote host label into a host chip that shows when the connection is reconnecting or
    lost."
  - "Moves the daemon version notice from over the terminal into the title bar."
  - "Reveals the tab bar in full screen when the pointer reaches the top edge, as a cross-fade under
    Reduce Motion."
  - "Shows the foreground process's name on a tab whose title is empty."
- [ ] **Step 6:** Mark Step 1c done in the plans index's step table and its *Last update* line, and on the
  board (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-1c.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** a badge costs one ledger lookup per pane on top of today's cached OSC 7501 badge.
    `foregroundProcessName()` is called only for a tab whose label is empty. There is no new timer.
  - **Risk:** medium.
    - Main.qml's bar placement is new on macOS (the band is always present).
    - The roles are renamed.
    - The attention ledger is fed from the parser thread's existing post.
  - **Coverage:** the five pure tables, the `[manager][tabbadge]` and `[manager][hostchip]` cases, the
    `[tabwell]`/`[band]` property tests including the §2.1 acceptance test, and eight new snapshot
    scripts.

---

## Owner decisions

The run proceeds on each recommended default.

1. **One tab on macOS keeps a 38-pt band with a centred title (R-e).** The canvas wants it, and the
   traffic lights need the room. Under `Multiple` today the bar collapses, leaving them over the cells.
   Native2026 macOS windows with one tab grow by 38 pt of chrome. *Default: yes.*
2. **The Disconnected chip has no Reconnect button (R-l).** No reconnect API exists. *Default: show
   "Disconnected" only. Reconnect arrives with the Moments footer ("Disconnected · Reconnect ↩").*
3. **The full-screen band overlays the top rows rather than pushing them down (R-k), on every row.**
   *Default: overlay, as macOS does.* Pushing would reflow the terminal on every reveal.
4. **The tab icon is the kit's tinted square, not a per-process symbol (R-g).** *Default: the square.
   Process icons come later, with an icon set and a process poll.*
5. **macOS opens profiles from a secondary click on "+", with no ▾ (R-n).** The canvas wants it, but §3.4
   does not list it. *Default: include it. It is one layout token and one handler.*
6. **The tab bar at the bottom on macOS (R-e).** *Default: the band stays at the top with the centred
   title, and the well sits in a bottom bar of the band's height.*
7. **The process-name label fallback applies to every edition (R-o).** The Phase 0 list had it as a
   Native2026 default. *Default: ungated, because an empty tab label is a defect.* The alternative is a
   profile field in both edition tables, and a YAML key.

## Spec gaps found

- §3.4 names `TabActivity::UnseenOutput` and `FinishedBlockBadge` with no source. Nothing in the code
  tracks either (R-b).
- §3.4's `std::optional<int> progressPercent` differs from `ProgramStatusBadge::progress`
  (`std::optional<uint8_t>`); the conversion happens in `tabBadge()`.
- The canvas (`surfaces`) reads "show_title_bar: false … drops the band: tabs move to the status line's
  {Tabs}". That contradicts §3.4, which binds.
- In the canvas but not in §3.4:
  - the centred single-tab title;
  - the profile secondary-click;
  - the tab icon;
  - the accent progress bar (the kit board draws it green; `mac-dark` and the ruling say accent);
  - the pane reconnecting overlay's restyle.
- The HostChip's second word (`daemon`/`ssh`) and its click-for-session-list have no source.
- On macOS, the TUI traffic lights' vertical snap, and the full-screen band's place under the menu bar,
  are Step 1d's.
- `surface.elevated` dark is `#2C2C2E` on `kit-tokens` and `#3A3C43` on `mac-dark`.
