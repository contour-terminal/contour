# Native Redesign — Step 1a (Kit Primitives, Tokens, Accessibility, New-Defaults Banner) Implementation Plan

Amended after its pre-flight scan (findings F1–F27).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the chrome its kit — the 1a token groups, the `Banner`, `Toast` and `KitDialog`
primitives and a size-literal gate — and connect it to the OS: accessibility preferences and platform
appearance (null and macOS backends), Reduce Motion for the terminal's own durations, a chrome that
follows an OSC 11 background, the hosting notice as a Banner, and the one-time new-defaults banner.

**Architecture:**
- **Tokens are data, Qt-free.** `config/ChromeTokenGroups.hpp` holds one `ChromeTokenRow` per
  `ChromeStyle` and a `constexpr` resolver that applies the motion, transparency and contrast
  overrides of `AccessibilityPreferences` (`config/AccessibilityPreferences.hpp`). Both are tested in
  `contour_test`.
- **The OS is an interface.** `platform/` gains `IAccessibilityPreferenceSource` and
  `IPlatformAppearance`, each with a null backend, a macOS `.mm` backend and a test double. Change
  notification is a `std::function` handed to the backend's constructor. `ContourGuiApp` receives both
  as factories in one `platform::AppearanceServices` constructor parameter; `TestApp` always passes
  the null ones.
- **The provider is rebuilt, never mutated.** `UiStyleProvider` takes a `ChromeInputs` (row, font,
  accessibility, appearance, scheme colours) and publishes the groups as `CONSTANT` QObjects
  (`chromeStyle.size.toast`). `window::ChromeStyleHost` owns it and republishes `chromeStyle` when an
  input changes.
- **Primitives are presentational.** `Toast.qml`, `Banner.qml` and `KitDialog.qml` read only tokens;
  queueing arrives in 3c. Main.qml gains a banner stack between the title bar and the content.
- **Pure decisions stay headless:** the motion adjustment of `vtbackend::Settings`, the live-background
  chrome scheme, and the new-defaults offer are all tested without a window.

**Tech Stack:** C++23, Qt 6.11 / QML, Qt Quick Controls (Fusion, ContourTui), Objective-C++ (AppKit) on
Apple, Catch2 v3, Python 3 (lint), CMake preset tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.1, which binds,
with §2.1 (token rows), §2.3 (accessibility preferences), §2.4 (platform appearance), and what they cite:
§1 (principles), §2.2 (chrome follows the terminal), §2.6 (testing) and §2.7 (the edition banner).
Visual design record: `docs/superpowers/specs/2026-10-23-native-redesign-design.md`. Canvas:
<https://claude.ai/artifact/6cp7tcSnurTSMbhSDePWqH> (boards *Design kit — tokens*, *Design kit —
components*, *Terminal (TUI) style*, *Daemon & remote hosts*). Plans index:
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

## Status board and handoff (every step)

This run is tracked on a status board, <https://claude.ai/artifact/U8h58rAp7H6yVfnvp4vvEd>, published from an untracked file at the root of the working tree. The plans index says how to keep it current.
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
  - **Dependency injection.** Accessibility preferences and platform appearance are reached only
    through `IAccessibilityPreferenceSource` and `IPlatformAppearance`, each with a null implementation
    for tests and for every platform without a backend. Files go through the injected stores
    (`GuiConfigStore`, the new `GuiStateStore`). No test's outcome depends on the developer's real OS
    settings; only the `[cocoa]` backend cases read them, and they compare against what they read.
  - **Configuration at construction.** `UiStyleProvider`, `ChromeStyleHost`, `NewDefaultsNotice`, the
    backends and every mock take their shape in the constructor. An input change rebuilds the provider.
  - **`enum class` over `bool`,** with the default enumerator at zero. The only new `bool`s are the
    QML-facing `Q_PROPERTY`s named below, each documented at its declaration as that carve-out.
  - **`std::expected`** for fallible calls. Error enums start at `= 0`.
  - **Data-driven tables:** token rows, colour sources, the motion settings, the size-literal property
    list and baseline, the snapshot rows, the dismissed-offer tokens.
  - **Doxygen** (`///`, `@param`, `@return`) on every new public declaration; `.hpp` with `#pragma once`.
  - **Zero warnings** under `-Werror`, and **no `NOLINT`**.
- **Build** with at most 3 jobs: `cmake --build out/appleclang-debug -j 3 --target <target>`. The tree is
  configured with `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`);
  reconfigure with it if needed. Never run two builds at once.
- **Tests:** each task runs only its own filter. The full suite runs in CI on Linux, macOS and Windows.
- **Format** every touched `.cpp`/`.hpp`/`.mm` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`
  before each commit; it is CI's version. Check every new or touched non-C++ file (`.qml`, `.py`,
  `.txt`) with `editorconfig-checker <files>` before its commit; CI runs it as the `editorconfig` job
  (`.github/workflows/build.yml:390-396`).
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** a designated initializer may
  omit only a field that has a default member initializer. So every new struct member gets `{}` or a
  value (which clang-tidy's `cppcoreguidelines-pro-type-member-init` requires anyway), and existing
  designated initializers need no edit when such a field is added. AppleClang may not warn locally; CI does.
- **Header-only functions are `inline`** (or `constexpr`). `std::views::iota` takes two bounds of the
  same type (`std::size_t { 0 }` against a `size_t` count).
- **Never edit `vendor/core-cpp`.**
- **TestApp is Classic (ruling R14).** `TestApp` builds `DefaultsEdition::Classic` and always passes the
  null appearance services. Native2026 is exercised through the snapshot scenes.
- **macOS-only code compiles only on Apple** (`.mm` added inside the `if(APPLE)` blocks of
  `src/contour/platform/CMakeLists.txt:78-85` and `src/contour/CMakeLists.txt:809-815`), with the null
  backend everywhere else. The Windows and Linux backends arrive in Steps 5a/5b.
- **Qt-free targets stay Qt-free:** `config/AccessibilityPreferences.hpp`, `config/ChromeTokenGroups.hpp`,
  `config/NewDefaultsOffer.hpp` and `config/GuiStateStore.*` include no Qt header.
- **Existing geometry does not move.** The new tokens are consumed only by the new primitives; the
  banner stack is zero-height while no banner shows, so `MainWindowQml_test`'s geometry cases stay green
  unchanged (§2.1 "Native rows").
- **Every visual task renders the Terminal (TUI) row** (§3.1 *TUI row*): Toast is a one-row inverse cell
  strip, Banner is one row in the scheme's palette[0], the dialog is a boxed popup with the hard
  one-cell shadow.
- After the code changes, run `/simplify` (Task 12).

## Review Focus

1. **The suite reads the developer's Reduce Motion.** A `TestApp` that fell back to the native backend
   would make terminal-duration tests pass or fail by the Mac's System Settings. `TestApp` must pass
   null services unless a test hands it a switch. Pinned in Task 3 (default `TestApp` reports
   `MotionPreference::Full` whatever the host says) and Task 9.
2. **A Reset Config that cannot take effect, or a banner that never goes away.** After Reset Config,
   `contour.yml` says `native2026` but a `settings.yml` saying `classic` still wins; the user must be
   offered the switch even if they once pressed "Keep Mine" on the first offer. Conversely, "Keep Mine"
   on that second offer must hide it for good. Pinned in Task 11 (decision table rows and the
   per-offer dismissal round trip).
3. **Rebuilding `chromeStyle` under live bindings.** An accessibility or accent change swaps the
   provider while popups and bindings still hold the old one; deleting it at once would leave dangling
   bindings and QML `TypeError`s. The old provider must outlive the current event. Pinned in Task 3.
4. **A background tab recolours the chrome.** OSC 11 from an unfocused session (a `vim` in another tab
   setting its background) must not flip the chrome's light/dark; only the focused session's does, and
   a focus change re-evaluates. Pinned in Task 10.
5. **A banner moves the terminal and never gives the room back.** The banner pushes the content down
   once; dismissing must restore the content's exact height, and with the tab bar at the bottom the
   banner still sits at the top of the window. Pinned in Task 8.

Also pinned, beyond the five: a `Use New` whose write fails keeps the banner up and logs why (Task 11);
the size-literal baseline fails in both directions, so it only ratchets down (Task 4).

---

## Spec resolutions (read before Task 1)

These settle where the spec is silent or the code disagrees with it. Implementers follow them.

- **R-a. `radius` is taken.** `UiStyleProvider` already publishes `int radius` (`UiStyleProvider.hpp:143`)
  and the chrome QML reads `chromeStyle.radius`. The §2.1 radius group is therefore published as
  **`chromeStyle.radii`** (`radii.control`, `radii.popover`, `radii.menu`, `radii.paneCard`,
  `radii.toast`). The legacy property stays until its readers migrate.
- **R-b. Where the terminal durations live.** §2.3 says the display is constructed with the snapshot.
  In the code, both durations travel `config::sessionSettings` (`Config.cpp:981-983`) →
  `vtbackend::Settings` → `Terminal` (`Terminal.cpp:1396`, `:7564`), and the display never sees them.
  Reduce Motion is therefore applied where `TerminalSession` calls `sessionSettings`
  (`TerminalSession.cpp:388`, `:5925`), through one pure function (Task 9).
- **R-c. AA-large status colours.** Every `StatusColors` value (`UiStyle.hpp:259-264`) already reaches
  ≥ 3:1 against both a white and a `#1C1C1E` surface (lowest: `#248A3D` at 4.4:1 on white,
  `#FF6961` at 4.9:1 on `#2C2C2E`). Increased Contrast keeps them, and the resolver test pins the
  3:1 floor so a later softening of the Standard values cannot break it.
- **R-d. Value types below `config/`.** `AccessibilityPreferences` must be read by the Qt-free token
  resolver and by `sessionSettings`' caller, so the value types live in `config/`; the interfaces that
  ask the OS live in `platform/` (§1.4).
- **R-e. Button layout waits for 5b.** `PlatformAppearance` carries `accent` and `windowCornerRadius`
  now; the Linux button layout field is added with the Linux backend that fills it.
- **R-f. TUI colour roles.** The Terminal row's banner and toast colours come from the default
  profile's palette at the current light/dark preference (`config::preferredColorPalette`). The
  provider is rebuilt when that palette changes (reload, OS light/dark flip). It is the *configured*
  scheme: a background set live by OSC 11 (Task 10) moves only the chrome's light/dark, not the
  Terminal row's colour roles.
- **R-g. The banner pushes the content.** Today's comment at `Main.qml:228-231` overlays the notice "so
  the terminal is not resized". §3.1 and the canvas say a Banner is a band that pushes the content down
  once, so the terminal loses a row while a banner shows. No height animation.
- **R-h. OSC 11 follows only where the chrome is already forced.** `qtColorSchemeFor`
  (`GuiTheme.hpp:43-58`) forces a scheme for `theme: terminal` only over a simple colour config; over a
  dual config the chrome follows the OS, and the OS handler at `ContourGuiApp.cpp:2101` relies on the
  palette not being overridden. The live background therefore decides only in the simple case.

**Ratified departures from the spec's text** (owner, after the pre-flight scan; each is justified by the
code and changes no behaviour the spec asks for): R-a publishes `radii.*` where §2.1 says `radius.*`;
the token rows live in a new `config/ChromeTokenGroups.hpp` though §2.1 is headed `config/UiStyle.hpp`;
R-b applies Reduce Motion in `sessionSettings` rather than at the display's construction; R-c keeps the
Standard status colours under Increased Contrast, since they are already their AA-large variant; and
`NewDefaultsOffer.hpp` is a pure decision in `config/`, which §1.3's list does not name.

---

### Task 1: Accessibility preferences and the 1a token groups (Qt-free)

**Files:**
- Create: `src/contour/config/AccessibilityPreferences.hpp`
- Create: `src/contour/config/ChromeTokenGroups.hpp`
- Create: `src/contour/config/ChromeTokenGroups_test.cpp`; add it to `contour_test`'s sources
  (`src/contour/CMakeLists.txt:1115-1127`, beside `config/UiStyle_test.cpp`).
- Modify: `src/contour/config/CMakeLists.txt` — list the two headers in `contour_config`'s sources.

**Interfaces:**
- Produces (namespace `contour::config`):
  - `enum class MotionPreference : uint8_t { Full = 0, Reduced };`
  - `enum class TransparencyPreference : uint8_t { Allowed = 0, Reduced };`
  - `enum class ContrastPreference : uint8_t { Standard = 0, Increased };`
  - `struct AccessibilityPreferences { MotionPreference motion = MotionPreference::Full; TransparencyPreference transparency = TransparencyPreference::Allowed; ContrastPreference contrast = ContrastPreference::Standard; [[nodiscard]] bool operator==(AccessibilityPreferences const&) const = default; };`
  - `enum class Material : uint8_t { None = 0, Glass, Mica, Solid };`
  - `enum class ToastStyle : uint8_t { Floating = 0, Inverse };`
  - `enum class DialogPlacement : uint8_t { Centered = 0, Sheet };`
  - `enum class ChromeColorRole : uint8_t { BannerFill = 0, BannerText, BannerGlyph, ToastFill, ToastText };`
  - `enum class ColorSourceKind : uint8_t { Fixed = 0, PaletteWindow, PaletteWindowText, Status, Scheme };`
  - `enum class SchemeSlot : uint8_t { Background = 0, Foreground, Bar, Secondary, Selection, Accent, Danger, Success, Warning };`
  - `struct ChromeColorSource { ColorSourceKind kind = ColorSourceKind::Fixed; uint32_t rgb = 0; StatusColorRole status = StatusColorRole::Danger; SchemeSlot slot = SchemeSlot::Background; int alphaPercent = 100; [[nodiscard]] bool operator==(ChromeColorSource const&) const = default; };`
  - `struct ChromeSchemeColors { std::array<vtbackend::RGBColor, SchemeSlotCount> bySlot {}; [[nodiscard]] bool operator==(ChromeSchemeColors const&) const = default; };`
  - Group structs, every member defaulted to 0 / the zero enumerator:
    `SizeTokens { int titleBar, tabWell, tab, control, paneHeader, toast, dialogWidth; }`,
    `SpaceTokens { int paneGap, splitHit, paneInset; std::array<int, 10> scale; }`,
    `RadiusTokens { int control, popover, menu, paneCard, toast; }`,
    `TypeTokens { double caption, footnote, body, tab, title; }` (0 = the chrome font's own size),
    `MotionTokens { int fastMs, standardMs, emphasizedMs, travel; }`,
    `ElevationLevel { int offsetX, offsetY, blur, opacityPercent; }`,
    `ElevationTokens { std::array<ElevationLevel, 4> levels; }` (e0..e3),
    `MaterialTokens { Material titleBand, popover; }`,
    `ContrastTokens { int separatorPercent; double activeCardRing; std::optional<int> paneDimPercent; }`,
    `TuiTokens { double inactivePaneOpacity; ToastStyle toastStyle; }`,
    `LayoutTokens { DialogPlacement dialog; }`.
    Each of these structs, `ElevationLevel` included, declares
    `[[nodiscard]] bool operator==(<Struct> const&) const = default;`, so a test can compare whole groups.
  - `struct ChromeTokenRow { SizeTokens size; SpaceTokens space; RadiusTokens radii; TypeTokens type; MotionTokens motion; ElevationTokens elevation; MaterialTokens material; ContrastTokens contrast; TuiTokens tui; LayoutTokens layout; std::array<ChromeColorSource, ChromeColorRoleCount> colors; };`
  - `inline constexpr size_t SchemeSlotCount`, `ChromeColorRoleCount` (derived from the last enumerator,
    as `StatusColorRoleCount` is at `UiStyle.hpp:256`).
  - `inline constexpr std::array<ChromeTokenRow, detail::ChromeStyleCount> ChromeTokenRows`, indexed by
    `ChromeStyle`, with a `static_assert` on its size.
  - `inline constexpr int ReducedMotionCrossFadeMs = 100;`
  - `[[nodiscard]] constexpr ChromeTokenRow resolveChromeTokens(ChromeStyle row, AccessibilityPreferences preferences) noexcept;`
  - `[[nodiscard]] inline ChromeSchemeColors chromeSchemeColorsOf(vtbackend::ColorPalette const& palette);`
    Slots: Background = `defaultBackground`, Foreground = `defaultForeground`, Bar = `normalColor(0)`,
    Secondary = `brightColor(0)`, Selection from `selection.background` (a `CellRGBColor` variant,
    `Color.hpp:482`): an `RGBColor` is taken as-is, `CellForegroundColor` gives `defaultForeground`, and
    `CellBackgroundColor` gives `defaultBackground`; Accent = `normalColor(4)`, Danger = `normalColor(1)`, Success = `normalColor(2)`,
    Warning = `normalColor(3)`.

**The rows** (native rows in logical pixels, the Terminal row in cells; values from the canvas *Design
kit — tokens* board, and §2.1's component tokens):

| Token | MacOS | Fluent | ContourLinux | Terminal |
|---|---|---|---|---|
| `size.titleBar · tabWell · tab` | 38 · 28 · 24 | 40 · 0 · 34 | 46 · 0 · 34 | 1 · 1 · 1 |
| `size.control · paneHeader` | 28 · 22 | 32 · 28 | 34 · 28 | 2 · 1 |
| `size.toast · dialogWidth` | 36 · 280 | 36 · 280 | 36 · 280 | 1 · 32 |
| `space.paneGap · splitHit · paneInset` | 6 · 10 · 6 | 4 · 10 · 4 | 6 · 10 · 6 | 1 · 1 · 0 |
| `space.scale` (s1…s10) | 2 4 6 8 10 12 16 20 24 32 | same | same | 0 0 0 1 1 1 1 1 2 3 |
| `radii.control · popover · menu · paneCard · toast` | 6 · 14 · 10 · 10 · 18 | 4 · 8 · 8 · 4 · 18 | 8 · 12 · 10 · 6 · 18 | 0 everywhere |
| `type.caption · footnote · body · tab · title` | 10 · 11 · 13 · 12 · 15 | 12 · 12 · 14 · 12 · 20 | 12 · 12.5 · 14 · 13.5 · 15 | 0 everywhere |
| `motion.fast · standard · emphasized · travel` | 100 · 160 · 220 · 8 | same | same | 100 · 160 · 220 · 0 |
| `elevation` e0 · e1 · e2 · e3 as {x, y, blur, %} | {0,0,0,0} · {0,1,2,12} · {0,4,16,25} · {0,12,40,35} | same | same | {0,0,0,0} · {1,1,0,60} ×3 |
| `material.titleBand · popover` | Glass · Glass | Mica · Solid | Solid · Solid | None · None |
| `contrast` (Standard) | separator 10, ring 0.0, paneDim nullopt | same | same | same |
| `tui.inactivePaneOpacity · toastStyle` | 1.0 · Floating | same | same | 0.62 · Inverse |
| `layout.dialog` | Sheet | Centered | Centered | Centered |
| `colors.BannerFill · BannerText · BannerGlyph` | PaletteWindow · PaletteWindowText · Status Warning | same | same | Scheme Bar · Scheme Foreground · Scheme Warning |
| `colors.ToastFill · ToastText` | Fixed `0x28282C` @ 92 % · Fixed `0xFFFFFF` | same | same | Scheme Foreground · Scheme Background |

**Overrides** in `resolveChromeTokens`:
- `MotionPreference::Reduced`: `fastMs`, `standardMs`, `emphasizedMs` all become `ReducedMotionCrossFadeMs`;
  `travel` becomes 0.
- `TransparencyPreference::Reduced`: `Glass` and `Mica` become `Solid`; `None` stays `None`.
- `ContrastPreference::Increased`: `separatorPercent` doubles, `activeCardRing` becomes 1.5, and
  `paneDimPercent` becomes 5 (§2.1 *Contrast overrides*). Status colours are unchanged (R-c).

- [ ] **Step 1: Write the failing tests** in `ChromeTokenGroups_test.cpp`, all tagged `[chrometokens]`:

```cpp
TEST_CASE("chromeTokens.componentTokens", "[chrometokens]")
{
    using namespace contour::config;
    using enum ChromeStyle;
    auto const mac = resolveChromeTokens(MacOS, {});
    CHECK(mac.size.toast == 36);
    CHECK(mac.radii.toast == 18);
    CHECK(mac.motion.fastMs == 100);
    CHECK(mac.motion.standardMs == 160);
    CHECK(mac.material.titleBand == Material::Glass);
    CHECK(mac.layout.dialog == DialogPlacement::Sheet);
    auto const tui = resolveChromeTokens(Terminal, {});
    CHECK(tui.size.toast == 1);
    CHECK(tui.radii.toast == 0);
    CHECK(tui.material.popover == Material::None);
    CHECK(tui.tui.inactivePaneOpacity == Catch::Approx(0.62));
    CHECK(tui.tui.toastStyle == ToastStyle::Inverse);
    CHECK(tui.elevation.levels[2].offsetX == 1);
    CHECK(tui.elevation.levels[2].blur == 0);
    CHECK(tui.colors[static_cast<size_t>(ChromeColorRole::BannerFill)].slot == SchemeSlot::Bar);
}
```

  Plus these cases, each a loop over every `ChromeStyle` (`std::views::iota(std::size_t { 0 }, detail::ChromeStyleCount)`):
  - `chromeTokens.reducedMotion`: under `{ .motion = MotionPreference::Reduced, .transparency = TransparencyPreference::Allowed, .contrast = ContrastPreference::Standard }`,
    all three durations equal `ReducedMotionCrossFadeMs` and `travel == 0`; every other group equals the
    unadjusted row's (compared with the groups' defaulted `operator==`).
  - `chromeTokens.reducedTransparency`: no resolved material is `Glass` or `Mica`; the Terminal row stays `None`.
  - `chromeTokens.increasedContrast`: `separatorPercent == 2 * row.separatorPercent`,
    `activeCardRing == 1.5`, `paneDimPercent == 5`.
  - `chromeTokens.statusColoursAreLargeTextSafe`: for every `StatusColors` row with source `Fixed`,
    `contrastRatio(RGBColor { row.light }, 0xFFFFFF_rgb) >= 3.0` and
    `contrastRatio(RGBColor { row.dark }, 0x1C1C1E_rgb) >= 3.0` (R-c; `RGBColor(uint32_t)` is `explicit`,
    `Color.hpp:55`). Bring `vtbackend::operator""_rgb` into scope at file level, as `GuiTheme_test.cpp:21` does.
  - `chromeTokens.schemeColours`: `chromeSchemeColorsOf(palette)` of a palette with distinct colours maps
    each slot as listed above (Bar = `normalColor(0)`, Warning = `normalColor(3)`, …); a second row whose
    `selection.background` is `CellForegroundColor {}` gives Selection = `defaultForeground`.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test`

Expected: compile failure — `ChromeTokenGroups.hpp` does not exist.

- [ ] **Step 3: Implement** the two headers with the rows above. The rows are `ChromeTokenRow { … }` with
  every field designated. The overrides are three small `constexpr` steps inside `resolveChromeTokens`.

- [ ] **Step 4: Run them and see them pass.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test && out/appleclang-debug/src/contour/contour_test "[chrometokens],UiStyle*"`

Expected: PASS. `UiStyle*` confirms `UiStyle.hpp` is untouched.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/AccessibilityPreferences.hpp src/contour/config/ChromeTokenGroups.hpp src/contour/config/ChromeTokenGroups_test.cpp
git add src/contour/config src/contour/CMakeLists.txt
git commit -m "config: describe the 1a chrome token groups and the accessibility overrides as data"
```

---

### Task 2: Accessibility and appearance sources (null, macOS, test doubles)

**Files:**
- Create: `src/contour/platform/AccessibilityPreferenceSource.hpp` (the interface, and
  `NullAccessibilityPreferenceSource` defined inline) and `.cpp` (only the non-Apple
  `makeAccessibilityPreferenceSource`, wholly inside `#if !defined(__APPLE__)`, as
  `TaskbarProgressSink.cpp:24` does).
- Create: `src/contour/platform/PlatformAppearance.hpp` and `.cpp` (same shape: `NullPlatformAppearance`
  inline in the header, only `makePlatformAppearance` in the guarded `.cpp`).
- Create: `src/contour/platform/AppearanceServices.hpp` (`nullAppearanceServices()` is `inline` here, so
  the null side exists on every platform, Apple included).
- Create: `src/contour/platform/CocoaAccessibilityPreferenceSource.mm` and
  `src/contour/platform/CocoaPlatformAppearance.mm` (ARC; Apple factories).
- Create: `src/contour/test/AppearanceFixtures.hpp` (test doubles).
- Create: `src/contour/platform/AppearanceSources_test.cpp` (all platforms) and
  `src/contour/platform/CocoaAppearanceSources_test.mm` (Apple only).
- Modify: `src/contour/platform/CMakeLists.txt` — the `.hpp`/`.cpp` pairs in `contour_platform`'s list
  (`:15-66`); the two `.mm` in the `if(APPLE)` `target_sources` at `:82`, with `-fobjc-arc` as `:83` sets it.
- Modify: `src/contour/CMakeLists.txt` — `platform/AppearanceSources_test.cpp` in `contour_gui_test`
  (`:749-807`); `platform/CocoaAppearanceSources_test.mm` in the `if(APPLE)` block at `:809-815`, with
  `-fobjc-arc`.

**Interfaces:**
- Consumes: `config::AccessibilityPreferences` (Task 1).
- Produces (namespace `contour::platform`):
  - `class IAccessibilityPreferenceSource { public: virtual ~IAccessibilityPreferenceSource() = default; [[nodiscard]] virtual config::AccessibilityPreferences current() const = 0; };`
  - `using AccessibilityPreferencesChanged = std::function<void(config::AccessibilityPreferences)>;`
    Called on the GUI thread.
  - `class NullAccessibilityPreferenceSource final: public IAccessibilityPreferenceSource` —
    `current()` returns `{}`; defined wholly in the header.
  - `[[nodiscard]] std::unique_ptr<IAccessibilityPreferenceSource> makeAccessibilityPreferenceSource(AccessibilityPreferencesChanged onChange);`
  - `struct PlatformAppearance { std::optional<vtbackend::RGBColor> accent {}; std::optional<int> windowCornerRadius {}; [[nodiscard]] bool operator==(PlatformAppearance const&) const = default; };`
  - `class IPlatformAppearance { … [[nodiscard]] virtual PlatformAppearance current() const = 0; };`,
    `using PlatformAppearanceChanged = std::function<void(PlatformAppearance)>;`,
    `class NullPlatformAppearance final` (defined wholly in the header), and
    `[[nodiscard]] std::unique_ptr<IPlatformAppearance> makePlatformAppearance(PlatformAppearanceChanged onChange);`
  - The two `make*` factories are the only out-of-line definitions: the guarded `.cpp` off Apple, the
    `.mm` on Apple.
  - `struct AppearanceServices { std::function<std::unique_ptr<IAccessibilityPreferenceSource>(AccessibilityPreferencesChanged)> accessibility = makeAccessibilityPreferenceSource; std::function<std::unique_ptr<IPlatformAppearance>(PlatformAppearanceChanged)> appearance = makePlatformAppearance; };`
  - `[[nodiscard]] inline AppearanceServices nullAppearanceServices();` — defined in
    `AppearanceServices.hpp`; factories that return the null backends.
- Produces (namespace `contour::test`, `AppearanceFixtures.hpp`):
  - `struct AccessibilitySwitch { config::AccessibilityPreferences preferences {}; platform::AccessibilityPreferencesChanged notify {}; void set(config::AccessibilityPreferences next); };` —
    `set`, defined in-class, stores and calls `notify` when it is set.
  - `struct AppearanceSwitch` — the same over `platform::PlatformAppearance`.
  - `[[nodiscard]] inline platform::AppearanceServices appearanceServicesWith(AccessibilitySwitch& accessibility, AppearanceSwitch& appearance);` —
    each factory stores the callback into the switch and returns a source that reads the switch.

**The macOS backends** (each in its `.mm`, with a class in an anonymous namespace):
- Accessibility reads `NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion`,
  `…ShouldReduceTransparency` and `…ShouldIncreaseContrast`. Its constructor adds an observer for
  `NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification` on
  `NSWorkspace.sharedWorkspace.notificationCenter` with `queue:nil` (NSWorkspace posts on the main
  thread, so the callback runs on the GUI thread); the block calls `onChange(current())`. The destructor
  removes the observer.
- Appearance reads `NSColor.controlAccentColor` converted with
  `colorUsingColorSpace:NSColorSpace.sRGBColorSpace` into an `RGBColor`, and returns
  `windowCornerRadius = std::nullopt` (macOS publishes none; the row carries the measured constant
  later). It observes `NSSystemColorsDidChangeNotification` on `NSNotificationCenter.defaultCenter`, `queue:nil`.

- [ ] **Step 1: Write the failing tests.**
  - `AppearanceSources_test.cpp`, tag `[appearance-source]`:
    - `NullAccessibilityPreferenceSource{}.current() == config::AccessibilityPreferences {}` and
      `NullPlatformAppearance{}.current() == platform::PlatformAppearance {}`.
    - `nullAppearanceServices()`: both factories return a source whose `current()` is the default, and
      neither calls its callback on construction.
    - `appearanceServicesWith(sw, aw)`: after `sw.set({ .motion = Reduced, .transparency = Allowed, .contrast = Standard })`
      the created source reports `Reduced`, and the callback handed to the factory received it once.
  - `CocoaAppearanceSources_test.mm`, tag `[appearance-source][cocoa]`:
    - `makeAccessibilityPreferenceSource({})->current()` equals the three `NSWorkspace` flags read in the test.
    - Posting `NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification` on the workspace centre calls
      the callback once with `current()`.
    - `makePlatformAppearance({})->current().accent` is set and equals `controlAccentColor` in sRGB;
      posting `NSSystemColorsDidChangeNotification` calls its callback once.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: compile failure — the headers do not exist.

- [ ] **Step 3: Implement** the headers (the null backends and `nullAppearanceServices()` inline in
  them), the two `.cpp` factories (`#if !defined(__APPLE__)`, returning the null backends), the two `.mm`
  (Apple factories only), and the fixtures.

- [ ] **Step 4: Run them and see them pass.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[appearance-source]"`

Expected: PASS, including the `[cocoa]` cases on this Mac (they need no display).

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/platform/AccessibilityPreferenceSource.* src/contour/platform/PlatformAppearance.* src/contour/platform/AppearanceServices.hpp src/contour/platform/Cocoa*Source*.mm src/contour/platform/CocoaPlatformAppearance.mm src/contour/platform/*AppearanceSources_test.* src/contour/test/AppearanceFixtures.hpp
git add src/contour/platform src/contour/test/AppearanceFixtures.hpp src/contour/CMakeLists.txt
git commit -m "platform: ask the OS for accessibility preferences and appearance through interfaces"
```

---

### Task 3: Grouped tokens on `chromeStyle`, rebuilt on change; the app's appearance services

**Files:**
- Create: `src/contour/window/ChromeTokenObjects.hpp` and `.cpp` — the QML-facing group objects.
- Create: `src/contour/window/ChromeStyleHost.hpp` and `.cpp`.
- Modify: `src/contour/window/UiStyleProvider.hpp`/`.cpp` — `ChromeInputs`, the new constructor, the group
  properties, `accentColor`, and Running from the platform accent.
- Modify: `src/contour/test/QmlChromeStyle.hpp:18-53` — `ChromeSetup` gains
  `config::AccessibilityPreferences accessibility {}`, `platform::PlatformAppearance appearance {}` and
  `config::ChromeSchemeColors scheme {}`, each with a `{}` default member initializer, so the existing
  designated initializers (`defaultChromeSetup`, `MainWindowQml_test.cpp:427`,
  `ChromeSnapshotScene.hpp:329`) need no edit; `installChromeStyle` builds `ChromeInputs` from it.
- Modify: `src/contour/ContourGuiApp.hpp:230-245` (constructor), `:856` (`_uiStyleProvider` becomes
  `_chromeStyleHost`), and add the two source members; `ContourGuiApp.cpp:396-410` (the signature gains
  the parameter) and the initialiser list from `:411`, `:2210-2211` and `:2252` (host instead of
  provider), `:2111` (refresh on a light/dark flip).
- Modify: `src/contour/session/TerminalSessionManager.cpp:741` — call `_app.refreshChromeStyle()` after
  `applyGuiTheme`.
- Modify: `src/contour/main.cpp:220-234` — pass `contour::platform::AppearanceServices {}` before `processStart`.
- Modify: `src/contour/test/GuiTestFixtures.hpp` — every `TestApp` constructor (`:480-503`, `:516-533`,
  `:563-579`) passes `appearance`; the first gains a trailing parameter
  `contour::platform::AppearanceServices appearance = contour::platform::nullAppearanceServices()`.
- Modify: `src/contour/CMakeLists.txt:278`, `:323` — list the new `window/` files in `contour_core`.
- Test: `src/contour/window/UiStyleProvider_test.cpp`, `src/contour/test/ContourGuiApp_test.cpp`.

**Interfaces:**
- Consumes: Task 1's `resolveChromeTokens`, `ChromeSchemeColors`, `chromeSchemeColorsOf`; Task 2's
  `PlatformAppearance`, `AppearanceServices`, `nullAppearanceServices`, `AccessibilitySwitch`.
- Produces:
  - `struct window::ChromeInputs { config::ChromeStyle row = config::ChromeStyle::Terminal; QFont font {}; config::AccessibilityPreferences accessibility {}; platform::PlatformAppearance appearance {}; config::ChromeSchemeColors scheme {}; };`
  - `[[nodiscard]] ChromeInputs chromeInputsFor(config::ChromeStyle row, QFont const& font);` — defaults for
    the rest; `scheme = chromeSchemeColorsOf(vtbackend::ColorPalette {})`.
  - `explicit UiStyleProvider(ChromeInputs inputs, QObject* parent = nullptr);` The existing
    `UiStyleProvider(config::ChromeStyle, QFont const&, QObject*)` stays as a one-line delegate to it.
  - New `UiStyleProvider` properties, all `CONSTANT` (`QObject*` to children it owns):
    `size`, `space`, `radii`, `type`, `motion`, `elevation`, `material`, `contrast`, `tui`, `layout`;
    `colors` (`QObject*`, `CONSTANT`; its own properties notify); and
    `Q_PROPERTY(QColor accentColor READ accentColor NOTIFY paletteRevisionChanged)`.
  - `[[nodiscard]] QColor accentColor() const;` — the platform accent when set, else the palette's
    Highlight. `statusColor(Running)` returns `accentColor()`.
  - Group objects (in `ChromeTokenObjects.hpp`, each a `QObject` constructed from its resolved struct and
    the cell metrics, every property `CONSTANT`, extents already in logical pixels — horizontal extents ×
    `widthQuantum`, vertical × the cell height under `LengthUnit::Cell`, as `chromeMetricsFor` does at
    `UiStyleProvider.cpp:51-93`):
    - `ChromeSizeTokens`: `qreal titleBar, tabWell, tab, control, paneHeader, toast, dialogWidth`
      (`dialogWidth` and `control` horizontal, the rest vertical).
    - `ChromeSpaceTokens`: `qreal paneGap, splitHit, paneInset, s1 … s10` (horizontal). A vertical use
      of a space token (Banner's `s3` padding, Task 5) is exact only while the Terminal row's value is 0,
      as `s3` is; a vertically quantised space group is a later step's concern.
    - `ChromeRadiusTokens`: `qreal control, popover, menu, paneCard, toast`.
    - `ChromeTypeTokens`: `QFont caption, footnote, body, tab, title` — the chrome font with
      `setPixelSize(static_cast<int>(std::lround(value)))`, or the chrome font itself where the value is 0.
    - `ChromeMotionTokens`: `int fast, standard, emphasized`; `qreal travel`.
    - `ChromeElevationLevel`: `qreal offsetX, offsetY, blur; qreal opacity` (0–1); `ChromeElevationTokens`:
      `QObject* e0, e1, e2, e3`.
    - `ChromeMaterialTokens`: `int titleBand, popover` (as `config::Material`'s underlying value) and the
      CONSTANT ints `none`, `glass`, `mica`, `solid` naming them, as `statusDanger` names roles
      (`UiStyleProvider.hpp:110-113`).
    - `ChromeContrastTokens`: `qreal separatorAlpha` (percent / 100), `qreal activeCardRing`.
      (`paneDimPercent` stays C++-side until Step 2 consumes it.)
    - `ChromeTuiTokens`: `qreal inactivePaneOpacity`, `int toastStyle`, and CONSTANT ints `floating`, `inverse`.
    - `ChromeLayoutTokens`: `int dialog`, and CONSTANT ints `centered`, `sheet`.
    - `ChromeColorTokens`: `QColor bannerFill, bannerText, bannerGlyph, toastFill, toastText`, each
      `NOTIFY changed`; it re-emits `changed` on the provider's `paletteRevisionChanged`. A `Palette*`
      source reads `QGuiApplication::palette()`, `Status` goes through `statusColor`, `Scheme` reads
      `ChromeInputs::scheme`, and `alphaPercent` applies last.
  - `class window::ChromeStyleHost` (not a QObject):
    - `ChromeStyleHost(QQmlContext& context, ChromeInputs inputs);` — builds the provider and sets the
      `chromeStyle` context property.
    - `void update(ChromeInputs inputs);` — no-op when `inputs` equal the current ones (compare every
      field); otherwise builds a new provider, sets the context property, and `deleteLater()`s the old one.
    - `[[nodiscard]] UiStyleProvider& provider() noexcept;` `[[nodiscard]] ChromeInputs const& inputs() const noexcept;`
  - `ContourGuiApp`:
    - constructor parameter `platform::AppearanceServices appearanceServices = {}` inserted before
      `processStart`; members `std::unique_ptr<platform::IAccessibilityPreferenceSource> _accessibilitySource`
      and `std::unique_ptr<platform::IPlatformAppearance> _platformAppearance`, declared **after**
      `_sessionManager` so they die first, built from the factories with callbacks to the two handlers below.
    - `[[nodiscard]] config::AccessibilityPreferences accessibilityPreferences() const;`
    - `[[nodiscard]] platform::PlatformAppearance platformAppearance() const;`
    - `void refreshChromeStyle();` — `_chromeStyleHost->update(chromeInputs())` when the host exists
      (guarded as `applyWindowControlStyle` is at `ContourGuiApp.cpp:1729`).
    - private `[[nodiscard]] window::ChromeInputs chromeInputs() const;` — row `_chromeStyle`, font
      `resolveChromeFont(_config, _chromeStyle, profileName())`, the two sources' `current()`, and
      `chromeSchemeColorsOf(*config::preferredColorPalette(defaultProfileColors(), _colorPreference))`
      (default palette when that is null).
    - private `void onAccessibilityPreferencesChanged(config::AccessibilityPreferences preferences);` and
      `void onPlatformAppearanceChanged(platform::PlatformAppearance appearance);` — each calls
      `refreshChromeStyle()` in this task. Task 9 extends the first.

- [ ] **Step 1: Write the failing tests.**
  - `UiStyleProvider_test.cpp`, tag `[uistyle]`:
    - "UiStyleProvider publishes the 1a token groups per row": a provider over
      `chromeInputsFor(MacOS, font)` has `size.toast == 36.0`, `radii.toast == 18.0`,
      `motion.standard == 160`; one over `Terminal` has `size.toast == provider.cellHeight()` and
      `radii.toast == 0.0` (read the groups through `property("size").value<QObject*>()->property("toast")`).
    - "UiStyleProvider applies Reduce Motion": `auto inputs = chromeInputsFor(MacOS, font);
      inputs.accessibility.motion = MotionPreference::Reduced;` (C++ has no nested designators;
      `ChromeInputs` is a value type, so assigning before use is not post-construction configuration)
      gives `motion.fast == motion.standard == motion.emphasized == 100` and `motion.travel == 0`.
    - "UiStyleProvider takes Running from the platform accent": with
      `inputs.appearance.accent = 0xFF00AA_rgb`, `statusColor(statusRunning())` and `accentColor()` are
      `#FF00AA`; with none, they equal `QGuiApplication::palette().color(QPalette::Highlight)`.
    - "UiStyleProvider resolves the colour roles": under `Terminal` with a scheme whose Bar slot is
      `0x123456`, `colors.bannerFill == #123456`; under `MacOS`, `colors.toastFill` is `#28282C` at alpha 0.92.
  - `UiStyleProvider_test.cpp`, tag `[chromehost]`:
    - "ChromeStyleHost republishes chromeStyle and keeps the old provider alive for the event": a
      `QQmlEngine`, an inline component `Item { property int d: chromeStyle.motion.standard }`, a host over
      `Full`; `d == 160`; `host.update(inputs with Reduced)`; `d == 100`; a `QPointer` to the first
      provider is still non-null right after `update`, and null after
      `QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`; `QmlMessageCapture` reports
      no diagnostic. `update` with equal inputs leaves `&host.provider()` unchanged.
  - `ContourGuiApp_test.cpp`, tag `[appearance]`:
    - "ContourGuiApp reads accessibility through its injected source": a `TestApp` built with
      `appearanceServicesWith(sw, aw)` and `sw.preferences.motion = Reduced` before construction reports
      `app().accessibilityPreferences().motion == Reduced`; after `sw.set({ … Full … })` it reports `Full`.
    - "A default TestApp never reads the host's accessibility settings": `TestApp {}` reports
      `config::AccessibilityPreferences {}` (Review Focus 1).

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: compile failure — `ChromeInputs`, `ChromeStyleHost` and the `TestApp` parameter do not exist.

- [ ] **Step 3: Implement** as listed. `UiStyleProvider` keeps its existing properties and event filter.
  Where `applyGuiTheme` re-runs on reload (`TerminalSessionManager.cpp:741`) and on the OS light/dark
  flip (`ContourGuiApp.cpp:2111`), add `refreshChromeStyle()`.

- [ ] **Step 4: Run them, and every suite that loads `chromeStyle`.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[uistyle],[chromehost],[appearance],[mainwindow],[qml],[a11y],[contour][gui][style]"`

Expected: PASS. The Terminal (TUI) row's rendering for this task is the `Terminal` provider case:
`size.toast` resolves to one cell row and the banner fill to the scheme's palette[0].

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/window/ChromeTokenObjects.* src/contour/window/ChromeStyleHost.* src/contour/window/UiStyleProvider.* src/contour/test/QmlChromeStyle.hpp src/contour/test/GuiTestFixtures.hpp src/contour/ContourGuiApp.* src/contour/main.cpp src/contour/session/TerminalSessionManager.cpp src/contour/window/UiStyleProvider_test.cpp src/contour/test/ContourGuiApp_test.cpp
git add src/contour
git commit -m "contour: publish the token groups on chromeStyle, and rebuild it when the OS appearance changes"
```

---

### Task 4: The size-literal gate

**Files:**
- Create: `scripts/qml_lint.py` — `code_lines()` moved verbatim out of `scripts/check-qml-colors.py:38-79`,
  so both gates share one comment/string scanner.
- Modify: `scripts/check-qml-colors.py` — import `code_lines` from `qml_lint` (insert the script's own
  directory at the front of `sys.path`); its self-test still passes unchanged.
- Create: `scripts/check-qml-sizes.py`, the sibling gate, importing the same scanner.
- Create: `scripts/qml-size-literal-baseline.txt`.
- Modify: `CMakeLists.txt` — register `check_qml_sizes` and `check_qml_sizes_selftest` with
  `contour_add_lint_gate` beside the colour gates (`:275-279`, registered at `:278-279`), and add
  `check_qml_sizes` to the warning text at `:287-289`.

**The rule.**
- Scans `src/contour/qml/**/*.qml` and `src/contour/styles/**/*.qml`, outside comments. `code_lines()`
  keeps string contents, so the property is matched as an assignment target, never anywhere in the line.
- An assignment is an offence when it assigns one of the **size properties** and its value contains a
  numeric literal other than `0` — an arithmetic multiplier (`2 * chromeStyle.space.s3`) included. A
  property is matched as `(^|[{;])\s*(<prop>)\s*:`, so `foo { width: 120; height: 4 }` on one line is
  two assignments. The properties are a table at the top of the script: `width`, `height`,
  `implicitWidth`, `implicitHeight`, `radius`, `spacing`, `padding`, `leftPadding`, `rightPadding`,
  `topPadding`, `bottomPadding`, `horizontalPadding`, `verticalPadding`, `leftInset`, `rightInset`,
  `topInset`, `bottomInset`, `anchors.margins`, `anchors.leftMargin`, `anchors.rightMargin`,
  `anchors.topMargin`, `anchors.bottomMargin`, `Layout.preferredWidth`, `Layout.preferredHeight`,
  `Layout.minimumWidth`, `Layout.minimumHeight`, `Layout.maximumWidth`, `Layout.maximumHeight`,
  `Layout.margins`, `Layout.leftMargin`, `Layout.rightMargin`, `Layout.topMargin`, `Layout.bottomMargin`,
  `font.pixelSize`, `font.pointSize`, `border.width`, `duration`.
- The grouped forms are in the table too, scoped to their group: inside `anchors { … }` the names
  `margins`, `leftMargin`, `rightMargin`, `topMargin`, `bottomMargin`; inside `Layout { … }` the
  `Layout.*` names above without the prefix; inside `font { … }` `pixelSize` and `pointSize`; inside
  `border { … }` `width`.
- `property <type> <name>: N` declarations are left out of 1a; the gate does not look at them.
- A numeric literal is a token `(?<![\w.])\d+(?:\.\d+)?(?![\w.])`, so `chromeStyle.space.s6` and
  `elevation.e2` are not literals. The gate counts **literals, not lines**: `Math.max(a, 8) + 8` is two.
- **Ratchet baseline.** `qml-size-literal-baseline.txt` holds `path count` lines, each path
  repository-relative with `/` separators (`Path.relative_to(REPO_ROOT).as_posix()`, as the colour gate
  writes its allowlist paths), so the gate gives the same answer on Windows. A file must have
  exactly its baseline count: more fails ("N new size literal(s)"), fewer also fails ("lower the
  baseline of <path> to N"), so the baseline only shrinks. A file not listed has baseline 0, which is
  what keeps every new component literal-free.
- `--print-baseline` prints the current counts in the file's format; `--self-test` runs inline samples.

- [ ] **Step 1: Write the self-test cases first,** in the script's `self_test()`:
  - offences: `width: 120`, `anchors.leftMargin: 8`, `implicitHeight: Math.max(a, b) + 8`,
    `implicitHeight: label.implicitHeight + 2 * chromeStyle.space.s3`, `font.pointSize: 14`,
    `duration: 160`, `radius: 4.5`, `Layout.maximumWidth: 360`;
  - counted per literal: `foo { width: 120; height: 4 }` is two; `implicitWidth: Math.max(a, 8) + 8` is two;
  - grouped forms: `anchors { margins: 8 }` and `font { pixelSize: 12 }` are offences; `margins: 8`
    outside any of the four groups is not;
  - not offences: `height: visible ? implicitHeight : 0`, `width: chromeStyle.size.toast`,
    `anchors.margins: chromeStyle.space.s6`, `opacity: 0.5`, `z: 11`, `// width: 120`,
    `text: "width: 120"`, `property real gap: 8` (out of 1a's scope);
  - baseline: a file at its count passes; one above fails; one below fails with the lower-it message; an
    unlisted file with one literal fails; `--print-baseline` writes `src/contour/qml/a.qml`-style paths
    with `/` separators.

- [ ] **Step 2: Run the self-test and see it fail** (the scanner body is a stub returning no offences).

Run: `python3 scripts/check-qml-sizes.py --self-test`

Expected: FAIL, listing the offence cases.

- [ ] **Step 3: Implement the scanner,** then write the baseline from today's tree:
  `python3 scripts/check-qml-sizes.py --print-baseline > scripts/qml-size-literal-baseline.txt`, with a
  `#` header saying the file only shrinks and how to regenerate it after removing literals.

- [ ] **Step 4: Run both gates.**

Run: `cmake out/appleclang-debug >/dev/null && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)" --output-on-failure`

(The reconfigure registers the two new gates.)

Expected: PASS for `check_qml_sizes`, `check_qml_sizes_selftest` and both colour gates.

- [ ] **Step 5: Check and commit.**

```bash
editorconfig-checker scripts/qml_lint.py scripts/check-qml-colors.py scripts/check-qml-sizes.py scripts/qml-size-literal-baseline.txt CMakeLists.txt
git add scripts/qml_lint.py scripts/check-qml-colors.py scripts/check-qml-sizes.py scripts/qml-size-literal-baseline.txt CMakeLists.txt
git commit -m "lint: hold numeric size literals in the chrome QML to a baseline that only shrinks"
```

---

### Task 5: `Banner.qml`

**Files:**
- Create: `src/contour/qml/Banner.qml`.
- Modify: `src/contour/CMakeLists.txt:370-405` — list `qml/Banner.qml`.
- Test: `src/contour/test/QmlComponents_test.cpp` (new cases at the end), and
  `src/contour/test/Accessibility_qml_test.cpp` (a new table-driven case).

**Interfaces:**
- Consumes: `chromeStyle.colors.bannerFill/bannerText/bannerGlyph`, `chromeStyle.space.s3/s5/s6`,
  `chromeStyle.type.footnote`, `chromeStyle.borderWidth`, `chromeStyle.contrast.separatorAlpha` (Task 3).
- Produces — `Banner` (a full-width band; its user places it), built on a Qt Quick Controls `Control`:
  - `property string text`, `property string glyph: "!"`, `property string actionText` (empty: no action
    button), `property string dismissText: qsTr("Dismiss")`;
  - `signal actionTriggered()`, `signal dismissed()`;
  - `visible: text.length > 0`; `topPadding`/`bottomPadding: chromeStyle.space.s3`,
    `leftPadding`/`rightPadding: chromeStyle.space.s6`; the `RowLayout` below is its `contentItem`, so
    its height is the content's plus the padding, with no multiplier written
    (`implicitHeight: implicitContentHeight + topPadding + bottomPadding`, bound explicitly so it does
    not hang on the active style's `Control`); `height: visible ? implicitHeight : 0`;
  - children by `objectName`: `bannerGlyph`, `bannerText`, `bannerAction`, `bannerDismiss`;
  - a bottom separator `chromeStyle.borderWidth` high in `bannerText`'s colour at
    `contrast.separatorAlpha`;
  - `Accessible.role: Accessible.AlertMessage`, `Accessible.name: text`; each button names itself.
- Layout: glyph, text (word-wrapped, fills), action, dismiss in a `RowLayout` with spacing `space.s5`;
  the side insets are the `space.s6` padding. Fill (the `Control`'s `background`) `colors.bannerFill`,
  text `colors.bannerText`, glyph `colors.bannerGlyph`. No literal number anywhere, and no arithmetic
  literal either (Task 4's gate).
- **TUI row:** under the Terminal tokens `space.s3` is 0 and the scheme slots apply, so the band is the
  content's height in the scheme's palette[0] — one row under ContourTui controls.

- [ ] **Step 1: Write the failing tests.**
  - `QmlComponents_test.cpp`, tags `[contour][gui][qml][kit][banner]`:
    - "Banner: shows its text, and its buttons emit (offscreen)" — load `Banner.qml` with
      `text = "Contour has new defaults"`, `actionText = "Use New"`, `dismissText = "Keep Mine"`; it is
      visible; `bannerText.text` matches; invoking `clicked` on `bannerAction` and `bannerDismiss` emits
      `actionTriggered` and `dismissed` once each (`QSignalSpy`); with `actionText = ""` the action button
      is invisible; with `text = ""` the banner is invisible and `height == 0`.
    - "Banner: the Terminal row is the content's height in the scheme's palette[0] (offscreen)" — install
      `ChromeSetup` for `ChromeStyle::Terminal` with a scheme whose Bar is `0x2A2D35`; the banner's
      `background`'s `color == #2A2D35` and `height == implicitHeight` equals the content row's `implicitHeight`
      (`space.s3 == 0`); under `MacOS` its height exceeds the content row by `topPadding + bottomPadding`,
      each equal to `space.s3`.
    - No QML diagnostics in either.
  - `Accessibility_qml_test.cpp`, `[contour][gui][qml][a11y]`: "accessibility: every kit primitive declares
    a role and a name" — a table of `{ url, initialProperties }` rows; this task adds the `Banner.qml` row.
    For each, the attached object exists, `role` is valid, and `name` is non-empty.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[banner],[a11y]"`

Expected: FAIL — `Banner.qml` is not in the module (component error).

- [ ] **Step 3: Implement `Banner.qml`.**

- [ ] **Step 4: Run them and the lint.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[banner],[a11y]" && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$" --output-on-failure`

Expected: PASS. The Terminal row case is this task's TUI rendering.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/Banner.qml src/contour/CMakeLists.txt
git add src/contour/qml/Banner.qml src/contour/CMakeLists.txt src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
git commit -m "contour: add the kit's Banner"
```

---

### Task 6: `Toast.qml`, and its snapshot script

**Files:**
- Create: `src/contour/qml/Toast.qml`; list it in `src/contour/CMakeLists.txt:370-405`.
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp:35-43` (`SnapshotScript` gains `Toast`), `:60-64`
  (`ScriptAction` gains `ShowToast`), `:87-136` (a `toast` row), `:143` (the count `static_assert` names
  the new last enumerator).
- Modify: `src/contour/test/ChromeSnapshotScene.hpp:187-205` — `show()` handles `ShowToast`; and
  `load()` (`:326-339`) sets the `ChromeSetup`'s `.scheme` from the variant, so every Terminal-row
  snapshot paints the toast and banners in the variant's scheme rather than `ColorPalette {}`'s.
- Test: `QmlComponents_test.cpp`, `Accessibility_qml_test.cpp` (one row), `ChromeSnapshotPlan_test.cpp:19`,
  `:60`, `:63` (counts) and `:62` (the last file name, rewritten to derive from the table),
  `ChromeSnapshot_test.cpp:60-91` (a `case`).

**Interfaces:**
- Consumes: `chromeStyle.size.toast`, `radii.toast`, `space.s5/s7`, `type.footnote`, `motion.standard`,
  `motion.travel`, `elevation.e2`, `tui.toastStyle`/`tui.inverse`, `colors.toastFill/toastText`.
- Produces — `Toast` (presentational; its host positions it and decides when it shows), built on a Qt
  Quick Controls `Control`:
  - `property string text`, `property string glyph` (optional, leading);
  - `property bool shown` — QML-facing, the carve-out for a QML property; drives `opacity` (0/1) through
    a `Behavior` of `motion.standard` ms and a `y` offset of `motion.travel` that settles to 0;
  - `height: chromeStyle.size.toast`; its `background` has `radius: chromeStyle.radii.toast`;
    `leftPadding`/`rightPadding: chromeStyle.space.s7`, and the width is the content's plus that padding
    (`implicitWidth: implicitContentWidth + leftPadding + rightPadding`) — no multiplier written, and no
    arithmetic literal either (Task 4's gate);
  - its shadow is a `PopupSurface`-style stack driven by `elevation.e2` (no shadow when `blur` and both
    offsets are 0);
  - children `toastText`, `toastGlyph`; `Accessible.role: Accessible.AlertMessage`, `Accessible.name: text`.
- `SnapshotScript::Toast`, row `{ .script = SnapshotScript::Toast, .name = "toast", .tabCount = 1, .panes = PaneShape::SingleLeaf, .page = ContentPage::Terminal, .action = ScriptAction::ShowToast, .connection = HostConnection::Local, .hostLabel = "" }`.
- The scene creates `qrc:/qt/qml/Contour/Ui/Toast.qml` with `text = "Copied 3 lines"`, `shown = true`,
  parented to `window().contentItem()`, centred horizontally at the content's top; it owns it in a new
  `std::unique_ptr<QObject> _overlay` declared before `_root` dies (after `_root` in the member list, so
  it is destroyed first).
- The scene's `ChromeSetup` gains
  `.scheme = config::chromeSchemeColorsOf(*config::preferredColorPalette(config::defaultColors(variant.edition), variant.appearance))`
  (§2.1 TUI row: "every colour role comes from the active terminal scheme").
- **TUI row:** `size.toast` is one row, `radii.toast` 0, `elevation.e2` the hard one-cell offset, and the
  colours are the scheme's foreground on its background — the one-row inverse cell strip.

- [ ] **Step 1: Write the failing tests.**
  - `QmlComponents_test.cpp`, `[contour][gui][qml][kit][toast]`:
    - "Toast: a pill of the toast tokens (offscreen)": under `MacOS`, `height == 36`, the background's
      `radius == 18`, `toastText.text == "Copied 3 lines"`; after `shown = true`,
      `QTest::qWaitFor([&] { return toast->property("opacity").toReal() == 1.0; }, 2000)` succeeds (as
      `QmlComponents_test.cpp:2312` waits, not a fixed `qWait`).
    - "Toast: the Terminal row is a one-row inverse strip (offscreen)": under `Terminal` with scheme
      Foreground `0xD9DCE3`, Background `0x1C1E24`: `height == cellHeight`, the background's `radius == 0`
      and `color == #D9DCE3`, `toastText.color == #1C1E24`.
    - "Toast: Reduce Motion fades without travel (offscreen)": with a setup built as
      `auto setup = ChromeSetup { … }; setup.accessibility.motion = MotionPreference::Reduced;` (no nested
      designators), the `y` animation's distance is 0 (read `motion.travel` through the toast's bound property
      `enterOffset`, which it exposes as `readonly property real enterOffset: chromeStyle.motion.travel`).
  - `Accessibility_qml_test.cpp`: add the `Toast.qml` row.
  - `ChromeSnapshotPlan_test.cpp`: `SnapshotScripts.size() == 7`, matrix `112`, ContourTui `28`; `:62`
    becomes `CHECK(snapshotFileName(matrix.back()) == std::format("{}.terminal.native2026.light.png", SnapshotScripts.back().name));`,
    so later rows need no edit there.
  - `ChromeSnapshot_test.cpp`: `case SnapshotScript::Toast:` finds `toastText` under the window's content
    item and checks its text.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test contour_test && out/appleclang-debug/src/contour/contour_gui_test "[toast],[a11y],[snapshot]"`

Expected: compile failure on `SnapshotScript::Toast`.

- [ ] **Step 3: Implement** `Toast.qml`, the script row and the scene action.

- [ ] **Step 4: Run them.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[toast],[a11y],[snapshot]" && out/appleclang-debug/src/contour/contour_test "[snapshot]" && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$"`

Expected: PASS; the offscreen snapshot case builds the `toast` script on the macOS and Terminal rows.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshotPlan.hpp src/contour/test/ChromeSnapshotScene.hpp src/contour/test/ChromeSnapshotPlan_test.cpp src/contour/test/ChromeSnapshot_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/Toast.qml src/contour/CMakeLists.txt
git add src/contour/qml/Toast.qml src/contour/CMakeLists.txt src/contour/test
git commit -m "contour: add the kit's Toast, with a snapshot script"
```

---

### Task 7: `KitDialog.qml`, and its snapshot script

**Files:**
- Create: `src/contour/qml/KitDialog.qml`; list it in `src/contour/CMakeLists.txt:370-405`.
- Modify: `ChromeSnapshotPlan.hpp` (`SnapshotScript::KitDialog`, `ScriptAction::OpenKitDialog`, a
  `kit-dialog` row, the count assert), `ChromeSnapshotScene.hpp` (`show()` handles it). The last-name
  check at `ChromeSnapshotPlan_test.cpp:62` derives from the table since Task 6 and needs no edit.
- Test: `QmlComponents_test.cpp`, `Accessibility_qml_test.cpp` (one row), `ChromeSnapshotPlan_test.cpp`
  (8 scripts, 128, 32), `ChromeSnapshot_test.cpp` (a `case`).

**Interfaces:**
- Consumes: `chromeStyle.size.dialogWidth`, `space.s4/s8`, `type.body` (the heading, bold),
  `type.footnote` (the message), `layout.dialog`/`layout.sheet`, `PopupSurface` (`qml/PopupSurface.qml`, which already
  draws the Terminal row's hard shadow), `statusColor`/`onStatusColor`.
- Produces — `KitDialog`, a `Dialog` (QtQuick.Controls, never QtQuick.Dialogs; see `ConfirmDialog.qml:4-7`):
  - `property string heading`, `property string message`, `property string acceptText: qsTr("OK")`,
    `property string rejectText: qsTr("Cancel")`, `property int acceptRole: chromeStyle.statusRunning`
    (a `StatusColorRole` value; `statusDanger` makes the accept button destructive);
  - `modal: true`, `closePolicy: Popup.CloseOnEscape`, `background: PopupSurface {}`,
    `width: chromeStyle.size.dialogWidth`. `PopupSurface` draws its corner and its shadow's corner from
    the legacy `chromeStyle.radius` (`PopupSurface.qml:49`, `:75`), not `radii.popover`; accepted for 1a,
    since the legacy radius is a token, and `PopupSurface` is the first reader to migrate when `radius`
    is retired (R-a);
  - placement: `x` centred in `Overlay.overlay`; `y` is 0 (a sheet attached to the window's top edge)
    when `layout.dialog === layout.sheet`, else centred;
  - children `kitDialogHeading`, `kitDialogMessage`, `kitDialogAccept`, `kitDialogReject`; the accept
    button's fill is `statusColor(acceptRole)` and its text `onStatusColor(acceptRole)`, read through
    `paletteRevision` as `ConfirmDialog.qml:79` does;
  - `Accessible.role: Accessible.Dialog`, `Accessible.name: heading`.
- `SnapshotScript::KitDialog` row `kit-dialog` with `ScriptAction::OpenKitDialog`; the scene creates the
  dialog parented to the window's overlay with heading "Close this tab?", message "“cmake” is still
  running. Closing the tab will end it.", accept "Close Tab", reject "Cancel", `acceptRole = statusDanger`,
  opens it, and waits for `opened` as the palette action does (`ChromeSnapshotScene.hpp:194-199`).
- **TUI row:** `dialogWidth` is 32 cells, `PopupSurface` draws the boxed popup with the hard one-cell
  shadow, and placement is centred.

- [ ] **Step 1: Write the failing tests.**
  - `QmlComponents_test.cpp`, `[contour][gui][qml][kit][dialog]`:
    - "KitDialog: a sheet on the macOS row, centred elsewhere (offscreen)": open it in a 1024 × 640 window
      overlay; under `MacOS` `y == 0`; under `ContourLinux` and `Terminal`,
      `y == (overlay.height - height) / 2` within 1 px; `width == size.dialogWidth`.
    - "KitDialog: accept and reject close it and emit (offscreen)": clicking `kitDialogAccept` emits
      `accepted` and closes; clicking `kitDialogReject` emits `rejected`.
    - "KitDialog: a destructive accept is painted in the danger colour (offscreen)": with
      `acceptRole = statusDanger`, the accept button's background colour equals
      `provider.statusColor(statusDanger())`.
  - Accessibility row, plan and scene counts as listed.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test contour_test && out/appleclang-debug/src/contour/contour_gui_test "[dialog],[a11y],[snapshot]"`

Expected: compile failure on `SnapshotScript::KitDialog`.

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run them.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[dialog],[a11y],[snapshot]" && out/appleclang-debug/src/contour/contour_test "[snapshot]" && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$"`

Expected: PASS. The `Terminal` placement case and the `kit-dialog` Terminal scene are the TUI rendering.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/test/ChromeSnapshot*.hpp src/contour/test/ChromeSnapshot*_test.cpp src/contour/test/QmlComponents_test.cpp src/contour/test/Accessibility_qml_test.cpp
editorconfig-checker src/contour/qml/KitDialog.qml src/contour/CMakeLists.txt
git add src/contour/qml/KitDialog.qml src/contour/CMakeLists.txt src/contour/test
git commit -m "contour: add the kit's sheet-styled dialog, with a snapshot script"
```

---

### Task 8: The hosting notice becomes a Banner

**Files:**
- Modify: `src/contour/qml/Main.qml`:
  - add a `Column { id: bannerStack; objectName: "bannerStack" }` after the `TitleBar` (`:143-175`),
    anchored left/right, `y: appWindow.tabBarPosition === 0 ? titleBar.height : 0`, `z: 1`;
  - the content `Item` (`:182-192`) anchors its top to `bannerStack.bottom` in both positions; its bottom
    stays `appWindow.tabBarPosition === 1 ? titleBar.top : parent.bottom`;
  - the hosting `Rectangle` (`:228-275`, its comment included) is replaced by a `Banner` inside
    `bannerStack`, `objectName: "hostingNoticeBar"`, `text: (appWindow.win && appWindow.win.hostingNotice) || ""`,
    `onDismissed: appWindow.win.dismissHostingNotice()`. Same `WindowController.hostingNotice` /
    `dismissHostingNotice` contract; no Retry (owner decision 2).
- Modify: `src/contour/config/UiStyle.hpp:272-273` — delete `HostingNoticeFillPercent` and
  `HostingNoticeBorderPercent`.
- Modify: `src/contour/window/UiStyleProvider.hpp`/`.cpp` — delete the two `hostingNotice*Alpha`
  properties (`hostingNoticeFillAlpha`, `hostingNoticeBorderAlpha`), their accessors and their
  definitions. Find them by name: Task 3 has moved the lines (`:117-118`, `:224-228`, `.cpp:272-280` at
  the plan's base).
- Modify: `src/contour/test/MockMainController.hpp:29-44` — `MainWindowScene` gains
  `QString hostingNotice {};` (a default member initializer, so the designated initializers that omit it
  stay clean under clang 22), and the constructor (`:103-109`) seeds `_hostingNotice` from it.
- Modify: `ChromeSnapshotPlan.hpp` — `SnapshotScript::HostingBanner`;
  `enum class SnapshotBanner : uint8_t { None = 0, Hosting };` and a `banner` column on
  `SnapshotScriptRow` (every existing row gets `.banner = SnapshotBanner::None`), plus a `hosting-banner`
  row; `ChromeSnapshotScene.hpp:168-175` names `.hostingNotice = …` from it.
- Modify: `scripts/qml-size-literal-baseline.txt` — lower `Main.qml`'s count (the removed bar held five).
- Test: `src/contour/test/MainWindowQml_test.cpp:381-414` (rewritten), `UiStyleProvider_test.cpp:334-346`
  (drop the two notice checks), snapshot tests (9 scripts, 144, 36).

**Interfaces:**
- Consumes: `Banner` (Task 5).
- Produces: `bannerStack` (Main.qml) — Task 11 adds the second banner into it.
  `SnapshotBanner` — Task 11 adds `NewDefaults`.

- [ ] **Step 1: Write the failing tests** in `MainWindowQml_test.cpp`, `[contour][gui][qml][mainwindow][hosting]`:
  - "Main.qml hosting notice: a Banner that pushes the content down and gives the room back (offscreen)":
    - before: `hostingNoticeBar` invisible, `content.y == titleBar.height`;
    - `controller.setHostingNotice(notice)`: the bar is visible, `bannerText` under it (scoped
      `bar->findChild`) has the text, `content.y == titleBar.height + bar.height`, and `content.height`
      dropped by `bar.height`;
    - clicking `bannerDismiss` empties `controller.hostingNotice()`, hides the bar, and restores
      `content.y` and `content.height` exactly (Review Focus 5);
    - no QML diagnostics.
  - "Main.qml hosting notice: with the tab bar at the bottom the banner is at the top (offscreen)":
    `setTabBarPosition(1)` before loading; with the notice up, `bannerStack.y == 0`,
    `content.y == bar.height`, and `titleBar.y + titleBar.height == window height`.
  - "Main.qml hosting notice: the Terminal row's banner is the scheme's palette[0] (offscreen)": load
    with a Terminal `ChromeSetup`; the bar's `background`'s `color` equals the setup's scheme Bar colour.
  - Snapshot: `case SnapshotScript::HostingBanner:` asserts `hostingNoticeBar` is visible.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test contour_test && out/appleclang-debug/src/contour/contour_gui_test "[hosting]"`

Expected: compile failure on `SnapshotScript::HostingBanner` (Step 1 adds its `case`).

- [ ] **Step 3: Implement** the Main.qml change, the deletions and the snapshot row; update the baseline.

- [ ] **Step 4: Run them, plus the geometry and provider suites.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[mainwindow],[uistyle],[snapshot]" && out/appleclang-debug/src/contour/contour_test "[snapshot]" && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$"`

Expected: PASS, with every pre-existing `[mainwindow]` geometry assertion unchanged.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/UiStyle.hpp src/contour/window/UiStyleProvider.* src/contour/test/MockMainController.hpp src/contour/test/ChromeSnapshot*.hpp src/contour/test/*_test.cpp src/contour/window/UiStyleProvider_test.cpp
editorconfig-checker src/contour/qml/Main.qml scripts/qml-size-literal-baseline.txt
git add src/contour scripts/qml-size-literal-baseline.txt
git commit -m "contour: show the hosting notice as a Banner that pushes the content down"
```

---

### Task 9: Reduce Motion for the terminal's own durations

**Files:**
- Modify: `src/contour/config/Config.hpp` (beside `sessionSettings`, `:3280-3284`) and `Config.cpp`.
- Modify: `src/contour/session/TerminalSession.cpp:388` (construction), `:5925` (`configureTerminal`), and
  add `applyMotionPreference`; `TerminalSession.hpp` declares it.
- Modify: `src/contour/session/TerminalSessionManager.hpp`/`.cpp` — `updateMotionPreference`, beside
  `updateColorPreference` (`.cpp:1137-1141`).
- Modify: `src/contour/ContourGuiApp.cpp` — `onAccessibilityPreferencesChanged` (Task 3) also calls
  `sessionsManager().updateMotionPreference(preferences.motion)`.
- Modify: `src/contour/test/GuiTestFixtures.hpp` — `ConfiguredTestApp` forwards an `AppearanceServices`
  to `TestApp` (below).
- Test: `src/contour/config/Config_test.cpp`, `src/contour/session/TerminalSession_test.cpp`.

**Interfaces:**
- Consumes: `config::MotionPreference`; `ContourGuiApp::accessibilityPreferences()`; `AccessibilitySwitch`,
  `appearanceServicesWith` (Tasks 2–3).
- Produces:
  - `inline constexpr auto MotionSettings = std::array { &vtbackend::Settings::screenTransitionDuration, &vtbackend::Settings::cursorMotionAnimationDuration };`
    — the durations Reduce Motion zeroes; a new one is a new entry.
  - `[[nodiscard]] vtbackend::Settings motionAdjusted(vtbackend::Settings settings, MotionPreference motion);`
    — under `Reduced` every `MotionSettings` member becomes `0ms`; under `Full` `settings` is returned unchanged.
  - `void TerminalSession::applyMotionPreference(config::MotionPreference motion);` — recomputes
    `motionAdjusted(sessionSettings(_config, _profile, _currentColorPreference, std::nullopt), motion)` and,
    under `std::scoped_lock { _terminal }`, assigns each `MotionSettings` member into `_terminal.settings()`.
  - `void TerminalSessionManager::updateMotionPreference(config::MotionPreference motion);` — every session.
  - Both `sessionSettings` call sites in `TerminalSession` wrap their result in
    `config::motionAdjusted(…, _app.accessibilityPreferences().motion)` (R-b).

- [ ] **Step 1: Write the failing tests.**
  - `Config_test.cpp`, `[config][motion]`: "motionAdjusted zeroes the motion durations under Reduce Motion
    and nothing else": settings with 150 ms / 80 ms and `smoothScrolling = true` → under `Reduced` both
    durations are `0ms` and `smoothScrolling` is still true; under `Full` the result equals the input
    field by field for every `MotionSettings` member.
  - `TerminalSession_test.cpp`, `[contour][session][motion]`:
    - "A session born under Reduce Motion has no glide and no transition": a `TestApp` built with
      `appearanceServicesWith(sw, aw)`, `sw.preferences.motion = Reduced`;
      `makeDisplaylessSession(testApp.app())`; `terminal().settings().screenTransitionDuration == 0ms` and
      `cursorMotionAnimationDuration == 0ms`.
    - "Turning Reduce Motion on and off follows live": the session is created through the manager, so
      `updateMotionPreference` reaches it — `contour::test::ScopedController const win { testApp.manager() };
      auto* session = testApp.manager().createSession(win.id);` as `TerminalSessionManager_test.cpp:516`
      does (`makeDisplaylessSession` builds a session the manager never registers: `_sessionsById` is
      filled only at `TerminalSessionManager.cpp:324`). Born under `Full` (Classic profile values,
      250 ms / 80 ms per `Config.hpp:1086` and `:1227`); `sw.set(Reduced)` → both 0 ms; `sw.set(Full)` →
      250 ms and 80 ms again.
    - "A configuration reload keeps Reduce Motion": born under `Reduced` in a `ConfiguredTestApp`-style
      setup; reload with `screen_transition_duration: 333` (as `TerminalSession_test.cpp:2421-2431`
      does); the duration stays 0 ms.
      (`ConfiguredTestApp` gains a constructor parameter `contour::platform::AppearanceServices` forwarded
      to `TestApp`, defaulting to `nullAppearanceServices()`.)

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_gui_test`

Expected: compile failure — `motionAdjusted` and `updateMotionPreference` do not exist.

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run them, and the existing reload case they sit beside.**

Run: `out/appleclang-debug/src/contour/contour_gui_test "[motion],[contour][session][config],[appearance]"`

Expected: PASS. No visual change; no TUI row rendering applies.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/session/TerminalSession.* src/contour/session/TerminalSessionManager.* src/contour/ContourGuiApp.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp src/contour/test/GuiTestFixtures.hpp
git add src/contour
git commit -m "contour: honour Reduce Motion in the cursor glide and the screen transition"
```

---

### Task 10: The chrome follows a background changed by OSC 11 (Phase 0 follow-up R8)

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp:706-714` (a new `Events` hook) and `:785-786` (the null
  events), plus a `Terminal` forwarder beside `:1672-1674`.
- Modify: `src/vtbackend/screen/Screen.cpp:3423-3474` — `resetDynamicColor` and `setDynamicColor` call the
  forwarder in their `DefaultBackgroundColor` case when the colour actually changes (DECAC item 1 goes
  through them, `:8097-8106`).
- Modify: `src/vtbackend/screen/Terminal.cpp` — the two palette funnels notify, each comparing
  `defaultBackground` before and after rather than each caller notifying:
  - `Terminal::setColorPalette` (`:8779`), which `resetColorPalette()` (`Terminal.hpp:2303`) and so DECSTR
    (`:7235`) and RIS (`:7422`) reach, and which XTPOPCOLORS (`popColorPalette`, `:8846`) calls;
  - `Terminal::resetColorPalette(ColorPalette const&)` (`:8784`), the GUI re-seed
    (`TerminalSession.cpp:1291`, `:5979`).
- Modify: `src/vtbackend/testing/MockTerm.hpp:290-332` — record it.
- Modify: `src/contour/platform/GuiTheme.hpp:43-58` — `qtColorSchemeFor` gains a live background.
- Modify: `src/contour/ContourGuiApp.hpp:459` / `.cpp:1660` — `applyGuiTheme` passes it through.
- Modify: `src/contour/session/TerminalSession.hpp:935-936`/`.cpp:2114-2132` — override the hook (post to
  the manager, as `setWindowFrameColor` does) and add `liveBackground()`.
- Modify: `src/contour/session/TerminalSessionManager.hpp`/`.cpp` — `sessionBackgroundChanged`, and
  `setFocusedSession` (`.cpp:376-384`) re-evaluates when the live background differs.
- Test: `src/vtbackend/screen/Screen_osc_test.cpp`, `src/contour/platform/GuiTheme_test.cpp`,
  `src/contour/session/TerminalSessionManager_test.cpp`.

**Interfaces:**
- Produces:
  - `virtual void Terminal::Events::defaultBackgroundColorChanged(RGBColor /*color*/) {}` and
    `void Terminal::notifyDefaultBackgroundColorChanged()` — reads `colorPalette().defaultBackground` and
    forwards it. Called by whichever thread holds the terminal lock (the parser thread, or the GUI thread
    for the re-seed), and only when the default background changed; `TerminalSession`'s override posts
    to the manager either way.
  - `MockTerm`: `std::optional<RGBColor> defaultBackground;` and `int defaultBackgroundChangeCount = 0;`.
  - `[[nodiscard]] inline std::optional<Qt::ColorScheme> qtColorSchemeFor(config::GuiTheme theme, config::ColorConfig const& colors, std::optional<vtbackend::RGBColor> liveBackground) noexcept;`
    — `Terminal` over a simple config uses `liveBackground` when set, else the config's background; every
    other case as today (R-h). The two-argument form becomes a delegate passing `std::nullopt`.
  - `void ContourGuiApp::applyGuiTheme(config::GuiTheme theme, config::ColorConfig const& colors, std::optional<vtbackend::RGBColor> liveBackground);`
    with the existing two-argument overload delegating with `std::nullopt`.
  - `[[nodiscard]] vtbackend::RGBColor TerminalSession::liveBackground() const;` — under the terminal lock.
  - `void TerminalSessionManager::sessionBackgroundChanged(vtworkspace::SessionId id, vtbackend::RGBColor color);`
    — when that session is `_focusedSession`, `_app.applyGuiTheme(theme, defaultProfileColors(), color)`;
    otherwise nothing (Review Focus 4).
  - `setFocusedSession`: when the next session is non-null and either there was no previous one or the two
    `liveBackground()`s differ, re-apply with the next one's; focusing nothing changes nothing.

- [ ] **Step 1: Write the failing tests.**
  - `Screen_osc_test.cpp`, `[screen][osc][background-event]`: "OSC 11, OSC 111, DECAC 1, DECSTR and RIS
    announce the default background", a table of `{ input, count, colour }` rows applied in order:
    `\033]11;rgb:1010/1010/1010\033\\` → count 1, colour `#101010`; `\033]111\033\\` → count 2, colour =
    the default palette's; `\033[1;7;4,|` → count 3; `\033[!p` (DECSTR) → count 4, the default palette's;
    `\033]11;rgb:2020/2020/2020\033\\` → count 5; `\033c` → count 6. An OSC 10 (foreground) does not
    change the count, and neither does a reset whose background is already the default.
  - `GuiTheme_test.cpp`, `[guitheme][chromefollow]`: `qtColorSchemeFor(Terminal, simpleBlack, 0xFAFAF8_rgb) == Light`;
    `qtColorSchemeFor(Terminal, dual, 0xFAFAF8_rgb) == std::nullopt`;
    `qtColorSchemeFor(Dark, simpleBlack, 0xFAFAF8_rgb) == Dark`.
  - `TerminalSessionManager_test.cpp`, `[manager][theme][chromefollow]`:
    - "OSC 11 in the focused session recolours a terminal-themed chrome": a `ConfiguredTestApp` whose file
      sets `theme: terminal` (Classic, so the colours are simple and black); one session, focused through
      `manager().setFocusedSession`; window lightness is dark; write
      `\033]11;rgb:fafa/fafa/f8f8\033\\` to its terminal and `QCoreApplication::processEvents()`; the
      palette's `Window` lightness rose.
    - "OSC 11 in an unfocused session leaves the chrome alone": two sessions, the second focused; the
      first receives the OSC 11; lightness unchanged. Focusing the first afterwards makes it light.
    - Both cases create their sessions through `manager().createSession(win.id)` under a
      `ScopedController`, as `TerminalSessionManager_test.cpp:516` does: an unregistered session has
      `modelSessionId() == {}`, which would make "unfocused" indistinguishable from focused.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target vtbackend_test contour_gui_test`

Expected: compile failure — `defaultBackgroundChangeCount` and the three-argument `qtColorSchemeFor` do not exist.

- [ ] **Step 3: Implement.**

- [ ] **Step 4: Run them.**

Run: `out/appleclang-debug/src/vtbackend/vtbackend_test "[background-event],Terminal.DECAC" && out/appleclang-debug/src/contour/contour_gui_test "[chromefollow],[manager][theme]"`

Expected: PASS. The chrome's light/dark is the only visual change. The Terminal row's colour roles stay
the configured scheme's (R-f); only the chrome's light/dark follows.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/testing/MockTerm.hpp src/vtbackend/screen/Screen_osc_test.cpp src/contour/platform/GuiTheme.hpp src/contour/platform/GuiTheme_test.cpp src/contour/ContourGuiApp.* src/contour/session/TerminalSession.* src/contour/session/TerminalSessionManager*
git add src/vtbackend src/contour
git commit -m "contour: let a terminal-themed chrome follow the focused session's OSC 11 background"
```

---

### Task 11: The new-defaults banner (§2.7), with the Reset Config case

**Files:**
- Modify: `src/contour/config/DefaultsEdition.hpp` — `DefaultsEditionSources` and `effectiveEdition`.
- Create: `src/contour/config/NewDefaultsOffer.hpp` — the decision table.
- Create: `src/contour/config/GuiStateStore.hpp` and `.cpp` — the GUI state file.
- Modify: `src/contour/config/Config.hpp:3139-3148` and `Config.cpp:690-712` — the scan returns its sources;
  `Config` gains `DefaultsEditionSources defaultsEditionSources {}` (beside `defaultsEdition`, `:1735`),
  set by the path overload of `loadConfigFromFile`.
- Create: `src/contour/window/NewDefaultsNotice.hpp` and `.cpp`.
- Modify: `src/contour/ContourGuiApp.hpp`/`.cpp` — build the notice after the QML engine (beside `:2252`),
  publish it as the `newDefaults` context property, and add `void onConfigurationReloaded();` (calls
  `refreshChromeStyle()` and, guarded as `refreshChromeStyle()` and `applyWindowControlStyle`
  (`ContourGuiApp.cpp:1729`) are, `if (_newDefaultsNotice) _newDefaultsNotice->refresh();` — engine-less
  `TestApp`s reload too, e.g. `[manager][theme]` at `TerminalSessionManager_test.cpp:1066`). The member
  `std::unique_ptr<window::NewDefaultsNotice> _newDefaultsNotice` is declared **before** `_qmlEngine`,
  beside `_chromeStyleHost` (Task 3's rename of `_uiStyleProvider`, `ContourGuiApp.hpp:853-856` at the
  plan's base), because the engine borrows it as a context property.
- Modify: `src/contour/session/TerminalSessionManager.cpp:741` — `onConfigurationReloaded()` replaces
  Task 3's `refreshChromeStyle()` call there.
- Modify: `src/contour/qml/Main.qml` — a second `Banner` in `bannerStack`, `objectName: "newDefaultsBanner"`,
  reading `typeof newDefaults !== "undefined" ? newDefaults : null` as `DaemonDriftBadge.qml:18` does.
- Modify: `src/contour/test/QmlMocks.hpp` — `MockNewDefaultsNotice`.
- Modify: `ChromeSnapshotPlan.hpp` (`SnapshotBanner::NewDefaults`, a `new-defaults-banner` row),
  `ChromeSnapshotScene.hpp` (owns a `MockNewDefaultsNotice` and sets `newDefaults` in `load()`,
  `:326-339`, before `loadMainWindow`).
- Modify: `src/contour/config/CMakeLists.txt` — `GuiStateStore.cpp GuiStateStore.hpp` and
  `NewDefaultsOffer.hpp` in `contour_config` (defined there, as in Task 1).
- Modify: `src/contour/CMakeLists.txt` — `config/NewDefaultsOffer_test.cpp` and
  `config/GuiStateStore_test.cpp` in `contour_test` (`:1115-1127`), `window/NewDefaultsNotice.cpp`/`.hpp`
  in `contour_core` (`:278`, `:323`), and `window/NewDefaultsNotice_test.cpp` in `contour_gui_test`.

**Interfaces:**
- Produces (namespace `contour::config`):
  - `struct DefaultsEditionSources { std::optional<DefaultsEdition> mainFile {}; std::optional<DefaultsEdition> settingsFile {}; [[nodiscard]] bool operator==(DefaultsEditionSources const&) const = default; };`
  - `[[nodiscard]] constexpr DefaultsEdition effectiveEdition(DefaultsEditionSources sources) noexcept;` —
    `settingsFile`, else `mainFile`, else `Classic`.
  - `[[nodiscard]] DefaultsEditionSources scanDefaultsEditionSources(std::filesystem::path const& configFile);`
    — today's loop at `Config.cpp:700-705`, keeping each file's statement; `scanDefaultsEdition` becomes
    `effectiveEdition(scanDefaultsEditionSources(configFile))`.
  - `enum class NewDefaultsOffer : uint8_t { None = 0, ExistingConfig, SettingsOverride };` with a
    `ConfigEnumInfo` table: `"existing_config"`, `"settings_override"` (None has no token).
  - `[[nodiscard]] constexpr NewDefaultsOffer newDefaultsOffer(DefaultsEditionSources sources) noexcept;`
  - `[[nodiscard]] constexpr bool isOfferShown(NewDefaultsOffer offer, std::span<NewDefaultsOffer const> dismissed) noexcept;`
    — `offer != None` and `offer` is not in `dismissed`.
  - `struct GuiState { std::vector<NewDefaultsOffer> dismissedOffers {}; [[nodiscard]] bool operator==(GuiState const&) const = default; };`
  - `enum class GuiStateError : uint8_t { Unreadable = 0, Malformed, Unwritable };`
  - `class GuiStateStore { public: virtual ~GuiStateStore() = default; [[nodiscard]] virtual std::expected<GuiState, GuiStateError> load() const = 0; [[nodiscard]] virtual std::expected<void, GuiStateError> save(GuiState const& state) = 0; };`
  - `inline constexpr std::string_view GuiStateFileName = "gui-state.yml";`
  - `class FileGuiStateStore final: public GuiStateStore { public: explicit FileGuiStateStore(std::filesystem::path configDir); … };`
    — a missing file loads as `GuiState {}`; the file is `dismissed_new_defaults: [existing_config]`; writes
    go through `atomicWriteFile` (`config/AtomicFileWrite.hpp`), as `FileGuiConfigStore::saveGuiSettings` does.
- Produces (namespace `contour::window`):
  - `class NewDefaultsNotice: public QObject` with
    `Q_PROPERTY(bool offered READ offered NOTIFY offeredChanged)` — QML-facing, documented carve-out;
    - `NewDefaultsNotice(std::function<config::Config const&()> config, std::shared_ptr<config::GuiConfigStore> settings, std::shared_ptr<config::GuiStateStore> state, std::function<std::expected<void, std::string>()> reload, QObject* parent = nullptr);`
    - `[[nodiscard]] bool offered() const noexcept;`
    - `[[nodiscard]] std::expected<void, std::string> switchToNewDefaults();` — copies
      `config().guiManagedSettings`, sets `globalOverrides[std::string { DefaultsEditionKey }] = "native2026"`,
      `saveGuiSettings`, then `reload()`; on an error returns it and leaves `offered` as it was.
    - `Q_INVOKABLE void useNew();` — calls it and `errorLog()`s a failure.
    - `Q_INVOKABLE void keepMine();` — appends the current offer to the stored `dismissedOffers` and saves;
      `offered` becomes false.
    - `void refresh();` — re-decides from `config().defaultsEditionSources` and the stored state; emits
      `offeredChanged` only on change.
- Produces (namespace `contour::test`): `MockNewDefaultsNotice` — `Q_PROPERTY(bool offered READ offered WRITE setOffered NOTIFY offeredChanged)`,
  `explicit MockNewDefaultsNotice(config::NewDefaultsOffer offer)` with
  `offered = offer != config::NewDefaultsOffer::None` (no `bool` constructor parameter; `setOffered` is the
  "parameter is the property" setter carve-out, as the moved mocks' setters are),
  `Q_INVOKABLE useNew()` / `keepMine()` appending to `QStringList calls`. The snapshot scene passes
  `NewDefaultsOffer::ExistingConfig` for a `SnapshotBanner::NewDefaults` row and `NewDefaultsOffer::None`
  otherwise.

**The decision table** (`newDefaultsOffer`):

| `contour.yml` states | `settings.yml` states | Offer |
|---|---|---|
| nothing | nothing | `ExistingConfig` |
| `native2026` | nothing or `native2026` | `None` |
| `native2026` | `classic` | `SettingsOverride` — the Reset Config case |
| `classic` | nothing | `None` — an explicit choice (owner decision 3) |
| anything | `native2026` | `None` |
| nothing or `classic` | `classic` | `None` — an explicit choice |

Copy, both offers: text "Contour has new defaults", action "Use New", dismiss "Keep Mine" (§2.7).

- [ ] **Step 1: Write the failing tests.**
  - `config/NewDefaultsOffer_test.cpp`, `[newdefaults]`: one row per table line above, asserting
    `newDefaultsOffer` and `effectiveEdition`; and `isOfferShown(SettingsOverride, { ExistingConfig })` is
    true while `isOfferShown(SettingsOverride, { SettingsOverride })` is false (Review Focus 2).
  - `config/GuiStateStore_test.cpp`, `[guistate]` (a `test/TempDir.hpp` directory): a missing file loads
    as `GuiState {}`; save then load round-trips `{ ExistingConfig, SettingsOverride }`; a file holding
    `dismissed_new_defaults: 7` loads as `GuiStateError::Malformed`; an unknown token is skipped, not an error.
  - `Config_test.cpp`, `[config][newdefaults]`: loading through the path overload a `contour.yml` with
    `defaults_edition: native2026` and a `settings.yml` with `defaults_edition: classic` gives
    `defaultsEditionSources == { .mainFile = Native2026, .settingsFile = Classic }` and
    `defaultsEdition == Classic`.
  - `window/NewDefaultsNotice_test.cpp`, `[newdefaults]` (a `QTemporaryDir`, a real `FileGuiConfigStore`,
    a real `FileGuiStateStore`, a `Config` whose sources are set in the test, and a `reload` lambda that
    counts calls and sets the sources to `{ .mainFile = …, .settingsFile = Native2026 }`):
    - `ExistingConfig`: `offered()`; `useNew()` writes `defaults_edition: native2026` to `settings.yml`
      (read the file back), calls `reload` once, and `offered()` becomes false after `refresh()`.
    - `keepMine()` hides it; a second notice over the same directory is not offered.
    - After `keepMine()` on `ExistingConfig`, sources `{ Native2026, Classic }` (a Reset Config) are offered
      again; `keepMine()` there hides that too, and a third notice is not offered.
    - A `reload` that returns `std::unexpected("broken")`: `switchToNewDefaults()` returns that error and
      `offered()` stays true.
  - `MainWindowQml_test.cpp`, `[mainwindow][newdefaults]`: with `newDefaults` =
    `MockNewDefaultsNotice { config::NewDefaultsOffer::ExistingConfig }`
    set on the engine before `loadMainWindow`, `newDefaultsBanner` is visible with the copy above;
    clicking its action and dismiss records `useNew` and `keepMine`; with no `newDefaults` property the
    load has no diagnostics and the banner is invisible; under the Terminal setup its `background`'s colour
    is the scheme Bar.
  - Snapshot counts: 10 scripts, 160, 40; `case SnapshotScript::NewDefaultsBanner:` asserts the banner is visible.

- [ ] **Step 2: Run them and see them fail.**

Run: `cmake --build out/appleclang-debug -j 3 --target contour_test contour_gui_test`

Expected: compile failure — `NewDefaultsOffer.hpp`, `GuiStateStore.hpp` and `NewDefaultsNotice.hpp` do not exist.

- [ ] **Step 3: Implement.** In `ContourGuiApp`, the notice gets
  `[this]() -> config::Config const& { return _config; }`, a `FileGuiConfigStore` and a `FileGuiStateStore`
  over `_config.configFile.parent_path()`, and `[this] { return _sessionManager.reloadAllSessions(); }`.

- [ ] **Step 4: Run them.**

Run: `out/appleclang-debug/src/contour/contour_test "[newdefaults],[guistate],[snapshot]" && out/appleclang-debug/src/contour/contour_gui_test "[newdefaults],[mainwindow],[snapshot],[config][newdefaults]" && ctest --test-dir out/appleclang-debug -R "check_qml_(sizes|colors)$"`

Expected: PASS. The Terminal-setup banner case and the Terminal `new-defaults-banner` scene are the TUI rendering.

- [ ] **Step 5: Format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/config/DefaultsEdition.hpp src/contour/config/NewDefaultsOffer.hpp src/contour/config/GuiStateStore.* src/contour/config/Config.* src/contour/window/NewDefaultsNotice.* src/contour/ContourGuiApp.* src/contour/session/TerminalSessionManager.cpp src/contour/test/*.hpp src/contour/config/*_test.cpp src/contour/window/NewDefaultsNotice_test.cpp src/contour/test/*_test.cpp
editorconfig-checker src/contour/qml/Main.qml src/contour/CMakeLists.txt src/contour/config/CMakeLists.txt
git add src/contour
git commit -m "contour: offer the new defaults once to Classic users, including after Reset Config"
```

---

### Task 12: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff 6da3e0a0..HEAD -- src scripts CMakeLists.txt`, so the plan's own commits are left out) and apply its findings;
  re-run the filters of every task it touches.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[chrometokens],[newdefaults],[guistate],[snapshot],UiStyle*"
out/appleclang-debug/src/contour/contour_gui_test "[appearance-source],[uistyle],[chromehost],[appearance],[kit],[a11y],[mainwindow],[snapshot],[motion],[chromefollow],[manager][theme],[newdefaults],[qml],[contour][gui][style]"
out/appleclang-debug/src/vtbackend/vtbackend_test "[background-event],Terminal.DECAC"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: Snapshots on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: `160`. Open `toast.terminal.*`, `kit-dialog.macos.*`, `hosting-banner.terminal.*` and
  `new-defaults-banner.macos.*` to look at the four new surfaces.
- [ ] **Step 4: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release at `:107` (the `x.y.z` block at `:95-105` is a
  commented-out template):
  Reduce Motion stops the cursor glide and the screen transition (macOS); the chrome follows a
  background set by OSC 11; the hosting notice is a banner; Classic users are offered the new defaults once.
- [ ] **Step 5:** Mark Step 1a done in the plans index and on the board, and update the PR body with
  `/update-pr`, reporting:
  - **Performance:** none on the render path; a provider rebuild only on an OS appearance change, a
    reload or a light/dark flip.
  - **Risk:** medium — `ContourGuiApp`'s constructor, the chrome-theme path and the Main.qml layout.
  - **Coverage:** the new Catch2 cases and the four snapshot scripts.

---

## Owner decisions

1. **Dialog placement is a token, not a sixth layout switch.** Principle 1 caps the layout switches at
   five; the canvas wants a sheet on macOS and a centred dialog elsewhere. This plan expresses it as
   `layout.dialog` read by one `y` binding. Confirm, or name it a sixth switch.
2. **No Retry on the hosting banner.** The canvas draws Retry; §3.1 keeps the
   `hostingNotice` / `dismissHostingNotice` contract. Retry needs a controller call that does not exist,
   so it is left for 3c's NoticeModel.
3. **An explicit `defaults_edition: classic` is never offered the switch.** Only a file that states no
   edition, or the Reset Config conflict, sees the banner.
