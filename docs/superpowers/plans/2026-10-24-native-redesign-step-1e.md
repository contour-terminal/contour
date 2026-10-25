# Native Redesign — Step 1e (Chrome Snapshot Driver) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A test-only driver that loads the real `Main.qml` against mocks, runs six named scripts on
every `ChromeStyle` row, in both `DefaultsEdition`s, and writes one PNG per variant for reviewers. It
runs on the `[display]` route, and its only acceptance is a render free of QML diagnostics.

**Architecture:**
- **A pure plan.** One Qt-free header holds the script table, the variant matrix, the file names, the
  row-to-host table and the per-edition look. It is tested in `contour_test`. Adding a script, a row or
  an edition adds a table row.
- **A scene.** One Qt fixture header turns a variant into mocks and loads `Main.qml`:
  - sessions, a pane tree, a palette model and a settings controller;
  - a pinned application palette, and the row's chrome tokens.

  It is tested offscreen through `objectName` assertions, so every script is proven to build without a
  display.
- **A writer.** It encodes PNGs and appends to a manifest through the injected
  `core::platform::FileSystem`, and is tested offscreen against `InMemoryFileSystem`.
- **The driver.** A `[display][snapshot]` case shows each scene, grabs it and writes it.
  - The Quick Controls style is fixed once per process, so the Terminal row needs a second pass under
    ContourTui. `test_main.cpp` takes the style from `CONTOUR_TEST_CONTROLS_STYLE`.
  - A wrapper script runs both passes, and CI uploads the result.
- **Nothing ships.** Every new file lives in `src/contour/test/` or `scripts/`. The only production
  edits are two behaviour-preserving extractions (`isTabBarShown`, `chromeStyleName`) and three QML
  `objectName`s.

**Tech Stack:** C++23, Qt 6.11 / QML, Qt Quick Controls (Fusion, ContourTui), Catch2 v3, CMake preset
`appleclang-debug` on this machine, weston headless on Linux CI.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §2.5, plus the
sections it cites: §2.6 (testing strategy) and §2.1 (rows). Visual design record:
`docs/superpowers/specs/2026-10-23-native-redesign-design.md`. Canvas:
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>. Plans index:
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
  - dependency injection: files through `core::platform::FileSystem`, environment through
    `core::Environment`;
  - configuration at construction: scenes and mocks take their shape in the constructor;
  - `enum class` over `bool`, with the default enumerator at zero;
  - `std::expected` for fallible calls, and static factories for fallible setup;
  - data-driven tables, `[[nodiscard]]`, and Doxygen on every new public declaration;
  - `.hpp` headers with `#pragma once`;
  - zero warnings under `-Werror`, and no `NOLINT`.
- **Build:** `cmake --build out/appleclang-debug -j 3 --target <target>`, never more than 3 jobs.
  - This machine has no `cargo`. If the tree must be reconfigured, configure with
    `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already set in `out/appleclang-debug/CMakeCache.txt`).
- **Tests:** each task runs only its own filter. The full suite runs in CI on Linux, macOS and Windows.
- **Never edit `vendor/core-cpp`.**
- **Test-only (Phase 0 ruling).** The snapshot driver must not ship in the app.
  - No new file goes into `contour_core`, `contour_qml` or the `contour` executable.
  - Task 7 verifies this against the built binary.
- **Both editions (R14).** `TestApp` is `DefaultsEdition::Classic` (`GuiTestFixtures.hpp:660`). This
  step is where Native2026 GUI coverage arrives.
  - The driver renders both editions.
  - It renders every `ChromeStyle` row: `MacOS`, `Fluent`, `ContourLinux` and `Terminal`.
  - It never goes through `TestApp`. It builds `config::Config { .defaultsEdition = e }` itself.
- **No pixel comparison** (§2.5, §2.6: "it never decides acceptance").
  - No golden images are committed, so there is no tolerance and no baseline-update procedure.
  - Assertions are limited to these: no QML diagnostics, a non-null image of the expected size, and a
    successful write.
- **Determinism**, so that a reviewer comparing two runs sees only real differences:
  - Window size is fixed: `SnapshotWindowSize` = 1024 × 640 logical px.
  - Colour scheme is pinned per variant: `QGuiApplication::setPalette(platform::buildThemePalette(...))`,
    plus `QStyleHints::setColorScheme` on Qt ≥ 6.8. It is restored afterwards.
  - Fonts: the bundled JetBrains Mono is registered with `QFontDatabase` from `src/contour/res/fonts`.
    The platform UI font is used as production uses it, and its resolved family is recorded.
  - Platform: weston headless with `LIBGL_ALWAYS_SOFTWARE=1` (`scripts/run-display-tests.sh`).
  - The device pixel ratio and the Qt version are recorded in `manifest.tsv` beside the PNGs. Nothing
    asserts on them.
- **Matrix.** 6 scripts × 4 rows × 2 editions × 2 appearances (dark, light) = **96 PNGs**.
  - 72 come from the Fusion pass, and 24 from the ContourTui pass (the Terminal row).
- **Name.** Files are named `<script>.<row>.<edition>.<appearance>.png`, for example
  `palette.terminal.native2026.dark.png`.
  - `<row>` is `chromeStyleName`: `macos | fluent | linux | terminal`.
  - `<edition>` is the config token: `classic | native2026`.
- **Moves are verbatim.** Mocks moved out of `QmlComponents_test.cpp` and `MainWindowQml_test.cpp` keep
  their members and their `bool`-taking drivers. These mirror Qt property setters, the "parameter is the
  property" carve-out. Only additions named in this plan change them.
- After the code changes, run `/simplify`.

## Review Focus

1. **Palette leaking out of a scene.** The `[display]` route runs every display case in one process.
   If a scene leaves the forced dark palette behind, a later `DisplayRendering_test` case renders
   against the wrong chrome. Destroying a scene must restore `QGuiApplication::palette()`. Pinned in
   Task 3.
2. **The snapshot case under the ordinary offscreen ctest run.** `grabWindow()` cannot render
   offscreen, and an unguarded case hangs or fails the main suite. The case must SKIP unless
   `CONTOUR_TEST_DISPLAY=1`. Pinned in Task 5.
3. **A controls style that matches no row.** Say `CONTOUR_TEST_CONTROLS_STYLE=ContourTUI` is set,
   with the wrong case. That pass must fail loudly, not write zero PNGs and report green. Pinned in
   Task 2 (`rowsRenderableUnder` is empty) and Task 5 (`REQUIRE_FALSE(rows.empty())`).
4. **The second pass clobbering the first.** Both passes write into one directory. Opening the writer
   again must keep the manifest lines already written. Pinned in Task 4.
5. **Native2026 `single-tab` showing a tab strip.** The mock shows the strip by default, but the
   Native2026 strip is `Multiple` and hides it at one tab. A snapshot with the strip would misrepresent
   the edition to every reviewer. Pinned in Task 3.

Existing users are not exposed: no shipped behaviour changes. `isTabBarShown` and `chromeStyleName` are
extractions pinned by the existing `tab strip*` and `UiStyle*` cases (Task 2), and the `objectName`
additions are inert.

---

### Task 1: Shared QML test fixtures, and Main.qml under every chrome row

Move the mocks that the scene needs out of two anonymous namespaces into shared headers. Then teach
`installChromeStyle` to take a `ChromeStyle` row directly. The current overload resolves against
`HostPlatform::Other` (`QmlChromeStyle.hpp:43`), so it can never reach `MacOS` or `Fluent`.

**Files:**
- Create `src/contour/test/QmlMocks.hpp`, in namespace `contour::test`. Move these verbatim:
  - `MockSession` from `QmlComponents_test.cpp:294-485`;
  - `MockPaneProxy` from `:493-590`;
  - `StubContourTerminal` from `:592-620`.
- Create `src/contour/test/MockMainController.hpp`. Move `MockMainController` from
  `MainWindowQml_test.cpp:80-321`, and `loadMainWindow` from `:324-363`.
  - Delete `MainWindowQml_test.cpp`'s own `StubContourTerminal` (`:50-74`). The `QmlMocks.hpp` one is
    a superset of it.
- Create `src/contour/test/QmlIncubation.hpp`. Move `IncubationDriver` from
  `SettingsPageQml_test.cpp:70-78`.
- Modify `src/contour/test/QmlChromeStyle.hpp`: add `ChromeSetup` and an overload that takes it.
- Modify `src/contour/test/QmlComponents_test.cpp`, `MainWindowQml_test.cpp` and
  `SettingsPageQml_test.cpp`. Each drops the moved code and adds `using contour::test::…;`.
- Modify `src/contour/CMakeLists.txt:768`: list `test/QmlMocks.hpp` and `test/MockMainController.hpp`
  in `contour_gui_test`'s sources next to `test/MainWindowControllerMock.hpp`, so that AUTOMOC runs on
  them once. Do not list them in `contour_gui_test_heavy_sources`.
- Test: `src/contour/test/MainWindowQml_test.cpp`

**Interfaces:**
- Produces:
  - `struct contour::test::ChromeSetup { config::ChromeStyle row; QFont font; config::WindowControlStyle windowControls; };`
  - `inline void installChromeStyle(QQmlEngine& engine, ChromeSetup const& setup);`. The existing
    `installChromeStyle(QQmlEngine&, UiStyle, WindowControlStyle)` becomes a one-line delegate,
    `ChromeSetup { resolveChromeStyle(style, HostPlatform::Other), QGuiApplication::font(), windowControlStyle }`,
    so every existing caller sees today's tokens.
  - `[[nodiscard]] std::unique_ptr<QObject> loadMainWindow(QQmlEngine& engine, MockMainController& controller, QmlMessageCapture& warnings, ChromeSetup const& chrome);`
  - `[[nodiscard]] std::unique_ptr<QObject> loadMainWindow(QQmlEngine& engine, MockMainController& controller, QmlMessageCapture& warnings);`.
    This one delegates with the defaults above, so the existing call sites do not change.
  - `contour::test::MockSession` gains `explicit MockSession(QColor background = QColor(Qt::black));`.
    `backgroundColor` stays `CONSTANT`, because it is configuration.
  - `contour::test::MockPaneProxy`, `contour::test::StubContourTerminal` and
    `contour::test::IncubationDriver` keep the interfaces they have today.

- [ ] **Step 1: Write the failing test.** Add it to `MainWindowQml_test.cpp`.

```cpp
TEST_CASE("Main.qml loads under every chrome row (offscreen)", "[contour][gui][qml][mainwindow][chromerow]")
{
    using contour::config::ChromeStyle;
    using contour::config::HostPlatform;
    struct Row { ChromeStyle row; HostPlatform host; char const* name; };
    for (auto const& [row, host, name]: std::array {
             Row { ChromeStyle::MacOS, HostPlatform::MacOS, "macos" },
             Row { ChromeStyle::Fluent, HostPlatform::Windows, "fluent" },
             Row { ChromeStyle::ContourLinux, HostPlatform::Other, "linux" },
             Row { ChromeStyle::Terminal, HostPlatform::Other, "terminal" } })
    {
        INFO("row: " << name);
        QQmlEngine engine;
        contour::test::MockMainController controller;
        contour::test::QmlMessageCapture warnings;
        auto const chrome = contour::test::ChromeSetup {
            row, QGuiApplication::font(),
            contour::config::resolveWindowControlStyle(contour::config::WindowControlStyle::Auto, host) };
        auto root = contour::test::loadMainWindow(engine, controller, warnings, chrome);
        auto const* style = engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
        REQUIRE(style != nullptr);
        CHECK(style->property("chromeStyle").toString() == QString::fromLatin1(name));
        CHECK(root->findChild<QQuickItem*>(QStringLiteral("titleBar")) != nullptr);
    }
}
```

- [ ] **Step 2: Run it and see it fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: a compile failure. `contour::test::ChromeSetup` and `MockMainController.hpp` do not exist yet.

- [ ] **Step 3: Implement.** Move the code as listed above, and add `ChromeSetup` and both overloads.
  The `ChromeSetup` overload of `installChromeStyle` is the old body, with `setup.row`, `setup.font`
  and `setup.windowControls` in place of the computed values. The `MockSession` constructor initialises
  a new `QColor _background` member, and `backgroundColor()` returns it.

- [ ] **Step 4: Run the new case and every suite whose fixtures moved.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[mainwindow],[qml],[settings]"`

Expected: PASS, and the case count is unchanged apart from the new case. This is the Terminal (TUI)
row's rendering for this task: the `terminal` iteration loads `Main.qml` on the Terminal token row.

- [ ] **Step 5: Commit.**

```bash
git add src/contour/test src/contour/CMakeLists.txt
git commit -m "contour: share the QML test mocks, and load Main.qml under every chrome row"
```

---

### Task 2: The snapshot plan, as data (Qt-free)

**Files:**
- Create: `src/contour/test/ChromeSnapshotPlan.hpp`, in namespace `contour::test::snapshot`.
  Header-only, and with no Qt include.
- Create: `src/contour/test/ChromeSnapshotPlan_test.cpp` and `src/contour/config/TabBarMode_test.cpp`.
  Add both to `contour_test` (`src/contour/CMakeLists.txt:1112`).
- Modify: `src/contour/config/TabBarMode.hpp`: add `isTabBarShown`.
- Modify: `src/contour/window/WindowController.cpp:897-906`: `tabBarShouldShow()` returns
  `config::isTabBarShown(_tabBarVisibility, count())`.
- Modify: `src/contour/config/UiStyle.hpp`: add `chromeStyleName`.
- Modify: `src/contour/window/UiStyleProvider.cpp:188-198`: `chromeStyleName()` returns
  `QString::fromLatin1(config::chromeStyleName(_row))`.
- Test: the two new test files, plus a case in `src/contour/config/UiStyle_test.cpp`.

**Interfaces:**
- Produces, in `config/`:
  - `[[nodiscard]] constexpr bool isTabBarShown(TabBarVisibility visibility, int tabCount) noexcept;`.
    It returns `Always` → true, `Never` → false, and `Multiple` → `tabCount > 1`.
  - `[[nodiscard]] constexpr std::string_view chromeStyleName(ChromeStyle row) noexcept;`. It returns
    `"macos" | "fluent" | "linux" | "terminal"`.
- Produces, in `contour::test::snapshot`. Every enum is `: uint8_t`, with its first enumerator `= 0`.
  - `enum class SnapshotScript { SingleTab = 0, ThreeTabs, SplitThree, Palette, Settings, RemoteReconnecting };`
  - `enum class PaneShape { SingleLeaf = 0, SplitThree };`. `SplitThree` is `[ a | split(b, c) ]`, with
    `a` active.
  - `enum class ContentPage { Terminal = 0, Settings };`
  - `enum class ScriptAction { None = 0, OpenPalette };`
  - `enum class HostConnection { Local = 0, Reconnecting };`
  - `struct SnapshotScriptRow { SnapshotScript script; std::string_view name; int tabCount; PaneShape panes; ContentPage page; ScriptAction action; HostConnection connection; std::string_view hostLabel; };`
  - `inline constexpr std::array SnapshotScripts`, in enumerator order, with a `static_assert` on the
    order and the count, as `EditionDefaultsTable` does (`Config.hpp:1114-1120`):

    | script | name | tabs | panes | page | action | connection | host |
    |---|---|---|---|---|---|---|---|
    | SingleTab | `single-tab` | 1 | SingleLeaf | Terminal | None | Local | `""` |
    | ThreeTabs | `three-tabs` | 3 | SingleLeaf | Terminal | None | Local | `""` |
    | SplitThree | `split-three` | 1 | SplitThree | Terminal | None | Local | `""` |
    | Palette | `palette` | 1 | SingleLeaf | Terminal | OpenPalette | Local | `""` |
    | Settings | `settings` | 1 | SingleLeaf | Settings | None | Local | `""` |
    | RemoteReconnecting | `remote-reconnecting` | 1 | SingleLeaf | Terminal | None | Reconnecting | `"build-01"` |

  - `[[nodiscard]] constexpr SnapshotScriptRow const& scriptRow(SnapshotScript script) noexcept;`
  - `struct SnapshotRowHost { config::ChromeStyle row; config::HostPlatform host; command::ShortcutStyle shortcuts; };`
  - `inline constexpr std::array SnapshotRowHosts`, in `ChromeStyle` order:

    | row | host | shortcuts |
    |---|---|---|
    | MacOS | MacOS | MacGlyphs |
    | Fluent | Windows | Text |
    | ContourLinux | Other | Text |
    | Terminal | Other | Text |

  - `[[nodiscard]] constexpr SnapshotRowHost const& rowHost(config::ChromeStyle row) noexcept;`. The
    window controls are `resolveWindowControlStyle(WindowControlStyle::Auto, rowHost(row).host)`.
  - `inline constexpr std::array SnapshotEditions { DefaultsEdition::Classic, DefaultsEdition::Native2026 };`
  - `inline constexpr std::array SnapshotAppearances { vtbackend::ColorPreference::Dark, vtbackend::ColorPreference::Light };`,
    whose names are `"dark"` and `"light"`.
  - `struct SnapshotVariant { SnapshotScript script; config::ChromeStyle row; config::DefaultsEdition edition; vtbackend::ColorPreference appearance; bool operator==(SnapshotVariant const&) const = default; };`
  - `[[nodiscard]] std::vector<config::ChromeStyle> rowsRenderableUnder(std::string_view controlsStyle);`.
    It returns the rows whose `uiStyleTokens(row).quickControlsStyle == controlsStyle`, in enumerator
    order.
  - `[[nodiscard]] std::vector<SnapshotVariant> snapshotMatrix(std::span<config::ChromeStyle const> rows);`.
    The nesting is script, then row, then edition, then appearance, with the script outermost.
  - `[[nodiscard]] std::string snapshotFileName(SnapshotVariant const& variant);`. It produces
    `std::format("{}.{}.{}.{}.png", …)`, using `configEnumToken` for the edition.
  - `struct SnapshotLook { vtbackend::RGBColor terminalBackground; double dimUnfocused; config::TabBarVisibility tabBarVisibility; };`
  - `[[nodiscard]] SnapshotLook snapshotLook(config::DefaultsEdition edition, vtbackend::ColorPreference appearance);`.
    It is built from `config::editionDefaults(edition)` and
    `config::preferredColorPalette(config::defaultColors(edition), appearance)->defaultBackground`.
  - `inline constexpr std::string_view DefaultControlsStyle = "Fusion";`
  - `inline constexpr std::string_view ControlsStyleVariable = "CONTOUR_TEST_CONTROLS_STYLE";`
  - `inline constexpr std::string_view SnapshotDirectoryVariable = "CONTOUR_SNAPSHOT_DIR";`
  - `[[nodiscard]] std::string controlsStyleFor(core::Environment const& env);`. It returns the
    variable's value, or `DefaultControlsStyle` when the variable is unset or empty.
  - `[[nodiscard]] std::filesystem::path snapshotDirectory(core::Environment const& env, std::filesystem::path const& workingDirectory);`.
    It returns the variable's value, or `workingDirectory / "chrome-snapshots"`.
  - `struct SnapshotFacts { std::string platform; double devicePixelRatio; std::string chromeFontFamily; std::string controlsStyle; std::string qtVersion; };`
  - `inline constexpr std::string_view ManifestHeader = "file\tscript\trow\tedition\tappearance\tplatform\tdpr\tfont\tcontrols\tqt\n";`
  - `[[nodiscard]] std::string manifestLine(SnapshotVariant const& variant, SnapshotFacts const& facts);`.
    It returns one tab-separated line ending in `\n`, with `dpr` formatted `{:g}`.

- [ ] **Step 1: Write the failing tests.** These go in `ChromeSnapshotPlan_test.cpp`, all tagged
  `[snapshot]`, plus `TabBarMode_test.cpp` (`[tabbar]`).

```cpp
TEST_CASE("snapshot.scriptTable", "[snapshot]")
{
    using namespace contour::test::snapshot;
    CHECK(SnapshotScripts.size() == 6);
    CHECK(scriptRow(SnapshotScript::ThreeTabs).tabCount == 3);
    CHECK(scriptRow(SnapshotScript::RemoteReconnecting).connection == HostConnection::Reconnecting);
    CHECK(scriptRow(SnapshotScript::Settings).page == ContentPage::Settings);
}

TEST_CASE("snapshot.rowsRenderableUnder", "[snapshot]")
{
    using contour::config::ChromeStyle;
    using namespace contour::test::snapshot;
    CHECK(rowsRenderableUnder("Fusion")
          == std::vector { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux });
    CHECK(rowsRenderableUnder("ContourTui") == std::vector { ChromeStyle::Terminal });
    CHECK(rowsRenderableUnder("ContourTUI").empty()); // review focus 3: a typo renders nothing
}

TEST_CASE("snapshot.matrixAndNames", "[snapshot]")
{
    using contour::config::ChromeStyle;
    using namespace contour::test::snapshot;
    auto const all = std::array { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux, ChromeStyle::Terminal };
    auto const matrix = snapshotMatrix(all);
    CHECK(matrix.size() == 96);
    CHECK(snapshotFileName(matrix.front()) == "single-tab.macos.classic.dark.png");
    CHECK(snapshotFileName(matrix.back()) == "remote-reconnecting.terminal.native2026.light.png");
    CHECK(snapshotMatrix(rowsRenderableUnder("ContourTui")).size() == 24);
}

TEST_CASE("snapshot.lookPerEdition", "[snapshot]")
{
    using contour::config::DefaultsEdition;
    using contour::config::TabBarVisibility;
    using vtbackend::ColorPreference;
    using namespace contour::test::snapshot;
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Dark).terminalBackground == 0x1C1E24_rgb);
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Light).terminalBackground == 0xFAFAF8_rgb);
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Dark).dimUnfocused == 0.15);
    CHECK(snapshotLook(DefaultsEdition::Native2026, ColorPreference::Dark).tabBarVisibility == TabBarVisibility::Multiple);
    // Classic names one scheme, so both appearances share its background.
    CHECK(snapshotLook(DefaultsEdition::Classic, ColorPreference::Dark).terminalBackground
          == snapshotLook(DefaultsEdition::Classic, ColorPreference::Light).terminalBackground);
    CHECK(snapshotLook(DefaultsEdition::Classic, ColorPreference::Dark).tabBarVisibility == TabBarVisibility::Always);
}

TEST_CASE("snapshot.environment", "[snapshot]")
{
    using namespace contour::test::snapshot;
    auto const none = core::testing::FakeEnvironment {};
    CHECK(controlsStyleFor(none) == "Fusion");
    CHECK(snapshotDirectory(none, "/work") == std::filesystem::path("/work/chrome-snapshots"));
    auto const set = core::testing::FakeEnvironment { { { "CONTOUR_TEST_CONTROLS_STYLE", "ContourTui" },
                                                        { "CONTOUR_SNAPSHOT_DIR", "/tmp/shots" } } };
    CHECK(controlsStyleFor(set) == "ContourTui");
    CHECK(snapshotDirectory(set, "/work") == std::filesystem::path("/tmp/shots"));
}

TEST_CASE("snapshot.manifestLine", "[snapshot]")
{
    using namespace contour::test::snapshot;
    auto const v = SnapshotVariant { SnapshotScript::Palette, contour::config::ChromeStyle::Terminal,
                                     contour::config::DefaultsEdition::Native2026, vtbackend::ColorPreference::Dark };
    CHECK(manifestLine(v, { "wayland", 1.0, "JetBrains Mono", "ContourTui", "6.11.1" })
          == "palette.terminal.native2026.dark.png\tpalette\tterminal\tnative2026\tdark\twayland\t1\tJetBrains Mono\tContourTui\t6.11.1\n");
}
```

  `TabBarMode_test.cpp`: `TEST_CASE("TabBarMode.isTabBarShown", "[tabbar]")` checks
  `Always`/`Never`/`Multiple` against tab counts 1 and 2: `true,true / false,false / false,true`.
  `UiStyle_test.cpp`: `TEST_CASE("UiStyle.chromeStyleName", "[config]")` checks all four names.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test`

Expected: a compile failure. `ChromeSnapshotPlan.hpp`, `isTabBarShown` and `chromeStyleName` do not
exist yet.

- [ ] **Step 3: Implement** the declarations above. `snapshotMatrix` uses nested range-for over the
  four tables, never index loops. `rowsRenderableUnder` filters
  `std::views::iota(0, ChromeStyleCount)` with `std::views::filter`.

- [ ] **Step 4: Run the new cases, and the cases pinning the two extractions.**

Run:

```bash
out/appleclang-debug/src/contour/contour_test "[snapshot],[tabbar],UiStyle*"
cmake --build out/appleclang-debug -j 3 --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "tab strip*,[uistyle]"
```

Expected: PASS.

- [ ] **Step 5: Commit.**

```bash
git add src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/config src/contour/window src/contour/CMakeLists.txt
git commit -m "contour: describe the chrome snapshots as a table of scripts, rows and editions"
```

---

### Task 3: The scene, which builds every script offscreen

**Files:**
- Create: `src/contour/test/ChromeSnapshotScene.hpp`, header-only, in `contour::test::snapshot`.
- Create: `src/contour/test/ChromeSnapshot_test.cpp`. Add it to `contour_gui_test_heavy_sources`
  (`src/contour/CMakeLists.txt:830`). It includes `SettingsController.hpp` and `Config.hpp`, as the
  listed `SettingsPageQml_test.cpp` does.
- Modify `src/contour/test/MockMainController.hpp`: add `MainWindowScene`, as described under
  Interfaces.
- Modify `src/contour/qml/Main.qml`:
  - add `objectName: "settingsPageLoader"` to the settings `Loader` (`:217`);
  - add `objectName: "commandPalette"` to `CommandPalette` (`:289`).
- Modify `src/contour/qml/TabStrip.qml:89`: add `objectName: "tabList"` to the `ListView`.

**Interfaces:**
- Consumes: Task 1 (`MockSession(QColor)`, `MockPaneProxy`, `StubContourTerminal`,
  `IncubationDriver`, `ChromeSetup`, `loadMainWindow(…, ChromeSetup const&)`), and Task 2 (the whole
  `snapshot` namespace, and `config::isTabBarShown`).
- Also consumes these existing functions:
  - `window::resolveChromeFont(config::Config const&, config::ChromeStyle, std::string const&)`
    (`UiStyleProvider.hpp:381`);
  - `platform::buildThemePalette(Qt::ColorScheme)` (`platform/GuiTheme.hpp`);
  - `platform::toQColor(vtbackend::RGBColor, std::uint8_t)` (`platform/ColorConversion.hpp:21`);
  - `command::shortcutIndex(config::Config const&, ShortcutStyle)` (`command/Shortcut.hpp:153`);
  - `window::CommandPaletteModel`, `command::CommandHistory` and `command::ActionCommandSource`, wired
    as `MockPaletteController` wires them (`QmlComponents_test.cpp:3785-3793`);
  - `window::SettingsController(ConfigAccessor, std::shared_ptr<GuiConfigStore>, ApplyCallback, std::shared_ptr<IGpuInventory const>)`
    (`SettingsController.hpp:122`), over a `config::FileGuiConfigStore` in a `QTemporaryDir`.
- Produces:
  - `struct contour::test::MainWindowScene { int tabCount = 2; QObject* rootPane = nullptr; QObject* commandPalette = nullptr; QObject* settingsController = nullptr; snapshot::ContentPage page = snapshot::ContentPage::Terminal; };`.
    The pointers are borrowed.
  - `explicit MockMainController(MainWindowScene scene = {});`. The defaults reproduce today's mock:
    two tabs, and a null pane, palette and settings controller.
    - `rowCount` and the `data()` bounds read `scene.tabCount`.
    - `activeTabRootPane()` and `commandPalette()` return the scene's pointers.
    - It adds `Q_PROPERTY(bool settingsActive READ settingsActive CONSTANT)`, which is
      `page == ContentPage::Settings`. This is a QML-facing mirror of `WindowController.hpp:108`.
    - It adds `Q_PROPERTY(QObject* settingsController READ settingsController CONSTANT)`.
    - It adds `Q_INVOKABLE void closeSettings() {}`.
  - `inline constexpr QSize SnapshotWindowSize { 1024, 640 };`
  - `enum class SnapshotFontError : uint8_t { DirectoryMissing = 1, NoFontLoaded };`
  - `[[nodiscard]] std::expected<QStringList, SnapshotFontError> registerBundledChromeFonts(std::filesystem::path const& directory);`.
    It calls `QFontDatabase::addApplicationFont` on every `*.ttf` in the directory, and returns the
    families it loaded. The default directory is
    `std::filesystem::path(CONTOUR_GUI_SOURCE_DIR) / "res" / "fonts"`. `CONTOUR_GUI_SOURCE_DIR` is a
    PUBLIC definition of `contour_core` (`src/contour/CMakeLists.txt:619`).
  - `class ScopedChromeAppearance`:
    - `explicit ScopedChromeAppearance(vtbackend::ColorPreference appearance);` saves
      `QGuiApplication::palette()`, sets `buildThemePalette(Dark|Light)`, and on Qt ≥ 6.8 calls
      `styleHints()->setColorScheme`.
    - The destructor restores the palette and calls `unsetColorScheme()`. The suite never pins a
      scheme, as `test_main.cpp` shows.
    - It is neither copyable nor movable.
  - `class ChromeSnapshotScene`:
    - `explicit ChromeSnapshotScene(SnapshotVariant variant);` builds everything and loads `Main.qml`.
      It `REQUIRE`s a clean load, as `DisplayHarness` does.
    - `void show();` resizes the window to `SnapshotWindowSize` and shows it. It then runs the script's
      `ScriptAction`: for `OpenPalette` it calls `controller.openCommandPalette()` and
      `REQUIRE(QTest::qWaitFor([&] { return palette->property("opened").toBool(); }, 2000))`. Finally it
      calls `incubation.settle()` and processes events twice.
    - `[[nodiscard]] QImage grab() const;` returns `window().grabWindow()`, and needs a display.
    - `[[nodiscard]] QQuickWindow& window() const;`
    - `[[nodiscard]] QObject& root() const;`
    - `[[nodiscard]] QStringList diagnostics() const;` returns the captured messages that satisfy
      `isQmlDiagnostic`.
    - `[[nodiscard]] QFont const& chromeFont() const noexcept;`
- How a variant becomes a scene. These are the decisions; the body is the implementer's.
  - **Config.** `config::Config _config { .defaultsEdition = variant.edition }`. The look is
    `snapshotLook(variant.edition, variant.appearance)`.
  - **Chrome.** `ChromeSetup { variant.row, resolveChromeFont(_config, variant.row, "main"), resolveWindowControlStyle(Auto, rowHost(variant.row).host) }`.
  - **Sessions.** One per leaf, each `MockSession(toQColor(look.terminalBackground))`.
    - Inactive leaves get `setDimUnfocused(look.dimUnfocused)`.
    - Under `HostConnection::Reconnecting` every session gets `setReconnecting(true)`.
  - **Pane tree.** `SingleLeaf` is one active leaf. `SplitThree` is
    `MockPaneProxy(false, a, MockPaneProxy(false, b, c))`, with `a` active.
  - **Palette model.** History capacity 5. The shortcuts are
    `shortcutIndex(_config, rowHost(variant.row).shortcuts)`, so the macOS row shows glyphs.
  - **Settings.** Built only for `ContentPage::Settings`, with a no-op apply callback and a null GPU
    inventory.
  - **Controller.** `MockMainController(MainWindowScene { … })`.
    - `attachedHost = hostLabel`.
    - `setTabBarShouldShow(config::isTabBarShown(look.tabBarVisibility, tabCount))`.
  - **Engine.** It gets `IncubationDriver`, and `qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal")`
    is called before the load. Every other suite re-registers its own stub before each load, as
    `QmlComponents_test.cpp:2853` and following do.
  - **Member order.** `ScopedChromeAppearance` is declared first and the root object last, so the tree
    dies before the mocks and the palette is restored after everything.

- [ ] **Step 1: Write the failing tests** in `ChromeSnapshot_test.cpp`. They are offscreen and tagged
  `[contour][gui][qml][snapshot]`.

```cpp
TEST_CASE("Every snapshot script builds on the macOS and Terminal rows (offscreen)", "[contour][gui][qml][snapshot]")
{
    using namespace contour::test::snapshot;
    for (auto const& variant: snapshotMatrix(std::array { contour::config::ChromeStyle::MacOS, contour::config::ChromeStyle::Terminal }))
    {
        if (variant.appearance != vtbackend::ColorPreference::Dark)
            continue; // one appearance is enough to prove the structure
        INFO(snapshotFileName(variant));
        auto scene = ChromeSnapshotScene { variant };
        scene.show();
        CHECK(scene.diagnostics().isEmpty());
        auto const& row = scriptRow(variant.script);
        auto const look = snapshotLook(variant.edition, variant.appearance);
        auto* titleBar = scene.root().findChild<QQuickItem*>(QStringLiteral("titleBar"));
        REQUIRE(titleBar != nullptr);
        // Review focus 5: Native2026 hides a one-tab strip, Classic does not.
        CHECK(titleBar->isVisible() == contour::config::isTabBarShown(look.tabBarVisibility, row.tabCount));
        switch (variant.script)
        {
            case SnapshotScript::ThreeTabs:
                CHECK(scene.root().findChild<QObject*>(QStringLiteral("tabList"))->property("count").toInt() == 3);
                break;
            case SnapshotScript::SplitThree:
                CHECK(scene.root().findChildren<contour::test::StubContourTerminal*>().size() == 3);
                break;
            case SnapshotScript::Palette:
                CHECK(scene.root().findChild<QObject*>(QStringLiteral("commandPalette"))->property("opened").toBool());
                break;
            case SnapshotScript::Settings:
                CHECK(scene.root().findChild<QObject*>(QStringLiteral("settingsPageLoader"))->property("item").value<QObject*>() != nullptr);
                break;
            case SnapshotScript::RemoteReconnecting:
                CHECK(scene.root().findChild<QQuickItem*>(QStringLiteral("reconnectingOverlay"))->isVisible());
                break;
            case SnapshotScript::SingleTab: break;
        }
    }
}

TEST_CASE("A snapshot scene gives the application palette back (offscreen)", "[contour][gui][qml][snapshot]")
{
    using namespace contour::test::snapshot;
    auto const before = QGuiApplication::palette();
    {
        auto scene = ChromeSnapshotScene { { SnapshotScript::SingleTab, contour::config::ChromeStyle::ContourLinux,
                                             contour::config::DefaultsEdition::Native2026, vtbackend::ColorPreference::Light } };
        scene.show();
        CHECK(QGuiApplication::palette().color(QPalette::Window) == QColor(0xef, 0xef, 0xef)); // buildThemePalette(Light)
    }
    CHECK(QGuiApplication::palette() == before); // review focus 1
}

TEST_CASE("The bundled chrome fonts register with Qt, and a missing directory says so (offscreen)", "[contour][gui][snapshot]")
{
    using namespace contour::test::snapshot;
    auto const families = registerBundledChromeFonts(std::filesystem::path(CONTOUR_GUI_SOURCE_DIR) / "res" / "fonts");
    REQUIRE(families.has_value());
    CHECK(families->contains(QStringLiteral("JetBrains Mono")));
    CHECK(registerBundledChromeFonts("/nonexistent/fonts").error() == SnapshotFontError::DirectoryMissing);
}
```

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: a compile failure. `ChromeSnapshotScene.hpp` and `MainWindowScene` do not exist yet.

- [ ] **Step 3: Implement** the declarations and decisions above, and add the three `objectName`s.

- [ ] **Step 4: Run the new cases, plus the suites whose QML or mock changed.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[snapshot],[mainwindow],[palette]"`

Expected: PASS. This is the Terminal (TUI) row's rendering for this task: every script builds on the
Terminal token row. The Fusion controls stay, because the process pinned them; Task 5 renders it
under ContourTui.

- [ ] **Step 5: Commit.**

```bash
git add src/contour/test src/contour/qml/Main.qml src/contour/qml/TabStrip.qml src/contour/CMakeLists.txt
git commit -m "contour: build each chrome snapshot script as a Main.qml scene over mocks"
```

---

### Task 4: The snapshot writer (PNG and manifest, through the file system)

**Files:**
- Create: `src/contour/test/ChromeSnapshotWriter.hpp`, header-only, in `contour::test::snapshot`.
- Test: `src/contour/test/ChromeSnapshot_test.cpp`

**Interfaces:**
- Consumes: Task 2 (`SnapshotVariant`, `SnapshotFacts`, `snapshotFileName`, `manifestLine`,
  `ManifestHeader`), and `core::platform::FileSystem` (`vendor/core-cpp/src/core/platform/FileSystem.hpp`:
  `createDirectories`, `isDirectory`, `exists`, `writeFile`, `appendFile`).
- Produces:
  - `enum class SnapshotWriteError : uint8_t { DirectoryUnavailable = 1, EncodeFailed, WriteFailed };`
  - `inline constexpr std::string_view ManifestFileName = "manifest.tsv";`
  - `class ChromeSnapshotWriter`:
    - `[[nodiscard]] static std::expected<ChromeSnapshotWriter, SnapshotWriteError> open(core::platform::FileSystem const& files, std::filesystem::path directory);`
      calls `createDirectories`, then fails with `DirectoryUnavailable` unless `isDirectory`. It writes
      `ManifestHeader` only when the manifest does not exist yet.
    - `[[nodiscard]] std::expected<std::filesystem::path, SnapshotWriteError> write(SnapshotVariant const& variant, QImage const& image, SnapshotFacts const& facts) const;`
      encodes with `QImage::save(&buffer, "PNG")` into a `QBuffer`. A null image or a failed save is
      `EncodeFailed`, and appends nothing. A failed `writeFile` or `appendFile` is `WriteFailed`.
      On success it returns the PNG's path.
    - `[[nodiscard]] std::filesystem::path const& directory() const noexcept;`
    - The constructor is private, and the members are `FileSystem const*` and `path`.

- [ ] **Step 1: Write the failing tests.** They are offscreen, tagged `[contour][gui][snapshot]`, and
  run against `core::platform::testing::InMemoryFileSystem`.

```cpp
TEST_CASE("The snapshot writer writes a PNG and its manifest line", "[contour][gui][snapshot]")
{
    using namespace contour::test::snapshot;
    auto files = core::platform::testing::InMemoryFileSystem {};
    auto const variant = SnapshotVariant { SnapshotScript::Palette, contour::config::ChromeStyle::Terminal,
                                           contour::config::DefaultsEdition::Native2026, vtbackend::ColorPreference::Dark };
    auto const facts = SnapshotFacts { "offscreen", 1.0, "JetBrains Mono", "Fusion", "6.11.1" };
    auto image = QImage(QSize(4, 4), QImage::Format_ARGB32);
    image.fill(Qt::red);

    auto writer = ChromeSnapshotWriter::open(files, "/out");
    REQUIRE(writer.has_value());
    auto const written = writer->write(variant, image, facts);
    REQUIRE(written.has_value());
    CHECK(*written == std::filesystem::path("/out/palette.terminal.native2026.dark.png"));
    CHECK(files.readFile(*written).value().starts_with("\x89PNG"));
    CHECK(files.readFile("/out/manifest.tsv").value() == std::string(ManifestHeader) + manifestLine(variant, facts));

    // Review focus 4: the second pass opens the same directory and keeps the first pass's lines.
    auto again = ChromeSnapshotWriter::open(files, "/out");
    REQUIRE(again.has_value());
    CHECK(files.readFile("/out/manifest.tsv").value() == std::string(ManifestHeader) + manifestLine(variant, facts));

    CHECK(writer->write(variant, QImage {}, facts).error() == SnapshotWriteError::EncodeFailed);
    CHECK(files.readFile("/out/manifest.tsv").value() == std::string(ManifestHeader) + manifestLine(variant, facts));
}

TEST_CASE("The snapshot writer refuses a directory it cannot use", "[contour][gui][snapshot]")
{
    using namespace contour::test::snapshot;
    auto files = core::platform::testing::InMemoryFileSystem { core::platform::testing::FileEntry {
        .path = "/out", .content = "not a directory" } };
    CHECK(ChromeSnapshotWriter::open(files, "/out").error() == SnapshotWriteError::DirectoryUnavailable);
}
```

- [ ] **Step 2: Run them and see them fail.** Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`.
  Expected: a compile failure, because `ChromeSnapshotWriter.hpp` does not exist.
- [ ] **Step 3: Implement** the declarations above.
- [ ] **Step 4: Run.** Run: `out/appleclang-debug/src/contour/contour_gui_test "[snapshot]"`. Expected: PASS.
- [ ] **Step 5: Commit.**

```bash
git add src/contour/test/ChromeSnapshotWriter.hpp src/contour/test/ChromeSnapshot_test.cpp
git commit -m "contour: write chrome snapshots and their manifest through the file system"
```

---

### Task 5: The `[display]` driver, with the controls style chosen per process

**Files:**
- Modify: `src/contour/test/test_main.cpp:121`.
  - Replace `QQuickStyle::setStyle(QStringLiteral("Fusion"))` with:
    - `QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"))`;
    - then `QQuickStyle::setStyle(QString::fromStdString(snapshot::controlsStyleFor(core::LiveEnvironment {})))`.

    This is the production pair (`ContourGuiApp.cpp:2136-2137`).
  - Extend the comment above it to say what the variable is for.
- Modify `src/contour/test/ChromeSnapshot_test.cpp`: add the driver case.
- Modify `src/contour/test/DisplayHarness.hpp`: move `displayTestsEnabled()` there from
  `DisplayRendering_test.cpp:86-89` as `[[nodiscard]] inline bool contour::test::displayTestsEnabled()`.
  `DisplayRendering_test.cpp` and the new case both use it.

**Interfaces:**
- Consumes: Tasks 2 to 4 (`rowsRenderableUnder`, `snapshotMatrix`, `snapshotDirectory`,
  `ChromeSnapshotScene`, `registerBundledChromeFonts`, `ChromeSnapshotWriter`).
- Produces: the case `"Chrome snapshots: every script on every row this process draws"`, tagged
  `[display][snapshot]`, which writes `<CONTOUR_SNAPSHOT_DIR>/*.png` and `manifest.tsv`.

- [ ] **Step 1: Write the failing test.**

```cpp
TEST_CASE("Chrome snapshots: every script on every row this process draws", "[display][snapshot]")
{
    using namespace contour::test::snapshot;
    if (!contour::test::displayTestsEnabled())
        SKIP("renders through a real compositor: run under scripts/run-chrome-snapshots.sh"); // review focus 2

    auto const controls = QQuickStyle::name().toStdString();
    auto const rows = rowsRenderableUnder(controls);
    INFO("Quick Controls style: " << controls << " (set " << ControlsStyleVariable << ")");
    REQUIRE_FALSE(rows.empty()); // review focus 3
    REQUIRE(registerBundledChromeFonts(std::filesystem::path(CONTOUR_GUI_SOURCE_DIR) / "res" / "fonts").has_value());

    auto const environment = core::LiveEnvironment {};
    auto writer = ChromeSnapshotWriter::open(core::platform::NativeFileSystem::instance(),
                                             snapshotDirectory(environment, std::filesystem::current_path()));
    REQUIRE(writer.has_value());

    for (auto const& variant: snapshotMatrix(rows))
    {
        INFO(snapshotFileName(variant));
        auto scene = ChromeSnapshotScene { variant };
        scene.show();
        REQUIRE(QTest::qWaitForWindowExposed(&scene.window(), 3000));
        auto const image = scene.grab();
        CHECK(scene.diagnostics().isEmpty());
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == SnapshotWindowSize * scene.window().devicePixelRatio());
        auto const facts = SnapshotFacts { QGuiApplication::platformName().toStdString(),
                                           scene.window().devicePixelRatio(),
                                           QFontInfo(scene.chromeFont()).family().toStdString(),
                                           controls, qVersion() };
        CHECK(writer->write(variant, image, facts).has_value());
    }
}
```

- [ ] **Step 2: Run it offscreen and see it SKIP.** Then run it on this machine's display, before
  `test_main.cpp` changes, and see the Terminal pass fail.

Run:

```bash
cmake --build out/appleclang-debug -j 3 --target contour_gui_test
out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
```

Expected:
- The first run reports the case as skipped, with the SKIP text.
- The second fails on `REQUIRE_FALSE(rows.empty())`, because the style is still pinned to Fusion.

- [ ] **Step 3: Implement** the `test_main.cpp` change and the `displayTestsEnabled()` move.

- [ ] **Step 4: Run both passes on this machine's display.** It uses the Cocoa platform here, and its
  windows flash. CI runs it under weston.

Run:

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l; tail -n +2 "$CONTOUR_SNAPSHOT_DIR/manifest.tsv" | wc -l
out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][style],[mainwindow]"
```

Expected:
- Both passes PASS.
- Both counts print `96`.
- The style and Main.qml suites still PASS under the default Fusion.
- Open one `*.terminal.*.png` and one `*.macos.*.png` to confirm that they differ. This is the
  Terminal (TUI) row's rendering for this task, now under ContourTui controls.

- [ ] **Step 5: Commit.**

```bash
git add src/contour/test src/contour/display/DisplayRendering_test.cpp
git commit -m "contour: render the chrome snapshots on the display route, one pass per controls style"
```

---

### Task 6: One command for both passes, and the CI artefact

**Files:**
- Create: `scripts/run-chrome-snapshots.sh`, which is executable.
  - Usage: `run-chrome-snapshots.sh <contour_gui_test> <output-dir>`.
  - It exports `CONTOUR_SNAPSHOT_DIR=<output-dir>` and empties that directory.
  - It runs `scripts/run-display-tests.sh --run <binary> "[display][snapshot]"` twice: once plain,
    once with `CONTOUR_TEST_CONTROLS_STYLE=ContourTui`.
  - It then fails unless the PNG count equals the number of manifest lines after the header and is
    non-zero. The count is never hard-coded, because the table owns it.
  - `--run` is required. Without it the wrapper prepends `[display]` as a separate argument, and
    Catch2 ORs the two, which would run every display case under ContourTui.
  - The header comment states what the script produces, and says that the PNGs are review artefacts
    and never acceptance (§2.5).
- Modify: `.github/workflows/build.yml`. Add two steps in job `ubuntu_2404_cc_matrix`, after
  "display-gated GUI tests (headless weston)" (`:1561-1565`).
  - Both carry `if: ${{ matrix.compiler == 'GCC 15' && matrix.cxx == '23' && matrix.runner == 'ubuntu-24.04' }}`,
    the condition "Upload unit tests" uses. The upload step has `always() &&` in front.
  - `"chrome snapshots (headless weston)"` has `continue-on-error: true`, because the display route
    is still non-gating (`:1557`). It runs `./scripts/run-chrome-snapshots.sh out/gcc-debug/src/contour/contour_gui_test chrome-snapshots`.
  - `"Upload chrome snapshots"` uses `actions/upload-artifact@v4` with `name: chrome-snapshots`,
    `path: chrome-snapshots/`, `if-no-files-found: warn` and `retention-days: 14`.

- [ ] **Step 1: Run the gates that cover these files and see the current state.**

Run: `ctest --test-dir out/appleclang-debug -L lint --output-on-failure`

Expected: PASS before the change. This is the baseline that `check_workflows` and `check_spelling`
must keep.

- [ ] **Step 2: Write the script and the workflow steps.**

- [ ] **Step 3: Check them.**

Run:

```bash
bash -n scripts/run-chrome-snapshots.sh
command -v shellcheck >/dev/null && shellcheck scripts/run-chrome-snapshots.sh
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
scripts/run-chrome-snapshots.sh; echo "exit=$?"
```

Expected:
- `bash -n` is silent.
- shellcheck, where it is installed, is clean.
- The lint gates PASS.
- The call with no arguments prints its usage, and exits with a non-zero status.
- Running it end to end needs weston and `dbus-run-session`, which this machine lacks. The CI step
  proves it, so record it on the board as "verified in CI" once the run is green.

- [ ] **Step 4: Commit.**

```bash
git add scripts/run-chrome-snapshots.sh .github/workflows/build.yml
git commit -m "ci: render the chrome snapshots under weston and upload them for review"
```

---

### Task 7: Finish

- [ ] **Step 1: Confirm that nothing ships.**

Run:

```bash
cmake --build out/appleclang-debug -j 3 --target contour
nm -C out/appleclang-debug/src/contour/contour.app/Contents/MacOS/contour | grep -c "snapshot::" || true
git grep -n "ChromeSnapshot" -- src ':!src/contour/test' ':!src/contour/CMakeLists.txt'
```

Expected:
- The `nm` count is `0`.
- `git grep` matches nothing.
- `CMakeLists.txt` lists the new files only under `contour_gui_test`, `contour_gui_test_heavy` and
  `contour_test`.

- [ ] **Step 2:** Run `/simplify` on the branch diff, and apply its findings. Re-run the filters of
  every task it touches.
- [ ] **Step 3:** Run every task's filter once more:
  `contour_test "[snapshot],[tabbar],UiStyle*"` and
  `contour_gui_test "[snapshot],[mainwindow],[qml],[settings],[palette],tab strip*,[uistyle]"`.
  Expected: PASS.
- [ ] **Step 4:** No release note. The change is test-only and invisible to users. Update the PR body
  with `/update-pr`, saying:
  - **Performance:** no runtime impact.
  - **Risk:** low. Two extractions, and three `objectName`s.
  - **Coverage:** the new offscreen cases.
- [ ] **Step 5:** Note for Step 4a, on the board and in the PR. When 4a turns Settings into a window
  of its own, it updates the `settings` row of `SnapshotScripts` in the same commit. Later steps add
  their scripts (HostChip, Banner, Toast, …) as rows of the same table.
