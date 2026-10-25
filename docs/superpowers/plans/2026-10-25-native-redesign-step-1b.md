# Native Redesign — Step 1b (Menus as Data, Native Menu Presenter, macOS Menu Bar) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Amended after its pre-flight scan (findings F1–F24).

**Goal:** Turn every menu the chrome shows into a `ContextMenuEntry` table in `command/`, present those
tables natively on macOS (an `NSMenu` for context menus, a global `QMenuBar` with six menus) and in-scene
everywhere else, with zero restyling on Linux and Windows.

**Architecture:**
- **Menus are data, Qt-free.** `command/` gains the tab context menu, the new-tab menu and the six-menu
  bar as tables over the action catalogue, plus `MenuTree`: one resolved tree (rows, chords, roles,
  swatches, and the actions the rows carry by index) that every presenter consumes. Adding a menu item
  is adding a table row.
- **Presentation is a token plus an interface.** `layout.menu` (`MenuPresentation { InScene = 0, Native }`)
  is `Native` only on the MacOS row. `platform::INativeMenuPresenter` has an inline null implementation
  and a Cocoa `.mm` that realises the `MenuTree` as an `NSMenu`. `window::presentMenu` routes: native when
  the row asks and the presenter can, otherwise today's in-scene `ActionContextMenu.qml`.
- **The menu bar is a table plus a thin realisation.** `command/MenuBarTable` holds the six menus;
  `window/AppMenuBar` realises them as a parentless `QMenuBar` once per app on a macOS host, maps
  `MenuItemRole` to `QAction::MenuRole`, refreshes a menu's rows when it opens, and rebuilds on reload.
  `platform::IMenuBarIntegration` (null inline, Cocoa `.mm`) hands the Window and Help menus to AppKit.
- **The terminal stays the key dispatcher.** The menu bar's ⌃/⌘ chords become NSMenu key equivalents, so
  `TerminalDisplay` claims every `ShortcutOverride` on every host; only under a global menu bar do Quit and
  Preferences go to the menu. A binding keeps its mode conditions.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls, Widgets for `QMenuBar` only), Objective-C++ (AppKit)
on Apple, Catch2 v3, Python 3 (lint), CMake preset tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.2, which binds,
with what it cites: §1 (principles), §2.1 (token rows), §2.6 (testing; the Widgets split), §2.5 (snapshot
driver). Visual design record: `docs/superpowers/specs/2026-10-23-native-redesign-design.md` (Decisions:
"Menus are native on macOS and Windows"; Coverage additions: "menus stay in-scene on every OS" for the TUI
row, "Attach to Host is … a Shell submenu"; "Shortcuts render per platform: ⌃⌥⇧⌘ glyphs on macOS").
Canvas: <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>. Plans index:
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

## Status board and handoff (every step)

This run is tracked on a status board, <https://claude.ai/artifact/YZUVcC1vdaWaLGJLsdS2AX>, published from `STATUS.md` at the root of the branch. The plans index says how to keep it current.
- **Find it** with `/status-board` in this worktree. The skill reads the remembered URL from the
  git dir (`status_page.py --url`).
- **Every agent that continues this work updates the board, using `/status-board update`.** Do it
  in the same turn as each state change: a step or task started, in review, committed, landed,
  blocked or unblocked, a plan written, or a ruling.
- A different worktree has a board of its own. Never write to another worktree's board.
  Report to the session that owns the board instead.
- If this worktree has no board, ask the owner rather than creating a second board.

## Global Constraints

- **AGENT.md applies in full:**
  - **Data-driven.** A menu is a table and code interprets it: adding a menu item is adding one row to
    the table of the menu it belongs to, and nothing else. The role mapping, the menu-bar menus, the
    exemptions from the reachability rule and the keys the menu bar keeps are tables too.
  - **Dependency injection.** Native menu presentation and the menu bar's AppKit hand-over are reached
    only through `platform::INativeMenuPresenter` and `platform::IMenuBarIntegration`, each with a null
    implementation defined inline in its header (so it exists on Apple too; every new `platform/` header is
    listed in `add_library(contour_platform …)`, `:15-69`, as `CocoaWindowFrame.hpp` is), a Cocoa `.mm` added inside the
    `if(APPLE)` block of `src/contour/platform/CMakeLists.txt:81-95`, and a non-Apple factory in a `.cpp`
    wholly inside `#ifndef __APPLE__` (the `PlatformAppearance.cpp` shape). Both are built through the
    `platform::AppearanceServices` factories, so every `TestApp` gets the null ones.
  - **Configuration at construction.** `AppMenuBar`, the presenters, and every new member of
    `WindowController` and `ContourGuiApp` take their shape in the constructor. `AppMenuBar::rebuild()` is
    domain state (the menu's rows follow the configuration), not configuration.
  - **`enum class` over `bool`,** with the default enumerator at zero. This step touches
    `ContextMenuEntry`, so its three `bool` members become enums in the same change (§1.7). The only new
    `bool`s are QML-facing `QVariantMap` keys and `Q_PROPERTY`s, documented as that carve-out.
  - **`std::expected`** for fallible calls (`MenuPresentError`, starting at `= 0`).
  - **Doxygen** (`///`, `@param`, `@return`, `@pre`) on every new public declaration; `.hpp` with
    `#pragma once`. Every AppKit call is documented `@pre` main thread.
  - **Zero warnings** under `-Werror`, and **no `NOLINT`**. `slots`, `signals` and `emit` are Qt macros:
    never use them as identifiers.
- **Build** with at most 3 jobs, `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in
  `out/appleclang-debug/CMakeCache.txt:395`): `cmake --build out/appleclang-debug -j 3 --target <target>`.
  Never run two builds at once.
- **Tests:** each task runs only its own filter. The full suite runs in CI on Linux, macOS and Windows.
- **Format** every touched `.cpp`/`.hpp`/`.mm` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i` before
  each commit (CI's version). Check every new or touched `.qml`, `.py`, `.txt`, `CMakeLists.txt` with
  `editorconfig-checker <files>` before its commit.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`,** and clang-tidy enforces
  `cppcoreguidelines-pro-type-member-init`: every new struct member gets `{}` or a value. A designated
  initializer may then omit it, but every new **row** of a table (`ChromeTokenRows`, `SnapshotScripts`,
  `AppearanceServices` in the two null factories) names every field explicitly.
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Counts in tests are derived from the tables** (`SnapshotScripts.size()`, `actionCatalog().size()`,
  `MenuBarMenuCount`, the palette's size), never written as literals.
- **QML:** the size and colour lint gates pass with zero new literals (`check_qml_sizes`, `check_qml_colors`;
  a new file's baseline is 0). Text colours come from tokens or from `SystemPalette` read as `KitDialog.qml`
  does, never from a popup's inherited `palette`. A `Popup`/`Menu` cannot carry `Accessible` or `Keys`
  attached properties: put them on an `Item`. Enums are unreachable through the `chromeStyle` context
  property: QML compares `int` properties.
- **Never edit `vendor/core-cpp`.**
- **TestApp is Classic (ruling R14)** and always passes `nullAppearanceServices()`, so no test can reach the
  Cocoa presenter (whose `present()` blocks in AppKit's tracking loop) or the real menu bar.
- **Every new visual surface is a snapshot row** (`ChromeSnapshotPlan.hpp`), and **every visual task renders
  the Terminal (TUI) row** (§3.2 *TUI row*: in-scene menus on every OS; the macOS menu bar stays native).
- **Qt-free targets stay Qt-free:** everything new in `command/` includes no Qt header.
- **Line numbers** below are at HEAD `d6f91bd6`. A later task locates code an earlier task moved by its name.
- **Commits:** one per task, each ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- After the code changes, run `/simplify` (Task 13).

## Review Focus

1. **A test that pops a real NSMenu hangs the suite.** `-[NSMenu popUpMenuPositioningItem:atLocation:inView:]`
   runs a modal tracking loop until the menu is dismissed. Every `TestApp` must get the null presenter, and
   `appearanceServicesWith()` must name `.menus` and `.menuBar` (their defaults are the native factories).
   Pinned in Task 5 (`nullAppearanceServices()` and `appearanceServicesWith(…)` both give a presenter that
   reports `Unsupported`), Task 12 (both give the null menu-bar integration) and Task 7 (the controller
   cases run in a `TestApp` and assert the in-scene route).
2. **A pick that closes what the menu was opened over.** "Close Tab" or "Close Other Tabs" picked from a
   tab menu, native or in-scene, deletes the tab, and a pane menu's "Close Pane" its pane. The pick must run
   after the menu has closed, against a pinned `TabId`/session that may have died meanwhile, as a no-op.
   Pinned in Task 7 (the pinned tab closed before the pick: nothing happens) and Task 8 (an in-scene pick is
   emitted on `closed`).
3. **The menu bar steals keys from the terminal.** A chord shown in the menu bar is an NSMenu key equivalent.
   If the terminal does not claim it, AppKit fires the menu row, which ignores the binding's mode conditions
   (`Ctrl+C` copies only in Select|Insert; `Ctrl+Alt+K` scrolls only outside the alternate screen, so vim
   gets it). Expected: the terminal claims every key on every host, and only under a global menu bar do
   Quit and Preferences go to the menu. Only a chord with ⌃ or ⌘ becomes a key equivalent at all, so
   Escape and ⇧↓ still reach the palette's and Settings' text fields. Pinned in Tasks 11 and 12.
4. **The tab menu acts on the active tab.** "Close Other Tabs" from a background tab's menu must keep the
   right-clicked tab, and "Rename…" must open that tab's editor. Pinned in Task 7.
5. **Linux and Windows change.** The pane and title-bar menus must offer exactly today's rows, no menu bar is
   created, and the `ShortcutOverride` claim takes every key there, which changes nothing because nothing
   off macOS consumes shortcuts (no `QShortcut`, no QML `Shortcut`); only under `Global` does any key go to
   the menu. Pinned in Task 1 (the existing
   `[contextmenu]` and title-bar cases pass unedited apart from the field renames), Task 10
   (`menuBarPlacementFor` is `None` off macOS) and Task 11 (`None` never yields `Menu`).

Also pinned, beyond the five: a menu-bar menu opened after a tab was added shows the new state (Task 12,
refresh on `aboutToShow`); a chord Qt has no key for (Hyper, Meta) is dropped rather than shown wrong
(Task 12).

---

## Spec resolutions (read before Task 1)

These settle where the spec is silent or the code disagrees with it. Implementers follow them; those marked
**(owner)** are repeated under *Owner decisions*.

- **R-a. The native presenter builds an `NSMenu` itself (owner).** §3.2 says the presenter "realises the tree
  as a `QMenu`, which on macOS is an NSMenu". Qt documents no such guarantee for `QMenu::popup()`, and
  `QMenu::toNSMenu()` (`QtWidgets/qmenu.h:170-171`) returns a menu only where Qt already backs one. A `QMenu`
  also needs a `QApplication`, which `contour_gui_test` does not have (`test_main.cpp:110`). So the `.mm`
  builds the `NSMenu` from the `MenuTree` and pops it with `popUpMenuPositioningItem:atLocation:inView:`.
  Nativeness is then a fact the `[cocoa]` test asserts. The pure `MenuTree` stays, as §3.2 requires.
- **R-b. Where the pieces live.** §3.2 names `window/NativeMenuPresenter`. Principle 4 puts what asks the
  OS in `platform/`, and `platform/` sits below `window/`. So `MenuTree` lives in `command/` (Qt-free, beside
  the entries it is built from), the presenter interface and its Cocoa backend in `platform/`, and the
  routing decision in `window/MenuRouting.hpp`. `contour_platform` links `contour_command`.
  `display/ShortcutOverride.hpp` (Task 11) is a pure decision outside §1.3's list (`window/`, `command/`,
  `vtbackend/`), accepted as 1a's F10 accepted `config/`: it decides about a `QKeyEvent`, and sits beside
  its consumer `TerminalDisplay`, which may not include `window/`.
- **R-c. "Zero visual change on Linux and Windows" (owner).** It is read as "no restyle": menus keep
  `ContourMenu`/`PopupSurface` and the in-scene path. The tab menu's content is §3.2's table on every OS, so
  Linux and Windows gain the Tab Color swatch row and "Move Tab to New Window". The pane and title-bar menus
  keep exactly today's rows.
- **R-d. New actions.** A menu row carries an action, and four rows have none: "Close Other Tabs", "Close Tabs
  to the Right", "Move Tab to New Window" (today `WindowController::closeOtherTabs/closeTabsToRight/tearOffTab`,
  `WindowController.hpp:220`, `:258-259`) and "About". They become catalogue actions `CloseOtherTabs`,
  `CloseTabsToRight`, `MoveTabToNewWindow` and `ShowAbout`. That also makes them bindable and reachable
  from the palette.
- **R-e. The tab menu acts on the right-clicked tab.** Today's `TabContextMenu.qml` calls the controller with
  its `tabIndex`. The table's rows carry tab actions, and `WindowController::runTabAction(row, action)` is the
  one place that maps a tab action to that row. A public `runTabAction(TabId, action)` resolves a tab to its
  row and does nothing for a tab that has gone. The menu pins the tab's `TabId` and picks through it, and so
  do the session handlers of the three new tab actions, with the acting session's tab.
- **R-f. About is an in-scene dialog (owner).** The catalogue has no About action, and none of `src/contour`
  shows one. `ShowAbout` opens a `KitDialog` ("Contour", the version, one OK button) on every row, one
  implementation. AppKit's standard About panel would be the macOS-native alternative.
- **R-g. "Every catalogue action is reachable from a menu".** "A menu" means the union of the menu bar and the
  four context menus (pane, title bar, tab, new tab). A parameterized action is reached through its per-name
  rows. Five are exempt, as data with a reason each: `CreateSelection`, `SendChars` and `WriteScreen` (they
  need user-written text); `MoveTabTo` (the drag and Move Tab Left/Right cover it); `OpenContextMenu` (it
  opens the menu it would be a row of). No exempt action may appear in a menu.
- **R-h. "No chord appears twice".** A chord is bound per mode, so two commands can share one: the defaults
  bind `Ctrl+C` to `CopySelection` and to `CancelSelection` (`Config.hpp:1562-1578`). That pair does not
  collide today only because `CancelSelection`'s first binding is Escape (`:1291-1299`), and the chord index
  keeps a command's first. A user's config can make them collide. So `buildMenuTree` attaches a chord to
  the first command row that shows it, walking depth-first, and a later row with the same chord shows none.
  The `[menubar]` test checks this over both editions' defaults and a crafted collision.
- **R-i. Chords appear only in the menu bar.** In-scene menus have never shown chords, so none appear there
  (R-c). Native context menus show none either: macOS context menus rarely do, and this keeps key
  equivalents out of popups. The `MenuTree` carries the chord for the menu bar. There, only a chord holding
  ⌃ or ⌘ becomes a key equivalent; a row whose chord has neither (e.g. "Clear Selection ⎋", "⇧↓") shows
  none, so AppKit cannot fire it from a text field that does not claim the key.
- **R-j. The terminal keeps its keys (Review Focus 3).** On macOS, Qt fills an NSMenu item's key equivalent
  from its `QAction` shortcut. Its menu delegate offers the key to the focus object as a `ShortcutOverride`
  first, and does not fire the item if the focus object accepts. So `TerminalDisplay` accepts every
  `ShortcutOverride`, on every host, except, under a global menu bar, those matching
  `QKeySequence::Quit` and `QKeySequence::Preferences`. The role is the authority for those two keys: the
  Quit and Settings… rows carry `QKeySequence::Quit`/`Preferences`, never the binding's chord (the
  defaults bind Ctrl+Shift+Q and Ctrl+Shift+,, which would make them ⌃⇧Q and ⌃⇧,). One table,
  `MenuOwnedKeys` (Task 11), names the keys both the claim and the menu bar read. A binding then keeps its
  mode conditions, ⌘Q still quits through the app menu (as Qt's default app menu does today), and ⌘,
  opens Settings.
- **R-k. The application menu.** Qt merges a `QAction` with a menu role into the application menu. The
  Contour table menu's rows with `MenuItemRole::None` get `QAction::ApplicationSpecificRole`, and the Contour
  `QMenu`'s own title is hidden (`menuAction()->setVisible(false)`), so "Contour" appears once.
- **R-l. The title-bar mirror.** "The title-bar menu's rows are also mirrored into View › Tab Bar" means its
  "Tab Bar" submenu: the visibility modes, then Position. Both menus build it with one function,
  `buildTabBarMenu`.
- **R-m. A pick runs once its menu has closed.** In-scene, `ActionContextMenu` emits `picked` on `closed`, or
  at once when the menu was not open (offscreen tests trigger rows directly). This replaces the special case
  in `TabContextMenu.qml:20-33` (opening the colour flyout from a closing menu lost its keyboard grab).
  Natively, the controller posts the presentation itself, so `present()` runs on a clean stack once the
  press handler that asked for the menu has returned (AppKit's tracking loop swallows the mouse-up, and runs
  Qt events that may destroy the TabItem under a QML handler's frame), and it posts the pick to its own
  event queue after `present()` returns.
- **R-n. Where the menu bar exists.** Only on a macOS host (`menuBarPlacementFor(HostPlatform::MacOS) ==
  Global`), under `ui_style: terminal` too: "the macOS menu bar stays native" (§3.2 TUI row). The
  `layout.menu` token governs context menus only.
- **R-o. `[display]` runs get a `QApplication`.** §2.6 wants the `QMenuBar` realisation exercised by
  `[display]` smoke tests. A widget needs a `QApplication`, so `test_main.cpp` constructs one, a
  `QGuiApplication` subclass, when `displayTestsEnabled()`. Offscreen runs keep `QGuiApplication`.
- **R-p. Snapshots.** The `tab-menu` snapshot shows the in-scene menu on every row, the macOS row included,
  where the app itself pops an `NSMenu`. The native menu and the menu bar live outside the window and have
  no snapshot: the `[cocoa]` outline test and the `[display][menubar]` smoke test are their acceptance.

---

### Task 1: Menu rows speak enums, and carry a role and a swatch

**Files:**
- Modify: `src/contour/command/ContextMenu.hpp:105-128` — the entry and the three new enums.
- Modify: `src/contour/command/ContextMenuTable.hpp:59-229` — `Row::role` and `withRole()`; `buildRows`,
  `hostRows`, the factories.
- Modify: `src/contour/command/ContextMenu.cpp` (every `ContextMenuEntry { … }`, e.g. `:145-151`),
  `src/contour/command/TitleBarContextMenu.cpp:44-112`.
- Modify: `src/contour/window/ContextMenuModel.cpp:34-41` — the QML keys stay `enabled`/`checkable`/`checked`.
- Test: `src/contour/command/ContextMenu_test.cpp`, `src/contour/command/TitleBarContextMenu_test.cpp` (field
  renames only), and new `src/contour/window/ContextMenuModel_test.cpp` (add to `contour_gui_test`,
  `src/contour/CMakeLists.txt:760-818`).

**Interfaces:**
- Produces (namespace `contour::command`, `ContextMenu.hpp`):
  - `enum class RowAvailability : std::uint8_t { Enabled = 0, Disabled };`
  - `enum class RowCheck : std::uint8_t { None = 0, Unchecked, Checked };` — `None` draws no check column.
  - `enum class MenuItemRole : std::uint8_t { None = 0, About, Preferences, Quit };`
  - `ContextMenuEntry` loses `bool enabled, checkable, checked` and gains
    `RowAvailability availability = RowAvailability::Enabled;`, `RowCheck check = RowCheck::None;`,
    `MenuItemRole role = MenuItemRole::None;`, `std::optional<vtbackend::RGBColor> swatch {};` (a command row
    drawn as a colour swatch; `#include <vtbackend/core/Color.hpp>`, Qt-free). Every construction site is a
    designated initializer, which must follow declaration order, so the members are declared in the order
    `kind, title, action, availability, check, role, swatch, children`.
  - `detail::MenuTable<State>::Row` gains `MenuItemRole role = MenuItemRole::None;`, declared after `checked`, and
    `[[nodiscard]] Row withRole(MenuItemRole role) const`; `buildRows` copies it onto the entry, and maps
    `enabled`/`checked` predicates to `availability`/`check` (`checked == nullptr` → `RowCheck::None`).
- `toContextMenuModel` keeps its signature in this task. It writes `enabled = availability == Enabled`,
  `checkable = check != None`, `checked = check == Checked` (the QML boundary keeps booleans).

- [ ] **Step 1: Write the failing tests.**
  - `ContextMenu_test.cpp` / `TitleBarContextMenu_test.cpp`: replace every read of `.enabled`, `.checkable`,
    `.checked` with the enum. For example `CHECK(copy.availability == RowAvailability::Disabled)` where
    `CHECK_FALSE(copy.enabled)` stood, and `CHECK(row.check == RowCheck::Checked)` where
    `CHECK(row.checkable); CHECK(row.checked)` stood. No assertion changes meaning.
  - New `ContextMenu.rolesPassThrough`, `[contextmenu]`, in `ContextMenu_test.cpp`: a
    `detail::MenuTable<ContextMenuState>` with one `Table::command(actions::Quit {}).withRole(MenuItemRole::Quit)`
    row; `buildRows` yields one entry whose `role == MenuItemRole::Quit`, and a plain row yields `None`.
  - New `ContextMenuModel_test.cpp`, `[contextmenumodel]`: entries with `{Enabled, None}`,
    `{Disabled, Unchecked}`, `{Enabled, Checked}` give QVariant rows whose
    `enabled/checkable/checked` are `true/false/false`, `false/true/false` and `true/true/true`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test contour_gui_test`

Expected: compile failure, because `RowAvailability` does not exist.

- [ ] **Step 3: Implement** the enums and fields, `withRole`, and every construction site. A designated
  initializer that named `.enabled = true, .checkable = false, .checked = false` now names
  `.availability = RowAvailability::Enabled, .check = RowCheck::None`. A generated checkable row writes
  `.check = info.value == state.tabBarVisibility ? RowCheck::Checked : RowCheck::Unchecked`.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[contextmenu],[menu]" && out/appleclang-debug/src/contour/contour_gui_test "[contextmenumodel],[contour][gui][qml]"`

Expected: PASS. `[contour][gui][qml]` includes "Terminal context menu builds its rows from the C++ model",
which proves the QML still sees booleans.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/command/ContextMenu*.{hpp,cpp} src/contour/command/TitleBarContextMenu*.cpp src/contour/window/ContextMenuModel*.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/command src/contour/window/ContextMenuModel* src/contour/CMakeLists.txt
git commit -m "command: menu rows say enabled, checked and their role with enums"
```

---

### Task 2: The four actions the menus need

**Files:**
- Modify: `src/contour/config/Actions.hpp` — the structs (beside `CloseAllTabs`, `:143`), the `Action`
  variant (`:161-…`, after `OpenCommandOutput`), `NonRepeatableActionConcept` (`:305-…`), `documentation::`
  (beside `:577`), `actionCatalog()` rows (end of the array, `:877`).
- Modify: `src/contour/session/TerminalSession.hpp:1176-1190` and `.cpp` (beside `operator()(actions::CloseTab)`,
  `:5368`) — four handlers.
- Modify: `src/contour/session/TerminalSessionManager.hpp/.cpp` (beside `beginTabTitleEdit`, `:968`) —
  `runTabAction(actions::Action const&, TerminalSession* acting)` and `showAbout(TerminalSession* acting)`.
- Modify: `src/contour/window/WindowController.hpp/.cpp` — `runTabAction`, `showAbout`, `aboutRequested`.
- Test: `src/contour/config/Actions_test.cpp`; new `src/contour/window/WindowMenus_test.cpp`. It builds a
  `TestApp`, as `MultiWindow_test.cpp` does, so it goes in `contour_gui_test_heavy_sources`
  (`src/contour/CMakeLists.txt:848-870`), not in the suite's own sources.

**Interfaces:**
- Produces (namespace `contour::actions`): `struct CloseOtherTabs {};`, `struct CloseTabsToRight {};`,
  `struct MoveTabToNewWindow {};`, `struct ShowAbout {};`, appended to `Action` in that order with catalogue
  names `"CloseOtherTabs"`, `"CloseTabsToRight"`, `"MoveTabToNewWindow"`, `"ShowAbout"`. All four join
  `NonRepeatableActionConcept`.
- Documentation strings: "Closes every tab in this window except the current one.", "Closes every tab to the
  right of the current one.", "Moves the current tab into a new window of its own. Does nothing with a single
  tab.", "Shows the version of Contour."
- Produces (`window::WindowController`):
  - `void runTabAction(int row, actions::Action const& action);` maps each tab action to row @p row:
    `SetTabTitle` → `emit tabTitleEditRequested(row)`; `SetTabColor` with a colour →
    `setTabColor(row, toQColor(*color))`, without one → `emit tabColorPickRequested(row)`; `ResetTabColor` →
    `resetTabColor(row)`; `MoveTabToNewWindow` → `tearOffTab(row)`; `CloseTab` → `closeTabAtIndex(row)`;
    `CloseOtherTabs` → `closeOtherTabs(row)`; `CloseTabsToRight` → `closeTabsToRight(row)`. Any other action
    goes to `activeSession()->executeAction(action)`. One `std::visit` with these overloads is the only
    mapping from a tab action to a tab.
  - `void runTabAction(vtworkspace::TabId tab, actions::Action const& action);` resolves @p tab to its row
    with the private `rowOfTab` (`WindowController.hpp:729`) and calls the row overload. It does nothing when
    the tab has no row (`rowOfTab` gives -1 for a tab that has gone).
  - `void showAbout();` → `emit aboutRequested();`, with the new signal `void aboutRequested();`.
- Produces (`session::TerminalSessionManager`): `void runTabAction(actions::Action const& action, TerminalSession* acting);`
  finds the acting session's tab (`tabHostingSession`) and calls the hosting controller's
  `runTabAction(tabHostingSession(acting)->id(), action)`; the manager never sees a row, since `rowOfTab`
  is private. `void showAbout(TerminalSession* acting);` calls the hosting controller's `showAbout()`.
- `TerminalSession::operator()` for the three tab actions calls `_manager->runTabAction(action, this)`; for
  `ShowAbout`, `_manager->showAbout(this)`. Each returns `true`.

- [ ] **Step 1: Write the failing tests.**
  - `Actions_test.cpp`, `[actions][catalog]`: `fromString("CloseOtherTabs")`, `"CloseTabsToRight"`,
    `"MoveTabToNewWindow"` and `"ShowAbout"` each hold the matching alternative. The existing
    "every action in the catalog round-trips" and "sits at its own variant index" cases cover the rest.
  - `Actions_test.cpp`, `[actions][repeat]`: each of the four is filtered out by `filterRepeatableActions`
    on `Repeat`.
  - `WindowMenus_test.cpp`, tag `[contour][windowmenus][tabaction]`, in a `TestApp` with a
    `ScopedController` and model tabs (`manager.model().createTab(controller.windowId())`, as
    `MultiWindow_test.cpp:84-90` does):
    - three tabs; `runTabAction(1, CloseOtherTabs {})` leaves `count() == 1`, and the survivor's `TabId` is
      the one that was row 1;
    - three tabs; `runTabAction(0, CloseTabsToRight {})` leaves `count() == 1`;
    - `runTabAction(2, SetTabTitle {})` emits `tabTitleEditRequested(2)` once, and `SetTabColor {}` emits
      `tabColorPickRequested(2)`, both observed with `QSignalSpy`;
    - `runTabAction(1, SetTabColor { .color = 0xFF0000_rgb })` colours row 1, and `ResetTabColor {}` clears it
      (read through `Roles::ColorRole`);
    - two tabs; `runTabAction(1, MoveTabToNewWindow {})`, then a second `ScopedController b`:
      `manager.consumePendingTransplant(b.controller)` is `true`, and `b->count() == 1`;
    - three tabs; `runTabAction(id, CloseTab {})` with the `TabId` of the third model tab (read from
      `manager.model()`, as the survivor check above is) leaves `count() == 2`, and the same call again, with
      that tab gone, changes nothing;
    - `showAbout()` emits `aboutRequested()` once.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test contour_gui_test`

Expected: compile failure on `actions::CloseOtherTabs`.

- [ ] **Step 3: Implement** the actions, the handlers, the manager forwarding and the controller mapping.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[actions]" && out/appleclang-debug/src/contour/contour_gui_test "[windowmenus]"`

Expected: PASS.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/Actions.hpp src/contour/config/Actions_test.cpp src/contour/session/TerminalSession.{hpp,cpp} src/contour/session/TerminalSessionManager.{hpp,cpp} src/contour/window/WindowController.{hpp,cpp} src/contour/window/WindowMenus_test.cpp
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/config src/contour/session src/contour/window src/contour/CMakeLists.txt
git commit -m "actions: close other tabs, close tabs to the right, move a tab to a new window, show About"
```

---

### Task 3: The tab menu, the new-tab menu and the tab bar menu as tables

**Files:**
- Create: `src/contour/command/TabMenus.hpp`, `src/contour/command/TabMenus.cpp`; list them in
  `src/contour/command/CMakeLists.txt:8-18`.
- Modify: `src/contour/command/TitleBarContextMenu.hpp/.cpp` — `tabBarVisibilityRows`, `tabBarPositionRows`,
  `tabBarTemplate`, `tabBarRows` (`TitleBarContextMenu.cpp:55-116`) become the public `buildTabBarMenu`. The
  title bar's "Tab Bar" submenu calls it.
- Test: new `src/contour/command/TabMenus_test.cpp` (add to `contour_test`, `src/contour/CMakeLists.txt:1131-1176`);
  `TitleBarContextMenu_test.cpp` (one new case).

**Interfaces:**
- Produces (namespace `contour::command`, `TabMenus.hpp`):
  - `enum class UserColorPresence : std::uint8_t { Absent = 0, Present };`
  - `struct TabContextMenuState { int tabIndex = 0; int tabCount = 0; std::vector<vtbackend::RGBColor> palette {}; std::optional<vtbackend::RGBColor> color {}; UserColorPresence userColor = UserColorPresence::Absent; };`
    `palette` is `SessionModel::colorPalette()` in order (`vtworkspace/SessionModel.cpp:77`). `color` is the
    tab's effective colour (`Tab::color()`, `vtworkspace/Tab.hpp:229`). `userColor` says whether the user
    gave it one (`Tab::color(TabColorSource::User)`).
  - `[[nodiscard]] std::vector<ContextMenuEntry> buildTabContextMenu(TabContextMenuState const& state);`
  - `struct NewTabMenuState { std::vector<std::string> profileNames {}; };`
  - `[[nodiscard]] std::vector<ContextMenuEntry> buildNewTabMenu(NewTabMenuState const& state);`
- Produces (`TitleBarContextMenu.hpp`):
  - `struct TabBarMenuState { config::TabBarVisibility visibility = config::TabBarVisibility::Always; config::TabBarPosition position = config::TabBarPosition::Top; };`
  - `[[nodiscard]] std::vector<ContextMenuEntry> buildTabBarMenu(TabBarMenuState const& state);`
    It gives one checkable row per visibility mode, a separator, then the "Position" submenu, exactly as
    `tabBarRows` does today.

**The tab menu table** (§3.2's rows, in order; separators collapse with `dropRedundantSeparators`):

| Row | Action | Rule |
|---|---|---|
| "Rename…" | `SetTabTitle {}` | — |
| "Tab Color" › one row per palette entry | `SetTabColor { .color = c }`, `swatch = c`, title `vtbackend::formatColor(c)` | checked when `c == state.color` |
| › separator, "Custom…" | `SetTabColor {}` | — |
| › "Reset" | `ResetTabColor {}` | enabled when `userColor == Present` |
| "Move Tab to New Window" | `MoveTabToNewWindow {}` | enabled when `tabCount > 1` |
| separator | | |
| "Close Tab" | `CloseTab {}` | — |
| "Close Other Tabs" | `CloseOtherTabs {}` | enabled when `tabCount > 1` |
| "Close Tabs to the Right" | `CloseTabsToRight {}` | enabled when `tabIndex < tabCount - 1` |

**The new-tab menu table** (today's `NewTabMenu.qml:57-85` as data): one row per profile, titled
`std::format("New tab — {}", name)`, running `CreateNewTab { .profileName = name }`; a separator; "Settings"
running `OpenConfiguration {}` (4a removes it).

- [ ] **Step 1: Write the failing tests** in `TabMenus_test.cpp`, tag `[tabmenu]`:
  - `tabMenu.rowsInOrder`: with `tabIndex = 0`, `tabCount = 3` and a three-colour palette, the top-level
    titles are `{"Rename…", "Tab Color", "Move Tab to New Window", "", "Close Tab", "Close Other Tabs",
    "Close Tabs to the Right"}` (the separator has an empty title and kind `Separator`).
  - `tabMenu.swatchesFollowThePalette`: "Tab Color" has `palette.size()` swatch rows, then a separator,
    "Custom…" and "Reset". Swatch `i` carries `SetTabColor { .color = palette[i] }` and `swatch == palette[i]`.
    Only the swatch equal to `state.color` is `Checked`; the others are `Unchecked`.
  - `tabMenu.resetNeedsTheUsersColour`: "Reset" is `Disabled` with `userColor = Absent`, `Enabled` with `Present`.
  - `tabMenu.singleTab`: with `tabCount = 1`, "Move Tab to New Window", "Close Other Tabs" and "Close Tabs to
    the Right" are `Disabled`, and "Close Tab" is `Enabled`.
  - `tabMenu.lastTab`: with `tabIndex = 2`, `tabCount = 3`, only "Close Tabs to the Right" is `Disabled`.
  - `newTabMenu.profilesThenSettings`, `[newtabmenu]`: profiles `{"dark", "light"}` give "New tab — dark",
    "New tab — light", a separator and "Settings", with the `CreateNewTab` profile names and
    `OpenConfiguration {}`. No profiles gives "Settings" alone.
  - `TitleBarContextMenu_test.cpp`, `[contour][titlebar][menu]`, "the tab bar submenu is buildTabBarMenu":
    the title bar menu's "Tab Bar" children equal `buildTabBarMenu` for the same modes, compared by
    kind, title, `commandId(action)` and `check`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test`

Expected: compile failure, because `TabMenus.hpp` does not exist.

- [ ] **Step 3: Implement** both builders over `detail::MenuTable`, using the `swatch` field from Task 1, and
  `buildTabBarMenu` over a `detail::MenuTable<TabBarMenuState>`. The title bar's "Tab Bar" row becomes
  `Table::submenu("Tab Bar", [](TitleBarContextMenuState const& s) { return buildTabBarMenu({ .visibility = s.tabBarVisibility, .position = s.tabBarPosition }); })`;
  a captureless lambda converts to `ChildBuilder`.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[tabmenu],[newtabmenu],[contour][titlebar][menu]"`

Expected: PASS.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/command/TabMenus*.{hpp,cpp} src/contour/command/TitleBarContextMenu*.{hpp,cpp}
editorconfig-checker src/contour/command/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/command src/contour/CMakeLists.txt
git commit -m "command: the tab menu, the new-tab menu and the tab bar menu are tables"
```

---

### Task 4: `MenuTree` and the chord index

**Files:**
- Create: `src/contour/command/MenuTree.hpp`, `src/contour/command/MenuTree.cpp`; list them in
  `src/contour/command/CMakeLists.txt`.
- Modify: `src/contour/command/Shortcut.hpp:148-172`, `src/contour/command/Shortcut.cpp:40-127`. The chord
  index is the primitive, and both `shortcutIndex` overloads render it, with no change in behaviour.
- Test: new `src/contour/command/MenuTree_test.cpp` (add to `contour_test`); `Shortcut_test.cpp` (one case).

**Interfaces:**
- Produces (`Shortcut.hpp`):
  - `struct ShortcutChord { vtbackend::Modifiers modifiers {}; ShortcutInput input {}; [[nodiscard]] bool operator==(ShortcutChord const&) const = default; };`
  - `using ChordIndex = std::unordered_map<std::string, ShortcutChord>;` (command id → chord).
  - `[[nodiscard]] ChordIndex chordIndex(config::InputMappings const& mappings);` and
    `[[nodiscard]] ChordIndex chordIndex(config::Config const& config);`. They follow the same rules as
    `shortcutIndex`: single-action key and char bindings, first wins, then the enabled fallback rows the
    user's table leaves unbound.
  - `[[nodiscard]] std::string shortcutText(ShortcutChord const& chord, ShortcutStyle style);`
- Produces (`MenuTree.hpp`, namespace `contour::command`):

```cpp
/// One row of a resolved menu: what every presenter draws.
struct MenuNode
{
    ContextMenuEntryKind kind = ContextMenuEntryKind::Command;
    std::string title {};
    RowAvailability availability = RowAvailability::Enabled;
    RowCheck check = RowCheck::None;
    MenuItemRole role = MenuItemRole::None;
    std::optional<vtbackend::RGBColor> swatch {};
    std::optional<ShortcutChord> chord {}; ///< Command rows: the chord that runs it, if shown.
    std::string shortcut {};               ///< @ref chord in the tree's style; empty without one.
    std::size_t actionIndex = 0;           ///< Command rows: index into MenuTree::actions.
    std::vector<MenuNode> children {};     ///< Submenu rows.
    [[nodiscard]] bool operator==(MenuNode const&) const = default;
};

/// A resolved menu and the actions its command rows run, by MenuNode::actionIndex.
struct MenuTree
{
    std::vector<MenuNode> nodes {};
    std::vector<actions::Action> actions {};
};

[[nodiscard]] MenuTree buildMenuTree(std::span<ContextMenuEntry const> entries,
                                     ChordIndex const& chords,
                                     ShortcutStyle style);
```

  - `actionIndex` follows a depth-first pre-order walk (a submenu's rows come before its next sibling), the
    order `toContextMenuModel` assigns ids in today (`ContextMenuModel.cpp:26-41`).
  - A command row gets `chords.find(commandId(action))`, unless an earlier row in the walk already shows
    that chord (R-h). `shortcut = shortcutText(*chord, style)`.

- [ ] **Step 1: Write the failing tests.**
  - `Shortcut_test.cpp`, `[contour][palette]`, "shortcutIndex is the chord index rendered": for
    `config::Config {}` and for `Config { .defaultsEdition = DefaultsEdition::Classic }`, every
    `shortcutIndex(config, style)` entry equals `shortcutText(chordIndex(config).at(id), style)` for both
    styles, and both maps have the same keys.
  - `MenuTree_test.cpp`, `[menutree]`:
    - `menuTree.actionsInWalkOrder`: entries `[A, Submenu[B, C], D]` give `actions` in the order `A, B, C, D`,
      with `actionIndex` values 0, 1, 2, 3 on the matching nodes.
    - `menuTree.copiesEveryColumn`: the title, availability, check, role and swatch of each entry reach its node.
    - `menuTree.chordFromTheIndex`: with
      `chords = { { "CreateNewTab", { .modifiers = Modifiers { Modifier::Control, Modifier::Shift }, .input = U'T' } } }`,
      a `CreateNewTab {}` row has that `chord`, and `shortcut == "Ctrl+Shift+T"` in `Text` and `"⌃⇧T"` in
      `MacGlyphs`. A row with no indexed chord has neither.
    - `menuTree.aChordIsShownOnce`: two rows whose ids map to the same chord, the first in a submenu: only
      the first in walk order has a chord.
    - `menuTree.separatorsAndSubmenusCarryNoAction`: `actions.size()` equals the number of command nodes,
      counted by walking the result.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test`

Expected: compile failure on `chordIndex` and `MenuTree.hpp`.

- [ ] **Step 3: Implement.** `indexBinding` (`Shortcut.cpp:44-56`) stores the `ShortcutChord`, and
  `shortcutIndex` maps the chord index through `shortcutText`. `buildMenuTree` keeps the chords it has
  attached in a `std::vector<ShortcutChord>`.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[menutree],[contour][palette],[command]"`

Expected: PASS. The existing `[contour][palette]` shortcut cases pin the unchanged palette column.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/command/MenuTree*.{hpp,cpp} src/contour/command/Shortcut*.{hpp,cpp}
editorconfig-checker src/contour/command/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/command src/contour/CMakeLists.txt
git commit -m "command: resolve a menu into one tree of rows, chords and actions"
```

---

### Task 5: The presentation token, the presenter interface and the routing

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp:36-40` (beside `DialogPlacement`), `:213-218`
  (`LayoutTokens`), the four rows' `.layout` (`:320`, `:340`, `:360`, `:388`).
- Create: `src/contour/platform/NativeMenuPresenter.hpp` (the interface, `MenuAnchor`, `MenuPicked`,
  `MenuPresentError`, `NullNativeMenuPresenter` defined inline) and `.cpp` (`makeNativeMenuPresenter`,
  returning the null presenter; Task 6 wraps it in `#ifndef __APPLE__` when the Cocoa one arrives).
- Modify: `src/contour/platform/AppearanceServices.hpp` — a `menus` factory, named in
  `nullAppearanceServices()`.
- Modify: `src/contour/test/AppearanceFixtures.hpp:99-114` — `appearanceServicesWith` names `.menus` (null).
- Modify: `src/contour/platform/CMakeLists.txt:15-69` (the pair, in `add_library(contour_platform …)`),
  `:99-108` (link `contour_command`).
- Create: `src/contour/window/MenuRouting.hpp` (header-only); list it in `src/contour/CMakeLists.txt`'s
  `_header_files` (`:264-…`).
- Create: `src/contour/test/MenuFixtures.hpp` — `RecordingMenuPresenter`.
- Modify: `src/contour/ContourGuiApp.hpp/.cpp` — `_nativeMenus`, `nativeMenuPresenter()`, `menuPresentation()`.
- Test: `src/contour/config/ChromeTokenGroups_test.cpp` (the row pin `chromeTokens.everyRowMatchesTheDesignKit`,
  `:85`, gains the column), new `src/contour/window/MenuRouting_test.cpp` (add to `contour_gui_test`),
  `src/contour/platform/AppearanceSources_test.cpp` (one case).

**Interfaces:**
- Produces (`config`): `enum class MenuPresentation : uint8_t { InScene = 0, Native };` and
  `LayoutTokens::menu = MenuPresentation::InScene`. The rows: MacOS `Native`; Fluent, ContourLinux, Terminal
  `InScene`. Each row's `.layout` names both fields, e.g. `.layout = { .dialog = DialogPlacement::Sheet, .menu = MenuPresentation::Native }`.
  It is not published to QML: only C++ reads it.
- Produces (namespace `contour::platform`, `NativeMenuPresenter.hpp`):

```cpp
/// Why a menu was not shown natively; the caller then shows it in the scene.
enum class MenuPresentError : uint8_t
{
    Unsupported = 0, ///< This platform has no native menu presenter.
    NoWindow,        ///< The anchor names a position but no window to place it in.
};

/// Where a menu opens: @ref position in @ref window's logical coordinates, or at the pointer when absent.
struct MenuAnchor
{
    QWindow* window = nullptr;
    std::optional<QPointF> position {};
};

/// Called with the picked command row's MenuNode::actionIndex; not called when the menu is dismissed.
using MenuPicked = std::function<void(std::size_t actionIndex)>;

class INativeMenuPresenter
{
  public:
    virtual ~INativeMenuPresenter() = default;
    /// Shows @p tree as a native menu at @p anchor.
    /// @pre GUI (main) thread. On macOS this blocks in AppKit's menu tracking loop until the menu closes,
    ///      and calls @p onPicked after that loop returns, never inside it.
    [[nodiscard]] virtual std::expected<void, MenuPresentError> present(command::MenuTree const& tree,
                                                                        MenuAnchor const& anchor,
                                                                        MenuPicked const& onPicked) = 0;
};

/// The presenter for platforms without native menus: every request is Unsupported.
class NullNativeMenuPresenter final: public INativeMenuPresenter { /* present() → std::unexpected(Unsupported), inline */ };

[[nodiscard]] std::unique_ptr<INativeMenuPresenter> makeNativeMenuPresenter();
```

- `AppearanceServices` gains
  `std::function<std::unique_ptr<INativeMenuPresenter>()> menus = makeNativeMenuPresenter;`.
  `nullAppearanceServices()` names `.menus = [] { return std::make_unique<NullNativeMenuPresenter>(); }`.
- Produces (namespace `contour::window`, `MenuRouting.hpp`):
  - `enum class MenuRoute : uint8_t { InScene = 0, Native };`
  - `[[nodiscard]] inline MenuRoute presentMenu(config::MenuPresentation presentation, platform::INativeMenuPresenter& presenter, command::MenuTree const& tree, platform::MenuAnchor const& anchor, platform::MenuPicked const& onPicked);`
    It returns `InScene` without calling the presenter when `presentation` is `InScene`. Otherwise it calls
    `present()` and returns `Native` on success, `InScene` on any error. The fallback is silent: the null
    presenter is the normal case off Apple.
- Produces (`ContourGuiApp`): `[[nodiscard]] platform::INativeMenuPresenter& nativeMenuPresenter() noexcept;`.
  The member `std::unique_ptr<platform::INativeMenuPresenter> _nativeMenus` is built in the constructor
  from `appearanceServices.menus()` and declared beside `_platformAppearance` (`ContourGuiApp.hpp:946-947`).
  Also `[[nodiscard]] config::MenuPresentation menuPresentation() const noexcept`, which returns
  `config::ChromeTokenRows[static_cast<size_t>(_chromeStyle)].layout.menu`. The row is fixed once
  `terminalGuiAction` resolves it (`ContourGuiApp.cpp:2209-2210`); in a `TestApp` it stays the default
  `ContourLinux`, hence `InScene`.
- Produces (`test/MenuFixtures.hpp`, namespace `contour::test`):
  `class RecordingMenuPresenter final: public platform::INativeMenuPresenter`. It holds
  `std::expected<void, platform::MenuPresentError> result {}`, records each call in
  `std::vector<command::MenuTree> trees` and `std::vector<platform::MenuAnchor> anchors`, keeps the last
  `MenuPicked` in `platform::MenuPicked lastPicked {}`, and offers `void pick(std::size_t index) const`,
  which calls it.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp`: `chromeTokens.everyRowMatchesTheDesignKit` pins `layout.menu` per row
    (MacOS `Native`, the other three `InScene`). New `chromeTokens.menuPresentationIgnoresPreferences`,
    `[chrometokens]`: for every row and every combination of the three accessibility preferences,
    `resolveChromeTokens(row, prefs).layout.menu == ChromeTokenRows[row].layout.menu`.
  - `AppearanceSources_test.cpp`, `[appearance-source]`: `nullAppearanceServices().menus()->present(…)`
    returns `std::unexpected(MenuPresentError::Unsupported)` and never calls `onPicked`. The same holds for
    `appearanceServicesWith(accessibility, appearance).menus()` (`AppearanceFixtures.hpp`), so the fixture
    cannot reach the native factory.
  - `MenuRouting_test.cpp`, `[menurouting]`, with a `RecordingMenuPresenter`:
    - `InScene` → `MenuRoute::InScene`, and `trees.empty()`;
    - `Native` with `result = {}` → `MenuRoute::Native`, one recorded tree, and the anchor passed through.
      `MenuTree` has no `operator==` (most `actions::Action` alternatives have none, and adding them is out
      of scope), so the check is `trees.front().nodes == tree.nodes` (`MenuNode` defaults `==`) plus the
      actions compared element-wise by `commandId`;
    - `Native` with `result = std::unexpected(Unsupported)` → `InScene`;
    - `Native`, `pick(2)` → the callback handed to `presentMenu` receives 2.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test contour_gui_test`

Expected: compile failure on `MenuPresentation` and `NativeMenuPresenter.hpp`.

- [ ] **Step 3: Implement** the token, the interface, the null presenter and its factory, the services field
  (named in both null factories), `MenuRouting.hpp`, the fixture and the two `ContourGuiApp` accessors.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[chrometokens]" && out/appleclang-debug/src/contour/contour_gui_test "[menurouting],[appearance-source]"`

Expected: PASS. Until Task 6, Apple builds get the null presenter from the unguarded factory too.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups*.{hpp,cpp} src/contour/platform/NativeMenuPresenter.{hpp,cpp} src/contour/platform/AppearanceServices.hpp src/contour/platform/AppearanceSources_test.cpp src/contour/test/AppearanceFixtures.hpp src/contour/test/MenuFixtures.hpp src/contour/window/MenuRouting*.{hpp,cpp} src/contour/ContourGuiApp.{hpp,cpp}
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/config src/contour/platform src/contour/test src/contour/window src/contour/ContourGuiApp.* src/contour/CMakeLists.txt
git commit -m "contour: a token chooses native or in-scene menus, behind a presenter interface"
```

---

### Task 6: The Cocoa presenter

**Files:**
- Create: `src/contour/platform/CocoaNativeMenuPresenter.mm` (ARC). It defines the Apple
  `makeNativeMenuPresenter()`; `NativeMenuPresenter.cpp`'s factory moves inside `#ifndef __APPLE__`.
- Create: `src/contour/platform/CocoaNativeMenu.hpp` — an Apple-only inspection seam with no Objective-C
  types in its signatures, so a `.mm` test can read back what `present()` would show. List it in
  `add_library(contour_platform …)` (`src/contour/platform/CMakeLists.txt:15-69`), as `CocoaWindowFrame.hpp` is.
- Create: `src/contour/platform/CocoaNativeMenuPresenter_test.mm`; add it to `contour_gui_test`'s `if(APPLE)`
  block (`src/contour/CMakeLists.txt:820-831`) with `-fobjc-arc`.
- Modify: `src/contour/platform/CMakeLists.txt:81-95` — the `.mm` in `target_sources` and in the
  `-fobjc-arc` list.

**Interfaces:**
- Consumes: `command::MenuTree`, `MenuNode` (Task 4); `INativeMenuPresenter`, `MenuAnchor` (Task 5).
- Produces (`CocoaNativeMenu.hpp`, namespace `contour::platform`):
  - `enum class SwatchImage : uint8_t { Absent = 0, Present };`
  - `struct NativeMenuOutline { command::ContextMenuEntryKind kind = command::ContextMenuEntryKind::Command; std::string title {}; command::RowAvailability availability = command::RowAvailability::Enabled; command::RowCheck check = command::RowCheck::None; SwatchImage image = SwatchImage::Absent; long tag = -1; std::vector<NativeMenuOutline> children {}; [[nodiscard]] bool operator==(NativeMenuOutline const&) const = default; };`
  - `[[nodiscard]] std::vector<NativeMenuOutline> outlineNativeMenu(command::MenuTree const& tree);` — builds the
    `NSMenu` exactly as `present()` does and reads each `NSMenuItem` back. `@pre` main thread.
- **The realisation** (one function shared by `present()` and `outlineNativeMenu`):
  - `autoenablesItems = NO`. A separator is `NSMenuItem.separatorItem`. A submenu item gets `submenu` set to
    the recursively built `NSMenu`, titled the same, and `enabled = availability == Enabled` (Task 10's
    submenus can be greyed).
  - A command item has `title`, `enabled = availability == Enabled`, and `state = NSControlStateValueOn` for
    `Checked` (`NSControlStateValueOff` for `Unchecked` and `None`). Its `tag` is `actionIndex`, and its
    target/action is one Objective-C object of a class prefixed `ContourMenuPick`, which stores the picked tag.
  - A swatch row gets an `image`: an `NSImage imageWithSize:flipped:drawingHandler:` square of the system
    menu font's line height, filled with the swatch colour.
  - No key equivalents (R-i).
- **`present()`:** with a position and no window → `std::unexpected(NoWindow)`. Next, off the cocoa
  platform plugin (`QGuiApplication::platformName() != "cocoa"`) → `std::unexpected(Unsupported)`:
  `winId()` is an `NSView*` only there (`CocoaWindowFrame.mm:102-106`), and an offscreen app run that opens a
  context menu would otherwise message garbage. The factory stays unconditional. With a window and position:
  `inView` is `(__bridge NSView*)reinterpret_cast<void*>(window->winId())`, since ARC rejects a direct
  integer-to-object-pointer cast (the existing casts are in non-ARC files), and the location is
  `NSMakePoint(x, y)`, since
  Qt's view is flipped, so window-logical coordinates are view coordinates. Without a position: `inView:nil`
  at `NSEvent.mouseLocation`. After `popUpMenuPositioningItem:nil atLocation:inView:` returns, a stored tag
  `>= 0` calls `onPicked(static_cast<std::size_t>(tag))`. `@pre` main thread, documented on the class and
  asserted with `NSThread.isMainThread`.

- [ ] **Step 1: Write the failing tests** in `CocoaNativeMenuPresenter_test.mm`, tag `[nativemenu][cocoa]`
  (no display needed; it never pops):
  - "the outline mirrors the tree": a tree from `buildMenuTree(buildTabContextMenu(state), {}, MacGlyphs)`,
    with a three-colour palette, gives an outline of the same shape. Each node has the matching kind,
    title, availability and check, and `tag == actionIndex` for command nodes. Each swatch node has
    `image == Present`, and no other node has an image.
  - "a disabled row stays disabled": `autoenablesItems` is off, so a `Disabled` node outlines `Disabled`.
  - "the factory is native on Apple": `makeNativeMenuPresenter()` is not a `NullNativeMenuPresenter`
    (`dynamic_cast` yields null).
  - "a position needs a window": `present(tree, { .window = nullptr, .position = QPointF { 1, 1 } }, {})` is
    `std::unexpected(MenuPresentError::NoWindow)`, and no menu is shown, since the check runs first.
  - "off the cocoa plugin it is Unsupported": skipped when `platformName() == "cocoa"` (it would pop).
    Otherwise (the offscreen suite) `present(tree, { .window = nullptr, .position = std::nullopt }, {})` is
    `std::unexpected(MenuPresentError::Unsupported)`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: compile failure, because `CocoaNativeMenu.hpp` does not exist.

- [ ] **Step 3: Implement** the `.mm` and the seam.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[nativemenu]"`

Expected: PASS on this Mac. On other platforms the file is not compiled.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/CocoaNativeMenu*.{hpp,mm} src/contour/platform/NativeMenuPresenter.cpp
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/platform src/contour/CMakeLists.txt
git commit -m "platform: present a menu tree as a native NSMenu on macOS"
```

---

### Task 7: Every context menu goes through the router

**Files:**
- Modify: `src/contour/window/ContextMenuModel.hpp:16-60`, `.cpp:10-49`. The model is built from a `MenuTree`,
  and consecutive swatch rows become one `swatches` row.
- Modify: `src/contour/window/WindowController.hpp` — the context-menu block (`:327-357`), the signals
  (`:690-694`), the members (`:850-865`) and the new members' declarations; `.cpp:145-240`.
- Test: `src/contour/window/WindowMenus_test.cpp` (from Task 2), `src/contour/window/ContextMenuModel_test.cpp`
  (from Task 1), `src/contour/test/QmlComponents_test.cpp:513-520` (the end-to-end case builds through
  `buildMenuTree`).

**Interfaces:**
- Consumes: `buildTabContextMenu`, `buildNewTabMenu` (Task 3); `buildMenuTree`, `chordIndex` (Task 4);
  `presentMenu`, `MenuRoute`, `MenuAnchor` (Task 5); `runTabAction` (Task 2).
- Produces (`ContextMenuModel.hpp`):
  - `[[nodiscard]] QVariantList toContextMenuModel(std::span<command::MenuNode const> nodes);` keeps today's
    keys and adds `"swatches"`. A run of consecutive command nodes with a `swatch` becomes one row
    `{ kind: "swatches", swatches: [ { title, color (QColor), actionId, enabled, checked } ] }`.
    `actionId` is the node's `actionIndex`. A submenu row carries `enabled` from its availability too.
  - `PublishedContextMenu::publish(command::MenuTree tree)` stores `tree.actions` and the model.
    `actionAt` is unchanged.
- Produces (`WindowController`):
  - `enum class MenuSurface : uint8_t { Pane = 0, TitleBar, Tab, NewTab };` and
    `inline constexpr std::size_t MenuSurfaceCount = static_cast<std::size_t>(MenuSurface::NewTab) + 1;`.
    The members `_paneContextMenu` and `_titleBarContextMenu` become
    `std::array<PublishedContextMenu, MenuSurfaceCount> _menus {}`.
  - The members `config::MenuPresentation _menuPresentation` and
    `platform::INativeMenuPresenter& _nativeMenus` are initialised in the constructor's list from
    `manager.app().menuPresentation()` and `manager.app().nativeMenuPresenter()`, as `_profileCommands` is
    at `WindowController.cpp:56`. Declare them after `_commandPalette`, to keep the initialiser order, and
    give `_menuPresentation` the default member initializer `config::MenuPresentation::InScene`.
  - `std::optional<vtworkspace::TabId> _tabContextMenuTab {}` is the tab the tab menu was opened over.
  - `Q_PROPERTY(QVariantList tabContextMenuModel READ tabContextMenuModel NOTIFY tabContextMenuModelChanged)` and
    `Q_PROPERTY(QVariantList newTabMenuModel READ newTabMenuModel NOTIFY newTabMenuModelChanged)`.
  - `Q_INVOKABLE void openTabContextMenu(int index, QPointF windowPosition);`
  - `Q_INVOKABLE void triggerTabContextMenuAction(int actionId);`
  - `Q_INVOKABLE void openNewTabMenu(QPointF windowPosition);`
  - `Q_INVOKABLE void triggerNewTabMenuAction(int actionId);`
  - Signals: `void tabContextMenuModelChanged();`, `void tabContextMenuRequested(int index);`,
    `void newTabMenuModelChanged();`, `void newTabMenuRequested();`.
  - The private `void presentSurface(MenuSurface surface, command::MenuTree tree, platform::MenuAnchor anchor);`
    publishes into `_menus[surface]` and emits that surface's model-changed signal. With
    `_menuPresentation == InScene` it then emits the surface's request signal at once:
    `contextMenuRequested()`, `titleBarContextMenuRequested()`, `tabContextMenuRequested(row)` or
    `newTabMenuRequested()`. With `Native` it posts the presentation,
    `platform::postToObject(this, …)`, so `present()` runs on a clean stack after the press handler that
    asked for it has returned: `openContextMenu` runs inside the right-press handler
    (`TerminalSession.cpp:3925-3934`), and `openTabContextMenu` inside a QML `TapHandler`. AppKit's tracking
    loop swallows the mouse-up (Qt's own `QCocoaMenu::showPopup` resets the view's buttons afterwards, which
    R-a bypasses), and it runs Qt events that could destroy the TabItem under the handler's frame. Posted,
    the release reaches Qt first, and AppKit keeps a menu opened after mouse-up in click-click mode. The
    posted call runs `presentMenu(_menuPresentation, _nativeMenus, …)`. Its `onPicked` is
    `platform::postToObject(this, [this, surface, index] { triggerSurfaceAction(surface, int(index)); })`,
    and an `InScene` result (the presenter declined) emits the surface's request signal as above.
  - The private `void triggerSurfaceAction(MenuSurface surface, int actionId);` copies the action out of
    `_menus[surface]`. `Pane` runs it on `_contextMenuSession` (`:173-190`); `TitleBar` and `NewTab` on
    `activeSession()`; `Tab` calls `runTabAction(*_tabContextMenuTab, action)`, the `TabId` overload of
    Task 2, which does nothing when the pinned tab has gone. The four `Q_INVOKABLE trigger*` functions
    delegate to it.
  - The four `open*` functions build their state as today. `openContextMenu` and `openTitleBarContextMenu`
    keep their bodies and pass `{ .window = _osWindow, .position = std::nullopt }`, which opens at the pointer.
    `openTabContextMenu` fills a `TabContextMenuState` from `tabAtRow(index)`, `count()` and
    `_manager.model().colorPalette()`. `openNewTabMenu` reads the profile names sorted, as
    `openTitleBarContextMenu` does at `:217-219`. Every tree is
    `buildMenuTree(entries, command::chordIndex(_manager.app().config()), command::platformShortcutStyle())`.

- [ ] **Step 1: Write the failing tests.**
  - `ContextMenuModel_test.cpp`, `[contextmenumodel]`, "swatch rows group into one row": a tree whose submenu
    holds three swatch nodes, a separator and two plain rows gives the submenu children
    `["swatches", "separator", "command", "command"]`. The `swatches` row holds three entries with the
    nodes' `actionIndex`, `QColor` and `checked`.
  - `WindowMenus_test.cpp`, `[contour][windowmenus]` (a `TestApp`, hence `InScene`, Review Focus 1):
    - "the tab menu is built for the right-clicked tab": three model tabs, `activateTab(0)`, then
      `openTabContextMenu(2, {})`. `tabContextMenuRequested` fires once with 2, and
      `tabContextMenuModel()` is non-empty. Triggering the `actionId` of "Close Other Tabs", found by title
      in the model, leaves `count() == 1` and keeps the tab that was row 2, not the active tab 0
      (Review Focus 4).
    - "a pick against a tab that has gone is a no-op": three tabs, `openTabContextMenu(2, {})`, then
      `closeTabsToRight(0)`. Triggering "Close Tab" leaves `count() == 1` (Review Focus 2).
    - "Rename… names the right-clicked row": `openTabContextMenu(1, {})`, then triggering "Rename…" emits
      `tabTitleEditRequested(1)`.
    - "the new-tab menu lists the profiles": `openNewTabMenu({})` emits `newTabMenuRequested`, and the model
      ends with a `Settings` row.
    - "the pane and title-bar menus still pop in the scene": `openTitleBarContextMenu()` emits
      `titleBarContextMenuRequested` once.
  - `QmlComponents_test.cpp:513-520`: build with `buildMenuTree(buildContextMenu(state), {}, ShortcutStyle::Text)`
    and `toContextMenuModel(tree.nodes)`. Check `commandId(tree.actions[0]) == "CopySelection"`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: compile failure on `openTabContextMenu` and on `toContextMenuModel(std::span<MenuNode const>)`.

- [ ] **Step 3: Implement** the model, the surfaces and the routing.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[windowmenus],[contextmenumodel],[contour][gui][qml]"`

Expected: PASS. The QML still uses `TabContextMenu.qml` and `NewTabMenu.qml` until Task 8, and they ignore
the new properties.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/ContextMenuModel*.{hpp,cpp} src/contour/window/WindowController.{hpp,cpp} src/contour/window/WindowMenus_test.cpp src/contour/test/QmlComponents_test.cpp
git add src/contour/window src/contour/test/QmlComponents_test.cpp
git commit -m "contour: route every context menu through the native-or-in-scene presenter"
```

---

### Task 8: The in-scene tab and new-tab menus render the tables, swatch row included

**Files:**
- Create: `src/contour/qml/MenuSwatchRow.qml`; list it in `src/contour/CMakeLists.txt:370-416`.
- Delete: `src/contour/qml/TabContextMenu.qml`, `src/contour/qml/NewTabMenu.qml`; remove them from
  `src/contour/CMakeLists.txt:384`, `:386`.
- Modify: `src/contour/qml/ActionContextMenu.qml:37-98` — a `swatches` row kind, submenus honour `enabled`,
  and the pick is deferred (R-m).
- Modify: `src/contour/qml/TabItem.qml:558-574` — the right-click calls the controller, and the per-tab
  `TabContextMenu` goes. Rename and Custom… stay per TabItem, through the existing `tabTitleEditRequested`/
  `tabColorPickRequested` handlers of the `Connections` block (`:595-605`).
- Modify: `src/contour/qml/TabStrip.qml:89-110`, `:292-316` — one `ActionContextMenu` named `tabContextMenu`
  for the whole strip, bound to `tabContextMenuModel` and popped below the requested tab's delegate; the "▾"
  button calls `openNewTabMenu`, and an `ActionContextMenu` named `newTabMenu` is bound to `newTabMenuModel`.
  One menu, not one per TabItem: §3.2's "one in-scene `ActionContextMenu.qml`", one object `findChild` can
  name, and one rebuild per publish rather than N.
- Modify: `src/contour/config/ChromeTokenGroups.hpp:106-116` (`SizeTokens`), `:43-50` and `:81`
  (`ChromeColorRole` and its count), `:271-…` (`NativeColors`), `:389-393` (the Terminal row's `.colors`) and
  the four rows; `src/contour/window/ChromeTokenObjects.hpp:36-70` (`ChromeSizeTokens`), `:400-…`
  (`ChromeColorTokens`).
- Modify: `src/contour/test/MainWindowControllerMock.hpp` (the shared surface: the two models, the four
  invokables, and five signals: the two models' NOTIFY signals, `tabContextMenuRequested(int)`,
  `newTabMenuRequested()` and `aboutRequested()`). They go in this shared base so that
  `RoutingMockController` (`session/KeyboardRouting_test.cpp:200`), which inherits it, has them too: a
  `Connections` handler without its signal is a QML warning, which fails the run-wide gate.
  `src/contour/test/MockMainController.hpp` (models from the real builders);
  `src/contour/test/QmlComponents_test.cpp:87-292` (`MockTabController`, which does not inherit the base:
  the same surface).
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp:35-47`, `:64-70`, `:107-206` (the script `TabMenu`, the
  action `OpenTabMenu`, the `tab-menu` row, and the count `static_assert` at `:205`, which pins
  `NewDefaultsBanner` as the last enumerator), `ChromeSnapshotScene.hpp:158-175` (`show()`).
- Test: `QmlComponents_test.cpp` (`:308-348` the load list, `:350-375` rewritten, `:861-888` folded into the
  rewritten case and deleted, new cases), `Accessibility_qml_test.cpp:164-171` (one row),
  `ChromeTokenGroups_test.cpp`, `UiStyleProvider_test.cpp` (the two size properties),
  `ChromeSnapshotPlan_test.cpp:17-29`, `:58-75`, `ChromeSnapshot_test.cpp:60-123`.

**Interfaces:**
- Consumes: the model rows of Task 7; `tabContextMenuRequested(int)`, `newTabMenuRequested()`.
- Produces (tokens): `SizeTokens::swatchWidth = 0`, `swatchHeight = 0`. MacOS, Fluent and ContourLinux use
  16 × 16 px; Terminal uses 2 × 1 cells, a two-cell block per swatch. They are published as
  `chromeStyle.size.swatchWidth` (× `units.horizontal`) and `swatchHeight` (× `units.vertical`).
- Produces (tokens): `ChromeColorRole::SwatchRing`, appended after `ToastText` (`ChromeColorRoleCount` is then
  derived from `SwatchRing`). `NativeColors` gives it `PaletteWindowText`; the Terminal row gives it
  `schemeColor(SchemeSlot::Foreground)`, the scheme's colour rather than Qt's palette, whose mismatch was
  the root of 1a's 1:1 banner defect (159b47dc). Published as `chromeStyle.colors.swatchRing`.
- Produces — `MenuSwatchRow` (a non-triggering `Control`, not a `MenuItem`: a `MenuItem`'s own `triggered`,
  which keyboard Enter fires too, would close the menu with no pick, and the order of the item's click and
  the swatch's tap is unspecified):
  - `property var swatches: []` (the model's array) and `signal swatchPicked(int actionId)`;
  - `contentItem: Row { spacing: chromeStyle.space.s2 }` holds one `Rectangle` per swatch:
    `width: chromeStyle.size.swatchWidth`, `height: chromeStyle.size.swatchHeight`,
    `radius: chromeStyle.radii.control`, `color: modelData.color`. A checked swatch has
    `border.width: chromeStyle.borderWidth` in `chromeStyle.colors.swatchRing`. A `TapHandler` emits
    `swatchPicked(modelData.actionId)`. Each swatch carries `objectName: "menuSwatch"`,
    `Accessible.role: Accessible.Button` and `Accessible.name: modelData.title`.
  - The row's own `Accessible.role: Accessible.MenuItem` and `Accessible.name: qsTr("Tab colours")`.
  - The row takes no keyboard focus: "Custom…" is the keyboard path to a tab colour.
  - No literals in any size property; colours come from the model or the tokens.
- Produces — `ActionContextMenu` (it keeps `signal picked(int actionId)`):
  - `fillMenu` creates a `MenuSwatchRow` for a `swatches` row, under `menu.contentItem`, and adds it with
    `menu.addItem`, as `MenuSeparator` is. It connects its `swatchPicked` to the deferred pick. A submenu is
    created with `enabled: row.enabled`.
  - The deferred pick: `property int _pendingActionId: -1`. A command row's `onTriggered` or a swatch pick
    sets `_pendingActionId` and calls `root.dismiss()` when `root.opened`, or emits `picked` at once when the
    menu is not open. `onClosed` emits `picked(_pendingActionId)` only when `_pendingActionId >= 0`, then
    resets it to -1. A dismissed menu emits nothing.
- `TabItem.qml`: the right-button `TapHandler` calls
  `root.controller.openTabContextMenu(root.tabIndex, root.mapToItem(null, eventPoint.position.x, eventPoint.position.y))`.
  The TabItem holds no menu. "Rename…" and "Custom…" reach it through the existing
  `onTabTitleEditRequested`/`onTabColorPickRequested`, now after the menu has closed (R-m).
- `TabStrip.qml`: `ActionContextMenu { id: tabContextMenu; objectName: "tabContextMenu"; entries: root.controller ? root.controller.tabContextMenuModel : []; onPicked: (id) => root.controller.triggerTabContextMenuAction(id) }`.
  A `Connections` on the controller handles `onTabContextMenuRequested(index)`: it reads that tab's
  delegate through the strip's list, `const item = list.itemAtIndex(index)`, and calls
  `tabContextMenu.popup(item, 0, item.height)`, as the new-tab menu does at `:305`. A popup without
  arguments would open at the live pointer, which places the snapshot wherever the mouse happens to be.
- `TabStrip.qml`: `onClicked: if (root.controller) root.controller.openNewTabMenu(newTabMenuButton.mapToItem(null, 0, newTabMenuButton.height))`.
  `onNewTabMenuRequested` pops `newTabMenu` at `(newTabMenuButton, 0, newTabMenuButton.height)`, as `:305`
  does today.
- The mocks' `openTabContextMenu`/`openNewTabMenu` publish models built by `buildTabContextMenu`/
  `buildNewTabMenu` → `buildMenuTree` → `toContextMenuModel`, and emit the request. The `trigger*` mocks
  record the id.
- `SnapshotScript::TabMenu` has the row `{ .script = SnapshotScript::TabMenu, .name = "tab-menu", .tabCount = 3, .panes = PaneShape::SingleLeaf, .page = ContentPage::Terminal, .action = ScriptAction::OpenTabMenu, .connection = HostConnection::Local, .hostLabel = "", .banner = SnapshotBanner::None }`.
  Three tabs, so Native2026's `Multiple` shows the strip. The scene calls `_controller.openTabContextMenu(1, {})`,
  waits for `tabContextMenu`'s `opened`, then opens its "Tab Color" sub-menu (`menuAt(i)` whose `title` is
  "Tab Color") and waits for that `opened` too.
- **TUI row:** ContourTui's `Menu.qml`/`MenuItem.qml` draw the in-scene menu. Swatches are two cells wide and
  one row high with radius 0, and the checked one is ringed in the foreground.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp` (`everyRowMatchesTheDesignKit`) pins `swatchWidth`/`swatchHeight` per row,
    and `SwatchRing` per row (`PaletteWindowText` on the native rows, `Scheme`/`Foreground` on Terminal).
    `UiStyleProvider_test.cpp`: under Terminal, `size.swatchWidth == 2 * cellWidth` and
    `swatchHeight == cellHeight` (the test computes its own expectation; the gate does not scan C++).
  - `QmlComponents_test.cpp`, `[contour][gui][qml][menus]`:
    - "the tab menu opens with the table's rows (offscreen)", replacing `:350-375` and absorbing `:861-888`
      ("A context menu actually opens, with its surface"), which is deleted: that case finds
      `tabContextMenu` on a TabItem host and opens it, and after this task the TabItem has no menu, while
      the strip's would be empty until the controller publishes. A TabStrip host with `MockTabController`
      and two tabs, under a `QmlMessageCapture`. `controller.openTabContextMenu(1, {})` (the mock publishes
      and requests), then the strip's one `tabContextMenu` is `opened`, its `count` equals the top-level size
      of the model the mock published, and `checkMenuOpensAtUsableSize` passes. Its `y` is below tab 1's
      delegate. After `close()`, no `TypeError` was logged.
    - "a swatch pick reaches the controller once the menu has closed (offscreen)": the "Tab Color" sub-menu
      holds one `MenuSwatchRow` with `palette.size()` children named `menuSwatch`. Tapping swatch 1
      (`QTest::mouseClick` at its centre in the window) records nothing while the menu is open. After
      `closed`, the controller has recorded swatch 1's `actionId`, once. Opening and dismissing the menu
      with no pick records nothing (no `picked(-1)`).
    - "a row triggered on a closed menu is picked at once (offscreen)": the existing end-to-end case
      (`:491-580`) still sees `pickedSpy.count() == 1` right after `triggered`.
    - "the Terminal row draws swatches as two-cell blocks (offscreen)": under `ChromeStyle::Terminal`, a
      swatch's `width == 2 * cellWidth`, `height == cellHeight`, and `radius == 0`. The checked swatch's
      `border.color` is the scheme's foreground, and its `vtbackend::contrastRatio` against
      `modelData.color` is above 1.0, so the ring never reads at 1:1 as 1a's banner did.
    - "the new-tab menu lists the profiles (offscreen)": a TabStrip host. Clicking `newTabMenuButton`
      records `openNewTabMenu`, and `newTabMenu`'s `count` equals the model's size.
  - `QmlComponents_test.cpp:308-348`: the load list drops the two deleted files and adds `MenuSwatchRow.qml`.
  - `Accessibility_qml_test.cpp`: a `MenuSwatchRow.qml` case with one swatch
    `{ title: "#EE6666", color: "#EE6666", actionId: 0, enabled: true, checked: false }`. The row and the
    swatch both declare a role and a name.
  - `ChromeSnapshotPlan_test.cpp`: drop the literal counts at `:21`, `:68`, `:74`, keeping the derived checks
    beside them. The last-enumerator pin at `:20` (`size() == NewDefaultsBanner + 1`) would fail once
    `TabMenu` is appended, so it becomes
    `CHECK(SnapshotScripts.size() == static_cast<std::size_t>(SnapshotScripts.back().script) + 1)`, and the
    `static_assert` at `ChromeSnapshotPlan.hpp:205` the same. The enumerator-order `static_assert` at
    `:200-204` already ties each row to its index, so later scripts need no edit here. Add
    `CHECK(scriptRow(SnapshotScript::TabMenu).tabCount == 3)`.
  - `ChromeSnapshot_test.cpp`: `case SnapshotScript::TabMenu:` finds the one `tabContextMenu` opened, and at
    least one `menuSwatch` visible.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test contour_test`

Expected: compile failure on `SnapshotScript::TabMenu` and `swatchWidth`.

- [ ] **Step 3: Implement** the tokens, `MenuSwatchRow.qml`, the `ActionContextMenu` changes, the TabItem and
  TabStrip wiring, the mocks, the snapshot row and the scene action. Delete the two QML files.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[menus],[a11y],[snapshot],[contour][gui][qml],[uistyle]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[menus],[snapshot]" && out/appleclang-debug/src/contour/contour_test "[snapshot],[chrometokens]" && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$"`

Expected: PASS. The ContourTui run is the TUI row.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/ChromeTokenGroups*.{hpp,cpp} src/contour/window/ChromeTokenObjects.{hpp,cpp} src/contour/window/UiStyleProvider_test.cpp src/contour/test/*.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp src/contour/test/ChromeSnapshot*_test.cpp
editorconfig-checker src/contour/qml/MenuSwatchRow.qml src/contour/qml/ActionContextMenu.qml src/contour/qml/TabItem.qml src/contour/qml/TabStrip.qml src/contour/CMakeLists.txt
git add -A src/contour/qml src/contour/config src/contour/window src/contour/test src/contour/CMakeLists.txt
git commit -m "contour: the tab and new-tab menus render their tables, with an inline swatch row"
```

---

### Task 9: The About dialog

**Files:**
- Modify: `src/contour/qml/KitDialog.qml:94-102` — the reject button hides when `rejectText` is empty.
- Modify: `src/contour/qml/Main.qml:304-331` — a `KitDialog` named `aboutDialog` and `onAboutRequested`.
- Modify: `src/contour/ContourGuiApp.hpp/.cpp` — `[[nodiscard]] QString applicationVersion() const`, defined
  in `ContourGuiApp.cpp` as `QStringLiteral(CONTOUR_VERSION_STRING)`. The macro is defined only for
  `Config.cpp`, `ContourApp.cpp`, `ContourGuiApp.cpp` and two tests (`src/contour/CMakeLists.txt:179-199`,
  deliberately, to keep the compile cache valid across worktrees), so it cannot appear in a header.
- Modify: `src/contour/window/WindowController.hpp/.cpp` —
  `Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)`; `applicationVersion()` is
  defined in `WindowController.cpp` and returns `_manager.app().applicationVersion()`. The value is still a
  build constant, reached through the composition root (§1.4), not an ambient read.
- Modify: `src/contour/test/MainWindowControllerMock.hpp` (the `applicationVersion` property, value
  `"0.0.0-test"`, and `Q_INVOKABLE void showAbout()` emitting `aboutRequested`); `ChromeSnapshotPlan.hpp`
  (the script `About`, the action `OpenAbout`, the `about` row; the count pins Task 8 derived from
  `SnapshotScripts.back()` need no edit); `ChromeSnapshotScene.hpp` (`show()`).
- Test: `QmlComponents_test.cpp`, `ChromeSnapshot_test.cpp`, `ChromeSnapshotPlan_test.cpp`.

**Interfaces:**
- Consumes: `ShowAbout` → `aboutRequested()` (Task 2); `KitDialog` (1a).
- Produces: `KitDialog`'s `kitDialogReject` gets `visible: root.rejectText.length > 0`. Main.qml gets
  `KitDialog { id: aboutDialog; objectName: "aboutDialog"; heading: qsTr("Contour"); message: qsTr("Version %1").arg(appWindow.win ? appWindow.win.applicationVersion : ""); acceptText: qsTr("OK"); rejectText: "" }`,
  opened by `function onAboutRequested() { aboutDialog.open(); }`.
- The row `{ .script = SnapshotScript::About, .name = "about", .tabCount = 1, .panes = PaneShape::SingleLeaf, .page = ContentPage::Terminal, .action = ScriptAction::OpenAbout, .connection = HostConnection::Local, .hostLabel = "", .banner = SnapshotBanner::None }`.
  The scene calls `_controller.showAbout()` and waits for `aboutDialog`'s `opened`.
- **TUI row:** the boxed `PopupSurface` popup with the hard one-cell shadow, centred, with one button.

- [ ] **Step 1: Write the failing tests.**
  - `QmlComponents_test.cpp`, `[contour][gui][qml][kit][dialog]`, "a KitDialog without a reject text shows one
    button (offscreen)": with `rejectText: ""`, `kitDialogReject` is not visible, and `kitDialogAccept` is.
    With the default, both are visible.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][about]`, "ShowAbout opens the About dialog with the version
    (offscreen)": load Main.qml against `MockMainController`, call `showAbout()`, then `aboutDialog` is
    `opened` and `kitDialogMessage`'s text contains `"0.0.0-test"`. Under `ChromeStyle::Terminal`, its `y`
    centres in the overlay, as the 1a placement test does.
  - `ChromeSnapshot_test.cpp`: `case SnapshotScript::About:` finds `aboutDialog` opened.
    `ChromeSnapshotPlan_test.cpp` adds `CHECK(scriptRow(SnapshotScript::About).action == ScriptAction::OpenAbout)`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test contour_test`

Expected: compile failure on `SnapshotScript::About`.

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[dialog],[about],[snapshot],[a11y]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[about],[snapshot]" && out/appleclang-debug/src/contour/contour_test "[snapshot]" && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$"`

Expected: PASS.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/WindowController.{hpp,cpp} src/contour/ContourGuiApp.{hpp,cpp} src/contour/test/*.hpp src/contour/test/QmlComponents_test.cpp src/contour/test/ChromeSnapshot*_test.cpp
editorconfig-checker src/contour/qml/KitDialog.qml src/contour/qml/Main.qml
git add src/contour/qml src/contour/window/WindowController.* src/contour/ContourGuiApp.* src/contour/test
git commit -m "contour: an About dialog, opened by the ShowAbout action"
```

---

### Task 10: `MenuBarTable`

**Files:**
- Create: `src/contour/command/MenuBarTable.hpp`, `src/contour/command/MenuBarTable.cpp`; list them in
  `src/contour/command/CMakeLists.txt`.
- Modify: `src/contour/command/ContextMenuTable.hpp:142-167` — `MenuTable::changeProfileRows` and
  `changeProfileSubmenu()`, moved from `ContextMenu.cpp:139-153`, sit beside `hostRows`, so the pane menu and
  the Shell menu share them. `MenuTable` gains the two things this table needs and lacks today:
  - `Table::rows(ChildBuilder)`, a template-only row kind whose built rows are spliced in place (the Window
    menu's tab list). `MenuTable` otherwise expands builders only for `Submenu` rows.
  - Submenus honour their `enabled` predicate through `RowAvailability`. `buildRows` writes
    `.enabled = true` on every submenu today (`ContextMenuTable.hpp:198-210`), which would silently drop the
    "(window)" and `paneCount > 1` rules below. The presenters already read the column for submenus
    (`NSMenuItem.enabled`, Task 6; QML `Menu.enabled`, Task 8; `menuAction()->setEnabled`, Task 12).
- Test: new `src/contour/command/MenuBarTable_test.cpp` (add to `contour_test`).

**Interfaces:**
- Produces (namespace `contour::command`, `MenuBarTable.hpp`):
  - `enum class MenuBarMenu : uint8_t { Application = 0, Shell, Edit, View, Window, Help };` and
    `inline constexpr std::size_t MenuBarMenuCount = static_cast<std::size_t>(MenuBarMenu::Help) + 1;`
  - `enum class MenuBarPlacement : uint8_t { None = 0, Global };` and
    `[[nodiscard]] constexpr MenuBarPlacement menuBarPlacementFor(config::HostPlatform host) noexcept;`
    (MacOS → `Global`, an exhaustive `switch`).
  - `enum class WindowPresence : uint8_t { Absent = 0, Present };`,
    `enum class SelectionPresence : uint8_t { Absent = 0, Present };`,
    `enum class SpeechAvailability : uint8_t { Unavailable = 0, Available };`,
    `enum class InputProtection : uint8_t { Off = 0, On };`
  - `struct MenuBarState { WindowPresence window = WindowPresence::Absent; SelectionPresence selection = SelectionPresence::Absent; SpeechAvailability speech = SpeechAvailability::Unavailable; InputProtection inputProtection = InputProtection::Off; int tabCount = 0; int activeTab = -1; int paneCount = 0; std::vector<std::string> tabTitles {}; config::TabBarVisibility tabBarVisibility = config::TabBarVisibility::Always; config::TabBarPosition tabBarPosition = config::TabBarPosition::Top; std::string activeProfile {}; std::vector<std::string> profileNames {}; std::vector<std::string> hostNames {}; std::vector<std::string> layoutNames {}; };`
  - `[[nodiscard]] MenuBarState menuBarStateOf(config::Config const& config);` fills the three name lists,
    sorted, and leaves the window fields at their defaults. Layout names come from the configured layouts,
    as `LayoutCommandSource::commands()` (`CommandCatalog.cpp`) reads them.
  - `[[nodiscard]] std::vector<ContextMenuEntry> buildMenuBar(MenuBarState const& state);` returns exactly
    `MenuBarMenuCount` `Submenu` entries, in `MenuBarMenu` order, titled "Contour", "Shell", "Edit", "View",
    "Window" and "Help".
  - `struct MenuReachabilityExemption { std::string_view action; std::string_view reason; };` and
    `inline constexpr std::array MenuReachabilityExemptions`, holding the five of R-g.

**The table.** A row is `title | action | rule`. The rule is "window" for `enabledWhen(window == Present)`.
An empty title means the action's catalogue title (`commandTitle`).
- **Contour:** "About Contour" `ShowAbout {}` role About · separator · "Settings…" `OpenConfiguration {}` role
  Preferences · "Open Configuration File" `OpenConfiguration { .inEditor = true }` · "Reload Configuration"
  `ReloadConfig {}` · "Reset Configuration" `ResetConfig {}` · separator · "Quit Contour" `Quit {}` role Quit.
- **Shell:** "New Window" `NewTerminal {}` · "New Tab" `CreateNewTab {}` · "New Tab with Profile" › one
  `CreateNewTab { .profileName }` per profile · `attachToHostSubmenu()` · "Launch Layout" › one
  `LaunchLayout { .name }` per layout · "Save Layout…" `SaveLayout {}` · separator · "Split Vertically"
  `SplitVertical` (window) · "Split Horizontally" `SplitHorizontal` (window) · separator ·
  `changeProfileSubmenu()` (window) · "Read-Only" `ToggleInputProtection`, checked when
  `inputProtection == On` (window) · separator · "Commands" › "Copy Last Command" `CopyCommandLine {}`,
  "Copy Last Prompt" `CopyLastCommandPrompt`, "Copy Last Output" `CopyLastCommandOutput`, "Copy Last Prompt
  and Output" `CopyLastCommandBlock`, "Copy Previous Mark Range" `CopyPreviousMarkRange`, "Select Last
  Output" `SelectCommandBlock {}`, "Open Last Output in Pager" `OpenCommandOutput {}`, "Rerun Last Command"
  `RerunCommand {}`, separator, "Clear to Prompt" `ClearToPrompt` · separator · "Save Screenshot"
  `SaveScreenshot` · "Copy Screenshot" `CopyScreenshot` · "Save VT Screenshot" `ScreenshotVT` · "Open Current
  Folder" `OpenFileManager` · separator · "Soft Reset" `SoftReset` · "Hard Reset" `ClearHistoryAndReset` ·
  separator · "Close Pane" `ClosePane` · "Close Tab" `CloseTab` · "Close Window" `CloseAllTabs` (all window).
- **Edit:** "Copy" `CopySelection {}` (selection) · "Paste" `PasteClipboard {}` · "Paste Selection"
  `PasteSelection {}` · "Select All" `SelectAll` · "Clear Selection" `CancelSelection` (selection) · separator
  · "Find…" `SearchReverse` · "Find Next" `FocusNextSearchMatch` · "Find Previous" `FocusPreviousSearchMatch`
  · "Clear Find Highlight" `NoSearchHighlight` · separator · "Open Selection" `OpenSelection` (selection) ·
  "Open Link…" `HintMode { .patterns = "url", .hintAction = vtbackend::HintAction::Open }` (the default
  binding's exact action, `Config.hpp:1664-1667`, so its chord shows) · "Vi Mode" `ViNormalMode` · separator
  · "Speech" › "Start Speaking" `SpeakSelection` (selection), "Stop Speaking" `StopSpeaking`, shown when
  `speech == Available`.
- **View:** "Command Palette…" `OpenCommandPalette` · separator · "Bigger" `IncreaseFontSize` · "Smaller"
  `DecreaseFontSize` · "Actual Size" `ResetFontSize` · "More Opaque" `IncreaseOpacity` · "Less Opaque"
  `DecreaseOpacity` · separator · "Tab Bar" › `buildTabBarMenu({ state.tabBarVisibility, state.tabBarPosition })`
  (window; R-l) · "" `ToggleStatusLine` · "" `ToggleTitleBar` · "" `ToggleFullscreen` · separator · "Scroll" ›
  "" for each of `ScrollUp`, `ScrollDown`, `ScrollOneUp`, `ScrollOneDown`, `ScrollPageUp`, `ScrollPageDown`,
  `ScrollToTop`, `ScrollToBottom`, then separator, "Previous Prompt" `ScrollMarkUp`, "Next Prompt"
  `ScrollMarkDown` · "Folds" › "" for each of `ToggleFold`, `ToggleLastFold`, `CollapseLastFold`,
  `CollapseAllFolds`, `ExpandAllFolds` · "Keyboard" › "" for each of `ToggleAllKeyMaps`,
  `ToggleInputMethodHandling`.
- **Window:** "Previous Tab" `SwitchToTabLeft` · "Next Tab" `SwitchToTabRight` · "Last Used Tab"
  `SwitchToPreviousTab` · separator · "Move Tab Left" `MoveTabToLeft` · "Move Tab Right" `MoveTabToRight` ·
  "Move Tab to New Window" `MoveTabToNewWindow` (`tabCount > 1`) · "Rename Tab…" `SetTabTitle` · "Tab Color…"
  `SetTabColor {}` · "Reset Tab Color" `ResetTabColor` · "Close Other Tabs" `CloseOtherTabs` (`tabCount > 1`)
  · "Close Tabs to the Right" `CloseTabsToRight` (`activeTab < tabCount - 1`) · separator · "Pane" › "Focus
  Left/Right/Up/Down" `FocusPaneLeft/Right/Up/Down`, separator, "Swap Left/Right/Up/Down" `SwapPane*`,
  separator, "Move Left/Right/Up/Down" `MovePane*`, separator, "Grow Left/Right/Up/Down"
  `ResizePane { .direction = Direction::Left/Right/Up/Down }`, separator, "" `ToggleSplitOrientation`, "Zoom
  Pane" `TogglePaneZoom` (the whole submenu `paneCount > 1`) · separator · one row per tab, titled with its
  title, running `SwitchToTab { .position = i + 1 }`, checked when `i == activeTab` (a `Table::rows`
  splice, not a submenu). Every Window row before the tab list is "window".
- **Help:** "Contour Documentation" `FollowHyperlink { .uri = "https://contour-terminal.org/" }` · "Report an
  Issue" `FollowHyperlink { .uri = "https://github.com/contour-terminal/contour/issues" }` · separator ·
  "Debug" › "" `CreateDebugDump`, separator, "" for each of `TraceEnter`, `TraceLeave`, `TraceStep`,
  `TraceBreakAtEmptyQueue`.

- [ ] **Step 1: Write the failing tests** in `MenuBarTable_test.cpp`, tag `[menubar]`. "A full state" means
  window `Present`, selection `Present`, speech `Available`, three tabs, two panes, one profile, one host and
  one layout.
  - `menuBar.sixMenusInOrder`: `buildMenuBar(fullState)` has `MenuBarMenuCount` entries, all `Submenu`,
    with the titles above in enum order. Also `buildMenuBar(MenuBarState {})` has `MenuBarMenuCount` entries.
  - `menuBar.everyCatalogueActionIsReachable` (R-g): the set of `actions::name(action)` over every command
    row of `buildMenuBar(fullState)` is joined with those of `buildContextMenu` (a state with a selection,
    clipboard text, a last command with its command line, a hyperlink, a link, a fold line, a pointed block
    with output and command line, a live prompt, splits, `canSpeak`, one profile, one host),
    `buildTitleBarContextMenu` (two tabs, one profile, one host), `buildTabContextMenu` (two tabs, a
    palette) and `buildNewTabMenu` (one profile). Every name in `actions::actionCatalog()` is in the
    union or in `MenuReachabilityExemptions`. The union's size plus the exemptions' size equals
    `actionCatalog().size()`, both derived.
  - `menuBar.exemptActionsAppearNowhere`: no exempt name is in that union.
  - `menuBar.eachRoleOnce`: across the whole tree, `About`, `Preferences` and `Quit` each appear on exactly
    one row, all in the Contour menu.
  - `menuBar.noChordTwice` (R-h): for `config::Config {}` and
    `config::Config { .defaultsEdition = config::DefaultsEdition::Classic }`,
    `buildMenuTree(buildMenuBar(fullState), chordIndex(config), ShortcutStyle::MacGlyphs)` has no two nodes
    with an equal `chord`. "Copy" shows `"⌃C"` and "Clear Selection" shows `"⎋"`, its first binding
    (`Config.hpp:1291-1299`). With `chordIndex(loadConfigFromYaml(…))` binding `Ctrl+Shift+N` to both
    `CreateNewTab` and `NewTerminal`, through `contour::test::loadConfigFromYaml` (`CoreFixtures.hpp`, as
    `Shortcut_test.cpp:176` uses it), "New Window" in Shell keeps the chord and "New Tab" shows none.
  - `menuBar.tabBarMirrorsTheTitleBar` (R-l): View › "Tab Bar" equals the title bar menu's "Tab Bar" for the
    same modes, compared by kind, title, `commandId` and `check`.
  - `menuBar.windowRowsNeedAWindow`: with `MenuBarState {}`, "Split Vertically", "Close Tab" and "Rename Tab…"
    are `Disabled`, while "New Window" and "Quit Contour" are `Enabled`. The View › "Tab Bar" and Window ›
    "Pane" submenus are `Disabled` too, and with a full state but `paneCount = 1`, "Pane" alone of the two
    is `Disabled`.
  - `menuBar.tabList`: three titles give three `SwitchToTab` rows at the end of Window, with positions 1–3.
    Only the active one is `Checked`.
  - `menuBar.placement`: `menuBarPlacementFor(MacOS) == Global`; `Windows`, `KdePlasma` and `Other` → `None`
    (Review Focus 5).
  - `menuBar.stateOfConfig`: `menuBarStateOf(config)` lists the configured profiles sorted, and has
    `window == Absent`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test`

Expected: compile failure, because `MenuBarTable.hpp` does not exist.

- [ ] **Step 3: Implement** `Table::rows` and the submenu availability, then the table over
  `detail::MenuTable<MenuBarState>`, one template function per menu. Use the `withRole` and
  `changeProfileSubmenu` helpers, and move the pane menu onto the shared `changeProfileRows`.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_test "[menubar],[contextmenu]"`

Expected: PASS. If `noChordTwice` reports a collision the R-h rule does not explain, record it in the ledger
and stop for a ruling. Do not delete a row to make the test pass.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/command/MenuBarTable*.{hpp,cpp} src/contour/command/ContextMenuTable.hpp src/contour/command/ContextMenu.cpp
editorconfig-checker src/contour/command/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/command src/contour/CMakeLists.txt
git commit -m "command: the macOS menu bar's six menus as one table over the action catalogue"
```

---

### Task 11: The terminal keeps its keys under a global menu bar

**Files:**
- Create: `src/contour/display/ShortcutOverride.hpp` (header-only); list it in `src/contour/CMakeLists.txt`'s
  `_header_files`.
- Modify: `src/contour/display/TerminalDisplay.cpp:1534-1555` (`TerminalDisplay::event`). The display reads
  the placement as `_session->app().menuBarPlacement()`; the session already reaches `app()`
  (`TerminalDisplay.cpp:1823`). `TerminalDisplay` is constructed by QML, so it cannot take the placement in
  its constructor: the framework-constructed carve-out, documented at the read.
- Modify: `src/contour/ContourGuiApp.hpp/.cpp` — `command::MenuBarPlacement _menuBarPlacement = command::MenuBarPlacement::None;`,
  set in the constructor's initialiser list (`ContourGuiApp.cpp:414-…`; the signature ends at `:413`) from
  `command::menuBarPlacementFor(config::detectHostPlatform(env))`, and its accessor `menuBarPlacement()`.
- Test: new `src/contour/display/ShortcutOverride_test.cpp` (add to `contour_gui_test`; it needs `QKeyEvent`
  but no display); `src/contour/display/DisplayRendering_test.cpp` (one `[display]` case).

**Interfaces:**
- Consumes: `command::MenuBarPlacement` (Task 10).
- Produces (namespace `contour::display`, `ShortcutOverride.hpp`):
  - `enum class ShortcutOverrideClaim : uint8_t { Terminal = 0, Menu };`
  - `struct MenuOwnedKey { command::MenuItemRole role; QKeySequence::StandardKey key; };` and
    `inline constexpr std::array MenuOwnedKeys { MenuOwnedKey { command::MenuItemRole::Preferences, QKeySequence::Preferences }, MenuOwnedKey { command::MenuItemRole::Quit, QKeySequence::Quit } };`
    (About has no key). This is the one role-to-key table (R-j): the claim reads its keys, and Task 12's
    menu bar gives a role row its key from it, so the two cannot drift. It lives here, not in
    `window/AppMenuBar`, because `display/` may not include `window/`.
  - `[[nodiscard]] inline std::optional<QKeySequence::StandardKey> standardKeyFor(command::MenuItemRole role) noexcept;`
    — the table's lookup.
  - `[[nodiscard]] inline ShortcutOverrideClaim shortcutOverrideClaim(QKeyEvent const& event, command::MenuBarPlacement placement);`
    It returns `Menu` only when `placement == Global` and `event.matches(row.key)` for some row of
    `MenuOwnedKeys`, and `Terminal` otherwise.
- `TerminalDisplay::event`: on `QEvent::ShortcutOverride` with a session, a `Terminal` claim calls
  `accept()` on the key event and returns `true`, on every host. A `Menu` claim falls through to
  `QQuickItem::event`, which leaves the event ignored. Nothing off macOS consumes shortcuts (no `QShortcut`,
  no QML `Shortcut`), so the claim changes nothing there; only under `Global` does any key go to the menu.
- `ContourGuiApp::menuBarPlacement() const noexcept`.

- [ ] **Step 1: Write the failing tests.**
  - `ShortcutOverride_test.cpp`, `[shortcutoverride]`. Build each event from a standard key's own binding, so
    nothing is platform-hard-coded:
    `auto const combo = QKeySequence(QKeySequence::Quit)[0]; QKeyEvent event(QEvent::ShortcutOverride, combo.key(), combo.keyboardModifiers());`
    - Quit and Preferences under `Global` → `Menu`; both under `None` → `Terminal`.
    - `standardKeyFor(Quit) == QKeySequence::Quit`, `standardKeyFor(Preferences) == QKeySequence::Preferences`,
      and `About` and `None` give `std::nullopt`.
    - `QKeySequence::Copy`'s key, `Ctrl+Alt+K` and a plain `A`, each under `Global` → `Terminal`.
    - The test skips a standard key whose binding is empty on this platform (`QKeySequence(k).isEmpty()`;
      Windows may define neither). On Apple it first `REQUIRE`s both to be non-empty, so the `Menu` cases
      cannot pass vacuously where they matter.
  - `DisplayRendering_test.cpp`, `[display][shortcutoverride]`: on the live display, send `Ctrl+Alt+K` as a
    `ShortcutOverride` after `event.ignore()`. It comes back accepted. Before this task the event comes back
    ignored, so this red is real.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: compile failure, because `ShortcutOverride.hpp` does not exist.

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[shortcutoverride]" && CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][shortcutoverride]"`

Expected: PASS. The second run opens a window on this Mac.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/display/ShortcutOverride*.{hpp,cpp} src/contour/display/TerminalDisplay.cpp src/contour/display/DisplayRendering_test.cpp src/contour/ContourGuiApp.{hpp,cpp}
editorconfig-checker src/contour/CMakeLists.txt
git add src/contour/display src/contour/ContourGuiApp.* src/contour/CMakeLists.txt
git commit -m "display: the terminal keeps every key but Quit and Settings under a global menu bar"
```

---

### Task 12: `AppMenuBar`, and the Window and Help menus handed to AppKit

**Files:**
- Create: `src/contour/window/AppMenuBar.hpp`, `src/contour/window/AppMenuBar.cpp` (Qt Widgets); list them in
  `src/contour/CMakeLists.txt`'s `_header_files` (`:264-…`) and `_core_source_files` (`:307-…`).
- Create: `src/contour/platform/MenuBarIntegration.hpp` (the interface, with `NullMenuBarIntegration` inline)
  and `.cpp` (the non-Apple factory inside `#ifndef __APPLE__`); list both in `add_library(contour_platform …)`
  (`src/contour/platform/CMakeLists.txt:15-69`). Create `src/contour/platform/CocoaMenuBarIntegration.mm`
  (ARC), added in the `if(APPLE)` block (`:81-95`) with `-fobjc-arc`, and link `Qt6::Widgets` `PRIVATE`
  inside that block.
- Modify: `src/contour/platform/AppearanceServices.hpp` — the `menuBar` factory, named in
  `nullAppearanceServices()`; `src/contour/test/AppearanceFixtures.hpp` names it in `appearanceServicesWith`.
- Modify: `src/contour/window/WindowController.hpp/.cpp` — `menuBarState()`, `runMenuBarAction()`.
- Modify: `src/contour/ContourGuiApp.hpp/.cpp` — `_menuBarIntegration` (from services, in the constructor),
  `_appMenuBar`, created after the QML engine block (`ContourGuiApp.cpp:2335`) when
  `_menuBarPlacement == Global`; reset in `releaseQtObjects` (`:2133-2144`) before `_qmlEngine`; rebuilt from
  `onConfigurationReloaded()` (`:1787-1794`), guarded as the notice is.
- Modify: `src/contour/test/test_main.cpp:110` — a `QApplication` when `displayTestsEnabled()` (R-o).
- Create: `src/contour/window/AppMenuBar_test.cpp` (add to `contour_gui_test`), and
  `src/contour/platform/CocoaMenuBarIntegration_test.mm` (add to the `if(APPLE)` test block, `-fobjc-arc`).
- Modify: `src/contour/test/MenuFixtures.hpp` — `RecordingMenuBarIntegration`;
  `src/contour/platform/AppearanceSources_test.cpp` (one case).

**Interfaces:**
- Consumes: `buildMenuBar`, `MenuBarState`, `menuBarStateOf`, `MenuBarMenu`, `MenuBarMenuCount` (Task 10);
  `buildMenuTree`, `chordIndex` (Task 4); `menuBarPlacement()`, `standardKeyFor` (Task 11).
- Produces (namespace `contour::platform`, `MenuBarIntegration.hpp`; `class QMenu;` forward-declared):

```cpp
/// Hands the menu bar's Window and Help menus to the operating system.
class IMenuBarIntegration
{
  public:
    virtual ~IMenuBarIntegration() = default;
    /// Makes @p windows the menu the OS lists its windows in, and @p help the one it adds its help
    /// search to. Called once per menu bar, with menus that outlive this call.
    /// @pre GUI (main) thread; on macOS, a native QMenuBar holds both menus.
    virtual void adopt(QMenu& windows, QMenu& help) = 0;
};
class NullMenuBarIntegration final: public IMenuBarIntegration { /* adopt() does nothing, inline */ };
[[nodiscard]] std::unique_ptr<IMenuBarIntegration> makeMenuBarIntegration();
```

  The Cocoa `adopt` sets `NSApp.windowsMenu = windows.toNSMenu()` and `NSApp.helpMenu = help.toNSMenu()`,
  skipping a nil menu. It is `@pre` main thread, documented and asserted with `NSThread.isMainThread`, as
  the Task 6 presenter is (AppKit is main-thread only). `AppearanceServices` gains
  `std::function<std::unique_ptr<IMenuBarIntegration>()> menuBar = makeMenuBarIntegration;`.
- Produces (namespace `contour::window`, `AppMenuBar.hpp`):
  - `[[nodiscard]] QAction::MenuRole qtMenuRoleFor(command::MenuItemRole role, command::MenuBarMenu menu) noexcept;`
    is a table lookup: `About` → `AboutRole`, `Preferences` → `PreferencesRole`, `Quit` → `QuitRole`. `None`
    gives `ApplicationSpecificRole` in `MenuBarMenu::Application` and `NoRole` elsewhere (R-k). `QAction` is
    in QtGui in Qt 6, so this needs no `QApplication`.
  - `[[nodiscard]] std::optional<QKeyCombination> qtKeyFor(command::ShortcutChord const& chord);` maps the
    `MenuNode`'s `chord`, never its display text: the defaults bind ViNormalMode to Ctrl+Shift+Space
    (`Config.hpp:1606-1615`), whose text is "⌃⇧ " while Qt's native text says "Space", so parsing the text
    would fail on a default chord. It is a table lookup. Modifiers go through the inverse of
    `input/KeyMapping.cpp:62-72` (Control → `Qt::ControlModifier`, Super → `Qt::MetaModifier`, under
    `AA_MacDontSwapCtrlAndMeta`; Alt and Shift as named). Keys go through a `vtbackend::Key` → `Qt::Key`
    table (the inverse of `KeyMappings`, `session/SessionInput.cpp:388-…`), and chars through `QChar`. A
    chord with Hyper or Meta gives `std::nullopt`, so a chord Qt has no key for is not shown wrong.
  - `[[nodiscard]] bool carriesKeyEquivalent(command::ShortcutChord const& chord) noexcept;` — true when the
    chord holds Control or Super (⌃ or ⌘). Only such a chord becomes a row's key equivalent (R-i); a row
    whose chord fails it gets no shortcut, so AppKit cannot fire "Clear Selection" on Escape or scroll on ⇧↓
    from the palette's or Settings' text fields.
  - `struct AppMenuBarSources { std::function<command::MenuBarState()> state {}; std::function<command::ChordIndex()> chords {}; std::function<void(actions::Action const&)> run {}; };`
  - `class AppMenuBar` with
    `AppMenuBar(AppMenuBarSources sources, platform::IMenuBarIntegration& integration);`,
    `void rebuild();` and `[[nodiscard]] QMenuBar& menuBar() noexcept;`. `@pre` a `QApplication` exists, on
    the GUI thread.
    - The constructor creates a parentless `QMenuBar` and one `QMenu` per `MenuBarMenu`, kept for the bar's
      life so their NSMenus stay the ones AppKit adopted. It fills them (`rebuild()`), calls
      `integration.adopt(window, help)` once, and hides the Application menu's `menuAction()` (R-k).
    - `rebuild()` builds one tree,
      `buildMenuTree(buildMenuBar(state()), chords(), command::platformShortcutStyle())`, and refills every
      menu's actions in the `QMenu`s it keeps: `QAction` text, `setEnabled`, `setCheckable`/`setChecked`,
      `setMenuRole(qtMenuRoleFor(…))`, separators and sub-menus (a sub-menu's `menuAction()->setEnabled`
      from its availability). The shortcut is data too: a role row with `standardKeyFor(node.role)` gets
      `QKeySequence(*key)`, never the binding's chord (R-j), so "Quit Contour" shows ⌘Q and "Settings…" ⌘,.
      Any other row gets `QKeySequence(*qtKeyFor(*node.chord))` when it has a chord that maps and
      `carriesKeyEquivalent`, and no shortcut otherwise. `triggered` posts `run` with a copy of
      `tree.actions[node.actionIndex]`, captured when the row is built, via `platform::postToObject` on the
      bar. A rebuild while a post is queued then cannot change what runs. The full refill runs at
      construction and on reload only.
    - Each top-level `QMenu`'s `aboutToShow` refreshes only that menu's state: `setEnabled`/`setChecked`
      over its existing actions from a fresh tree, plus the Window menu's tab list. It re-creates no other
      action, so the Window menu AppKit adopted (and appended its window list to) and the role actions Qt
      merged into the app menu stay the objects they were.
- Produces (`WindowController`):
  - `[[nodiscard]] command::MenuBarState menuBarState() const;` starts from `menuBarStateOf(config)` and adds
    this window: `Present`, the active session's selection, `canSpeak` and input protection (as its
    `contextMenuState()` reports them), `count()`, `activeTabIndex()`, the active tab's pane count,
    `tabTitles()`, the live tab bar modes and the active profile.
  - `void runMenuBarAction(actions::Action const& action);` runs on `activeSession()`, or does nothing when
    there is none.
- `ContourGuiApp`'s sources: `state` is the focused controller's `menuBarState()`
  (`_sessionManager.focusedWindow()` → `controllerFor`), or `menuBarStateOf(_config)` without one; `chords` is
  `command::chordIndex(_config)`; `run` is the focused controller's `runMenuBarAction`.
- `test_main.cpp`: `auto const app = contour::test::displayTestsEnabled(core::LiveEnvironment {}) ? std::unique_ptr<QGuiApplication> { std::make_unique<QApplication>(argc, argv) } : std::make_unique<QGuiApplication>(argc, argv);`
- **TUI row:** on macOS under `ui_style: terminal` the menu bar is the same native bar (R-n). No in-window
  surface changes.

- [ ] **Step 1: Write the failing tests.**
  - `AppMenuBar_test.cpp`, `[menubar][keys]` (offscreen, QtGui only):
    - "every role maps": `qtMenuRoleFor` for each `MenuItemRole` × {`Application`, `Edit`} matches the table
      above.
    - "every default chord maps": for both editions' defaults, every chord in `chordIndex(config)` gives an
      engaged `qtKeyFor`, Ctrl+Shift+Space (ViNormalMode) included, as `Qt::Key_Space` with Control and
      Shift. If a default chord does not map, record it in the ledger and stop for a ruling.
    - "a chord Qt has no key for is dropped": `qtKeyFor({ .modifiers = Modifiers { Modifier::Hyper }, .input = U'é' })`
      is `std::nullopt`, and so is the same chord with `Modifier::Meta`.
    - "only ⌃ and ⌘ chords are key equivalents": `carriesKeyEquivalent` holds for Ctrl+C and for a
      Super chord, and not for Escape or Shift+Down.
  - `AppMenuBar_test.cpp`, `[display][menubar]`, skipped unless
    `qobject_cast<QApplication*>(QCoreApplication::instance())`, with a `RecordingMenuBarIntegration`:
    - "the bar holds the six menus": `menuBar().actions().size() == MenuBarMenuCount`, with the table's titles
      in order. `adopt` was called once, with the Window and Help menus.
    - "the rows are the tree's": in each menu, the non-separator actions, counted recursively, equal the
      tree's command nodes, and the About/Settings…/Quit actions carry their roles.
    - "the Quit and Settings… actions carry the standard keys": their shortcuts are
      `QKeySequence(QKeySequence::Quit)` and `QKeySequence(QKeySequence::Preferences)`, not the defaults'
      Ctrl+Shift+Q and Ctrl+Shift+,.
    - "a chord without ⌃ or ⌘ is no key equivalent": "Clear Selection" has an empty shortcut, and "Copy"
      has one.
    - "the role rows survive a rebuild": after `rebuild()`, the About, Settings… and Quit actions are still
      present, with their roles.
    - "a triggered row runs its action": triggering "New Tab", then `processEvents()`, makes `run` receive a
      `CreateNewTab` (`commandId == "CreateNewTab"`).
    - "a menu opens with the current state": with `state()` returning `tabCount = 1`, "Close Other Tabs" is
      disabled. Change the stub to `tabCount = 2` and emit the Window menu's `aboutToShow`: it is enabled.
    - On Apple, "Contour appears once": the Application menu's `menuAction()->isVisible()` is false.
  - `CocoaMenuBarIntegration_test.mm`, `[display][menubar][cocoa]`. Each case skips unless
    `qobject_cast<QApplication*>(QCoreApplication::instance())`, as `AppMenuBar_test` does, and unless
    `contour::test::displayTestsEnabled(core::LiveEnvironment {})`, as `REQUIRE_DISPLAY_OR_SKIP()` does
    (`DisplayRendering_test.cpp:88-97`; the macro is local to that file). `contour_gui_test` runs offscreen
    under a `QGuiApplication` (`test_main.cpp:96-110`), where constructing a `QMenu` is fatal and would
    abort the whole macOS suite in CI, and `toNSMenu()` is nil off the cocoa plugin.
    - After `makeMenuBarIntegration()->adopt(window, help)` on two `QMenu`s of a native `QMenuBar`,
      `NSApp.windowsMenu == window.toNSMenu()` and `NSApp.helpMenu == help.toNSMenu()`.
    - "a rebuild keeps the adopted menus": an `AppMenuBar` over the real integration; after `rebuild()`,
      `NSApp.windowsMenu` is the same `NSMenu` as before it.
  - `AppearanceSources_test.cpp`, `[appearance-source]`: `nullAppearanceServices().menuBar()` and
    `appearanceServicesWith(…).menuBar()` are `NullMenuBarIntegration`s (`dynamic_cast` is non-null), so no
    test can reach the Cocoa integration.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: compile failure on `AppMenuBar.hpp`.

- [ ] **Step 3: Implement** the integration (null, factory, `.mm`), `AppMenuBar`, the controller additions,
  the app wiring and the test runner change.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[menubar][keys],[appearance-source],[windowmenus]" && CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][menubar]"`

Expected: PASS. The display run also exercises the Cocoa integration on this Mac.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/AppMenuBar*.{hpp,cpp} src/contour/platform/MenuBarIntegration.{hpp,cpp} src/contour/platform/CocoaMenuBarIntegration*.mm src/contour/platform/AppearanceServices.hpp src/contour/platform/AppearanceSources_test.cpp src/contour/test/AppearanceFixtures.hpp src/contour/test/MenuFixtures.hpp src/contour/test/test_main.cpp src/contour/window/WindowController.{hpp,cpp} src/contour/ContourGuiApp.{hpp,cpp}
editorconfig-checker src/contour/platform/CMakeLists.txt src/contour/CMakeLists.txt
git add src/contour/window src/contour/platform src/contour/test src/contour/ContourGuiApp.* src/contour/CMakeLists.txt
git commit -m "contour: a native macOS menu bar of six menus, built from the table"
```

---

### Task 13: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts CMakeLists.txt`,
  so the plan's own commit is left out) and apply its findings. Re-run the filters of every task it touches.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[contextmenu],[menu],[actions],[tabmenu],[newtabmenu],[menutree],[contour][palette],[command],[chrometokens],[menubar],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[contextmenumodel],[windowmenus],[menurouting],[appearance-source],[nativemenu],[menus],[about],[dialog],[a11y],[snapshot],[uistyle],[shortcutoverride],[menubar][keys],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[menus],[about],[snapshot]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots and the display cases on this Mac's display** (windows flash; CI runs them under
  weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot],[display][menubar],[display][shortcutoverride]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, where
  `SnapshotScripts.size()` is read from `ChromeSnapshotPlan.hpp`. Look at `tab-menu.*`, `about.*` and two
  existing scripts (`palette.*`, `kit-dialog.*`): R-o's `QApplication` must leave the existing pictures
  looking as they did.
- [ ] **Step 4: Manual check of the app on this Mac** (`out/appleclang-debug/src/contour/contour.app`), with the
  results recorded in the ledger:
  - Right-click a tab: an NSMenu with swatch images opens. "Close Other Tabs" keeps the right-clicked tab.
  - After a right-click menu over the terminal is dismissed, a left-drag selects (the press's grabber saw
    its release, R-m).
  - The menu bar shows Contour (About, Settings…, Quit), Shell, Edit, View, Window and Help. The Window menu
    lists the open windows, and Help has the search field.
  - ⌘Q quits and ⌘, opens Settings, and the app menu shows them as ⌘Q and ⌘,. A bound chord shown in the
    menu (e.g. Classic's Ctrl+Shift+T) reaches the terminal's binding: vi mode's `Ctrl+C` does not copy.
  - AppKit's own key equivalents still work with the terminal focused: Hide (⌘H), Hide Others (⌥⌘H) and
    Minimize. If any fails, record it in the ledger; the fix is a row of `MenuOwnedKeys` (Task 11), data,
    not code.
  - Under `ui_style: terminal`, a tab right-click opens the in-scene menu and the menu bar is unchanged.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release at `:107`
  (the `x.y.z` block at `:97-105` is a commented-out template): a native macOS menu bar; native context
  menus on macOS; the tab menu gains a colour row and Move Tab to New Window; the new actions CloseOtherTabs,
  CloseTabsToRight, MoveTabToNewWindow and ShowAbout.
- [ ] **Step 6:** Mark Step 1b done in the plans index and on the board, and update the PR body with
  `/update-pr`, reporting:
  - **Performance:** none on the render path; a menu tree is built per right-click and per menu-bar open.
  - **Risk:** medium. The key dispatch on macOS (Task 11), the tab menu's targeting, and a new
    `QApplication` in display test runs.
  - **Coverage:** the new Catch2 cases, the `[cocoa]` outline, the `[display][menubar]` smoke test and two
    snapshot scripts.

---

## Owner decisions

1. **The native presenter builds an NSMenu itself, not a QMenu (R-a).** §3.2 names `QMenu`. Confirm, or rule
   that `QMenu` must be used. The latter needs a spike showing that `QMenu::popup()` is native on macOS 15+
   with Qt 6.11.
2. **The tab menu's new rows reach Linux and Windows (R-c).** Confirm that §3.2's table applies on every OS,
   or rule that Linux and Windows keep today's five rows until 5a/5b.
3. **About is a KitDialog on every row (R-f).** Confirm, or ask for AppKit's standard About panel on macOS,
   which would be one more `.mm` behind an `IAboutPanel` with the dialog as its fallback.
4. **The menu bar's contents (Task 10's table).** The canvas carries no menu-bar board. Review the rows, the
   titles, and the five reachability exemptions.
