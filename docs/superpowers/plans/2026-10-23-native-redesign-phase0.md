# Native Redesign — Phase 0 (Foundations) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lay the groundwork every later phase draws on, and change nothing visible in the chrome's
geometry. This means:
- per-platform UiStyle rows;
- chrome light/dark that follows the terminal;
- shortcut glyphs on macOS;
- the contour-dark, contour-light and contour-classic schemes;
- no focus frame on a single pane;
- no hard-coded colours in the QML;
- the new first-launch defaults, including a bundled JetBrains Mono.

**Architecture:**
- **Token rows.** Everything stays data-driven. `UiStyle` remains the user's choice. A new
  `ChromeStyle` row id (MacOS, Fluent, ContourLinux, Terminal) is resolved once from
  `UiStyle × HostPlatform` and indexes the token table.
- **Pure decisions** (colour scheme from background, shortcut text, contrast) live in headless
  headers with Catch2 tests.
- **QML** reads only provider properties.
- **Size, type and motion tokens are not added here.** Phase 1 adds each one together with its first consumer, into the rows this phase creates.

**Tech Stack:** C++23, Qt 6.11 / QML, Catch2, CMake presets (`appleclang-debug` on macOS,
`clang-asan` on Linux).

**Spec:** `docs/superpowers/specs/2026-10-23-native-redesign-design.md` (canvas:
https://claude.ai/artifact/6cp7tcSnurTSMbhSDePWqH)

## Global Constraints

- Follow AGENT.md:
  - `enum class` instead of `bool` in APIs;
  - `std::expected` for fallible calls;
  - data-driven tables;
  - configuration at construction;
  - `[[nodiscard]]`;
  - Doxygen on new public API;
  - `.hpp` headers;
  - no `NOLINT`;
  - zero warnings under `-Werror`.
- Never edit `vendor/core-cpp`.
- **Colour and default values** are exactly those in the spec's tables.
- **Visible chrome geometry** must not change in this phase. `MainWindowQml_test` geometry assertions
  stay green.
- **Existing users:** a generated `contour.yml` pins old values. New defaults only change `Config{}`.
  Never edit `DefaultColorPalette` or the `contour` scheme in place: RIS / XTPOPCOLORS depend on them.
- After the code changes, run `/simplify`.
- Run the suite with `ctest --preset appleclang-debug` (on Linux, `ctest --preset clang-asan`).
- Work on a branch from `master`. The uncommitted ⌘ binding prototype is **not** part of this plan:
  see #2175.

## Review Focus

1. **A user `colors:` section that names `default`** must keep today's palette after `contour-dark`
   becomes the default. Pinned in Task 4.
2. **`theme: terminal` with a dual (light/dark) colour config** must follow the OS, not force one
   scheme. A forced scheme would flip-flop the chrome against the grid. Pinned in Task 3.
3. **Shortcuts with Hyper or Meta on macOS** have no glyph and must still render readably as
   `Hyper+…`, never as an empty prefix. Pinned in Task 2.
4. **A missing bundled font file** (a distro package that strips resources) must fall back to the
   platform default silently, never fail font loading. Pinned in Task 7.
5. **The QML colour lint** must not flag colours in comments, or in `"#00000000"`-style transparent
   literals that stay in the allowlist. A false positive blocks every contributor. Pinned in Task 6.

---

### Task 0: Branch

- [ ] **Step 1: Park the prototype and branch from master**

```bash
git stash push -m "macos-command-key-bindings prototype (see #2175)" -- src/contour/config/Config.cpp src/contour/config/Config.hpp src/contour/config/Config_test.cpp
git switch master && git pull --ff-only && git switch -c feature/native-redesign-phase0
git add docs/superpowers && git commit -m "docs: native redesign design record and phase 0 plan"
```

Expected: `git status` is clean on the new branch.

---

### Task 1: ChromeStyle rows resolved from UiStyle × HostPlatform

**Files:**
- Modify: `src/contour/config/UiStyle.hpp`: add `ChromeStyle`, re-index the token table, add `resolveChromeStyle`.
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}`: the constructor takes `ChromeStyle`.
- Modify: `src/contour/ContourGuiApp.cpp:1494`: resolve once, then use the row for `QQuickStyle` and the provider.
- Test: `src/contour/config/UiStyle_test.cpp`

**Interfaces:**
- Produces:
  - `enum class ChromeStyle : uint8_t { MacOS = 0, Fluent, ContourLinux, Terminal };`
  - `[[nodiscard]] constexpr ChromeStyle resolveChromeStyle(UiStyle style, HostPlatform platform) noexcept;`
  - `[[nodiscard]] constexpr UiStyleTokens const& uiStyleTokens(ChromeStyle row) noexcept;`, which replaces the `UiStyle` overload.
  - `UiStyleProvider(config::ChromeStyle row, QFont const& chromeFont, QObject* parent = nullptr);`
  - `Q_PROPERTY(QString chromeStyle READ chromeStyleName CONSTANT)`, returning `"macos" | "fluent" | "linux" | "terminal"`.

- [ ] **Step 1: Write the failing tests** in `UiStyle_test.cpp`

```cpp
TEST_CASE("UiStyle.resolveChromeStyle", "[config]")
{
    using enum contour::config::ChromeStyle;
    using contour::config::HostPlatform;
    using contour::config::UiStyle;
    CHECK(resolveChromeStyle(UiStyle::Native, HostPlatform::MacOS) == MacOS);
    CHECK(resolveChromeStyle(UiStyle::Native, HostPlatform::Windows) == Fluent);
    CHECK(resolveChromeStyle(UiStyle::Native, HostPlatform::KdePlasma) == ContourLinux);
    CHECK(resolveChromeStyle(UiStyle::Native, HostPlatform::Other) == ContourLinux);
    for (auto p: { HostPlatform::MacOS, HostPlatform::Windows, HostPlatform::KdePlasma, HostPlatform::Other })
        CHECK(resolveChromeStyle(UiStyle::Terminal, p) == Terminal);
}

TEST_CASE("UiStyle.nativeRowsKeepTodaysGeometry", "[config]")
{
    // Phase 0 must not move a pixel: every native row starts as a copy of the old Native row.
    for (auto row: { ChromeStyle::MacOS, ChromeStyle::Fluent, ChromeStyle::ContourLinux })
    {
        auto const& t = uiStyleTokens(row);
        CHECK(t.chromeHeightUnits == 34);
        CHECK(t.tabHeightUnits == 32);
        CHECK(t.cornerRadiusPixels == 3);
        CHECK(t.quickControlsStyle == "Fusion");
    }
    CHECK(uiStyleTokens(ChromeStyle::Terminal).quickControlsStyle == "ContourTui");
}
```

- [ ] **Step 2: Run them and see them fail**

Run: `cmake --build --preset appleclang-debug --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "UiStyle.*"`

Expected: a compile failure, because `ChromeStyle` is undeclared.

- [ ] **Step 3: Implement**
  - The table holds 4 rows in `ChromeStyle` order. MacOS, Fluent and ContourLinux are copies of
    today's Native row; Terminal is today's Terminal row.
  - `static_assert` that the row count equals the `ChromeStyle` count.
  - `UiStyle` keeps its 2-entry config table (`native`, `terminal`), so the YAML schema does not change.
  - `resolveChromeFont` and every other `uiStyleTokens(config.uiStyle)` caller resolve through
    `resolveChromeStyle(config.uiStyle.value(), detectHostPlatform(env))`.
  - `ContourGuiApp` resolves the row once at startup, stores it in a `config::ChromeStyle _chromeStyle`
    member, and passes it everywhere.

- [ ] **Step 4: Run the tests and the existing QML geometry tests**

Run: `out/appleclang-debug/src/contour/contour_gui_test "UiStyle.*,MainWindow*,[chrome]"`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git commit -am "contour: resolve the chrome's token row from ui_style and the host platform"
```

---

### Task 2: Shortcut text per platform (⌃⌥⇧⌘ glyphs on macOS)

**Files:**
- Modify: `src/contour/command/Shortcut.{hpp,cpp}`
- Modify: callers of `shortcutText` / `modifiersText`: `Shortcut.cpp:48,122,127`, plus any QML-facing model that renders a chord (grep `shortcutText`).
- Test: `src/contour/command/Shortcut_test.cpp`

**Interfaces:**
- Produces:
  - `enum class ShortcutStyle : uint8_t { Text = 0, MacGlyphs };`
  - `[[nodiscard]] constexpr ShortcutStyle platformShortcutStyle() noexcept;`, which is `MacGlyphs` under `__APPLE__` and `Text` otherwise.
  - `[[nodiscard]] std::string modifiersText(vtbackend::Modifiers modifiers, ShortcutStyle style);`
  - `[[nodiscard]] std::string shortcutText(vtbackend::Modifiers modifiers, ShortcutInput input, ShortcutStyle style);`
- Data:
  - `ShortcutModifierRow` gains `std::string_view glyph`: Control `⌃`, Alt `⌥`, Shift `⇧`, Super `⌘`; Hyper and Meta get an empty glyph.
  - A new `ShortcutKeyGlyphTable` maps Enter `↩`, Escape `⎋`, Tab `⇥`, Backspace `⌫`, Delete `⌦`, the arrows `← ↑ → ↓`, PageUp `⇞`, PageDown `⇟`, Home `↖` and End `↘`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("Shortcut.macGlyphs", "[command]")
{
    using namespace contour::command;
    using vtbackend::Modifier;
    auto const cmdShift = vtbackend::Modifiers { Modifier::Super, Modifier::Shift };
    CHECK(shortcutText(cmdShift, ShortcutInput { U'P' }, ShortcutStyle::MacGlyphs) == "⇧⌘P");
    CHECK(shortcutText(vtbackend::Modifiers { Modifier::Control }, ShortcutInput { vtbackend::Key::Enter },
                       ShortcutStyle::MacGlyphs) == "⌃↩");
    CHECK(shortcutText(cmdShift, ShortcutInput { U'P' }, ShortcutStyle::Text) == "Shift+Super+P");
    // Review focus 3: a modifier without a glyph falls back to its name, never vanishes.
    CHECK(shortcutText(vtbackend::Modifiers { Modifier::Hyper }, ShortcutInput { U'K' }, ShortcutStyle::MacGlyphs)
          == "Hyper+K");
}
```

Glyph order follows Apple: ⌃⌥⇧⌘. That is the table's row order for those four, so the macOS
rendering walks the same table.

- [ ] **Step 2: Run and see it fail** (it does not compile yet). Run: `out/appleclang-debug/src/contour/contour_gui_test "Shortcut.*"`
- [ ] **Step 3: Implement.** Glyph rows join without separators. A glyph-less modifier renders as
  `Name+`. Every existing caller passes `platformShortcutStyle()`.
- [ ] **Step 4: Run** `out/appleclang-debug/src/contour/contour_gui_test "Shortcut.*,CommandPalette*"`. Expected: PASS. Update any palette test that asserted `Ctrl+…`, so it is platform-conditional.
- [ ] **Step 5: Commit:** `git commit -am "contour: render shortcuts with ⌃⌥⇧⌘ glyphs on macOS"`

---

### Task 3: GUI theme `terminal`, where the chrome follows the terminal background

**Files:**
- Modify: `src/contour/config/Config.hpp:127`: add `GuiTheme::Terminal`, and add `"terminal"` to the formatter at `:3158` and the reader. Change the default at `:1527` to `Terminal`.
- Modify: `src/contour/config/ConfigDocumentation.hpp`: document `terminal`.
- Modify: `src/contour/platform/GuiTheme.hpp`: add the pure decision.
- Modify: `src/contour/ContourGuiApp.cpp:1091` `applyGuiTheme`: pass the default profile's colour config.
- Test: `src/contour/platform/GuiTheme_test.cpp`. Create it if it does not exist, and add it to the `contour_gui_test` sources next to the other `platform/*_test.cpp`.

**Interfaces:**
- Produces:
  - `[[nodiscard]] double relativeLuminance(vtbackend::RGBColor color) noexcept;` in `src/vtbackend/core/Color.hpp` (WCAG 2.x; test `#000000` → 0.0 and `#FFFFFF` → 1.0).
  - `[[nodiscard]] constexpr Qt::ColorScheme colorSchemeOfBackground(vtbackend::RGBColor background) noexcept;`, which returns `Dark` when `relativeLuminance` is below 0.18, and `Light` otherwise.
  - `[[nodiscard]] constexpr std::optional<Qt::ColorScheme> qtColorSchemeFor(config::GuiTheme theme, config::ColorConfig const& colors) noexcept;`, which replaces the one-argument form.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("GuiTheme.terminalFollowsBackground", "[platform]")
{
    using contour::config::GuiTheme;
    auto simple = [](vtbackend::RGBColor bg) {
        auto c = contour::config::SimpleColorConfig {};
        c.colors.defaultBackground = bg;
        return contour::config::ColorConfig { c };
    };
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, simple(0x1C1E24_rgb)) == Qt::ColorScheme::Dark);
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, simple(0xFAFAF8_rgb)) == Qt::ColorScheme::Light);
    // Review focus 2: a dual config already follows the OS; the chrome must follow it too.
    CHECK(qtColorSchemeFor(GuiTheme::Terminal, contour::config::ColorConfig { contour::config::DualColorConfig {} })
          == std::nullopt);
    CHECK(qtColorSchemeFor(GuiTheme::Dark, simple(0xFAFAF8_rgb)) == Qt::ColorScheme::Dark);
    CHECK(qtColorSchemeFor(GuiTheme::System, simple(0x000000_rgb)) == std::nullopt);
    CHECK(Config {}.theme.value() == GuiTheme::Terminal);
}
```

- [ ] **Step 2: Run and see it fail.** Run: `out/appleclang-debug/src/contour/contour_gui_test "GuiTheme.*"`
- [ ] **Step 3: Implement.**
  - `applyGuiTheme(config::GuiTheme theme, config::ColorConfig const& colors)`.
  - Both call sites (`ContourGuiApp.cpp:1476` and the reload path at `:1156`) pass `_config.profile(profileName())->colors.value()`.
  - The Settings theme combo lists `terminal` first, labelled "Match terminal".
- [ ] **Step 4: Run** `out/appleclang-debug/src/contour/contour_gui_test "GuiTheme.*,Config*,Settings*"`. Expected: PASS.
- [ ] **Step 5: Commit:** `git commit -am "contour: let the chrome's light or dark follow the terminal background"`

---

### Task 4: Built-in schemes contour-dark, contour-light and contour-classic

**Files:**
- Modify: `src/vtbackend/core/ColorPalette.cpp:53` (`BuiltinColorPalettes`):
  - Add rows `contour-dark` and `contour-light` with the spec's values. Each row sets `palette[0..15]`, `defaultForeground`, `defaultBackground`, `cursor` and `selection`.
  - Add `contour-classic`, an alias row identical to `contour`.
- Test: `src/vtbackend/core/ColorPalette_test.cpp`

**Interfaces:**
- Produces: the names `"contour-dark"`, `"contour-light"` and `"contour-classic"`, resolvable through the existing `defaultColorPalettes()` / `defaultColorPaletteNames()`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ColorPalette.contourSchemesAreReadable", "[vtbackend]")
{
    for (auto const name: { "contour-dark", "contour-light" })
    {
        auto const p = vtbackend::defaultColorPalettes(name);
        REQUIRE(p.has_value());
        CHECK(contrastRatio(p->defaultForeground, p->defaultBackground) >= 4.5);
        for (auto const i: { 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14 })
            CHECK(contrastRatio(p->palette[i], p->defaultBackground) >= 4.5);
    }
    CHECK(vtbackend::defaultColorPalettes("contour-dark")->defaultBackground == 0x1C1E24_rgb);
    CHECK(vtbackend::defaultColorPalettes("contour-light")->palette[4] == 0x1F5FBF_rgb);
}

TEST_CASE("ColorPalette.classicIsTodaysPalette", "[vtbackend]")
{
    // Review focus 1: the old palette survives under both names.
    CHECK(vtbackend::defaultColorPalettes("contour-classic")->palette == vtbackend::ColorPalette {}.palette);
    CHECK(vtbackend::defaultColorPalettes("contour")->palette == vtbackend::ColorPalette {}.palette);
}
```

The test needs `contrastRatio`. Add it to `src/vtbackend/core/Color.hpp`:

`[[nodiscard]] double contrastRatio(RGBColor a, RGBColor b) noexcept;` (WCAG 2.x), built on Task 3's `relativeLuminance`.

Use whatever accessor `defaultColorPalettes` actually exposes. If it takes a palette by reference, adapt the test shape, not the assertions.

- [ ] **Step 2: Run and see it fail.** Run: `cmake --build --preset appleclang-debug --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "ColorPalette.*"`
- [ ] **Step 3: Implement** the three rows and `contrastRatio`.
- [ ] **Step 4: Run** `out/appleclang-debug/src/vtbackend/vtbackend_test "ColorPalette.*,Color.*"`. Expected: PASS.
- [ ] **Step 5: Commit:** `git commit -am "vtbackend: add the contour-dark, contour-light and contour-classic schemes"`

---

### Task 5: No focus frame on a single pane

**Files:**
- Modify: `src/contour/qml/TerminalPane.qml:203-209`:
  - Add `property bool showFocusFrame: false`.
  - The border is `visible: pane.showFocusFrame && pane.paneActive`, 1 px, colour `palette.highlight`.
- Modify: `src/contour/qml/PaneNode.qml`. The leaf Loader sets `showFocusFrame: root.inSplit`. Add `property bool inSplit: false`, which split children receive as `true` through the existing `onLoaded` binding.
- Test: `src/contour/test/QmlComponents_test.cpp`

- [ ] **Step 1: Write the failing test** `"TerminalPane: no focus frame on a single pane"`. Use the existing `QmlComponents_test` harness that instantiates `PaneNode` with a mock leaf.
  - Assert `findChild("focusFrame")->property("visible") == false` for a root leaf.
  - Assert `== true` for an active leaf inside a split.
  - Give the frame `objectName: "focusFrame"`.
- [ ] **Step 2: Run and see it fail.** Run: `out/appleclang-debug/src/contour/contour_gui_test "TerminalPane*"`
- [ ] **Step 3: Implement** as described above.
- [ ] **Step 4: Run** `out/appleclang-debug/src/contour/contour_gui_test "TerminalPane*,PaneNode*,MainWindow*"`. Expected: PASS.
- [ ] **Step 5: Commit:** `git commit -am "contour: draw the focus frame only when a tab is split"`

---

### Task 6: No colour literals in the chrome QML

**Files:**
- Create: `scripts/check-qml-colors.py`. It fails on any `"#RRGGBB"` / `"#AARRGGBB"` / `Qt.rgba(` literal in `src/contour/qml/**/*.qml` and `src/contour/styles/**/*.qml` outside comments, except lines listed in `scripts/qml-color-allowlist.txt` (`path:fragment`). It supports `--self-test`.
- Modify: `CMakeLists.txt` next to `:241`, with `contour_add_lint_gate(check_qml_colors check-qml-colors.py)` and `contour_add_lint_gate(check_qml_colors_selftest check-qml-colors.py --self-test)`.
- Modify: `src/contour/window/UiStyleProvider.{hpp,cpp}` and `config/UiStyle.hpp`. Token `statusColors`, an array of `{light, dark}` pairs indexed by a new `enum class StatusColorRole : uint8_t { Danger = 0, Warning, Success, Running }`. Values:
  - Danger: `{0xD70015, 0xFF6961}`
  - Warning: `{0xC93400, 0xFF9F0A}`
  - Success: `{0x248A3D, 0x32D74B}`
  - Running: the accent (taken from the palette at runtime)
- Provider: `Q_INVOKABLE [[nodiscard]] QColor statusColor(int role) const;`. It picks light or dark by `colorSchemeOfBackground` of the application palette's `Window` colour.
- Modify:
  - `qml/ConfirmDialog.qml:77-88`: `chromeStyle.statusColor(0)`, with hover and pressed variants via `Qt.darker`/`Qt.lighter`.
  - `qml/StatusLineItemDialog.qml:226`: Warning.
  - `qml/WindowControls.qml`: the `#99000000` glyph and the `"white"` hover text come from tokens or the palette.
  - `qml/TabColorFlyout.qml`: the swatch border becomes `palette.mid`.
- Seed `scripts/qml-color-allowlist.txt` only with protocol-defined colours that are data:
  - the traffic-light hex (already a token, so read it);
  - the transparent `"#00000000"`.
- Test: `src/contour/window/UiStyleProvider_test.cpp`.

- [ ] **Step 1: Write the failing tests**
  - `UiStyleProvider_test`: `CHECK(provider.statusColor(0) == QColor(0xD7,0x00,0x15));` under a light palette, and `QColor(0xFF,0x69,0x61)` under a dark one.
  - `check-qml-colors.py --self-test` asserts that:
    - a literal in code is flagged;
    - the same literal inside `//` and `/* */` comments is not flagged (review focus 5);
    - an allowlisted line passes.
- [ ] **Step 2: Run and see it fail.** Run: `ctest --test-dir out/appleclang-debug -R check_qml_colors --output-on-failure`. Expected: the gate lists today's literals, among them `ConfirmDialog.qml:79`, `StatusLineItemDialog.qml:226`.
- [ ] **Step 3: Implement** the provider token and replace each literal. `TerminalPane.qml:207` is already gone (Task 5).
- [ ] **Step 4: Run** `ctest --test-dir out/appleclang-debug -L lint && out/appleclang-debug/src/contour/contour_gui_test "UiStyleProvider*,ConfirmDialog*,QmlComponents*"`. Expected: PASS.
- [ ] **Step 5: Commit:** `git commit -am "contour: take every chrome color from the style or palette, and lint the QML for literals"`

---

### Task 7: Bundle JetBrains Mono

**Files:**
- Create:
  - `src/contour/res/fonts/JetBrainsMono[wght].ttf` and `src/contour/res/fonts/JetBrainsMono-Italic[wght].ttf` (JetBrains Mono v2.304, variable).
  - `src/contour/res/fonts/OFL.txt`.
  - `src/contour/res/fonts/README.md`, giving the source URL, version, licence and sha256.
- Modify: `src/contour/CMakeLists.txt`. Install the fonts into `Contents/Resources/fonts` (macOS bundle) or `${CMAKE_INSTALL_DATADIR}/contour/fonts` (Linux). Windows does not ship them; it uses Cascadia Mono.
- Modify: `src/text_shaper/CoreTextLocator.mm` and `FontconfigLocator.cpp`. Before the first lookup, register each `*.ttf` in an injected directory list:
  - `CTFontManagerRegisterFontsForURL(..., kCTFontManagerScopeProcess, ...)` on macOS;
  - `FcConfigAppFontAddDir` with fontconfig.
- Interfaces:
  - `struct BundledFontDirectories { std::vector<std::filesystem::path> paths; };` is passed to the locator factory in `FontLocatorProvider.cpp`. This is constructor injection, with no global.
  - `[[nodiscard]] std::expected<void, FontRegistrationError> registerBundledFonts(BundledFontDirectories const&);`
  - `enum class FontRegistrationError : uint8_t { DirectoryMissing = 1, RegistrationFailed };`
- Test: `src/text_shaper/FontLocatorProvider_test.cpp`

- [ ] **Step 1: Write the failing tests**
  - Registering a non-existent directory returns `FontRegistrationError::DirectoryMissing`, and the locator still resolves `"monospace"`. This is review focus 4.
  - On macOS and Linux, registering the source-tree `res/fonts` makes `locate("JetBrains Mono")` return a path ending in `JetBrainsMono[wght].ttf`.
- [ ] **Step 2: Run and see it fail.** Run: `cmake --build --preset appleclang-debug --target text_shaper_test && out/appleclang-debug/src/text_shaper/text_shaper_test "FontLocator*"`
- [ ] **Step 3: Implement.** Download the two TTFs from https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304. Verify and record their sha256 in the README. `ContourGuiApp` computes the directory from the app bundle or install prefix and passes it in.
- [ ] **Step 4: Run** the tests above plus `ctest --test-dir out/appleclang-debug -L lint`. The spelling and lint gates must accept the new files. Expected: PASS.
- [ ] **Step 5: Commit:** `git commit -m "contour: bundle JetBrains Mono (SIL OFL 1.1) and register it before font lookup"`

---

### Task 8: First-launch defaults

**Files:**
- Modify `src/contour/config/Config.hpp`; the line numbers are those the defaults audit recorded:

| Line | Setting | New value |
|---|---|---|
| `:873-877` | font family | `"JetBrains Mono"` on macOS and Linux, `"Cascadia Mono"` on Windows (an `#ifdef` beside the existing Monaco branch) |
| `:889` | font size | 13 on `__APPLE__`, 12 elsewhere |
| `:973-976` | terminal size | 100 × 30 |
| `:977-978` | margins | horizontal 8, vertical 6 |
| `:621-643` | status line display | none |
| `:1522-1525` | tab bar visibility | `Multiple` |
| `:651-658` | link decoration mode | `LinkDecorationMode::Modifier` |
| `:960` | `dim_unfocused` | 0.15 |
| `:965` | bell | `.sound = "off"`, alert true |
| `:374` | scrollback | 10000 |
| `:1000-1013` | cursor (insert mode) | block, steady |
| `:1020` | colors | `DualColorConfig { .colorSchemeLight = "contour-light", .colorSchemeDark = "contour-dark" }`, with the palettes resolved the way the loader resolves names |

- Modify `src/contour/config/ConfigDocumentation.hpp` so that every generated value above matches.
- Test: `src/contour/config/Config_test.cpp`

- [ ] **Step 1: Write the failing test** `TEST_CASE("Config.firstLaunchDefaults", "[config]")`, asserting each value in the table against `Config {}` and its default profile. It also asserts that `contour generate config` output contains `colors:\n  dark: contour-dark` (use the existing YAML-writer test helper).
- [ ] **Step 2: Run and see it fail.** Run: `out/appleclang-debug/src/contour/contour_gui_test "Config.firstLaunchDefaults"`
- [ ] **Step 3: Implement** the new values. Then run the full GUI suite and update only those assertions that encoded old *default* values. Never relax an assertion about behaviour.
- [ ] **Step 4: Run the full suite** `ctest --preset appleclang-debug`. Expected: 100 % pass. The e2e coverage config at `src/contour/test/e2e/coverage-config.yml.in` may need the new scheme names.
- [ ] **Step 5: Commit:** `git commit -am "contour: ship first-launch defaults that look good without a config"`

---

### Task 9: Finish

- [ ] **Step 1:** Run `/simplify` on the branch diff and apply its findings.
- [ ] **Step 2:** Add release notes with `/add-release-note`: new defaults, contour-dark/light/classic, `theme: terminal`, macOS shortcut glyphs, bundled JetBrains Mono.
- [ ] **Step 3:** Run `ctest --preset appleclang-debug` (on Linux CI, `clang-asan`), then take before/after screenshots with `scratchpad/snapshot-hook.patch`, applied locally and never committed.
- [ ] **Step 4:** Open a draft PR with `/draft-pr`.
