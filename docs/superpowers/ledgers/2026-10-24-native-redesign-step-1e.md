# SDD ledger — plan: docs/superpowers/plans/2026-10-24-native-redesign-step-1e.md

Spec: `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §2.5 (binds), with
§2.1 and §2.6. Scanned against HEAD 05fa207a on `feature/native-redesign-phase0`. Rulings R14 (TestApp is
Classic) and R19 (full matrix incl. light/dark; snapshot step non-gating) are taken as settled.

## Pre-flight scan

### 1. Cross-task pairs (shared file or interface)

| Tasks | Earlier task produces | Later task consumes | Finding |
|---|---|---|---|
| 1 → 2 | `src/contour/CMakeLists.txt:768` (gui sources: `QmlMocks.hpp`, `MockMainController.hpp`); `[chromerow]` case asserting `chromeStyle` names per row | `CMakeLists.txt:1112` (`contour_test`: two new tests); `config::chromeStyleName` extraction behind `UiStyleProvider::chromeStyleName()` | CMake: consistent (separate blocks). Pinning: **conflict F11**: Task 1's `[chromerow]` case is the only test that pins the provider's names, and Task 2 Step 4 doesn't run it. |
| 1 → 3 | `MockMainController.hpp`, `loadMainWindow(…, ChromeSetup const&)`, `ChromeSetup`, `MockSession(QColor)`, `MockPaneProxy`, `StubContourTerminal`, `IncubationDriver` | Task 3 edits `MockMainController.hpp` (adds `MainWindowScene` and its constructor) and builds the scene from these | Signatures match: `MockPaneProxy(bool, MockPaneProxy*, MockPaneProxy*)`, `setDimUnfocused(float)`, `setReconnecting(bool)` and `attachedHost` (on `MainWindowControllerMock`) all exist. **Conflict F3**: `loadMainWindow` moves into a header with no `inline`. Once `MainWindowQml_test.cpp` (contour_gui_test) and `ChromeSnapshot_test.cpp` (contour_gui_test_heavy, linked into the same executable) both include it, the link fails with duplicate symbols. **F7**: the scene configures the mock after construction. |
| 1 → 5 | `MockMainController.hpp` / `loadMainWindow` (through Task 3's scene) | The `[display][snapshot]` case in `ChromeSnapshot_test.cpp` | Same as F3. Otherwise consistent. |
| 1 → 7 | Filters `[mainwindow],[qml],[settings]` | Task 7 Step 3 union | Consistent: `[qml]` also covers `[chromerow]`. |
| 2 → 3 | `contour::test::snapshot` (`SnapshotVariant`, `scriptRow`, `rowHost`, `snapshotLook`, `ContentPage`, `HostConnection`), `config::isTabBarShown(TabBarVisibility, int)` | Scene decisions; `MainWindowScene::page` is a `snapshot::ContentPage`, so `MockMainController.hpp` includes `ChromeSnapshotPlan.hpp` | Types and names match. **F9**: Task 2 says every enum in the namespace starts at `= 0`, but Tasks 3 and 4 add error enums at `= 1`. **F14**: `SnapshotLook::dimUnfocused` is a `double` and `MockSession::setDimUnfocused` takes a `float` (bugprone-narrowing-conversions). |
| 2 → 4 | `SnapshotVariant`, `SnapshotFacts` field order {platform, dpr, font, controls, qt}, `snapshotFileName`, `manifestLine`, `ManifestHeader` | `ChromeSnapshotWriter::write`/`open`, and its tests | Consistent: both tests build `SnapshotFacts` in the same order, and "header only when absent" matches the append-only manifest. |
| 2 → 5 | `controlsStyleFor(Environment const&)`, `ControlsStyleVariable`, `rowsRenderableUnder`, `snapshotMatrix`, `snapshotDirectory(env, cwd)` | `test_main.cpp` style selection; the driver case | **Conflict F3**: the plan header is "header-only", yet its non-constexpr functions (`rowsRenderableUnder`, `snapshotMatrix`, `snapshotFileName`, `snapshotLook`, `controlsStyleFor`, `snapshotDirectory`, `manifestLine`) carry no `inline`. `test_main.cpp` and `ChromeSnapshot_test.cpp` both include it and link into `contour_gui_test`, which gives an ODR/duplicate-symbol failure (GCC also raises `-Wmissing-declarations`). **F4**: the `snapshotDirectory` cwd fallback makes every plain `[display]` run write PNGs into cwd. |
| 2 → 6 | Variable names `CONTOUR_SNAPSHOT_DIR`, `CONTOUR_TEST_CONTROLS_STYLE`, token `"ContourTui"` (`UiStyle.hpp:373`); count owned by the table | Wrapper script exports and count check | Consistent. The count isn't hard-coded. |
| 2 → 7 | Filters `[snapshot],[tabbar],UiStyle*` | Task 7 Step 3 | Consistent. |
| 3 → 4 | Creates `ChromeSnapshot_test.cpp` (heavy sources, `:830`) | Appends writer cases to it | Consistent: Task 4 needs no CMake edit and commits only its own two files. F9 applies to `SnapshotWriteError` as well. |
| 3 → 5 | `ChromeSnapshotScene` (`show`, `grab`, `window`, `diagnostics`, `chromeFont`), `SnapshotWindowSize`, `registerBundledChromeFonts`, `ScopedChromeAppearance` | The driver case | Every member the driver calls is declared in Task 3, so consistent. **F8**: `registerBundledChromeFonts` reads the real filesystem by path, which breaks the plan's own DI constraint. |
| 3 → 7 | Filters `[snapshot],[mainwindow],[palette]` | Task 7 Step 3 | Consistent. |
| 4 → 5 | `ChromeSnapshotWriter::open(FileSystem const&, path)`, `write(...)` | `open(NativeFileSystem::instance(), …)` | Consistent: `NativeFileSystem::instance()` exists (`NativeFileSystem.hpp:39`). |
| 4 → 6 | Manifest invariant: one line per written PNG, header written once, kept on reopen | The wrapper fails unless PNG count == manifest lines after the header, and that count is > 0 | Consistent. Review focus 4 holds. |
| 5 → 6 | Case `[display][snapshot]`; it SKIPs unless `CONTOUR_TEST_DISPLAY=1`; the style comes from the env through `test_main` | The wrapper calls `run-display-tests.sh --run <bin> "[display][snapshot]"` twice | `--run` exists and does what the plan says. **Conflict F4**: the existing CI step "display-gated GUI tests (headless weston)" (`build.yml:1561-1565`) runs every `[display]` case. It therefore also runs the new snapshot case under Fusion: 72 extra software-GL renders, and PNGs written into `$PWD/chrome-snapshots`. The same happens on every local `scripts/run-display-tests.sh`. |
| 5 → 7 | Task 5 Step 4 also runs `[contour][gui][style]` | Task 7 Step 3 union | Minor: Task 7 leaves out `[contour][gui][style]`, the suite that `test_main`'s style change affects. Add it. |

### 2. Per-task self-consistency

| Task | Agrees with itself? | Detail |
|---|---|---|
| 1 | Mostly | Tests match the code. Files created match files touched and committed. Interfaces match steps, except that `loadMainWindow` in a header lacks `inline` (F3). Line references are off by a few lines (F12). |
| 2 | No | The `[snapshot]` test uses `0x1C1E24_rgb` without `using vtbackend::operator""_rgb;` (F6). Step 3's `std::views::iota(0, ChromeStyleCount)` doesn't compile: the name is `config::detail::ChromeStyleCount`, a `size_t`, and the iota deduction guide rejects mixed signedness (F5). "Header-only" contradicts non-`inline` function declarations (F3). Step 4 claims the extraction is pinned by existing cases, but no existing case pins it (F11). Files are consistent (`TabBarMode_test.cpp` goes in `config/`, built by `contour_test`). |
| 3 | Partly | The Global Constraint "mocks take their shape in the constructor" is contradicted by `attachedHost = hostLabel` (a `string_view` assigned to a `QString` member) and `setTabBarShouldShow(...)` after construction (F7). DI constraint: `registerBundledChromeFonts(path)` (F8). Enum convention (F9). Narrowing in `setDimUnfocused` (F14). The "Engine" bullet re-registers `StubContourTerminal`, which `loadMainWindow` already does: harmless, but redundant. Tests match the code. Every `objectName` the tests query is either added here or already exists (`titleBar`, `reconnectingOverlay`). Files match the commit. |
| 4 | No | The test "refuses a directory it cannot use" fails against `InMemoryFileSystem`. `createDirectories("/out")` over a file inserts `/out` into `_directories` without error (`InMemoryFileSystem.cpp:525-537`), so `isDirectory` then returns true and `open` succeeds (F1). Files match the commit. |
| 5 | No | Step 2's expected red can't happen. (a) Step 1 calls `contour::test::displayTestsEnabled()`, which Step 3 creates, so Step 2 is a compile error. (b) Before the `test_main` change the style is Fusion whatever the env says, so `rowsRenderableUnder("Fusion")` returns 3 rows and `REQUIRE_FALSE(rows.empty())` passes (F2). Files: `DisplayRendering_test.cpp` is edited (and committed) but not listed under Files. `ContourTuiStyle_test.cpp:23-28` goes stale (F13). `test_main.cpp:121` is really `:115` (F12). |
| 6 | Yes | The script, the `--run` reasoning, the count check, the `if:` copied from "Upload unit tests", and `continue-on-error` (R19) all agree. `:1557` is a comment line; `continue-on-error` is at `:1559`/`:1564` (F12). Missing: the editorconfig check for the new script (F10). |
| 7 | Yes | The `contour.app/Contents/MacOS/contour` path exists in `out/appleclang-debug`, and the filters cover every earlier task except `[contour][gui][style]` (row 5 → 7 above). |

### Plan references verified against HEAD

Present and correct:
- `GuiTestFixtures.hpp:660` (Classic)
- `MainWindowQml_test.cpp:80` (mock) and `:324-361` (`loadMainWindow`)
- `QmlComponents_test.cpp:294-485` (`MockSession`) and `:2853`
- `SettingsPageQml_test.cpp:70-78`
- `CMakeLists.txt:619/768/830/1112`
- `WindowController.cpp:897-906`, `WindowController.hpp:108`
- `UiStyleProvider.cpp:188-198`, `UiStyleProvider.hpp:381`
- `Shortcut.hpp:153`, `ColorConversion.hpp:21`, `SettingsController.hpp:122`, `Config.hpp:1114-1120`
- `ContourGuiApp.cpp:2136-2137`, `DisplayRendering_test.cpp:86-89`
- `Main.qml:289`, `TabStrip.qml:89`
- `build.yml:1561-1565`
- `platform::buildThemePalette` (Light Window = `#efefef`)
- `config::preferredColorPalette` and `defaultColors`
- `core::LiveEnvironment`, `core::testing::FakeEnvironment` (explicit map constructor; `T{…}` works)
- `core::platform::testing::InMemoryFileSystem` and `FileEntry{.path,.content}`
- the `run-display-tests.sh --run` flag
- res/fonts JetBrains Mono
- the scheme backgrounds `0x1C1E24` / `0xFAFAF8`

Off by a few lines (F12):
- `test_main.cpp:121` → 115
- `QmlChromeStyle.hpp:43` → 44
- `MockPaneProxy :493-590` → 493-582
- `StubContourTerminal :592-620` → 592-617
- `Main.qml:217` → the Loader opens at 216
- `build.yml:1557` → 1559

No API the plan names is missing.

## Findings needing a ruling

1. **F1: the writer's "unusable directory" test can't pass on `InMemoryFileSystem` (Task 4).**
   - Plan text: "`open` … calls `createDirectories`, then fails with `DirectoryUnavailable` unless `isDirectory`". The test seeds `FileEntry { .path = "/out", .content = "not a directory" }`.
   - Why it fails: the fake's `createDirectories` turns a file path into a directory without complaint.
   - Proposed: before creating anything, `open` returns `DirectoryUnavailable` when `files.exists(dir) && !files.isDirectory(dir)`. It also maps a `createDirectories` error to `DirectoryUnavailable` rather than dropping a `[[nodiscard]]` result. The precedent is `OutputFileError::DirectoryUnavailable` ("or a link is in its place").
   - Grounding: §2.5 needs "a successful write" to mean something.

2. **F2: Task 5 Step 2's red can't happen.**
   - Plan text: "The second fails on `REQUIRE_FALSE(rows.empty())`, because the style is still pinned to Fusion."
   - Why: with Fusion pinned, `rows` holds three rows. Step 1's `displayTestsEnabled()` also doesn't exist until Step 3.
   - Proposed:
     - Move `displayTestsEnabled()` in Step 1.
     - Add `REQUIRE(controls == controlsStyleFor(core::LiveEnvironment {}))` ahead of the rows check. That is the real red before the change: the requested style isn't the one in effect. It also covers review focus 3 directly.

3. **F3: header-only functions without `inline` (Tasks 1, 2, 3; breaks at Task 3/5 link).**
   - Plan text:
     - Task 2: "Header-only", followed by `[[nodiscard]] std::vector<…> snapshotMatrix(…);` and the others.
     - Task 1: `[[nodiscard]] std::unique_ptr<QObject> loadMainWindow(…)`.
     - Task 3: `registerBundledChromeFonts(…)`.
   - Proposed: declare each of them `inline`, as `QmlChromeStyle.hpp`'s `installChromeStyle` already is. `test_main.cpp`, `MainWindowQml_test.cpp` and `ChromeSnapshot_test.cpp` all link into `contour_gui_test`.

4. **F4: the plain `[display]` route picks up the snapshot case (Task 5 vs Task 6 and the existing CI step).**
   - Plan text: the case is tagged `[display][snapshot]`, and `snapshotDirectory` "returns … `workingDirectory / "chrome-snapshots"`".
   - Why it matters: `build.yml:1565` already runs `run-display-tests.sh <bin>`, which selects `[display]`. That run renders 72 Fusion snapshots into the workspace before the dedicated step empties the directory and renders them again. Every local display run leaves `chrome-snapshots/` in cwd.
   - Proposed:
     - Keep the `[display]` tag (§2.5 binds).
     - Make `snapshotDirectory` return `std::optional<std::filesystem::path>`, empty when `CONTOUR_SNAPSHOT_DIR` is unset, and drop the cwd fallback.
     - The case SKIPs without it. The wrapper and Task 5 Step 4 already set it.
     - Adjust the `snapshot.environment` test to match.

5. **F5: `iota(0, ChromeStyleCount)` doesn't compile (Task 2 Step 3).**
   - Proposed: `std::views::iota(size_t { 0 }, config::detail::ChromeStyleCount)`, the spelling `Config.hpp:1114` uses.

6. **F6: the `_rgb` literal is not in scope in `snapshot.lookPerEdition` (Task 2 test).**
   - Proposed: add `using vtbackend::operator""_rgb;` at file scope, as `GuiTheme_test.cpp:21` does.

7. **F7: the scene configures the mock after construction (Task 3).**
   - Plan text: "`attachedHost = hostLabel`" and "`setTabBarShouldShow(config::isTabBarShown(look.tabBarVisibility, tabCount))`".
   - Why: this contradicts the plan's Global Constraint ("mocks take their shape in the constructor") and AGENT.md.
   - Proposed:
     - `MainWindowScene` gains `QString hostLabel` and `config::TabBarVisibility tabBarVisibility = Always`.
     - The `MockMainController(MainWindowScene)` constructor sets `attachedHost`, and seeds `_tabBarShouldShow` from `isTabBarShown(scene.tabBarVisibility, scene.tabCount)`.
     - The existing setters stay for the tests that flip them as state.

8. **F8: two ambient reads break the plan's DI constraint (Tasks 3 and 5).**
   - Plan text: Global Constraints say "files through `core::platform::FileSystem`, environment through `core::Environment`". But `registerBundledChromeFonts(std::filesystem::path const&)` reads the disk directly, and `displayTestsEnabled()` reads `qgetenv`.
   - Proposed:
     - `registerBundledChromeFonts(FileSystem const&, path)` uses `listDirectory`, `readFile` and `QFontDatabase::addApplicationFontFromData`. The `DirectoryMissing` case then runs on `InMemoryFileSystem`.
     - `displayTestsEnabled(core::Environment const&)`.

9. **F9: error enums start at 1 (Tasks 3 and 4).**
   - Plan text: `SnapshotFontError … { DirectoryMissing = 1, … }` and `SnapshotWriteError … { DirectoryUnavailable = 1, … }`. Task 2 says "Every enum is `: uint8_t`, with its first enumerator `= 0`".
   - The tree's error enums all start at 0: `OutputFileError`, `ShellIntegrationError`, `FileOpenerError`, `CommandPaneError`.
   - Proposed: `= 0`.

10. **F10: no clang-format 22 or editorconfig step (all code tasks and Task 6).**
    - Why: the plan never formats, and the code blocks exceed the column limit. Commit 715fd1a7 already had to fix CI's clang-format 22 and editorconfig gates on this branch.
    - Proposed: before each commit, run `/opt/homebrew/opt/llvm@22/bin/clang-format -i` on every touched `.cpp`/`.hpp`. Task 6's script follows `.editorconfig`.

11. **F11: the `chromeStyleName` extraction isn't pinned by the filter Task 2 runs.**
    - Plan text: "extractions pinned by the existing `tab strip*` and `UiStyle*` cases (Task 2)".
    - Why: no existing test reads the provider's `chromeStyle` property. Only Task 1's new `[chromerow]` case does.
    - Proposed: Task 2 Step 4's gui filter becomes `"tab strip*,[uistyle],[chromerow]"`, and the Review Focus sentence is corrected to match.

12. **F12: stale line references.** The list is above. Proposed: correct them in the plan text.

13. **F13: a comment goes stale after Task 5.** `ContourTuiStyle_test.cpp:23-28` says the binary "has already pinned Fusion in test_main.cpp" and that the style is "untestable by selecting it". Proposed: Task 5 updates that comment to name `CONTOUR_TEST_CONTROLS_STYLE`.

14. **F14: double→float narrowing (Task 3).** `setDimUnfocused(look.dimUnfocused)` passes a `double` (`EditionDefaults::dimUnfocused`) to a `float` parameter. `bugprone-narrowing-conversions` is on (`bugprone-*`). Proposed: an explicit `static_cast<float>` at the scene.

15. **F15 (interpretation, low): extra assertions beyond the spec's "only assertion".**
    - §2.5 says "Its only assertion is that each script renders without QML warnings". The plan adds a non-null image, the expected size, and a successful write, plus Task 3's offscreen structural checks.
    - Proposed: keep them. They check that the harness works, not acceptance, and §2.6 makes property tests the acceptance route. State this reading in the plan's "No pixel comparison" bullet so a reviewer doesn't read it as drift from §2.5.

## Rulings
- Ruling S1: all fifteen pre-flight findings F1–F15 are accepted with the resolution proposed above; each task's dispatch points its implementer at the findings for that task, which override the plan text — the spec (§2.5) is unaffected — cost if wrong: small rework in the task concerned.
- Ruling S2 (= phase-0 R19): full matrix incl. light/dark; snapshot step non-gating; with F4, snapshots render only when CONTOUR_SNAPSHOT_DIR is set.
- Task 1: BASE 05fa207a; implementer dispatched (sonnet), findings F3/F10/F12 carried.
- Task 1: implementer DONE bb1e394d ([chromerow] 1, [mainwindow] 10, [qml] 121, [settings] 103). Review dispatched.
- Task 1: review Spec ✅ Quality Approved (nit parked: windowControls mapping per row not asserted).
- Task 1: complete (bb1e394d).
- Task 2: BASE 3635823a.
- Task 2: implementer dispatched (sonnet; F3/F5/F6/F10/F11/F12).
- Task 2: implementer DONE a7a4160f (contour_test 15 cases; gui 25 cases). F4 applied early: snapshotDirectory(env) -> optional<path>. Review dispatched.
- Task 2: review Spec ✅ Quality Approved (nit parked: rowHosts test skips ContourLinux and non-Mac `shortcuts`).
- Task 2: complete (a7a4160f).
- Task 3: BASE a7a4160f.
- Task 3: implementer dispatched (opus; F3/F7/F8/F9/F10/F12/F14/F15).
- Task 3: implementer DONE 2c3cf05e (37 cases/645 assertions; [qml] 123; lint 8/8). Concern: clang-tidy pro-type-member-init on Task 2's SnapshotLook (dimUnfocused, tabBarVisibility) — to fix in the next fix round. Review dispatched.
- Task 3: review Spec ✅ Quality Approved (Low: unchecked findChild derefs; mock includes snapshot plan for ContentPage — parked). Fix round 1 dispatched: REQUIRE non-null + SnapshotLook member init (Task 2's clang-tidy finding).
- Task 3: fix round 1 committed 74729c45 (REQUIRE non-null; SnapshotLook defaults = Classic). Tests pass; clang-tidy 22 clean on both test files. Re-review inline by controller (small, mechanical). Approved.
- Task 3: complete (2c3cf05e, 74729c45).
- Task 4: BASE 74729c45.
- Task 4: implementer dispatched (sonnet; F1/F3/F9/F10/F12).
- Task 4: implementer DONE b1fd88e3 ([snapshot] 5 cases/238). Review dispatched.
- Task 4: review inline by controller (9 KB diff): F1 honoured (file-in-place refused before createDirectories), F9 enum from 0, all I/O via FileSystem, manifest preserved across passes, tests cover success/second pass/null image/file-in-place. Approved.
- Task 4: complete (b1fd88e3).
- Task 5: BASE b1fd88e3.
- Task 5: implementer dispatched (opus; F2/F4/F8/F10/F12/F13).
- Task 5: implementer DONE 9a3b36ac (96 PNGs: 72 Fusion + 24 ContourTui; [snapshot] 5+1 skip; [qml] 123; [display] 2 pre-existing failures sizeIncrement/latency probe, identical at base). Controller viewed two PNGs: real renders of today's chrome, rows distinct. Review dispatched.
- Task 5: review Spec ✅ Quality Approved (nit parked: include grouping in ChromeSnapshot_test.cpp:35-36).
- Task 5: complete (9a3b36ac).
- Task 6: BASE 9a3b36ac.
- Task 6: implemented inline by controller (script + two workflow steps; small, fully specified). 378a3fab. bash -n, shellcheck clean, usage exits 64, lint 8/8. End-to-end run needs weston: verified in CI once green. Self-review: matches brief (--run, count from table, non-gating, always() upload, 14-day retention).
- Task 6: complete (378a3fab).
- Task 7: Step 1 verified — nm finds 0 snapshot:: symbols in contour.app; ChromeSnapshot* only in test targets (CMakeLists 844, 1127). /simplify dispatched (opus).
- Task 7: /simplify committed 034368b1 (mock decoupled; TestEnvironment.hpp; command::shortcutStyleFor(HostPlatform); [chromerow] loops the row table; parked nits done). contour_test 15/15, gui 236+1 skip, lint 8/8. Task 7 Step 3 filters covered by this run. Final whole-step review next.
- Final review (opus): Ready: Yes. Minors fixed in c0d823f9 (both passes run, 20-min timeout, Windows branch in platformShortcutStyle); shellcheck clean. Unverified until CI: bundled-font registration on offscreen Linux/Windows.
- Task 7: complete. Step 1e complete (bb1e394d..c0d823f9), pushed.
- Note for Step 4a: when Settings becomes its own window, update the `settings` row of SnapshotScripts in the same commit; later steps add their scripts as rows of the same table.
