# SDD ledger — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-1b.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.2 (binds), with what it cites (§1,
§2.1, §2.5, §2.6). Scanned against HEAD 96baa447 on `feature/native-redesign-phase0`; the only commit after the plan's
stated base d6f91bd6 is the plan itself, so its line numbers are judged at d6f91bd6 = HEAD for code. Rulings R14 (TestApp
is Classic) and R21 (NSMenu directly on macOS; the swatch row and "Move Tab to New Window" on every OS; About as an
in-scene KitDialog; the six-menu bar as proposed) are taken as settled and not reopened.

## Pre-flight scan

### 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → 3 | `RowAvailability`, `RowCheck`, `ContextMenuEntry::swatch`, `Row::withRole` | Swatch rows built as entries in a `ChildBuilder`; `Reset`/`Close*` availability | Consistent. **F22**: the plan never fixes the new members' order, and every `ContextMenuEntry { … }` in the tree is a designated initializer that must follow it. |
| 1 → 4 | The entry's `availability`/`check`/`role`/`swatch` | `MenuNode` copies each column (`copiesEveryColumn`) | Consistent. |
| 1 → 6 | `RowAvailability`, `RowCheck` | `NativeMenuOutline` fields | Consistent. |
| 1 → 7 | `ContextMenuModel_test.cpp` (old `toContextMenuModel(vector, actions&)` overload) | Task 7 replaces the overload with `toContextMenuModel(span<MenuNode const>)` and rewrites the cases | Consistent: Task 7 lists the file. The old overload has no caller after Task 7 and should be deleted there. |
| 1 → 10 | `withRole`; `ContextMenuTable.hpp` (the role column) | Contour menu roles; `changeProfileRows`/`changeProfileSubmenu` moved into the same header | Same file, disjoint parts. **F12**: Task 10 also needs two things `MenuTable` lacks: rows spliced in place (the tab list), and a submenu that honours its `enabled` predicate. |
| 2 → 3 | `CloseOtherTabs`, `CloseTabsToRight`, `MoveTabToNewWindow` | Tab-menu rows | Consistent. |
| 2 → 7 | `WindowController::runTabAction(int, Action)`, `WindowMenus_test.cpp` | `triggerSurfaceAction(Tab, …)` → `runTabAction(rowOfTab(*pinned), …)`; the new `[windowmenus]` cases | Signatures match. **F10**: the manager's `runTabAction` is to call `rowOfTab`, which is private (`WindowController.hpp:729`). **F19**: `WindowMenus_test.cpp` uses `TestApp`, so it belongs in the heavy list. |
| 2 → 9 | `showAbout()` / `aboutRequested()` | Main.qml `onAboutRequested`; the mock's `showAbout` | Consistent. |
| 2 → 10 | The four new actions | Contour/Window rows; reachability union | Consistent. Checked by hand: the plan's union plus the five exemptions covers all 115 alternatives (111 today + 4). |
| 3 → 6 | `buildTabContextMenu` | `[cocoa]` outline test | Consistent. |
| 3 → 7 | `TabContextMenuState`, `buildTabContextMenu`, `buildNewTabMenu` | `openTabContextMenu` fills the state from `tabAtRow`, `count()`, `colorPalette()` (span → vector); `openNewTabMenu` | Consistent. Profile order matches today's: `SettingsController` sorts too (`SettingsController.cpp:2014`). |
| 3 → 10 | `buildTabBarMenu(TabBarMenuState)` | View › "Tab Bar" (R-l); `tabBarMirrorsTheTitleBar` | Consistent. A captureless lambda converts to `ChildBuilder` (`ContextMenuTable.hpp:40`). |
| 4 → 5 | `MenuTree` | `INativeMenuPresenter::present`; `RecordingMenuPresenter::trees` | **Conflict F9**: "one recorded tree equal to the one passed" won't compile. `MenuTree` has no `operator==`, and most `actions::Action` alternatives (e.g. `CancelSelection{}`) have none either. |
| 4 → 6 | `MenuNode::actionIndex` | `NSMenuItem.tag` | Consistent. |
| 4 → 7 | `buildMenuTree`, `chordIndex` | Every surface's tree; the end-to-end `QmlComponents_test.cpp:513-520` | Consistent. Chords are computed for in-scene and native context menus that never show them (R-i). Harmless. |
| 4 → 10 | The R-h first-in-walk rule | `noChordTwice` over both editions and a crafted collision | Consistent. `Config.hpp:1291-1299` (Escape → CancelSelection first) and `:1562-1578` (two Ctrl+C rows) are right. `HintMode{url,Open}` matches `:1664-1667`. `Config { .defaultsEdition = Classic }` is valid (`Config_test.cpp:119`). |
| 4 → 12 | `MenuNode::shortcut` (glyph text), `MenuNode::chord` | `qtShortcutFor(node.shortcut)` parses the text | **Conflict F6**: the defaults bind ViNormalMode to Ctrl+Shift+Space (`Config.hpp:1606-1615`). `inputText` spells that "⌃⇧ ", and Qt spells it "⌃⇧Space", so "a glyph chord round-trips" fails in both editions, which triggers the plan's own stop. **Conflict F5**: the role rows get the binding's chord. |
| 5 → 6 | `NativeMenuPresenter.hpp/.cpp`, the unguarded `makeNativeMenuPresenter` | The `.cpp` factory moves inside `#ifndef __APPLE__`; the `.mm` defines the Apple one | Shape matches `PlatformAppearance.cpp`. **Conflict F7**: the ARC `.mm` casts `winId()` with `reinterpret_cast<NSView*>`, which ARC rejects. It also lacks the `platformName() == "cocoa"` guard (`CocoaWindowFrame.mm:102`). |
| 5 → 7 | `presentMenu`, `MenuAnchor`, `menuPresentation()`, `nativeMenuPresenter()` | `WindowController` members bound from `manager.app()` in the initialiser list; `presentSurface` | Consistent. `_chromeStyle` is resolved once (`ContourGuiApp.cpp:2209`), so capturing `_menuPresentation` at construction is safe. **F11**: the native route blocks in AppKit inside a mouse-press / QML handler. |
| 5 → 8 | `ChromeTokenGroups.hpp` (`LayoutTokens::menu`, the four rows' `.layout`) | Same file: `SizeTokens::swatchWidth/Height`, the four rows' `.size`, `everyRowMatchesTheDesignKit` | Sequential edits to the same rows. Consistent. |
| 5 → 12 | `AppearanceServices::menus`, `nullAppearanceServices()`, `appearanceServicesWith`, `MenuFixtures.hpp`, `platform/CMakeLists.txt` link | `.menuBar` added beside it; `RecordingMenuBarIntegration` | Consistent. `main.cpp:234` passes `AppearanceServices {}`, so production gets the native factories. **F23**: only the null factory is tested. |
| 5/11 → 12 | `ContourGuiApp` members `_nativeMenus`, `_menuBarPlacement` | `_menuBarIntegration`, `_appMenuBar` | Declaration order has to keep `_menuBarIntegration` alive longer than `_appMenuBar`. `releaseQtObjects` resets the bar first, so it holds. |
| 6 → 12 | `.mm` in the `if(APPLE)` blocks of `platform/CMakeLists.txt` and `contour_gui_test` (`-fobjc-arc` lists) | A second pair of `.mm` files in the same blocks | Consistent. **Conflict F8**: `CocoaMenuBarIntegration_test.mm` "needs a `QApplication`" but has no skip. In CI's offscreen macOS run, constructing a `QMenu` aborts the whole suite. |
| 7 → 8 | `tabContextMenuModel`, `newTabMenuModel`, `tabContextMenuRequested(int)`, `newTabMenuRequested()`, `trigger*` | `TabItem.qml`, `TabStrip.qml`, the three mocks | Names match. **Conflict F4**: one `ActionContextMenu` named `tabContextMenu` per TabItem. `findChild` finds tab 0's, so the snapshot scene and `ChromeSnapshot_test` miss the menu opened for row 1. Every TabItem also rebuilds on each right-click, and `popup()` opens at the live pointer. **Conflict F3**: `QmlComponents_test.cpp:861-888` opens an unpublished, empty `tabContextMenu`. |
| 7 → 12 | `WindowController.hpp/.cpp` | `menuBarState()`, `runMenuBarAction()` | Disjoint. Consistent. |
| 8 → 9 | `ChromeSnapshotPlan.hpp` (`TabMenu`, `OpenTabMenu`, row, `static_assert`), `ChromeSnapshotScene.hpp`, `MainWindowControllerMock.hpp` (`aboutRequested`) | `About`, `OpenAbout`, row; `showAbout`, `applicationVersion` | **Conflict F2**: `ChromeSnapshotPlan_test.cpp:20` and `ChromeSnapshotPlan.hpp:205` pin `NewDefaultsBanner` as the last enumerator. Task 8 keeps `:20` ("the derived checks"), so it fails, and Task 9 doesn't list the `static_assert`. |
| 9 (alone) | `Q_PROPERTY applicationVersion` returning `QStringLiteral(CONTOUR_VERSION_STRING)` in `WindowController.hpp` | — | **Conflict F1**: the macro is defined only for `Config.cpp`, `ContourApp.cpp`, `ContourGuiApp.cpp` and two tests (`src/contour/CMakeLists.txt:179-199`, deliberately). |
| 10 → 11 | `MenuBarPlacement`, `menuBarPlacementFor` | `ShortcutOverride.hpp` (display/) | Needs Task 5's `contour_platform` → `contour_command` link, since `ContourTerminalDisplay` reaches `command/` only through `contour_platform` (`display/CMakeLists.txt:60`). It holds by order. **F13**: the plan doesn't say how `TerminalDisplay` gets the placement. |
| 10 → 12 | `buildMenuBar`, `MenuBarState`, `menuBarStateOf`, `MenuBarMenu`, `MenuBarMenuCount` | `AppMenuBar` | Consistent. **F20**: `rebuild()` refills all six menus on every `aboutToShow`. |
| 11 → 12 | `MenuOwnedStandardKeys {Quit, Preferences}`, `menuBarPlacement()` | Role rows whose `QAction` shortcut is the binding's chord | **Conflict F5**: the defaults bind Quit to Ctrl+Shift+Q (`Config.hpp:1621-1624`) and OpenConfiguration to Ctrl+Shift+, (`:1598-1605`). The role rows would carry ⌃⇧Q and ⌃⇧, in place of ⌘Q and ⌘,. Task 11 then leaves ⌘Q to the menu, which no longer holds it, which contradicts R-j and Task 13 Step 4. |
| 1–12 → 13 | Each task's filter | Step 2's union | Covers every tag (`[menu]` covers `[contour][titlebar][menu]`). The metainfo target (`:107`, the template at `:95-105`) and the `/simplify` base (the plan commit 96baa447) are right. |

### 2. Per-task self-consistency

| Task | Agrees with itself? | Detail |
|---|---|---|
| 1 | Yes | Files, tests and commit agree. Every construction site listed is real (`ContextMenu.cpp:145-151`, `ContextMenuTable.hpp:154-222`, `TitleBarContextMenu.cpp:44-112`). No other file reads `.enabled`/`.checkable`/`.checked`. **F22** (member order). |
| 2 | Mostly | The handlers, catalogue end (`Actions.hpp:877-880`), concept (`:305`) and docs (`:577`) are placed right. `tearOffTabToNewWindow` works headless (`ContourGuiApp.cpp:2498`), so the transplant case is sound. **F10**: `rowOfTab` is private. **F19**: `WindowMenus_test.cpp` belongs in `contour_gui_test_heavy_sources`. |
| 3 | Yes | Tests match the interfaces. `formatColor` (`Color.hpp:457`), `Tab::color(TabColorSource)` (`Tab.hpp:240`) and `colorPalette()` (`SessionModel.cpp:77`) exist. |
| 4 | Yes | `ShortcutInput` is `variant<Key, char32_t>` (`Shortcut.hpp:27`), so `.input = U'T'` is valid, and `Modifiers{Control, Shift}` is used verbatim in `Config.hpp`. `indexBinding` is at `Shortcut.cpp:43-57`. The tags `[contour][palette]` and `[command]` exist. |
| 5 | No | **F9** (tree equality). The token, rows and test pin match `ChromeTokenGroups.hpp:36-40/213-218/320/340/360/388` and `_test.cpp:85`. **F24**: the CMake lines are stale. **F23**: Review Focus 1 says both null factories are pinned here, but only `nullAppearanceServices()` is tested. |
| 6 | No | **F7** (ARC cast; cocoa guard). "The factory is native on Apple" stays true under the proposed guard, because the guard goes in `present()`, not in the factory. `CocoaNativeMenu.hpp` is not listed in `add_library` (**F19**). |
| 7 | Mostly | The `:327-357`, `:690-694`, `:850-865` and `:145-240` ranges and `WindowController.cpp:56` match. **F11**: blocking in AppKit inside an input handler. The Review Focus 2 and 4 cases are well formed (`rowOfTab` gives -1 for a dead tab). |
| 8 | No | **F2**, **F3**, **F4**. **F15**: `MenuSwatchRow` as a `MenuItem`. **F16**: the ring colour. **F18**: the mock surface is five signals, not three. `MockTabController` is `QmlComponents_test.cpp:87-292`, not `:87-140`. Deleting the two QML files leaves the size baseline untouched, since neither is listed. |
| 9 | No | **F1** (version macro); **F2** (`static_assert` not listed). `KitDialog.qml:94-102` and `Main.qml:304-331` are right. |
| 10 | Partly | **F12**: no splice facility for the tab list; "(window)" on submenus is ignored by `buildRows` (`ContextMenuTable.hpp:198-210`, `.enabled = true`). The reachability list checks out exactly; `LayoutCommandSource::commands()` reads `layouts` keys (`CommandCatalog.cpp:112-120`). |
| 11 | Partly | **F13**: the code claims on every host, but the Architecture section and Review Focus 5 say "only on a host with a global menu bar". The plan also doesn't say where `TerminalDisplay` reads the placement. **F14**: AppKit's own key equivalents (⌘H, ⌥⌘H) would be claimed. `detectHostPlatform` exists (`WindowControlStyle.hpp:298`); `TerminalDisplay::event` is at `:1534-1555`. |
| 12 | No | **F5**, **F6**, **F8**. **F20** (`rebuild` on every `aboutToShow`; the hidden Application menu). **F15** (non-⌘ key equivalents). `QMenu::toNSMenu()` is at `qmenu.h:170-171` (Qt 6.11.1); `QAction::MenuRole`'s default is `TextHeuristicRole`, so mapping `None` to `NoRole` outside the app menu is needed and right. `test_main.cpp:96` is really `:110`. |
| 13 | Yes | The snapshot count formula is derived from the table (192 once Tasks 8 and 9 land). The `0.9.0` block at `:107` is right. |

### Plan references verified against HEAD

Present and correct:
- `ContextMenu.hpp:105-128`, `ContextMenuTable.hpp:59-229` and `:142-167`
- `ContextMenu.cpp:139-153` (`profileRows`), `TitleBarContextMenu.cpp:44-116`
- `ContextMenuModel.hpp:16-60`, `.cpp:10-49` and `:26-41`
- `Shortcut.hpp:148-172`, `Shortcut.cpp:40-127`
- `Actions.hpp:143/161/305/577/877`
- `Config.hpp:1291-1299/1562-1578/1664-1667`
- `TerminalSession.hpp:1176-1190`, `.cpp:5368`
- `TerminalSessionManager.cpp:968`
- `WindowController.hpp:220/258-259/327-357/690-694/850-865`, `.cpp:56/145-240/217-219`
- `MultiWindow_test.cpp:84-90`
- `ChromeTokenGroups.hpp` (all cited lines), `ChromeTokenObjects.hpp:36-70`
- `ChromeSnapshotPlan.hpp:35-47/64-70/107-206`, `ChromeSnapshotScene.hpp:158-175`
- `ChromeSnapshotPlan_test.cpp:17-29/58-75`, `ChromeSnapshot_test.cpp:60-123`
- `QmlComponents_test.cpp:308-348/350-375/491-580/513-520`
- `Accessibility_qml_test.cpp:164-171`
- `TabItem.qml:558-574/595-605`, `TabStrip.qml:292-316` (`:305`), `ActionContextMenu.qml:37-98`, `KitDialog.qml:51-54/94-102`, `Main.qml:304-331`
- `ContourGuiApp.hpp:946-947`, `ContourGuiApp.cpp:1787-1794/2209-2210/2335`
- `AppearanceFixtures.hpp:99-114`
- `src/contour/CMakeLists.txt:384/386/760-818/820-831/1131-1176`
- `command/CMakeLists.txt:8-18`
- `CMakeCache.txt:395` (`CONTOUR_EXTENSIONS_BUNDLED:BOOL=OFF`)
- `metainfo.xml:95-107`

Stale: see F24. No API the plan names is missing except the four it creates. `rowOfTab` (private) and `CONTOUR_VERSION_STRING` (per-source) exist but can't be reached where the plan uses them.

## Findings needing a ruling

Blocking (a task's own steps, the build, or CI fail as written): **F1–F9**. The rest are non-blocking.

1. **F1 (blocking): `CONTOUR_VERSION_STRING` is not defined where Task 9 uses it.**
   - Plan text (Task 9): "`Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)`, returning `QStringLiteral(CONTOUR_VERSION_STRING)` (a build constant, not an ambient read)".
   - Why: `src/contour/CMakeLists.txt:179-199` attaches the macro only to `Config.cpp`, `ContourApp.cpp`, `ContourGuiApp.cpp` and two tests, on purpose, to keep the compile cache valid across worktrees. `WindowController.hpp` is included by most of `contour_core`, so every includer fails to compile.
   - Proposed: add `[[nodiscard]] QString ContourGuiApp::applicationVersion() const` in `ContourGuiApp.cpp`, which has the macro. `WindowController::applicationVersion()` returns `QString` from `_manager.app().applicationVersion()`, defined in `WindowController.cpp`. The value is still a build constant reached through the composition root (§1.4). The mock keeps `"0.0.0-test"`.

2. **F2 (blocking): the snapshot table's "last enumerator" pins (Tasks 8, 9).**
   - Plan text (Task 8): "drop the literal counts at `:21`, `:68`, `:74`, keeping the derived checks beside them". Task 9's Files name "the script `About`, the action `OpenAbout`, the `about` row" only.
   - Why: `ChromeSnapshotPlan_test.cpp:20` checks `size() == NewDefaultsBanner + 1`, which fails once `TabMenu` is appended. `ChromeSnapshotPlan.hpp:205` has the same pin as a `static_assert`, which Task 8 updates and Task 9 doesn't, so Task 9 fails to compile.
   - Proposed: in Task 8, rewrite both pins as `static_cast<std::size_t>(SnapshotScripts.back().script) + 1`. The enumerator-order `static_assert` at `:200-204` already ties each row to its index, so later scripts need no edit (Global Constraints: counts derived from the tables).

3. **F3 (blocking): an existing tab-menu test opens an empty menu (Task 8).**
   - Plan text: Task 8 rewrites `:350-375` only.
   - Why: `QmlComponents_test.cpp:861-888` ("A context menu actually opens") finds `tabContextMenu` on a TabItem host and calls `open()`. After Task 8 that menu's entries are the unpublished `tabContextMenuModel`, so `checkMenuOpensAtUsableSize` fails (`count > 0`, `itemAt(0)`), and Step 4's `[contour][gui][qml]` fails.
   - Proposed: add `:861-888` to Task 8's Test list. It first calls `controller.openTabContextMenu(0, {})` (the mock publishes and requests), then waits for `opened`. Alternatively, fold it into the new "the tab menu opens with the table's rows" case and delete it.

4. **F4 (blocking): one tab menu per TabItem (Tasks 7, 8; fails at Task 13 Step 3).**
   - Plan text: "`TabItem.qml` … `ActionContextMenu { id: contextMenu; objectName: "tabContextMenu"; entries: root.controller ? root.controller.tabContextMenuModel : [] … }` pops on `onTabContextMenuRequested(index)` when `index === root.tabIndex`"; the scene "waits for `tabContextMenu`'s `opened`"; `ChromeSnapshot_test` "finds `tabContextMenu` opened".
   - Why:
     - With three tabs there are three menus of that name. `findChild` returns tab 0's, but the scene opens row 1's, so the snapshot case fails.
     - Every TabItem rebuilds its whole menu on each publish (N× object churn per right-click).
     - `popup()` with no arguments opens at the live pointer, so the snapshot is placed by wherever the developer's mouse is.
   - Proposed:
     - Put one `ActionContextMenu` named `tabContextMenu` in `TabStrip.qml`, bound to `tabContextMenuModel`.
     - `tabContextMenuRequested(int index)` pops it below that tab's delegate, read through the strip's repeater: `popup(item, 0, item.height)`, as the new-tab menu does at `:305`.
     - Rename and Custom… stay per TabItem through the existing `tabTitleEditRequested`/`tabColorPickRequested`.
     - The tests and the scene find the one menu. This also matches §3.2's "one in-scene `ActionContextMenu.qml`".

5. **F5 (blocking): the role rows carry the binding's chord, not ⌘Q / ⌘, (Tasks 11, 12).**
   - Plan text: R-j "⌘Q still quits through the app menu … and ⌘, opens Settings". Task 12: "`setShortcut(qtShortcutFor(node.shortcut))`" for every row. Task 13 Step 4: "⌘Q quits and ⌘, opens Settings".
   - Why:
     - The first bindings are Ctrl+Shift+Q for `Quit` (`Config.hpp:1621-1624`) and Ctrl+Shift+, for `OpenConfiguration` (`:1598-1605`, Insert mode).
     - Qt syncs a merged role item's key equivalent from its `QAction` shortcut, so the app menu would show "Quit Contour ⌃⇧Q", and ⌘Q would match no item.
     - Task 11 then deliberately leaves ⌘Q unclaimed, so it falls through to the terminal as Super+Q.
     - This is a macOS regression, and none of the plan's tests catches it.
   - Proposed: make the role the authority for its key, as data.
     - A role table in `AppMenuBar` maps each `MenuItemRole` to its `QAction::MenuRole` and its `QKeySequence::StandardKey` (Quit, Preferences; About has none).
     - A role row gets that standard key, never the binding's chord.
     - `MenuOwnedStandardKeys` is derived from the same table, so Task 11 and Task 12 cannot drift.
     - Add to `[display][menubar]`: "the Quit and Settings… actions carry `QKeySequence::Quit` / `Preferences`".
     - §3.2's "Shortcut text comes from `Shortcut.hpp`" still holds for every other row.

6. **F6 (blocking): the glyph round-trip fails on a default chord (Task 12).**
   - Plan text: "`qtShortcutFor` gives `QKeySequence::fromString(…, NativeText)` when its `toString(NativeText)` gives back @p shortcut … If a default chord fails, record it in the ledger and stop for a ruling."
   - Why: both editions bind ViNormalMode to Ctrl+Shift+Space (`Config.hpp:1606-1615`, `.input = ' '`). `inputText` renders the char as " ", so the text is "⌃⇧ ", and Qt's native text for Key_Space is "Space". The step stops by construction. Parsing display text is also the wrong seam: `MenuNode` already carries the `chord`.
   - Proposed: `[[nodiscard]] std::optional<QKeyCombination> qtKeyFor(command::ShortcutChord const&)`.
     - It is a table lookup: modifiers through the inverse of `input/KeyMapping.cpp:63-72` (Control → `Qt::ControlModifier`, Super → `Qt::MetaModifier`, under `AA_MacDontSwapCtrlAndMeta`), keys through a `vtbackend::Key` → `Qt::Key` table, and chars through `QChar`.
     - It gives `nullopt` for Hyper/Meta.
     - Test: every default chord of both editions maps, and Hyper is dropped. The "drop a chord Qt cannot read" goal is kept.

7. **F7 (blocking): the Cocoa presenter's `winId()` cast and platform guard (Task 6).**
   - Plan text: "`inView` is `reinterpret_cast<NSView*>(window->winId())`", in an ARC `.mm`.
   - Why:
     - ARC rejects an integer-to-object-pointer cast ("cast of 'WId' to 'NSView *' is disallowed with ARC"). The existing casts (`CocoaWindowFrame.mm:27`, `CocoaWindowResizeGrid.mm:51`) are in non-ARC files.
     - `winId()` is an `NSView*` only under the cocoa plugin (`CocoaWindowFrame.mm:102-105`). An offscreen run of the app (the e2e tests on macOS CI) that opens a context menu would send a message to garbage.
   - Proposed:
     - `(__bridge NSView*)reinterpret_cast<void*>(window->winId())`.
     - `present()` returns `std::unexpected(MenuPresentError::Unsupported)` when `QGuiApplication::platformName() != "cocoa"`, checked after the `NoWindow` test. "A position needs a window" still holds offscreen.
     - The factory stays unconditional, so "the factory is native on Apple" holds.

8. **F8 (blocking on CI): `CocoaMenuBarIntegration_test.mm` has no skip (Task 12).**
   - Plan text: "`[display][menubar][cocoa]` (needs a `QApplication`)". `AppMenuBar_test`'s display cases skip "unless `qobject_cast<QApplication*>(…)`", but this file's case states no such guard.
   - Why: `contour_gui_test` runs offscreen under a `QGuiApplication` (`test_main.cpp:96-110`), and constructing a `QMenu` (a `QWidget`) without a `QApplication` is a fatal error. The full macOS suite in CI would abort.
   - Proposed: the same skip as `AppMenuBar_test`, plus `REQUIRE_DISPLAY_OR_SKIP()` (`DisplayRendering_test.cpp:88-91`), since `toNSMenu()` is nil off the cocoa plugin. State it in the test text.

9. **F9 (blocking): `MenuTree` equality doesn't compile (Task 5).**
   - Plan text: "`Native` with `result = {}` → `MenuRoute::Native`, one recorded tree equal to the one passed".
   - Why: `MenuTree` declares no `operator==`, and `std::variant<…>::operator==` needs every alternative's. `CancelSelection{}` and most of the other 110 have none (`Actions.hpp:43-158`; only five block actions default one).
   - Proposed: assert `trees.front().nodes == tree.nodes` (`MenuNode` defaults `==`), and compare the actions by `commandId` element-wise. Don't add `==` to the action structs, which would be scope creep.

10. **F10: the manager can't call `rowOfTab` (Task 2).**
    - Plan text: "`TerminalSessionManager::runTabAction` finds the acting session's tab (`tabHostingSession`) and its row (`rowOfTab`) and calls the hosting controller's `runTabAction`".
    - Why: `rowOfTab` is private (`WindowController.hpp:715-729`), and `WindowController` has no friends.
    - Proposed: give the controller a public `void runTabAction(vtworkspace::TabId tab, actions::Action const& action)` that resolves the row itself and is a no-op for a dead tab. The manager passes `tabHostingSession(acting)->id()`. `triggerSurfaceAction` uses it with the pinned `TabId`, so the int-row overload is the only mapping (R-e).

11. **F11: a blocking NSMenu inside an input handler (Tasks 6, 7).**
    - Plan text: Task 7 "`presentSurface` … Then it calls `presentMenu(_menuPresentation, _nativeMenus, …)`". `openContextMenu` runs inside the right-press handler (`TerminalSession.cpp:3925-3934`), and `openTabContextMenu` inside a QML `TapHandler`.
    - Why:
      - `popUpMenuPositioningItem:` swallows the mouse-up. Qt's own `QCocoaMenu::showPopup` resets the view's mouse buttons afterwards; R-a bypasses Qt, so it doesn't. The press's grabber (TerminalDisplay / the TapHandler) then never sees a release.
      - The nested AppKit loop also runs Qt events while the QML handler's frame is still on the stack. A shell exit that closes the tab destroys the TabItem under it.
    - Proposed: the `Native` route posts the presentation (`platform::postToObject(this, …)`), so `present()` runs on a clean stack after the press handler returns. The release reaches Qt first, and AppKit keeps a menu opened after mouse-up in click-click mode. Pin it in Task 13 Step 4: after a right-click menu is dismissed, a left-drag selects. The owner's NSMenu ruling (R21) stands; this only fixes when it is called.

12. **F12: `MenuTable` lacks what Task 10's table needs (Task 10).**
    - Plan text: "one row per tab … (a `ChildBuilder` rows function, not a submenu)"; "`changeProfileSubmenu()` (window)"; "Tab Bar › … (window; R-l)"; "Pane › … (the whole submenu `paneCount > 1`)".
    - Why:
      - `MenuTable` only expands builders for `Submenu` rows.
      - `buildRows` writes `.enabled = true` on every submenu (`ContextMenuTable.hpp:203-209`), so the "(window)" and `paneCount > 1` rules on submenus are silently ignored.
    - Proposed: Task 10 adds two things to `ContextMenuTable.hpp`.
      - `Table::rows(ChildBuilder)`: a template-only row kind spliced in place.
      - Submenus honour `enabled` through `RowAvailability`. The presenters already have the column: `QMenu::menuAction()->setEnabled`, `NSMenuItem.enabled`, QML `Menu.enabled`.
      - A `menuBar.windowRowsNeedAWindow` assertion covers "Tab Bar" and "Pane".
      - Alternatively, rule that submenus are `shownWhen` rather than greyed, and reword the table.

13. **F13: the ShortcutOverride claim's scope and seam (Task 11).**
    - Plan text: the Architecture section says "`TerminalDisplay` claims every `ShortcutOverride` except Quit and Preferences on a host with a global menu bar". Review Focus 5 says "the `ShortcutOverride` claim does nothing there". Task 11 says "returns `Menu` only when `placement == Global` and … `Terminal` otherwise", and "a `Terminal` claim calls `accept()`".
    - Why: as specified, the terminal accepts every override on Linux and Windows too, and the `[display][shortcutoverride]` case on CI's weston relies on that. Nothing in the tree consumes shortcuts off macOS (no `QShortcut`, no QML `Shortcut`), so the behaviour is harmless. The text contradicts it, and the plan never says how the QML-constructed `TerminalDisplay` reads the placement.
    - Proposed: keep the code. Reword the Architecture section and Review Focus 5 to "claims every override on every host; only under `Global` does any key go to the menu". `TerminalDisplay` reads `_session->app().menuBarPlacement()` (the session already reaches `app()`, `TerminalDisplay.cpp:1823`). The display is a framework-constructed type, which is the documented carve-out.

14. **F14: AppKit's own key equivalents (Tasks 11, 13).** Hide (⌘H), Hide Others (⌥⌘H) and Minimize are AppKit app-menu or Window-menu items. Under "claim everything except Quit/Preferences" they may stop working, whereas today they work. This can't be verified without Qt's sources here. Proposed: add the three to Task 13 Step 4's manual check. If any fails, add their combinations as rows of the F5 key table (data, not code).

15. **F15: non-⌘ key equivalents fire outside the terminal (Task 12).** Rows such as "Clear Selection ⎋" and "Scroll One Down ⇧↓" become NSMenu key equivalents. When focus is in the palette's or Settings' text fields, which don't claim those overrides, AppKit fires the menu row: Escape clears the terminal selection, and ⇧↓ scrolls it instead of extending the text selection. Proposed (ruling): attach a key equivalent only to chords containing ⌃ or ⌘ (one predicate over `ShortcutChord`, tested). The text still appears in the menu as a disabled hint, or is omitted. Alternatively, accept this as the macOS convention.

16. **F16: `MenuSwatchRow` as a `MenuItem` (Task 8).**
    - Plan text: "`MenuSwatchRow` (a `MenuItem`) … A `TapHandler` emits `swatchPicked`"; "`onClosed` emits `picked(_pendingActionId)`".
    - Why:
      - The row's own `triggered` closes the menu with no pick, which keyboard Enter does too.
      - The order of the item's click and the swatch's tap is unspecified.
      - An unguarded `onClosed` emits `picked(-1)` on every dismiss.
    - Proposed:
      - Make the row a non-triggering `Control` added with `addItem`, as `MenuSeparator` is.
      - Emit on `closed` only when `_pendingActionId >= 0`.
      - Give the row ←/→ + Enter `Keys` handling (it is an `Item`, so Keys may attach), or state that "Custom…" is the keyboard path.

17. **F17: the swatch ring colour on the Terminal row (Task 8).** Plan text: "`border.width` … in `sys.windowText`", and the TUI row's "ringed in the foreground". Under the Terminal row, `SystemPalette` is Qt's palette, not the scheme's. That mismatch is the root cause of 1a's 1:1 banner defect (159b47dc). Proposed: take the ring from a scheme-sourced colour token, the Terminal row's `Foreground` slot (the native rows may keep the token's `windowText` equivalent). Add a contrast assertion against `modelData.color` in the Terminal-row case.

18. **F18: the mock surface is misstated (Task 8).** Plan text: "the two models, the four invokables, the three signals including `aboutRequested`". The models need their two NOTIFY signals, and the QML needs both request signals, so that is five signals. `RoutingMockController` (`KeyboardRouting_test.cpp:200`) inherits `MainWindowControllerMock`. It gets them for free only if they go in the shared base, which the plan should say, since a `Connections` handler without its signal is a QML warning that fails the run-wide gate.

19. **F19: CMake placement.**
    - `WindowMenus_test.cpp` builds a `TestApp`, as `MultiWindow_test.cpp` does, and belongs in `contour_gui_test_heavy_sources` (`src/contour/CMakeLists.txt:848-870`). A source there is not listed in the suite's own sources.
    - `CocoaNativeMenu.hpp`, `NativeMenuPresenter.hpp` and `MenuBarIntegration.hpp` go in `add_library(contour_platform …)`, as `CocoaWindowFrame.hpp` does.

20. **F20: `rebuild()` on every `aboutToShow` (Task 12).**
    - Plan text: "Each top-level `QMenu`'s `aboutToShow` calls `rebuild()`", and `rebuild()` "refills every menu".
    - Why:
      - Opening Edit re-creates the actions of the Window menu, which AppKit has adopted and appended its window list to.
      - It also re-creates the role actions Qt merged into the app menu.
      - The test checks only enablement.
      - Whether a hidden Application `menuAction()` (R-k) still merges its role rows is unverified.
    - Proposed: `aboutToShow` refreshes only that menu's state (`setEnabled`/`setChecked` over its existing actions, plus the Window tab list). The full refill runs on reload only. Add `[display][menubar]` checks that Quit, About and Settings… are present after a rebuild, and that `NSApp.windowsMenu` is unchanged.

21. **F21: departures from §1.3 to ratify.** `display/ShortcutOverride.hpp` is a pure decision outside §1.3's list (`window/`, `command/`, `vtbackend/`). 1a's F10 precedent accepted `config/`. Proposed: accept it, since the decision is about a `QKeyEvent` and belongs beside its only consumer. Alternatively, move it to `window/` (no Qt Widgets either way). Record the choice in the plan's spec resolutions beside R-b.

22. **F22: `ContextMenuEntry`'s member order (Task 1).** Every construction site is a designated initializer, which must follow declaration order. Proposed: state it as `kind, title, action, availability, check, role, swatch, children`, and the same for `Row` (`role` after `checked`).

23. **F23: main-thread and null-factory coverage.**
    - Task 12's Cocoa `adopt` should assert `NSThread.isMainThread`, as Task 6's presenter does (AppKit is main-thread only).
    - Review Focus 1 says "both null factories" are pinned in Task 5, but only `nullAppearanceServices()` is tested. Add one `appearanceServicesWith(…)` case: `.menus()` is null, and later `.menuBar()` records nothing.

24. **F24: stale references.**
    - `platform/CMakeLists.txt:78-92` → `:81-95` (Global Constraints, Tasks 6, 12); `:101-109` → `:99-108`; `:15-66` → `:15-69`.
    - `ContourGuiApp.cpp:411-…` → `:413` (the signature ends at `:412`); `releaseQtObjects` `:2133-2144` → `:2131-2143`.
    - `test_main.cpp:96` → `:110`.
    - `QmlComponents_test.cpp:87-140` → `:87-292`.
    - `src/contour/CMakeLists.txt` `_header_files` `:266-…` → `:264`.
    - Proposed: correct them in the plan text. Later tasks locate code by name, as the plan already says.

## Rulings

## Rulings
- Ruling B1: all 24 pre-flight findings F1–F24 are accepted with their proposed resolutions and folded into the plan text (one docs commit) before Task 1 — the spec is unaffected — cost if wrong: one docs commit to revert.
- Ruling B2 (= R21): NSMenu on macOS; swatch row and Move Tab to New Window on every OS; About as an in-scene KitDialog; the six-menu menu bar as proposed.
- Plan amended d7f2f4f8 (F1–F24; F3/F5/F15/F16/F24 adjusted per agent note). Task 1: BASE d7f2f4f8.
- Task 1: implementer dispatched (sonnet).
- Task 1: implementer DONE 0eb3e889 ([contextmenu],[menu] 34; [contextmenumodel],[contour][gui][qml] 138). Reviewed inline: RowAvailability {Enabled=0,Disabled}, RowCheck {None=0,Unchecked,Checked} (checkable+checked merged — the impossible state is gone), MenuItemRole, optional swatch; Qt-free. Approved.
- Task 1: complete (0eb3e889).
- Task 2: BASE 0eb3e889.
- Task 2: implementer dispatched (sonnet).
- Task 2: implementer DONE 2aa4a6f1 (contour_test 52; [windowmenus] 7). Review dispatched (sonnet; destructive close semantics).
- Task 2: review Spec ✅ Quality Approved (anchor semantics, no index shift, normal close path, transplant on move, all tables). Test-only fix round: session teardown, last-tab anchor no-op, anchor≠active.
- Task 2: fix round committed 3b6e1dce (3 tests; [windowmenus] 10 cases). Accepted.
- Task 2: complete (2aa4a6f1, 3b6e1dce).
- Task 3: BASE 3b6e1dce.
- Task 3: implementer dispatched (sonnet).
- Task 3: implementer DONE 8f7a3069 (16 cases/549). Reviewed inline: tab/new-tab menus via detail::MenuTable; swatch rows generated from the palette (like profile rows). Approved.
- Task 3: complete (8f7a3069).
- Task 4: BASE 8f7a3069.
- Task 4: implementer dispatched (sonnet).
- Task 4: BLOCKED on a full disk (117 MiB free). Owner approved deleting out/appleclang-tracy (2.6 GB); 2.7 GB free now. Implementer resumed from its uncommitted work. Accepted deviation: action order tested with holds_alternative (Action has no ==).
- Task 4: implementer DONE fb582412 ([menutree],[contour][palette],[command] 35; [menu],[tabmenu],[shortcut] 15). Reviewed inline: depth-first walk, chord attached only to the first row showing it, reuses shortcutText/chordIndex. Controller syntax-checked the five changed command TUs with llvm@22 clang -Wmissing-designated-field-initializers: clean. Approved.
- Task 4: complete (fb582412).
- Task 5: BASE fb582412.
- Task 5: implementer dispatched (opus).
- Task 5: implementer DONE ec0977eb ([chrometokens],[menutree] 15; [menurouting],[appearance-source] 13). Reviewed inline: INativeMenuPresenter + inline NullNativeMenuPresenter, std::expected<void, MenuPresentError>, MenuRouting, layout.menu token, MenuFixtures. Note for Task 6: NativeMenuPresenter.cpp's makeNativeMenuPresenter is unguarded today — must go under #ifndef __APPLE__ when the Cocoa factory lands. Approved.
- Task 5: complete (ec0977eb).
- Task 6: BASE ec0977eb.
- Task 6: implementer dispatched (opus); carried the factory-guard note.
- Task 6: implementer DONE 39f2bf14 ([nativemenu] 5 cases/196; [menurouting] 4). Review dispatched (opus).
- Task 6: review Spec ✅ Quality Approved (ARC ownership sound: weak target kept alive by strong local through the modal loop; tag dispatch; main-thread asserts; one factory per platform). Minor fix round: NoWindow when handle()==nullptr; document weak capture for onPicked; include inside #ifdef. Note for Task 7: router must capture weakly.
- Task 6: fix round 1 committed 841b13b0 (NoWindow before winId, test; weak-capture doc; include inside #ifdef). 10 cases/215. Accepted.
- Task 6: complete (39f2bf14, 841b13b0).
- Task 7: BASE 841b13b0.
- Task 7: implementer dispatched (opus), weak-capture rule carried.
- Task 7: implementer DONE 807a820e (154 + 37 cases). Weak capture: this via queued call, TabId, QPointer<QWindow>. Controller: native pick must resolve against the captured tree, not _menus[surface] at pick time (wrong action if reopened) — fix round 1 dispatched with a test.
- Task 7: fix round 1 committed 50a3bf1c (pickFromShown resolves against the shown tree + target; runSurfaceAction; raw this → QPointer; test A-shown/B-published). 22 cases. Accepted.
- Parked (final review): WindowController's presentation mode isn't injectable, so the Native path is tested via the helper, not end to end.
- Task 7: complete (807a820e, 50a3bf1c).
- Task 8: BASE 50a3bf1c.
- Task 8: implementer dispatched (opus); 1a contrast lessons carried.
- Task 8: implementer DONE 0dff3a88 (brief filter 189+1skip; ContourTui 11+1skip; contour_test 17; gui 150+1skip; lint 10/10; 176 snapshots, 44 ContourTui). Deviations accepted: deferred pick checks root.visible; strip forceLayout before popup; submenu opened via triggered; SystemPalette overrides in ContourMenu.qml apply to all in-scene menus. Approved inline.
- Task 8: complete (0dff3a88).
- Task 9: BASE 0dff3a88.
- Task 9: implementer dispatched (sonnet).
- Task 9: implementer DONE 18a73c7f (all requested filters pass; 12 scripts → 192 snapshots). Accepted. Parked for Task 13: About contrast test covers only the "contour" scheme; extend to contour-light/contour-dark like Banner.
- Task 9: complete (18a73c7f).
- Task 10: BASE 18a73c7f.
- Task 10: implementer dispatched (sonnet).
- Task 10: implementer DONE f3ea8308 (53 cases/3953). Accepted (collision test binds the second action under mode: Insert because chordIndex ignores multi-action chords; no extra exemptions needed).
- Task 10: complete (f3ea8308).
- Task 11: BASE f3ea8308.
- Task 11: implementer dispatched (opus).
- Owner instruction (2026-10-25): commit and push everything for hand-over, with the status board link in the PR and plans. Done 59726671: ledgers copied to docs/superpowers/ledgers, 'Resume here' in the plans index, board link in every plan and the PR body. Task 11 WIP left to its implementer's commit.
- Task 11: implementer DONE 0952516f ([shortcutoverride] incl. live display; [input] 60+1skip; [keyboard]; [menubar] 10). Review dispatched.
- Task 11: review Spec ✅ Quality Approved (key delivery, platforms, AA_MacDontSwapCtrlAndMeta, single table verified). Test-only fix round dispatched (Meta+Q under the swap attribute; live ⌘Q refused and KeyPress reaches PTY; bound terminal action). Note for Task 12: Qt's default ⌘H / ⌥⌘H now reach the terminal (spec-intended) — call it out in the menu bar task and release notes.
- 2026-10-25: work stopped on the owner's word. Task 11 fix round (three extra tests) interrupted; its partial edits to src/contour/display/DisplayRendering_test.cpp and ShortcutOverride_test.cpp are left uncommitted in the owner's working tree. Task 11 itself (0952516f) is committed and approved. Next: finish that test round, then Tasks 12–13.
- 2026-10-25 (resumed by a new controller from the branch alone, HEAD c53e6946): the previous fix-round edits and old workspace are gone. Ruling: redo the Task 11 test-only fix round from the ledger's one-line description with a fresh implementer — the round is test-only, so re-deriving the three cases costs at worst a different test shape — if wrong, a re-review flags a missing case.
- Ruling: new commits carry Signed-off-by only, no Co-Authored-By (owner's standing rule) — the PR will be regrouped before merge anyway — cost if wrong: an amended trailer.
- Task 11: fix round 1/5 dispatched (fresh implementer, opus). FIX_BASE c53e6946.
- Owner (2026-10-25): "rebase ASAP". Rebased a copy (branch rebase/native-redesign, worktree ../contour-rebase) onto origin/master 974feaf1: 82 commits. Ruling: dropped 84a597ff (vthost SOCK_CLOEXEC) and took master's PlatformStreams_posix.cpp in fd34db81 — master's 14791d6e already fixes it with an error check, so the PR no longer touches vthost — cost if wrong: re-add a 5-line hunk. Pending: cherry-pick the Task 11 fix-round commit onto it, build, then reset feature/native-redesign-phase0 to it.
- Task 11: fix round 1 committed e001b488 → cbcbb084 after cherry-pick onto the rebase (3 tests + ScopedApplicationAttribute.hpp; [shortcutoverride] 6+1skip offscreen, live 13 assertions/3 cases, live cocoa [shortcutoverride] 32/7). Concern: the ⌘Q unit case skips offscreen (Qt's Quit = Exit key there). feature/native-redesign-phase0 reset to rebase/native-redesign (cbcbb084). Full build running before force-push. Scoped re-review next.
- Task 11: fix round 1/5 (3 addressed, 0 open; commits abbc4759..cbcbb084). Re-review clean.
- Task 11: minor (deferred): ScopedApplicationAttribute's bare bool _wasSetBefore member is undocumented (AGENT.md bool rule).
- Task 11: minor (deferred): the ⌘Q unit and live cases run only under cocoa with CONTOUR_TEST_DISPLAY=1; no CI lane enforces them — a macOS cocoa [shortcutoverride] lane would.
- Task 11: complete (30464032, cbcbb084; rebased hashes).
