# The native chrome

This page is for maintainers who change the application's own chrome: the title band, the tab well,
pane cards, menus, dialogs, the Settings window and the small overlays around the grid. It records the
design rules the chrome is built to and where each one lives in the tree, so a change can respect them
without rediscovering them. The code wins over this page; where they disagree, fix the page.

## Purpose and principles

The chrome looks native on each platform and is still one component tree. macOS came first, then
Windows 11, then **one** Linux design that serves every desktop; there is no GNOME-specific or
KDE-specific variant. A fourth look, the Terminal (TUI) style, is a first-class row of the same
components, not an afterthought.

1. **The chrome is data-driven.** Every extent, radius, colour role, type size, duration and glyph comes
   from a token row (`src/contour/config/UiStyle.hpp`, `src/contour/config/ChromeTokenGroups.hpp`) or
   from the terminal's colour scheme. QML interprets tokens and holds no literals. Two lint gates
   enforce this: `scripts/check-qml-colors.py` (no hex colours, no all-literal `Qt.rgba`) and
   `scripts/check-qml-sizes.py` (no numeric size literals, with a shrinking baseline in
   `scripts/qml-size-literal-baseline.txt`). Both run under `ctest -L lint`.
2. **There is one component tree.** Platforms differ through token rows plus a handful of layout
   switches (`LayoutTokens`: caption buttons, tab shape, header-bar arrangement, profile dropdown,
   Settings layout, and a few more). There are no per-platform QML forks. A visual change lands in the
   Terminal row in the same change, not later.
3. **Pure decisions are headless.** Which badge wins, whether a pane shows a header, which menu rows
   exist, where the find bar goes, how a status segment collapses: each is a dependency-free header
   under `src/contour/window/`, `src/contour/command/`, `src/contour/session/` or `src/vtbackend/`,
   with Catch2 tables. QML renders the answer and decides nothing.
4. **The OS is reached through interfaces.** Everything that asks the OS lives behind an interface in
   `src/contour/platform/` with a null implementation and a test double, injected at construction.
5. **Nothing is drawn over non-blank cells.** Badges, chips, toasts, banners, link pills and block
   chips go into chrome bands, pane headers, gutters or blank cells only. The one exception is the
   hint labels, which by definition sit over the first cells of the match they label.
6. **Glass and material are chrome only**, never behind terminal text. The chrome's light or dark
   appearance follows the terminal background (`theme: terminal`, the default; the luminance
   threshold is in `colorSchemeOfBackground`), unless the configuration carries a dual light/dark
   colour scheme, in which case it follows the OS.
7. **Existing users keep their look.** New defaults sit behind `DefaultsEdition`
   (`src/contour/config/DefaultsEdition.hpp`), see below.
8. **Contrast is a rule, not a taste.** Chrome text must reach 4.5:1 against what it sits on
   (`MinimumTextContrast`), marks such as dots and status glyphs 3:1 (`MinimumMarkContrast`). The
   built-in `contour-dark` and `contour-light` schemes keep every colour except black and white at
   4.5:1 against their background, bright black on dark at 5.0. The OS selected-row colour pair is
   exempt only while it already reads: `legibleHighlight` in `src/contour/config/HighlightPair.hpp`
   keeps the OS hue and swaps the text to white or black when the pair falls under 4.5:1. This is the
   Breeze fallback: KDE's default `#3DAEE9` under white reads about 2.5:1, under black 8.6:1, so the
   text flips and the fill stays.

Other fixed rules of the look:

- Status colours are semantic. "Running" and "working" use the accent, never yellow or green; yellow
  means search. In the Terminal row, yellow appears as a glyph only, never as a fill.
- Tabs live in the title band. Settings is a separate window (`Cmd+,` / `Ctrl+,`), never a tab.
- A single pane runs edge to edge with no focus frame. Pane cards with headers appear only once a tab is
  split, and focus is shown by dimming the others, not by an outline (except under increased contrast).
- Shortcuts render per platform: modifier glyphs on macOS, `Ctrl+Shift+P` elsewhere
  (`src/contour/command/Shortcut.hpp`).
- Command blocks never draw UI over non-blank cells.

## Architecture: rows, token groups and styles

### UiStyle, ChromeStyle and the token table

`UiStyle` is what the user asks for (`ui_style: native | terminal`). `resolveChromeStyle(UiStyle,
HostPlatform)` turns it into a `ChromeStyle`, the row the chrome is actually painted with:

| Row | Where it applies |
|---|---|
| `MacOS` | native on macOS |
| `Fluent` | native on Windows |
| `ContourLinux` | native on KDE Plasma and every other host |
| `Terminal` | `ui_style: terminal`, on every OS |

Enumerator order is the row order of the table. Two tables are indexed by it:

- `UiStyleTokenTable` in `UiStyle.hpp`, the older `UiStyleTokens` (units, corner radius, border width,
  popup shadow and scrim, tab metrics, glyph vocabulary, `quickControlsStyle`).
- `ChromeTokenRows` in `ChromeTokenGroups.hpp`, one `ChromeTokenRow` per style, composed of groups:
  `size`, `space`, `radii`, `type`, `motion`, `elevation`, `material`, `contrast`, `tui`, `layout`,
  `tint`, `colors` (a `ChromeColorSource` per `ChromeColorRole`), `statusInk`, `dim`, `minimal`,
  `blockChips` and `settings`.

Both carry a `static_assert` that every `ChromeStyle` has exactly one row, so a sixth row is one table
edit: add the enumerator, add a row to each table, and the compiler names whatever else is missing. The
exception is the Qt Quick Controls half, below.

Rules for token groups:

- A group is added by the first change that consumes it, never ahead of need. Component extents quoted
  in a design are named tokens (`size.toast`, `size.palette`, `space.blockGutter`, ...), so the
  size-literal gate never has to flag them.
- Enums used by tokens have an explicit underlying type and put the default at zero.
- A colour is a *source*, not a value: an OS palette slot, a scheme slot, a derivation. The chrome
  resolves it against the active scheme at provider-build time (`chromeSchemeColorsOf`), so a scheme
  change recolours the chrome without touching QML.
- Counted-in-cells extents (`LengthUnit::Cell`) are how the Terminal row snaps to the grid. Never
  mix a pixel literal into a cell row.

### Resolution and the provider

`resolveChromeTokens(style, accessibilityPreferences, density, materialSupport)` applies, in order:
the Minimal density sub-row, Reduce Motion (durations collapse to a short cross-fade, travel to zero),
Reduce Transparency or an OS that cannot draw the material (materials become `Solid`), and Increased
Contrast (separators double, the active pane card gets a ring, the pane dim drops). It is a pure
`constexpr` function with a test row per rule (`ChromeTokenGroups_test.cpp`).

`UiStyleProvider` (`src/contour/window/UiStyleProvider.hpp`) exposes the resolved row to QML as grouped
`Q_PROPERTY` objects (`chromeStyle.size.tab`, ...). Its properties are constant. When any input changes
(an OS accessibility or appearance change, a reload, a light/dark flip, the density) `ChromeStyleHost`
builds a **new** provider and republishes it; the old one is `deleteLater()`ed because the change often
arrives from a signal the old provider is still on the stack for. Never add change notification to
provider properties.

### The Qt Quick Controls styles

Qt resolves a Quick Controls style by name, once per process, before the first control exists. So a
row also names its style (`quickControlsStyle`) and each style needs real control files:

| Row | Controls style |
|---|---|
| `MacOS` | `Fusion`, with the chrome drawn by the shared QML |
| `Fluent` | `FluentWinUI3` |
| `ContourLinux` | `src/contour/styles/ContourLinux/` |
| `Terminal` | `src/contour/styles/ContourTui/` |

The two first-party modules are built the same way (`src/contour/CMakeLists.txt`) and must implement
the same set of controls; their `*_test.cpp` (`ContourTuiStyle_test.cpp`, `ContourLinuxStyle_test.cpp`)
load every control. A change of `ui_style` takes effect on the next start for this reason.

The hand-drawn chrome in `src/contour/qml/` never learns which style is active.

### Config and defaults

Config keys you will touch: `ui_style`, `chrome_density` (`comfortable | minimal`), `theme`
(`terminal` follows the background), `show_title_bar`, `defaults_edition`, `scrollbar.mode` and the
window-control style (`src/contour/config/WindowControlStyle.hpp`).

`DefaultsEdition { Classic = 0, Native2026 }` exists because a configuration that sets only
`profiles.main.font` would otherwise inherit every new default. A fresh install is `Native2026` by
virtue of the generated file carrying `defaults_edition: native2026`. A file that sets nothing is
`Classic`. The edition is read by a pre-scan (`settings.yml` wins over `contour.yml`) because `Config`
and the seed `TerminalProfile` are constructed before the YAML is parsed. Classic users get a one-time
offer (`NewDefaultsOffer.hpp`, `window/NewDefaultsNotice.hpp`): Use New writes `native2026` through the
Settings session, Keep Mine remembers the dismissal. Every default lives in one table per edition, not
in scattered `if`s.

## The window chrome model

### Title band and tab well

The top of every window is one band (`TitleBar.qml`, `TabStrip.qml`, `TabItem.qml`) whose arrangement
is decided by pure code: `window/TitleBandLayout.hpp` (which frame, window mode, tab-strip gate and bar
edge combine to which layout), `window/HeaderBarLayout.hpp` (items, menus and slots of the Linux header
bar), `window/TabWellLayout.hpp` and `window/TitleBandGeometry.hpp`. Keep the keying of those tables
`(shape-or-any, frame, gate-or-any, position)`: new cases are new rows.

- **Tabs** are pills in a well; the badge slot is resolved by `window/TabBadge.hpp` with the fixed
  priority Blocked > Error > Working > Done > Output. OSC 7501 program status, the unseen-output dot and
  the finished-command badge share that one slot.
- **Host chip** (`HostChip.qml`, `window/HostChipState.hpp`): Connected, Reconnecting, Disconnected.
  The daemon drift chip lives in the band's trailing area, never over the cells.
- **Notices**: `window/NoticeModel.hpp` queues `Toast` (a transient pill) and `Banner` (a band below the
  title band that pushes the content down once). Config reload errors, "Copied N lines" and restore
  summaries all arrive here. Toasts persist while hovered and are announced politely through
  `platform/Announcer.hpp`.
- **Dialogs** are `KitDialog.qml`, an in-scene sheet-styled `Dialog` (a Quick Controls `Dialog` never
  becomes an NSWindow sheet, so it is styled as one). Placement is a token (`layout.dialog`).
- **Menus** are data: `command/MenuTree.hpp`, `command/ContextMenu.hpp`, `command/TabMenus.hpp`,
  `command/MenuBarTable.hpp`, `command/BlockMenu.hpp`. `layout.menu` picks the presenter:
  `Native` realises the tree as a `QMenu` (an NSMenu on macOS), `InScene` uses the QML menus. The
  `QMenu` realisation is only exercised by display tests; the tree builder is unit tested.
- **Command palette** (`CommandPalette.qml`, `command/CommandPaletteTree.hpp`): rows grouped by category,
  parameterised actions collapse to one drill-in row.
- **No title bar and full screen**: with `show_title_bar: true` the server frame is used and the tabs sit
  in a strip below it; in full screen the band reveals when the pointer reaches the top edge.

### Panes, densities and cards

There are two densities, Comfortable and Minimal. `chrome_density` selects the `minimal` sub-row, which
`resolveChromeTokens` copies over the pane extents, so a density change rebuilds the provider.

- `PaneNode.qml` draws splits; the visual gap is `space.paneGap` and the grab area `space.splitHit`
  is a transparent item that overlaps the cards and never changes layout. The window-size solver reads the
  same gap, so the two cannot drift (`vtworkspace::DefaultPaneGap`).
- `window/PaneHeaderPolicy.hpp` decides, from density, held modifier, pointer, focus, pane count and size,
  whether a header shows its name, name and working directory, or nothing, and whether its actions show.
  An inline header that would leave less than one cell row of content hides.
- Per-pane data (`displayWorkingDirectory`, `foregroundProcessName`, `programStatus`) is exposed by
  `session/PaneProxy.hpp`. Process-name polling runs only while a header is visible.
- A reconnecting pane dims by the dim token and shows a static toast pill. A disconnected or exited pane
  gets `ExitedFooter.qml` in its bottom chrome, never over the cells.
- The Terminal row draws everything in cells: one-row headers, `|` and `-` cell lines for splits, inactive
  panes at reduced opacity, a hard one-cell shadow, menus in-scene on every OS, the find bar in the pane
  header row, and `ui_font_family` / `ui_font_size` applying to the chrome font.

### Window controls and frame

Window controls are chosen by `WindowControlStyle` (Auto matches the host) and only drawn while the
window is frameless. On macOS AppKit's real traffic lights are kept; the Terminal row snaps them to the
first cells of the bar row. Windows draws 46-wide caption buttons with Segoe Fluent glyphs. Linux uses
client-side decorations by default with a 46 high header bar; the button order follows the desktop's
layout. With `show_title_bar: true` the server draws the frame and only the window buttons move out of
the tab row. The Terminal row on Linux and Windows draws coloured cell dots from the scheme.

## Platform integration seams

Every seam has the same shape: a **decision** that is pure and tested everywhere, and an **adapter** that
asks the OS, behind an interface with a null implementation. The adapters that cannot run on CI are
thin; the decision is never inside them. Look in `src/contour/platform/`:

| Concern | Interface | Notes |
|---|---|---|
| Reduce motion, transparency, contrast | `IAccessibilityPreferenceSource` | Cocoa, Windows and portal backends |
| Accent colour, corner radius, button layout | `IPlatformAppearance` | the Linux backend reads the XDG Settings portal; fallback accent `#3584E4` |
| Title band material | `ITitleBandMaterial` | null implementation and a Solid fallback |
| Global hotkey | `IGlobalHotkey` | Carbon hot key, `RegisterHotKey`, GlobalShortcuts portal |
| Secure input | `ISecureInput` | `EnableSecureEventInput` on macOS |
| Shell rc editing | `IShellRcEditor` | shows the line before it appends it |
| Notifications | `IUserNotificationCenter`, `Notifier` | decision vs adapter: `NotificationRouter` vs `FreeDesktopNotifier` |
| Dock, taskbar, launcher counts | `TaskbarProgressSink` and friends | the tally is pure (`window/TaskbarAttentionTally.hpp`) |

Platform notes that are easy to get wrong:

- **macOS.** The window keeps the full-size content view. As shipped the title band is the client's own
  tint over a compact toolbar (`Solid`): with AppKit's transparent title bar the `NSTitlebarView` took
  the band's clicks. Glass remains a possible follow-up as a within-window `NSVisualEffectView` behind
  the Qt content. `nativeControlsInset()` stays the horizontal inset for the traffic lights; the band's
  height comes from the safe area. Alpha-capable surfaces can clear `hasShadow`, so it is re-asserted
  after the frame is applied. On macOS `QWindow::setBaseSize` hard-resizes the `NSWindow`, and the
  OpenGL RHI backend maps a window that never composites; both are avoided in the code and must stay so.
  The application menu bar is built from `command/MenuBarTable.hpp` once per app and rebuilt on reload.
- **Windows 11.** Mica through `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_TABBEDWINDOW` with the frame extended
  into the client area on an alpha-capable surface; the band is `Solid` when that fails or under
  Reduce Transparency. `background.blur` turns the whole window Acrylic and replaces Mica. Snap Layouts
  need the maximise button to answer `WM_NCHITTEST` with `HTMAXBUTTON`, plus the non-client mouse
  messages to drive the QML button's hover; all of it sits in the hit-test table
  (`platform/CaptionHitTest.hpp`) so it is testable off Windows. Menus stay **in-scene** on Windows: a
  native menu is an `HMENU`, which draws no swatch row, ignores the terminal scheme the chrome follows
  and has no supported dark mode. A Windows contrast theme overrides `theme: terminal`. The Settings
  window keeps the system title bar with Mica. The caption, Mica and Settings objects check the platform
  when called, not at construction, because construction runs before `QApplication` exists.
- **Linux.** One design for all desktops. Accent and light/dark come from the XDG Settings portal through
  `PortalCall`; the button layout comes from the first source that answers (GNOME's
  `button-layout`, KWin's `ButtonsOnLeft`/`ButtonsOnRight`, else close on the right). Window shadows are
  drawn client-side per windowing system (`X11WindowShadow`, `WaylandWindowShadow`) using Breeze's own
  parameters so a Contour window and a Breeze one cast the same shadow. Selected rows use the legible
  highlight rule from the principles above.

## The Settings window and its one save model

Settings is a separate `QQuickWindow`, one per application (`window/SettingsWindowHost.hpp`), opened by
`OpenConfiguration` and by the application menu, and focused if already open. Its sections (General,
Appearance, Keyboard, Shell Integration, Profiles, then an "Edit contour.yml..." entry) come from the
table in `window/SettingsSections.hpp`; a test requires every field group to appear in exactly one
section, so a new group that is not mapped fails the build's tests. Field groups are data
(`window/SettingsFields.hpp`, `profileFieldGroups`, `globalFieldDescriptors`), and the controls are the
`Kit*` components, so a new setting is a row, not a layout.

There is **one** save model:

- Every commit (a control change, leaving a field) is staged and written by a debounced save, 300 ms,
  atomically, to the side file it belongs to: `profiles/<name>.yml` for profiles, `settings.yml` for
  globals. `contour.yml` is never written.
- The controller keeps a draft of `settings.yml`, so two global changes inside one debounce both land.
- "Revert Changes" restores every side file as it was when the session first touched it, and never
  writes anything itself. Closing Settings or quitting flushes what is staged.
- Read-only profiles offer "Duplicate to Edit".
- Fallible setters return `std::expected<void, SettingsError>`, not `bool`.
- Profile pages preview against an offscreen `TerminalDisplay` rendering a canned transcript
  (`window/SettingsPreview.hpp`).
- `option_as_alt` is an enum (`Off | Left | Right | Both`); the YAML boundary still accepts the old
  boolean and converts it, so the enum stays inside the boundary.

## The moments

Each moment is a decision header plus, where the OS is involved, one interface from the table above.

- **Shell integration card.** After three prompts without OSC 133 a dismissible card offers to append the
  one-line source to the shell rc file, showing the line first. The edit goes through `IShellRcEditor`
  and is reversible.
- **Restore.** A restored session gets a "Restored" divider and one toast; a process that ended while the
  daemon was down shows the exited footer instead of silently restarting.
- **Drag and drop.** Dropped files insert shell-escaped paths. A tab dropped on a pane's outer 20 %
  splits it (`window/DropZonePolicy.hpp`).
- **Finished elsewhere.** A command that ran for 10 s or longer while the window was unfocused raises a
  notification and sets the Done or Error badge. On macOS this requires running from an app bundle with
  a bundle identifier, otherwise the tray notifier is used.
- **Secure input.** When the PTY echo is off a lock badge shows (`SecureInputPolicy`,
  `SecureLockPlacement`), and macOS secure event input is enabled for as long as it holds.
- **Quick terminal.** A global hotkey slides a window down from the top (full width, 40 % of the height,
  Minimal density). It is a tool window that does not accept focus on show, which Qt maps to a
  non-activating panel on macOS. The hotkey, its refusals and the window each have a policy header
  (`window/QuickTerminalPolicy.hpp`, `window/HotkeyRefusals.hpp`).
- **Keyboard to chrome.** `Ctrl+F6` cycles focus terminal, tabs, toolbar (`window/ChromeFocus.hpp`,
  `window/ChromeKeys.hpp`). A test requires every default chord to be unique.
- **Bell, dock, taskbar.** A visual bell is a tab badge plus a dock bounce or taskbar flash. The dock,
  taskbar and Linux launcher show how many tabs are Blocked (`TaskbarAttentionTally`).
- **IME.** The candidate window anchors at the cursor cell and preedit is drawn inline with a one-point
  underline (`display/ImeQueryRect.hpp`).

## Terminal-side pieces

These live below the chrome but are part of the same design:

- **Status line** is on by default, cell-drawn and scheme-coloured. Its placeholders (`Cwd`, `Git`,
  `LastCommand`, `Selection`, `Scroll`, ...) are each four edits (struct, variant, `ItemTraits`,
  serializer) in `vtbackend/screen/StatusLineBuilder.hpp`; role colours (`Color=accent|success|...`) and
  `Hide=Insert` are flags. `Git` reads `.git/HEAD` through a narrow filesystem seam off the terminal's
  lock; a collapse step drops low-priority segments when the line is too narrow.
- **Command blocks** draw a rail in the gutter (`RailSegment { None = 0, Head, Body, Tail }`), a status chip
  only over blank cells (`vtbackend/shell/BlockChipPlacement.hpp`), a block menu from
  `command/BlockMenu.hpp`, a fold chip and scrollbar ticks. The glyph column stays for the Terminal row.
  Running uses the accent, `ColorPalette::blockStatusColor`'s fallback is blue.
- **Find bar** has literal/regex and whole-word options; an invalid regex is an `std::expected` error
  shown in the count slot; the walk is chunked so the lock is released between chunks.
- **Permissions** are one two-button dialog with a remember checkbox, `(PermissionDecision,
  PermissionMemory)` instead of two bools (`session/Permission.hpp`).
- **Hint labels, link target pill, scrollbar** follow the "blank cells only" rule, hint labels aside.

## Testing strategy

- **Pure decisions** are Catch2 tables next to the header.
- **QML components** are loaded against mocks (`test/Qml*_test.cpp`, `test/QmlMocks.hpp`) and
  asserted through `objectName` and `findChild`; every element a test needs gets an `objectName`.
- **Acceptance is property tests, not images.** `test/MainWindowQml_test.cpp` asserts token-driven values
  (the tab well's height equals `chromeStyle.size.tabWell`, a radius equals its token). A change that moves
  geometry on purpose updates those assertions in the same commit.
- **Style variants in ctest.** The QML tests run once per controls style, because Qt picks it once per
  process: `contour_gui_test`, `_fluent`, `_contourlinux` and `_contourtui`, selected with
  `CONTOUR_TEST_CONTROLS_STYLE`. A control added to one style module must be added to both.
- **Widgets in tests.** `contour_gui_test` builds a `QGuiApplication`, not a `QApplication`, so code that
  needs `QMenu` splits into a pure tree builder (tested) and a thin realisation (display smoke tests).
- **Snapshots are review artefacts.** `test/ChromeSnapshot_test.cpp` and `test/ChromeSnapshotPlan.hpp`
  render named scripts (`single-tab`, `split-three`, `palette`, `settings`, ...) per chrome row,
  edition and appearance; `scripts/run-chrome-snapshots.sh <contour_gui_test> <out-dir>` runs one pass per
  controls style under a headless compositor and checks the PNG count against `manifest.tsv`. CI uploads
  them. Nothing compares them against a baseline.
- **Display passes.** Tests tagged `[display]` drive a real `TerminalDisplay` through RHI and are
  skipped offscreen. `scripts/run-display-tests.sh` runs them under headless weston (CI) or an existing
  X11 session. Run them once per body of work, not per edit.
- **Pixel tests for in-grid visuals.** Anything drawn in or over the grid (rails, chips, the cursor, hint
  labels) gets a `[display]` test that grabs a real frame and samples pixels
  (`test/DisplayPixels.hpp`, `display/CommandBlockRendering_test.cpp`,
  `display/InGridVisualsRendering_test.cpp`). Chrome snapshots cannot see the grid.
- **Offscreen verification of QML.** Use `QT_QPA_PLATFORM=offscreen` and `grabWindow()`; OS-level screen
  capture needs a permission nobody may be there to grant.
- **Accessibility.** Every control sets `Accessible.role` and `Accessible.name`; a table-driven test walks
  the chrome's accessible tree (`test/Accessibility_qml_test.cpp`).

## Decisions that are easy to undo by accident

- Putting a literal in QML "just this once". The gates exist because nobody notices the third one.
- Adding change notification to a provider property instead of rebuilding the provider.
- Adding a style row without the controls module (or only to one of the two token tables).
- Switching Windows to native menus. They cannot show the swatch row or follow the terminal scheme.
- Drawing a chip, toast or pill over cells that might hold text. Use the placement policy, which
  collapses to a glyph when the blank space is not there.
- Using green or yellow for "running". It is the accent.
- Moving the OS highlight pair into a surface without `legibleHighlight`.
- Making a new default unconditional rather than a `Native2026` row; existing configurations change look.
- Reading the OS from a constructor that runs before `QApplication`, or from a decision header.
- Letting the pane-gap token and the window-size solver disagree.
- Treating snapshot PNGs as acceptance, or skipping the Terminal row because "it is only the TUI".
- Re-enabling the OpenGL RHI backend or `setBaseSize` on macOS.
- Changing a config enum's spelling after release; the spelling is the file format.
