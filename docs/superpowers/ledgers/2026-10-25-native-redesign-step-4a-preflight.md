# Pre-flight scan — plan: docs/superpowers/plans/2026-10-25-native-redesign-step-4a.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.3 (binds), with what it cites (§1,
§2.1, §2.5, §2.6). Scanned against HEAD 97e661ac on `feature/native-redesign-phase0`. The commits after the plan's
stated base 79061d14 are 9811812d (text_shaper only), ebe374b1 (the plan) and 97e661ac (the 1b cleanup: comment
spellings in `command/TabMenus.*` and `ContextMenu*.hpp`, plus `AppMenuBar*`, `ShortcutOverride.hpp`,
`CocoaMenuBarIntegration_test.mm`, `ScopedApplicationAttribute.hpp`, and two lines in `QmlComponents_test.cpp` at
:6563). None of them moves a line the plan cites, so the plan's numbers are judged at 79061d14 = HEAD for code.
Owner decisions D1–D3 (`progress.md`) are settled and not reopened. The canvas extents were checked against the local
copies: `mac-settings` 960×820, `win-settings` 1110×920 titled "Contour Settings", `linux-settings` 1100×920, and
`tui` "same separate window, built from ContourTui controls".

## Pre-flight scan

### 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → 2 | `SizeTokens::settings{Width,Height,MinWidth,MinHeight}`, `ChromeSizeTokens` properties of the same names, `SettingsWindowButtons`, `SettingsWindowConventions`, `SettingsWindowPlacement`, `settingsWindowGeometry`, `settingsWindowConventionsFor` | `SettingsWindow.qml` binds `chromeStyle.size.settings*`; the host's `windowFlags`/`title`; `show()` builds a `SettingsWindowPlacement` | Names and types match. Widths scale by `_units.horizontal`, heights by `_units.vertical` (`ChromeTokenObjects.hpp:55-71`), so Task 2's Terminal case (`120 * widthQuantum`, `24 * cellHeight`) agrees with Task 1. **F1**: `show()` measures `frameMargins()` before the window has a platform window. |
| 1 → 3 | `settingsWindowConventionsFor(HostPlatform)` | `ContourGuiApp::settingsWindow()` over `_hostPlatform` | Consistent. `detectHostPlatform(env)` exists (`WindowControlStyle.hpp:298`) and is already called with the constructor's `env` (`ContourGuiApp.cpp:460`). **F16**: `:1758` and `:2319` detect it a second and third time. |
| 1 → 4 | `settingsWindowConventionsFor` | The snapshot scene's host, `rowHost(_variant.row).host` | Consistent. `rowHost` exists (`ChromeSnapshotPlan.hpp:266`). The Terminal and Linux rows render as `HostPlatform::Other` (`:250-251`), so their snapshots are CloseOnly. |
| 1 (alone) | `SettingsWindowConventionRows`, `static_assert` on `KdePlasma + 1` | — | **F2**: a last-enumerator pin. |
| 2 → 3 | `SettingsWindowHost`, `SettingsWindowFactory` (`expected<unique_ptr<QQuickWindow>, SettingsWindowError>(SettingsWindowHost&)`), `loadSettingsWindow`, `SettingsWindowError::Unavailable` | The app's default factory (`Unavailable` while `_qmlEngine` is null); the TestApp factory over `_settingsEngine`; `openSettings` logging the error's name | Signatures match. Production loads `Main` with `loadFromModule` (`ContourGuiApp.cpp:2625`), and the plan's `QQmlComponent::loadFromModule` is the same module path. The tests reach it too: SettingsPageQml_test goes through `loadSettingsWindow` from Task 2 on. |
| 2 → 3 | `SettingsPageQml_test.cpp` (`SettingsPageView` over a host) | The reload case (`:1176-1226`) still uses `windowController->openSettings()` / `settingsController()` through Task 3 | Consistent. Task 3 keeps `WindowController`'s settings surface, and Task 4 rewires the case. |
| 2 → 4 | `SettingsWindowHost`, `loadSettingsWindow`, the `"settingsWindow"`/`"settingsPage"` objectNames | `ChromeSnapshotScene::_settingsWindow`; `ChromeSnapshot_test`'s Settings case (`subject().objectName()`, `findChild("settingsPage")`) | Consistent. QML children of an `ApplicationWindow` have it as QObject parent, which is why `root().findChild("settingsPageLoader")` works today (`ChromeSnapshot_test.cpp:98`). The scene's `_engine` already carries an `IncubationDriver` (`ChromeSnapshotScene.hpp:431`), and `show()` settles after the action (`:177`). |
| 2 → 4 | `SettingsPageQml_test.cpp` (`TestWindow`, `activeWindow`) | Task 4's reload case: the controller becomes `&testApp.app().settingsController()` | Consistent: `SettingsPageView` takes a `SettingsController*`. |
| 2 → 4 | `src/contour/qml/Main.qml` (root becomes `ThemedWindow`, palette block at `:45-79` moved) | Task 4 drops the pane loader's `visible:` (`:247-248`, `:254`) and the settings loader (`:260-273`) | Disjoint parts of the file. No other QML names `appPalette` (only `Main.qml:59-79`). The size baseline for `Main.qml` stays 1 (`anchors.margins: 8`, `:280`). |
| 3 → 4 | `ContourGuiApp::settingsController()`, `settingsWindow()`, `openSettings(QScreen*)`; `onConfigurationReloaded()` refreshing the app's controller | Task 4 removes the manager loop's per-window refresh (`TerminalSessionManager.cpp:786-794`); MultiWindow, TerminalSessionManager and SettingsPageQml cases call `app.app().openSettings(nullptr)` / `settingsController()` | Consistent. `reloadAllSessions()` calls `_app.onConfigurationReloaded()` (`:784`) before the loop, so Task 3's refresh covers what Task 4 removes. Task 3's "a reload refreshes the app's Settings" pins it. **F4**: one rewritten section pins a focus state that production does not reach. |
| 3 → 4 | `WindowController.hpp/.cpp` (`screen()` added beside `isMinimized()`) | Task 4 removes the settings surface from the same files | Disjoint parts. Consistent. |
| 3 → 4 | `TerminalSessionManager::openSettings` → `_app.openSettings(screen)` (no controller state) | Task 4 removes `contentShown` (`:483`) and `revealSession`'s `closeSettings` (`:516-518`) | Consistent. After Task 3, nothing but the tests sets `settingsActive`, so Task 4 removes dead state. |
| 3 → 4 | `GuiTestFixtures.hpp` (TestApp builds Settings windows offscreen on `_settingsEngine`) | Every Task 4 case that opens Settings through the app | Consistent. A plain `QQmlEngine` has no incubation controller, so the page builds synchronously there. (`QQuickWindow` installs its own controller only under a `QQmlApplicationEngine`.) |
| 1b (landed) → 3 | `runMenuBarAction` (`ContourGuiApp.cpp:1836-1851`), `applicationController()` (`TerminalSessionManager.cpp:450-459`), `runWithoutWindow` (`:1862-1880`), `appCommand` rows that are always enabled (`MenuBarTable.cpp:62-66`) | Settings… (`MenuBarTable.cpp:120`) | Reused correctly. With a session, the Application scope runs `controller->runMenuBarAction` → `session->executeAction` (`WindowController.cpp:404-408`, `TerminalSession.cpp:5722-5726`) → `operator()(OpenConfiguration)` → `manager.openSettings(this)` → `_app.openSettings(that window's screen)`. Nothing on that path raises the window, so carry-in 1 is met. With no session or no window, the new `runWithoutWindow` arm gives `openSettings(nullptr)`. The Settings… row stays enabled with no window, because `appCommand` adds no predicate. **F14**: the docs. |
| 1b (landed) → 2/5 | `AppMenuBar::refreshAll` on `focusWindowChanged` (`ContourGuiApp.cpp:2462-2467`); the Window-scoped rows are `enabledWhen(hasWindow)` (`MenuBarTable.cpp:57-60`) | ⌘W in the Settings window (R-k), including the owner's `⌘W → ClosePane` binding | Consistent. With Settings focused, `focusedController()` is null and the state is `menuBarStateOf(_config)` (`:1826-1830`), so every Window row, the bound Close Pane included, is disabled. A disabled item takes no key equivalent, so ⌘W reaches the QML `Shortcut`. Task 5 Step 4 confirms it by hand. **F11**: AppKit's own window list. |
| 4 → 5 | `SnapshotSurface`, the `settings` row; the snapshot count | Task 5 Step 3: `SnapshotScripts.size() × 4 × 2 × 2` | Consistent. The count is derived. Both pins are already derived (`ChromeSnapshotPlan.hpp:222-228`, `ChromeSnapshotPlan_test.cpp:20`), so the column rename touches no pin. This gives 192 files. |
| 1–4 → 5 | Each task's filter | Step 2's union | Covers every tag. `[contour][gui][qml]` covers `[mainwindow]`, `[a11y]`, `[menus]` and `[snapshot]`, and `[settingswindow]` covers `[rules]`, `[host]` and `[routing]`. The `/simplify` base: **F15**. The metainfo target `0.9.0` at `:107` (template `:97-105`) is right. |

### 2. Per-task self-consistency

| Task | Agrees with itself? | Detail |
|---|---|---|
| 1 | Mostly | **Tests vs code:** the two head examples match the current heads (`ChromeTokenGroups_test.cpp:105`, `:135`) with the four values spliced in after `swatchHeight`. `numbersOf` is `:47-73`. The comment `size(9)` is at `:90`. I recomputed all eight placement rows against `reachableWindowPosition` (`WindowPositionStore.cpp:14-22`), and every expected `QRect` is right. **Files:** `SettingsWindowRules.{hpp,cpp}` and its test are created, then listed (`CMakeLists.txt:281`, `:336`, `:798`), staged and formatted. **Filters:** `[chrometokens]` (contour_test, `CMakeLists.txt:1145`) and `[contour][uistyle]` exist, and `[contour][settingswindow][rules]` is created. Only `designated-field` rows name `.size` (no other `SizeTokens` construction in the tree), so the four rows are the whole edit. **F2**. **F10** (row ranges are one line low). |
| 2 | No | **F1**: the "placed on first show" case fails as written. **F7**: the Close-shortcut case does not wait for activation. **F3**: "All 22 cases pass inside the window" overstates the change. 17 cases build the page; 5 (`:823`, `:1117`, `:1139`, `:1228`, `:1290`) build a lone `SettingRow` in their own `TestWindow`. **F5**: `ControllerFixture` is copied. **F6**: the heavy list. **What holds:** the seven sites are exactly `:784-785`, `:943-944`, `:1028-1029`, `:1072-1073`, `:1204-1205`, `:1363-1364` and `:1426-1427`. `page` as a `QPointer<QObject>` keeps every use compiling (`fx.page.get()` at `:451-525`, `fx.page->` elsewhere; there is no `reset`/`release` outside the constructor). `TestWindow` returned by value works by guaranteed elision, as `auto window = TestWindow(1000, 800)` already does. The `profileFields()` rows carry `"int"`/`"string"` types (`SettingsController.cpp:64`, `:173`, `:305`). `setProfileField` keeps the draft, and `refresh()` (`:2001-2066`) rebuilds only the lists, so "closing keeps the draft" holds with `show()` refreshing. Lint: the new files carry no size or colour literal. |
| 3 | Mostly | **Tests vs code:** the five routing cases follow from the interfaces. `ScopedControllerWithOsWindow` (`GuiTestFixtures.hpp:837`) never shows its window, so a minimised window stays minimised, and `screen()` of an unshown window is the primary screen. `runMenuBarRow` is `WindowMenus_test.cpp:120-125`, in an anonymous namespace (**F5**). **Files:** the test goes into the heavy list beside `WindowMenus_test.cpp` (`:879`), as it builds a `TestApp`. **Filters:** `[contour][session][actions]` (TerminalSession_test), `[contour][app]` (ContourGuiApp_test), `[actions]` (Actions_test) and `[menubar]` (MenuBarTable_test, in contour_test) all exist. **Notes:** "four constructors" is three (**F9**). "Collaborators … moved" is really *copied*; Task 4 deletes the originals (**F10**). TerminalSession_test `:1206-1209` now builds and shows a real Settings window: its "safe no-op" comment changes, as the plan says. The interface `openSettings(QScreen*)` returns `void` and logs, which is acceptable for a sink. |
| 4 | Mostly | **Tests vs code:** the new-tab test matches `TabMenus.cpp:96-104` with the separator and Settings rows gone. `QmlComponents_test.cpp:1127-1128` is the right count line, and the mock's model is built by the real table (`MainWindowControllerMock.hpp:55-62`), so it follows. `MultiWindow_test.cpp:441-475` and `TerminalSessionManager_test.cpp:846-854`/`:907-912` are the right spans. **Lint:** `TabStrip.qml` 3 → 2 is right, since the only literal in the removed tab is `2 * chromeStyle.labelPadding` at `:221`. The colour gate counts hex and `Qt.rgba` only, so the removed `"transparent"` changes nothing. **Removals:** every `settingsActive`/`settingsController`/`openSettings`/`closeSettings`/`toggleSettings` use in the tree is listed (the grep matched only the cited sites). `MainWindowScene` has one designated-init site (`ChromeSnapshotScene.hpp:134-142`). **F4**, **F8**, **F10** (the scene's `show()` is `:157-180`). |
| 5 | Mostly | The snapshot count is derived. The 0.9.0 block is right. **F15**: the `/simplify` base now includes 97e661ac. **F11** and **F12**: items for the manual check. |

### 3. References verified against HEAD

Present and correct:
- `ChromeTokenGroups.hpp:114-127` (`SizeTokens`, `swatchHeight` at `:124`)
- `ChromeTokenObjects.hpp:35-76`, `:65-67`
- `ChromeTokenGroups_test.cpp:46-200`; `UiStyleProvider_test.cpp:471-501`
- `WindowPositionStore.hpp:32-35` (`reachableWindowPosition`)
- `SettingsController.hpp:122-126` (the constructor)
- `SettingsPage.qml:2-6`, `:270-272`, `:291`
- `Main.qml:8`, `:45-79`, `:247-248`, `:254`, `:260-273`
- `TabStrip.qml:4-7`, `:211-270`, `:292-294`
- `ContourGuiApp.hpp:250-266`, `:298-301`, `:572-576`, `:1001` (`_qmlEngine`), `:1013` (`_menuBarPlacement`)
- `ContourGuiApp.cpp`:
  - `:284-295` (`quitIfWindowless`);
  - `:401-416`, `:460` (the constructor);
  - `:1845-1851` (the Application route), `:1864-1880` (`runWithoutWindow`), `:1882-1897` (`onConfigurationReloaded`);
  - `:2240-2252` (`releaseQtObjects`, a `core::Finally` lambda);
  - `:2383`, `:2388` (registrations), `:2625` (`loadFromModule`)
- `main.cpp:220-235`
- `Actions.hpp:60`, `:476-479`; `Config.cpp:4645-4651`; `Config.hpp:1605`, `:1619` (both Ctrl+Shift+,); `MenuBarTable.cpp:121`
- `TerminalSession.cpp:4754-4771`
- `TerminalSessionManager.hpp:256-259`
- `TerminalSessionManager.cpp:10`, `:480-492`, `:515-518`, `:749-756`, `:786-794`
- `WindowController.hpp`:
  - `:52`, `:129`, `:134`;
  - `:339-361`, `:385` (doc at `:386`), `:582` (`isMinimized`);
  - `:798-799`, `:810-814`, `:986-991`
- `WindowController.cpp:21`, `:87-104`, `:123-150`, `:532-534`, `:590-592`, `:641-642`, `:650-652`
- `GuiTestFixtures.hpp:483-512`, `:523-542`, `:569-595`, `:735-760`
- `Config_test.cpp:814-834`; `TerminalSession_test.cpp:1197`, `:1204-1205`, `:4207`, `:5129`
- `TabMenus.cpp:97-104`, `TabMenus.hpp:47`; `TabMenus_test.cpp:138-150`
- `WindowMenus_test.cpp:120-127`, `:382-394`
- `QmlComponents_test.cpp:1127-1128`
- `MultiWindow_test.cpp:441-475`
- `TerminalSessionManager_test.cpp:846-854`, `:907-912`
- `SettingsPageQml_test.cpp:62-90`, `:94-128`, `:172-225` (to `:229`), the seven sites, `:1196-1199`
- `MockMainController.hpp:38-40`, `:81-84`, `:170-175`
- `ChromeSnapshotPlan.hpp:58-63`, `:103`, every row's `.page` (`:116`-`:215`)
- `ChromeSnapshotScene.hpp:133-142`, `:328-345`, `:455-466`
- `ChromeSnapshotPlan_test.cpp:26`; `ChromeSnapshot_test.cpp:97-101`, `:258-276`
- `scripts/qml-size-literal-baseline.txt:27` (`TabStrip.qml 3`)
- `src/contour/CMakeLists.txt`:
  - `:276`, `:281`, `:321`, `:336` (sources);
  - `:379-418` (`_qml_files`, `SettingsPage.qml` at `:398`);
  - `:796`, `:798` (the test list); `:858-880` (heavy list, `WindowMenus_test.cpp` at `:879`)
- `out/appleclang-debug/CMakeCache.txt:392` (`CONTOUR_EXTENSIONS_BUNDLED:BOOL=OFF`)
- `metainfo.xml:97-105`, `:107`
- The tools: `/opt/homebrew/opt/llvm@22/bin/clang-format`, `editorconfig-checker`
- Tags present: `[chrometokens]`, `[contour][uistyle]`, `[newtabmenu]`, `[tabmenu]`, `[menubar]`, `[actions]`, `[snapshot]`, `[contour][gui][qml][menus|mainwindow|snapshot|a11y|settings]`, `[contour][multiwindow][settings]`, `[manager][notification]`, `[contour][windowmenus]`, `[contour][session][actions]`, `[contour][app]`, `[config]` (in contour_gui_test via `Config_test.cpp`), `[display][snapshot]`, `[display][menubar]`
- Tags the plan creates: `[settingswindow]` with `[rules]`, `[host]`, `[routing]`

Stale or imprecise (no build impact; see **F10**):
- The `ChromeTokenGroups.hpp` rows' `.size` blocks are one line lower than cited: `MacRow` `:320-329`, `FluentRow` `:342-351`, `LinuxRow` `:364-373`, `TerminalRow` `:390-399`.
- `ChromeSnapshotScene.hpp` `:150-170`: `show()` is `:157-180`, `grab()` `:184`, `window()` `:187`.
- The spec's own cites are stale, not the plan's:
  - "shared engine (`ContourGuiApp.cpp:2169`)": the engine is made at `:2399`;
  - `TabStrip.qml:210-260` is `:211-270`;
  - `Main.qml:203-226` is `:260-273`.

Facts the plan relies on, checked:
- `quitIfWindowless` counts every visible top-level window without a transient parent (`ContourGuiApp.cpp:287-294`), so R-n holds by construction.
- No code iterates the top-level windows or the engine's `rootObjects()` assuming they are terminal windows. The only top-level walk is `:287`.
- The suite-wide gate fails only on QML diagnostics (`test_main.cpp:131-155`), and the suite never sets `quitOnLastWindowClosed`.

## Findings needing a ruling

Blocking (a task's own steps, the build, or CI would fail as written): **F1**. The rest are non-blocking.

1. **F1 (blocking): first-show placement reads the frame of a window that does not exist yet (Task 2).**
   - **Plan text:**
     - Task 2's Interfaces: "`show()` reads `preferredSize` … and the rest from `minimumSize()`, `frameMargins()`, `QGuiApplication::screens()` …".
     - The test "it is placed on first show": "`window()->geometry() == settingsWindowGeometry({ … .frame = frameMargins() })`", evaluated after the show.
   - **Why it fails the test:**
     - On the first show the window has no platform window, so `QWindow::frameMargins()` is empty. The host therefore places it with a zero frame.
     - Once shown, the offscreen QPA reports real margins. `WindowSizing_test.cpp:258` relies on that: `CHECK(window.frameMargins().top() > 0)`.
     - So the test's expected rect uses another frame than the host used. The offscreen screen is 800×600 and the default row is `LinuxRow`'s 1100×920 (`installChromeStyle` resolves Native against Other, `QmlChromeStyle.hpp:78-84`). The host gives `(0,0,800,600)`, while the expectation is narrower and offset by the frame. The case fails on every CI platform.
   - **Why it is a real bug too:** this is the case the plan exists for (Review Focus 4: Windows' 920 on a 768-high work area). The window is placed client-first at y = 0 of the available area, so the native title bar lands above it and cannot be reached.
   - **Proposed:**
     - On a first show, `show()` calls `window->create()` before it measures, then reads `frameMargins()`. This is the codebase's own precedent: `WindowController::restoreWindowPosition` (`WindowController.cpp:1346-1364`), whose comment says "Realize the still-hidden window now, so its actual native frame participates in the clamp".
     - Follow the same precedent's Wayland guard (`:1342-1343`): skip `setPosition` there, where it is ignored anyway.
     - Keep the test as written. With `create()` first, the margins before and after the show agree.

2. **F2: the conventions table is pinned to its last enumerator (Task 1).**
   - **Plan text:** "A `static_assert` pins the row count to `static_cast<size_t>(HostPlatform::KdePlasma) + 1`". The test checks "every enumerator through `std::views::iota(std::size_t { 0 }, SettingsWindowConventionRows.size())`".
   - **Why:**
     - `HostPlatform` has no count constant (`WindowControlStyle.hpp:135-141`).
     - A fifth host added after `KdePlasma` passes the assert unchanged, and then `settingsWindowConventionsFor` indexes past the table. The test cannot catch it either, because it iterates the table's own size.
     - This is the snapshot-table pin problem from 1b's F2, in a new table.
   - **Proposed:**
     - Give each row a `.host` column, and `static_assert` that the rows are in enumerator order. This is exactly how `SnapshotRowHosts` does it (`ChromeSnapshotPlan.hpp:247-259`).
     - Add `inline constexpr std::size_t HostPlatformCount = static_cast<std::size_t>(HostPlatform::KdePlasma) + 1;` beside the enum, in its one place. Pin the table to that constant, and have the test iterate it.

3. **F3: "All 22 `SettingsPageQml_test` cases pass inside the window" (Task 2 Step 4; Task 5 Step 6's risk line).**
   - **Why:** 17 cases build the page and so run in the Settings window. The 5 `SettingRow` cases (`:823`, `:1117`, `:1139`, `:1228`, `:1290`) build one row in their own `TestWindow(600|900, …)` and never touch the page.
   - **Proposed:** say "all 22 pass; the 17 that build the page run inside the Settings window". There is no code change, and the spec's "still passes inside the window" is met.

4. **F4: a rewritten manager section pins a focus state that production never reaches (Task 4).**
   - **Plan text:** "the Settings window covers no pane: after `app.app().openSettings(nullptr)`, `second` is `Focused` and `first` is `VisibleUnfocused`."
   - **Why:**
     - The case's OS window is never bound in that section (`TerminalSessionManager_test.cpp:806`; only the minimised section binds it, `:858`). So nothing reacts when Settings takes activation.
     - In the app, the terminal window deactivates. That runs `onOSWindowActiveChanged` → `clearFocusedWindow` (`WindowController.cpp:1310-1322`, `TerminalSessionManager.cpp:442-448`), and `second` reads `VisibleUnfocused`.
     - The assertion passes for a reason that is not the property it names.
   - **Proposed:** pin what "covers no pane" means: after `openSettings`, neither pane is `Hidden`. Either assert `!= Hidden` for both, or bind the window, show it, open Settings, and expect both to read `VisibleUnfocused`, as the section "a window that loses focus leaves its panes on screen" (`:821-826`) does.

5. **F5: test fixtures copied rather than shared (Tasks 2, 3).**
   - **Plan text:** "a `SettingsController` as `SettingsPageQml_test`'s `ControllerFixture` builds it (`:94-128`)"; "a helper that runs a menu-bar row by title, as `WindowMenus_test.cpp:120-127` does".
   - **Why:** `ControllerFixture` and `runMenuBarRow`/`nodeTitled`/`createSessionTabs` (`WindowMenus_test.cpp:70`, `:107-125`) live in anonymous namespaces. Reusing them means copying them, which AGENT.md's no-duplication rule and Task 5's `/simplify` would then undo.
   - **Proposed:**
     - Move `ControllerFixture` into a `test/SettingsFixtures.hpp`.
     - Move the three menu helpers into the existing `test/MenuFixtures.hpp`.
     - Do both in the task that first needs them: Task 2 and Task 3.

6. **F6: `SettingsWindowHost_test.cpp` is likely a heavy source (Task 2).**
   - **Why:** it includes `Config.hpp`, `GuiConfigStore.hpp` and `SettingsController.hpp`, as `SettingsController_test.cpp` and `SettingsPageQml_test.cpp` do. Both of those are in `contour_gui_test_heavy_sources` (`CMakeLists.txt:873`, `:876`), which by its comment means a peak of at least 2 GB under GCC -O2 -g.
   - **Proposed:** list it in the heavy sources (beside `SettingsController_test.cpp`), or measure as the comment at `:850-857` describes.

7. **F7: the keyboard cases must wait for activation (Task 2).**
   - **Plan text:** "the Close shortcut closes it: `QTest::keySequence(window(), QKeySequence::Close)` leaves `isVisible() == false`."
   - **Why:** a `Qt.WindowShortcut` matches only while its window is `QGuiApplication::focusWindow()`. `show()` only *requests* activation, so the case is racy as written.
   - **Proposed:** `REQUIRE(QTest::qWaitForWindowActive(host.window()))` before the key sequence. The focus case already says this.

8. **F8: a chained comparison and a literal count (Task 4).**
   - **Plan text:** "`rowCount() == window->count() == 2`".
   - **Why:** written as one expression, it compares a `bool` with `2`. And `2` is a count written as a literal (Global Constraints).
   - **Proposed:** name the tab count once (`constexpr auto Tabs = 2;`, passed to `createTabs`), and check `rowCount() == window->count()` and `window->count() == Tabs` separately.

9. **F9: TestApp has three constructors that build the app, not four (Task 3).**
   - **Plan text:** "All four constructors pass `core::platform::defaultSteadyClock().now(), settingsWindowFactory()`".
   - **Why:** `:483`, `:524` and `:571` construct `_app`. `:516` and `:557` delegate to `:483`.
   - **Proposed:** say "the three constructors that build `_app`". Wording only.

10. **F10: wording and line drift (Tasks 1, 3, 4).**
    - Row ranges and `ChromeSnapshotScene.hpp`'s `show()`, as listed under *Stale*.
    - Task 3's "today's collaborators (`WindowController.cpp:90-104`, moved)" is a copy. Task 3 Step 3 keeps the original, and Task 4 deletes it. Say "copied".

11. **F11: AppKit lists the Settings window in the Window menu (macOS; not in the plan).**
    - **Why:** 1b hands the Window menu to AppKit (`NSApp.windowsMenu = menu`, `CocoaMenuBarIntegration.mm:49-51`). AppKit appends every titled window to it, so "Settings" appears in Window ▸ (and in ⌘` cycling) beside the terminal windows.
    - **Proposed:** record what Task 5 Step 4 sees. If it should be excluded, as many settings windows are, that is a follow-up: one `excludedFromWindowsMenu` call behind a platform adapter, not part of 4a.

12. **F12: check that CloseOnly still resizes on macOS (Task 5 Step 4).**
    - **Why:** `settingsWindowFlags(CloseOnly)` is `Window | WindowTitleHint | WindowCloseButtonHint | CustomizeWindowHint`, with no `WindowMaximizeButtonHint`. Whether Qt's Cocoa style mask then keeps the window resizable is not pinned by any test. Review Focus 4 and Step 4's "Resize Settings" both assume it does.
    - **Proposed:** make it an explicit Step 4 item. If the window turns out fixed-size, the fix is in `settingsWindowFlags`' table row, not in QML.

13. **F13: `OpenConfiguration`'s target is invisible in the formatter (Task 3; optional).**
    - **Why:** `formatActionArguments` (`Actions.hpp:1039-1100`) has no arm for `OpenConfiguration`. Settings' key-binding pane (`SettingsController.cpp:1867`) therefore shows the Settings binding and the editor binding alike, as "OpenConfiguration".
    - **Proposed:** since §1.7 has this step reshape the struct anyway, add the arm (`, in_editor: true` for `Editor`). A pre-existing gap, not a regression.

14. **F14: the Application-scope docs (Task 3).**
    - **Why:** `runMenuBarAction`'s doc (`ContourGuiApp.hpp:558-563`) names "Quit, New Window" as the rows that need no window, and `runWithoutWindow`'s log comment (`ContourGuiApp.cpp:1878`) says Settings needs one. Both become false. The plan updates only `:572-576`.
    - **Proposed:** update all three. Also: "Open Configuration File" with no window still only logs. That is fine for 4a (R-f covers the window target only), but say so in the doc.

15. **F15: the `/simplify` base and the staging globs (Tasks 3–5).**
    - **Plan text:** "`git diff <the plan commit>..HEAD -- src scripts`"; Task 3's `git add … src/contour/window …`; Task 4's `git add src/contour/window src/contour/session src/contour/command src/contour/qml src/contour/test`.
    - **Why:** the 1b cleanup 97e661ac landed after the plan commit ebe374b1. It touches `src/` (`AppMenuBar*`, `TabMenus.*`, …), so the step's `/simplify` diff would review it too. The directory-wide `git add`s would also sweep any uncommitted work of a concurrent agent into a task's commit.
    - **Proposed:** base `/simplify` on Task 1's parent commit, or list the step's own commits. Stage the files each task lists, after a `git status` check.

16. **F16: host detection happens three times (Task 3; `/simplify` material).**
    - **Why:** once `_hostPlatform` exists, `ContourGuiApp.cpp:1758` and `:2319` still call `detectHostPlatform(processEnvironment())`.
    - **Proposed:** both read `_hostPlatform`. Behaviour does not change.
