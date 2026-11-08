# Platform Chord Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every Contour application command a default chord on both Linux/Windows and macOS,
translated by platform convention, and make that parity a tested property.

**Architecture:** All changes are rows in the data tables `keyRows()` / `charRows()` of
`src/contour/config/DefaultBindings.cpp`, plus one new test file that compares the per-platform action
sets against a written exemption table. The golden files under `src/contour/config/testdata/` are
regenerated per task and their diff reviewed by hand. No new mechanism: the `⌃⌘` Mac rows are explicit
`MacOnly` rows with a new `CtrlSuper` chord constant (the spec left internal-kind vs explicit rows to the
builder; explicit rows chosen — one new constant, no new resolution path, nothing the config writer must
learn).

**Tech Stack:** C++23, Catch2, CMake presets (`appleclang-debug` on macOS, `clang-asan` on Linux).

**Spec:** `docs/superpowers/specs/2026-11-08-platform-chord-parity-design.md`

## Global Constraints

- Every new row is `both(...)` (`RowReach::WrittenAndFallback`), **except** rows needing
  `DeclinedInput::Consume`, which are `fallback(...)` (only fallbacks can consume): ⇧⌘O and Ctrl+Shift+L.
- New rows apply to both editions (no `native2026Only`).
- Rows acting at the shell prompt are `PrimaryScreenOnly`.
- No `Alt` in any macOS character row (existing test "no macOS default relies on Alt for a character key").
- No macOS row on ⌘Q, ⌘, ⌘H, ⌥⌘H, ⌘M, ⌘X, ⌘S (existing tests).
- `BindingCollisions` ("no default chord fires twice") stays green on every platform and edition.
- Zero warnings (`-Werror`), clang-format on C++ files, no `NOLINT`.
- Commit messages end with `Signed-off-by: Christian Parpart <christian@parpart.family>`; no AI trailers.

## Build and test commands (used by every task)

Worktree: `/Users/christianparpart/projects/contour-chord-parity`.

- macOS: `cmake --preset appleclang-debug` (once), then
  `cmake --build --preset appleclang-debug --target contour_test`;
  binary `out/appleclang-debug/src/contour/contour_test`.
- Linux: same with preset `clang-asan`, binary `out/clang-asan/src/contour/contour_test`.
- Regenerate goldens: `<binary> "[.golden-write]"`, then `git diff src/contour/config/testdata/` — every
  changed line must be a row this task added, moved or changed. Anything else is a bug in the task.
- Focused run: `<binary> "[bindings],[defaults],[palette]"`.

## Review Focus

1. **Alternate-screen applications** (vim, tmux, htop) must still receive Ctrl+↑/↓, Ctrl+Shift+↑/↓,
   Ctrl+Shift+L and ⇧⌘O — pinned in Tasks 4 and 5 by asserting those rows carry `PrimaryScreenOnly`.
2. **The Ctrl+Shift+↑/↓ meaning change** reaches every existing desktop user (old row was fallback-only)
   — pinned in Task 5 (desktop: select output; Mac: still ScrollMark), and called out in the release
   note (Task 7).
3. **macOS system-owned ⌃⌘ chords** (⌃⌘Q lock screen, ⌃⌘F full screen, ⌃⌘D look up, ⌃⌘Space
   character viewer) must not be taken — pinned in Task 4.
4. **Existing Mac configs** that list ⇧⌘P must still get ⌘P (as a fallback) — pinned in Task 1.
5. **Windows** must get exactly the Linux table — already pinned by "default bindings on the desktop are
   unchanged" comparing Xkb and Win32 against one golden; each task's golden regen keeps it so.

---

### Task 1: ⌘P opens the command palette on macOS

**Files:**
- Modify: `src/contour/config/DefaultBindings.cpp` (charRows, the OpenCommandPalette row ~line 308)
- Test: `src/contour/config/BindingCollisions_test.cpp`, `src/contour/command/Shortcut_test.cpp`,
  `src/contour/config/ConfigSideFiles_test.cpp:155`
- Goldens: `src/contour/config/testdata/default-bindings-mac-added.txt` (regenerated)

**Interfaces:**
- Produces (test helpers in `BindingCollisions_test.cpp`'s anonymous namespace, reused by Tasks 2–5):
  - `[[nodiscard]] std::vector<std::string_view> charActions(KeyboardPlatformKind kind, Modifiers mods, char32_t input, MatchModes modes = {})`
  - `[[nodiscard]] std::vector<std::string_view> keyActions(KeyboardPlatformKind kind, Modifiers mods, vtbackend::Key input, MatchModes modes = {})`
  
  Each returns `actions::name(row.binding.front())` for every row on that chord, across
  `makeDefaultInputMappings(Classic, kind)`, `makeDefaultInputMappings(Native2026, kind)` and the
  matching `builtinFallback*Mappings(kind)`, in that order, de-duplicated preserving order. When `modes`
  is non-default, only rows whose `modes` equal it are returned.
  - `[[nodiscard]] std::vector<std::string_view> fallbackCharActions(KeyboardPlatformKind, Modifiers, char32_t)` — fallbacks only.
  - `[[nodiscard]] FallbackCharMapping const* fallbackCharRow(KeyboardPlatformKind, Modifiers, char32_t)` and
    `[[nodiscard]] FallbackKeyMapping const* fallbackKeyRow(KeyboardPlatformKind, Modifiers, vtbackend::Key)` —
    the first matching fallback row or `nullptr`, for asserting `mapping.modes`, `mapping.binding` and `declined`.

  Note: "no default chord fires twice" checks written rows only, so the fallback-only rows (⇧⌘O,
  Ctrl+Shift+L) are collision-checked solely by these per-chord single-element assertions. They are
  load-bearing; keep them exact.

- [ ] **Step 1: Write the failing tests**

In `BindingCollisions_test.cpp`, add the helpers above and:

```cpp
TEST_CASE("macOS opens the command palette with Command+P and Shift+Command+P", "[bindings][palette]")
{
    auto const mac = KeyboardPlatformKind::Mac;
    CHECK(charActions(mac, Modifiers { Modifier::Super }, U'P') == std::vector<std::string_view> { "OpenCommandPalette" });
    CHECK(charActions(mac, Modifiers { Modifier::Super, Modifier::Shift }, U'P') == std::vector<std::string_view> { "OpenCommandPalette" });
    // A configuration written before this change lists Shift+Command+P only; Command+P reaches it as a fallback.
    CHECK(fallbackCharActions(mac, Modifiers { Modifier::Super }, U'P') == std::vector<std::string_view> { "OpenCommandPalette" });
}
```

In `Shortcut_test.cpp` change the two macOS expectations:
`index.at("OpenCommandPalette") == "Super+P"` (test "shortcutIndex names the macOS defaults by Command")
and `spelled(KeyboardPlatformKind::Mac, "OpenCommandPalette") == "⌘P"`.

In `ConfigSideFiles_test.cpp` ("the command palette is bound to Ctrl+Shift+P by default"), the first
`OpenCommandPalette` row of the build platform's defaults becomes ⌘P on a Mac build. Make it
platform-aware: on `BuildKeyboardPlatformKind == Mac` the first row's modifiers are `Super` alone and
another row has `Super|Shift`; elsewhere the first row is `primaryModifiers(host).with(Shift)` as today.
(Linux CI would not catch this; it fails only on `appleclang-debug`.)

- [ ] **Step 2: Build and run; verify the three new/changed assertions fail**

Run: `<binary> "[bindings],[palette]"` — Expected: FAIL on the Super+P checks.

- [ ] **Step 3: Add the row**

Directly **before** the existing `written(All, PrimaryShift, U'P', …)` row (display picks the first ⌘
chord, so order matters):
`both(PlatformScope::MacOnly, Primary, U'P', Any, { actions::OpenCommandPalette {} })`, with a comment:
⌘P is the palette as in Warp/VS Code-style apps; ⇧⌘P stays for VS Code/Zed/Ghostty muscle memory; this
row comes first so menus and the palette show ⌘P.
Also give each kept departure (spec, "Kept departures") a one-line comment naming what owns the plain ⌘
chord, where its row lacks one: ⇧⌘H (⌘H hides the app), ⇧⌘K (⌘K clears), ⌘D/⇧⌘D (iTerm2's splits),
⌘G/⇧⌘G/⌘E (the macOS find chords), DesktopOnly Quit (the menu bar owns ⌘Q).

- [ ] **Step 4: Regenerate goldens, review the diff, run tests**

`<binary> "[.golden-write]"`; diff shows only the ⌘P lines in `default-bindings-mac-added.txt`.
Run `<binary> "[bindings],[defaults],[palette]"` — Expected: PASS.

- [ ] **Step 5: Commit** — `config: open the command palette with Command+P on macOS`

---

### Task 2: Close chords — Primary+W closes the pane, ⇧⌘W closes the tab

**Files:**
- Modify: `src/contour/config/DefaultBindings.cpp` (charRows: ClosePane row ~307, Mac tab chords ~365)
- Test: `src/contour/config/BindingCollisions_test.cpp`, `src/contour/command/Shortcut_test.cpp`,
  `src/contour/config/ConfigBlocksAndLinks_test.cpp:2359`
- Goldens: regenerated

**Interfaces:** Consumes `charActions` (Task 1).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("every platform closes the active pane with Primary+W", "[bindings][panes]")
{
    CHECK(charActions(KeyboardPlatformKind::Xkb, Modifiers { Modifier::Control, Modifier::Shift }, U'W') == std::vector<std::string_view> { "ClosePane" });
    CHECK(charActions(KeyboardPlatformKind::Win32, Modifiers { Modifier::Control, Modifier::Shift }, U'W') == std::vector<std::string_view> { "ClosePane" });
    CHECK(charActions(KeyboardPlatformKind::Mac, Modifiers { Modifier::Super }, U'W') == std::vector<std::string_view> { "ClosePane" });
    CHECK(charActions(KeyboardPlatformKind::Mac, Modifiers { Modifier::Super, Modifier::Shift }, U'W') == std::vector<std::string_view> { "CloseTab" });
    // The desktop keeps Ctrl+Shift+X.
    CHECK(charActions(KeyboardPlatformKind::Xkb, Modifiers { Modifier::Control, Modifier::Shift }, U'X') == std::vector<std::string_view> { "ClosePane" });
}
```

In `Shortcut_test.cpp` ("the desktop spells Primary as Ctrl+Shift" section) add
`CHECK(spelled(KeyboardPlatformKind::Xkb, "ClosePane") == "Ctrl+Shift+W");` and in the macOS section
`CHECK(spelled(KeyboardPlatformKind::Mac, "ClosePane") == "⌘W");` and
`CHECK(spelled(KeyboardPlatformKind::Mac, "CloseTab") == "⇧⌘W");`.

- [ ] **Step 2: Run; verify failure** — `<binary> "[bindings],[palette]"`.

- [ ] **Step 3: Change the rows**
  - Add `both(All, Primary, U'W', Any, { actions::ClosePane {} })` **before** the
    `written(DesktopOnly, Primary, U'X', …ClosePane)` row (so Ctrl+Shift+W is the displayed chord).
  - Remove `fallback(MacOnly, Super, U'W', Any, { actions::CloseTab {} })`.
  - Add `both(PlatformScope::MacOnly, PrimaryShift, U'W', Any, { actions::CloseTab {} })` in the Mac tab
    chord block; update that block's comment (⌘W closes the focused split and the tab with its last
    pane, as iTerm2/Ghostty; ⇧⌘W the whole tab; on the desktop Primary+Shift collapses to Ctrl+Shift, so
    CloseTab has no desktop chord).
  - In `ConfigBlocksAndLinks_test.cpp` ("the tab chords follow the platform") change
    `{ super, U'W', actions::CloseTab {} }` to `{ super, U'W', actions::ClosePane {} }` and add
    `{ shiftSuper, U'W', actions::CloseTab {} }`; the desktop `== nullptr` half stays.
  - **Leave** the value `"Ctrl+Shift+X"` in `MainWindowControllerMock::shortcutText` and
    `QmlComponentsTestSupport::shortcutText` (`src/contour/test/`): QML tests assert it against the mock,
    not the table. Only fix the comments there that call it "ClosePane's default chord", and the
    `useDesktopBindings` comment in `src/contour/test/CoreFixtures.hpp:58` ("only the desktop binds …
    ClosePane"), now stale.

- [ ] **Step 4: Regenerate goldens, review diff, run** `<binary> "[bindings],[defaults],[palette]"` and
  `out/<preset>/src/contour/contour_gui_test` if it exists in the build (offscreen:
  `QT_QPA_PLATFORM=offscreen`). Expected: PASS.

- [ ] **Step 5: Commit** — `config: close the pane with Primary+W everywhere, the tab with Shift+Command+W`

---

### Task 3: Desktop copy and font-size chords

**Files:** `src/contour/config/DefaultBindings.cpp` (charRows), `BindingCollisions_test.cpp`, goldens.

**Interfaces:** Consumes `charActions` (Task 1).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("the desktop copies with Ctrl+Shift+C and enlarges the font with Ctrl+Shift+=", "[bindings]")
{
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 })
    {
        auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };
        CHECK(charActions(kind, ctrlShift, U'C') == std::vector<std::string_view> { "CopySelection" });
        CHECK(charActions(kind, ctrlShift, U'=') == std::vector<std::string_view> { "IncreaseFontSize" });
        CHECK(charActions(kind, ctrlShift, U'+') == std::vector<std::string_view> { "IncreaseFontSize" });
    }
}
```

Also assert the copy row's full binding is `CopySelection` then `CancelSelection` (two actions), mirroring
"copying with Ctrl+C still clears the selection".

- [ ] **Step 2: Run; verify failure.**

- [ ] **Step 3: Add rows** (all `DesktopOnly`, `Primary`, `Any`, `both`):
  `U'C'` → `{ CopySelection {}, CancelSelection {} }`; `U'='` and `U'+'` → `IncreaseFontSize {}`. Place
  the font rows next to the existing Ctrl+Shift+- DecreaseFontSize rows. Comment: kitty / GNOME Terminal
  chords; copy does not decline on an empty selection, so a written row needs no consume.

- [ ] **Step 4: Regenerate goldens, review diff, run focused tests.** Expected: PASS.

- [ ] **Step 5: Commit** — `config: Ctrl+Shift+C copies and Ctrl+Shift+= enlarges the font on Linux and Windows`

---

### Task 4: The macOS second tier (⌃⌘) and ⇧⌘O

**Files:** `src/contour/config/DefaultBindings.cpp` (chord constants ~line 58, charRows),
`BindingCollisions_test.cpp`, `DefaultBindings_test.cpp`, goldens.

**Interfaces:**
- Produces: `constexpr auto CtrlSuper = ChordSpec { .extra = Modifiers { Modifier::Control, Modifier::Super } };`
  in DefaultBindings.cpp's anonymous namespace, documented as "the macOS counterpart of a desktop
  Ctrl+Alt row: Option composes characters, ⌃⌘ is the Mac's own second tier".
- Consumes `charActions` (Task 1).

- [ ] **Step 1: Write the failing tests** (BindingCollisions_test.cpp)

```cpp
TEST_CASE("macOS answers the desktop's Ctrl+Alt chords with Control+Command", "[bindings]")
{
    auto const mac = KeyboardPlatformKind::Mac;
    auto const ctrlCmd = Modifiers { Modifier::Control, Modifier::Super };
    CHECK(charActions(mac, ctrlCmd, U'.') == std::vector<std::string_view> { "ToggleStatusLine" });
    CHECK(charActions(mac, ctrlCmd, U'O') == std::vector<std::string_view> { "OpenFileManager" });
    CHECK(charActions(mac, ctrlCmd, U'S') == std::vector<std::string_view> { "ScreenshotVT" });
    CHECK(charActions(mac, ctrlCmd, U',') == std::vector<std::string_view> { "ToggleInputMethodHandling" });
    CHECK(charActions(mac, ctrlCmd, U'V') == std::vector<std::string_view> { "PasteClipboard" });
}

TEST_CASE("macOS opens the last command's output with Shift+Command+O at the prompt only", "[bindings]")
{
    auto const mac = KeyboardPlatformKind::Mac;
    auto const chord = Modifiers { Modifier::Super, Modifier::Shift };
    CHECK(fallbackCharActions(mac, chord, U'O') == std::vector<std::string_view> { "OpenCommandOutput" });
    // On the primary screen only, consumed when it declines -- as the desktop's Ctrl+Shift+G.
    auto const* row = fallbackCharRow(mac, chord, U'O');
    REQUIRE(row != nullptr);
    CHECK(row->mapping.modes == MatchModes { 0, MatchModes::AlternateScreen });
    CHECK(row->declined == DeclinedInput::Consume);
}
```

The PasteClipboard row must have `.strip = true` — assert via `fallbackCharRow(mac, ctrlCmd, U'V')` and
`std::get<actions::PasteClipboard>(row->mapping.binding.front()).strip`.

In `DefaultBindings_test.cpp` add:

```cpp
TEST_CASE("no macOS default takes a Control+Command chord the system owns", "[config][defaults]")
// For ⌃⌘Q (lock screen), ⌃⌘F (full screen), ⌃⌘D (look up), and Key/char Space with Control|Super:
// none appears in either edition's Mac charMappings nor in builtinFallbackCharMappings(Mac).
```
(same shape as "no macOS default takes a chord the menu bar owns").

- [ ] **Step 2: Run; verify failure.**

- [ ] **Step 3: Add rows**, each `both(PlatformScope::MacOnly, CtrlSuper, …, Any, …)` beside its desktop
  `CtrlAlt` row: `U'.'` ToggleStatusLine, `U'O'` OpenFileManager, `U'S'` ScreenshotVT, `U','`
  ToggleInputMethodHandling, `U'V'` `PasteClipboard { .strip = true }`. Then
  `fallback(PlatformScope::MacOnly, PrimaryShift, U'O', PrimaryScreenOnly, { actions::OpenCommandOutput {} }, DeclinedInput::Consume)`
  next to the desktop Ctrl+Shift+G row, with a comment (⌘G is find-next on macOS).

- [ ] **Step 4: Regenerate goldens, review diff, run focused tests.** Expected: PASS.

- [ ] **Step 5: Commit** — `config: the macOS Control+Command tier and Shift+Command+O`

---

### Task 5: Command navigation and clear-to-prompt on Linux and Windows

**Files:** `src/contour/config/DefaultBindings.cpp` (keyRows ~204, charRows), `BindingCollisions_test.cpp`,
goldens.

**Interfaces:** Consumes `keyActions`, `charActions`, `fallbackCharActions` (Task 1).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("the desktop steps between commands as VS Code does", "[bindings]")
{
    using vtbackend::Key;
    auto const prompt = MatchModes { 0, MatchModes::AlternateScreen };
    for (auto const kind: { KeyboardPlatformKind::Xkb, KeyboardPlatformKind::Win32 })
    {
        CHECK(keyActions(kind, Modifiers { Modifier::Control }, Key::UpArrow, prompt) == std::vector<std::string_view> { "FocusPreviousCommand" });
        CHECK(keyActions(kind, Modifiers { Modifier::Control }, Key::DownArrow, prompt) == std::vector<std::string_view> { "FocusNextCommand" });
        CHECK(keyActions(kind, Modifiers { Modifier::Control, Modifier::Shift }, Key::UpArrow) == std::vector<std::string_view> { "SelectPreviousCommandOutput" });
        CHECK(keyActions(kind, Modifiers { Modifier::Control, Modifier::Shift }, Key::DownArrow) == std::vector<std::string_view> { "SelectNextCommandOutput" });
        // ...and in no other mode: an alternate-screen application keeps the chords.
        CHECK(keyActions(kind, Modifiers { Modifier::Control }, Key::UpArrow).size() == 1);
        CHECK(fallbackCharActions(kind, Modifiers { Modifier::Control, Modifier::Shift }, U'L') == std::vector<std::string_view> { "ClearToPrompt" });
    }
    // macOS keeps Control+Shift+Up/Down as the prompt jump.
    CHECK(keyActions(KeyboardPlatformKind::Mac, Modifiers { Modifier::Control, Modifier::Shift }, Key::UpArrow) == std::vector<std::string_view> { "ScrollMarkUp" });
}
```

Additionally assert every row returned for Ctrl+↑/↓, Ctrl+Shift+↑/↓ and Ctrl+Shift+L on the desktop has
modes `PrimaryScreenOnly` (via `keyActions(..., prompt)` returning the same set as the unfiltered call),
and `fallbackCharRow(kind, Ctrl|Shift, U'L')->declined == DeclinedInput::Consume`.

- [ ] **Step 2: Run; verify failure.**

- [ ] **Step 3: Change the rows**
  - The two `fallback(All, CtrlShift, Key::UpArrow/DownArrow, PrimaryScreenOnly, ScrollMarkUp/Down)`
    rows become `PlatformScope::MacOnly`; update their comment.
  - Add `both(DesktopOnly, Ctrl, Key::UpArrow/DownArrow, PrimaryScreenOnly, FocusPrevious/NextCommand)`
    and `both(DesktopOnly, CtrlShift, Key::UpArrow/DownArrow, PrimaryScreenOnly, SelectPrevious/NextCommandOutput)`
    next to the Mac ⌘↑/⇧⌘↑ rows, with a comment naming VS Code's integrated terminal and the meaning
    change for Ctrl+Shift+↑/↓ (prompt jump stays on Ctrl+Alt+K/J).
  - Add `fallback(DesktopOnly, Primary, U'L', PrimaryScreenOnly, { actions::ClearToPrompt {} }, DeclinedInput::Consume)`
    next to the Mac ⌘K row (comment: the shell's Ctrl+L plus Shift; Ctrl+Shift+K is NewWindowTo).

- [ ] **Step 4: Regenerate goldens, review diff, run focused tests.** Expected: PASS.

- [ ] **Step 5: Commit** — `config: VS Code's command navigation and Ctrl+Shift+L on Linux and Windows`

---

### Task 6: The parity test

**Files:**
- Create: `src/contour/config/ChordParity_test.cpp`
- Modify: `src/contour/CMakeLists.txt` (add it to `contour_test` sources next to `config/DefaultBindings_test.cpp`, ~line 1496)

**Interfaces:** Consumes the public `makeDefaultInputMappings`, `builtinFallbackKeyMappings`,
`builtinFallbackCharMappings` (`DefaultBindings.hpp`/`Config.hpp`) and `actions::name`.

- [ ] **Step 1: Write the test**

- `[[nodiscard]] std::set<std::string_view> keyboardActions(KeyboardPlatformKind kind)`: the names of the
  first action of every key and char row (not mouse) in both editions' written tables and the fallbacks.
- The exemption table, verbatim:

```cpp
struct Exemption { std::string_view action; KeyboardPlatformKind boundOn; std::string_view reason; };
constexpr auto Exemptions = std::array {
    Exemption { "Quit", KeyboardPlatformKind::Xkb, "the macOS menu bar owns Command+Q" },
    Exemption { "SpeakSelection", KeyboardPlatformKind::Xkb, "macOS has a system-wide Speak Selection (Option+Esc)" },
    Exemption { "CloseTab", KeyboardPlatformKind::Mac, "on the desktop Primary+Shift+W collapses to ClosePane's Ctrl+Shift+W" },
    Exemption { "FocusToolbar", KeyboardPlatformKind::Mac, "AppKit's Control+F5; the desktop has no toolbar chord" },
    Exemption { "SearchSelection", KeyboardPlatformKind::Mac, "a macOS find-pasteboard convention; Ctrl+Shift+E is SplitVertical" },
    Exemption { "SendChars", KeyboardPlatformKind::Mac, "Mac word/line motions; desktop shells handle Ctrl+Left/Right, Home, End" },
};
```

- `TEST_CASE("every keyboard command is bound on both Linux/Windows and macOS, or exempt with a reason", "[bindings][parity]")`:
  compute `desktopOnly = keyboardActions(Xkb) \ keyboardActions(Mac)` and `macOnly` the reverse; build the
  expected sets from `Exemptions` by `boundOn`; `CHECK(desktopOnly == expectedDesktop)` and
  `CHECK(macOnly == expectedMac)` with `INFO` listing each difference and, for exempt ones, the reason.
  A stale exemption fails the same check.
- Also `CHECK(keyboardActions(Xkb) == keyboardActions(Win32))`.
- Deviation from the spec's "for each defaults edition", stated in a comment in the test: the sets are the
  union over both editions, because the only edition-scoped keyboard rows (the Native2026 chrome-focus
  chords) would otherwise make `FocusToolbar`'s exemption stale in Classic. No parity gap hides behind
  the union today.

- [ ] **Step 2: Build and run** `<binary> "[parity]"` — Expected: PASS. If it fails, the difference
  is either a row Tasks 1–5 missed (fix the row) or a pre-existing gap the spec did not list: **do not add
  an exemption on your own** — report it back with the action name and the platform.

- [ ] **Step 3: Run the whole suite** `ctest --preset <preset>` (or `--test-dir out/appleclang-debug`)
  — Expected: all pass.

- [ ] **Step 4: Commit** — `config: test that every keyboard command has a chord on both platforms`

---

### Task 7: Documentation and release note

**Files:**
- Modify: `docs/configuration/key-mapping.md`
- Modify: `metainfo.xml` (the topmost development `<release>` entry)

- [ ] **Step 1: Update `key-mapping.md`**
  - Add a section "Default chords" with one table: Command | Linux / Windows | macOS | Note — one row per
    command in the parity test's action set that has a default keyboard chord, the chords taken from the
    golden files. Departures from the Primary translation carry their reason (⌘H hide, ⌘K clear, ⌘D
    iTerm2 splits, ⌘G find, ⌘Q menu bar, ⌃⌘ tier because Option composes).
  - Fix the "macOS defaults" section (lines ~49–70): ClosePane is now ⌘W; the stripping paste,
    ScreenshotVT, OpenFileManager, ToggleStatusLine, ToggleInputMethodHandling have ⌃⌘ chords;
    OpenCommandOutput is ⇧⌘O; ScrollMarkUp/Down are ⌃⇧↑/↓ on macOS (the current text wrongly lists
    them as unbound); only SpeakSelection and the tenth tab remain without a macOS chord.
  - Fix the built-in-bindings table row for Ctrl+Shift+G to add `⇧⌘O` on macOS; add Ctrl+Shift+L / ⌘K.
  - The opening warning's `Ctrl+Shift+C` mention is now true; leave it.

- [ ] **Step 2: Add release-note `<li>` entries** to `metainfo.xml` (use the
  `contour-workflows:add-release-note` skill): one for macOS (⌘P palette, ⌘W closes the pane / ⇧⌘W the
  tab, ⌃⌘ chords, ⇧⌘O), one for Linux/Windows (Ctrl+Shift+C copy, Ctrl+Shift+W close, Ctrl+Shift+= font,
  Ctrl+↑/↓ command navigation, Ctrl+Shift+L clear to prompt), and one stating the behaviour change:
  "Ctrl+Shift+Up/Down now select the previous/next command's output on Linux and Windows; jumping
  between prompts stays on Ctrl+Alt+K/J."

- [ ] **Step 3: Verify** `mkdocs`-style links are not broken by checking any anchors you referenced exist
  (`grep -n` the target headings); `xmllint --noout metainfo.xml` passes.

- [ ] **Step 4: Commit** — `docs: the default chords on Linux, Windows and macOS side by side`

---

## Finish

- Run `/simplify`, then `/code-review` (per project workflow), address findings.
- Change summary reports: performance impact (none — table rows only), risk (the Ctrl+Shift+↑/↓ meaning
  change; Ctrl+↑ at the prompt for shells that bind it), and coverage (new tests per row).
