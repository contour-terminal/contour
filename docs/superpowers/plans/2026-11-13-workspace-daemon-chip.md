# One chip for daemon and workspace — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In daemon mode, every visible title band shows one `daemon › workspace ▾` chip that absorbs the
host chip. The chip opens the full switcher below itself, and its tooltip names the ShowSwitcher shortcut.
With the sidebar shown, the daemon control heads the sidebar instead.

**Architecture:** Every decision (names, dot, elision step, placement, accessible text) is a pure function
in a new Qt-free header, `window/LocationChip.hpp`, table-tested in `contour_test`. `WindowController`
exposes the results to QML. `WorkspaceChip.qml` becomes the merged chip; `HostChip.qml` is deleted. The
switcher loses its "widened chip" state, because the chip now opens it at `SwitcherScope::Everything`.

**Tech Stack:** C++23, Qt 6 / QML, Catch2 (`contour_test`, `contour_gui_test` offscreen).

**Spec:** `docs/superpowers/specs/2026-11-13-workspace-daemon-chip-design.md`. The board is
`chip-discovery.dc.html` on <https://claude.ai/artifact/RcxAEXjArUvKdqXdWdGTyu>.

## Global Constraints

- **Prerequisite.** Rebase onto `origin/master` only after `fix/sidebar-focus-and-workspace-cycle` has
  merged. That branch fixes the in-process chip click (bug 4) and may touch the switcher. Task 1 starts
  from the rebased tree.
- **Project rules.** AGENT.md and `.agent/rules/ui-ux.md` bind every task:
  - `enum class` (`: uint8_t`, zero = off/absent) instead of `bool`.
  - Every new public symbol gets a Doxygen comment.
  - No `NOLINT`; zero warnings under `-Werror`.
  - `clang-format` on C++ only, never on `.qml`.
  - QML decides nothing; every size, colour and glyph comes from `chromeStyle`.
- **Copy.** Use these strings exactly:
  - default daemon: `local`
  - unnamed daemon: `daemon`
  - tooltip: `Switch workspace or daemon`
  - in-process tooltip: `Switch workspace`
  - separator: `›`
  - caret: `▾`
  - workspace floor: 6 characters plus `…`
- **Build and test.**
  - Build: `cmake --build out/appleclang-debug`, in the foreground, in this worktree's own tree. Never wait
    on `pgrep -f "cmake --build"`.
  - Unit tests: `out/appleclang-debug/src/contour/contour_test`.
  - GUI tests: `QT_QPA_PLATFORM=offscreen out/appleclang-debug/src/contour/contour_gui_test`.
- **Commits.** End every commit message with `Signed-off-by: Christian Parpart <christian@parpart.family>`
  and no other trailer.

## Review Focus

1. **Long names.** A host label of 40+ characters plus *Reconnecting…* must not push the tabs below their
   minimum width, and the workspace name must keep at least 6 characters. Test in Task 1
   (`locationChipFit`) and Task 5 (offscreen).
2. **A daemon labelled `local`.** A named local daemon whose label is literally `local` reads the same as
   the default one. That is accepted. A test pins the naming, so nobody "fixes" the default back to
   `default` (Task 1).
3. **Escape in a chip-opened popup closes it at once**, even with daemons listed, and Back inside a
   daemon's level still returns to the top level (Task 3).
4. **The default local daemon goes down** (live upgrade, stopped). The chip shows the dot and the state, and
   `hostNoticeOf` stays `None` for it, so no band gets pinned (Tasks 1 and 4).
5. **The band hides while the sidebar is shown.** The sidebar control alone names the daemon; no chip
   lingers in the band (Tasks 2 and 6).

---

### Task 1: Pure chip decisions — `window/LocationChip.hpp`

**Files:**
- Create: `src/contour/window/LocationChip.hpp`
- Create: `src/contour/window/LocationChip_test.cpp`
- Modify: `src/contour/CMakeLists.txt` (add the test to `contour_test`'s sources, next to
  `window/HostChipState_test.cpp`)

**Interfaces:**
- Consumes:
  - `session::SwitchTarget` (`session/WindowSwitching.hpp`)
  - `vthost::DefaultSocketLabel`
  - `HostChipState` and `hostChipLook()` (`window/HostChipState.hpp`)
  - `DaemonLocality` (`window/DaemonDriftChoice.hpp`)
- Produces (all in `namespace contour::window`):
  - `[[nodiscard]] std::string locationDaemonName(session::SwitchTarget const& address, std::string_view hostLabel)`
  - `enum class DaemonDot : uint8_t { Hidden = 0, Shown };` and
    `[[nodiscard]] constexpr DaemonDot daemonDotOf(DaemonLocality, HostChipState) noexcept`
  - `enum class LocationChipFit : uint8_t { Full = 0, NoStateText, DaemonElided, DotOnly, WorkspaceElided };`
  - `struct LocationChipWidths { double full; double noStateText; double daemonElided; double dotOnly; double workspaceFloor; };`
  - `[[nodiscard]] constexpr LocationChipFit locationChipFit(LocationChipWidths const&, double available) noexcept`
  - `[[nodiscard]] std::string locationChipAccessibleName(std::string_view daemon, std::string_view workspace)`
  - `[[nodiscard]] std::string locationChipAccessibleDescription(HostChipState, DaemonDot, std::string_view expanded)`
  - `[[nodiscard]] std::string locationChipToolTip(HostChipState, LocationChipFit, std::string_view daemon)`

- [ ] **Step 1: Write the failing tests** in `LocationChip_test.cpp`, tag `[LocationChip]`:
  - `locationDaemonName`:
    - `{Label, "default"}` → `"local"`
    - `{Label, "work"}` → `"work"`
    - `{Label, "local"}` → `"local"` (Review Focus 2)
    - `{Same, ""}` → `"daemon"`
    - `{Host, "starfive"}` with hostLabel `"starfive"` → `"starfive"`
    - `{Host, "sf"}` with hostLabel `"me@starfive"` → `"me@starfive"` (the host label wins)
  - `daemonDotOf`:
    - `(Remote, Connected)` → `Shown`
    - `(Local, Connected)` → `Hidden`
    - `(Local, Reconnecting)` → `Shown`
    - `(Local, Disconnected)` → `Shown`
  - `locationChipFit`: with widths `{300, 220, 160, 110, 80}`, available `400` → `Full`, `250` →
    `NoStateText`, `200` → `DaemonElided`, `120` → `DotOnly`, `90` and `10` → `WorkspaceElided`.
  - `locationChipAccessibleName("starfive", "billing")` == `"Workspace billing on starfive"`.
  - `locationChipAccessibleDescription`:
    - `(Reconnecting, Shown, "collapsed")` == `"Reconnecting, collapsed"`
    - `(Connected, Hidden, "expanded")` == `"expanded"`
  - `locationChipToolTip`:
    - `(Reconnecting, NoStateText, "starfive")` == `"Reconnecting to starfive · Switch workspace or daemon"`
    - `(Reconnecting, Full, "starfive")` == `"Switch workspace or daemon"`
    - `(Connected, DotOnly, "local")` == `"Switch workspace or daemon"`

- [ ] **Step 2: Run** `contour_test "[LocationChip]"`. Expected: it fails to compile (the header is
  missing).

- [ ] **Step 3: Implement the header.**
  - `locationChipFit` returns the first step whose width fits, `WorkspaceElided` otherwise.
  - The state word in the description and the tooltip is `hostChipLook(state).status` without its
    trailing `…`, with the first letter kept capitalised. Disconnected reads "Disconnected from <daemon>".

- [ ] **Step 4: Run** `contour_test "[LocationChip]"`. Expected: PASS.

- [ ] **Step 5: Commit.** `window: add the location chip's pure decisions`

**Amendments from the plan review (binding; sent to the implementer mid-task):**
- **Widths.** `LocationChipWidths` drops `workspaceFloor`. Add instead
  `inline constexpr std::size_t LocationChipWorkspaceFloor = 6;`.
- **Accessible name.** `locationChipAccessibleName(std::string_view daemon, SidebarSummary const& summary)`
  keeps the existing marker clause ("…, another workspace needs you"). An empty daemon gives exactly
  `workspaceChipAccessibleName(summary)`; the two share their code.
- **Accessible description.** `locationChipAccessibleDescription(HostChipState, std::string_view expanded)`
  (no `DaemonDot`), built from `hostChipLook(state).accessibleState` with its first letter capitalised.
  NeedsAttention reads "Reconnecting, needs your attention, collapsed".
- **Tooltip prefix.** A per-state table:
  - Connected: none
  - Reconnecting: "Reconnecting to {}"
  - Disconnected: "Disconnected from {}"
  - NeedsAttention: "Reconnecting to {} needs your attention"

  An empty daemon (in process) gives "Switch workspace".
- **State glyph.** `constexpr std::string_view daemonStateGlyph(HostChipState)`: `●` connected, `◑`
  reconnecting, `✕` disconnected, `!` needs attention.

### Task 2: Where the chip shows — placement rule

**Files:**
- Modify: `src/contour/window/LocationChip.hpp`
- Modify: `src/contour/window/LocationChip_test.cpp`

**Interfaces:**
- Consumes: `BarPresence` and `BarCentre` (`window/TitleBandLayout.hpp`).
- Produces:
  - `enum class DaemonHosting : uint8_t { InProcess = 0, Daemon };`
  - `enum class LocationChipPlace : uint8_t { None = 0, Band, Sidebar };`
  - `[[nodiscard]] constexpr LocationChipPlace locationChipPlace(BarPresence band, BarCentre centre, DaemonHosting) noexcept`
  - `[[nodiscard]] constexpr std::string_view locationChipPlaceName(LocationChipPlace) noexcept`, returning
    `"none"`, `"band"` or `"sidebar"` for QML.

- [ ] **Step 1: Write the failing test** as a table over every `BarCentre` × `BarPresence` × `DaemonHosting`:
  - **Daemon:**
    - band Hidden → `None`
    - `Breadcrumb` → `Sidebar`
    - `Tabs`, `Title`, `Empty`, `ChipAndTabs` → `Band`
  - **InProcess:**
    - `ChipAndTabs` with the band Shown → `Band`
    - everything else → `None` (today's rule)

- [ ] **Step 2: Run** `contour_test "[LocationChip]"`. Expected: FAIL.

- [ ] **Step 3: Implement** it as a `switch` over `BarCentre`, with no `default:`, so a new centre is a
  compile error.

- [ ] **Step 4: Run.** Expected: PASS.

- [ ] **Step 5: Commit.** `window: decide where the location chip shows`

**Amendment from the plan review (binding):** the in-process rule is `ChipAndTabs` with the band
**not Hidden** (shown, reveal or pinned) → `Band`; everything else → `None`.

### Task 3: The chip opens the full switcher; the widened state goes

**Files:**
- Modify:
  - `src/contour/session/TerminalSessionManager.cpp`: `showWorkspaceDropdown` opens with
    `SwitcherScope::Everything`.
  - `src/contour/window/SwitcherKeys.hpp`: drop the chip clause from `isNested()` (lines ~222–225).
  - `src/contour/window/SwitcherController.cpp`: drop the `AllDaemons` cases (~536, ~567, ~590) and the
    narrowing branch in `back()` (~625). In the opening code (~256), a `SwitcherScope::Everything` opening
    from the chip discovers daemons as the centred one does.
  - `src/contour/window/SwitcherRows.hpp` / `.cpp`:
    - remove `SwitcherRowKind::AllDaemons`, its row rule and the "All daemons…" row (~221);
    - `switcherDaemonName()` returns `locationDaemonName(address, {})`, except that `Same` keeps
      `"this daemon"` in the switcher's own rows.
- Test:
  - `window/SwitcherKeys_test.cpp`
  - `window/SwitcherRows_test.cpp`
  - `window/SwitcherController` tests (`contour_test "[switcher]"`)

**Interfaces:**
- Consumes: Task 1's `locationDaemonName`.
- Produces: `SwitcherScope::OneDaemon` remains, for the switch-asks-to-pick path only.

- [ ] **Step 1: Write the failing tests.**
  - The keys context `{origin: Chip, scope: Everything, level: Top}` is not nested, so Escape closes the
    popup (Review Focus 3).
  - Inside `SwitcherLevel::Daemon` opened from the chip, Back returns to Top.
  - The rows of a chip opening list the window's daemon's workspaces first, under `"Workspaces on local"`
    for the address `{Label, "default"}`, and contain no `AllDaemons` row.
  - The initial selection is unchanged: the first workspace not shown (the existing
    `switcherInitialSelection` test keeps passing).

- [ ] **Step 2: Run** `contour_test "[switcher]"`. Expected: the new cases FAIL.

- [ ] **Step 3: Implement** the edits listed under Files. Delete the tests that pinned the widen and narrow
  behaviour, rather than skipping them.

- [ ] **Step 4: Run** `contour_test`. Expected: PASS, with no reference to `AllDaemons` left
  (`grep -rn AllDaemons src/` is empty).

- [ ] **Step 5: Commit.** `switcher: the workspace chip opens every daemon, and Escape closes it`

**Amendments from the plan review (binding; they override the steps above where they differ):**
- **Test targets.**
  - `test/SwitcherController_test.cpp` (tag `[contour][switcher]`) builds into `contour_gui_test`, so run
    both `contour_test "[switcher]"` and `contour_gui_test "[switcher]"`.
  - Add a test that `TerminalSessionManager::showWorkspaceDropdown` opens `{scope: Everything, origin: Chip}`.
    It lives in `session/TerminalSessionManager_test.cpp`, tag `[manager]`, in `contour_gui_test`.
- **`AllDaemons` also lives in the hint text:**
  - `SwitcherHintSubject::AllDaemons` (`SwitcherRows.hpp:167`, `SwitcherText.hpp:146`, `SwitcherText.cpp:148,190`)
  - its tests in `SwitcherText_test.cpp:206,349,413`
  - the now-dead Chip-with-`OneDaemon` branch in `SwitcherText.cpp:173`

  Remove all of them.
- **Tests that open the chip with `OneDaemon`** move to `Everything`:
  - `test/SwitcherQml_test.cpp:115`
  - `window/SidebarController_test.cpp:703`
  - `test/SwitcherController_test.cpp:453,701`
- **Daemon names.** `switcherDaemonName` for a `Host` address keeps `address.daemon`. The chip's name for a
  host comes from `hostLabel` in Task 4. *Ruling:* the popup may name a saved host by its saved name, while
  the chip shows the label it is connected as. Both are true names; unifying them is not this plan's job.

### Task 4: `WindowController` exposes the chip

**Files:**
- Modify: `src/contour/window/WindowController.hpp` / `.cpp`
- Modify: `src/contour/session/TerminalSessionManager.hpp` / `.cpp` (an accessor
  `daemonNameOf(WorkspaceId) -> std::string`, built from `daemonTargetOf()` and `hostLabelOf()`)
- Test: `src/contour/session/TerminalSessionManager_test.cpp`

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces these QML-facing members of `WindowController`, notified on `hostLabelChanged` and
  `hostChipStateChanged`:
  - `Q_PROPERTY(QString chipDaemonName ...)`: empty for an in-process window.
  - `Q_PROPERTY(int chipDaemonDot ...)`: a `DaemonDot` value.
  - `Q_PROPERTY(QString chipAccessibleName ...)` and `Q_PROPERTY(QString chipAccessibleDescription ...)`:
    the description uses `isWorkspaceDropdownOpen` for expanded/collapsed.
  - `Q_INVOKABLE QString chipToolTip(int fit) const`
  - `Q_INVOKABLE QString locationChipPlace(QString band, QString centre) const`: the QML carve-out, names
    in and a name out, like `UiStyleProvider::titleBand`.
- `hostLabel` and `hostChipState` stay. `hostNoticeOf(hostChipPresence(hostLabel), …)` is **not**
  changed, so the default local daemon raises no band notice (Review Focus 4).

- [ ] **Step 1: Write the failing tests** in `TerminalSessionManager_test.cpp`:
  - `daemonNameOf` is `"local"` for a window on the default daemon, `"work"` on `--socket-label work`, and
    empty in process.
  - `hostNoticeOf` for the default daemon in Reconnecting stays `None`.

- [ ] **Step 2: Run** `contour_test "[TerminalSessionManager]"`. Expected: FAIL.

- [ ] **Step 3: Implement** the accessor and the properties.

- [ ] **Step 4: Run** `contour_test`. Expected: PASS.

- [ ] **Step 5: Commit.** `window: expose the location chip to QML`

**Amendments from the plan review (binding):**
- **Test target.** `session/TerminalSessionManager_test.cpp` is in `contour_gui_test`, tag `[manager]`; run
  `contour_gui_test "[manager]"`, not `contour_test`. Extend the fixture `DaemonNamedBinding`
  (`test/GuiTestFixtures.hpp`), whose `hostLabel()` is always empty today, so it can return a host label.
- **One change signal.** Replace the "notified on hostLabelChanged and hostChipStateChanged" wording: add a
  single `chipChanged()` signal as the `NOTIFY` of every chip property. Emit it from:
  - `hostLabelChanged`
  - `hostChipStateChanged`
  - the sidebar model's `summaryChanged` (the name carries the marker clause)
  - the switcher open state (`isWorkspaceDropdownOpen`, for expanded/collapsed)
- **Accessibility.** `chipAccessibleName` uses Task 1's
  `locationChipAccessibleName(daemon, SidebarSummary const&)`, which keeps the "another workspace needs you"
  clause. `chipAccessibleDescription` uses `locationChipAccessibleDescription(state, expanded)`.
- **Drop the failing-first requirement for the `hostNoticeOf` test.** It pins behaviour that already holds,
  so it passes at once. Keep it as a regression test.

### Task 5: The merged chip in the band, both styles

**Files:**
- Modify: `src/contour/qml/WorkspaceChip.qml`. It becomes the merged chip and keeps
  `objectName: "workspaceChip"` and its child names (`workspaceChipLabel`, `workspaceChipCaret`,
  `workspaceChipMarker`). It adds `workspaceChipDaemon`, `workspaceChipDot` and `workspaceChipStatus`.
- Delete: `src/contour/qml/HostChip.qml` (and its entry in `src/contour/CMakeLists.txt`'s QML list).
- Modify: `src/contour/qml/TitleBar.qml`:
  - `hostChipSlot` is removed;
  - `workspaceChipSlot.active` reads `controller.locationChipPlace(root.presence, root.centre) === "band"`;
  - the width budget at ~304–308 counts one chip.
- Test:
  - `src/contour/test/SidebarQml_test.cpp`
  - `src/contour/test/MainWindowQml_test.cpp`: the `hostChip` cases at ~1319–1380 move to `workspaceChip`
    and its new children.
  - `src/contour/test/ChromeSnapshot_test.cpp` (~653, ~863)
  - `src/contour/test/Accessibility_qml_test.cpp`

**Interfaces:**
- Consumes: Task 4's properties.
- Produces: the chip's `fit` property (a `LocationChipFit` value), set by `TitleBar.qml` from the band's
  remaining width. The widths come from `TextMetrics`; the decision is `locationChipFit`, called through a
  `WindowController` `Q_INVOKABLE int chipFit(QVariantList widths, real available)`.

- [ ] **Step 1: Write the failing offscreen tests.**
  - **Daemon mode, default daemon, every centre** (`chip+tabs`, `tabs`, `title`, `empty`): `workspaceChip`
    is visible, `workspaceChipDaemon.text == "local"` and `workspaceChipDot` is invisible.
  - **Same window, remote reconnecting:** the dot is visible and `workspaceChipStatus.text ==
    "Reconnecting…"`.
  - **Centre `breadcrumb`:** no visible `workspaceChip` in the band.
  - **Narrow band** (Review Focus 1), with a 40-character host label: the label keeps ≥ 6 characters, and
    the tab well's width stays ≥ its minimum.
  - **Tooltip:** `workspaceChipToolTip` text begins `"Switch workspace or daemon"` and carries the
    ShowSwitcher chord.
  - **Accessibility:** name `"Workspace billing on local"`; role `ButtonMenu`.
  - **Terminal style** (`ui_style: terminal`): the label renders as
    `[local › billing ▾]`, and as `[◑ starfive Reconnecting… › billing ▾]` when reconnecting.
  - No `hostChip` object exists anywhere in the scene.

- [ ] **Step 2: Run** the `contour_gui_test` cases. Expected: FAIL.

- [ ] **Step 3: Implement.**
  - Move the dot and status from `HostChip.qml` into `WorkspaceChip.qml`. The dot colour comes from
    `hostChipLook`'s role through `chromeStyle`.
  - Set `KitToolTipHost { shortcutCommand: "ShowSwitcher" }` with `text: controller.chipToolTip(fit)`.
  - In-process: `chipDaemonName` is empty, so the daemon segment is absent and the tooltip text is
    `Switch workspace` (still with `shortcutCommand`, since bug 4's fix makes ShowSwitcher work there).

- [ ] **Step 4: Run** `contour_gui_test`. Expected: PASS.

- [ ] **Step 5: Compare against the board.** Grab the chip offscreen for macOS dark, Fluent, ContourLinux
  and Terminal. Compare its height, radius, fills, gaps and glyphs with `chip-discovery.dc.html` rows 1, 2,
  5 and 6. List the deviations in the task report.

- [ ] **Step 6: Commit.** `chrome: one chip names the daemon and the workspace`

**Amendments from the plan review (binding):**
- **No binding loop.** `fixedPartsWidth` in `TitleBar.qml` includes the chip slot and feeds
  `TabStrip.availableWidth`, so `fit` must not be computed from the remaining width. Compute `available` as:

  > band width − the other fixed parts (traffic-light or window-button area, new-tab button, drift chip) −
  > `dragReserve` − the tab well's minimum (`tabWellLayout`'s minimum width for the current tab count)

  `chipFit` takes a `QVariantMap` with keys `full`, `noStateText`, `daemonElided`, `dotOnly` and
  `available`, not a positional list.
- **What each fit step renders:**
  - `DaemonElided`: the daemon name middle-elided to at most 10 characters.
  - `WorkspaceElided`: the workspace name right-elided to `LocationChipWorkspaceFloor` (6) characters plus
    `…`, never fewer.
  - In the Terminal style all widths are whole cells (`chromeStyle.cellWidth` multiples).
- **Terminal-style state glyphs.** `UiStyle.hpp:548` has one `hostDotGlyph`. Use Task 1's
  `daemonStateGlyph(state)` through a `WindowController` property `chipStateGlyph`, so `[◑ starfive …]`
  renders per state. `UiStyleProvider::isHostChipShown` (`UiStyleProvider.hpp:753`) and `hostDotGlyph` go
  if nothing else reads them; update `UiStyle_test` and `UiStyleProvider_test` to match.
- **`TitleBar.qml`.**
  - `workspaceChipSlot.active` keeps `root.isBand &&` in front, so the tabs-only bar (role "tabs") never
    grows a second chip.
  - Remove `hostChipSlot` from `toolbarItems` (line ~86) and from the `leftMargin` expression (line ~501).
- **Test doubles.** Add the new `WindowController` members (`chipDaemonName`, `chipDaemonDot`,
  `chipStateGlyph`, `chipAccessibleName`, `chipAccessibleDescription`, `chipToolTip`, `chipFit`,
  `locationChipPlace`, `chipChanged`) to `test/MainWindowControllerMock.hpp` and
  `test/MockMainController.hpp`, with a settable daemon name and state per scene.
- **Every `hostChip` use to migrate:**
  - `MainWindowQml_test.cpp` ~1319–1380, ~3190–3312, ~3464–3484, ~4121 and ~4631. Fix
    `reportedToolbarItems == 3` there.
  - `Accessibility_qml_test.cpp:249`, which loads `HostChip.qml` by URL.
  - `ChromeSnapshot_test.cpp` ~154, ~653 and ~863.
  - `ChromeSnapshotPlan.hpp`: new scripts for the merged chip. `ChromeSnapshotPlan_test.cpp:120` asserts
    "empty host label means a local connection", which no longer holds once the default daemon shows a
    chip; rewrite it to the new rule.
- **WorkspaceChip's interface.** It declares `required property var controller`, alongside the existing
  `sidebar`, and reads `controller.chipDaemonName`, `controller.chipToolTip(fit)` and the other members
  through it; `TitleBar.qml` passes `root.controller`.
- **UI rule tests.**
  - Rule 5.5: add an offscreen case with ShowSwitcher unbound. The tooltip then shows the text and no chord.
  - Spec: the offscreen cases run on all four chrome rows (macOS, Fluent, ContourLinux, Terminal), not
    macOS alone.

### Task 6: The daemon control heads the sidebar

**Files:**
- Modify: `src/contour/qml/Sidebar.qml`. In daemon mode the title row's heading (`sidebarTitle`, ~193) is
  replaced by a daemon-only chip:
  - `objectName: "sidebarDaemonChip"`;
  - text `chipDaemonName` plus `▾`, with the dot and status when not connected;
  - a click opens the switcher anchored below it (`controller.showWorkspaceDropdown(anchor)`);
  - the same tooltip.

  In process, the heading stays "Workspaces". On the ContourLinux header pane, the control goes in
  `TitleBar.qml`'s part of the row.
- Test: `src/contour/test/SidebarQml_test.cpp`

**Interfaces:**
- Consumes: Task 4's properties, and `locationChipPlace(...) === "sidebar"`.

- [ ] **Step 1: Write the failing tests.**
  - With the sidebar shown in daemon mode, `sidebarDaemonChip` is visible and its text starts with
    `"local"`.
  - Clicking it opens the switcher with `origin == Chip`.
  - In the Terminal style it renders `[local ▾]` as the first row.
  - In process, `sidebarTitle` reads `"Workspaces"`.

- [ ] **Step 2: Run.** Expected: FAIL.

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run** `contour_gui_test` and the full `ctest --test-dir out/appleclang-debug`. Expected:
  PASS.

- [ ] **Step 5: Commit.** `sidebar: name the daemon at the head of the sidebar`

**Amendments from the plan review (binding):**
- **Each chrome row is different:**
  - **macOS:** the sidebar heading is `SidebarHeading::Unnamed` (no `sidebarTitle` shown). Add the daemon
    chip as the first item of the title row's leading edge, after the sidebar toggle. Do not replace a
    heading that does not exist.
  - **ContourLinux:** the heading is `headerSidebarTitle` in `TitleBar.qml`. Add `src/contour/qml/TitleBar.qml`
    to the files, and put the daemon chip there.
  - **Fluent:** replace `sidebarTitle` with the chip, as written above.
  - **Terminal:** the chip is the title row's content, rendered `[local ▾]`. The spec's "first row of the
    cell tree" means that title row; no tree row is added.
- **Placement rule.** In process the band chip uses Task 2's rule (`ChipAndTabs`, band not Hidden); the
  sidebar control appears only in daemon mode.

### Task 7: Release note and finish

**Files:**
- Modify: `metainfo.xml` (via the `contour-workflows:add-release-note` skill)
- Modify: `docs/` where the host chip or the workspace chip is described (`grep -rln "host chip\|workspace chip" docs/`)

- [ ] **Step 1:** Add the release note: "The title bar names the daemon and workspace in one chip, which
  opens the switcher and shows its shortcut".
- [ ] **Step 2:** Update the docs that `grep -rlni "host chip\|workspace chip" docs/` finds. They
  include `docs/…/native-chrome.md:160`, `accessibility.md:52` and `persistent-sessions.md:221`.
- [ ] **Step 3:** Run `/simplify`, then `/code-review`, and fix every finding.
- [ ] **Step 4: Commit.** `docs: describe the daemon and workspace chip`
