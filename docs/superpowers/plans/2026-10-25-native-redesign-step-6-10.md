# Native Redesign — Step 6.10 (IME: the Candidate Window at the Cursor Cell, an Inline Preedit With a 1-pt Underline) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

Written ahead (pipelining ruling, 2026-10-25). Line numbers are at HEAD `661b6029`. Every earlier step runs first:
4a is done, 1c is in progress, and 1d, 2, 3a–3f, 4b, 5a and 5b are written. This step touches none of their code.
A pre-flight scan re-verifies every reference before Task 1.

**Goal:** An input method's candidate window always opens at the cursor cell — at the end of a line, after a split or
font change, while scrolled back — never at the pane's origin; and the text being composed is drawn inline at the
cursor in the cell's own colours with a 1-pt underline, instead of today's bold, underlined white on red.

**Architecture:**
- **The anchor is a pure decision over the existing seam.** `display/ImeQueryRect.hpp` (today's `imeCursorAddressable`
  and `imeCursorRectangle`) gains `imeAnchorCell()`, which clamps the wrap-pending column onto the last cell and says
  when there is no cell to anchor at; `TerminalDisplay::inputMethodQuery(Qt::ImCursorRectangle)` answers the last good
  rectangle instead of an empty one, and re-announces the rectangle when the item moves, the cell geometry changes or
  the preedit changes.
- **The preedit's look is a setting with an edition column.** `vtbackend::PreeditStyle { Highlighted = 0, Inline }`
  in `vtbackend::Settings`, from a new `input_method_editor_style` key (Classic `highlighted`, Native2026 `inline`).
  `RenderBufferBuilder` draws `Inline` in the palette's default colours, flags its cells with a render-only
  `CellFlag::InputMethodPreedit`, and clips the preedit at the page's right edge.
- **The rasterizer draws 1 pt, not the font's underline.** `vtrasterizer::Decorator::PreeditUnderline`, mapped from
  that flag, strokes `GridMetrics::preeditUnderlineThickness` pixels: one point at the font's DPI
  (`max(1, round(dpi / 72))`), at the font's underline position.

**Tech Stack:** C++23, Qt 6.11 (Quick, input methods), Catch2 v3, CMake build tree `out/appleclang-debug`.

**Spec:** `docs/superpowers/specs/2026-10-24-native-redesign-implementation-design.md` §3.16 row 6.10 binds, with what
it cites: §1 (principles; 3 pure decisions; 7 AGENT.md), §2.6 (testing), §2.7 (`DefaultsEdition`). The design record
is `docs/superpowers/specs/2026-10-23-native-redesign-design.md`. The canvas is
<https://claude.ai/artifact/12ASSTRD5ua5SwarsMJXwm>; its local copies `moments` (9 · Passwords and input methods) and
`tui` are in `.superpowers/design/canvas/project/`; `surfaces` lists "IME and accessibility → Moments 9–10". The plans
index is `docs/superpowers/plans/2026-10-24-native-redesign-plans-index.md`.

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

- **The spec, verbatim (§3.16, row 6.10):** "IME | Candidate window anchored at the cursor cell; inline preedit with a
  1-pt underline | existing `ImeQueryRect`".
- **Canvas values, exact** (`moments` 9): "IME preedit is inline at the cursor with a 1 pt underline; the candidate
  window anchors to the cursor cell, never the pane origin." The board draws the preedit `にほんご` in the text's own
  colour (`#D9DCE3`, contour-dark's foreground) on the background, underlined in that same colour; the candidate list
  hangs below the preedit's first cell.
  - `PreeditStyle::Inline`: foreground and underline the palette's default foreground, background the palette's
    default background, no bold; `Highlighted`: today's `inputMethodEditor` pair with Bold and Underline
    (`RenderBufferBuilder.cpp:998-1001`), unchanged.
  - 1 pt = `dpi / 72` device pixels, at least 1.
- **Binding rulings** (ledgers 0, 1a, 1b, 1e, 4a; the SDD ledgers of 1c, 1d, 2, 3a–3f, 4b, 5b):
  - Any new default goes through `DefaultsEdition`: `input_method_editor_style` is an `EditionDefaults` column.
  - The anchor uses the existing `ImeQueryRect` seam, GUI-thread only, under the state lock, one locked
    `GridMetrics` copy per query (`TerminalDisplay.cpp:1464-1533`); a test's `[threading]` case stays green.
  - `enum class` over `bool`, zero the default; `std::expected` where a call can fail.
  - Every visual surface is a snapshot row and every visual task renders the Terminal row. The preedit is grid content
    and the candidate window is the OS's, so neither is a chrome snapshot (the snapshot scene holds no terminal,
    `ChromeSnapshot`, ruling S2); the grid draws identically under every `ChromeStyle`, the Terminal row included, and
    the render-buffer and decoration tests are its acceptance.
  - No OS service is added (the platform input methods are reached through Qt's existing `inputMethodQuery`), so no
    platform seam and no Windows- or Linux-only code; CI's Windows and Linux jobs run the same tests.
  - Owner manual checks are batched per phase: listed in the ledger, never awaited.
- **AGENT.md applies in full:** data-driven (the `PreeditStyle` spellings, the edition column, the flag-to-decorator
  table in `DecorationRenderer.cpp:37-45`), configuration at construction (the style reaches `vtbackend::Settings` with
  the profile, as `inputMethodEditor` support does), Doxygen on every new public declaration, `.hpp` with `#pragma once`,
  zero warnings, no `NOLINT`; `slots`, `signals` and `emit` are never identifiers.
- **Build** with every core, configured `-DCONTOUR_EXTENSIONS_BUNDLED=OFF`:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target <target>`. Never two builds in one tree at once.
- **Tests:** each task runs only its own filter; the full suite runs in CI on Linux, macOS and Windows.
- **Lint:** before each commit run
  `/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) <the task's .cpp files>`
  and fix every finding; format with `/opt/homebrew/opt/llvm@22/bin/clang-format -i`.
- **CI's clang 22 enforces `-Wmissing-designated-field-initializers`:** every new struct member gets `{}` or a value;
  every table row names every field (`EditionDefaultsTable`, the decorator table).
- **Header-only functions are `inline` or `constexpr`.** `std::views::iota` takes two bounds of one type.
- **Qt-free stays Qt-free:** `vtbackend` and `config/PreeditStyle.hpp` include no Qt.
- **Never edit `vendor/core-cpp`.**
- **Commits:** one per task. Stage only that task's files, by name. Each message ends with exactly
  `Signed-off-by: Christian Parpart <christian@parpart.family>`, never `Co-Authored-By`.
- After the code changes, run `/simplify` (Task 4).

## Review Focus

1. **Composing at the end of a line.** After typing to the last column the cursor is wrap-pending (column == page
   width), which `imeCursorAddressable` rejects, so today's query answers an empty rectangle and the candidate window
   jumps to the pane's origin. Expected: it anchors at the last cell. Pinned in Task 1 (`imeAnchorCell.clampsTheWrapPendingColumn`
   and the display case).
2. **The pane moves under an open candidate window.** A split is dragged, a sibling closes, the font is zoomed, or the
   window moves to a screen with another scale while composing. Expected: the rectangle is re-announced and the
   candidate window follows the cell. Pinned in Task 1 ("geometry and font changes re-announce the rectangle").
3. **A query the grid cannot answer.** A resize between capture and query, the cursor out of the viewport (scrolled
   back before composing starts), or the alternate screen's own scroll guard. Expected: the last good rectangle, never
   an empty one; composing scrolls to the bottom first (`Terminal::updateInputMethodPreeditString` already calls
   `scrollToBottomOnInput`, `Terminal.cpp:1158-1160`), and the next answer is the new cell. Pinned in Task 1.
4. **A long preedit near the right edge.** `にほんごにほんご` composed four columns before the edge. Expected: the preedit
   is clipped at the page's right edge (a wide cluster that would straddle it is not drawn), no cell is emitted past the
   last column, and the cursor stays inside the page. Pinned in Task 2.
5. **Existing users' preedit.** A Classic config keeps the bold, underlined `input_method_editor` colours; a scheme that
   sets `input_method_editor` keeps them under `highlighted`; `inline` uses the scheme's own default colours and the
   1-pt underline at every DPI (1× and 2×). Pinned in Task 2 (the edition fixtures) and Task 3 (thickness at 72, 144 and
   96 DPI).

Also pinned, beyond the five: the preedit's underline is not the font's (a thick-underline font keeps a 1-pt preedit
line, Task 3); the `[threading]` IME case stays green (Task 1).

---

## Spec resolutions (read before Task 1)

Those marked **(owner)** are repeated under *Owner decisions*; the run proceeds on their default.

- **R-a. Which cell is "the cursor cell".** The grid cursor's cell, which is the preedit's first cell (the preedit is
  drawn from the cursor rightwards). Platform IMEs place the candidate list below that rectangle; the board draws it
  there.
- **R-b. The wrap-pending column** (column == page width) anchors at column `width − 1` of the same line. Any other
  position outside the page has no anchor, and the query answers the last good rectangle (R-c).
- **R-c. The last good rectangle.** `TerminalDisplay` keeps the last rectangle it answered for `Qt::ImCursorRectangle`
  (GUI thread only) and answers it whenever the grid cannot; before any answer, the rectangle of cell (0, 0), never an
  empty one.
- **R-d. Edition (owner, 1).** `input_method_editor_style: inline | highlighted`, `highlighted` in Classic and `inline` in
  Native2026 (§2.7).
- **R-e. Clipping (owner, 2).** A preedit wider than the columns left is cut at the right edge rather than wrapped onto
  the next row, which would draw over cells the program owns (§1 principle 5's spirit; terminals differ here).
- **R-f. A render-only flag.** `CellFlag::InputMethodPreedit` is never set by a control sequence and never stored in the
  grid: only `RenderBufferBuilder` puts it on the preedit's render cells, as the table's own comment documents.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `src/contour/display/ImeQueryRect.hpp`, `TerminalDisplay.{hpp,cpp}` (modify) | The anchor and its updates | 1 |
| `src/vtbackend/screen/Settings.hpp`, `RenderBufferBuilder.cpp`, `core/CellFlags.hpp` (modify); `src/contour/config/PreeditStyle.hpp` (create); `config/Config.*`, `ConfigDocumentation.hpp`, `session/TerminalSession.cpp` (modify) | The style, its key, the inline cells and clipping | 2 |
| `src/vtrasterizer/Decorator.hpp`, `DecorationRenderer.cpp`, `GridMetrics.hpp`, `Renderer.cpp` (modify) | The 1-pt line | 3 |

---

### Task 1: The candidate window's anchor

**Files:**
- Modify: `src/contour/display/ImeQueryRect.hpp:15-63` — `imeAnchorCell`.
- Modify: `src/contour/display/TerminalDisplay.{hpp,cpp}` — `_lastImeRectangle`; the `Qt::ImCursorRectangle` case
  (`:1489-1505`); `updateInputMethod(Qt::ImCursorRectangle)` from `geometryChange` (`:477-488`, any change),
  `notifyCellGeometryChanged` (`:1604-1615`) and `inputMethodEvent` (`:1447-1461`, after the preedit is stored).
- Test: `src/contour/display/ImeQueryRect_test.cpp` and new `src/contour/display/TerminalDisplayIme_test.cpp` (both
  contour_gui_test, `[contour][ime]`; the new file drives a `TestApp` session's display offscreen).

**Interfaces:**
- Produces (`contour::display`, `ImeQueryRect.hpp`):
  `[[nodiscard]] constexpr std::optional<vtbackend::CellLocation> imeAnchorCell(vtbackend::CellLocation cursor, vtbackend::PageSize pageSize) noexcept;`
  — the cursor when `imeCursorAddressable`; `{line, width − 1}` when `column == width` on a line inside the page (R-b);
  otherwise `std::nullopt`.
- Produces (`TerminalDisplay`): member `QRectF _lastImeRectangle` (GUI thread; documented R-c), set on every non-empty
  answer, answered whenever `imeAnchorCell` gives none, the cursor is out of the viewport, or the query runs off the GUI
  thread (which already answers nothing, unchanged).

- [ ] **Step 1: Write the failing tests.**
  - `ImeQueryRect_test.cpp`, `[contour][ime]`:
    - "imeAnchorCell.interiorCellsAnchorAtThemselves": every cell of a 3 × 4 page.
    - "imeAnchorCell.clampsTheWrapPendingColumn" (Review Focus 1): `{line 2, column 4}` on 4 columns → `{2, 3}`.
    - "imeAnchorCell.outsideThePageHasNoAnchor": line 3 of 3, column 5 of 4, negative values → `nullopt`.
  - `TerminalDisplayIme_test.cpp`, `[contour][ime]`, offscreen: a session at
    80 × 24 printing 80 characters (wrap-pending) → `inputMethodQuery(Qt::ImCursorRectangle)` is the rectangle of
    `{line, 79}`, not empty; after a resize that leaves the cursor outside the page → the previous rectangle, not empty
    (Review Focus 3); "geometry and font changes re-announce the rectangle" (Review Focus 2): a `QInputMethod` spy (the
    item's `updateInputMethod` reaches `QGuiApplication::inputMethod()->update`) counts one update after a
    position-only `setX`, one after a font-size change, and one after a preedit event.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target contour_gui_test`. Expected: compile error, no
  `imeAnchorCell`.
- [ ] **Step 3: Implement** `imeAnchorCell`, the last rectangle, and the three update calls.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/contour/contour_gui_test "[contour][ime]" && out/appleclang-debug/src/vtbackend/vtbackend_test "Terminal.IME*"`.
  Expected: PASS, the concurrent-IME `[threading]` case included.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/contour/display/TerminalDisplay.cpp src/contour/display/ImeQueryRect_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/contour/display/ImeQueryRect.hpp src/contour/display/ImeQueryRect_test.cpp src/contour/display/TerminalDisplayIme_test.cpp src/contour/display/TerminalDisplay.hpp src/contour/display/TerminalDisplay.cpp
git add src/contour/display/ImeQueryRect.hpp src/contour/display/ImeQueryRect_test.cpp src/contour/display/TerminalDisplayIme_test.cpp src/contour/display/TerminalDisplay.hpp src/contour/display/TerminalDisplay.cpp src/contour/CMakeLists.txt
git commit -m "contour: the input method's candidate window stays at the cursor cell, end of line included" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 2: The inline preedit: its style, its key, and clipping at the edge

**Files:**
- Modify: `src/vtbackend/core/CellFlags.hpp:22-52` — `_(InputMethodPreedit, 21)`, with R-f's comment.
- Modify: `src/vtbackend/screen/Settings.hpp:114` — `PreeditStyle preeditStyle = PreeditStyle::Highlighted;` and the enum.
- Modify: `src/vtbackend/screen/RenderBufferBuilder.{hpp,cpp}` — `tryRenderInputMethodEditor` (`:993-1020`) reads the
  style; `renderUtf8Text` (`:935-985`) gains a column limit for the preedit.
- Create: `src/contour/config/PreeditStyle.hpp` — the spellings (`ConfigEnumInfo`), as `config/ChromeDensity.hpp`
  (Step 2) is.
- Modify: `src/contour/config/Config.hpp` (`EditionDefaults` `:1035` gains `vtbackend::PreeditStyle preeditStyle;`, both
  rows name it; `ConfigEntry<vtbackend::PreeditStyle, documentation::InputMethodEditorStyle> inputMethodEditorStyle { … }`
  beside `inputMethodEditor`, `:1224`), `Config.cpp` (load beside `input_method_editor`, `:2125`; write back),
  `ConfigDocumentation.hpp` (beside `:833`), `src/contour/session/TerminalSession.cpp` (copy into the terminal's
  settings where `inputMethodEditor` support is applied).
- Test: `src/vtbackend/screen/RenderBufferBuilder_test.cpp`, `src/vtbackend/screen/Terminal_input_test.cpp`
  (vtbackend_test); `src/contour/config/Config_test.cpp` (contour_gui_test).

**Interfaces:**
- Produces (`vtbackend`, `Settings.hpp`): `enum class PreeditStyle : uint8_t { Highlighted = 0, Inline };`
- Produces (`vtbackend::CellFlag`): `InputMethodPreedit` (bit 21), render-only (R-f).
- Produces (`contour::config`): spellings `"highlighted"` / `"inline"`, labels "Highlighted" / "Inline, underlined";
  documentation "How text an input method is composing is drawn at the cursor: `inline` in the cell's own colours with
  a thin underline, or `highlighted` in the colour scheme's `input_method_editor` colours."
- Behaviour (`RenderBufferBuilder`): under `Inline`, the preedit's cells take the palette's default foreground and
  background, the flag `InputMethodPreedit` (and neither Bold nor Underline), and the default foreground as their
  underline colour; under `Highlighted`, exactly today's cells. Under both, `renderUtf8Text` stops before a cluster that
  would end past the page's last column, and the cursor column advance is the columns actually drawn (R-e).

- [ ] **Step 1: Write the failing tests.**
  - `RenderBufferBuilder_test.cpp`, `[renderbuffer][ime]`, a `MockTerm` at 10 × 3:
    - "an inline preedit takes the cell's colours and the preedit flag": `PreeditStyle::Inline`, preedit `"ab"` at
      `{0, 2}` → two cells with the default foreground and background, `InputMethodPreedit` set, `Bold` and `Underline`
      clear, underline colour the default foreground.
    - "a highlighted preedit is unchanged": `Highlighted` → the `inputMethodEditor` pair with Bold and Underline, no
      `InputMethodPreedit`.
    - "a long preedit is clipped at the right edge" (Review Focus 4): cursor at `{0, 6}`, preedit `"にほんご"` (8
      columns) → cells only up to column 9, no cell at column ≥ 10, the last drawn cluster ends at column 9, and the
      render cursor's column is 9 or less; preedit `"aにb"` at `{0, 8}` → `"a"` drawn, `"に"` (would straddle) not.
  - `Terminal_input_test.cpp`, `[terminal]`: the existing `Terminal.IME.CursorVisibleDuringComposition` passes
    unedited under both styles.
  - `Config_test.cpp`, `[config][ime]` (Review Focus 5): a Classic file → `Highlighted`; the generated config →
    `Inline`; `input_method_editor_style: highlighted` in a Native2026 file → `Highlighted`, written back as such; an
    unknown spelling warns and keeps the edition's value.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtbackend_test contour_gui_test`. Expected:
  compile error, no `vtbackend::PreeditStyle`.
- [ ] **Step 3: Implement** the flag, the setting, the builder's two styles and the clip, the config header, the
  column, the entry, the loader, the writer, the documentation and the session's copy.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/vtbackend/vtbackend_test "[renderbuffer],[terminal]" && out/appleclang-debug/src/contour/contour_gui_test "[config][ime],[config]"`.
  Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtbackend/screen/RenderBufferBuilder.cpp src/vtbackend/screen/RenderBufferBuilder_test.cpp src/contour/config/Config.cpp src/contour/session/TerminalSession.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtbackend/core/CellFlags.hpp src/vtbackend/screen/Settings.hpp src/vtbackend/screen/RenderBufferBuilder.hpp src/vtbackend/screen/RenderBufferBuilder.cpp src/vtbackend/screen/RenderBufferBuilder_test.cpp src/contour/config/PreeditStyle.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession.cpp
git add src/vtbackend/core/CellFlags.hpp src/vtbackend/screen/Settings.hpp src/vtbackend/screen/RenderBufferBuilder.hpp src/vtbackend/screen/RenderBufferBuilder.cpp src/vtbackend/screen/RenderBufferBuilder_test.cpp src/contour/config/PreeditStyle.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp src/contour/config/CMakeLists.txt src/contour/session/TerminalSession.cpp
git commit -m "vtbackend: an inline preedit in the cell's own colours, cut at the right edge; input_method_editor_style" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 3: The 1-pt preedit underline

**Files:**
- Modify: `src/vtrasterizer/Decorator.hpp:18-41` — `PreeditUnderline`, appended.
- Modify: `src/vtrasterizer/GridMetrics.hpp:46-50` — `int preeditUnderlineThickness = 1;` beside `underline`, and its
  formatter line; the free function below.
- Modify: `src/vtrasterizer/Renderer.cpp` — set it wherever `underline.thickness` is set from the font, from the
  font descriptions' DPI.
- Modify: `src/vtrasterizer/DecorationRenderer.cpp` — the flag table (`:37-45`) gains
  `pair { vtbackend::CellFlag::InputMethodPreedit, Decorator::PreeditUnderline }`; `createTileData` gains its case.
- Test: `src/vtrasterizer/DecorationRenderer_test.cpp`, `src/vtrasterizer/GridMetrics_test.cpp` (vtrasterizer_test).

**Interfaces:**
- Consumes: `CellFlag::InputMethodPreedit` (Task 2).
- Produces (`vtrasterizer`): `[[nodiscard]] constexpr int pointsToPixels(double points, double dpi) noexcept;` →
  `max(1, round(points × dpi / 72))`, in `GridMetrics.hpp`; `GridMetrics::preeditUnderlineThickness =
  pointsToPixels(1.0, dpi.y)`.
- The `PreeditUnderline` tile is a solid stroke `preeditUnderlineThickness` rows high whose bottom row sits at the
  font's `underline.position`, like the `Underline` case's placement but with its own thickness, and never above the
  baseline (the `underlineFamilyStaysBelowTheBaseline` rule).

- [ ] **Step 1: Write the failing tests.**
  - `GridMetrics_test.cpp`, `[gridmetrics]`: `pointsToPixels(1, 72) == 1`, `(1, 144) == 2`, `(1, 96) == 1`,
    `(1, 216) == 3`, `(1, 36) == 1` (Review Focus 5).
  - `DecorationRenderer_test.cpp`, `[decoration]`:
    - "preeditUnderlineIsOnePointWhateverTheFontSays": grid metrics with `underline.thickness = 4` and
      `preeditUnderlineThickness = 2` → the tile's inked rows are exactly 2, at the underline position.
    - "preeditUnderlineStaysBelowTheBaseline": as the underline-family case does, over the same metrics table.
    - "theFlagMapsToTheDecorator": a render cell with `InputMethodPreedit` renders one `PreeditUnderline` decoration
      and no `Underline`.
- [ ] **Step 2: Run them and see them fail.** Run:
  `cmake --build out/appleclang-debug -j $(sysctl -n hw.ncpu) --target vtrasterizer_test`. Expected: compile error, no
  `Decorator::PreeditUnderline`.
- [ ] **Step 3: Implement** the decorator, the metric, its computation, the table row and the tile.
- [ ] **Step 4: Run them and see them pass.** Run:
  `out/appleclang-debug/src/vtrasterizer/vtrasterizer_test "[decoration],[gridmetrics]"`. Expected: PASS.
- [ ] **Step 5: Lint, format and commit.**

```bash
/opt/homebrew/opt/llvm@22/bin/clang-tidy -p out/appleclang-debug --extra-arg=-isysroot$(xcrun --show-sdk-path) src/vtrasterizer/DecorationRenderer.cpp src/vtrasterizer/Renderer.cpp src/vtrasterizer/DecorationRenderer_test.cpp src/vtrasterizer/GridMetrics_test.cpp
/opt/homebrew/opt/llvm@22/bin/clang-format -i src/vtrasterizer/Decorator.hpp src/vtrasterizer/GridMetrics.hpp src/vtrasterizer/Renderer.cpp src/vtrasterizer/DecorationRenderer.cpp src/vtrasterizer/DecorationRenderer_test.cpp src/vtrasterizer/GridMetrics_test.cpp
git add src/vtrasterizer/Decorator.hpp src/vtrasterizer/GridMetrics.hpp src/vtrasterizer/Renderer.cpp src/vtrasterizer/DecorationRenderer.cpp src/vtrasterizer/DecorationRenderer_test.cpp src/vtrasterizer/GridMetrics_test.cpp
git commit -m "vtrasterizer: the preedit's underline is one point at the font's DPI" -m "Signed-off-by: Christian Parpart <christian@parpart.family>"
```

---

### Task 4: Finish

- [ ] **Step 1:** Run `/simplify` on the step's diff, `git diff <the plan commit>..HEAD -- src scripts`, apply its
  findings, re-run the filters of every task it touches, and commit once with the Signed-off-by line, staging only the
  files it changed.
- [ ] **Step 2:** Run every task's filter once more:

```bash
out/appleclang-debug/src/vtbackend/vtbackend_test "[renderbuffer],[terminal]"
out/appleclang-debug/src/vtrasterizer/vtrasterizer_test "[decoration],[gridmetrics]"
out/appleclang-debug/src/contour/contour_gui_test "[contour][ime],[config][ime],[config]"
ctest --test-dir out/appleclang-debug -L lint --output-on-failure
```

  Expected: PASS.
- [ ] **Step 3: The grid on this Mac's display**, both rows, to see that the preedit and its underline render under
  every `ChromeStyle` (no new snapshot script: the snapshot scene holds no terminal):

```bash
CONTOUR_TEST_DISPLAY=1 out/appleclang-debug/src/contour/contour_gui_test "[display]"
CONTOUR_TEST_DISPLAY=1 CONTOUR_TEST_CONTROLS_STYLE=ContourTui out/appleclang-debug/src/contour/contour_gui_test "[display]"
```

  Expected: PASS, no QML warnings.
- [ ] **Step 4: Owner manual checks**, batched per phase (listed in the ledger, not awaited), on
  `out/appleclang-debug/src/contour/contour.app` with a fresh (Native2026) config, macOS Japanese – Romaji input:
  - Type `nihongo` at the prompt: `にほんご` appears inline in the foreground colour with a thin underline; Space opens
    the candidates below the first preedit cell. On a Retina and a 1× display the line is one point.
  - Fill the line to the last column, then compose: the candidates sit under the last cell, not at the pane's corner.
  - Compose in the right pane of a split, then drag the divider: the candidates follow. ⌘+ while composing: they
    follow.
  - `defaults_edition: classic`: the preedit is bold, underlined, white on red, as before.
- [ ] **Step 5: Release notes** with `/add-release-note` in `metainfo.xml`, under the open `0.9.0` release:
  - "Keeps the input method's candidate window at the cursor cell, including at the end of a line and while panes
    move or the font changes, instead of the pane's corner."
  - "Draws text being composed by an input method inline, in the cell's own colours with a 1-pt underline, in the new
    defaults; adds `input_method_editor_style` (`inline`, `highlighted`)."
- [ ] **Step 6:** Mark Step 6.10 done in the plans index's step table and its *Last update* line, and on the board
  (`/status-board update`). Copy the SDD ledger to `docs/superpowers/ledgers/2026-10-25-native-redesign-step-6-10.md`.
  Update the PR body with `/update-pr`, reporting:
  - **Performance:** none measurable: one more flag test per rendered cell in the decoration pass, one more tile kind;
    Callgrind on `bench-headless` before and after, recorded in the ledger.
  - **Risk:** low. The anchor only replaces empty answers and adds updates; the preedit's look changes only for
    Native2026; the new cell flag is render-only.
  - **Coverage:** `[contour][ime]` incl. the end-of-line and moved-pane cases; `[renderbuffer][ime]` incl. clipping;
    `[decoration]` and `[gridmetrics]` thickness cases; the `[config][ime]` edition fixtures.

---

## Owner decisions

The run proceeds on each recommended default.

1. **Classic users' preedit (R-d).** *Default: `highlighted` (today's look) in Classic, `inline` in Native2026.* The
   alternative makes `inline` the look for everyone.
2. **A preedit wider than the line (R-e).** *Default: cut at the right edge.* The alternative wraps it onto the next
   row, over cells the program owns, as some terminals do.

## Spec gaps found

- The candidate window "at the cursor cell" leaves the wrap-pending column, an out-of-page cursor and a moving pane
  unspecified; today's empty answer is what sends the window to the pane's origin (R-b, R-c).
- The preedit's colours are not given beyond the board's foreground-on-background; the existing `input_method_editor`
  scheme colours have no stated fate (R-d).
- A preedit longer than the line has no rule (R-e). The preedit's own caret and clause highlighting
  (`QInputMethodEvent::TextFormat`) are not mentioned and stay out of scope.
- Moment 9's preedit is grid content and its candidate window the OS's; neither fits "every visual surface is a
  snapshot row", which assumes chrome.
