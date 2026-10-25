# Native Redesign — Step 4b (Settings Sections, Controls, One Save Model, `option_as_alt` per Side, Live Preview) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25) at HEAD `2c1a166c`. Step 4a is done; Steps 1c, 1d, 2 and 3a–3f run
before 4b (plans in `docs/superpowers/plans/`; 3e has no plan file at this HEAD). Where 4b touches code those steps
add or change, the task names it as their plans do and carries a **Depends on Step <id>** note. A pre-flight scan
re-verifies every reference before Task 1; line numbers are at `2c1a166c`, and a later task finds code an earlier one
moved by its name.

**Goal:** Turn the Settings window into the canvas's System-Settings-style window — a sidebar of five sections
(General, Appearance, Keyboard, Shell Integration, Profiles) plus "Edit contour.yml…", grouped inset lists of rows with
trailing native-looking controls, per-row layouts from token rows — over one save model in which every change applies
live and is written atomically (debounced 300 ms) through the injected `FileSystem`, "Revert Changes" undoes this
session's edits, contour.yml profiles stay read-only behind "Duplicate to Edit"; with `option_as_alt` per side
(`off|left|right|both`), a live preview on every profile page, and the colour-scheme, link-pattern and status-line
editors as pages.

**Architecture:**
- **The save model is Qt-free and owns every write.** `config::SettingsEditSession` stages the bytes of GUI-owned side
  files (`settings.yml`, `profiles/<name>.yml`, `colorschemes/<name>.yml`), snapshots each file's bytes the first time
  the session touches it, flushes through a `config::SaveScheduler` (300 ms debounce; a `QTimer` adapter in production,
  a manual one in tests), refuses to overwrite a file that changed on disk since it last saw it, and reverts by putting
  the snapshot bytes back. `config::GuiConfigStore` becomes byte-level (`read`/`write`/`remove` of a `SideFile`) and
  `FileGuiConfigStore` writes through `core::platform::FileSystem` with the durable create–write–sync–replace–sync
  sequence. `SettingsController` stages into the session; the app's reload is the one place the controller refreshes.
- **Settings is tables.** `window/SettingsSections.hpp` maps every field group (profile and global) to its section, once;
  `window/SettingsControls.hpp` picks each row's control from its value type and range, with a per-row override;
  `window/ProfileRules.hpp` decides what a profile allows (edit, delete, rename, duplicate, set default) from its origin
  and the lock. The descriptor tables move out of `SettingsController.cpp` into `window/SettingsFields.{hpp,cpp}` and
  each descriptor names its `SettingsGroup`.
- **Per-platform layout is a token row.** `LayoutTokens::settings` (`SettingsLayout { InsetGrouped = 0, Cards,
  BoxedList, Cells }`) is the one settings layout switch (§1.2); `settingsLayoutTraits()` turns it into the header
  arrangement, the row shape and the stepper arrangement. A new `SettingsTokens` group holds every extent; new
  `ChromeColorRole`s hold every colour; glyph tokens draw the Terminal row's `[x]`, `(•)`, `━━●──` and `▴▾`.
- **Kit controls are one component each** (`KitSwitch`, `KitSegmented`, `KitStepper`, `KitSlider`, `KitPopUp`,
  `KitColorWell`, `KitFontPicker`), drawn from tokens on every row, the Terminal row included.
- **The preview is a real terminal.** A `SessionRole::Preview` session over a `vtpty::MockPty` seeded with a canned
  transcript, owned by `window::SettingsPreview`, re-applied with `SettingsController::previewConfig()` (the live
  configuration with the draft in place) on every draft change, before anything is written; the profile page shows it
  in a `TerminalDisplay`.
- **`option_as_alt` is an enum inside the boundary.** `config::OptionAsAlt { Off = 0, Left, Right, Both }` with a
  ConfigEnum table; the YAML reader still takes the old boolean (`true` → `Both`, `false` → `Off`); the key boundary
  reads which Option key is down from macOS's device-dependent modifier bits.

**Tech Stack:** C++23, Qt 6.11 (Quick, Quick Controls), QML, yaml-cpp, Catch2 v3, Python 3 (lint), CMake build tree
`out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.13 (`:719-761`) binds, with what
it cites: §1 (principles, `:28-64`: data-driven chrome, one component tree with "settings layout" one of the five
layout switches, pure decisions headless, OS behind interfaces), §2.1 (token rows, `:68-126`), §2.3 (Reduce Motion,
`:139-167`), §2.6 (testing, `:206-226`), §2.7 (DefaultsEdition, `:228-284`), §4 ("Live-apply Settings writes too
often | Debounced 300 ms, with atomic writes", `:829-842`), §5 (`:844-854`). §3.13's citations at this HEAD:
`profileFieldGroups()` `SettingsController.cpp:266` (as cited), `globalFieldDescriptors()` `:1551` (as cited);
`option_as_alt` is `TerminalProfile::optionKeyAsAlt`, `ConfigEntry<bool, documentation::OptionKeyAsAlt>`,
`Config.hpp:1277` (cited `:1137`), read at `Config.cpp:1758`, documented at `ConfigDocumentation.hpp:3208-3226`,
`:3419`, consumed at `SessionInput.cpp:632-645`, offered as the Settings row `option_key_as_alt`
(`SettingsController.cpp:489-492`). Design record: `docs/superpowers/specs/2026-10-23-native-redesign-design.md`
("Settings is a separate window (⌘, / Ctrl+,)", `:16`; contrast requirement, `:125-129`). Canvas
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `mac-settings`, `win-settings`, `linux-settings`,
`kit-components`, `surfaces` (Settings editors), `statusline` (the Settings line), `tui` (Settings rows) and
`defaults-all` (`option_as_alt` row) are in `.superpowers/design/canvas/project/`. Ledgers: Step 4a
(`docs/superpowers/ledgers/2026-10-25-native-redesign-step-4a.md`: D2, the double refresh, the `windowController`
rename, the Terminal row's ~20 px band), Phase 0 (ruling R7, "Match terminal"), Step 3a's SDD ledger (the Settings
status-line page). Plans index: `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

## Status board and handoff (every step)

This run is tracked on a status board: <https://claude.ai/artifact/YZUVcC1vdaWaLGJLsdS2AX>. The
owner asked for the link to live here so that any agent continuing the work can find the board and
keep it current. The page is kept with `/status-board` (the `contour-workflows` plugin) on the machine doing the work; its source is local to that working tree and never committed.
- **Keep it up to date** with `/status-board update`, which republishes to the link above
  (`/status-board` remembers the link per working tree). The board belongs to the owner's account, so a
  session signed in as the owner can read and publish it; the design canvas, the owner's copy of
  Yaraslau's, is in the same account.
- **Every agent that continues this work updates the board.** Do it
  in the same turn as each state change: a step or task started, in review, committed, landed,
  blocked or unblocked, a plan written, or a ruling.
- A different worktree has a board of its own. Never write to another worktree's board.
  Report to the session that owns the board instead.
- If this worktree has no board, ask the owner rather than creating a second board.

## Global Constraints

- **The spec, verbatim (§3.13):**
  - "The sections are General, Appearance, Keyboard, Shell Integration and Profiles, then an "Edit contour.yml…"
    entry." "A table maps each existing field group to its section, and a test checks that every group appears
    exactly once."
  - "Profiles apply live. Each commit — a control change, or leaving a field — writes the side file atomically,
    debounced by 300 ms." "'Revert Changes' restores the snapshot taken when the page opened." "Read-only profiles keep
    'Duplicate to Edit'." "`setGlobalField` returns `std::expected<void, SettingsError>` instead of `bool`."
  - "A font picker, using the locator's family list." "Colour wells, with a hex field and a contrast readout."
    "Steppers and sliders for numbers." "Segmented controls." "The editors for colour schemes, link patterns and the
    status line become pages."
  - "`option_as_alt` … becomes `Off | Left | Right | Both`." "The YAML boundary still accepts the old boolean and
    converts it, so the enum lives inside the boundary."
  - "Profile pages get an offscreen `TerminalDisplay` that renders a canned transcript."
  - Tests: "The section table, with every group exactly once." "Live apply, Revert and debounce." "Reading the old
    `option_as_alt` boolean." "The read-only profile rules."
- **Canvas copy, exact:** "Search settings" (macOS, Linux) / "Find a setting" (Fluent); "Edit contour.yml…";
  "Revert Changes" (disabled until something changed); "Duplicate to Edit"; "Use Option as Meta" with help "The other
  Option key keeps typing accents like é and ü." and segments "Off · Left · Right · Both"; "Dim inactive panes" / "How
  far a pane without focus steps back." shown as "15 %"; "Modified · Reset" beside an overridden value; the preview
  caption "Live preview — {family} {size} · {scheme}" (e.g. "Live preview — JetBrains Mono 13 · Contour Dark"); the
  contrast readout "{ratio} : 1" with one decimal (the 4.5 : 1 rule); theme option labels "Match terminal",
  "Match system", "Dark", "Light" (Phase 0 R7).
- **Canvas values, exact** (`mac-settings`, `win-settings`, `linux-settings`, `kit-components`, `tui`). Native rows in
  points, the Terminal row in cells. Values marked † are derived where the board shows no number; the pre-flight
  re-measures them against the board and corrects this table in its findings.

  | Token | MacOS | Fluent | ContourLinux | Terminal |
  |---|---|---|---|---|
  | `layout.settings` | `InsetGrouped` | `Cards` | `BoxedList` | `Cells` |
  | `settings.sidebarWidth` | 230 | 290 | 270 | 24 |
  | `settings.sidebarRow` | 28 | 36 | 38 | 1 |
  | `settings.iconTile` (edge; 0 = no tile) | 20 | 20 | 0 | 0 |
  | `settings.iconGlyph` (icon edge) | 14† | 16† | 16 | 1 |
  | `settings.searchField` (height) | 28 | 32 | 34 | 1 |
  | `settings.headerBar` (page header height) | 52 | 64 | 46 | 1 |
  | `settings.listMaxWidth` (0 = fill) | 640 | 0 | 640 | 80 |
  | `radius.settingsGroup` | 10 | 4 | 12 | 0 |
  | `settings.row` | 44 | 68 | 52 | 1 |
  | `settings.rowWithHelp` | 52 | 68 | 60 | 2 |
  | `settings.rowPadding` (horizontal) | 14 | 16 | 14 | 1 |
  | `settings.cardGap` (between rows of a group) | 0 | 4 | 0 | 0 |
  | `settings.groupGap` (between groups) | 16 | 20 | 22 | 1 |
  | `settings.groupTitleGap` (title to list) | 6 | 6 | 10 | 0 |
  | `settings.control` (pop-up, button, field height) | 22 | 32 | 34 | 1 |
  | `settings.switchWidth` × `settings.switchHeight` | 32 × 19 | 40 × 20 | 40 × 22 | 3 × 1 |
  | `settings.slider` (track width) | 160 | 160 | 160 | 11 |
  | `settings.stepperField` (value field width) | 72 | 72† | 72† | 6 |
  | `settings.colorWell` (edge) | 22† | 32† | 34† | 2 |
  | `settings.previewHeight` | 104 | 128 | 120 | 6 |
  | `settings.previewWidth` (0 = fill) | 0 | 360 | 0 | 0 |
  | `radius.settingsPreview` | 10 | 6 | 12 | 0 |
  | `type.settingsLabel` | 13 | 14 | 14 | 0 (the cell font) |
  | `type.settingsHelp` | 11 | 12 | 12.5 | 0 |
  | `type.settingsGroupTitle` | 13 (semibold) | 14 (semibold) | 14 (bold) | 0 |
  | `type.settingsPageTitle` | 15 (semibold) | 28 (semibold) | 15† | 0 |

  `settingsLayoutTraits()`: `InsetGrouped` → header `Toolbar`, rows `Grouped`, stepper `Stacked`; `Cards` → header
  `Breadcrumb`, rows `SeparateCards`, stepper `Stacked`; `BoxedList` → header `SplitHeaderBar`, rows `Grouped`,
  stepper `SideBySide` (− / +); `Cells` → header `Toolbar`, rows `Grouped`, stepper `Stacked` (`▴▾`).
  Terminal-row glyph tokens (`tui` board): switch on `[x]`, off `[ ]`; segment on `(•)`, off `( )`; slider fill `━`,
  knob `●`, track `─`; stepper `▴▾`; colour well `██`; sidebar cursor row = the scheme's selection colours. Native rows
  leave these glyphs empty and draw shapes.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the 1c, 1d, 2, 3a–3f SDD ledgers):
  - No colour or size literal in chrome QML (`check_qml_colors`, `check_qml_sizes`); a new file's baseline is 0 and a
    baseline only goes down (`scripts/qml-size-literal-baseline.txt`). Deleted files leave the baseline.
  - Text contrast at least 4.5:1 on contour-dark and contour-light for every new text role (labels, help, the
    selection, "Modified · Reset", the contrast readout itself).
  - Reduce Motion: every new transition (the switch knob, the sidebar selection, the page change) reads
    `chromeStyle.motion` and collapses to `ReducedMotionCrossFadeMs` (100) or none under `MotionStyle::CrossFade`.
  - Every visual surface is a snapshot row; every visual task renders the Terminal row
    (`CONTOUR_TEST_CONTROLS_STYLE=ContourTui`). TestApp is Classic (R14) and passes `nullAppearanceServices()`.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
  - The Settings window keeps the OS's frame (1d R-h); it is one per app and survives close (4a).
- **AGENT.md applies in full:** data-driven tables (`SettingsSectionRows`, `SettingsControlRules`,
  `ProfileActionRules`, `settingsLayoutTraits`, the token rows, `OptionAsAltTable`, `SnapshotScripts`); dependency
  injection (the `FileSystem`, the `SaveScheduler`, `IFontFamilySource`, the preview's session factory — never
  `std::filesystem` or `QTimer` reached from a decision); configuration at construction (`SettingsEditSession`'s store,
  scheduler and editability; the controller's services struct); `enum class` over `bool` with zero the default;
  `std::expected` for fallible calls; Doxygen on every new public declaration; `.hpp` with `#pragma once`; zero
  warnings, no `NOLINT`; `slots`, `signals` and `emit` are Qt macros, never identifiers. QML-facing
  `Q_PROPERTY`/`QVariantMap` booleans (`hasChanges`, `editable`, `overridden`) are the documented carve-out, as are
  `Config`'s own boundary types.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`):
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds in one tree.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding. Format touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`; check new or
  touched `.qml`, `.txt`, `CMakeLists.txt` with `editorconfig-checker <files>`. **Never clang-format a `.qml`.**
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value,
  and every table row names every field (`ChromeTokenRows`, `SnapshotScripts`, every new table).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type. Counts in
  tests come from tables (`SettingsSectionCount`, `SettingsGroupCount`, `SettingsSectionRows.size()`,
  `SnapshotScripts.size()`).
- **Qt-free stays Qt-free:** everything new in `config/` and the `window/*.hpp` decisions (`SettingsSections`,
  `SettingsControls`, `ProfileRules`, `SettingsSearch`, `ColorContrastReadout`) includes no Qt header.
- **Never edit `vendor/core-cpp`.** The working tree carries other agents' edits (`ChromeTokenGroups.hpp`,
  `UiStyle.hpp`, `UiStyleProvider.*`, `ChromeTokenObjects.*` and their tests at plan time): stage only the task's own
  files, by name, never `git add -A` or a directory.
- **Commits:** one per task, message ending with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`; never `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 19).

## Review Focus

1. **An edit while an editor changes the same file on disk.** The user has `profiles/main.yml` open in vim, saves it,
   and moves a Settings slider before the 300 ms flush. Expected: Settings never overwrites bytes it has not seen; the
   flush sees the file changed since its last read, writes nothing, takes the disk's version as the new base (so a
   later Revert does not clobber it either), re-seeds the page from it and says "profiles/main.yml changed outside
   Settings; showing that version." Pinned in Task 2 (`SettingsEditSession.aFileChangedOnDiskIsNotOverwritten`) and
   Task 3 (`SettingsController: a side file changed on disk re-seeds the draft and says so`).
2. **Revert after a reload.** The user edits a profile in Settings, then edits `contour.yml` by hand (live_config
   reloads), then presses "Revert Changes". Expected: only the side files this session touched go back to their bytes
   at the session's start (a file it created goes away); the hand edit in contour.yml stays; Revert is disabled
   afterwards. Pinned in Task 2 (`SettingsEditSession.revertRestoresOnlyTheFilesItTouched`) and Task 3
   (`SettingsController: Revert after a contour.yml reload keeps the hand edit`).
3. **A locked or read-only configuration.** `gui_config_locked: true`; a profile defined in contour.yml; a config
   directory the user cannot write. Expected: locked — every control is disabled, nothing is staged, a banner says why,
   no Revert; a contour.yml profile — its controls are disabled with "Defined in contour.yml · Duplicate to Edit";
   unwritable — the commit fails with the OS's reason in the banner, the old file is intact with no temporary left
   behind, and the control shows the value still in effect. Pinned in Task 1
   (`FileGuiConfigStore: a refused replace keeps the old file and leaves no temporary`), Task 3 (the locked and
   unwritable cases) and Task 10 (the banners and disabled controls in QML).
4. **One profile, two windows' worth of state.** The profile is in use by panes in two terminal windows, and possibly by
   a second Contour process with its own Settings open (`spawn_new_process`). Expected: one commit reaches every pane
   using that profile in every window, plus the preview; a second process's flush over a file this one wrote is a
   ChangedOnDisk, never a silent clobber. Pinned in Task 2
   (`SettingsEditSession.twoSessionsOverOneFileNeverClobber`) and Task 4
   (`ContourGuiApp: a Settings commit reaches the panes of every window using that profile`).
5. **Keyboard alone, and VoiceOver.** No pointer: Tab into the sidebar, ↑/↓ through the sections and profiles, Return
   or Tab into the page, Tab through every control in reading order, Space toggles a switch, ←/→ move a segmented
   control, ↑/↓ step a stepper or slider, ⌘F / Ctrl+F focuses search, Escape leaves a popover. VoiceOver hears each
   sidebar row as a list item with its name, and each control as its role with the row's label as name and the help as
   description. Pinned in Task 9 (each kit control's keys, role and name) and Task 10
   (`SettingsPage: the keyboard alone reaches every section and every control`,
   `accessibility: every settings row's control carries its label and help`).

Also pinned, beyond the five: closing the Settings window or quitting with a pending edit flushes it (Task 4); a value
the loader would reject is refused before it is staged (Task 3); a Settings window closed on a second screen reopens
there after a restart (Task 5); `option_as_alt: true` in an old file means both keys (Task 11).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Everything applies live (owner, 1).** §3.13 says "Profiles apply live"; the canvas says "Everything applies live
  and is written atomically". One `SettingsEditSession` stages profile side files, colour-scheme side files and
  `settings.yml` alike, under one 300 ms debounce. Save, Save As and the scheme editor's Save buttons go. A text field
  commits on Return or on leaving it; every other control on change.
- **R-b. "The snapshot taken when the page opened"** is per side file: the bytes (or absence) of each GUI-owned file
  the first time this session touches it, from the moment the Settings window first opens in this run of the app.
  Closing and reopening the window keeps the session (the window survives close, 4a); restarting the app starts a new
  one. Revert writes each touched file's snapshot back (removing one that was absent), then reloads. contour.yml is
  never written, so a hand edit there is never reverted.
- **R-c. Disk wins (owner, 6).** Before writing a file the session reads it; if its bytes differ from what the session
  last read or wrote, the session writes nothing to it, adopts the disk's bytes as both its last-seen bytes and its
  snapshot, drops what was pending for it, and reports `SettingsErrorKind::ChangedOnDisk`. The controller re-seeds the
  draft from the reloaded configuration.
- **R-d. A failed write rolls the draft back.** On `WriteFailed` the pending bytes are dropped and the controller
  re-seeds the draft from the configuration still in effect, so no control shows a value that is not on disk.
- **R-e. One refresh per save (4a parked).** The controller no longer calls `refresh()` after `_apply()`:
  `ContourGuiApp::onConfigurationReloaded()` (`ContourGuiApp.cpp:1891-1895`) is the one refresh, for Settings' own
  writes and for hand edits alike. `refresh()` never touches the open draft; only `editProfile()`, `ChangedOnDisk`,
  `WriteFailed` and Revert re-seed it.
- **R-f. Read-only profiles.** A profile is editable only when its origin is `SideFile` and the page is not locked.
  `MainConfig` profiles say "Defined in contour.yml", `Builtin` ones "Built in"; both offer "Duplicate to Edit", which
  writes `profiles/<name> copy.yml` ("<name> copy 2", … when taken) from the merged profile and opens it. Delete and
  Rename exist for side-file profiles only; Set as Default for every profile. The rules are one table
  (`ProfileActionRules`).
- **R-g. "+ / − / …".** "+" writes a new side-file profile "New Profile" (then "New Profile 2", …) seeded from the
  default profile, through the session (so Revert removes it) and opens it. "−" asks (KitDialog) and deletes the
  selected side-file profile, through the session. "…" opens a menu: Duplicate, Rename…, Set as Default.
- **R-h. The sections and their groups.** Profile groups render on each profile page (Profiles section). Global fields,
  which have no groups today, get groups: General — *Windows* (`reflow_on_resize`, `snap_to_rows`,
  `remember_window_position`, `spawn_new_process`, `tab_switch_on_horizontal_wheel`), *Command palette*
  (`command_palette_recent_count`, `word_delimiters`), *Advanced* (`read_buffer_size`, `grapheme_clustering`,
  `text_scaling_method`, `renderer.gpu`); Appearance — *Window chrome* (`theme`, `ui_style`, `chrome_density` (Step 2),
  `window_control_style`, `ui_font_family`, `ui_font_size`), *Tab bar* (`tab_bar_position`, `tab_bar_visibility`);
  Keyboard — *Key bindings* (the read-only list); Shell Integration — *Gutter* (`gutter.*`), *Command blocks*
  (`command_blocks.*`), *Folding* (`folding.*`). The profile group *Terminal* loses `option_key_as_alt` to a new
  profile group *Keyboard*; the colour-scheme selection (today a QML-only section) becomes the profile group *Colors*.
  A field a later step adds (Step 2's `chrome_density`, Step 3e's scrollbar mode if it adds a row) takes the group its
  plan's section implies; the coverage test fails until it has one.
- **R-i. Controls (owner, 3 for the font picker).** `bool` → Switch; `enum` with at most four options whose labels fit
  → Segmented, else Pop-up; `int` → Stepper; `double` with a range of at most 1.0 → Slider shown in percent; other
  `double` → Stepper with one decimal; `string` → Text field; the font family rows → Font picker; colour slots → Colour
  well. A descriptor may name its control explicitly (`option_as_alt` → Segmented). The font picker lists
  `platform::IFontFamilySource::monospaceFamilies()`, the bundled "JetBrains Mono" first: FontLocator has no family list
  (`text_shaper/FontLocator.hpp:58-87`; `all()` returns sources), so the Qt implementation asks `QFontDatabase` for
  fixed-pitch families, which Qt fills from the same CoreText/DirectWrite/fontconfig the locators use.
- **R-j. Per-platform layouts now (owner, 2).** All four rows get their board values in this step: Settings has no
  "today's geometry" to keep (§2.1's Phase-5 rule is about the main window's band, and 4a only hosted the old page).
  The FluentWinUI3 control style stays Step 5a's and the ContourLinux style module Step 5b's; until then those rows
  draw the kit controls from their tokens on Fusion.
- **R-k. Sidebar icons (owner, 4).** Each section row carries a `SettingsIcon`; the glyph per icon comes from a glyph
  table (monochrome symbols from the chrome font, no emoji), drawn on a coloured tile where `settings.iconTile` > 0.
  SF Symbols and Segoe Fluent icons are left for Steps 5a and 6. Profile rows show the profile's tab colour as the
  tile, or the accent.
- **R-l. The status-line page (owner, 5).** The indicator template is per profile, so the page is a profile sub-page
  (Profiles › main › Status Line). Appearance › Status Line opens it for the default profile. Its live preview is the
  profile preview with the indicator status line shown (the canvas's "drawn by this same builder").
- **R-m. Shell Integration's other rows** — install status per shell (Step 6.1) and the quick-terminal hotkey (6.7) —
  are not in this step.
- **R-n. Window state (4a D2).** On close, Settings saves its normal geometry, screen name, maximised state and current
  page to `GuiState::settingsWindow` (`gui-state.yml`); the first show of a run restores them through
  `settingsWindowGeometry()`'s `previous` (clamped to the screens there are). Settings reopens on its last page.
- **R-o. The Terminal row's band (4a).** The page fills the window (`anchors.fill`), and its last row stretches, so no
  band shows below a page that is not a whole number of cells high.
- **R-p. `option_as_alt` (Global Constraints).** The YAML key stays `option_as_alt` and gains the tokens
  `off|left|right|both`; the reader also takes yaml-cpp's boolean spellings (`true|yes|on|y` → `Both`,
  `false|no|n` → `Off`; `off` is the token). The writer writes the token. Both editions default to `Off`
  (`defaults-all`: "Off (per side …) … Approved"), so the DefaultsEdition table gains no row; a test pins that both
  editions give `Off`. The Settings row's key becomes `option_as_alt` (it was `option_key_as_alt`, a key no file has).
- **R-q. Which Option key is down** comes from `QKeyEvent::nativeModifiers()`, which on macOS carries the NSEvent's
  `modifierFlags`: `NX_DEVICELALTKEYMASK` (`0x20`) and `NX_DEVICERALTKEYMASK` (`0x40`). Off macOS the setting is
  ignored, as today. Task 12's display check confirms the bits on this Mac.
- **R-r. The preview.** One preview session per Settings window, created on the first profile page shown, 80 × 6 cells
  (Terminal row: the page width in cells × 6), never in any tab or `_sessionsById`, closed with the window's last
  close of the app. Its PTY swallows writes; it takes no keyboard focus. Snapshots show the preview frame over a stub
  (the snapshot scene has no session manager); its content is pinned by Task 13.

### Non-goals

- Editing key bindings (Keyboard stays a viewer); shell-integration install status (6.1); the quick-terminal hotkey
  (6.7); SF Symbols / Segoe icons (R-k); FluentWinUI3 and ContourLinux control styles (5a, 5b).
- Writing contour.yml, ever. "Edit contour.yml…" opens it in the OS editor (`OpenConfiguration{Editor}`).
- Settings windows per terminal window; the window stays one per app.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/config/AtomicFileWrite.{hpp,cpp}` (modify) | `writeFileDurably` through a `FileSystem` | 1 |
| `src/contour/config/GuiConfigStore.{hpp,cpp}` (modify) | `SideFile`, byte-level store over a `FileSystem` | 1 |
| `src/contour/config/SettingsEditSession.{hpp,cpp}` (create) | `SettingsError`, `SaveScheduler`, staging, flush, snapshot, Revert, conflicts | 2 |
| `src/contour/test/ManualSaveScheduler.hpp` (create) | Test double for `SaveScheduler` | 2 |
| `src/contour/window/QtSaveScheduler.hpp` (create) | `QTimer` adapter | 3 |
| `src/contour/window/SettingsController.{hpp,cpp}` (modify) | Live apply through the session, Revert, Duplicate, sections, controls, preview config | 3, 6, 7, 11, 13, 15–17 |
| `src/contour/window/NewDefaultsNotice.cpp`, `src/contour/ContourGuiApp.{hpp,cpp}` (modify) | The byte-level store; wiring; flush on close and quit; one refresh | 1, 4, 5 |
| `src/contour/config/GuiStateStore.{hpp,cpp}`, `window/SettingsWindowHost.{hpp,cpp}`, `window/SettingsWindowRules.{hpp,cpp}` (modify) | Settings' remembered frame and page | 5 |
| `src/contour/window/SettingsFields.{hpp,cpp}` (create, moved from the controller) | The descriptor tables, each with its group and control | 6, 7 |
| `src/contour/window/SettingsSections.hpp`, `SettingsSearch.hpp` (create) | Section/group table; search | 6 |
| `src/contour/window/SettingsControls.hpp`, `ProfileRules.hpp` (create); `config/Config.hpp` (GuiTheme table) | Control rules; profile action rules; option labels | 7 |
| `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `window/ChromeTokenObjects.{hpp,cpp}`, `window/UiStyleProvider.{hpp,cpp}` (modify) | Settings tokens, layout switch, colours, glyphs | 8 |
| `src/contour/platform/FontFamilySource.{hpp,cpp}` (create) | `IFontFamilySource`, null and Qt implementations | 9 |
| `src/contour/qml/Kit{Switch,Segmented,Stepper,Slider,PopUp,ColorWell,FontPicker}.qml` (create) | The kit controls | 9 |
| `src/contour/qml/SettingsPage.qml` (rewrite), `SettingsWindow.qml` (modify), `SettingsSidebar.qml`, `SettingsGroup.qml`, `SettingsRow.qml`, `SettingsSectionPage.qml`, `ProfilePage.qml` (create); `SettingRow.qml`, `SettingsSection.qml`, `SettingsNavItem.qml`, `SettingsListItem.qml` (delete) | The window | 10 |
| `src/contour/config/OptionAsAlt.hpp` (create); `Config.{hpp,cpp}`, `ConfigDocumentation.hpp` (modify) | The schema change | 11 |
| `src/contour/input/KeyMapping.{hpp,cpp}`, `input/CMakeLists.txt`, `session/SessionInput.cpp` (modify) | The per-side decision at the key boundary | 12 |
| `src/contour/session/PreviewTranscript.hpp`, `window/SettingsPreview.{hpp,cpp}` (create); `session/TerminalSession.hpp`, `TerminalSessionManager.{hpp,cpp}` (modify) | The preview session | 13 |
| `src/contour/qml/SettingsPreview.qml` (create), `ProfilePage.qml` (modify) | The preview hero | 14 |
| `src/contour/window/ColorContrastReadout.hpp` (create); `qml/ColorSchemesPage.qml` (create); `ColorSchemeEditor.qml`, `ColorSlotField.qml` (delete) | Colour schemes page | 15 |
| `src/contour/qml/LinkPatternsPage.qml` (create); `LinkPatternList.qml` (delete) | Link patterns page | 16 |
| `src/contour/qml/StatusLinePage.qml` (create); `StatusLineIndicatorEditor.qml` (modify) | Status line page | 17 |
| `src/contour/test/ChromeSnapshotPlan.hpp`, `ChromeSnapshotScene.hpp` (modify) | Snapshot rows | 18 |
| `src/contour/CMakeLists.txt`, `src/contour/config/CMakeLists.txt`, `src/contour/platform/CMakeLists.txt`, `scripts/qml-size-literal-baseline.txt`, `metainfo.xml` (modify) | Registration, baselines, notes | as needed, 19 |

### Shared files

- `src/contour/window/SettingsController.cpp`: Step 2 adds the `chrome_density` row (`globalEnumFieldFromTable`), Step 3a
  the indicator bridge (`indicatorColorRoles`, role colours), Step 3f nothing. **Depends on Steps 2 and 3a:** Task 6 moves
  their rows with the rest and gives them groups.
- `src/contour/config/ChromeTokenGroups.hpp`, `UiStyle.hpp`, `ChromeTokenObjects.*`, `UiStyleProvider.*`: every earlier
  step appends groups, roles and glyphs. Task 8 appends after whatever is last at execution.
- `src/contour/test/ChromeSnapshotPlan.hpp`/`ChromeSnapshotScene.hpp`: append scripts after whatever is last.
- `src/contour/session/TerminalSession.hpp`/`TerminalSessionManager.*`: 3b, 3c, 3f edit them; Task 13 adds one
  `SessionRole` enumerator and one factory function.
- `src/contour/session/SessionInput.cpp`: 3f adds block-focus key routing; Task 12 edits the `#ifdef __APPLE__` branch
  at `:632-645` only.

---
## Phase A — One save model

### Task 1: Side files are written durably through the injected `FileSystem`

**Files:**
- Modify: `src/contour/config/AtomicFileWrite.{hpp,cpp}` — a second function beside `atomicWriteFile` (`.hpp:30`).
- Modify: `src/contour/config/GuiConfigStore.hpp:23-81`, `GuiConfigStore.cpp` — the byte-level interface.
- Modify: `src/contour/config/CMakeLists.txt` — `contour_config` links `core::platform` if it does not already (pre-flight).
- Modify (callers): `src/contour/window/SettingsController.cpp` (every `_store->save…`/`delete…`),
  `src/contour/window/NewDefaultsNotice.cpp` (`saveGuiSettings`), `src/contour/ContourGuiApp.cpp:2443` and `:2797`
  (construct with `_fileSystem`), `src/contour/test/SettingsFixtures.hpp:48`, `src/contour/test/ChromeSnapshotScene.hpp:356`,
  `src/contour/test/SettingsPageQml_test.cpp:292` (`UnwritableStore`), `src/contour/window/NewDefaultsNotice_test.cpp`.
- Test: `src/contour/config/GuiConfigStore_test.cpp` (create; add to `contour_test`, `src/contour/CMakeLists.txt:1153-1203`).

**Interfaces:**
- Produces (`contour::config`, `AtomicFileWrite.hpp`):
  `[[nodiscard]] std::expected<void, std::string> writeFileDurably(core::platform::FileSystem const& fileSystem, std::filesystem::path const& path, std::string_view content);`
  — `createDirectories(parent)`, remove a stale `.<name>.tmp`, `createNewFile(temp)`, `writeFile`, `syncFile`,
  `replaceFile(temp, path)`, `syncDirectory(parent)`, chained with `and_then` (the `SessionSnapshotStore.cpp:85-96`
  sequence); the temporary is removed on every failure.
- Produces (`GuiConfigStore.hpp`):
  - `enum class SideFileKind : uint8_t { GuiSettings = 0, Profile, ColorScheme };`
  - `struct SideFile { SideFileKind kind = SideFileKind::GuiSettings; std::string name {}; [[nodiscard]] auto operator<=>(SideFile const&) const = default; };`
  - `[[nodiscard]] SideFile guiSettingsFile();`, `[[nodiscard]] SideFile profileFile(std::string name);`,
    `[[nodiscard]] SideFile colorSchemeFile(std::string name);`
  - `[[nodiscard]] std::filesystem::path relativePathOf(SideFile const& file);` — `settings.yml`,
    `profiles/<sideFileName(name)>`, `colorschemes/<sideFileName(name)>`.
  - `class GuiConfigStore` (replaces the five typed methods):
    `[[nodiscard]] virtual std::expected<std::optional<std::string>, std::string> read(SideFile const& file) const = 0;`
    (nullopt: no such file), `[[nodiscard]] virtual std::expected<void, std::string> write(SideFile const& file, std::string_view bytes) = 0;`,
    `[[nodiscard]] virtual std::expected<void, std::string> remove(SideFile const& file) = 0;` (an absent file is success).
  - `FileGuiConfigStore(core::platform::FileSystem const& fileSystem, std::filesystem::path configDir);` — the
    `FileSystem` is borrowed and outlives the store (the app's `_fileSystem`, `ContourGuiApp.hpp:875`).
  - Callers write `store.write(profileFile(name), emitProfileYaml(profile))`,
    `store.write(colorSchemeFile(name), emitColorSchemeYaml(palette))`, `store.write(guiSettingsFile(), emitGuiSettingsYaml(settings))`.

- [ ] **Step 1: Write the failing tests** (`GuiConfigStore_test.cpp`, `[guiconfigstore]`, over
  `core::platform::testing::InMemoryFileSystem` and `vthost::testing::FaultyFileSystem`):
  - `relativePathOf` gives `settings.yml`, `profiles/main.yml`, `colorschemes/Contour Dark.yml` (the `sideFileName`
    spelling) for the three kinds.
  - `FileGuiConfigStore: a write creates profiles/ and reads back` — `write(profileFile("main"), "a: 1\n")`, then
    `read` gives `"a: 1\n"`; `read(profileFile("absent"))` gives `std::optional<std::string>{}`.
  - `FileGuiConfigStore: a refused replace keeps the old file and leaves no temporary` (Review Focus 3) — the file holds
    `"old"`; a `FaultyFileSystem` failing `FaultOp::Rename` to it; `write(…, "new")` returns an error containing
    "Permission denied"; the file still reads `"old"`; `listDirectory(configDir / "profiles")` holds one entry.
  - `FileGuiConfigStore: a failed write reports it and leaves no temporary` — `FaultOp::WriteFile`; same assertions.
  - `remove` of an absent file succeeds; of a present one makes `read` give nullopt.
- [ ] **Step 2: Run them and see them fail.** `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test`.
  Expected: compile failure on `SideFile`.
- [ ] **Step 3: Implement** `writeFileDurably`, the interface and `FileGuiConfigStore`, then move every caller onto
  `write`/`remove`. `UnwritableStore` returns `std::unexpected("Read-only file system")` from `write`/`remove` and
  `std::optional<std::string>{}` from `read`. The fixture and the snapshot scene construct with a
  `core::platform::NativeFileSystem` they own.
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_test "[guiconfigstore],[config]" && out/appleclang-debug/src/contour/contour_gui_test "[settings],[newdefaults],[contour][gui][qml][settings]"`
  Expected: PASS; the existing `[settings]` cases pass unedited apart from the store construction.
- [ ] **Step 5: Tidy, format, commit** (`config: GUI side files are written durably through the injected file system`).

---

### Task 2: `SettingsEditSession` — staging, debounce, snapshot, Revert, conflicts

**Files:**
- Create: `src/contour/config/SettingsEditSession.{hpp,cpp}` (add to `contour_config`).
- Create: `src/contour/test/ManualSaveScheduler.hpp`.
- Test: `src/contour/config/SettingsEditSession_test.cpp` (create; `contour_test`).

**Interfaces:**
- Consumes: Task 1's `GuiConfigStore`, `SideFile`, `relativePathOf`.
- Produces (`contour::config`, `SettingsEditSession.hpp`):
  - `enum class SettingsErrorKind : uint8_t { WriteFailed = 0, Locked, ReadOnly, ChangedOnDisk, InvalidValue, UnknownSetting, NotApplied, NameTaken, ReloadFailed };`
  - `struct SettingsError { SettingsErrorKind kind = SettingsErrorKind::WriteFailed; std::string message {}; };` —
    `message` is the sentence the page shows.
  - `class SaveScheduler { public: virtual ~SaveScheduler() = default; virtual void schedule(std::chrono::milliseconds delay, std::function<void()> fire) = 0; virtual void cancel() = 0; };`
    — `schedule` replaces any pending fire (a restart, which is what a debounce is).
  - `inline constexpr auto SettingsSaveDebounce = std::chrono::milliseconds { 300 };`
  - `enum class SettingsEditability : uint8_t { Editable = 0, Locked };`
  - `class SettingsEditSession` (configured at construction, not copyable):
    - `using ReloadCallback = std::function<std::expected<void, std::string>()>;`
    - `using ErrorSink = std::function<void(SettingsError const&)>;` — receives errors found by a timer-driven flush.
    - `SettingsEditSession(GuiConfigStore& store, SaveScheduler& scheduler, SettingsEditability editability, ReloadCallback reload, ErrorSink reportError);`
    - `[[nodiscard]] std::expected<void, SettingsError> stage(SideFile const& file, std::optional<std::string> bytes);`
      — nullopt removes the file; snapshots the file on first touch (R-b); schedules a flush after `SettingsSaveDebounce`;
      `Locked` → `SettingsErrorKind::Locked`, nothing staged.
    - `[[nodiscard]] std::expected<void, SettingsError> flush();` — writes every pending file (R-c conflict check per
      file, R-d drop on failure), then calls `reload()` once if anything was written or adopted; the first error is
      returned, the rest go to `reportError`. A flush with nothing pending does nothing and does not reload.
    - `[[nodiscard]] std::expected<void, SettingsError> revert();` — cancels the scheduler, drops pending, restores each
      touched file whose disk bytes still equal its last-seen bytes (a changed one is adopted, R-c), reloads once.
    - `[[nodiscard]] bool hasChanges() const noexcept;` — something pending, or a touched file whose last-seen bytes
      differ from its snapshot.
    - `[[nodiscard]] bool hasPending() const noexcept;`
  - `ManualSaveScheduler` (test): `schedule` records the delay and the callback; `[[nodiscard]] std::optional<std::chrono::milliseconds> pendingDelay() const;`
    `void fire();` (runs and clears it); `[[nodiscard]] int scheduleCount() const noexcept;`.

- [ ] **Step 1: Write the failing tests** (`[settingssession]`; a `FileGuiConfigStore` over an `InMemoryFileSystem`, a
  `ManualSaveScheduler`, a reload counter):
  - `SettingsEditSession.debounceCoalesces` — three `stage(profileFile("main"), …)` calls with `"a"`, `"b"`, `"c"`:
    nothing on disk, `pendingDelay() == 300ms`, `scheduleCount() == 3`; `fire()` → the file reads `"c"`, one reload.
  - `SettingsEditSession.revertRestoresOnlyTheFilesItTouched` (Review Focus 2) — `settings.yml` holds `"s0"`,
    `profiles/main.yml` `"p0"`, and `contour.yml` `"c0"`. Stage and flush `"p1"` into main, stage and flush a new
    `profiles/New Profile.yml`; write `"c1"` to `contour.yml` directly (the hand edit). `revert()`: main reads `"p0"`,
    `New Profile.yml` is gone, `contour.yml` reads `"c1"`, `settings.yml` reads `"s0"` (never touched, never written),
    `hasChanges()` is false, reloads counted 3.
  - `SettingsEditSession.aFileChangedOnDiskIsNotOverwritten` (Review Focus 1) — flush `"p1"`; write `"vim"` to the file
    directly; stage `"p2"`; `fire()`: the file reads `"vim"`; the error sink got `ChangedOnDisk` whose message contains
    `profiles/main.yml`; one reload; `revert()` afterwards leaves `"vim"`.
  - `SettingsEditSession.twoSessionsOverOneFileNeverClobber` (Review Focus 4) — sessions A and B over one file system;
    A stages `"a"` and flushes; B, which read `"p0"` when it staged, stages `"b"` and flushes → `ChangedOnDisk`, file
    reads `"a"`.
  - `SettingsEditSession.aFailedWriteDropsThePendingBytes` (Review Focus 3) — `FaultyFileSystem` failing the rename:
    `flush()` returns `WriteFailed` with "Permission denied"; `hasPending()` false; the file holds its old bytes.
  - `SettingsEditSession.lockedStagesNothing` — `Locked`: `stage` returns `Locked`, `pendingDelay()` empty.
  - `SettingsEditSession.revertWithNothingTouchedIsANoOp` — `revert()` succeeds, no reload.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test`. Expected: compile failure on `SettingsEditSession`.
- [ ] **Step 3: Implement.** Per touched file the session keeps `snapshot` and `lastSeen` (`std::optional<std::string>`)
  in a `std::map<SideFile, …>`, and `pending` beside it. Messages: "Couldn't save {path}: {reason}.", "{path} changed
  outside Settings; showing that version.", "Settings are locked by gui_config_locked in contour.yml.".
- [ ] **Step 4: Run them and see them pass.** Run: `out/appleclang-debug/src/contour/contour_test "[settingssession],[guiconfigstore]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`config: one Settings save model: debounced staging, snapshots, Revert, and disk wins`).

---

### Task 3: `SettingsController` applies live through the session

**Files:**
- Create: `src/contour/window/QtSaveScheduler.hpp` (header-only, a `QTimer` member, single-shot).
- Modify: `src/contour/window/SettingsController.hpp:22-324`, `SettingsController.cpp` (the constructor `:1937`, the
  draft and scheme invokables `:2096-2600`, `setGlobalField`/`resetGlobalField` `:2384-2424`, `setDefaultProfile` `:2327`).
- Modify: `src/contour/test/SettingsFixtures.hpp` (the new constructor; the fixture's apply reloads and then calls
  `controller->refresh()`, as `onConfigurationReloaded` does).
- Modify (QML callers, minimally, so the page keeps working until Task 10): `src/contour/qml/SettingsPage.qml:222`,
  `:363-381`, `:631`, `:787-815`, `:871-877`; `ColorSchemeEditor.qml:116-190`.
- Test: `src/contour/window/SettingsController_test.cpp` (existing `[settings]` cases that call `saveProfile`,
  `saveProfileAs` or `saveColorScheme` move to the live model), `src/contour/test/SettingsPageQml_test.cpp`.

**Interfaces:**
- Consumes: Task 2's `SettingsEditSession`, `SettingsError`, `SaveScheduler`.
- Produces (`window/SettingsController.hpp`):
  - `struct SettingsControllerServices { std::shared_ptr<config::GuiConfigStore> store {}; std::function<std::expected<void, std::string>()> reload {}; std::shared_ptr<display::IGpuInventory const> gpuInventory {}; std::shared_ptr<config::SaveScheduler> scheduler {}; };`
  - `SettingsController(ConfigAccessor config, SettingsControllerServices services, QObject* parent = nullptr);`
  - `Q_PROPERTY(bool hasChanges READ hasChanges NOTIFY changesChanged)` — "Revert Changes" is enabled by it.
  - C++ API: `[[nodiscard]] std::expected<void, config::SettingsError> setGlobalField(QString const& key, QVariant const& value);`,
    `[[nodiscard]] std::expected<void, config::SettingsError> resetGlobalField(QString const& key);`,
    `[[nodiscard]] std::expected<void, config::SettingsError> setProfileField(QString const& key, QVariant const& value);`,
    `[[nodiscard]] std::expected<void, config::SettingsError> setSchemeColor(QString const& key, QString const& color);`,
    `[[nodiscard]] std::expected<QString, config::SettingsError> duplicateProfile(QString const& name);` (R-f),
    `[[nodiscard]] std::expected<QString, config::SettingsError> createProfile();` (R-g).
  - QML wrappers (each reports a failure through `errorOccurred` and returns nothing): `Q_INVOKABLE void commitGlobalField(QString const& key, QVariant const& value);`,
    `Q_INVOKABLE void commitGlobalReset(QString const& key);`, `Q_INVOKABLE void commitProfileField(QString const& key, QVariant const& value);`,
    `Q_INVOKABLE void commitSchemeColor(QString const& key, QString const& color);`,
    `Q_INVOKABLE void duplicate(QString const& name);`, `Q_INVOKABLE void addProfile();`,
    `Q_INVOKABLE void revertChanges();`, `Q_INVOKABLE void flushPendingChanges();`.
  - Removed: `saveProfile`, `saveProfileAs`, `saveColorScheme`, `newProfile`, `newColorScheme` (a new scheme is
    `duplicateColorScheme(QString const& name)` → `Q_INVOKABLE void duplicateScheme(QString const& name)`), and the
    `bool` returns of `deleteProfile`, `renameProfile`, `deleteColorScheme`, `renameColorScheme`, `setDefaultProfile`
    (each becomes `std::expected<void, config::SettingsError>` in C++ plus a `Q_INVOKABLE void request…` wrapper:
    `requestDeleteProfile`, `requestRenameProfile`, `requestDeleteScheme`, `requestRenameScheme`, `requestDefaultProfile`).
  - `signals: void changesChanged();` beside the existing ones.
  - The controller rebuilds its session in `refresh()` when `gui_config_locked` changed (configuration, not state).

- [ ] **Step 1: Write the failing tests** (`SettingsController_test.cpp`, `[settings]`, over the fixture with a
  `ManualSaveScheduler`):
  - "a profile field applies after 300 ms, not before" — `commitProfileField("font_size", 14.0)` on a side-file profile:
    the side file is unchanged, `hasChanges()` true; `fire()`: the side file has `font_size: 14`, the reloaded
    configuration's profile has 14.
  - "one save refreshes the lists once" (4a's parked double refresh) — a `QSignalSpy` on `changed`: after `fire()`
    `spy.count() == 1`.
  - "setGlobalField refuses a value the loader would reject, before staging" —
    `setGlobalField("read_buffer_size", 0)` is `InvalidValue`, `pendingDelay()` empty; an unknown key is `UnknownSetting`.
  - "a contour.yml profile is read-only and Duplicate to Edit makes an editable copy" — `setProfileField` on a
    `MainConfig` profile is `ReadOnly`; `duplicateProfile("main")` gives `"main copy"`; after `fire()` that profile's
    origin is `SideFile` and its fields equal main's; a second duplicate gives `"main copy 2"`.
  - "Revert after a contour.yml reload keeps the hand edit" (Review Focus 2) — commit and flush a field; append a profile
    `extra` to contour.yml and reload; `revertChanges()`: the side file has its original bytes, `extra` still exists,
    `hasChanges()` false, the draft shows the original value.
  - "a side file changed on disk re-seeds the draft and says so" (Review Focus 1) — commit, flush; rewrite the side file
    with `font_size: 20` by hand; commit `font_size` 15; `fire()`: `errors.back()` contains "changed outside
    Settings", the draft's `font_size` is 20.
  - "a locked configuration stages nothing" (Review Focus 3) — `gui_config_locked: true`: every commit wrapper leaves no
    pending flush and reports "Settings are locked…".
  - "an unwritable directory rolls the control back" (Review Focus 3) — `UnwritableStore`: commit 15, `fire()`: the
    error names the reason, the draft's `font_size` is the value in effect, `hasChanges()` false.
  - "addProfile writes New Profile and Revert removes it" — `addProfile()`, `fire()`: `New Profile` listed, editable;
    `revertChanges()`: gone.
  - "a colour-scheme slot applies live" — `commitSchemeColor("background", "#101010")` on a side-file scheme, `fire()`:
    the scheme file has it.
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: compile failure on
  `SettingsControllerServices`.
- [ ] **Step 3: Implement.** Every setter edits the draft, emits `draftChanged`, then stages the draft's full YAML
  (`emitProfileYaml`, `emitColorSchemeYaml`, `emitGuiSettingsYaml`) for its side file. `ChangedOnDisk`, `WriteFailed`
  and Revert re-seed the open drafts with `editProfile(_editingProfile)` / `editColorScheme(_editingScheme)` after the
  reload. `refresh()` no longer re-seeds and the controller no longer calls it after a reload (R-e). QML: Save/Save As
  buttons go; rows call the wrappers.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[settings],[contour][gui][qml][settings]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, `editorconfig-checker` the QML, commit** (`settings: every change applies live, Revert undoes the session, setGlobalField says why`).

---

### Task 4: The app wires the save model; live apply reaches every window

**Files:**
- Modify: `src/contour/ContourGuiApp.{hpp,cpp}` — `settingsController()` (`:2790-2806`): services with
  `std::make_shared<config::FileGuiConfigStore>(_fileSystem, configDir)`, a `window::QtSaveScheduler`, and `reload` =
  `_sessionManager.reloadAllSessions()` plus `applyWindowControlStyle`; `onConfigurationReloaded` (`:1891-1895`) stays
  the one refresh; the app's `aboutToQuit` and `SettingsWindowHost::closeSettings()` call `flushPendingChanges()`.
- Modify: `src/contour/window/SettingsWindowHost.cpp:55-70` (close flushes; the comment on refresh-on-show).
- Test: `src/contour/test/ContourGuiApp_test.cpp`, `src/contour/window/SettingsWindowHost_test.cpp`.

**Interfaces:**
- Consumes: Task 3's `SettingsControllerServices`, `flushPendingChanges()`.
- Produces: `SettingsWindowHost::closeSettings()` flushes the controller before closing (documented).

- [ ] **Step 1: Write the failing tests.**
  - `ContourGuiApp_test.cpp`, `[contour][settings][liveapply]`: "a Settings commit reaches the panes of every window
    using that profile" (Review Focus 4) — a `TestApp` (Classic, `MockPtySessionFactory`) with two windows, one pane each,
    both on profile `main` defined by a side file; `settingsController().commitProfileField("font_size", 17.0)`, then
    `flushPendingChanges()`: both sessions' `profile().fonts.value().size.pt == 17`; a third window on another profile is
    unchanged.
  - `SettingsWindowHost_test.cpp`, `[settingswindow]`: "closing Settings flushes a pending edit" — commit, `closeSettings()`
    before any timer: the side file has the value.
  - `ContourGuiApp_test.cpp`: "quitting flushes a pending edit" — emit `QCoreApplication::aboutToQuit`: the side file has it.
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`; run the three. Expected: FAIL (values absent).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][settings][liveapply],[settingswindow],[settings],[manager]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`contour: the app saves Settings live through the injected file system`).

---

### Task 5: Settings remembers its frame and its page (4a D2)

**Files:**
- Modify: `src/contour/config/GuiStateStore.{hpp,cpp}` — `GuiState` (`.hpp:19-25`) gains the Settings window's state;
  the file reads and writes a `settings_window:` map.
- Modify: `src/contour/window/SettingsWindowHost.{hpp,cpp}` — takes a `config::GuiStateStore&` at construction;
  restores on the first show, saves on close.
- Modify: `src/contour/ContourGuiApp.cpp:2820-2828` (`settingsWindow()` passes its `FileGuiStateStore`).
- Modify: `src/contour/qml/SettingsPage.qml` — `currentPage` read from and written to `host.lastPage`.
- Test: `src/contour/config/GuiStateStore_test.cpp`, `src/contour/window/SettingsWindowHost_test.cpp`,
  `src/contour/window/SettingsWindowRules_test.cpp`.

**Interfaces:**
- Produces (`GuiStateStore.hpp`):
  `struct SettingsWindowState { int x = 0; int y = 0; int width = 0; int height = 0; std::string screen {}; WindowShowState show = WindowShowState::Normal; std::string page {}; [[nodiscard]] bool operator==(SettingsWindowState const&) const = default; };`,
  `enum class WindowShowState : uint8_t { Normal = 0, Maximized };`, and `GuiState::settingsWindow`
  (`std::optional<SettingsWindowState> settingsWindow {};`). YAML: `settings_window: { x, y, width, height, screen, show: normal|maximized, page }`.
- Produces (`SettingsWindowHost.hpp`): constructor gains `config::GuiStateStore& state` after `factory`;
  `Q_PROPERTY(QString lastPage READ lastPage WRITE setLastPage NOTIFY lastPageChanged)` (the page is domain state the
  window reports; documented as such).
- `settingsWindowGeometry()` is unchanged: the host feeds the remembered rectangle as `previous`; the remembered
  screen, if present, is the anchor.

- [ ] **Step 1: Write the failing tests.**
  - `GuiStateStore_test.cpp`, `[guistate]`: a state with `settingsWindow` round-trips; a file without the key loads
    with `settingsWindow == std::nullopt`; an unknown `show:` word loads as `Normal`.
  - `SettingsWindowHost_test.cpp`, `[settingswindow]`: "Settings reopens where it closed after a restart" — host A shows,
    moves to (300, 200, 900, 700), sets `lastPage` "section:appearance", closes; host B over the same in-memory state
    store shows at (300, 200, 900, 700) with `lastPage` "section:appearance". A remembered rectangle off every screen is
    pulled onto the anchor (the existing clamp, asserted again here). A remembered `Maximized` reopens maximised.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test contour_gui_test`. Expected: compile failure on `settingsWindow`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[guistate]" && out/appleclang-debug/src/contour/contour_gui_test "[settingswindow]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`settings: the window reopens where it was, on the page it showed`).
- [ ] **Step 6: Owner checks, Phase A** (ledger, not awaited): change the font size in Settings — every window's panes
  follow within a blink; edit the same profile file in an editor and then move a control — Settings shows the editor's
  version and says so; `chmod a-w ~/.config/contour/profiles` — the banner names the reason and the control springs
  back; Revert after a hand edit of contour.yml keeps the hand edit; quit with an edit pending — it is on disk; Settings
  reopens on the screen, frame and page it closed on.

---
## Phase B — Settings as tables

### Task 6: Sections and groups as one table; search

**Files:**
- Create: `src/contour/window/SettingsSections.hpp` (Qt-free), `src/contour/window/SettingsSearch.hpp` (Qt-free).
- Create: `src/contour/window/SettingsFields.{hpp,cpp}` — the descriptor structs, their builders and the two tables,
  moved verbatim from `SettingsController.cpp:56-1790` (`ProfileFieldDescriptor`, `ProfileFieldGroup`, the field
  builders, `profileFieldGroups()`, `GlobalFieldDescriptor`, the global builders, `globalFieldDescriptors()`), now in
  `contour::window::fields` and declared in the header. Each `ProfileFieldGroup` and each `GlobalFieldDescriptor`
  gains `SettingsGroup group`. **Depends on Steps 2 and 3a** (their rows move too).
- Modify: `src/contour/window/SettingsController.{hpp,cpp}` — the `sections` and `sidebar` properties, `group` on every
  field row, `searchSettings`.
- Test: `src/contour/window/SettingsSections_test.cpp`, `SettingsSearch_test.cpp` (create; `contour_test`),
  `src/contour/window/SettingsController_test.cpp`.

**Interfaces:**
- Produces (`contour::window`, `SettingsSections.hpp`):
  - `enum class SettingsSection : uint8_t { General = 0, Appearance, Keyboard, ShellIntegration, Profiles };`
    `inline constexpr size_t SettingsSectionCount = static_cast<size_t>(SettingsSection::Profiles) + 1;`
  - `enum class SettingsScope : uint8_t { Global = 0, Profile };`
  - `enum class SettingsGroup : uint8_t { GeneralWindows = 0, GeneralPalette, GeneralAdvanced, AppearanceChrome, AppearanceTabBar, KeyboardBindings, ShellGutter, ShellCommandBlocks, ShellFolding, ProfileColors, ProfileWindow, ProfileFont, ProfileScrolling, ProfileBehaviour, ProfileTerminal, ProfileKeyboard, ProfileHistory, ProfileCommandBlocks, ProfileMouse, ProfileVisualEffects, ProfileCursor, ProfileBell, ProfileBackground, ProfileLinks, ProfileStatusLine };`
    `inline constexpr size_t SettingsGroupCount = static_cast<size_t>(SettingsGroup::ProfileStatusLine) + 1;`
  - `enum class SettingsIcon : uint8_t { Gear = 0, Paintbrush, Keyboard, Terminal, Person, Document };`
  - `struct SettingsSectionRow { SettingsSection section = SettingsSection::General; std::string_view title {}; std::string_view id {}; SettingsIcon icon = SettingsIcon::Gear; };`
    `inline constexpr auto SettingsSectionRows = std::array { … };` — titles "General", "Appearance", "Keyboard",
    "Shell Integration", "Profiles"; ids `general`, `appearance`, `keyboard`, `shell-integration`, `profiles`.
  - `struct SettingsGroupRow { SettingsGroup group = SettingsGroup::GeneralWindows; SettingsSection section = SettingsSection::General; SettingsScope scope = SettingsScope::Global; std::string_view title {}; };`
    `inline constexpr auto SettingsGroupRows = std::array { … };` — R-h's membership and titles ("Windows", "Command
    palette", "Advanced", "Window chrome", "Tab bar", "Key bindings", "Gutter", "Command blocks", "Folding", "Colors",
    "Window", "Text" (canvas, for today's "Font"), "Scrolling", "Behaviour", "Terminal", "Keyboard", "History &
    scrollbar", "Command blocks", "Mouse & permissions", "Visual effects", "Cursor", "Bell", "Background", "Links",
    "Status line"), in page order. `static_assert`s: one row per enumerator, in enumerator order.
  - `[[nodiscard]] constexpr SettingsGroupRow const& settingsGroupRow(SettingsGroup group) noexcept;`
  - `[[nodiscard]] constexpr auto groupsOf(SettingsSection section)` — a view of the rows in that section, in order.
  - `inline constexpr std::string_view EditConfigEntryTitle = "Edit contour.yml…";`
- Produces (`SettingsSearch.hpp`): `struct SettingsSearchEntry { std::string key; std::string label; std::string help; SettingsGroup group = SettingsGroup::GeneralWindows; };`
  `[[nodiscard]] std::vector<std::size_t> matchSettings(std::string_view query, std::span<SettingsSearchEntry const> entries);`
  — case-insensitive substring over label, help and key; label matches first, then help, then key; stable within each;
  an empty or blank query matches nothing.
- Produces (controller): `Q_PROPERTY(QVariantList sections READ sections NOTIFY changed)` — one map per section
  `{ id, title, icon (int), groups: [{ id (int), title, scope ("global"|"profile") }] }`;
  `Q_PROPERTY(QVariantList sidebar READ sidebar NOTIFY changed)` — the section rows, then one row per profile
  `{ kind: "profile", name, origin, editable, isDefault, tabColor }`, then `{ kind: "edit", title: "Edit contour.yml…" }`;
  every `globalFields` and `profileFields` row gains `group` (int) and `groupTitle`;
  `[[nodiscard]] Q_INVOKABLE QVariantList searchSettings(QString const& query) const;` — `{ key, label, section, group, scope }`
  for the open profile's fields and the globals.

- [ ] **Step 1: Write the failing tests.**
  - `SettingsSections_test.cpp`, `[settingssections]`: every `SettingsGroup` enumerator appears in `SettingsGroupRows`
    exactly once (counted over `std::views::iota(size_t{0}, SettingsGroupCount)`); every section has at least one
    group; `groupsOf(Profiles)` are all `SettingsScope::Profile` and no other section's are; titles are unique within a
    section; `SettingsSectionRows` titles in order are exactly the five spec titles.
  - `SettingsSearch_test.cpp`, `[settingssearch]`: "scroll" finds "Scrollback lines" before a field whose help mentions
    scrolling; "GUTTER." matches `gutter.timestamps` by key; "" and "  " match nothing.
  - `SettingsController_test.cpp`, `[settings][sections]`: "every field group appears exactly once" — the `group` of
    every row of `globalFields()` and of `profileFields()` (with a profile open) is a known group of the right scope,
    and the set of groups the field rows name, plus `KeyboardBindings` and `ProfileColors` (pages without field rows),
    is exactly `SettingsGroupRows`; `option_as_alt` (Task 11 renames; here `option_key_as_alt`) is in `ProfileKeyboard`.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test contour_gui_test`. Expected: compile failure on `SettingsSection`.
- [ ] **Step 3: Implement.** The move first (no behaviour change), then the groups. The QML page keeps working: it groups
  by `groupTitle` where it grouped by `group` before.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[settingssections],[settingssearch]" && out/appleclang-debug/src/contour/contour_gui_test "[settings],[contour][gui][qml][settings]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`settings: sections and groups are one table, and every field names its group`).

---

### Task 7: Controls, option labels and the profile rules as data

**Files:**
- Create: `src/contour/window/SettingsControls.hpp`, `src/contour/window/ProfileRules.hpp` (Qt-free).
- Modify: `src/contour/window/SettingsFields.{hpp,cpp}` — descriptors gain `control`, `range`, `optionLabels`; enum
  builders take labels from the ConfigEnum tables; `globalEnumField` takes `std::span<std::pair<std::string_view, std::string_view> const>` (token, label).
- Modify: `src/contour/config/Config.hpp:134-140` — a ConfigEnum table for `GuiTheme` (tokens unchanged).
- Modify: `src/contour/window/SettingsController.cpp` — rows carry `control`, `min`, `max`, `step`, `decimals`, `unit`,
  `optionLabels`, `actions`; `profiles` rows carry the `ProfileActionRules` answer.
- Test: `src/contour/window/SettingsControls_test.cpp`, `ProfileRules_test.cpp` (create; `contour_test`),
  `src/contour/window/SettingsController_test.cpp`.

**Interfaces:**
- Produces (`SettingsControls.hpp`):
  - `enum class SettingsValueType : uint8_t { Bool = 0, Int, Double, String, Enum, FontFamily, Color };`
  - `enum class SettingsControl : uint8_t { Switch = 0, Segmented, PopUp, Stepper, Slider, TextField, FontPicker, ColorWell };`
  - `enum class NumberUnit : uint8_t { None = 0, Percent, Points, Milliseconds, Lines };`
  - `struct NumberRange { double min = 0.0; double max = 0.0; double step = 1.0; int decimals = 0; NumberUnit unit = NumberUnit::None; [[nodiscard]] bool operator==(NumberRange const&) const = default; };`
  - `inline constexpr std::size_t SegmentedMaxOptions = 4;` `inline constexpr std::size_t SegmentedMaxLabel = 12;`
  - `[[nodiscard]] constexpr SettingsControl defaultControlFor(SettingsValueType type, std::span<std::string_view const> optionLabels, std::optional<NumberRange> range) noexcept;`
    — R-i.
  - `[[nodiscard]] constexpr bool controlAccepts(SettingsControl control, SettingsValueType type) noexcept;` — the
    table `SettingsControlRules` (Switch: Bool; Segmented, PopUp: Enum; Stepper: Int, Double; Slider: Double; TextField:
    String, Int, Double; FontPicker: FontFamily; ColorWell: Color).
  - `[[nodiscard]] std::string formatNumber(double value, NumberRange const& range);` — Percent: `round(value × 100)`
    then " %" ("15 %"); Points " pt"; Milliseconds " ms"; Lines grouped thousands with a thin space ("10 000"); else the
    value with `decimals`.
- Produces (`ProfileRules.hpp`):
  - `enum class ProfileEditability : uint8_t { Editable = 0, DefinedInConfig, BuiltIn, Locked };`
  - `[[nodiscard]] constexpr ProfileEditability profileEditability(config::SettingsOrigin origin, config::SettingsEditability lock) noexcept;`
  - `enum class ProfileAction : uint8_t { Edit = 0, Duplicate, Rename, Delete, SetDefault };`
    `inline constexpr size_t ProfileActionCount = 5;`
  - `struct ProfileActionRule { ProfileEditability editability = ProfileEditability::Editable; std::array<ActionAvailability, ProfileActionCount> actions {}; std::string_view banner {}; };`
    with `enum class ActionAvailability : uint8_t { Offered = 0, Withheld };`;
    `inline constexpr auto ProfileActionRules = std::array { … };` — Editable: all offered, no banner; DefinedInConfig:
    Duplicate and SetDefault, banner "Defined in contour.yml"; BuiltIn: Duplicate and SetDefault, banner "Built in";
    Locked: none, banner "Settings are locked by gui_config_locked in contour.yml".
  - `[[nodiscard]] constexpr ProfileActionRule const& profileActionRule(ProfileEditability editability) noexcept;`
  - `[[nodiscard]] std::string uniqueCopyName(std::string_view base, std::function<bool(std::string_view)> const& taken);`
    — "base copy", then "base copy 2", "base copy 3", …; `uniqueNewName` does the same for "New Profile".
- Field rows gain `control` (int, `SettingsControl`), `min`, `max`, `step`, `decimals`, `unit` (int), and every enum row
  `optionLabels` (the table's labels; `theme` gives "Match terminal", "Match system", "Dark", "Light", Phase 0 R7).

- [ ] **Step 1: Write the failing tests.**
  - `SettingsControls_test.cpp`, `[settingscontrols]`: `defaultControlFor(Bool, {}, {}) == Switch`; an enum with
    labels {"Off","Left","Right","Both"} → Segmented; with five labels → PopUp; with a 13-character label → PopUp;
    `Double` over `{0, 1, 0.05}` → Slider, over `{6, 72, 0.5}` → Stepper; `Int` → Stepper; `String` → TextField.
    `controlAccepts` holds for each `defaultControlFor` answer over every `SettingsValueType`. `formatNumber(0.15,
    {0,1,0.01,0,Percent}) == "15 %"`, `formatNumber(10000, {…, Lines}) == "10 000"`.
  - `ProfileRules_test.cpp`, `[profilerules]` ("the read-only profile rules", §3.13): `SideFile` × `Editable` →
    `Editable`, all five offered; `MainConfig` → `DefinedInConfig`, only Duplicate and SetDefault, banner "Defined in
    contour.yml"; `Builtin` → `BuiltIn`; any origin × `Locked` → `Locked`, nothing offered; one rule per
    `ProfileEditability`, in order. `uniqueCopyName("main", taken={"main copy"}) == "main copy 2"`.
  - `SettingsController_test.cpp`, `[settings][controls]`: every field row's `control` is accepted by its type
    (`controlAccepts`); `theme`'s `optionLabels` start "Match terminal"; `dim_unfocused` is a Slider in percent;
    `history.limit` (scrollback) a Stepper in lines; `font_family` a FontPicker; the `profiles` row for a contour.yml
    profile carries `editable: false` and the DefinedInConfig banner.
- [ ] **Step 2: Run them and see them fail.** Build. Expected: compile failure on `SettingsControl`.
- [ ] **Step 3: Implement.** Ranges come from the existing validators' bounds (`globalIntField`'s min/max, the profile
  builders' checks); a descriptor with no bound gets none, and its number control is a Stepper with no limits.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[settingscontrols],[profilerules],[config]" && out/appleclang-debug/src/contour/contour_gui_test "[settings]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`settings: each row names its control, and the profile rules are a table`).

---

## Phase C — Layout and controls

### Task 8: Settings tokens per row

**Files:**
- Modify: `src/contour/config/ChromeTokenGroups.hpp` — `SettingsLayout` and its traits, `SettingsTokens`,
  `LayoutTokens::settings`, `ChromeTokenRow::settings`, new `ChromeColorRole`s and their sources in every row (`:358-375`,
  the four rows `:520-760`). `SizeTokens`' four `settings*` window extents (`:208-211`) stay where 4a put them.
- Modify: `src/contour/config/UiStyle.hpp` — the Terminal-row glyphs (beside `closeGlyph`, `:222-226`).
- Modify: `src/contour/window/ChromeTokenObjects.{hpp,cpp}`, `src/contour/window/UiStyleProvider.{hpp,cpp}` —
  `chromeStyle.settings.*`, `chromeStyle.layout.settings`, `chromeStyle.settingsTraits.*`, the glyphs, the colours.
- Test: `src/contour/config/ChromeTokenGroups_test.cpp` (`[chrometokens]`), `src/contour/window/UiStyleProvider_test.cpp`
  (`[contour][uistyle]`).

**Interfaces:**
- Produces (`contour::config`):
  - `enum class SettingsLayout : uint8_t { InsetGrouped = 0, Cards, BoxedList, Cells };`
  - `enum class SettingsHeader : uint8_t { Toolbar = 0, Breadcrumb, SplitHeaderBar };`
    `enum class SettingsRowShape : uint8_t { Grouped = 0, SeparateCards };`
    `enum class StepperArrangement : uint8_t { Stacked = 0, SideBySide };`
  - `struct SettingsLayoutTraits { SettingsHeader header = SettingsHeader::Toolbar; SettingsRowShape rows = SettingsRowShape::Grouped; StepperArrangement stepper = StepperArrangement::Stacked; };`
    `[[nodiscard]] constexpr SettingsLayoutTraits settingsLayoutTraits(SettingsLayout layout) noexcept;` (Global
    Constraints' traits).
  - `struct SettingsTokens` — `int` members `sidebarWidth, sidebarRow, iconTile, iconGlyph, searchField, headerBar,
    listMaxWidth, groupRadius, row, rowWithHelp, rowPadding, cardGap, groupGap, groupTitleGap, control, switchWidth,
    switchHeight, slider, stepperField, colorWell, previewHeight, previewWidth, previewRadius`, and `double` members
    `labelType, helpType, groupTitleType, pageTitleType`, each `= 0`, `operator==` defaulted. Values: the Global
    Constraints table, per row.
  - `LayoutTokens::settings = SettingsLayout::InsetGrouped;` (declared last); `ChromeTokenRow::settings {}` (declared
    after `tint`).
  - `ChromeColorRole` appends, after whatever is last: `SettingsSidebarFill, SettingsPageFill, SettingsGroupFill,
    SettingsSeparator, SettingsLabel, SettingsHelp, SettingsSelectionFill, SettingsSelectionText, SettingsModified,
    SettingsIconTile, SettingsIconGlyph, SwitchOnFill, SwitchOffFill, SwitchKnob, ControlFill, ControlText,
    PreviewFrame`. Native rows: sidebar/page/group from `Surface`/`PaletteWindow`, label `PaletteWindowText`, help
    `Label` level 2, separator `Fill` at the row's separator alpha, selection and switch-on the accent (`Status`
    Running), "Modified" the accent. Terminal row: scheme slots — surfaces `Background`/`Bar`, label `Foreground`,
    help `Secondary`, selection `Selection`/`SelectionText`, switch-on `Accent`, modified `Accent`.
- Produces (`UiStyle.hpp`, `ChromeStyleInfo`): `switchOnGlyph`, `switchOffGlyph`, `segmentOnGlyph`, `segmentOffGlyph`,
  `sliderFillGlyph`, `sliderKnobGlyph`, `sliderTrackGlyph`, `stepperGlyph`, `colorWellGlyph` — the Terminal row's
  `tui`-board glyphs, empty on the native rows (the existing "empty where it is a shape" convention).
- QML reads `chromeStyle.layout.settings` and `chromeStyle.settingsTraits.header/rows/stepper` as `int`s.

- [ ] **Step 1: Write the failing tests.**
  - `ChromeTokenGroups_test.cpp`, `[chrometokens]`: each row's `settings` equals the Global Constraints table, cell for
    cell, and its `layout.settings` the table's value; `settingsLayoutTraits` for each layout; every new text role
    (`SettingsLabel`, `SettingsHelp`, `SettingsSelectionText` on `SettingsSelectionFill`, `SettingsModified`,
    `ControlText` on `ControlFill`) reaches `MinimumTextContrast` against its surface in light and dark on the native
    rows and over contour-dark and contour-light on the Terminal row, and under `ContrastPreference::Increased`.
  - `UiStyleProvider_test.cpp`, `[contour][uistyle]`: `chromeStyle.settings.row` is 44 on MacOS and 1 on Terminal;
    `chromeStyle.layout.settings` is the int of `Cells` on Terminal; the Terminal row's `switchOnGlyph` is `[x]` and the
    MacOS row's is empty.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test contour_gui_test`. Expected: compile failure on `SettingsTokens`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[chrometokens]" && out/appleclang-debug/src/contour/contour_gui_test "[contour][uistyle]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`chrome: the Settings window's layout, extents, colours and glyphs per row`).

---

### Task 9: The kit controls, and the font family source

**Files:**
- Create: `src/contour/qml/KitSwitch.qml`, `KitSegmented.qml`, `KitStepper.qml`, `KitSlider.qml`, `KitPopUp.qml`,
  `KitColorWell.qml`, `KitFontPicker.qml` (list them in `_qml_files`, `src/contour/CMakeLists.txt:385-428`; baseline 0).
- Create: `src/contour/platform/FontFamilySource.{hpp,cpp}` (list in `add_library(contour_platform …)`).
- Modify: `src/contour/window/SettingsController.{hpp,cpp}` — `SettingsControllerServices::fontFamilies`; the
  `fontFamilies` property.
- Modify: `src/contour/test/QmlMocks.hpp` if a mock controller is needed for the kit cases.
- Test: `src/contour/test/QmlComponents_test.cpp` (`[contour][gui][qml][kit]`), `src/contour/test/Accessibility_qml_test.cpp`
  (`[contour][gui][qml][a11y]`), `src/contour/platform/FontFamilySource_test.cpp` (create; `contour_gui_test`).

**Interfaces:**
- Produces (`contour::platform`, `FontFamilySource.hpp`):
  `class IFontFamilySource { public: virtual ~IFontFamilySource() = default; [[nodiscard]] virtual std::vector<std::string> monospaceFamilies() const = 0; };`,
  `class NullFontFamilySource final` (returns `{ "JetBrains Mono" }`), `class QtFontFamilySource final` (bundled
  `"JetBrains Mono"` first, then `QFontDatabase::families()` filtered by `QFontDatabase::isFixedPitch`, sorted, deduplicated).
- Produces: `SettingsControllerServices::fontFamilies` (`std::shared_ptr<platform::IFontFamilySource const> fontFamilies {};`,
  null → `NullFontFamilySource`); `Q_PROPERTY(QStringList fontFamilies READ fontFamilies NOTIFY changed)` listed by `refresh()`.
- Each kit control (all read `chromeStyle`; none carries a literal):
  - `KitSwitch`: `property bool checked`, `property string name`, `property string description`, `property bool editable: true`,
    `signal toggled(bool checked)`; `Accessible.role: Accessible.CheckBox`, `Accessible.checkable: true`; Space and Return
    toggle; the knob moves over `chromeStyle.motion.fastMs`, none under `CrossFade`. Terminal row: draws
    `switchOnGlyph`/`switchOffGlyph`.
  - `KitSegmented`: `property var options`, `property var labels`, `property string value`, `name`, `description`,
    `editable`, `signal picked(string value)`; one `Accessible.RadioButton` per segment, the group `Accessible.Grouping`
    named `name`; ←/→ move and pick. Terminal row: `(•) Label` / `( ) Label`.
  - `KitStepper`: `property real value`, `from`, `to`, `stepSize`, `decimals`, `property string unitText`, `name`,
    `description`, `editable`, `signal committed(real value)`; `Accessible.role: Accessible.SpinBox`; ↑/↓ step; typing
    commits on Return or focus loss; arrangement from `chromeStyle.settingsTraits.stepper`; Terminal row `▴▾`.
  - `KitSlider`: as the stepper, `Accessible.Slider`, ←/→/↑/↓ step, shows the formatted value ("15 %") trailing; commits
    on release and on each key step. Terminal row: `━━━━●──────` over `settings.slider` cells.
  - `KitPopUp`: `options`, `labels`, `value`, `name`, `description`, `editable`, `signal picked(string value)`;
    `Accessible.ComboBox`.
  - `KitColorWell`: `property string hex`, `property string against` (the background the readout measures), `name`,
    `editable`, `signal committed(string hex)`; a well (`colorWellGlyph` on the Terminal row) plus a hex field that
    accepts `#rgb`/`#rrggbb` and refuses the rest, and the readout "{ratio} : 1" from
    `controller.contrastReadout(hex, against)` (Task 15 provides it; until then the property `readout` is set by the
    caller).
  - `KitFontPicker`: `property string family`, `property var families`, `name`, `editable`, `signal picked(string family)`;
    a button showing the family that opens a `PopupSurface` with a search field and the list; Escape closes it.
  - Every control shows `editable: false` as disabled (label at 35 %, kit "States") and ignores input then.

- [ ] **Step 1: Write the failing tests.**
  - `QmlComponents_test.cpp`, `[contour][gui][qml][kit]`, each on the MacOS row and under
    `CONTOUR_TEST_CONTROLS_STYLE=ContourTui`: KitSwitch's width/height equal `chromeStyle.settings.switchWidth/Height`
    (32 × 19 on MacOS); Space emits `toggled(true)`; non-editable ignores Space. KitSegmented with four options: →
    emits `picked("Left")` from `"Off"`; the Terminal row's label text of the picked segment starts with `(•)`.
    KitStepper ↑ from 12 with step 0.5 emits `committed(12.5)`; at `to` emits nothing. KitSlider shows "15 %" for 0.15.
    KitColorWell: typing `#12345` then Return emits nothing and puts the old hex back; `#123456` emits it. KitFontPicker
    filters "jet" to "JetBrains Mono" and Escape closes the popup.
  - Reduce Motion: with `MotionPreference::Reduced` the switch's knob animation duration is 0.
  - `Accessibility_qml_test.cpp`: "every kit control declares a role and a name" — each kit control's `Accessible.role`
    as above and `Accessible.name` equal to its `name`.
  - `FontFamilySource_test.cpp`, `[platform][fonts]`: `QtFontFamilySource` lists "JetBrains Mono" first once, every entry
    fixed-pitch.
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: FAIL (files missing).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][kit],[contour][gui][qml][a11y],[platform][fonts]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][kit]" && ctest --test-dir out/appleclang-debug -R "check_qml" --output-on-failure`
  Expected: PASS; the lint finds no literal in the new files.
- [ ] **Step 5: Tidy, format, `editorconfig-checker` the QML, commit** (`qml: kit switch, segmented, stepper, slider, pop-up, colour well and font picker`).

---

### Task 10: The Settings window — sidebar, groups, rows, banners, keyboard and VoiceOver

**Files:**
- Rewrite: `src/contour/qml/SettingsPage.qml` — `property var host` (was `windowController`, 4a), `property var
  controller: host ? host.controller : null`, `property string currentPage` (`section:<id>`, `profile:<name>`,
  `profile:<name>:schemes`, `profile:<name>:links`, `profile:<name>:statusline`), the layout per
  `chromeStyle.settingsTraits.header`, the page stack, "Revert Changes", the error banner (kit `Banner`), the read-only
  banner with "Duplicate to Edit".
- Modify: `src/contour/qml/SettingsWindow.qml` — passes `host`; the page `anchors.fill: parent` (R-o).
- Create: `src/contour/qml/SettingsSidebar.qml` (search, section rows with icon tiles, profile rows, "+ − …",
  "Edit contour.yml…"), `SettingsGroup.qml` (title + inset list per `settingsTraits.rows`), `SettingsRow.qml` (label,
  help, trailing kit control chosen by `field.control`, "Modified · Reset"), `SettingsSectionPage.qml` (a section's
  global groups; Keyboard shows the bindings list), `ProfilePage.qml` (the profile's groups; links to Colors, Links,
  Status Line sub-pages).
- Delete: `src/contour/qml/SettingRow.qml`, `SettingsSection.qml`, `SettingsNavItem.qml`, `SettingsListItem.qml`
  (their baseline lines go; `_qml_files` updated).
- Modify: `src/contour/window/SettingsWindowHost.{hpp,cpp}` — `Q_INVOKABLE void editConfigurationFile();` running the
  injected `std::function<void()> editConfiguration` (the app passes one that opens `_config.configFile` in the OS
  editor, as `OpenConfiguration{Editor}` does in `TerminalSession.cpp:4759`).
- Test: `src/contour/test/SettingsPageQml_test.cpp` (`[contour][gui][qml][settings]`; the existing cases move to the
  new objectNames), `src/contour/test/Accessibility_qml_test.cpp` (the "every settings editor carries the row's label"
  case, `:113`, becomes the row case below).

**Interfaces:**
- Consumes: Tasks 3, 6–9.
- objectNames the tests use: `settingsSidebar`, `settingsSearchField`, `sidebarRow:<id>` (sections), `profileRow:<name>`,
  `addProfileButton`, `removeProfileButton`, `profileMenuButton`, `editConfigRow`, `settingsPageTitle`,
  `revertChangesButton`, `settingsErrorBanner`, `readOnlyBanner`, `duplicateToEditButton`, `settingsGroup:<group id>`,
  `settingsRow:<key>`, `settingsControl:<key>`, `modifiedResetButton:<key>`, `lockedBanner`.
- The search field: typing shows `controller.searchSettings(text)` as a result list in the sidebar; Return or a click on
  a result opens its page and scrolls its row into view; Escape clears. ⌘F / Ctrl+F focus it.

- [ ] **Step 1: Write the failing tests** (`SettingsPageQml_test.cpp`, over the fixture; each also under ContourTui):
  - "Settings lists the five sections, the profiles, then Edit contour.yml…" — the sidebar's rows in order.
  - "a control change reaches the controller and Revert enables" — toggle `settingsControl:reflow_on_resize`:
    `revertChangesButton.enabled` becomes true; `fire()`; click Revert: the switch shows the old value, the button
    is disabled.
  - "a contour.yml profile shows its banner and disabled controls" (Review Focus 3) — open `profileRow:main` (MainConfig):
    `readOnlyBanner` visible with "Defined in contour.yml"; every `settingsControl:*` on the page is `enabled: false`;
    `duplicateToEditButton` opens `profileRow:main copy`.
  - "a locked configuration disables every control and hides Revert" — `gui_config_locked: true`.
  - "an overridden global shows Modified · Reset" — a `settings.yml` override: `modifiedResetButton:theme` visible;
    click it: the override goes after `fire()`.
  - "search finds a row on another page" — type "scrollback": a result; Return opens the profile page with
    `settingsRow:history…` in view.
  - "the keyboard alone reaches every section and every control" (Review Focus 5) — focus the sidebar; ↓ four times
    selects `shell-integration`; Tab moves focus into the page; repeated Tab visits every `settingsControl:*` of the page
    exactly once, in the order of `controller.globalFields` for that section; Space on a focused switch toggles it.
  - "the Terminal row's page fills the window" (R-o) — ContourTui: the page's height equals the window's content height
    at sizes that are not whole cells (e.g. 1872×1430).
  - `Accessibility_qml_test.cpp`: "every settings row's control carries its label and help" (Review Focus 5) — for every
    `settingsRow:*` on General and on a profile page: its control's `Accessible.name` is the row's label and
    `Accessible.description` its help; sidebar rows are `Accessible.ListItem` named by their titles inside an
    `Accessible.List` named "Settings sections".
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: FAIL (objectNames absent).
- [ ] **Step 3: Implement.** The page header per traits: Toolbar — the title in the page's top bar (`settings.headerBar`
  high); Breadcrumb — "Profiles › main" in `type.settingsPageTitle`; SplitHeaderBar — the sidebar and the page each own
  half of a `settings.headerBar` row, search in the sidebar half. Page change and selection follow `chromeStyle.motion`.
  Rows are `settings.row` high, `settings.rowWithHelp` with help, list width `min(settings.listMaxWidth, page)` (0 =
  page).
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings],[contour][gui][qml][a11y],[settingswindow],[settings]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings],[contour][gui][qml][a11y]" && ctest --test-dir out/appleclang-debug -R "check_qml" --output-on-failure`
  Expected: PASS.
- [ ] **Step 5: Regenerate the size baseline** for the deleted files (`python3 scripts/check-qml-sizes.py --print-baseline`,
  keeping the header); **tidy, format, `editorconfig-checker`, commit** (`settings: the window is a sidebar of sections over grouped rows of native controls`).
- [ ] **Step 6: Owner checks, Phases B and C** (ledger, not awaited): the five sections and "Edit contour.yml…" in that
  order on macOS; rows 44 tall (52 with help), groups radius 10, list at most 640 wide; the small switch; Option as
  Meta segmented; "Modified · Reset"; search "dim" finds Dim inactive panes; a contour.yml profile with Duplicate to
  Edit; VoiceOver (⌘F5) reads sidebar rows and each control's label and help; keyboard alone through the sidebar and a
  page; `ui_style: terminal` draws `[x]`, `(•)`, `━━●──`, `▴▾` and no band below the page.

---
## Phase D — `option_as_alt` per side

### Task 11: `option_as_alt` becomes `off | left | right | both`

**Files:**
- Create: `src/contour/config/OptionAsAlt.hpp` (Qt-free; the enum, its table, the per-side decision; list it in
  `contour_config`'s sources).
- Modify: `src/contour/config/Config.hpp:1277` — `ConfigEntry<OptionAsAlt, documentation::OptionKeyAsAlt> optionAsAlt { OptionAsAlt::Off };`
  (renamed from `optionKeyAsAlt`); the reader's overload beside the ConfigEnum one (`:2097-2101`).
- Modify: `src/contour/config/Config.cpp:1758` — `loadFromEntry(child, "option_as_alt", where.optionAsAlt);`.
- Modify: `src/contour/config/ConfigDocumentation.hpp:3208-3226` — the new text.
- Modify: `src/contour/window/SettingsFields.cpp` — the row: key `option_as_alt`, label "Use Option as Meta", help
  "The other Option key keeps typing accents like é and ü.", `enumFieldFromTable<config::OptionAsAlt>`, control
  Segmented, group `ProfileKeyboard` (R-p).
- Modify: `src/contour/session/SessionInput.cpp:634` (compile fix only here: `!= OptionAsAlt::Off`; Task 12 makes it
  per side).
- Test: `src/contour/config/Config_test.cpp` (`[config][optionasalt]`), `src/contour/config/OptionAsAlt_test.cpp`
  (create; `contour_test`), `src/contour/window/SettingsController_test.cpp`.

**Interfaces:**
- Produces (`contour::config`, `OptionAsAlt.hpp`):
  - `enum class OptionAsAlt : uint8_t { Off = 0, Left, Right, Both };`
  - `detail::OptionAsAltTable` with rows `{Off, "off", "Off"}`, `{Left, "left", "Left"}`, `{Right, "right", "Right"}`,
    `{Both, "both", "Both"}`; the `configEnumValues<OptionAsAlt>()` specialization.
  - `[[nodiscard]] constexpr std::optional<OptionAsAlt> parseOptionAsAlt(std::string_view text) noexcept;` — a token
    first (case-insensitive), then the legacy booleans: `true`, `yes`, `on`, `y` → `Both`; `false`, `no`, `n` → `Off`;
    anything else nullopt.
  - `enum class OptionKeySide : uint8_t { None = 0, Left, Right, Both };`
  - `enum class OptionKeyRole : uint8_t { Composes = 0, Alt };`
  - `[[nodiscard]] constexpr OptionKeyRole optionKeyRole(OptionAsAlt setting, OptionKeySide down) noexcept;` — `Alt`
    when the held side is one the setting names (`Both` either side; `Left` with `Left` or `Both` held; `Right` with
    `Right` or `Both`), else `Composes`; `None` held is always `Composes`.
- Produces (YAMLConfigReader): `void loadFromEntry(YAML::Node const& node, std::string const& entry, OptionAsAlt& where)`
  calling `detail::loadConfigEnum(node, entry, keyPathTo(node, entry), where, logger, parseOptionAsAlt, "off, left, right, both")`.
- Documentation text: "{comment} Which Option keys act as Alt (Meta) on macOS: off, left, right or both. The other
  Option key keeps typing accents like é and ü. true and false are still read, as both and off.\n{comment} This value is
  ignored on other platforms.\n{comment}\n{comment} Default: off\noption_as_alt: {}\n"; the web text likewise with
  `option_as_alt: left`.

- [ ] **Step 1: Write the failing tests.**
  - `OptionAsAlt_test.cpp`, `[optionasalt]`: `parseOptionAsAlt` over `off`, `Left`, `RIGHT`, `both`, `true`, `yes`,
    `on`, `false`, `no`, `maybe` → `Off, Left, Right, Both, Both, Both, Both, Off, Off, nullopt`; `optionKeyRole` as a
    full 4 × 4 table (16 rows, counted from the enums).
  - `Config_test.cpp`, `[config][optionasalt]` ("reading the old `option_as_alt` boolean", §3.13): a profile with
    `option_as_alt: true` loads as `Both`, `false` as `Off`, `left` as `Left`; `option_as_alt: sideways` logs an error and
    keeps `Off`; `emitProfileYaml` of a `Right` profile contains `option_as_alt: right` and reloads as `Right`; a profile
    without the key is `Off` under both `DefaultsEdition::Classic` and `Native2026` (R-p); the generated config
    (`defaultConfigString()`) documents `option_as_alt: off`.
  - `SettingsController_test.cpp`, `[settings]`: the `option_as_alt` row offers `off, left, right, both` labelled
    "Off, Left, Right, Both", control Segmented; committing `left` and flushing writes `option_as_alt: left`.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test contour_gui_test`. Expected: compile failure on `OptionAsAlt`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[optionasalt],[config]" && out/appleclang-debug/src/contour/contour_gui_test "[config][optionasalt],[config],[settings]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`config: option_as_alt names the Option keys that act as Alt: off, left, right or both`).

---

### Task 12: The key boundary decides per side

**Files:**
- Modify: `src/contour/input/KeyMapping.{hpp,cpp}` — `optionKeySideOf`.
- Modify: `src/contour/input/CMakeLists.txt` — `contour_input` links `contour_config` (header use only; the
  pre-flight confirms no cycle: `contour_config` links no `contour_input`).
- Modify: `src/contour/session/SessionInput.cpp:632-645` — the macOS branch asks `optionKeyRole`.
- Test: `src/contour/session/SessionInput_test.cpp` (`[session][input]`).

**Interfaces:**
- Consumes: Task 11's `OptionKeySide`, `OptionKeyRole`, `optionKeyRole`.
- Produces (`contour::input`, `KeyMapping.hpp`):
  `inline constexpr quint32 MacLeftOptionBit = 0x00000020;` (`NX_DEVICELALTKEYMASK`),
  `inline constexpr quint32 MacRightOptionBit = 0x00000040;` (`NX_DEVICERALTKEYMASK`),
  `[[nodiscard]] constexpr config::OptionKeySide optionKeySideOf(quint32 nativeModifiers) noexcept;` — a pure function
  of the bits on every host (tested everywhere), consulted only under `#ifdef __APPLE__`.
- `SessionInput.cpp`: `optionKeyRole(session.profile().optionAsAlt.value(), input::optionKeySideOf(event->nativeModifiers())) == OptionKeyRole::Alt`
  replaces `session.profile().optionKeyAsAlt.value()`.

- [ ] **Step 1: Write the failing tests** (`SessionInput_test.cpp`, `[session][input]`):
  - "optionKeySideOf reads macOS's device-dependent Option bits" — `0x20` → `Left`, `0x40` → `Right`, `0x60` → `Both`,
    `0x80000` alone (the device-independent Option flag) → `None`.
  - "Option as Alt applies to the configured side only" (`#ifdef __APPLE__`, else `SKIP`) — profile `Left`: a
    `QKeyEvent(KeyPress, Qt::Key_E, Qt::AltModifier, "é", false, 1)` built with native modifiers `0x80020` sends `ESC e`
    to the PTY; with `0x80040` it sends `é`. Profile `Both`: both send `ESC e`. Profile `Off`: both send `é`. (Construct
    the event with `QKeyEvent(type, key, modifiers, nativeScanCode, nativeVirtualKey, nativeModifiers, text)`.)
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: compile failure on `optionKeySideOf`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[session][input]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`input: Option acts as Alt only on the side option_as_alt names`).
- [ ] **Step 6: Owner check, Phase D** (ledger, not awaited): with `option_as_alt: left`, left-⌥ + b in zsh moves back a
  word and right-⌥ + e types é (R-q confirms the bits on this Mac); an old `option_as_alt: true` acts on both; the
  Settings segmented row writes the token.

---

## Phase E — Live preview

### Task 13: A preview session over a canned transcript

**Files:**
- Create: `src/contour/session/PreviewTranscript.hpp` — the transcript and the caption.
- Modify: `src/contour/session/TerminalSession.hpp:146-156` — `SessionRole::Preview` (appended): no shell, no
  notifications, not saved by a layout, never closes its pane (it has none), takes no focus.
- Modify: `src/contour/session/TerminalSessionManager.{hpp,cpp}` — `createPreviewSession`.
- Create: `src/contour/window/SettingsPreview.{hpp,cpp}` — the QObject the Settings window binds to.
- Modify: `src/contour/window/SettingsController.{hpp,cpp}` — `previewConfig()`, `previewProfileName()`.
- Modify: `src/contour/window/SettingsWindowHost.{hpp,cpp}`, `src/contour/ContourGuiApp.cpp` — the host owns a
  `SettingsPreview` built from an injected `SettingsPreview::SessionFactory`.
- Test: `src/contour/window/SettingsPreview_test.cpp` (create; `contour_gui_test`), `src/contour/session/TerminalSessionManager_test.cpp`.

**Interfaces:**
- Produces (`contour::session`, `PreviewTranscript.hpp`):
  `inline constexpr std::string_view PreviewTranscript = "\x1b[32m~/repo/contour\x1b[0m \x1b[35m❯\x1b[0m git status --short\r\n\x1b[31m M\x1b[0m src/contour/config/Config.cpp\r\n\x1b[32m~/repo/contour\x1b[0m \x1b[35m❯\x1b[0m ";`
  (the `mac-settings` board's lines, in the scheme's ANSI green, magenta and red);
  `inline constexpr vtbackend::PageSize PreviewPageSize { vtbackend::LineCount(6), vtbackend::ColumnCount(80) };`;
  `[[nodiscard]] std::string previewCaption(std::string_view family, double pointSize, std::string_view scheme);` →
  "Live preview — JetBrains Mono 13 · Contour Dark" (a whole size without decimals).
- Produces (`TerminalSessionManager`): `[[nodiscard]] std::unique_ptr<TerminalSession> createPreviewSession(config::Config const& config, std::string const& profileName);`
  — a `vtpty::MockPty(PreviewPageSize)` with `appendStdOutBuffer(PreviewTranscript)`, `SessionRole::Preview`, not in
  `_sessionsById` or any window; the session's terminal applies `config`'s `profileName`.
- Produces (`contour::window`, `SettingsPreview.hpp`):
  - `class SettingsPreview final : public QObject` — `using SessionFactory = std::function<std::unique_ptr<session::TerminalSession>(config::Config const&, std::string const&)>;`
    `SettingsPreview(SettingsController& controller, SessionFactory factory, QObject* parent = nullptr);`
    `Q_PROPERTY(contour::session::TerminalSession* session READ session NOTIFY sessionChanged)` (created on first
    `show(profileName)`), `Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)`,
    `Q_INVOKABLE void show(QString const& profileName);`, `Q_INVOKABLE void showStatusLine(QString const& profileName);`
    (the same, with the indicator status line forced on, R-l). It re-applies on `SettingsController::draftChanged` and
    `changed`: `session->applyReloadedConfig(controller.previewConfig(), name)`.
- Produces (controller): `[[nodiscard]] config::Config previewConfig() const;` — `_config()` with the open draft put in
  place of its profile (and the open scheme draft in place of its palette).
- `SettingsWindowHost`: `Q_PROPERTY(contour::window::SettingsPreview* preview READ preview CONSTANT)`; its constructor
  takes `SettingsPreview::SessionFactory` after the state store; null factory → `preview` null (snapshot scene, R-r).

- [ ] **Step 1: Write the failing tests.**
  - `SettingsPreview_test.cpp`, `[settingspreview]` (a `TestApp`, the fixture's controller): `show("main")`: the
    session's screen text of rows 0–2 contains "~/repo/contour ❯ git status --short" and " M src/contour/config/Config.cpp";
    the session is in no window and not listed by the manager; `commitProfileField("font_size", 18.0)` changes the
    preview session's profile font size to 18 before any flush (the manual scheduler not fired) — "every change shows
    here first"; the caption reads "Live preview — {family} 18 · {scheme}"; `showStatusLine("main")` gives a profile
    whose status line display is the indicator.
  - `TerminalSessionManager_test.cpp`, `[manager]`: a preview session does not count toward any window's tabs and is
    untouched by `reloadAllSessions()`'s session loop (it re-applies through `SettingsPreview` only).
  - `previewCaption("JetBrains Mono", 13.0, "Contour Dark") == "Live preview — JetBrains Mono 13 · Contour Dark"`;
    `12.5` gives "12.5".
- [ ] **Step 2: Run them and see them fail.** Build `contour_gui_test`. Expected: compile failure on `SettingsPreview`.
- [ ] **Step 3: Implement.** The preview's writes to its PTY go nowhere (`MockPty` keeps them); it is closed and
  destroyed with the host.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[settingspreview],[manager],[settingswindow]"`. Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (`settings: a preview terminal shows the profile's draft before it is written`).

---

### Task 14: The profile page's preview hero

**Files:**
- Create: `src/contour/qml/SettingsPreview.qml` — a `TerminalDisplay { session: host.preview.session }` in a frame of
  `settings.previewHeight` × (`settings.previewWidth` or the list width), `radius.settingsPreview`, the
  `PreviewFrame` colour, the caption below in `SettingsHelp`; `enabled: false`, `activeFocusOnTab: false`,
  `Accessible.role: Accessible.Graphic`, `Accessible.name: host.preview.caption`.
- Modify: `src/contour/qml/ProfilePage.qml` — the hero first, then the groups; on Fluent the hero's trailing buttons
  "Duplicate" and "Open in new tab" (the latter runs `NewTerminal{profile}` through the host).
- Test: `src/contour/test/SettingsPageQml_test.cpp`.

**Interfaces:**
- Consumes: Task 13's `SettingsWindowHost::preview`, `SettingsPreview::show`.
- objectNames: `settingsPreview`, `settingsPreviewCaption`.

- [ ] **Step 1: Write the failing tests** (also under ContourTui): opening `profileRow:<side-file profile>` calls
  `preview.show(name)` (a recording preview double through the host), and `settingsPreview` is visible, `settings.previewHeight`
  high on MacOS (104) and 6 cells on the Terminal row; the caption shows the preview's caption; Tab never stops on the
  preview; with a null `host.preview` (the snapshot scene) the frame shows and no warning is printed.
- [ ] **Step 2: Run them and see them fail.** Expected: FAIL (`settingsPreview` absent).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings]" && ctest --test-dir out/appleclang-debug -R "check_qml" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: `editorconfig-checker`, commit** (`settings: every profile page opens on a live preview`).
- [ ] **Step 6: Owner check, Phase E** (ledger, not awaited): drag Dim inactive panes, change the font and the scheme —
  the preview follows at once and the terminals within ~300 ms; the caption names font and scheme; VoiceOver reads the
  caption and skips into the controls.

---
## Phase F — The editors as pages

### Task 15: Colour schemes: the gallery, colour wells, the contrast readout

**Files:**
- Create: `src/contour/window/ColorContrastReadout.hpp` (Qt-free).
- Create: `src/contour/qml/ColorSchemesPage.qml` — Profiles › <name> › Colors: the mode (one scheme, or light and
  dark), the gallery (one card per scheme: background, foreground and the eight normal colours as a strip, the name,
  a check on the chosen one; "+ New scheme" duplicates the selected one), then, for an editable scheme, the editor: 16
  ANSI slots, foreground and background, cursor, selection and the block-status slots, each a `KitColorWell` with a hex
  field and its readout against the scheme's background (foreground and the ANSI colours) or against what it sits on
  (selection text on selection).
- Delete: `src/contour/qml/ColorSchemeEditor.qml`, `ColorSlotField.qml` (their tests move; baselines go).
- Modify: `src/contour/window/SettingsController.{hpp,cpp}` — `contrastReadout`, `schemeGallery`.
- Test: `src/contour/window/ColorContrastReadout_test.cpp` (create; `contour_test`), `SettingsController_test.cpp`,
  `src/contour/test/SettingsPageQml_test.cpp` (the `ColorSchemeEditor` case `:1394` moves here).

**Interfaces:**
- Produces (`ColorContrastReadout.hpp`): `enum class ContrastVerdict : uint8_t { Passes = 0, Fails };`
  `struct ContrastReadout { double ratio = 1.0; ContrastVerdict verdict = ContrastVerdict::Passes; };`
  `[[nodiscard]] ContrastReadout contrastReadoutOf(vtbackend::RGBColor foreground, vtbackend::RGBColor background, double required = 4.5);`
  (over `vtbackend::contrastRatio`, `Color.hpp:116`); `[[nodiscard]] std::string formatContrast(ContrastReadout readout);`
  → "4.8 : 1" (one decimal).
- Produces (controller): `[[nodiscard]] Q_INVOKABLE QVariantMap contrastReadout(QString const& foreground, QString const& background) const;`
  → `{ text: "4.8 : 1", passes: bool }` (QML-facing bool, the carve-out); `Q_PROPERTY(QVariantList schemeGallery READ schemeGallery NOTIFY changed)`
  → `{ name, editable, background, foreground, normal: [8 × "#rrggbb"] }`. `schemeColors` rows gain `against`
  (the hex the readout measures against).

- [ ] **Step 1: Write the failing tests.**
  - `ColorContrastReadout_test.cpp`, `[contrastreadout]`: black on white → ratio 21.0, "21.0 : 1", Passes; `#767676` on white
    (4.54) Passes and `#777777` on white (4.48) Fails, each ratio within 0.01 of `vtbackend::contrastRatio`.
  - `SettingsController_test.cpp`, `[settings][schemes]`: the gallery lists every scheme with eight normal colours; the
    background slot's `against` is empty and every ANSI slot's is the background; `contrastReadout("#000000","#ffffff")`
    is `{ "21.0 : 1", true }`.
  - `SettingsPageQml_test.cpp`: "the colour schemes page edits a slot live" — open Colors, pick an editable scheme's
    card, type `#102030` into `settingsControl:scheme.color1`'s hex field, Return: after `fire()` the scheme file has it;
    a read-only scheme shows its wells disabled and "Duplicate to Edit"; the readout text of `color1` follows the hex.
    Under ContourTui the wells draw `██`.
- [ ] **Step 2: Run them and see them fail.** Build. Expected: compile failure on `contrastReadoutOf`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[contrastreadout]" && out/appleclang-debug/src/contour/contour_gui_test "[settings][schemes],[contour][gui][qml][settings]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings]"`. Expected: PASS.
- [ ] **Step 5: Baseline, tidy, format, `editorconfig-checker`, commit** (`settings: colour schemes are a gallery and colour wells with a contrast readout`).

---

### Task 16: Link patterns as a page

**Files:**
- Create: `src/contour/qml/LinkPatternsPage.qml` — Profiles › <name> › Links: the link gesture line
  (`controller.linkGesture`), a table of `controller.linkPatterns` (name, kind, a `KitSwitch` "Clickable" per row, the
  problem under a row), "+" (`addLinkPattern`), "−" (`removeLinkPattern`, user rows only), a detail group for the
  selected user row (name, regex, URL fields), and the sample field with its live preview (`previewLinkPattern`).
- Delete: `src/contour/qml/LinkPatternList.qml` (its three QML cases, `SettingsPageQml_test.cpp:656-806`, move).
- Modify: `src/contour/window/SettingsController.cpp` — `setLinkPattern`/`addLinkPattern`/`removeLinkPattern` stage
  through the session (Task 3's pattern; they edit the profile draft).
- Test: `src/contour/test/SettingsPageQml_test.cpp` (`[contour][gui][qml][settings][links]`), `SettingsController_test.cpp` (`[settings][links]`).

**Interfaces:**
- objectNames: `linkPatternsTable`, `linkPatternRow:<index>`, `linkPatternClickable:<index>`, `addLinkPatternButton`,
  `removeLinkPatternButton`, `linkPatternName`, `linkPatternRegex`, `linkPatternUrl`, `linkSampleField`, `linkSamplePreview`.

- [ ] **Step 1: Write the failing tests** (moved cases plus): "the Clickable switch of a built-in applies live" — switch
  `linkPatternClickable:0` off, `fire()`: the profile file holds the name-only override; "a read-only profile's table is
  inert" (moved); "typing a sample shows the spans" (moved, new objectNames); a regex with a problem shows it under its
  row and the save is refused with the reason (`draftProblem`, as today). ContourTui as well.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[settings][links],[contour][gui][qml][settings][links]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings][links]"`. Expected: PASS.
- [ ] **Step 5: Baseline, `editorconfig-checker`, commit** (`settings: link patterns are a page with a Clickable switch per row`).

---

### Task 17: The status line as a page

**Files:**
- Create: `src/contour/qml/StatusLinePage.qml` — Profiles › <name> › Status Line (and Appearance › Status Line for the
  default profile, R-l): the preview hero (`host.preview.showStatusLine(name)`), the profile's status-line fields
  (group `ProfileStatusLine`), then the three segment editors (`StatusLineIndicatorEditor`) in grouped lists, the Add
  menu offering `indicatorPlaceholders()` (Step 3a's included).
- Modify: `src/contour/qml/StatusLineIndicatorEditor.qml`, `StatusLineSegmentRow.qml`, `StatusLineChip.qml`,
  `StatusLineItemDialog.qml` — kit controls and tokens in place of their own (their size baselines only go down);
  `setIndicatorSegment` stages live.
- Modify: `src/contour/window/SettingsSections.hpp` — Appearance gains the page entry `SettingsPageLink { section = Appearance, title = "Status Line", target = DefaultProfileStatusLine }`
  (a row of `SettingsPageLinks`, so the sidebar search finds it).
- Test: `src/contour/test/SettingsPageQml_test.cpp` (the indicator cases `:533-655` move), `SettingsController_test.cpp`
  (`[settings][statusline]`). **Depends on Step 3a** (its placeholders and `indicatorColorRoles`).

**Interfaces:**
- Produces: `enum class SettingsPageTarget : uint8_t { DefaultProfileStatusLine = 0 };`,
  `struct SettingsPageLink { SettingsSection section = SettingsSection::Appearance; std::string_view title {}; SettingsPageTarget target = SettingsPageTarget::DefaultProfileStatusLine; };`,
  `inline constexpr auto SettingsPageLinks = std::array { … };` in `SettingsSections.hpp`; the controller's `sections`
  rows gain `links: [{ title, target }]`.
- objectNames: `statusLinePage`, `statusLineSegment:<0|1|2>`, `statusLineAddButton:<segment>`.

- [ ] **Step 1: Write the failing tests** (moved cases plus): Appearance › Status Line opens `statusLinePage` for the
  default profile; adding `{Git}` (Step 3a) to the right segment and flushing writes it to the profile file and the
  preview's status line shows the item's sample; a read-only profile's editors are inert (moved). ContourTui as well.
- [ ] **Step 2: Run them and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[settings][statusline],[contour][gui][qml][settings],[contour][gui][qml][statusline]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][settings],[contour][gui][qml][statusline]" && ctest --test-dir out/appleclang-debug -R "check_qml" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Baseline, `editorconfig-checker`, commit** (`settings: the status line is a page with a live preview`).
- [ ] **Step 6: Owner check, Phase F** (ledger, not awaited): the scheme gallery and wells with a readout that turns at
  4.5 : 1; a link pattern's Clickable switch; the status-line page's preview shows each added placeholder.

---

## Phase G — Snapshots and finish

### Task 18: A snapshot per section and page, the Terminal row included

**Files:**
- Modify: `src/contour/test/ChromeSnapshotPlan.hpp` — a `SnapshotSettingsPage` column and the rows.
- Modify: `src/contour/test/ChromeSnapshotScene.hpp` — opens the page the row names (through `host.lastPage`), with a
  contour.yml profile `main`, a side-file profile `work`, one side-file scheme, and no preview (R-r).
- Test: `src/contour/test/ChromeSnapshotPlan_test.cpp` (`[snapshot]`), `ChromeSnapshot_test.cpp`.

**Interfaces:**
- Produces: `enum class SnapshotSettingsPage : uint8_t { None = 0, General, Appearance, Keyboard, ShellIntegration, Profile, ProfileReadOnly, ColorSchemes, LinkPatterns, StatusLine };`;
  `SnapshotScriptRow::settingsPage = SnapshotSettingsPage::None;` (declared last; every row names it). The existing
  `settings` row becomes `General`. New `SnapshotScript`s after whatever is last: `SettingsAppearance`,
  `SettingsKeyboard`, `SettingsShellIntegration`, `SettingsProfile`, `SettingsProfileReadOnly`, `SettingsColorSchemes`,
  `SettingsLinkPatterns`, `SettingsStatusLine`, named `settings-appearance`, `settings-keyboard`,
  `settings-shell-integration`, `settings-profile`, `settings-profile-readonly`, `settings-color-schemes`,
  `settings-link-patterns`, `settings-status-line`, all `SnapshotSurface::SettingsWindow`.

- [ ] **Step 1: Write the failing tests.** The plan test's "one row per script, in enumerator order" and "names are
  unique" pass with the rows; a new case checks that every `SnapshotSettingsPage` but `None` is shown by exactly one
  Settings-window row and that every `SettingsWindow` row names a page.
- [ ] **Step 2: Run them and see them fail.** Build `contour_test contour_gui_test`. Expected: compile failure on `SnapshotSettingsPage`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[snapshot]" && out/appleclang-debug/src/contour/contour_gui_test "[snapshot]" && CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]" && CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"`
  Expected: PASS, no QML warnings.
- [ ] **Step 5: Tidy, format, commit** (`test: snapshot every Settings section and page on every row`).

---

### Task 19: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff (`git diff <the plan commit>..HEAD -- src scripts CMakeLists.txt`) and
  apply its findings; re-run the filters of every task it touches.
- [ ] **Step 2:** Every task's filter once more:

```bash
out/appleclang-debug/src/contour/contour_test "[guiconfigstore],[settingssession],[guistate],[settingssections],[settingssearch],[settingscontrols],[profilerules],[chrometokens],[optionasalt],[contrastreadout],[config],[snapshot]"
out/appleclang-debug/src/contour/contour_gui_test "[settings],[newdefaults],[settingswindow],[contour][settings][liveapply],[manager],[contour][uistyle],[platform][fonts],[contour][gui][qml][kit],[contour][gui][qml][settings],[contour][gui][qml][a11y],[contour][gui][qml][statusline],[config],[session][input],[settingspreview],[snapshot]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][kit],[contour][gui][qml][settings],[contour][gui][qml][a11y],[contour][gui][qml][statusline],[snapshot]"
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

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, read from
  `ChromeSnapshotPlan.hpp`. Look at every `settings*` picture on all four rows: the MacOS sidebar 230 with tiles, the
  Fluent cards and breadcrumb, the ContourLinux split header bar and boxed lists, the Terminal row's cells with no band
  below the page; `settings-profile-readonly.*` shows the banner and Duplicate to Edit.
- [ ] **Step 4: Owner manual checks**, batched per phase: the Phase A–F lists from Tasks 5, 10, 12, 14 and 17, in the
  ledger, not awaited; plus Windows and Linux runs of Settings at Step 5a/5b's checks (R-j).
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Rebuilds the Settings window as General, Appearance, Keyboard, Shell Integration and Profiles, with search and an
    Edit contour.yml… entry; rows use switches, segmented controls, steppers, sliders, a font picker and colour wells."
  - "Applies every Settings change live and writes it atomically; Revert Changes undoes the session's edits; a file
    changed in an editor is never overwritten; profiles defined in contour.yml stay read-only, with Duplicate to Edit."
  - "Shows a live terminal preview on every profile page."
  - "Turns the colour-scheme, link-pattern and status-line editors into pages; colour wells show their contrast."
  - "`option_as_alt` takes off, left, right or both, so one Option key can type accents; true and false still work."
  - "Settings reopens where it was, on the page it showed."
- [ ] **Step 6:** Mark Step 4b done in the plans index's step table and *Last update*, and on the board with
  `/status-board update`. Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-4b.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none on the render path; Settings writes at most once per 300 ms of edits, one reload per flush;
    the preview is one extra 80 × 6 terminal while Settings shows a profile.
  - **Risk:** high. The save path changes for every Settings write (Tasks 1–4: durable writes through the injected file
    system, conflict detection, Revert); the whole page is rewritten (Task 10); a config-schema change at the YAML
    boundary (Task 11); key handling on macOS (Task 12).
  - **Coverage:** the `[guiconfigstore]`, `[settingssession]`, `[settingssections]`, `[settingscontrols]`,
    `[profilerules]`, `[optionasalt]`, `[contrastreadout]` tables; the controller's live-apply, Revert, conflict, lock
    and read-only cases; QML property, keyboard and accessibility tests on the native and Terminal rows; nine snapshot
    scripts.

---

## Owner decisions

The run proceeds on each default.

1. **Everything applies live (R-a).** *Default: profiles, colour schemes and `settings.yml` all apply live through one
   300 ms debounce; the Save buttons go.* The alternative keeps the scheme editor's explicit Save (§3.13 only names
   profiles).
2. **Settings layouts for Windows and Linux now (R-j).** *Default: all four rows get their board values in 4b; the
   FluentWinUI3 and ContourLinux control styles stay 5a's and 5b's.* The alternative keeps the old page on those rows
   until Phase 5, which means two Settings pages for a phase.
3. **Where the font picker's families come from (R-i).** *Default: `platform::IFontFamilySource` over `QFontDatabase`'s
   fixed-pitch families, JetBrains Mono first.* The alternative adds a family enumeration to `text::FontLocator` for
   CoreText, DirectWrite and fontconfig, which §3.13's "the locator's family list" suggests but no locator has.
4. **Sidebar icons (R-k).** *Default: monochrome glyphs from a table, on coloured tiles; SF Symbols and Segoe Fluent
   icons later.*
5. **The status-line page (R-l).** *Default: a profile sub-page, with Appearance › Status Line opening the default
   profile's.* The canvas puts it under Appearance, but the template is per profile.
6. **A file changed on disk wins (R-c).** *Default: Settings writes nothing over bytes it has not seen, adopts the
   disk's version and says so.* The alternative asks ("Keep Settings' change / Use the file").

## Spec gaps found

- §3.13 cites `Config.hpp:1137`; the field is `TerminalProfile::optionKeyAsAlt` at `:1277`, and the Settings row's key was
  `option_key_as_alt`, a key no file has (R-p).
- "The locator's family list": `text::FontLocator` has none (`all()` returns font sources) (R-i).
- "Revert restores the snapshot taken when the page opened" says nothing of files changed on disk meanwhile, nor of a
  window that is closed and reopened (R-b, R-c).
- §3.13 says "Profiles apply live"; the canvas says everything does (R-a).
- The global fields have no groups for the section table to map; 4b names them (R-h).
- "Atomic" writes today go through `std::ofstream` (`AtomicFileWrite.cpp`), outside the injected `FileSystem` (Task 1).
- The left/right Option distinction relies on Qt passing NSEvent's device-dependent bits in `nativeModifiers()`, which
  Qt does not document (R-q; a display check).
- The canvas's Shell Integration rows "install status per shell" and "the quick-terminal hotkey" belong to 6.1 and 6.7 (R-m).
- The status-line page sits under Appearance on the canvas, but the template is per profile (R-l).
- The snapshot scene has no session manager, so the preview's content is not in the pictures (R-r).
- Step 3e has no plan file at this HEAD; if it adds a Settings row (the scrollbar mode), the coverage test of Task 6
  names the group it needs.
- Several Fluent and ContourLinux extents (marked † in Global Constraints) have no number on their boards.
