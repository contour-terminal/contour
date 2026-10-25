# Native Redesign — Step 3a (Status Line Content: Placeholders, Flags, Role Colours, Collapse, Final Template) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25) at HEAD `30cdc3f0`, before Steps 4a (final fixes), 1c, 1d and 2
land. Line numbers are at that HEAD. Where 3a touches code those steps add or change, the task names it as their
plans do and carries a **Depends on Step <id>** note; locate such code by name. A pre-flight scan re-verifies every
reference before Task 1. Steps 3a–3f may run in any order or in parallel worktrees: see *Shared files* below.

**Goal:** The indicator status line says what changes while you work: five new placeholders (`{Cwd}`, `{Git}`,
`{LastCommand}`, `{Selection}`, `{Scroll}`), an `InputMode` that hides in Insert and draws as a chip, colours given as
scheme roles, segments that drop by priority in a narrow pane, and the approved template as the `Native2026` default.

**Architecture:**
- **Everything decides in `vtbackend`, Qt-free.** Each placeholder is the four edits §3.7 names (struct, `Item`
  alternative, `ItemTraits`, a `VTSerializer::visit`), plus its payload's parse and serialise branch where it has one.
  The status line stays cells, drawn by the same builder on every `ChromeStyle` row, the Terminal row included.
- **Colours are data resolved at render time.** `Styles` holds an `ItemColor` (`RGBColor` or a `ColorRole`); a
  five-row table maps each role to a palette index, read when the line is drawn, so a scheme change recolours it.
- **The branch is a cached file read through the injected `core::platform::FileSystem`** the `Terminal` already takes:
  `GitHeadCache` walks to `.git` (a directory, or a worktree's `.git` file), keys on HEAD's path and mtime, and does
  I/O only after OSC 133 D, an OSC 7 report, or a directory change. Never per frame.
- **Collapse is a pure table.** `StatusLineLayout.hpp` holds today's placement rule and a drop loop over
  `ItemTraits<T>::Priority`; `Terminal::updateIndicatorStatusLine` measures each item and asks it what to draw.
- **The template is one row of the edition table.** Only `Native2026IndicatorTemplate` changes; Classic users keep
  theirs (§2.7).

**Tech Stack:** C++23, Catch2 v3, Qt 6.11 QML (the chip editor only), CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.7 (`:521-549`) binds, with
what it cites: §1 (principles, `:28-64`), §2.6 (testing, `:206-226`), §2.7 (the edition gate, `:228-284`), §4 (the
`Git` risk row, `:837`). §3.7's code citations have moved: `currentWorkingDirectory()` is `Terminal.hpp:3512`, the
`FileSystem` parameter `:834`, `ItemTraits`/`forEachItemType` `StatusLineBuilder.hpp:200-275`, the visits
`StatusLineBuilder.cpp:366-779`, `tryParseColorAttribute` `:69-76`; `finishedBlocks` is
`SemanticBlockTracker.hpp:98-99` and `CommandLineText::display()` is `CommandLineText.hpp:34` (see R-c). The design
record is `docs/superpowers/specs/2026-10-23-native-redesign-design.md` (`:59-67`: "on by default, cell-drawn and
scheme-coloured", the template, "the new `Hide=` flag and the role colours … land in Phase 3"). Visual authority (data
only): `.superpowers/design/canvas/project/statusline.dc.html`, `tui.dc.html` ("Status line … uses the scheme's
colors (no #0270C0) and sits in the last row"), `defaults-all.dc.html` ("a quiet status line saying where you are and
how the last command went"). Canvas: <https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>. Plans index:
`docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`. Steps 4a, 1c, 1d and 2
(`2026-10-25-native-redesign-step-4a.md`, `-1c.md`, `-1d.md`, `-2.md`) are assumed done.

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

- **The spec, verbatim (§3.7):**
  - Each new placeholder "takes four edits: the struct, the variant, `ItemTraits`, and a serializer overload".
  - `Git`: "Read through the `core::platform::FileSystem` the `Terminal` already takes … `readFile`, with
    `lastWriteTime` for the cache key. The cache key is the cwd plus `.git/HEAD`'s mtime, invalidated on OSC 133 D.
    It is never read per frame. Tested with `core::platform::testing::InMemoryFileSystem`. No new interface."
  - `Selection`: "Always reports lines. Reports characters only when the selection is below 10 000 cells; above that,
    lines only. Never O(selection) per frame."
  - `Scroll`: "`viewport().scrollOffset()` against the history size … Empty at the bottom."
  - "A new `Hide=Insert` flag on `InputMode`." "Role colours: `Color=accent|success|danger|warning|muted`, resolved
    against the palette at render time. `tryParseColorAttribute` … gains a branch for these."
  - "A pure collapse step drops segments by priority when the line is too narrow."
  - "This step switches to the full approved template. The Settings chip editor picks the new placeholders up
    automatically." The Settings *page* (Appearance › Status Line, live preview) is not §3.7's; it is Step 4b's.
- **Canvas values, exact** (`statusline.dc.html`):
  - Texts: `✓ ctest · 2m 14s`, `✗ exit 8 ctest -R gui · 12.4 s`, `✓ make · 4 ms`, `↑ 1 240 / 10 000`,
    `12 lines · 418 chars`, `~/repo/contour`, `feature/native-redesign`, `NORMAL`, `14:02`.
  - Roles resolve to the scheme's palette: accent = 4 (blue), success = 2 (green), danger = 1 (red), warning = 3
    (yellow, "yellow means search"), muted = 8 (bright black, "secondary text is bright black"). Running = accent.
    Never `#0270C0` or `#FFFF00`; "`Color=` values in user templates still win"; "Hex still works".
  - Mode chip: "Hidden in Insert, a filled chip otherwise"; one cell of padding each side, no radius. Separators are
    spacing. Narrow pane: "segments drop right to left, by priority".
  - `{Scroll}` "nothing at the bottom"; `{LastCommand}` "only with shell integration"; `{Cwd}` "home shown as ~,
    middle-elided"; `{Git}` "a file read, no process spawned".
- **Binding rulings** (ledgers 0, 1a, 1b, 1e; the 4a, 1c, 1d and 2 SDD ledgers): status colours are roles, never
  `#0270C0`/`#FFFF00`; Running is the accent, never green or yellow; yellow means search; every new default goes
  through `DefaultsEdition`'s table and Classic keeps its look (R17); TestApp is Classic (R14); `vtbackend`,
  `contour_config` and `contour_command` stay Qt-free; owner manual checks are batched per phase, listed in the
  ledger, never awaited.
- **AGENT.md applies in full:**
  - **Data-driven.** Roles, input-mode spellings, priorities and the edition's template are tables; a sixth role is a
    row of `ColorRoleNames`, a new placeholder's priority is one `ItemTraits` constant. `MaxWidth` is one generic
    branch for every item that has a `maxWidth` member, not one per type.
  - **Dependency injection.** The only I/O (`{Git}`) goes through the `FileSystem` the `Terminal` is constructed with
    (`Terminal.hpp:834`); tests pass an `InMemoryFileSystem`. No new interface, no `NativeFileSystem::instance()`.
  - **Configuration at construction.** `GitHeadCache` takes its `FileSystem` in its constructor; `invalidate()` and
    `refresh()` are domain state.
  - **`enum class` over `bool`,** zero the default (`ColorRole::Accent` is first only because no role is a
    default; `InputModeLook::Text`, `StatusLinePriority::Low`, `StatusLineItemFate::Shown`, `GitHeadKind::Branch`,
    `GitHeadFreshness::Stale`). A surviving `bool` return asks a question (`showsModeChip`, `containsItem`). The
    chip editor's `chip` key is a `QVariantMap` boolean, the documented QML carve-out.
  - **Doxygen** (`///`, `@param`, `@return`) on every new public declaration; `.hpp` with `#pragma once`.
  - **Zero warnings** under `-Werror`, **no `NOLINT`**. Every new struct member gets an initializer (`{}` or a value):
    the settings bridge builds unknown items as `T { styles }` (`SettingsController.cpp`, `serializeIndicatorSegment`),
    and clang 22's `-Wmissing-designated-field-initializers` / `-Wmissing-field-initializers` break CI otherwise.
- **Build** with every core and `-DCONTOUR_EXTENSIONS_BUNDLED=OFF` (already in `out/appleclang-debug/CMakeCache.txt`):
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never run two builds at once.
- **clang-tidy** every touched `.cpp`:
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <files>`.
  Expected: no diagnostics.
- **Format** every touched `.cpp`/`.hpp` with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`. Check every touched
  `.qml`, `.md`, `.txt` and `CMakeLists.txt` with `editorconfig-checker <files>`. **Never clang-format a `.qml` file.**
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows. Counts in tests
  come from tables (`ColorRoleNames.size()`, `InputModeNames.size()`, `std::variant_size_v<Item>`), never literals.
  Never `co_await` inside a Catch2 assertion.
- **TUI row.** The status line is cells and never reads `ChromeStyle`, so it renders identically on every row; its
  acceptance is the cell tests here. The one QML change (Task 9) also runs with `CONTOUR_TEST_CONTROLS_STYLE=ContourTui`.
- **QML:** no new colour or size literal (`check_qml_colors`, `check_qml_sizes`); new controls none.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files by name (other agents share the tree; never `git add -A` or
  a directory). Each message ends with exactly `Signed-off-by: Christian Parpart <christian@parpart.family>`, and
  never carries `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 11).

## Review Focus

1. **A working directory in a deep or non-UTF-8 path.** OSC 7 may report a 60-level path, a 300-byte component, or
   bytes that are not UTF-8 (`%FF`) or are controls (`%1B`). Expected: `{Cwd}` is home-abbreviated, never wider than
   its `MaxWidth` (keeping the first and last components, `/…/d59`), shows U+FFFD for bad bytes and never writes an
   ESC to the status screen; `{Git}` still finds a `.git` 100 levels up, and a path Windows cannot spell gives no
   branch rather than a wrong one. Pinned in Task 2 (`serializeToVT.cwdOfADeepOrBrokenPath`) and Task 3
   (`GitHead.findsTheRepositoryFromADeepDirectory`).
2. **A detached HEAD, or a worktree's `.git` file.** `git checkout <sha>` leaves a bare hash in HEAD; `git worktree
   add` and submodules leave `.git` as a file (`gitdir: …`, absolute or relative). Expected: seven hex digits for a
   detached HEAD, the worktree's own branch, and nothing (never a crash or the parent's branch) for a malformed or
   oversized file. Pinned in Task 3 (`GitHead.parsesEveryHeadForm`, `GitHead.followsAWorktreeFile`).
3. **A very narrow pane.** A split can leave 12, 5 or 1 column. Expected: low-priority items go first (Clock, then
   Git), ties go right to left, the mode chip and a privilege boundary (`{Context}`) go last, a single item wider than
   the line is cut at the last column as today, nothing overlaps and nothing crashes. Pinned in Task 8
   (`collapseStatusLine.dropsByPriorityRightToLeft`, `Terminal.indicatorStatusLine.anyWidthDrawsWithoutOverlap`).
4. **A user template mixing hex colours and the new role keys.** `{Cwd:Color=#C0C030}{Git:Color=muted}
   {Clock:Color=#000000,BackgroundColor=accent}`. Expected: hex is drawn as written, a role follows the active scheme
   (and a live scheme change), the template round-trips byte for byte through the parser and through the Settings
   chip editor, and an unknown word (`Color=magenta`) leaves the colour unset rather than turning it black. Pinned in
   Task 1 (`statusLine.colors.hexAndRolesMix`, `SettingsController: the indicator bridge keeps role colours`) and
   Task 9 (`StatusLineItemDialog keeps an item's roles and payload`).
5. **The status line in a split with one pane inactive.** Every pane has its own line. Expected: the unfocused pane's
   line shows its own directory and last command on the scheme's inactive pair, its role colours from its own palette,
   and the focused pane's mode chip never leaks into it; refocusing restores the chip. Pinned in Task 10
   (`IndicatorTemplate.eachPaneDrawsItsOwnLine`).

Also pinned, beyond the five: a Classic template in a pane wide enough renders cell for cell as before (Task 8, the
existing `[terminal]` indicator cases pass unedited); an item opened and saved in the chip editor keeps `Hide=`,
`Chip` and `MaxWidth` (Task 9).

---

## Spec resolutions (read before Task 1)

Implementers follow these; those marked **(owner)** are repeated under *Owner decisions*.

- **R-a. Placement.** All placeholders, roles and payload helpers go into the existing `StatusLineBuilder.hpp/.cpp`
  (namespace `vtbackend::StatusLineDefinitions`), so Tasks 1–7 add no files. New files are `screen/GitHead.*`
  (Task 3), `screen/StatusLineLayout.*` (Task 8) and `config/IndicatorTemplate_test.cpp` (Task 10). The `Selection`
  placeholder's struct is `SelectionSize` (placeholder name `"Selection"`), because `vtbackend::Selection` is the
  selector class and both namespaces meet in `SettingsController.cpp`.
- **R-b. Which working directory.** `{Cwd}` shows `Terminal::resolveWorkingDirectory(CwdPurpose::Display)`
  (`Terminal.hpp:3082`): OSC 3008 first, then OSC 7, so a container's `/srv/app` shows, as the canvas draws it. §3.7's
  `currentWorkingDirectory()` is the raw OSC 7 URL, undecoded. `{Git}` reads only a provably local directory,
  `CwdPurpose::OpenLocally`, converted with `core::pathFromUtf8`; an ssh or foreign directory has no branch.
- **R-c. The last command.** `SemanticBlockTracker::finishedBlocks` is a DEC 2034 reader-session view and returns
  nothing without a session (`SemanticBlockTracker.cpp:60-61`). `{LastCommand}` reads the store directly:
  `commandBlocks().lastFinished()` (`CommandBlock.hpp:564`) when its `end == CommandBlockEnd::Reported`, and nothing
  while `commandBlocks().current()` is `Running`. "Shown until the next prompt" is read as until the next command
  starts: the prompt after a command is where it is read. The command text is `commandLine.display()`.
- **R-d. Git cache semantics.** Per frame no I/O. `refresh(dir)` does nothing while the cache is `Fresh` for `dir`.
  After `invalidate()` (OSC 133 D in `Terminal::endCommand`, every OSC 7 report in `setCurrentWorkingDirectory`,
  changed or not, since shells re-report per prompt) or a different `dir`, it walks up to the first `.git`, stats
  HEAD, and re-reads only when `{HEAD path, mtime}` differs from the key it holds. `InMemoryFileSystem::lastWriteTime`
  returns `now()` (`InMemoryFileSystem.cpp:784`), so in tests every refresh after an invalidation re-reads; the
  equal-key branch is pinned by comparing keys, and "never per frame" by a stale-until-invalidated test. The I/O runs
  on the render thread under the terminal lock, once per command at most (§4's row).
- **R-e. Mode chip (owner).** Phase 0 colours the whole line by mode (`applyContourScheme`, `ColorPalette.cpp:128-131`:
  Normal = page on blue, Visual = page on magenta). With role colours on it, green on blue is unreadable, and an accent
  chip vanishes. So `InputMode` gains a `Chip` flag: the item draws in its mode's pair (`indicatorStatusLine
  {Normal,Visual,Insert}Mode`), which is exactly the canvas's chip (accent for NORMAL, magenta for VISUAL), and a line
  whose template shows a chip keeps the idle (Insert) pair in every mode. A template without `Chip` (Classic, any user
  template) keeps today's whole-line colours. No palette changes.
- **R-f. Roles.** Parsed case-insensitively, written lowercase. They apply to `Color`, `BackgroundColor`, and `Tabs`'
  `ActiveColor`/`ActiveBackground` alike: one colour type, one parser. A word that is neither a role nor
  `parseColor`'s syntax leaves the colour unset, as `Color=Yellow` does today. The settings bridge parses through the
  same function, so an unparsable value becomes unset, not black (`hexToRgb` gives black today).
- **R-g. Intrinsic colours.** `{LastCommand}` draws its status run (`✓` / `✗ exit N`) in success / danger and ` · <duration>`
  in muted, and the chip draws in its mode pair, **only when the item names no `Color=` of its own**; with one, the
  whole item takes it (user colours win).
- **R-h. Collapse.** It drops whole items, never shortens one (the canvas's `N`, `✗ 8`, `↑1.2k` compact forms are a
  spec gap). It keeps today's placement rule (`Terminal.cpp:1696-1720`: the middle is centred or not shown; the right
  ends in the last column or is not shown) as `placeStatusLineSegments`, and drops while some non-empty segment would
  not be placed and more than one non-empty item remains. Order: lowest `Priority` first; among equals, the right
  segment's last item, then the middle's, then the left's. A Classic template that fits draws exactly as today.
- **R-i. Payload survival in the chip editor.** The editor's dialog rebuilds an item from scratch on Apply
  (`StatusLineItemDialog.qml:126-160`), which already drops `Context`'s `verbosity` and `maxWidth`; with the new default
  it would drop `Hide=Insert`. Apply starts from a copy of the item it was opened with. The bridge carries `hide`,
  `chip` and a generic `maxWidth`.
- **R-j. The template (owner).** Only `Native2026IndicatorTemplate` (`Config.hpp:664-668`) changes; 0.9.0 is
  unreleased, so `native2026` is edited in place, not a third edition. The builder has no implicit gap, so the
  canvas's placeholder order gains `Left=`/`Right=` spacing and roles where the canvas colours an item:
  - left: `{InputMode:Bold,Chip,Hide=Insert,Left= ,Right= }{Context:Left= }{Cwd:Color=success,Left= }{Git:Left=  }`
  - middle: `{LastCommand}{Selection:Left=  }{SearchPrompt:Color=warning,Left=  }`
  - right: `{Progress:Color=accent,Right=  }{Scroll:Right=  }{Clock:Faint,Right= }`

  `{Git}` takes the foreground: the canvas draws it magenta, which no role names. `{Tabs}` and `{Title}` leave the
  default (the canvas: "the chrome already shows both").

---

### Task 1: Role colours

**Files:**
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp:25-32` (`Styles`), `:125-130` (`Tabs`), plus the role table and
  three functions after `CellFlagNames` (`:171-185`).
- Modify: `src/vtbackend/screen/StatusLineBuilder.cpp:69-76` (`tryParseColorAttribute`), `:266-281`
  (`makeTextColor`, `makeBackgroundColor`), `:823-831` (`AttributeWriter::addColor`).
- Modify: `src/contour/window/SettingsController.cpp` — `writeColorPair`, `writeStyles`, `optionalColor` (`:2614-2666`).
  **Depends on Steps 4a and 2** (both edit this file elsewhere; locate by name).
- Modify: `docs/configuration/indicator-statusline.md` — the `Color=`/`BackgroundColor=` rows name the five roles.
- Test: `src/vtbackend/screen/StatusLineBuilder_test.cpp` (update `:589-590` to compare an `ItemColor`),
  `src/contour/window/SettingsController_test.cpp` (the indicator bridge block, `:1519-…`).

**Interfaces:**
- Produces (namespace `vtbackend::StatusLineDefinitions`):
  - `enum class ColorRole : uint8_t { Accent = 0, Success, Danger, Warning, Muted };`
  - `struct ColorRoleName { ColorRole role; std::string_view name; std::size_t paletteIndex; };`
  - `inline constexpr auto ColorRoleNames = std::array { {Accent,"accent",4}, {Success,"success",2}, {Danger,"danger",1}, {Warning,"warning",3}, {Muted,"muted",8} };`
    with a `static_assert` that row *i* is enumerator *i*.
  - `using ItemColor = std::variant<RGBColor, ColorRole>;`
  - `[[nodiscard]] std::optional<ItemColor> parseItemColor(std::string_view text);` — a role (ASCII
    case-insensitive), else `parseColor(text)`, else nullopt.
  - `[[nodiscard]] std::string formatItemColor(ItemColor const& color);` — the role's name, or `formatColor`.
  - `[[nodiscard]] RGBColor resolveItemColor(ItemColor const& color, ColorPalette const& palette) noexcept;` —
    `palette.indexedColor(row.paletteIndex)` for a role.
  - `Styles::foregroundColor`, `Styles::backgroundColor`, `Tabs::activeColor`, `Tabs::activeBackground` become
    `std::optional<ItemColor>`.
- `tryParseColorAttribute` returns `std::optional<ItemColor>` through `parseItemColor`; `VTSerializer::makeTextColor`/
  `makeBackgroundColor` take `std::optional<ItemColor> const&` and resolve against `vt.colorPalette()` when drawing.

- [ ] **Step 1: Write the failing tests** (`[statusline][colors]`):
  - `statusLine.colors.everyRoleParsesAndRoundTrips`: for each row of `ColorRoleNames`,
    `parseItemColor(row.name) == ItemColor { row.role }`, also for the upper-cased spelling;
    `formatItemColor(ItemColor { row.role }) == row.name`; `parseItemColor("#FF0000") == ItemColor { RGBColor { 0xFF, 0, 0 } }`;
    `parseItemColor("magenta")`, `("Yellow")` and `("")` are nullopt.
  - `statusLine.colors.hexAndRolesMix` (Review Focus 4): `"{Cwd…}"` is not there yet, so use
    `"{VTType:Color=#C0C030}{Clock:Color=muted}{Title:Color=#000000,BackgroundColor=accent}"`;
    `serializeStatusLineSegment(parse(t)) == t` byte for byte; `"{Clock:Color=Accent}"` serialises to
    `"{Clock:Color=accent}"`; `"{Clock:Color=magenta}"` parses to a `Clock` with no colour.
  - `serializeToVT.roleIsResolvedWhenDrawn`: a `MockTerm`; `{VTType:Color=accent}` with `Styling::Enabled` contains
    `std::format("\033[38:2:{}:{}:{}m", c.red, c.green, c.blue)` for `c = colorPalette().normalColor(4)`; after
    `colorPalette().palette[4] = 0x123456_rgb` the same segment contains `38:2:18:52:86`.
  - `statusLine.colors.rolesReadOnContourSchemes`: for `contour-dark` and `contour-light` (`defaultColorPalettes`), every
    role's `contrastRatio(resolveItemColor(role, p), p.indicatorStatusLineInactive.background)` is `>= 4.5`, muted
    `>= 3.0` (measured today: lowest 4.58 and 4.14). If it fails, stop and report: scheme values are not this step's.
  - `SettingsController_test.cpp`, `[settings]`: "the indicator bridge keeps role colours" — `parseIndicatorSegment`
    of the Review Focus 4 template gives rows whose `color`/`bgColor` are `"muted"`, `"accent"` and `"#C0C030"` with
    `hasColor` true, and `serializeIndicatorSegment` of those rows returns the template unchanged; a row with
    `hasColor` true and `color` `"#zz"` serialises with no `Color=`.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test`
  Expected: compile failure, `ColorRole` is not a member of `StatusLineDefinitions`.
- [ ] **Step 3: Implement** the table, the three functions (definitions in `StatusLineBuilder.cpp`), the type change,
  the resolve-on-draw, and the bridge: `writeColorPair` writes `formatItemColor`, `writeStyles`/`optionalColor` read
  through `parseItemColor(text.toStdString())`. `hexToRgb` stays for its other caller (`:2484`). Update the docs rows.
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test contour_gui_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[statusline]" && out/appleclang-debug/src/contour/contour_gui_test "[settings]"`
  Expected: PASS, the existing `[statusline]` and `[settings]` cases unedited apart from `:589-590`.
- [ ] **Step 5: Tidy, format, commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/screen/StatusLineBuilder.cpp src/contour/window/SettingsController.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/screen/StatusLineBuilder.{hpp,cpp} src/vtbackend/screen/StatusLineBuilder_test.cpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp
editorconfig-checker docs/configuration/indicator-statusline.md
git add src/vtbackend/screen/StatusLineBuilder.hpp src/vtbackend/screen/StatusLineBuilder.cpp src/vtbackend/screen/StatusLineBuilder_test.cpp src/contour/window/SettingsController.cpp src/contour/window/SettingsController_test.cpp docs/configuration/indicator-statusline.md
git commit -m "vtbackend: status-line colours may name a scheme role, resolved when drawn

Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: `{Cwd}`, and `MaxWidth` for every item that has one

**Files:**
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp` — `struct Cwd`, appended to `Item` (`:132-152`), `ItemTraits<Cwd>`.
- Modify: `src/vtbackend/screen/StatusLineBuilder.cpp` — `makeItemOfType` (`:161-172`: `Context`'s `MaxWidth` branch
  becomes the generic one), `VTSerializer::elide` (`:657-716`, now a free function in the anonymous namespace),
  `visit(Cwd const&)`, `serializeStatusLineSegment` (`:960-972`: the generic `MaxWidth` write).
- Modify: `src/vtbackend/screen/Terminal.hpp` — public `homeDirectory()` beside `currentWorkingDirectory()` (`:3512`).
- Modify: `docs/configuration/indicator-statusline.md` — the `{Cwd}` row and a `MaxWidth=` row.
- Test: `src/vtbackend/screen/StatusLineBuilder_test.cpp`.

**Interfaces:**
- Produces: `struct Cwd: Styles { ColumnCount maxWidth { 40 }; };` — `ItemTraits<Cwd>`: Name `"Cwd"`, Label
  `"Working directory"`, Sample `"~/repo/contour"`.
- Produces: `[[nodiscard]] std::string_view Terminal::homeDirectory() const noexcept;` — `_homeDirectory` (`:4376`).
- `MaxWidth=<n>` (n > 0) is parsed and written for every `T` with `requires (T t) { t.maxWidth; }`, and written only
  when it differs from `T {}.maxWidth`. `Context` keeps its spelling and default (32).
- Produces (anonymous namespace, `StatusLineBuilder.cpp`):
  `std::string elideMiddle(std::vector<std::string> const& parts, std::string_view separator, ColumnCount maxWidth);` —
  today's `elide`, unchanged in behaviour.
- `visit(Cwd const&)`: `resolveWorkingDirectory(CwdPurpose::Display)`; none → empty. Otherwise
  `sanitizeCommandLine(abbreviateHomePath(path, homeDirectory()), SanitizePurpose::Display)`, split on `/`, then
  `elideMiddle(parts, "/", maxWidth)`.

- [ ] **Step 1: Write the failing tests** (`[statusline][cwd]`), each through `serializeToVT(…, Styling::Disabled)` of
  `{Cwd}` after `mc.writeToScreen("\033]7;<url>\033\\")`:
  - `serializeToVT.cwdShowsWhereTheShellIs`: no report → `""`; `file:///srv/app` → `"/srv/app"`;
    `file://elsewhere.example/srv/app` → `"/srv/app"` (shown, though foreign); when
    `core::defaultEnvironment().get("HOME")` is non-empty, `file://<home>/repo/contour` → `"~/repo/contour"`.
  - `serializeToVT.cwdOfADeepOrBrokenPath` (Review Focus 1): `/d0/…/d59` → `"/…/d59"`; one 300-character component
    `/aaa…` → a result whose column width is `<= 40` and which starts with `"…"`; `file:///tmp/a%FFb` →
    `"/tmp/a�b"`; `file:///tmp/a%1Bb` → contains no `\033`.
  - `parseStatusLineSegment.maxWidthIsGeneric`: `{Cwd:MaxWidth=20}` round-trips; `{Cwd}` has `maxWidth == 40` and
    serialises without `MaxWidth`; `{Cwd:MaxWidth=0}` keeps 40; the existing `Context` cases pass unedited.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test`
  Expected: compile failure, no `StatusLineDefinitions::Cwd`.
- [ ] **Step 3: Implement** the struct, the trait, the visit, the generic `MaxWidth` and `homeDirectory()`.
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test contour_gui_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[statusline]" && out/appleclang-debug/src/contour/contour_gui_test "[settings]"`
  Expected: PASS. `serializeStatusLineSegment.everyPlaceholderNameRoundTrips` now covers `Cwd` too.
- [ ] **Step 5: Tidy, format, commit** (`StatusLineBuilder.{hpp,cpp}`, `StatusLineBuilder_test.cpp`, `Terminal.hpp`,
  the docs page), message `vtbackend: a {Cwd} placeholder, home-abbreviated and middle-elided`.

---

### Task 3: `{Git}`, read through the terminal's filesystem

**Files:**
- Create: `src/vtbackend/screen/GitHead.hpp`, `src/vtbackend/screen/GitHead.cpp`, `src/vtbackend/screen/GitHead_test.cpp`.
- Modify: `src/vtbackend/CMakeLists.txt` — the header beside `screen/StatusLineBuilder.hpp` (`:108`), the source beside
  `:204`, the test beside `:362`.
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp/.cpp` — `struct Git`, `Item`, `ItemTraits<Git>`, `visit(Git const&)`,
  and `containsItem<T>`.
- Modify: `src/vtbackend/screen/Terminal.hpp` — `#include <vtbackend/screen/GitHead.hpp>`, member `_gitHead` declared
  after `_fileSystem` (`:4379`), `gitBranch()`, private `refreshGitHead()`.
- Modify: `src/vtbackend/screen/Terminal.cpp` — the constructor's initializer list (`:280-…`), `refreshGitHead()`
  called in `updateIndicatorStatusLine` after the mirror and take-over returns (`:1640-1669`), `_gitHead.invalidate()`
  in `endCommand` (`:6617-6622`) and `setCurrentWorkingDirectory` (`:6645-6661`, before the `changed` test).
- Modify: `src/vtbackend/testing/MockTerm.hpp` — a constructor taking a `FileSystem` (`:52-88`, `:470-500`).
- Modify: `docs/configuration/indicator-statusline.md` — the `{Git}` row.
- Test: `GitHead_test.cpp`, `src/vtbackend/screen/StatusLineBuilder_test.cpp`.

**Interfaces:**
- Produces (namespace `vtbackend`, `GitHead.hpp`, Qt-free, `#include <core/platform/FileSystem.hpp>`):
  - `enum class GitHeadKind : uint8_t { Branch = 0, Detached, OtherRef };`
  - `struct GitHead { GitHeadKind kind = GitHeadKind::Branch; std::string name {}; bool operator==(GitHead const&) const = default; };`
  - `inline constexpr std::uintmax_t MaxGitMetadataSize = 4096;` — a HEAD or `.git` file larger is ignored.
  - `inline constexpr std::size_t DetachedHeadDigits = 7;`
  - `[[nodiscard]] std::optional<GitHead> parseGitHead(std::string_view contents);` — `ref: refs/heads/<n>` →
    `{Branch, n}`; `ref: refs/<r>` → `{OtherRef, r}`; 40 or 64 hex digits → `{Detached, first 7}`; trailing
    whitespace ignored; anything else nullopt.
  - `[[nodiscard]] std::optional<std::filesystem::path> findGitHeadFile(core::platform::FileSystem const& fileSystem, std::filesystem::path const& directory);`
    — walks from @p directory to the root; at the first `.git`: a directory gives `.git/HEAD` if it is a regular file;
    a regular file (≤ `MaxGitMetadataSize`) whose first line is `gitdir: <p>` gives `<p>/HEAD`, `<p>` relative to the
    directory holding the `.git` file; anything else at that first `.git` gives nullopt (the walk stops there, as git does).
  - `class GitHeadCache` with `explicit GitHeadCache(core::platform::FileSystem const& fileSystem);`,
    `void invalidate() noexcept;`, `void refresh(std::optional<std::filesystem::path> const& directory);`,
    `[[nodiscard]] std::string const& branch() const noexcept;` (display text: `sanitizeCommandLine(name, Display)`;
    empty for none). Private: `enum class GitHeadFreshness : uint8_t { Stale = 0, Fresh };`, the directory, the key
    `struct { std::filesystem::path head; std::filesystem::file_time_type mtime; }`, the branch. Semantics: R-d.
- Produces (`StatusLineBuilder.hpp`): `struct Git: Styles {};` — Name `"Git"`, Label `"Git branch"`, Sample `"main"`;
  `template <typename T> [[nodiscard]] bool containsItem(StatusLineDefinition const& definition) noexcept;`.
- Produces (`Terminal`): `[[nodiscard]] std::string const& gitBranch() const noexcept;` (public, beside
  `homeDirectory()`); `void refreshGitHead();` (private): only when `containsItem<StatusLineDefinitions::Git>`, it
  passes `resolveWorkingDirectory(CwdPurpose::OpenLocally)` through `core::pathFromUtf8` (an error → nullopt) to
  `_gitHead.refresh`. `visit(Git const&)` returns `vt.gitBranch()`.
- Produces (`MockTerm`): `MockTerm(PageSize size, core::platform::FileSystem const& fileSystem);` — the terminal reads
  printed paths and Git metadata from @p fileSystem; everything else as `MockTerm { size }`.

- [ ] **Step 1: Write the failing tests.** `GitHead_test.cpp`, `[git]`, over an `InMemoryFileSystem files`:
  - `GitHead.parsesEveryHeadForm` (Review Focus 2), a `GENERATE(table…)`: `"ref: refs/heads/main\n"` → `{Branch,"main"}`;
    `"ref: refs/heads/feature/native-redesign"` → `{Branch,"feature/native-redesign"}`; `"ref: refs/remotes/origin/x"` →
    `{OtherRef,"remotes/origin/x"}`; 40 hex `a1b2c3d…` → `{Detached,"a1b2c3d"}`; 64 hex → 7 digits; `""`, `"ref:"`,
    `"garbage"`, 39 hex → nullopt.
  - `GitHead.findsTheRepositoryFromADeepDirectory` (Review Focus 1): `.git/HEAD` under `/r`; from `/r` and from
    `/r/d0/…/d99` → `/r/.git/HEAD`; from `/elsewhere` → nullopt.
  - `GitHead.followsAWorktreeFile` (Review Focus 2): `/w/.git` is a file `"gitdir: /r/.git/worktrees/w\n"` → the
    `worktrees/w/HEAD`; a submodule's `"gitdir: ../.git/modules/s"` in `/r/s/.git` → `/r/.git/modules/s/HEAD`; a `.git`
    file without `gitdir:` → nullopt and the walk does not reach `/r/.git` above it; a 5 000-byte `.git` file → nullopt.
  - `GitHeadCache.isStaleUntilInvalidated`: refresh(`/r/src`) → `"main"`; `files.writeFile("/r/.git/HEAD", "ref: refs/heads/next")`;
    refresh(`/r/src`) twice → `"main"`; `invalidate()`; refresh → `"next"`; refresh(`/other`) → `""`; refresh(nullopt) → `""`.
  - `GitHeadCache.showsOnlySafeText`: a branch `"evil\x1b[2Jx"` → no `\033` in `branch()`; a 5 000-byte HEAD → `""`.
  - `StatusLineBuilder_test.cpp`, `[statusline][git]`, `serializeToVT.gitFollowsTheShell`: `MockTerm { size, files }`,
    indicator on, template `{Git}`, OSC 7 `file:///r/src`, `refreshRenderBuffer()` → the line reads `main`; rewrite HEAD;
    refresh twice → still `main`; `typeAtPrompt(mc, "git switch next")`, `\033]133;C\033\\`, `\033]133;D;0\033\\`,
    refresh → `next`; OSC 7 `file://elsewhere.example/r/src` → no branch.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test`
  Expected: compile failure, `GitHead.hpp` not found.
- [ ] **Step 3: Implement** `GitHead.{hpp,cpp}`, the item, `containsItem`, the `Terminal` wiring and the `MockTerm`
  constructor (it threads the reference into the `Settings` constructor's `terminal { … }` in place of
  `NativeFileSystem::instance()`; existing constructors are unchanged).
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test contour_gui_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[git],[statusline],[terminal]" && out/appleclang-debug/src/contour/contour_gui_test "[settings]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit** (the three new files, `CMakeLists.txt`, `StatusLineBuilder.{hpp,cpp}`,
  `StatusLineBuilder_test.cpp`, `Terminal.{hpp,cpp}`, `MockTerm.hpp`, the docs page; `editorconfig-checker` on the
  CMake file), message `vtbackend: a {Git} placeholder, read from .git/HEAD through the terminal's filesystem`.

---

### Task 4: `{LastCommand}`

**Files:**
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp/.cpp` — `struct LastCommand`, `Item`, `ItemTraits`,
  `formatCommandDuration`, `visit(LastCommand const&)`, and in `VTSerializer::operator()` (`:318-343`) the intrinsic
  colour runs (R-g).
- Modify: `docs/configuration/indicator-statusline.md` — the `{LastCommand}` row.
- Test: `src/vtbackend/screen/StatusLineBuilder_test.cpp`.

**Interfaces:**
- Produces: `struct LastCommand: Styles { ColumnCount maxWidth { 32 }; };` — Name `"LastCommand"`, Label
  `"Last command"`, Sample `"✓ make · 4 ms"`. `maxWidth` bounds the command text alone; a longer one is cut at its end
  with `…` (by columns, never inside a code point).
- Produces: `[[nodiscard]] std::string formatCommandDuration(std::chrono::steady_clock::duration duration);` — under
  1 s `"<ms> ms"`; under 60 s `"<s>.<tenths> s"` (tenths truncated); under 1 h `"<m>m <s>s"`; else `"<h>h <m>m"`.
- `visit`: R-c's record; text `"✓ <cmd> · <dur>"` for exit 0, `"✗ exit <n> <cmd> · <dur>"` otherwise; no exit code →
  no status run; no command text → no `<cmd>` and no doubled space; no duration → no ` · <dur>`.

- [ ] **Step 1: Write the failing tests** (`[statusline][lastcommand]`):
  - `formatCommandDuration.table`: 4 ms → `"4 ms"`; 999 ms → `"999 ms"`; 1 s → `"1.0 s"`; 12 449 ms → `"12.4 s"`;
    59 999 ms → `"59.9 s"`; 134 s → `"2m 14s"`; 3 725 s → `"1h 2m"`.
  - `serializeToVT.lastCommandSaysHowItWent` (`Styling::Disabled`): no shell integration → `""`;
    `typeAtPrompt(mc, "ctest")`, `;C`, `steadyClock.advance(134s)`, `;D;0`, `;A` → `"✓ ctest · 2m 14s"`; then
    `typeAtPrompt(mc, "ctest -R gui")`, `;C`, advance 12 400 ms, `;D;8` → `"✗ exit 8 ctest -R gui · 12.4 s"`; while a
    third command runs (`;C`, no `;D`) → `""`; a block a new `;A` ended without `;D` → `""`.
  - `serializeToVT.lastCommandIsBoundedAndSafe`: a 100-character command → the command part is 32 columns ending in
    `"…"`; a command line holding `\x1b` → no `\033` in the result.
  - `serializeToVT.lastCommandColoursItsStatus` (`Styling::Enabled`): after the failure, the text holds the danger
    colour's `38:2` sequence before `✗` and the muted one before ` · `; with `{LastCommand:Color=#102030}` only
    `38:2:16:32:48` appears.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test`. Expected: compile failure, no `LastCommand`.
- [ ] **Step 3: Implement.** The status and duration runs are emitted as `Tabs` does its active tab
  (`SGRSAVE`, colour, text, `SGRRESTORE`, `StatusLineBuilder.cpp:763-776`), and only with `Styling::Enabled` and no
  `foregroundColor` on the item.
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[statusline]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message `vtbackend: a {LastCommand} placeholder from the command blocks`.

---

### Task 5: `{Selection}` and `Selection::extent()`

**Files:**
- Modify: `src/vtbackend/input/vi/Selector.hpp:54-117`, `src/vtbackend/input/vi/Selector.cpp` — `extent()`.
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp/.cpp` — `struct SelectionSize`, `Item`, `ItemTraits`,
  `groupDigits`, `visit(SelectionSize const&)`.
- Modify: `docs/configuration/indicator-statusline.md` — the `{Selection}` row.
- Test: `src/vtbackend/input/vi/Selector_test.cpp`, `src/vtbackend/screen/StatusLineBuilder_test.cpp`.

**Interfaces:**
- Produces (`Selector.hpp`):
  - `inline constexpr std::size_t SelectionCharacterCountLimit = 10'000;`
  - `struct SelectionExtent { LineCount lines {}; std::optional<std::size_t> characters {}; bool operator==(SelectionExtent const&) const = default; };`
  - `[[nodiscard]] SelectionExtent Selection::extent() const;` — `lines = |to.line − from.line| + 1`, O(1).
    `characters` only when `lines × _helper.pageSize().columns < SelectionCharacterCountLimit` (an O(1) bound checked
    first), then the non-empty cells (`!_helper.cellEmpty`) of `ranges()`: at most 10 000 cells, never O(selection).
- Produces (`StatusLineBuilder.hpp`): `struct SelectionSize: Styles {};` — Name `"Selection"`, Label `"Selection size"`,
  Sample `"12 lines · 418 chars"`; `[[nodiscard]] std::string groupDigits(std::uint64_t value);` — thousands joined by
  a space: `1240` → `"1 240"`.
- `visit`: nothing without `vt.selector()` or while its state is `Waiting`; else `"<n> line(s) · <c> char(s)"`, the
  `· …` part only with `characters`; singular for 1.

- [ ] **Step 1: Write the failing tests.**
  - `Selector_test.cpp`, `[selector]`, `Selector.extentCountsLinesAndCharacters`: on the file's `TestSelectionHelper`
    with text written, a linear selection over 3 lines reports `lines == LineCount(3)` and the non-blank cell count; a
    reversed one (from below to above) the same; a rectangular one its rows; a selection whose rows × columns is
    `SelectionCharacterCountLimit - 1` reports characters, one at exactly the limit (e.g. 1 000 rows × 10 columns)
    reports `characters == std::nullopt` and still its lines.
  - `StatusLineBuilder_test.cpp`, `[statusline][selection]`: `groupDigits` table (0 → `"0"`, 999 → `"999"`,
    1000 → `"1 000"`, 1240 → `"1 240"`, 10000 → `"10 000"`, 1234567 → `"1 234 567"`); `serializeToVT.selectionSaysWhatYWouldCopy`:
    no selection → `""`; a two-line selection over `"hello world\r\nabc"` → `"2 lines · 13 chars"` (count as the
    implementation counts non-blank cells; derive the expected count from the text written); one cell → `"1 line · 1 char"`.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test`. Expected: compile failure, no `extent`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[selector],[statusline]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message `vtbackend: a {Selection} placeholder, counting characters only below 10 000 cells`.

---

### Task 6: `{Scroll}`

**Files:**
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp/.cpp` — `struct Scroll`, `Item`, `ItemTraits`, `visit(Scroll const&)`
  beside `visit(HistoryLineCount const&)` (`:408-430`).
- Modify: `docs/configuration/indicator-statusline.md` — the `{Scroll}` row.
- Test: `src/vtbackend/screen/StatusLineBuilder_test.cpp`.

**Interfaces:**
- Produces: `struct Scroll: Styles {};` — Name `"Scroll"`, Label `"Scroll position"`, Sample `"↑ 1 240 / 10 000"`.
- `visit`: `""` while `viewport().scrollingDisabled()` or the offset is 0; else
  `"↑ " + groupDigits(offset) + " / " + groupDigits(scrollableLineCount)`, the same bound `HistoryLineCount` uses.

- [ ] **Step 1: Write the failing test** `serializeToVT.scrollIsEmptyAtTheBottom` (`[statusline][scroll]`): the
  `historyItemCountsThePageOnDisplay` fixture (`:537-551`) with 1 300 lines of history; at the bottom → `""`;
  `viewport().scrollUp(LineCount(1240))` → `std::format("↑ 1 240 / {}", groupDigits(scrollable))`; back to the bottom → `""`.
- [ ] **Step 2: Run it and see it fail.** Build `vtbackend_test`. Expected: compile failure, no `Scroll`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run it and see it pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[statusline]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message `vtbackend: a {Scroll} placeholder, empty at the bottom`.

---

### Task 7: `InputMode` hides in a mode and draws as a chip

**Files:**
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp:57` (`InputMode`), the mode-name table, `showsModeChip`.
- Modify: `src/vtbackend/screen/StatusLineBuilder.cpp` — `makeItemOfType`, `visit(InputMode const&)` (`:440-443`),
  `VTSerializer::operator()` (the chip pair, R-e/R-g), `serializeStatusLineSegment`.
- Modify: `src/vtbackend/screen/Terminal.hpp/.cpp` — `indicatorStatusLineColorsFor` (public) and
  `indicatorStatusLineColors()` (`Terminal.cpp:1566-1581`).
- Modify: `docs/configuration/indicator-statusline.md` — `Hide=<mode>` and `Chip`.
- Test: `src/vtbackend/screen/StatusLineBuilder_test.cpp`.

**Interfaces:**
- Produces (namespace `StatusLineDefinitions`):
  - `enum class InputModeLook : uint8_t { Text = 0, Chip };`
  - `InputMode` gains `std::optional<ViMode> hiddenIn {};` and `InputModeLook look = InputModeLook::Text;`.
  - `struct InputModeName { ViMode mode; std::string_view name; };` and `inline constexpr auto InputModeNames` with one
    row per `ViMode`: `Normal`, `Insert`, `Visual`, `VisualLine`, `VisualBlock`, `Hint`.
  - `[[nodiscard]] std::optional<ViMode> inputModeFrom(std::string_view name) noexcept;` (ASCII case-insensitive;
    unknown → nullopt, so the item is never hidden) and `[[nodiscard]] std::string_view inputModeName(ViMode mode) noexcept;`.
- Produces (namespace `vtbackend`): `[[nodiscard]] bool showsModeChip(StatusLineDefinition const& definition) noexcept;`.
- Produces (`Terminal`, public): `[[nodiscard]] RGBColorPair indicatorStatusLineColorsFor(ViMode mode) const noexcept;`
  — today's switch (`Terminal.cpp:1571-1579`). `indicatorStatusLineColors()`: unfocused → Inactive; focused with
  `showsModeChip(_indicatorStatusLineDefinition)` → the Insert pair; otherwise `indicatorStatusLineColorsFor(mode)`.
- Template: `Hide=<mode>` and the bare flag `Chip`; written after the styles as `Chip` then `Hide=<name>`, each only
  when set. The chip pair wraps the adornments and the text (`Left= NORMAL Right=`), only with `Styling::Enabled` and
  no `foregroundColor`/`backgroundColor` on the item.

- [ ] **Step 1: Write the failing tests** (`[statusline][inputmode]`):
  - `parseStatusLineSegment.inputModeHideAndChip`: for each row of `InputModeNames`, `{InputMode:Hide=<name>}` has
    `hiddenIn == row.mode`, also lower-cased; `{InputMode:Hide=Typing}` has none; `"{InputMode:Chip,Hide=Insert}"`
    serialises to itself; `"{InputMode:Bold,Chip,Hide=Insert,Left= ,Right= }"` is a canonical fixed point.
  - `serializeToVT.inputModeHidesInItsMode`: `{InputMode:Hide=Insert}` → `""` in Insert, `"NORMAL"` after
    `inputHandler().setMode(ViMode::Normal)`.
  - `Terminal.indicatorStatusLine.aChipKeepsTheLineQuiet` (`[terminal][statusline]`): indicator on, template
    `{InputMode:Chip,Left= ,Right= } x`, Normal mode, `refreshRenderBuffer()`. Reading the render buffer's status row as
    `Screen_context_test.cpp:1003-1014` (`backgroundAt`) does, columns 0–7 have background
    `indicatorStatusLineNormalMode.background` and column 9 has `indicatorStatusLineInsertMode.background`. Visual mode
    → the chip takes the Visual pair. The same template without `Chip` in Normal mode → column 9 takes the Normal pair
    (today's whole-line behaviour).
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test`. Expected: compile failure, no `hiddenIn`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[statusline],[terminal]"`
  Expected: PASS.
- [ ] **Step 5: Tidy, format, commit**, message `vtbackend: {InputMode} may hide in a mode and draw as a chip in its mode's colours`.

---

### Task 8: Collapse by priority

**Files:**
- Create: `src/vtbackend/screen/StatusLineLayout.hpp`, `src/vtbackend/screen/StatusLineLayout_test.cpp`.
- Modify: `src/vtbackend/CMakeLists.txt` — the header and the test, as in Task 3.
- Modify: `src/vtbackend/screen/StatusLineBuilder.hpp:200-260` — `Priority` on every `ItemTraits` specialization.
- Modify: `src/vtbackend/screen/Terminal.cpp:1676-1720` — measure, collapse, place.
- Test: `StatusLineLayout_test.cpp`, `src/vtbackend/screen/StatusLineBuilder_test.cpp`.

**Interfaces:**
- Produces (namespace `vtbackend`, `StatusLineLayout.hpp`, header-only, `constexpr`/`inline`):
  - `enum class StatusLinePriority : uint8_t { Low = 0, Normal, High, Essential };` — dropped lowest first.
  - `enum class StatusLineSide : uint8_t { Left = 0, Middle, Right };`
  - `struct StatusLineItemExtent { StatusLineSide side; ColumnCount width; StatusLinePriority priority; };`
  - `enum class StatusLineItemFate : uint8_t { Shown = 0, Dropped };`
  - `struct StatusLineSegmentWidths { ColumnCount left {}; ColumnCount middle {}; ColumnCount right {}; };`
  - `struct StatusLinePlacement { std::optional<ColumnOffset> middleStart {}; std::optional<ColumnOffset> rightStart {}; };`
  - `[[nodiscard]] constexpr StatusLinePlacement placeStatusLineSegments(StatusLineSegmentWidths widths, ColumnCount columns) noexcept;`
    — exactly today's rule: middle when non-empty and `columns/2 − 1 > middle/2` and `left <= (columns − middle)/2`,
    at `(columns − middle)/2`; right when non-empty and `right < columns` and `taken + right <= columns`, at
    `columns − right`, `taken` being the left's end or the middle's.
  - `[[nodiscard]] std::vector<StatusLineItemFate> collapseStatusLine(std::span<StatusLineItemExtent const> items, ColumnCount columns);`
    — R-h. Zero-width items stay `Shown` and never count.
- `ItemTraits<T>::Priority` (`static constexpr StatusLinePriority`), one per placeholder:
  Essential `InputMode`, `Context`, `ProtectedMode`, `TraceMode`, `SearchMode`, `SearchPrompt`; High `Cwd`,
  `LastCommand`, `SelectionSize`, `Scroll`, `Progress`; Normal `Git`, `Title`, `Tabs`, `Hyperlink`,
  `HistoryLineCount`, `VTType`, `Command`, `Extension`, `Text`; Low `Clock`, `CellSGR`, `CellTextUtf8`,
  `CellTextUtf32`.
- `Terminal::updateIndicatorStatusLine`: measure each item of the three segments with `Styling::Disabled` (the
  `widthOf` lambda, `:1681-1684`, applied to a one-item segment), collapse, rebuild the three segments from the
  `Shown` items, re-measure each filtered segment (so a cluster joining across items is measured as the grid lays it
  out), place with `placeStatusLineSegments`, and draw as today.

- [ ] **Step 1: Write the failing tests.** `StatusLineLayout_test.cpp`, `[statusline][collapse]`:
  - `placeStatusLineSegments.isTodaysRule`, a table: `{0,0,3}` in 20 → right at 17; `{0,8,0}` in 20 → middle at 6;
    `{6,8,0}` in 20 → middle at 6; `{7,8,0}` in 20 → no middle; `{0,0,20}` in 20 → no right; `{12,0,8}` in 20 → right at 12.
  - `collapseStatusLine.keepsWhatFits`: everything fits → all `Shown`.
  - `collapseStatusLine.dropsByPriorityRightToLeft` (Review Focus 3): the canvas's narrow pane, items
    `[Left: Essential 8, High 15, Normal 24][Middle: High 22][Right: High 17, Low 6]` in 40 columns → the Low and the
    Normal dropped first, then High items right to left until it fits; assert the exact fates. Two equal Normals on the
    left and right → the right one goes first.
  - `collapseStatusLine.neverDropsTheLastItem`: one item wider than the line → `Shown`; Essentials go only after every
    other item.
  - `Terminal.indicatorStatusLine.anyWidthDrawsWithoutOverlap` (`StatusLineBuilder_test.cpp`, `[terminal][statusline]`):
    template left `{InputMode:Left= ,Right= }{VTType:Left= }`, middle `{Title}`, right `{Clock:Left= }` in Normal mode;
    for every width 1…40, `refreshRenderBuffer()` does not crash and the status row's text is at most that wide; at
    12 columns `NORMAL` survives and the clock does not.
- [ ] **Step 2: Run them and see them fail.** Build `vtbackend_test`. Expected: compile failure, `StatusLineLayout.hpp`
  not found.
- [ ] **Step 3: Implement** the header, the priorities, and the new layout in `updateIndicatorStatusLine`.
- [ ] **Step 4: Run them and see them pass.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test && out/appleclang-debug/src/vtbackend/vtbackend_test "[statusline],[terminal],[extensions]"`
  Expected: PASS, with `anOverflowingIndicatorShowsItsHead`, `theRightSegmentEndsInTheLastColumn` and
  `aSegmentIsMeasuredAsTheGridLaysItOut` (`Terminal_test.cpp:4290-…`) and the extension slot cases unedited.
- [ ] **Step 5: Tidy, format, commit**, message `vtbackend: a narrow status line drops items by priority, right to left`.

---

### Task 9: The chip editor keeps roles and payloads

**Files:**
- Modify: `src/contour/window/SettingsController.hpp/.cpp` — `parseIndicatorSegment`, `serializeIndicatorSegment`
  (`:2669-2808`), `indicatorColorRoles()`. **Depends on Steps 4a and 2** (locate by name).
- Modify: `src/contour/qml/StatusLineItemDialog.qml:90-160` (`edit`, `apply`, the two `ColorSlotField`s `:303-316`),
  `src/contour/qml/ColorSlotField.qml` (`roleNames`, validation `:49-56`, swatch `:59-62`),
  `src/contour/qml/StatusLinePreviewItem.qml:48`, `src/contour/qml/StatusLineIndicatorEditor.qml:300-310` (the dialog's
  `colorRoles`). **Depends on Step 4a**, which hosts the editor in the Settings window.
- Test: `src/contour/window/SettingsController_test.cpp`, `src/contour/test/QmlComponents_test.cpp` (beside `:1913-1952`).

**Interfaces:**
- Produces: `[[nodiscard]] Q_INVOKABLE QStringList SettingsController::indicatorColorRoles() const;` — the names of
  `ColorRoleNames`, in order.
- Row keys: `maxWidth` (int) for every item with a `maxWidth` member (the `Context` branch becomes this generic one);
  `hide` (QString, `inputModeName` or `""`) and `chip` (bool, QML-facing) for `InputMode`.
- QML: `StatusLineItemDialog` gains `property var original: ({})` (set in `edit`) and `property var colorRoles: []`;
  `apply()` starts from `Object.assign({}, root.original)` and overrides what the dialog edits. `ColorSlotField` gains
  `property var roleNames: []`; a trimmed text in `roleNames` is reported like a hex; the swatch fills only for a
  `#` value. `StatusLinePreviewItem` uses the item's colour only when it starts with `#`.

- [ ] **Step 1: Write the failing tests.**
  - `SettingsController_test.cpp`, `[settings][statusline]`: "the indicator bridge keeps every payload" — each of
    `{InputMode:Chip,Hide=Insert}`, `{Cwd:MaxWidth=20}`, `{LastCommand:MaxWidth=50}`,
    `{Context:Verbosity=Full,MaxWidth=10}` comes back unchanged from `serializeIndicatorSegment(parseIndicatorSegment(t))`;
    `indicatorColorRoles()` has `ColorRoleNames.size()` entries, the first `"accent"`.
  - `QmlComponents_test.cpp`, `[contour][gui][qml][statusline]`: "StatusLineItemDialog keeps an item's roles and payload
    (offscreen)" (Review Focus 4), built as the `:1913` case: edit `{ type: "InputMode", hide: "Insert", chip: true,
    maxWidth: 7, hasColor: true, color: "accent" }` with `colorRoles: ["accent", "danger"]`; the foreground
    `colorSlotField`'s text is `"accent"`; `apply` → the applied map keeps `hide`, `chip`, `maxWidth` and
    `color == "accent"`; setting the field's text to `"danger"` then `apply` → `color == "danger"`; no QML warning
    during the case (`QmlMessageCapture`).
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test && out/appleclang-debug/src/contour/contour_gui_test "[settings][statusline],[contour][gui][qml][statusline]"`
  Expected: FAIL — `Hide=Insert` is lost, `indicatorColorRoles` is unknown.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and see them pass, on both control styles.**
  Run: `out/appleclang-debug/src/contour/contour_gui_test "[settings],[contour][gui][qml]" && CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][statusline],[contour][gui][qml][settings]" && ctest --test-dir out/appleclang-debug -R "check_qml" --output-on-failure`
  Expected: PASS; the QML lint baselines do not rise.
- [ ] **Step 5: Tidy, format, commit** (`clang-format` the C++ only; `editorconfig-checker` the four `.qml`), message
  `contour: the status-line chip editor keeps role colours, Hide, Chip and MaxWidth`.

---

### Task 10: The approved template, behind the edition gate

**Files:**
- Modify: `src/contour/config/Config.hpp:659-668` — `Native2026IndicatorTemplate` and its comment (R-j). **Depends on
  Step 2** (it edits `EditionDefaults`; this task edits only the constant).
- Modify: `src/contour/config/Config_test.cpp:3988-3990` — `Native2026Expectation`'s three strings.
- Create: `src/contour/config/IndicatorTemplate_test.cpp`; add it to `contour_test`'s sources after
  `config/GuiStateStore_test.cpp` (`src/contour/CMakeLists.txt:1160`).
- Modify: `docs/configuration/indicator-statusline.md` — the example becomes the new default.
- Test: the two test files.

**Interfaces:**
- Consumes: `config::editionDefaults(DefaultsEdition::Native2026).statusLine`, `vtbackend::parseStatusLineDefinition`,
  `vtbackend::MockTerm`, `Terminal::setStatusLineDefinition`, everything Tasks 1–8 produce.
- `ClassicIndicatorTemplate` and the Classic row are untouched.

- [ ] **Step 1: Write the failing tests.** `IndicatorTemplate_test.cpp`, `[config][statusline]`; each case parses the
  Native2026 template into a 100-column `MockTerm` (a `contour-dark` palette via `defaultColorPalettes`), indicator on:
  - `IndicatorTemplate.isTheApprovedOne`: the left segment's item types are `InputMode, Context, Cwd, Git`, the
    middle's `LastCommand, SelectionSize, SearchPrompt`, the right's `Progress, Scroll, Clock`; `InputMode` has
    `look == Chip` and `hiddenIn == ViMode::Insert`; no item carries an `RGBColor` (roles only); none is `Tabs` or `Title`.
  - `IndicatorTemplate.drawsTheCanvasStates`: Insert mode, OSC 7 `/srv/app`, a finished `ctest` (134 s, exit 0), clock
    14:02 → the status row reads `" /srv/app"` at the left, `"✓ ctest · 2m 14s"` centred, `"14:02 "` at the right, and
    no `INSERT`; then `;D;8` on `ctest -R gui` → `"✗ exit 8 ctest -R gui · 12.4 s"`; Normal mode and scrolled 40 lines
    back → starts `" NORMAL "` and holds `"↑ 40 / "`.
  - `IndicatorTemplate.eachPaneDrawsItsOwnLine` (Review Focus 5): two `MockTerm`s, A in Normal mode at `/srv/a`, B in
    Insert mode at `/srv/b` with a finished `make`; `B.terminal.sendFocusOutEvent()`; both `refreshRenderBuffer()`.
    A's row holds `NORMAL` and `/srv/a`, not `/srv/b`; B's row holds `/srv/b` and `✓ make`, no `NORMAL`, and its column 0
    background is `indicatorStatusLineInactive.background`; B's cwd cells use B's palette's success colour after B's
    `palette[2]` is changed alone. `B.terminal.sendFocusInEvent()` → B's column 0 background is the Insert pair's.
  - `Config_test.cpp`: `Native2026Expectation`'s strings become R-j's three segments; the Classic expectation is unchanged.
- [ ] **Step 2: Run them and see them fail.**
  Run: `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_test contour_gui_test && out/appleclang-debug/src/contour/contour_test "[config][statusline]"`
  Expected: FAIL — the Native2026 template is still the interim one.
- [ ] **Step 3: Implement** R-j's template and the docs example.
- [ ] **Step 4: Run them and see them pass.**
  Run: `out/appleclang-debug/src/contour/contour_test "[config]" && out/appleclang-debug/src/contour/contour_gui_test "Config.firstLaunchDefaults,[config],[settings]"`
  Expected: PASS; `the indicator bridge round-trips a segment unchanged` passes on the new default too.
- [ ] **Step 5: Tidy, format, commit** (`Config.hpp`, `Config_test.cpp`, `IndicatorTemplate_test.cpp`,
  `src/contour/CMakeLists.txt`, the docs page), message
  `contour: the native2026 status line shows where you are and how the last command went`.

---

### Task 11: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src docs/configuration`, so the
  plan's own commit is left out. Apply its findings, re-run the filters of every task it touches, and commit once with
  the Signed-off-by line, staging only the files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[statusline],[git],[selector],[terminal],[extensions],[ColorPalette]"
out/appleclang-debug/src/contour/contour_test "[config]"
out/appleclang-debug/src/contour/contour_gui_test "Config.firstLaunchDefaults,[config],[settings],[contour][gui][qml]"
CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[contour][gui][qml][statusline],[contour][gui][qml][settings]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) $(git diff --name-only <the plan commit>..HEAD -- 'src/*.cpp')
```

  Expected: PASS, and no clang-tidy diagnostics.
- [ ] **Step 3: Snapshots, on this Mac's display** (windows flash; CI runs them under weston):

```bash
export CONTOUR_SNAPSHOT_DIR="$PWD/out/chrome-snapshots"; rm -rf "$CONTOUR_SNAPSHOT_DIR"
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display][snapshot]"
ls "$CONTOUR_SNAPSHOT_DIR"/*.png | wc -l
```

  Expected: the count equals `SnapshotScripts.size() × 4 rows × 2 editions × 2 appearances`, with the size read from
  `ChromeSnapshotPlan.hpp`. This step adds no script: the status line is cells, outside the chrome snapshots. Look at
  `settings.*` on every row: the editor looks as it did after Step 4a.
- [ ] **Step 4: Owner manual checks.** Batched per phase (ruling): list them in the ledger; do not wait on them. On this
  Mac, `out/appleclang-debug/src/contour/contour.app`, with a fresh config (`contour generate config`):
  - Typing at a zsh prompt in a repository: no mode chip, `~/…` in green, the branch, `✓ <cmd> · <time>` after a
    command, the clock faint at the right; `false` → `✗ exit 1 false`.
  - `git switch other`: the branch follows at the next prompt. In a `git worktree add` directory: its own branch.
    `git checkout HEAD~1`: seven hex digits.
  - Vi normal mode (the default binding): an accent `NORMAL` chip, the rest of the line unchanged; scroll back:
    `↑ n / m`; `v` and select: a magenta chip and `n lines · m chars`.
  - Split into three narrow panes: the clock, then the branch, go first; nothing overlaps; the inactive panes' lines
    show their own directories, dimmed with their pane.
  - `ssh` to another host (OSC 7 from there): the directory shows, no branch.
  - Switch between contour-dark and contour-light: every role recolours. `ui_style: terminal`: the same line.
  - Settings › the status-line chips: open `InputMode`, change nothing, Apply: `Hide=Insert` survives in `contour.yml`.
  - A Classic `contour.yml` (no `defaults_edition`): the status line is exactly as before.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release (`:107`):
  - "Adds the status-line placeholders `{Cwd}`, `{Git}` (read from `.git/HEAD`, no process spawned), `{LastCommand}`,
    `{Selection}` and `{Scroll}`."
  - "Status-line colours may name a scheme role (`Color=accent|success|danger|warning|muted`), so one template reads
    well in every scheme; hex colours still work."
  - "Adds `Hide=<mode>` and `Chip` to `{InputMode}`, and `MaxWidth=` to `{Cwd}` and `{LastCommand}`."
  - "A status line too wide for its pane drops its least important items first, right to left."
  - "The new default status line shows where you are, the branch, how the last command went and the scroll position."
- [ ] **Step 6:** Mark Step 3a done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-3a.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** each repaint measures every item once more (one `Styling::Disabled` pass per item instead of per
    segment); `{Git}` does no I/O per frame — a walk of at most one `stat` per directory level and one ≤ 4 KiB read,
    on the render thread under the terminal lock, after an OSC 133 D or OSC 7 report; `{Selection}` counts at most
    10 000 cells.
  - **Risk:** medium. `Styles`' colour type changes across vtbackend and the settings bridge; the indicator's layout
    moves into a pure function; a Git read happens under the terminal lock (a slow network filesystem stalls one frame
    per command).
  - **Coverage:** the `[statusline]`, `[git]`, `[selector]` and `[collapse]` tables, the rendered-row cases on both
    focus states, the bridge and dialog round trips on both control styles, and the edition fixture.

---

## Shared files (Phase 3)

Files this step edits that another Phase 3 step's spec section also names, or will edit for what it names. Rebase onto
whichever lands first and re-run that task's filter; each overlap is in a different function.

- `src/vtbackend/screen/Terminal.cpp` — §3.11 (3e) names `applyHintOverlay` (`:5395`); §3.12 (3f) names
  `fillGutter` (`:3937`) and `fillStickyHeader` (`:4361`). 3a edits the constructor, `updateIndicatorStatusLine`,
  `indicatorStatusLineColors`, `endCommand`, `setCurrentWorkingDirectory`.
- `src/vtbackend/screen/Terminal.hpp` — the declarations of the same (3e, 3f).
- `src/vtbackend/CMakeLists.txt` — §3.8 (3b) adds `vtbackend_test` cases; §3.12 (3f) adds
  `shell/BlockChipPlacement.hpp`.
- `src/contour/config/Config.hpp` and `src/contour/config/Config_test.cpp` — §3.11 (3e) makes
  `hyperlink_hover_tooltip: false` the default, an edition column and an edit of `Native2026Expectation`, the struct
  Task 10 edits three lines of.
- `metainfo.xml` — every step's release notes, under `0.9.0`.
- Not named by a §3.8–§3.12 section but likely touched by their QML tests: `src/contour/test/QmlComponents_test.cpp`.

## Owner decisions

The run proceeds on each recommended default.

1. **The mode chip (R-e).** Phase 0 colours the whole line by mode; the canvas draws a quiet line with a coloured chip.
   *Default: a `Chip` flag on `{InputMode}` draws the mode's pair as the chip, and a template with a chip keeps the
   line on the idle pair; the Native2026 template uses it, Classic and user templates keep whole-line colours.* The
   alternative keeps the whole line coloured by mode, which makes the role colours on it unreadable in Normal mode.
2. **The template's colours (R-j).** *Default: `{Cwd}` success (the canvas's green), `{SearchPrompt}` warning,
   `{Progress}` accent, `{Clock}` faint, `{Git}` the foreground.* The canvas draws `{Git}` magenta, which no role
   names; the alternative is a sixth role (`Color=magenta`, palette 5), a one-row table change.
3. **Collapse drops, never shortens (R-h).** *Default: whole items drop by priority; the canvas's compact forms
   (`N`, `✗ 8`, `↑1.2k`, `…/contour`) are parked for a later step.* The alternative adds a compact rendering per item.
4. **`native2026` is edited in place (R-j).** 0.9.0 is unreleased. *Default: no third edition.* A dev-build config
   generated since Phase 0 keeps the interim template text it wrote.
5. **`{LastCommand}` while a command runs (R-c).** *Default: empty until the command finishes.* The alternative shows
   `◑ <command>` in the accent while it runs.

## Spec gaps found

- §3.7's citations are stale (current lines in the *Spec* paragraph). `finishedBlocks` is a DEC 2034 reader-session
  view that is empty without a session, so `{LastCommand}` reads `lastFinished()`/`current()` (R-c);
  `CommandBlock.hpp:145` is the record's member, the accessor `CommandLineText::display()`.
- §3.7 names `currentWorkingDirectory()`, the raw OSC 7 URL; the canvas's container path needs OSC 3008 first, so
  `{Cwd}` uses `resolveWorkingDirectory(Display)` and `{Git}` `OpenLocally` (R-b).
- `InMemoryFileSystem::lastWriteTime` returns `now()`, so the "unchanged mtime, no read" branch cannot be observed in a
  test without a forwarding double, and `vthost::testing::ForwardingFileSystem` sits above `vtbackend` (R-d).
- Phase 0's per-mode whole-line pairs conflict with role colours and the canvas's chip (R-e).
- The role list has no magenta, which the canvas uses for `{Git}` and the VISUAL chip (the chip gets it from the Visual
  pair).
- On the canvas but not in §3.7, left for a later step: per-type `{Context}` colours (root red, ssh and container
  cyan); `{Progress}` as an accent bar with OSC 7501's message; `{Hyperlink}` taking the ⌘-held target (3e's pill);
  compact narrow forms; `│` separators on the Terminal row (the template is per profile, not per `ChromeStyle`);
  announcing a mode change and each exit status, and the StatusBar accessible role; the Appearance › Status Line page
  with a live preview (Step 4b).
- "Shown until the next prompt" read as until the next command starts (R-c).
