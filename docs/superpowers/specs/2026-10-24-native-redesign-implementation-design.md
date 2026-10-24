# Native redesign: implementation design for all phases

This document turns the visual design into an engineering design. For each phase it states what
gets built, which seams in today's code it uses, which types it adds, how it is tested, and what it
must not break.

How things look is defined elsewhere: on the canvas
(<https://claude.ai/artifact/6cp7tcSnurTSMbhSDePWqH>) and in the design record,
[`2026-10-23-native-redesign-design.md`](2026-10-23-native-redesign-design.md). How they are built
is defined here. Implementation plans are written one per PR-sized step from this document, and
cite its section numbers.

**Baseline.**
- Master is at `4637e4a6`, with PR #2176 merged.
- CI validates Qt 6.11 (`CONTOUR_QT_VALIDATED_VERSION`, pinned to 6.11.1).
- `find_package(Qt6)` states no minimum version, so every Qt ≥ 6.9 API sits behind
  `QT_VERSION_CHECK`.

**Review history.**
- Round 1 used three Fable reviewers: code accuracy, coverage and principles, sequencing and risk.
  Their findings are folded in; the main structural ones are summarised in §6.
- Round 3 confirmed the round-2 fixes. It found one ordering fix: the pre-scan now runs after the
  auto-created file exists. The document is ready.
- Round 2 (one Fable reviewer) checked the fixes, the step order and the citations. Its findings
  are folded in: the edition pre-scan, reuse of the injected `FileSystem`, enum outputs for the
  header policy, and named component tokens.

## 1. Principles that bind every phase

1. **The chrome is data-driven.**
   - Every extent, radius, colour role, type size, duration and glyph comes from one of two
     places: a token row (`config/UiStyle.hpp`, resolved per `ChromeStyle`), or the terminal's
     colour scheme.
   - QML interprets tokens and holds no literals.
   - Two lint gates enforce this:
     - `check-qml-colors`, added in Phase 0 Task 6;
     - a sibling gate for numeric size literals, added in 1a.
2. **There is one component tree.**
   - Platforms differ through token rows, plus at most five layout switches: caption buttons, tab
     shape, header-bar arrangement, profile dropdown, settings layout.
   - There are no per-platform QML forks.
   - The Terminal (TUI) row is drawn by the same components and lands in the same PR as each
     visual change, not later.
3. **Pure decisions are headless.**
   - Every decision lives in a dependency-free header under `contour/window/`, `contour/command/`
     or `vtbackend/`, and has Catch2 tests.
   - Examples: which badge wins, whether a pane shows a header, which menu rows exist, where the
     find bar goes, how a status segment collapses.
   - QML only renders the answer.
4. **The OS is reached through interfaces.**
   - Everything that asks the OS is an interface in `contour/platform/`: accessibility
     preferences, accent colour, button layout, materials, hotkeys, notifications.
   - Each interface has a null implementation and a test double, and is injected at construction
     (AGENT.md: dependency injection, configuration at construction).
5. **Nothing goes over non-blank cells.**
   - Badges, chips, toasts, banners, hint labels and link pills go into chrome bands, pane
     headers, gutters, or blank cells only.
6. **Existing users keep their look** (§2.7).
7. **AGENT.md applies throughout.**
   - Zero warnings.
   - `enum class` over `bool`, with the default enumerator at zero.
   - `std::expected` for fallible calls.
   - Qt-free targets stay Qt-free: `contour_config`, `contour_command`, `vtbackend`.
   - When this design touches an API that takes `bool`s, it fixes the API in the same change.

## 2. Cross-cutting foundations

### 2.1 Token rows (`config/UiStyle.hpp`)

Phase 0 Task 1 introduced `ChromeStyle { MacOS, Fluent, ContourLinux, Terminal }` (commit
`1129e623`). The row is resolved once, from `UiStyle × HostPlatform`.

Token groups are added by the first step that consumes them, never ahead of need:

| Group | Added in | Contents |
|---|---|---|
| status colours | 0 | `StatusColorRole { Danger = 0, Warning, Success, Running }` → `{light, dark}`; Running = accent |
| size, radius, type, motion, elevation, materials | 1a | `size.titleBar/tabWell/tab/control/paneHeader`, `space.paneGap/splitHit/paneInset`; `radius.control/popover/menu/paneCard`; `type.caption/footnote/body/tab/title`; `motion.fast` (100 ms) / `standard` (160 ms) / `emphasized`; `e0..e3`; `material.titleBand/popover` ∈ `Material { None = 0, Glass, Mica, Solid }` |
| density | 2 | `ChromeDensity { Comfortable = 0, Minimal }` selects a size sub-row |

**Component tokens.** Extents quoted later in this document are named tokens, added by the step
that consumes them, so the size-literal gate never flags them:

| Token | Value | Step |
|---|---|---|
| `size.toast` | 36 | 1a |
| `radius.toast` | 18 | 1a |
| `size.palette` | 640 | 3d |
| `size.paletteRow` | 32 | 3d |
| `size.rail` | 3 | 3f |
| `space.blockGutter` | 22 | 3f |
| `size.foldChipHit` | 24 | 3f |

The Terminal row's own values are tokens too:
- `tui.inactivePaneOpacity`: 0.62;
- `tui.toastStyle`: inverse.

**Native rows.**
- Until Phase 5, the Fluent and ContourLinux rows keep today's geometry, so the shared QML cannot
  carry the macOS band onto Windows and Linux.
- An acceptance test in 1c enforces this: `MainWindowQml_test` geometry is unchanged under those
  two rows.

**Terminal (TUI) row.**
- Every size counts in cells, the radius is 0, there are no materials, and the shadow is a hard
  one-cell offset.
- Every colour role comes from the active terminal scheme:
  - background for surfaces, palette[0] for bars, the foreground for text;
  - bright black for secondary text, the scheme's selection colour;
  - blue for accent, red and green for status;
  - yellow as a glyph only, never a fill.

**Provider.**
- `UiStyleProvider` exposes the token groups as grouped `Q_PROPERTY` objects
  (`chromeStyle.size.tab`, …).
- It is constructed from the resolved row, the chrome font, the platform appearance (§2.4), an
  `AccessibilityPreferences` snapshot (§2.3), and, from Step 2, the density.
- When any of those inputs changes, the provider is rebuilt, never mutated.

**Contrast overrides.** `ContrastPreference::Increased` changes these tokens:
- the separator alpha doubles;
- the active pane card gets a 1.5-pt accent ring;
- the pane dim drops to 5 %;
- status colours use their AA-large-safe variants.

The resolver has a test row for this.

### 2.2 Chrome colour follows the terminal

Phase 0 Task 3 makes this the default:
- It adds `GuiTheme::Terminal`. `GuiTheme` is `{System, Dark, Light}` today (`Config.hpp:130`).
- It adds `colorSchemeOfBackground`, built on the existing `relativeLuminance()`
  (`vtbackend/core/Color.hpp:109`). The threshold is 0.18.
- A dual colour config follows the OS instead.

Live scheme changes are re-evaluated through `ContourGuiApp::applyGuiTheme`
(`ContourGuiApp.cpp:1641-1686`).

### 2.3 Accessibility preferences (1a)

```cpp
enum class MotionPreference : uint8_t { Full = 0, Reduced };
enum class TransparencyPreference : uint8_t { Allowed = 0, Reduced };
enum class ContrastPreference : uint8_t { Standard = 0, Increased };
struct AccessibilityPreferences { MotionPreference motion; TransparencyPreference transparency; ContrastPreference contrast; };
class IAccessibilityPreferenceSource {
  public:
    virtual ~IAccessibilityPreferenceSource() = default;
    [[nodiscard]] virtual AccessibilityPreferences current() const = 0;
}; // change notification: a std::function supplied to the backend's constructor
```

**Backends.**
- 1a ships the null backend and the macOS backend. The macOS backend lives in a new `.mm`, and
  reads `NSWorkspace accessibilityDisplayShouldReduceMotion/ShouldReduceTransparency/ShouldIncreaseContrast`.
  It observes `NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification` for changes.
- Phase 5 adds the Windows and Linux backends:
  - Windows: `SPI_GETCLIENTAREAANIMATION`, `SPI_GETHIGHCONTRAST`, and `EnableTransparency` from
    the registry.
  - Linux: XDG `org.freedesktop.appearance` `contrast` and `reduced-motion`.

**Terminal content also honours Reduce Motion (1a).**
- The cursor glide and the screen transition are profile settings: `cursorMotionAnimationDuration`
  and `screenTransitionDuration` (`Config.hpp:1098-1103`).
- They are consumed on the display/rasterizer path, which the chrome tokens never reach.
- The session's display is therefore constructed with the snapshot, and forces both durations to
  0 under `MotionPreference::Reduced`. A test covers this.

### 2.4 Platform appearance

`IPlatformAppearance` supplies:
- the accent colour;
- the window corner radius, where the OS publishes it (otherwise a measured constant in the row);
- on Linux, the button layout.

The macOS backend lands in 1a, and the Linux and Windows backends land in Phase 5.

**Linux backend.**
- It reads the XDG Settings portal through the existing `PortalCall` plumbing: `ReadAll` plus
  `SettingChanged` on `org.freedesktop.appearance` (`accent-color`, `color-scheme`).
- The button layout comes from the first of these that answers:
  1. `org.gnome.desktop.wm.preferences button-layout`, where the portal serves it;
  2. KWin's `ButtonsOnLeft`/`ButtonsOnRight`;
  3. otherwise, a close button on the right.
- The fallback accent colour is `#3584E4`.

### 2.5 Chrome snapshot driver (1e, test-only, can land first)

There is no screenshot driver today; `grabWindow()` results are thrown away.

**What 1e adds.**
- A `ChromeSnapshot` harness in `src/contour/test/`.
- It loads `Main.qml` with `MockMainController` (`MainWindowQml_test.cpp:324-345`).
- It runs named scripts and writes one PNG per `ChromeStyle` row. Examples: `single-tab`,
  `three-tabs`, `split-three`, `palette`, `settings`, `remote-reconnecting`.

**How it runs.**
- Offscreen QPA cannot run a scene-graph render loop (`test_main.cpp:84-93`).
- So the harness is tagged `[display]` and runs on the existing weston route
  (`scripts/run-display-tests.sh`).
- CI uploads the PNGs as an artefact, for reviewers to look at.
- Its only assertion is that each script renders without QML warnings.

**What it does not do.** It never decides acceptance (§2.6).

### 2.6 Testing strategy

- **Pure decisions** are Catch2 tables in the module's `*_test` target.
- **QML components** follow the `QmlComponents_test.cpp` pattern: load against mocks, then assert
  through `objectName` and `findChild`. Every element a test needs gets an `objectName`.
- **Acceptance is property tests, not images.**
  - `MainWindowQml_test` asserts token-driven values: for example, the tab well's height equals
    `chromeStyle.size.tabWell`, the pill radius equals its token, the badge slot reports the
    resolved kind, and the band's inset is applied.
  - A step that changes geometry on purpose updates these assertions in the same commit, citing
    the canvas value.
- **Test scope per task.** Each task runs its own test filter (owner request, 2026-10-24).
  - The full suite runs in CI on Linux, macOS and Windows. FreeBSD excludes
    `contour_gui_test` (`build.yml:578`).
  - A task touching `vthost` also runs the `daemon` label locally.
- **Widgets in tests.** `contour_gui_test` constructs a `QGuiApplication`, not a `QApplication`.
  So code that builds Widgets objects (`QMenu`, `QMenuBar`) is split in two:
  - a pure tree-building step, which returns plain structs and is tested;
  - a thin realisation, which is exercised only by the `[display]` smoke tests.
- **Accessibility.** Every new control sets `Accessible.role` and `Accessible.name`. A
  table-driven test walks the chrome's accessible tree.

### 2.7 Existing users keep their look

A config file that sets only `profiles.main.font` would otherwise inherit every new default:
- colours;
- status line on;
- tab bar `Multiple`;
- link decoration `Modifier`;
- dim 0.15;
- scrollback 10 000;
- and the rest.

That is the common case, so the new defaults are gated by an **edition**:

```cpp
enum class DefaultsEdition : uint8_t { Classic = 0, Native2026 };
```

**Which edition applies.** The rules follow `Config.cpp`'s load order.
- **Fresh install.** `loadConfigFromFile` calls `createFileIfNotExists` (`:1199`) before parsing.
  It writes `defaultConfigString()` (defined at `:551`, written at `:569`), the same text `contour generate config` emits
  (`ContourApp.cpp:851`). That text carries `defaults_edition: native2026`, so a fresh install
  is `Native2026` because of the file it creates. The path overload of `loadConfigFromFile` hoists
  `createFileIfNotExists(fileName)` ahead of the pre-scan, so the pre-scan always finds a file;
  the `Config&` overload keeps its own call, which is now a no-op. There is no missing-file branch.
- **Pre-scan.** `Config` (`:597`) and the seed `TerminalProfile` (`:1208`, also the base for
  side-file inheritance at `:1109`) are constructed before the YAML is read. So a pre-scan reads
  `defaults_edition` from `contour.yml` and then from `settings.yml`, where `settings.yml` wins.
  The result is passed into the constructors as `Config{edition}` and `TerminalProfile{edition}`.
- **Existing files.** If neither `contour.yml` nor `settings.yml` sets the key, the edition is
  `Classic`.
- **The `Config&` overload cannot honour the pre-scan**, because the edition is fixed at
  construction. Edition-sensitive fixtures therefore load through the path overload. Existing
  `Config_test` fixtures that use `Config{}` with `loadConfigFromFile(config, path)`
  (`Config_test.cpp:91`, `:607`, `:2696`) see `Native2026` defaults, and Phase 0 Task 8 Step 3
  updates their assertions about changed defaults.
- **Plain `Config{}`** means `Native2026`. That keeps `documentationGlobalConfig(Config{})`
  (`:576`) and the Phase 0 tests unchanged.
- **Typed loaders.** The key is excluded from the late typed-loader pass of
  `mergeGuiManagedSideFiles` (`:1141`, `:1159`), because the pre-scan has already read it.
- **Missing-key diff.** The key is also excluded from `compareEntries` (`:994`), so Classic files
  do not log a missing key on every load.
- **Default tables.** Every default lives in one table per edition, rather than in scattered `if`s.

**Users on Classic.** They see a one-time banner, built in 1a: "Contour has new defaults · Use New
· Keep Mine".
- "Use New" writes `defaults_edition: native2026` to the GUI-owned `settings.yml`, then reloads.
  The GUI reload builds a fresh object (`tryLoadConfigFromFile`, `TerminalSessionManager.cpp:726`),
  so no stale value survives the switch. The reload-into-existing overload (`CoreFixtures.hpp:48`)
  is test-only.
- "Keep Mine" stores the dismissal in the GUI state.

**Where it lands.** The edition gate and its table land with Phase 0 Task 8.

**Tests.** `Config_test` fixtures check:
- a partial `contour.yml` keeps the pre-redesign values;
- a `settings.yml` that sets `native2026` overrides that;
- the generated config is `Native2026`.

## 3. Steps

Each step is one PR unless it says otherwise. The order is:

```
0 → 1e → 1a → 1b → 4a → 1c → 1d → 2 → 3a … 3e (any order) → 3f → 4b → 5a, 5b → 6.1 … 6.10
```

Steps 3a to 3e are independent of each other. Step 3f (command blocks) needs 1b (the menu
presenter) and 2 (pane headers).

### 3.0 Phase 0 — Foundations

The work is owned by `docs/superpowers/plans/2026-10-23-native-redesign-phase0.md`. The plan
changes from this review are:

1. **Change `ColorPalette::blockStatusColor`'s Running fallback** (it already exists) to
   `normalColor(4)`, blue, instead of `normalColor(3)`, yellow
   (`vtbackend/core/ColorPalette.hpp:396-406`). Yellow means search. Schemes that set
   `block_status.running` themselves are unaffected.
2. **Task 8 gains the `DefaultsEdition` gate** (§2.7). Every new default goes into the
   `Native2026` table.
3. **The `hyperlink_hover_tooltip: false` default moves to 3e.** It ships in the same PR as the
   link target pill that replaces the tooltip.
   - `LinkDecorationMode::Modifier` can still become the default in Phase 0. The mode already
     exists (`vtbackend/core/LinkDecoration.hpp:19-24`) and works without the pill.
4. **`colorSchemeOfBackground` is new in Phase 0.** It is not an existing seam.
5. **`relativeLuminance` and `contrastRatio` already exist** (`Color.hpp:109`, `:116`). Tasks 3
   and 4 consume them rather than producing them.

### 3.1 Step 1a — Kit primitives, tokens and preferences

**Contents.**
- The token groups from §2.1, consumed only by the new primitives in this step.
- The size-literal lint gate.
- `AccessibilityPreferences`, with the null and macOS backends (§2.3).
- `IPlatformAppearance`, with the macOS backend (§2.4).
- Reduce Motion for the terminal's own durations (§2.3).
- New components (`Toast` and `Banner` are presentational only; queueing comes in 3c):
  - `Toast.qml`: a pill, 36 high, radius 18.
  - `Banner.qml`: a band below the title bar that pushes the content down once.
  - `KitDialog.qml`: a sheet-styled in-scene `Dialog`. A Qt Quick Controls `Dialog` never becomes
    an NSWindow sheet, so it is styled as one instead.
- The new-defaults banner from §2.7, which consumes `Banner`.
- **Retiring PR #2176's `hostingNoticeBar`.** The bar in `Main.qml:229-270` becomes a `Banner`
  with the same `WindowController.hostingNotice` / `dismissHostingNotice` contract. This matches
  the design record's statement that the notice becomes a Banner.

**Tests.**
- The token resolver, including the motion, transparency and contrast overrides.
- A load test and a property test for each primitive.
- The hosting notice rendered as a Banner.
- The edition banner's decision table.

**TUI row.**
- Toast: a one-row inverse cell strip.
- Banner: one row in the scheme's palette[0].
- Dialog: a boxed popup with the hard shadow.

### 3.2 Step 1b — Menus as data, presenters, macOS menu bar

**Zero visual change on Linux and Windows.**

**Menus as data.** Three menus become `ContextMenuEntry` tables in `command/`:
- `TabContextMenu.qml`'s static items (Rename, Tab Color › an inline swatch row, Move Tab to New
  Window, Close Tab, Close Other Tabs, Close Tabs to the Right);
- `NewTabMenu.qml`;
- the title-bar menu, which is already data (`command/TitleBarContextMenu`).

On macOS, the title-bar menu's rows are also mirrored into View › Tab Bar.

**Presenter.** `menuPresentation` is a token with two values, `InScene = 0` and `Native`.
- The macOS row uses `Native`: `window/NativeMenuPresenter` realises the tree as a `QMenu`, which
  on macOS is an NSMenu, sub-menus included, and calls `popup()`.
  - It first builds a pure `MenuTree` struct, which is tested.
  - The `QMenu` realisation is exercised only by the `[display]` smoke tests.
- Every other row keeps today's in-scene `ActionContextMenu.qml`.
  - On Windows a `QMenu` is a widget painted by `QStyle`, not a Win32 menu. So Windows uses
    in-scene menus until 5a decides between in-scene and QML `Popup.Native` with pre-built
    sub-menus.

**Menu bar (macOS).** `command/MenuBarTable` holds the six menus (Contour, Shell, Edit, View,
Window, Help) as `ContextMenuEntry` trees over the action catalogue.
- It adds a Qt-free column, `MenuItemRole { None = 0, About, Preferences, Quit }`.
- `window/AppMenuBar` maps that column to `QAction::MenuRole`, realises the menus as a parentless
  `QMenuBar` once per app, and rebuilds it on config reload.
- Shortcut text comes from `Shortcut.hpp`.

**Tests.**
- Every catalogue action is reachable from a menu.
- No chord appears twice.
- Each role is set exactly once.
- The tab context menu table.
- The `MenuTree` builder.

**TUI row.** In-scene menus on every OS. The macOS menu bar stays native, since it lives outside
the window.

### 3.3 Step 4a — Settings window shell

This step lands before the title band, so a pill-tab Settings entry never coexists with the
native menu bar's "Settings…".

**Contents.**
- A new `SettingsWindow.qml` hosts today's `SettingsPage.qml` unchanged. It is a separate
  `QQuickWindow`, one per app, loaded on the shared engine (`ContourGuiApp.cpp:2169`).
- `OpenConfiguration` (⌘, / Ctrl+,) and Contour › Settings… show the window and focus it.
- Removed:
  - the pseudo-tab (`TabStrip.qml:210-260`);
  - `settingsActive` and its `Main.qml` loader (`Main.qml:203-226`);
  - `NewTabMenu`'s Settings row.

**Tests.**
- Opening the window twice yields one instance.
- The tab model no longer has a settings row.
- `SettingsPageQml_test` still passes inside the window.

### 3.4 Step 1c — Title band, tab well, chips

**Contents.**
- **Tab well and tabs.** The well, the pill tabs, the badge slot, close-on-hover, the tab colour
  tint (20 % on the pill, 100 % on its icon), and the flyout restyle. They are rendered by
  `TabStrip.qml` / `TabItem.qml` with the existing roles (`WindowController.cpp:320-336`).
- **Badge slot (pure, `window/TabBadge.hpp`):**

  ```cpp
  enum class TabActivity : uint8_t { None = 0, UnseenOutput };
  enum class FinishedBlockBadge : uint8_t { None = 0, Succeeded, Failed }; // cleared when the tab is seen
  enum class TabBadgeKind : uint8_t { None = 0, Output, Done, Working, Error, Blocked };
  struct TabBadgeInputs { BadgeKind programStatus; std::optional<int> progressPercent; FinishedBlockBadge finishedBlock; TabActivity activity; };
  struct TabBadge { TabBadgeKind kind; std::optional<int> progressPercent; };
  [[nodiscard]] constexpr TabBadge resolveTabBadge(TabBadgeInputs const&) noexcept; // Blocked > Error > Working > Done > Output
  ```

  - `BadgeKind` is PR #2176's type (`window/ProgramStatusBadge.hpp:30`).
  - Working uses `StatusColorRole::Running` (the accent), replacing the green that
    `programBadgeLooks` in `TabItem.qml` uses today.
- **HostChip.** `HostChip.qml` replaces `TabStrip.qml`'s `hostBadge`.
  - Its state comes from the pure `window/HostChipState.hpp`, one of `Connected = 0`,
    `Reconnecting` or `Disconnected`.
  - The title keeps `RemoteChrome::remoteWindowTitle`.
- **Drift chip.** `DaemonDriftBadge` moves from its bottom-right anchor over the cells into the
  band's trailing area. It reads the same `DaemonDriftModel`, and its confirmation goes through
  `KitDialog`.
- **No title bar and full screen.**
  - With `show_title_bar` (`Config.hpp:1056`) set to true, the native server frame is used: no
    band, and the tabs sit in a strip below the frame, as today.
  - In full screen (`Config.hpp:1066`), the band reveals when the pointer reaches the top edge.

**Acceptance (`MainWindowQml_test` properties).**
- The macOS row matches the canvas tokens.
- The Fluent and ContourLinux rows keep today's geometry.
- The badge priority table holds, and so does the `HostChipState` table.

**TUI row.**
- The tab row is one cell high, with `│` separators.
- Badge glyphs replace the pixel badges.
- On macOS, AppKit's traffic lights are snapped to the first cells.
- Linux CSD and Windows draw `● ● ●` cells from the scheme.

### 3.5 Step 1d — macOS title-band material and safe area

**Material.**
- **Primary mechanism (supported AppKit behaviour):** keep `NSWindowStyleMaskFullSizeContentView`,
  and set `titlebarAppearsTransparent = NO`. AppKit then draws its own title-bar material beneath
  the content view.
- The QQuickWindow gets an alpha-capable format. The band area is cleared to transparent, and
  panes stay opaque.
- Watch out: CocoaWindowFrame warns that an alpha surface can clear `hasShadow`
  (`CocoaWindowFrame.mm:62-64`). The step re-asserts the shadow after `apply()`.
- **Fallback:** a `Solid` band, painted from the terminal background tint. It is used under
  `TransparencyPreference::Reduced`, or if the material flickers on resize.
- The choice sits behind `ITitleBandMaterial`, which also has a null implementation.

**Safe area.**
- `nativeControlsInset()` (`CocoaWindowFrame.mm:81-97`) stays the horizontal inset source for the
  traffic lights.
- Under Qt ≥ 6.9, the band's height comes from `QWindow::safeAreaMargins().top`, but only if the
  window adopts `Qt::ExpandedClientAreaHint`.
- The step either moves the expansion to that hint and deletes the manual style-mask code, keeping
  a single owner, or keeps the manual code and the measured band height. **Decision in the 1d
  plan, after a spike.**

### 3.6 Step 2 — Panes: cards, headers, densities

**Seams.**
- `PaneNode.qml` draws splits with a `SplitView`.
- Its `handle` thickness is `vtworkspace::DefaultSplitHandleThickness = 6`
  (`PaneLayout.hpp:37`). The window-size solver uses the same value (`WindowController.cpp:1619-1625`).
- The dim layer is at `TerminalPane.qml:181-200`, and the reconnecting overlay at `:205-220`.

**Per-pane data (new `Q_PROPERTY`s on `TerminalSession`, exposed through `PaneProxy`).**
- `displayWorkingDirectory`: notifies on OSC 7 and OSC 3008 changes.
- `foregroundProcessName`: generalises the polling that today runs only for extensions
  (`TerminalSession.cpp:1157-1174`). It runs only while a header is visible, every 1 s.
- `programStatus`: per pane. The tab aggregate stays.

**Gap and hit area.**
- The visual gap is `space.paneGap`: 6 in Comfortable, 1 in Minimal. The size solver uses it.
- The handle becomes a transparent item of `space.splitHit` width (10 / 8), with a centred
  grabber, 2 × 32, shown on hover. The hit area overlaps the cards and never changes the layout.

**Header policy (pure, `window/PaneHeaderPolicy.hpp`).**
- Inputs, all enums with zero meaning none:
  - `ChromeDensity`;
  - `HeaderReveal { None = 0, HeldModifier }`;
  - `PointerPresence { Away = 0, Hovering }`;
  - `PaneFocus { Unfocused = 0, Focused, KeyboardFocused }`;
  - the visible pane count;
  - the width in points.
- Outputs, as enums:
  - `PaneHeaderContent { Hidden = 0, Name, NameAndCwd }`, where the cwd appears from 360 pt wide;
  - `HeaderActions { Hidden = 0, Shown }`.

**Density.** A new global key, `chrome_density: comfortable | minimal`. Changing it rebuilds the
provider.

**Pane states.**
- Reconnecting: the pane dims by the token, plus a static `Toast` pill (from 1a).
- Disconnected or exited: `ExitedFooter.qml` sits in the pane's bottom chrome, never over the
  cells. Return restarts the session.

**TUI row.**
- The header is one row of cells: name, cwd, and `⊞ ⊟ Z ×`.
- Splits are `│` and `─` cell lines.
- Inactive panes are drawn at 0.62 opacity.
- In Minimal, the bar row is hidden while there is one tab, and there are no headers.

**Tests.**
- The header policy table.
- Per-pane property notifications, using the mock PTY.
- The handle's hit width versus the visual gap.
- The size solver with the new gap.
- Pane-state properties.

### 3.7 Step 3a — Status line content (vtbackend)

**New placeholders.** Each takes four edits: the struct, the variant, `ItemTraits`, and a
serializer overload (`StatusLineBuilder.hpp:25-258`, `.cpp:366-780`).

| Placeholder | Source | Notes |
|---|---|---|
| `Cwd` | `currentWorkingDirectory()` (`Terminal.hpp:3496`) | |
| `Git` | `.git/HEAD` | Read through the `core::platform::FileSystem` the `Terminal` already takes (`Terminal.hpp:826`): `readFile`, with `lastWriteTime` for the cache key. The cache key is the cwd plus `.git/HEAD`'s mtime, invalidated on OSC 133 D. It is never read per frame. Tested with `core::platform::testing::InMemoryFileSystem`. No new interface. |
| `LastCommand` | `finishedBlocks(CommandBlockStore const&, …)` (`SemanticBlockTracker.hpp:98`) | Reads `exitCode` and `duration`. The command text goes through `CommandLineText`'s display accessor (`CommandBlock.hpp:145`). |
| `Selection` | new `Selection::extent()` | Always reports lines. Reports characters only when the selection is below 10 000 cells; above that, lines only. Never O(selection) per frame. |
| `Scroll` | `viewport().scrollOffset()` against the history size | Empty at the bottom. |

**Flags and colours.**
- A new `Hide=Insert` flag on `InputMode`.
- Role colours: `Color=accent|success|danger|warning|muted`, resolved against the palette at
  render time. `tryParseColorAttribute` (`.cpp:69`) gains a branch for these.

**Collapse.** A pure collapse step drops segments by priority when the line is too narrow.

**Template.** This step switches to the full approved template. The Settings chip editor picks
the new placeholders up automatically (`StatusLineBuilder.hpp:273`).

**Tests.**
- Parse and serialise each placeholder.
- The `Git` cache, including invalidation.
- The selection-extent threshold.
- The collapse table.
- Role-colour resolution.

### 3.8 Step 3b — Find bar

**Today.** The find bar's only toggle is case (`session/SearchStatus.hpp:136-160`). Search runs
**on the GUI thread, under the state lock** (`TerminalSession.cpp:4408-4420`).

**Search options.**
- Add `SearchSyntax { Literal = 0, Regex }` and `SearchWordBoundary { Any = 0, WholeWord }`.
- An invalid regex is reported as `std::expected` and shown as "Invalid pattern" in the count slot.

**Performance.**
- The walk becomes chunked: 500 lines per step, releasing the lock between steps, and yielding to
  the event loop.
- The count updates as the walk progresses.
- A regex is compiled once per query.

**Placement.**
- Pure decision: `window/FindBarPlacement.hpp`.
- The bar sits under the pane header, or at the top right of a single pane. It moves to the bottom
  right when the current match is under it.
- The count text reads "2 of 5".

**Tests.**
- Regex and whole-word matching in `vtbackend_test`, including an invalid regex.
- Chunked search equals unchunked search.
- The placement table.

### 3.9 Step 3c — Notices, dialogs and permissions

**NoticeModel.** A per-window `window/NoticeModel` queues notices for `Toast` and `Banner`.
Toasts stay while hovered, and are announced through `Announcer` with Polite priority. The
notices it carries:
- **Config reload errors.** Today these are only logged (`TerminalSessionManager.cpp:85-87`,
  `WindowController.cpp:85-86`, `TerminalSession.cpp:4812-4816`). `reloadAllSessions()` already
  returns `std::expected`.
- "Copied N lines".
- Restore summaries.

**Permissions use one Dialog.**
- `RequestPermission.qml`, a five-button `Qt.labs.platform` MessageDialog, is replaced by
  `KitDialog`.
- Its title names the action, and it shows detail where there is some (for example, a paste
  preview).
- It has two buttons, Deny and Allow, plus one checkbox.

**Guarded-role API.** The five `(bool allow, bool remember)` APIs (`TerminalSession.hpp:823-833`,
`executeRole :1261`) become `(PermissionDecision, PermissionMemory)`:

```cpp
enum class PermissionDecision : uint8_t { Deny = 0, Allow };
enum class PermissionMemory : uint8_t { Once = 0, Session, Persistent };
```

- Both are `Q_ENUM`.
- `PermissionCache` stores `PermissionDecision`.
- The checkbox label follows the memory scope:
  - guarded roles use `Session`: "Don't ask again in this session";
  - PR #2176's control and extension consent uses `Persistent`: "Remember for this extension".

**Extension consent.** The consent API (`allowPendingExtension` / `denyPendingExtension`,
`SessionChrome.qml:353-357`) is migrated onto the same pair.

**Save Layout.** `SaveLayoutDialog.qml` becomes a `KitDialog` that says what will be saved. Save
stays disabled while the name is empty, and offers "Replace?" inline when the name is taken.

**Tests.**
- The NoticeModel queue.
- An enum round trip through QML.
- Each guarded role's decision and memory.
- The Save Layout validation table.

### 3.10 Step 3d — Command palette

**Today.** `CommandPaletteModel` has two sections, `Recent` and `All`
(`CommandPaletteModel.cpp:125-137`), and lists parameterised rows flat ("Change Profile: dark").

**Grouping.** A `CommandCategory` column is added to the catalogue (`command/CommandCatalog.hpp`).
Rows are grouped by Pane, Tab, Window, Layouts, Blocks, Profiles and Hosts.

**Drill-in.** Parameterised actions collapse into one row per action, marked `›`. Its children
are the existing source rows.

**Layout.** 640 wide, with rows 32 high. The description shows in a footer, for the selected row
only.

**Shortcuts and matching.** Shortcut text comes from `ShortcutStyle` (Phase 0 Task 2). Fuzzy matching is
unchanged.

**Tests.**
- The grouping table.
- The drill-in model.
- Filtering across drill-in children.

### 3.11 Step 3e — Small overlays

**Size indicator.** Today it is a QML item at `TerminalPane.qml:130-166`, reading "Size: C x L".
It becomes a `Toast` pill: "100 × 30", or "13 pt" on a font zoom.

**Scrollbar.** The `ScrollBar` is at `SessionChrome.qml:135`.
- A new `ScrollBarMode { Overlay = 0, Always }`, with Overlay as the default (zero means default).
- An overlay scrollbar hides at the bottom of the scrollback, **unless marks exist above the
  viewport**. Marks keep it visible.
- This is a pure decision with its own table.

**Link target pill.**
- While the follow modifier is held, the link target shows in a pill. It sits in the pane
  header's trailing area, or at the top right of a single pane, and only ever over blank cells.
- `hyperlink_hover_tooltip: false` becomes the default in this same PR.
- The hyperlink tooltip (`SessionChrome.qml:64-90`) goes.

**Hint labels.**
- Today `Terminal::applyHintOverlay` (`Terminal.cpp:5395`) writes labels into cells, using the
  palette's `hintLabel` and `hintMatch`.
- They are restyled: the label uses the cursor colour, non-matches dim to 45 %, and a typed prefix
  switches the label to the selection colour.
- They stay in cells, because labels sit over the match's first cells by definition. That is the
  only allowed exception to principle 5, and is stated as such.

**Tests.**
- The scrollbar visibility table.
- The pill placement, which must only ever be over blank cells.
- The hint label colours.

### 3.12 Step 3f — Command blocks

**Seams.**
- The gutter is built by `Terminal::fillGutter` (`Terminal.cpp:3937`) into `RenderGutterCell`s,
  and drawn by `Renderer::renderGutter` (`vtrasterizer/Renderer.cpp:1006`) with private-use
  glyphs from `BoxDrawingRenderer` (`:437-440`, `:2795-2804`).
- The sticky header comes from `fillStickyHeader` (`:4361`) and is drawn by `renderStickyHeader`
  (`:1064`).
- Block colours come from `blockStatus*` (`ColorPalette.hpp:371-425`).

**Rail.**
- `RenderGutterCell` gains a `RailSegment { None = 0, Head, Body, Tail }` and an outcome.
- `Renderer::renderGutter` draws a 3-pt rounded bar, plus a head glyph: ✗ for a failure, ▸ while
  running, ✓ on hover.
- The rail sits in `space.blockGutter` (22 pt), after line numbers and timestamps.
- The private-use glyphs remain for the TUI row.

**Status chip (pure, `vtbackend/shell/BlockChipPlacement.hpp`).**
- The chip is drawn only over blank cells to the right of the prompt.
- If it does not fit, it collapses to the rail's head glyph.

**Block menu.**
- Hovering a block lights its rail and shows a `⋯` button.
- The menu opens through the 1b presenter, from a `command/BlockMenu` table: Copy Output, Copy
  Command, Rerun, Open in Pager, Collapse Output.
- The block tooltip (`SessionChrome.qml:97-128`) moves into the menu's header **in this PR**.

**Keyboard and announcements.**
- ⌘↑ / ⌘↓ focus a block.
- ⌃↩ or ⇧F10 opens the block's menu.
- Changes are announced through `Announcer`.

**Fold chip.** "80 lines hidden ⌄", with a 24-pt hit area.

**Sticky header.** It moves into the pane header, or into the title band for a single pane.
`RenderStickyHeader` stays as the data source.

**Scrollbar ticks.** Failures are solid ticks, running blocks hollow ones, and search matches
yellow.

**Tests.**
- Building rail segments.
- The chip placement table: with an RPROMPT, without one, and in a narrow pane.
- The block menu table.
- Binding the sticky header data.

### 3.13 Step 4b — Settings sections and controls

**Seams.**
- The field groups are data: `profileFieldGroups()` (`SettingsController.cpp:266`) and
  `globalFieldDescriptors()` (`:1551`).
- The save model today:
  - profiles are drafts until Save, which writes `profiles/<name>.yml`;
  - globals write `settings.yml` at once;
  - `contour.yml` is never written.

**Sections.**
- The sections are General, Appearance, Keyboard, Shell Integration and Profiles, then an "Edit
  contour.yml…" entry.
- A table maps each existing field group to its section, and a test checks that every group
  appears exactly once.

**One save model.**
- Profiles apply live. Each commit — a control change, or leaving a field — writes the side file
  atomically, debounced by 300 ms.
- "Revert Changes" restores the snapshot taken when the page opened.
- Read-only profiles keep "Duplicate to Edit".
- `setGlobalField` returns `std::expected<void, SettingsError>` instead of `bool`.

**Controls.**
- A font picker, using the locator's family list.
- Colour wells, with a hex field and a contrast readout.
- Steppers and sliders for numbers.
- Segmented controls.
- The editors for colour schemes, link patterns and the status line become pages.

**`option_as_alt`.**
- It becomes `Off | Left | Right | Both`.
- It is `ConfigEntry<bool>` today (`Config.hpp:1137`). The YAML boundary still accepts the old
  boolean and converts it, so the enum lives inside the boundary.

**Live preview.** Profile pages get an offscreen `TerminalDisplay` that renders a canned
transcript.

**Tests.**
- The section table, with every group exactly once.
- Live apply, Revert and debounce.
- Reading the old `option_as_alt` boolean.
- The read-only profile rules.

### 3.14 Step 5a — Windows 11

**Mica.**
- Set `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_TABBEDWINDOW` on the window.
- Call `DwmExtendFrameIntoClientArea` with margins of `-1`. Today it uses a 1-px top margin
  (`extendFrame`, `Win32WindowFrame.cpp:133-140`, `cyTopHeight = 1` at `:139`).
- The QQuickWindow gets an alpha-capable surface. The band is cleared to transparent, and the
  panes are painted opaque.
- If any of that fails, or under Reduced transparency, the band is `Solid`.

**Snap Layouts.**
- `WM_NCHITTEST` returns `HTMAXBUTTON` over the maximise button.
- The filter must also handle:
  - `WM_NCLBUTTONDOWN` and `WM_NCLBUTTONUP`: maximise or restore, and swallow the default
    handling;
  - `WM_NCMOUSEMOVE` and `WM_NCMOUSELEAVE`: drive the QML button's hover state.
- All of these go into the hit-test table and its tests.

**Window chrome.**
- Caption buttons are 46 wide, drawn with Segoe Fluent glyphs.
- The tabs join the content area.
- The profile dropdown stays.
- The Fluent row's `quickControlsStyle` is `"FluentWinUI3"`.

**Menus.** Choose between in-scene menus and QML `Popup.Native` with sub-menus built before
`open()`. The choice is recorded in the 5a plan, after a spike.

**Backends.** The Windows backends for accessibility preferences and platform appearance.

### 3.15 Step 5b — Linux

**Style module.** One `ContourLinux` Quick Controls module, in `src/contour/styles/ContourLinux/`,
built the way `ContourTui` is.

**Header bar.**
- It is 46 high, with the tabs inline, a search button and a menu button.
- The window buttons follow the desktop's layout (§2.4).
- Linux uses client-side decorations by default.
- With `show_title_bar: true`, the server draws the frame. The tab row stays, and only the window
  buttons move out of it. There is no `window_decorations` key; it is `show_title_bar`,
  `Config.cpp:1599`.

**Backends.**
- The portal backends for appearance and accessibility.
- Accent and light/dark come from the portal.

**Tests.**
- The button-layout parser, for both the GNOME string and the KWin keys.
- Parsing portal replies.
- The style module loads every control.

### 3.16 Phase 6 — Moments (ten PRs, 6.1 to 6.10)

| # | Moment | Design | Platform seam |
|---|---|---|---|
| 6.1 | Shell integration card | After 3 prompts without OSC 133, a dismissible card. Install appends the one-line source to the rc file, showing it first, using `contour generate integration` | `IShellRcEditor` |
| 6.2 | Restore | Daemon restore (PR #2176), plus a "Restored · session ended hh:mm" divider and one Toast | none |
| 6.3 | Exited process | Rerun of the `ExitedFooter` from Step 2 when the daemon restarts | none |
| 6.4 | Drag and drop | Dropped files insert shell-escaped paths. A tab dropped on a pane's outer 20 % splits it (pure `DropZonePolicy`) | none |
| 6.5 | Finished elsewhere | A notification for a command that ran 10 s or more while unfocused, plus the Done or Error badge | macOS `UNUserNotificationCenter` `.mm`, only when running from an `.app` with a bundle identifier (otherwise `TrayNotifier`) |
| 6.6 | Secure input | When echo is off, a lock badge, plus `EnableSecureEventInput` on macOS | `ISecureInput` |
| 6.7 | Quick terminal | A global hotkey opens a window sliding down from the top, full width × 40 %, in Minimal density | `IGlobalHotkey`: macOS `RegisterEventHotKey`, Windows `RegisterHotKey`, Linux GlobalShortcuts portal. The window is `Qt::Tool | Qt::WindowDoesNotAcceptFocus` (Qt maps it to a non-activating `NSPanel`) |
| 6.8 | Keyboard to chrome | ⌃F6 / Ctrl+F6 cycles focus: terminal → tabs → toolbar. A test checks that every chord in the default bindings is unique | none |
| 6.9 | Bell, dock, taskbar | The visual bell is a tab badge plus a dock bounce or taskbar flash. The dock and taskbar badge counts tabs that are Blocked | existing `TaskbarProgressSink` backends, extended with a badge count |
| 6.10 | IME | Candidate window anchored at the cursor cell; inline preedit with a 1-pt underline | existing `ImeQueryRect` |

## 4. Risks

| Risk | Mitigation |
|---|---|
| Native `QMenu` behaves differently from in-scene menus | macOS only. A token selects the presenter. The pure `MenuTree` is tested |
| The title-band material flickers, or loses the shadow | The supported `titlebarAppearsTransparent = NO` route comes first. The shadow is re-asserted. A `Solid` fallback exists |
| Per-pane properties cost redraws | They notify only on change, and process polling runs only for visible headers |
| Regex search on 10 000 lines blocks the GUI thread | Chunked walk with the lock released between chunks (3b), and a compiled regex |
| The `Git` placeholder reads files every frame | It is cached on cwd plus HEAD mtime, and invalidated on OSC 133 D |
| Live-apply Settings writes too often | Debounced 300 ms, with atomic writes |
| Partial user configs change look | The `DefaultsEdition` gate (§2.7), the opt-in banner and a fixture test |
| Snap Layouts break the QML caption buttons | The NC mouse messages are handled, with a hit-test table |
| Mica needs a transparent surface | An alpha format, with `Solid` as the fallback |
| PRs too large to review | Each step is one PR, and Phase 6 is ten |

## 5. What each implementation plan must contain

- **A header** linking the canvas, the design record, this document, and the **status board**
  (the private artifact `/status-board` publishes).
  - Every agent that continues a plan updates the board at each state change of a task: started,
    in review, landed, blocked.
- **Tasks in TDD order**: a failing test, the implementation, the passing test, then a commit.
  - Each task names its own test filter.
  - The full suite runs in CI.
- **The TUI row's rendering** for every visual task.
- **A review focus list**: where the step could silently break existing users.

## 6. Round-1 review: structural changes

| Finding | Change |
|---|---|
| Phase 2 used a Toast that Phase 3 builds | Kit primitives move to 1a |
| The Settings pseudo-tab coexisted with the native "Settings…" | 4a (the window shell) comes before 1c |
| Partial configs would silently change look | §2.7 `DefaultsEdition` gate |
| Acceptance relied on inspecting images | Property tests decide acceptance; the snapshots are artefacts |
| Phases 1, 3 and 6 were several PRs each | Split into 1a–1e, 3a–3f and 6.1–6.10; old "3b" renamed 3f |
| The hover tooltip was turned off before its replacement | The flip moves to 3e, with the pill |
| `safeAreaMargins` was not a drop-in replacement for the inset | 1d keeps the inset; a spike decides the height source |
| `QMenu` is native only on macOS | Windows stays in-scene until 5a |
| Search ran on the GUI thread | 3b chunks the walk |
| Six surfaces had no phase | Each one now has a step |
| Reduce Motion had no seam for terminal durations | §2.3 |
