# Phase 4 — The gutter

THIS IS A DRAFT DOCUMENT

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the one-column fold strip left of the grid into a configurable, segmented gutter — per-line timestamps, line numbers (absolute / relative / hybrid) and a block column carrying fold controls, exit status and user marks — with a hover tooltip, a collapsed-fold size label, vim's `z` fold keys, and an eviction counter that keeps absolute line numbers honest however rows leave the grid.

**Architecture:** Every decision is a pure unit in `vtbackend/screen/Gutter.hpp` — the segment table that yields the layout, line-number arithmetic, timestamp formatting, the block column's precedence table, its colours, cell painting, the fold label — each with its own tests. `Grid` counts every row its stable floor passes. `Terminal::fillGutter` only gathers per-row facts and hands them to the painter; the renderer draws each gutter cell at the column it carries and draws annotations outside `RenderBuffer::cells`. On the contour side `gutterWidthFor(GutterLayout, cellSize)` is fed from the same `gutterSettingsFor(Config)` the terminal is configured with, so the reserved width, the hit-test and what is drawn cannot disagree.

**Tech Stack:** C++23, CMake presets (`clangcl-debug`), Catch2, Qt 6 / QML + `Qt6::Test`, yaml-cpp.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §5 (5.1–5.5), the `gutter.*` and colour-scheme rows of §11.1, and the eviction-counter item of §16.1. Global constraints: [README](README.md#global-constraints). Contract produced: **C2**. Consumed: **C1** (phase 1), **C6** (phase 3).

---

## Before you start

- [ ] Record the phase start for the gate: `git rev-parse HEAD` → note it as `<phase-4-start>` (the coordinator records it; a subagent reports it).
- [ ] Confirm the C1/C6 symbols this phase calls exist on the branch (`git grep -n` each): `CommandBlockRecord`, `CommandBlockState`, `CommandBlockOutcome`, `outcomeOf`, `CommandBlockId`, `Terminal::commandBlockAt`, `Terminal::commandBlocks`, `Terminal::lineBirthTime`, `TerminalClocks`, `LineFlag::UserMark`, `sanitizeCommandLine`, `SanitizePurpose::Display`. A missing one is a stop-and-report, not a reason to invent it.

### Decisions this phase makes (so reviewers need not rediscover them)

1. **`CommandBlockOutcome` lives in `src/vtbackend/core/CommandBlockOutcome.hpp`** — phase 1 (Task 1.1) already creates it there, because C2 puts `ColorPalette::blockStatusColor(CommandBlockOutcome)` on `ColorPalette`, which is in `core/`, and AGENT.md says `core/` may include nothing but `core/`. Task 4.4 only confirms it and includes it.
2. **The time zone is injected.** It is ambient process state, so `TerminalClocks` gains `LocalTimeConverter localTime = &systemLocalTime;` and `Terminal` gains `localTimeOf()`. Tests pass a converter that reads UTC fields as local time and get the same text in every zone (Task 4.5).
3. **"Evicted" means "the stable floor passed it".** `Grid::syncStableFloor()` counts every advance; a column reflow, which renames rows rather than dropping them, puts the count back. That covers line-wise eviction, block-atomic trims, ED 3, a shrunken history limit and a page grown into a full ring at one site — and phase 5's `ClearToPrompt` for free, provided it moves the floor through `syncStableFloor()`. A narrowing reflow that overflows capacity drops nothing at reflow time (the ring grows); the next scroll evicts the overflow and is counted there (Task 4.1). This answers the §16.1 verification item.
4. **▸ ◆ ● are drawn by vtrasterizer's box-drawing renderer** at their Unicode codepoints (U+25B8, U+25C6, U+25CF), as ▶ ▼ already are: one size and weight in every font, centred on the fold column's stem. No private-use codepoints are needed — Unicode has these shapes — so `docs/vt-extensions/private-use-glyphs.md` is unchanged (Task 4.3).
5. **With fold markers off, every finished block's head shows ●**, whether or not it has output to fold — otherwise `folding.show_markers: false` with `gutter.exit_status: true` would show a block's status only when it printed nothing (Task 4.7).
6. **A success the scheme does not colour is drawn exactly as today's fold column** (faded at rest, full strength hovered); a scheme-set success, failure and running take their colour at rest and are lifted halfway toward the text colour when hovered (Task 4.7).
7. **Gutter clicks:** a left press on the timestamps or line numbers is consumed and does nothing; on the block column it keeps today's behaviour. Hover over a non-block segment is consumed, lights nothing and shows no tooltip (Task 4.12).
8. **Engine defaults are all off** (coordinator ruling, overriding the C2 draft): `vtbackend::GutterSettings` defaults every switch to `false`, so a default-constructed `Settings` reserves no gutter, exactly as before this phase, and no existing render test changes. The user-facing defaults (exit status and user marks on) live in `contour::config::GutterConfig` and reach the terminal through `gutterSettingsFor()`. **Every vtbackend test in this phase sets the switches it relies on explicitly** (`GutterSettings { .foldMarkers = …, .exitStatus = …, .userMarks = … }`); a test that relied on the old draft's `exitStatus = true` default must set it.

### Contract additions (named in each task's Interfaces block)

C2 is produced as written (`CommandBlockOutcome` comes from phase 1's `core/CommandBlockOutcome.hpp`), and `gutterWidthFor` is additionally `inline … noexcept`. Beyond it: `GutterSegment` gains default member initialisers and `operator==`; the pure helpers in `Gutter.hpp` — timestamps (`measureTimestampFormat`, `effectiveTimestampFormat`, `formatGutterTimestamp`, `TimestampRun`, Task 4.5), line numbers (`lineNumberFor`, `lineNumberText`, Task 4.6), the block column, its colours, the painter and the fold label (`blockColumnGlyph`, `blockColumnColors`, `appendGutterCells`, `collapsedFoldLabel`, `foldLabelAnnotation`, Task 4.7) and `std::formatter<LineNumberMode>` (Task 4.11); vtrasterizer's `GutterCells.hpp` and `Renderer::renderAnnotations()` (Task 4.2); `gutterSegmentAt()`; `LocalTimeConverter`, `systemLocalTime()`, `TerminalClocks::localTime`, `Terminal::localTimeOf()`; `FoldingAvailability`, `Settings::foldingAvailability`, `FoldingConfig::availability()`; `Terminal::collapseFoldContaining()`; `ViFoldCommand` and `ViInputHandler::Executor::fold()`; `contour::config::GutterConfig`, `gutterSettingsFor()`; `session::configuredGutterLayout()`; `geometry::gutterColumnAt()`; `session::GutterHit` and the new `sendGutterHoverEvent` / `sendGutterPressEvent` parameter; `TerminalSession::commandBlockTooltip()`, `commandBlockTooltipAnchor`, `commandBlockHoverChanged()`; `vtbackend::formatCommandDuration()` in `shell/CommandBlock.hpp` (Task 4.13a — the one duration formatter; phases 6 and 7 consume it), `session::commandBlockTooltipText()`.

### Build and test commands used below

| Purpose | Command |
|---|---|
| vtbackend tests | `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "<filter>"` |
| vtrasterizer tests | `cmake --build --preset clangcl-debug --target vtrasterizer_test` then `out/build/clangcl-debug/bin/vtrasterizer_test.exe "<filter>"` |
| Qt-free contour tests | `cmake --build --preset clangcl-debug --target contour_test` then `out/build/clangcl-debug/bin/contour_test.exe "<filter>"` |
| GUI tests | `cmake --build --preset clangcl-debug --target contour_gui_test` then `out/build/clangcl-debug/bin/contour_gui_test.exe "<filter>"` (if it exits `0xC0000135`, set the Qt `PATH` / `QT_QPA_PLATFORM_PLUGIN_PATH` as the README describes) |

---

### Task 4.1: `Grid` counts the rows that leave it

**Files:**
- Modify: `src/vtbackend/grid/Grid.hpp` (accessor after `stableRangeFloor()` at :846; `syncStableFloor()` at :1135-1141; new field after `_stableFloor` at :1216)
- Modify: `src/vtbackend/grid/Grid.cpp` (`Grid::reset()` at :726-734; the column switch in `Grid::resize()` at :1138-1155)
- Test: `src/vtbackend/grid/Grid_test.cpp` (append at the end of the file, after the block-atomic section's closing `// }}}`)

**Interfaces:**
- Consumes: nothing new.
- Produces (C2): `[[nodiscard]] uint64_t Grid::evictedRowCount() const noexcept;` — zero after construction and after `reset()`; advanced by every advance of the stable floor except a column reflow's. For phase 5: a `Grid` operation that drops rows by shrinking `_linesUsed` or rotating the ring and then calls `syncStableFloor()` is counted automatically; it must never assign `_stableFloor` directly.

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/grid/Grid_test.cpp`:

```cpp
// {{{ evicted row count (the gutter's absolute line numbers, spec §5.3)
namespace
{
/// How many rows the grid can still address: the history above the floor plus the page.
[[nodiscard]] uint64_t addressableRowCount(Grid const& grid)
{
    return static_cast<uint64_t>(unbox<int64_t>(boxed_cast<LineOffset>(grid.pageSize().lines))
                                 - unbox<int64_t>(grid.addressableTop()));
}
} // namespace

TEST_CASE("Grid.evictedRowCount.lineWiseEvictionAtCapacity", "[grid][evicted]")
{
    // Ring capacity: 2 page + 1 history = 3 slots.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(1));
    CHECK(grid.evictedRowCount() == 0);

    grid.scrollUp(LineCount(1)); // fills the one history slot
    CHECK(grid.evictedRowCount() == 0);

    grid.scrollUp(LineCount(1)); // at capacity: the oldest row goes
    CHECK(grid.evictedRowCount() == 1);

    grid.scrollUp(LineCount(3)); // three at once, all past capacity
    CHECK(grid.evictedRowCount() == 4);
}

TEST_CASE("Grid.evictedRowCount.aScrollThatFillsAndOverflowsCountsOnlyTheOverflow", "[grid][evicted]")
{
    // One free history slot and a three-line scroll: one row is kept, two are dropped.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(1));
    grid.scrollUp(LineCount(3));
    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.evictedRowCount() == 2);
}

TEST_CASE("Grid.evictedRowCount.anInfiniteScrollbackNeverEvicts", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, Infinite {});
    for ([[maybe_unused]] auto const _: std::views::iota(0, 100))
        grid.scrollUp(LineCount(1));
    CHECK(grid.evictedRowCount() == 0);
}

TEST_CASE("Grid.evictedRowCount.aBlockAtomicTrimCountsEveryRowItDrops", "[grid][evicted]")
{
    // Headroom, and a prompt every third line: the trim snaps to a block start and drops whole blocks.
    auto grid = makeGrid(LineCount(1), LineCount(2), LineCount(6));
    auto appended = uint64_t { 0 };
    for (auto const i: std::views::iota(0, 30))
    {
        appendLine(grid, 'L', i, i % 3 == 0 ? BlockStart::Yes : BlockStart::No);
        ++appended;
        // Every row the grid ever held -- the page row it started with and each appended line -- is
        // either still addressable or counted as evicted. Absolute line numbers rest on this identity.
        CHECK(addressableRowCount(grid) + grid.evictedRowCount() == 1 + appended);
    }
    CHECK(grid.evictedRowCount() > 0);
}

TEST_CASE("Grid.evictedRowCount.clearingTheScrollbackCountsIt", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(5));
    grid.scrollUp(LineCount(3));
    REQUIRE(grid.historyLineCount() == LineCount(3));

    grid.clearHistory(); // ED 3
    CHECK(grid.evictedRowCount() == 3);
}

TEST_CASE("Grid.evictedRowCount.shrinkingTheHistoryLimitCountsWhatFellOut", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(1), ColumnCount(5) }, false, LineCount(3));
    grid.scrollUp(LineCount(3));
    REQUIRE(grid.historyLineCount() == LineCount(3));

    grid.setHistoryLimits(HistoryLimits::plain(LineCount(1)));
    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.evictedRowCount() == 2);
}

TEST_CASE("Grid.evictedRowCount.aHeightChangeKeepsEveryRowAccountedFor", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(4), ColumnCount(5) }, false, LineCount(2));
    grid.scrollUp(LineCount(2));
    auto const rowsEver = uint64_t { 6 }; // the four page rows it started with, and two scrolled in
    REQUIRE(addressableRowCount(grid) + grid.evictedRowCount() == rowsEver);

    // Shrinking with the cursor on the bottom row pushes the top of the page into the scrollback.
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(5) },
                       CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) },
                       false);
    CHECK(addressableRowCount(grid) + grid.evictedRowCount() == rowsEver);

    // Growing with the cursor at the top appends two blank rows below it.
    (void) grid.resize(PageSize { LineCount(4), ColumnCount(5) },
                       CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
                       false);
    CHECK(addressableRowCount(grid) + grid.evictedRowCount() == rowsEver + 2);
}

TEST_CASE("Grid.evictedRowCount.aReflowEvictsNothing", "[grid][evicted]")
{
    // Ring capacity: 2 page + 2 history. Four full-width lines, so the two blank rows the page started
    // with are what filling it evicts.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(4) }, true, LineCount(2));
    for (auto const text: { "AAAA"sv, "BBBB"sv, "CCCC"sv, "DDDD"sv })
        appendLine(grid, text);
    REQUIRE(grid.evictedRowCount() == 2);

    // Narrowing re-chops every line in two -- eight rows, more than the ring holds. A reflow renames
    // rows and drops none (the overflow stays until scrolling pushes it out), so nothing is counted.
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(2) },
                       CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) },
                       false);
    CHECK(grid.evictedRowCount() == 2);

    // ...and the next scroll counts exactly the row it pushes out.
    grid.scrollUp(LineCount(1));
    CHECK(grid.evictedRowCount() == 3);

    // Widening rejoins them, which renames rows again and drops none.
    (void) grid.resize(PageSize { LineCount(2), ColumnCount(4) },
                       CellLocation { .line = LineOffset(1), .column = ColumnOffset(1) },
                       false);
    CHECK(grid.evictedRowCount() == 3);
}

TEST_CASE("Grid.evictedRowCount.reclaimingAGarbageSlotIsNotAnEviction", "[grid][evicted]")
{
    // At capacity, a full-page reverse scroll wraps a destroyed page row into the oldest history slot,
    // below the floor. That slot was never addressable, so recycling it counts nothing.
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(2));
    for ([[maybe_unused]] auto const _: std::views::iota(0, 4))
        grid.scrollUp(LineCount(1));
    REQUIRE(grid.evictedRowCount() == 2);

    reverseScrollFullPage(grid, LineCount(1));
    CHECK(grid.evictedRowCount() == 2);

    grid.scrollUp(LineCount(1)); // recycles the garbage slot
    CHECK(grid.evictedRowCount() == 2);

    grid.scrollUp(LineCount(1)); // evicts a real row
    CHECK(grid.evictedRowCount() == 3);
}

TEST_CASE("Grid.evictedRowCount.aResetStartsOver", "[grid][evicted]")
{
    auto grid = Grid(PageSize { LineCount(2), ColumnCount(5) }, false, LineCount(1));
    grid.scrollUp(LineCount(5));
    REQUIRE(grid.evictedRowCount() > 0);

    grid.reset(); // RIS
    CHECK(grid.evictedRowCount() == 0);
}
// }}}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL to compile — `'evictedRowCount': is not a member of 'vtbackend::Grid'`.

- [ ] **Step 3: Add the accessor, the field and the counting**

In `src/vtbackend/grid/Grid.hpp`, after `[[nodiscard]] int64_t stableRangeFloor() const noexcept { return _stableFloor; }` (:846) insert:

```cpp

    /// How many rows have left this grid for good since it was created or last reset.
    ///
    /// The gutter's absolute line numbers rest on it: a row's number is this count plus its distance
    /// from addressableTop(), plus one, so it stays put while the history scrolls away (spec §5.3).
    /// "Left for good" means the stable floor passed it -- line-wise eviction at capacity, a
    /// block-atomic trim, ED 3, a shrunken history limit, a page grown into a full ring -- and it is
    /// counted where the floor moves rather than at each of those sites, so a new way of dropping rows
    /// is counted by construction. The one floor move that is NOT an eviction is a column reflow's,
    /// which renames rows instead of dropping them (@see resize).
    /// @return The count; zero after reset().
    [[nodiscard]] uint64_t evictedRowCount() const noexcept { return _evictedRowCount; }
```

Replace `syncStableFloor()` (:1135-1141):

```cpp
    /// Re-establishes the floor invariant `_stableFloor >= _stableBase - history` after
    /// anything moved the base or shrank the history. max() keeps it monotonic: eviction
    /// only ever advances it within a generation.
    void syncStableFloor() noexcept
    {
        _stableFloor = std::max(_stableFloor, _stableBase - unbox<int64_t>(historyLineCount()));
    }
```

with:

```cpp
    /// Re-establishes the floor invariant `_stableFloor >= _stableBase - history` after
    /// anything moved the base or shrank the history. max() keeps it monotonic: eviction
    /// only ever advances it within a generation -- and every row it advances past is counted
    /// as evicted (@see evictedRowCount).
    void syncStableFloor() noexcept
    {
        auto const raised = std::max(_stableFloor, _stableBase - unbox<int64_t>(historyLineCount()));
        _evictedRowCount += static_cast<uint64_t>(raised - _stableFloor);
        _stableFloor = raised;
    }
```

After `int64_t _stableFloor = 0; ///< Oldest addressable id; monotonic within a generation.` (:1216) insert:

```cpp
    uint64_t _evictedRowCount = 0; ///< Rows the floor has passed since the last reset (@see evictedRowCount).
```

- [ ] **Step 4: Reset the count on RIS and keep it across a reflow**

In `src/vtbackend/grid/Grid.cpp`, replace `Grid::reset()` (:726-734):

```cpp
void Grid::reset()
{
    _linesUsed = _pageSize.lines;
    _lines.rotateRight(_lines.zeroIndex());
    for (int i = 0; i < unbox(_pageSize.lines); ++i)
        _lines[i].reset(defaultLineFlags(), GraphicsAttributes {}, _pageSize.columns);
    bumpGeneration(RowIdentity::Destroyed);
    verifyState();
}
```

with:

```cpp
void Grid::reset()
{
    _linesUsed = _pageSize.lines;
    _lines.rotateRight(_lines.zeroIndex());
    for (int i = 0; i < unbox(_pageSize.lines); ++i)
        _lines[i].reset(defaultLineFlags(), GraphicsAttributes {}, _pageSize.columns);
    bumpGeneration(RowIdentity::Destroyed);
    // RIS: the session starts over, and its numbering with it. Zeroed after the bump, whose floor sync
    // has just counted the whole scrollback this threw away.
    _evictedRowCount = 0;
    verifyState();
}
```

In `Grid::resize()`, replace:

```cpp
    using crispy::Comparison;
    switch (crispy::strongCompare(newSize.columns, _pageSize.columns))
```

with:

```cpp
    // A reflow rebuilds the ring and re-keys every row through rotateBuffersLeft(), which walks the
    // stable floor across rows that were RENAMED, not dropped: a narrowing reflow that overflows the
    // capacity keeps the overflow until the next scroll evicts it, and that scroll is what counts it.
    // So the floor's walk across a column change is put back below (@see evictedRowCount).
    auto const evictedBeforeReflow = _evictedRowCount;

    using crispy::Comparison;
    switch (crispy::strongCompare(newSize.columns, _pageSize.columns))
```

and replace:

```cpp
        case Comparison::Equal: break;
    }

    switch (crispy::strongCompare(newSize.lines, _pageSize.lines))
```

with:

```cpp
        case Comparison::Equal: break;
    }
    _evictedRowCount = evictedBeforeReflow;

    switch (crispy::strongCompare(newSize.lines, _pageSize.lines))
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[evicted]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[grid]"`
Expected: PASS — all `[evicted]` cases, and no regression in `[grid]` (the counter is write-only for every existing path).

- [ ] **Step 6: Format and commit**

```bash
clang-format -i src/vtbackend/grid/Grid.hpp src/vtbackend/grid/Grid.cpp src/vtbackend/grid/Grid_test.cpp
git add src/vtbackend/grid/Grid.hpp src/vtbackend/grid/Grid.cpp src/vtbackend/grid/Grid_test.cpp
git commit -F - <<'EOF'
vtbackend: count the rows that leave the grid

Every row the stable floor passes is counted as evicted, at the one place
the floor moves, so line-wise eviction, block-atomic trims, ED 3, a shrunken
history limit and a page grown into a full ring are all covered. A column
reflow renames rows rather than dropping them and puts the count back; RIS
zeroes it. The gutter's absolute line numbers are built on this count.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.2: Gutter cells carry their column; annotations are drawn outside the cells

**Files:**
- Modify: `src/vtbackend/render/RenderBuffer.hpp` (`RenderGutterCell` at :92-103; `RenderBuffer` at :116-131)
- Create: `src/vtrasterizer/GutterCells.hpp`
- Modify: `src/vtrasterizer/Renderer.hpp` (`renderGutter` declaration at :390-397)
- Modify: `src/vtrasterizer/Renderer.cpp` (render passes at :757-764 and :789-795; `renderGutter` at :926-956)
- Modify: `src/vtrasterizer/CMakeLists.txt` (`_header_files`, `_test_files`)
- Test: `src/vtrasterizer/GutterCells_test.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces (C2): `RenderGutterCell::column` (`ColumnOffset`, default `-1`); `struct RenderAnnotation { CellLocation position; std::u32string text; RenderAttributes attributes; };`; `std::vector<RenderAnnotation> RenderBuffer::annotations`, cleared by `RenderBuffer::clear()`.
- Produces (vtrasterizer): `[[nodiscard]] vtbackend::RenderCell gutterRenderCell(vtbackend::RenderGutterCell const& marker);`, `[[nodiscard]] std::vector<vtbackend::RenderCell> annotationRenderCells(vtbackend::RenderAnnotation const& annotation);`, `void Renderer::renderAnnotations(std::span<vtbackend::RenderAnnotation const> annotations);`

There is no `Renderer` test harness (it needs fonts and a render target), so the placement is extracted into the pure `GutterCells.hpp` and tested there; `Renderer` only loops over it.

- [ ] **Step 1: Write the failing tests**

Create `src/vtrasterizer/GutterCells_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/render/RenderBuffer.hpp>

#include <vtrasterizer/GutterCells.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using vtrasterizer::annotationRenderCells;
using vtrasterizer::gutterRenderCell;

TEST_CASE("GutterCells.aGutterCellIsDrawnAtItsOwnColumn", "[renderer][gutter]")
{
    // A multi-column gutter is the same grid at negative columns: column -4 is four cells left of
    // column 0, through the very metrics every grid cell is placed with.
    auto const marker = RenderGutterCell {
        .lineOffset = LineOffset(3), .codepoint = U'7', .attributes = {}, .column = ColumnOffset(-4)
    };
    auto const cell = gutterRenderCell(marker);
    CHECK(cell.position.line == LineOffset(3));
    CHECK(cell.position.column == ColumnOffset(-4));
    CHECK(cell.codepoints == U"7");
    CHECK(cell.width == 1);
    // Its own shaping group, so it is placed at its own pen position.
    CHECK(cell.groupStart);
    CHECK(cell.groupEnd);
}

TEST_CASE("GutterCells.aCellThatNamesNoColumnSitsBesideTheGrid", "[renderer][gutter]")
{
    auto const cell = gutterRenderCell(RenderGutterCell { .lineOffset = LineOffset(0), .codepoint = U'x' });
    CHECK(cell.position.column == ColumnOffset(-1));
}

TEST_CASE("GutterCells.anAnnotationIsOneRunOfConsecutiveCells", "[renderer][gutter]")
{
    auto const annotation = RenderAnnotation {
        .position = CellLocation { .line = LineOffset(2), .column = ColumnOffset(5) },
        .text = U"⋯ 2",
        .attributes = {},
    };
    auto const cells = annotationRenderCells(annotation);
    REQUIRE(cells.size() == 3);
    for (auto const index: { 0, 1, 2 })
    {
        CAPTURE(index);
        auto const& cell = cells[static_cast<size_t>(index)];
        CHECK(cell.position.line == LineOffset(2));
        CHECK(cell.position.column == ColumnOffset(5 + index));
        CHECK(cell.codepoints == std::u32string(1, annotation.text[static_cast<size_t>(index)]));
        // Shaped as one run: it opens on the first cell and closes on the last.
        CHECK(cell.groupStart == (index == 0));
        CHECK(cell.groupEnd == (index == 2));
    }
}

TEST_CASE("GutterCells.anEmptyAnnotationDrawsNothing", "[renderer][gutter]")
{
    CHECK(annotationRenderCells(RenderAnnotation {}).empty());
}

TEST_CASE("GutterCells.clearingABufferClearsItsAnnotations", "[renderer][gutter]")
{
    auto buffer = RenderBuffer {};
    buffer.annotations.push_back(RenderAnnotation { .position = {}, .text = U"x", .attributes = {} });
    buffer.clear();
    CHECK(buffer.annotations.empty());
}
```

In `src/vtrasterizer/CMakeLists.txt`, add `    GutterCells.hpp` after `    GridMetrics.hpp` in `_header_files`, and `    GutterCells_test.cpp` after `    GlyphSlicing_test.cpp` in `_test_files`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtrasterizer_test`
Expected: FAIL to compile — `vtrasterizer/GutterCells.hpp` not found.

- [ ] **Step 3: Give the gutter cell a column and add the annotation**

In `src/vtbackend/render/RenderBuffer.hpp`, replace the `RenderGutterCell` block (:92-103):

```cpp
/// One row's gutter content: the fold marker drawn in the strip left of the grid.
///
/// A row of its own rather than a RenderCell in @ref RenderBuffer::cells, deliberately: the gutter sits
/// at a NEGATIVE column, outside the page entirely, and everything that walks the cells -- selection,
/// hit-testing, the accessibility bridge, screenshots -- is entitled to assume a column is on the page.
/// Keeping the two apart means none of them has to learn otherwise.
struct RenderGutterCell
{
    LineOffset lineOffset;       ///< The screen row this marker belongs to.
    char32_t codepoint {};       ///< The marker glyph.
    RenderAttributes attributes; ///< Its colors.
};
```

with:

```cpp
/// One cell of the gutter: a character drawn in the strip left of the grid.
///
/// A cell of its own rather than a RenderCell in @ref RenderBuffer::cells, deliberately: the gutter sits
/// at NEGATIVE columns, outside the page entirely, and everything that walks the cells -- selection,
/// hit-testing, the accessibility bridge, screenshots -- is entitled to assume a column is on the page.
/// Keeping the two apart means none of them has to learn otherwise.
struct RenderGutterCell
{
    LineOffset lineOffset;       ///< The screen row this cell belongs to.
    char32_t codepoint {};       ///< The character.
    RenderAttributes attributes; ///< Its colors.

    /// The column it is drawn at, relative to grid column 0: -1 is the cell beside the grid, and a
    /// gutter N columns wide spans -N .. -1 (@see vtbackend::GutterLayout).
    ColumnOffset column { -1 };
};

/// Render-only text the terminal places over the page -- a collapsed fold's `⋯ 1,234 lines`.
///
/// Not a RenderCell either, for the gutter's reason: selection, copying and the accessibility bridge
/// walk RenderBuffer::cells, and an annotation is something to read, never something to select.
struct RenderAnnotation
{
    CellLocation position;       ///< Where its first character goes, in screen coordinates.
    std::u32string text;         ///< One column per codepoint.
    RenderAttributes attributes; ///< Its colors.
};
```

In `struct RenderBuffer`, replace:

```cpp
    std::vector<RenderGutterCell> gutter {};
```

with:

```cpp
    std::vector<RenderGutterCell> gutter {};
    std::vector<RenderAnnotation> annotations {};
```

and in `clear()` replace `        gutter.clear();` with:

```cpp
        gutter.clear();
        annotations.clear();
```

- [ ] **Step 4: Create the placement helpers**

Create `src/vtrasterizer/GutterCells.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/render/RenderBuffer.hpp>

#include <ranges>
#include <string>
#include <vector>

namespace vtrasterizer
{

/// The cell a gutter cell is drawn as: at its own column left of the grid, as a shaping group of its own.
///
/// Split out of Renderer::renderGutter so the placement is testable without fonts or a render target. A
/// gutter cell is mapped through the very GridMetrics every grid cell is, at a NEGATIVE column, which is
/// the whole of what lets a gutter several columns wide exist without a second coordinate system.
///
/// @param marker The gutter cell the terminal emitted.
/// @return The cell to hand the background and text renderers.
[[nodiscard]] inline vtbackend::RenderCell gutterRenderCell(vtbackend::RenderGutterCell const& marker)
{
    // groupStart and groupEnd together make each cell its own shaping group, so it is placed at its own
    // pen position rather than run together with whatever the text renderer saw last.
    return vtbackend::RenderCell {
        .codepoints = std::u32string(1, marker.codepoint),
        .image = {},
        .position = vtbackend::CellLocation { .line = marker.lineOffset, .column = marker.column },
        .attributes = marker.attributes,
        .width = 1,
        .sizing = {},
        .groupStart = true,
        .groupEnd = true,
    };
}

/// The cells an annotation is drawn as: one per codepoint, rightwards from its position, shaped as one run.
///
/// @param annotation The annotation the terminal placed.
/// @return Its cells, left to right; empty for an empty text.
[[nodiscard]] inline std::vector<vtbackend::RenderCell> annotationRenderCells(
    vtbackend::RenderAnnotation const& annotation)
{
    auto cells = std::vector<vtbackend::RenderCell> {};
    cells.reserve(annotation.text.size());
    for (auto const index: std::views::iota(size_t { 0 }, annotation.text.size()))
    {
        cells.push_back(vtbackend::RenderCell {
            .codepoints = std::u32string(1, annotation.text[index]),
            .image = {},
            .position = vtbackend::CellLocation { .line = annotation.position.line,
                                                  .column = annotation.position.column
                                                            + vtbackend::ColumnOffset::cast_from(index) },
            .attributes = annotation.attributes,
            .width = 1,
            .sizing = {},
            .groupStart = index == 0,
            .groupEnd = index + 1 == annotation.text.size(),
        });
    }
    return cells;
}

} // namespace vtrasterizer
```

- [ ] **Step 5: Draw through the helpers**

In `src/vtrasterizer/Renderer.hpp`, replace the `renderGutter` declaration and its comment (:390-397):

```cpp
    /// Draws the fold markers into the gutter -- the strip the page margin reserves to the LEFT of
    /// column 0 (@see contour::geometry::fitPageToPixels, which folds the gutter into pageMargin.left).
    ///
    /// This is the ONE place in the tree that addresses a negative column, and the reason it can: a
    /// gutter cell is mapped through the same GridMetrics as any other, and column -1 lands exactly in
    /// the strip, so no second coordinate system is introduced for it. Keeping that here means nothing
    /// that walks RenderBuffer::cells -- selection, hit-testing, accessibility -- ever meets one.
    void renderGutter(std::span<vtbackend::RenderGutterCell const> gutter);
```

with:

```cpp
    /// Draws the gutter -- the strip the page margin reserves to the LEFT of column 0 (@see
    /// contour::geometry::fitPageToPixels, which folds the gutter into pageMargin.left).
    ///
    /// The ONE place in the tree that addresses negative columns, and the reason it can: each gutter cell
    /// carries its column (-N .. -1) and is mapped through the same GridMetrics as any other, so the strip
    /// needs no second coordinate system. Keeping that here means nothing that walks
    /// RenderBuffer::cells -- selection, hit-testing, accessibility -- ever meets one.
    void renderGutter(std::span<vtbackend::RenderGutterCell const> gutter);

    /// Draws the annotations -- render-only text such as a collapsed fold's `⋯ N lines` -- over the cells
    /// already drawn, where the terminal placed them. Never through RenderBuffer::cells, which selection,
    /// copying and the accessibility bridge walk.
    void renderAnnotations(std::span<vtbackend::RenderAnnotation const> annotations);
```

In `src/vtrasterizer/Renderer.cpp`, add `#include <vtrasterizer/GutterCells.hpp>` to the vtrasterizer include group. Replace `Renderer::renderGutter` (:926-956) with:

```cpp
void Renderer::renderGutter(std::span<vtbackend::RenderGutterCell const> gutter)
{
    ZoneScoped;
    for (auto const& marker: gutter)
    {
        auto const cell = gutterRenderCell(marker);
        try
        {
            _backgroundRenderer.renderCell(cell);
            _textRenderer.renderCell(cell);
        }
        catch (std::exception const& e)
        {
            errorLog()(
                "renderGutter: skipping a cell on line {} due to exception: {}", marker.lineOffset, e.what());
        }
    }
}

void Renderer::renderAnnotations(std::span<vtbackend::RenderAnnotation const> annotations)
{
    ZoneScoped;
    for (auto const& annotation: annotations)
    {
        for (auto const& cell: annotationRenderCells(annotation))
        {
            try
            {
                _backgroundRenderer.renderCell(cell);
                _textRenderer.renderCell(cell);
            }
            catch (std::exception const& e)
            {
                errorLog()("renderAnnotations: skipping a cell on line {} due to exception: {}",
                           annotation.position.line,
                           e.what());
            }
        }
    }
}
```

In the single-pass branch replace:

```cpp
            renderGutter(std::span(renderBuffer.get().gutter));
        });
    }
    else
```

with:

```cpp
            renderGutter(std::span(renderBuffer.get().gutter));
            renderAnnotations(std::span(renderBuffer.get().annotations));
        });
    }
    else
```

and in the two-pass branch replace:

```cpp
            // The gutter belongs to the main display, and scrolls with it.
            renderGutter(std::span(renderBuffer.get().gutter));
```

with:

```cpp
            // The gutter and the annotations belong to the main display, and scroll with it.
            renderGutter(std::span(renderBuffer.get().gutter));
            renderAnnotations(std::span(renderBuffer.get().annotations));
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtrasterizer_test vtbackend_test` then `out/build/clangcl-debug/bin/vtrasterizer_test.exe "[gutter]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[folding]"`
Expected: PASS. `[folding]`'s gutter tests are unchanged: `Terminal::fillGutter` still emits cells without naming a column, which is `-1`.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i src/vtbackend/render/RenderBuffer.hpp src/vtrasterizer/GutterCells.hpp src/vtrasterizer/GutterCells_test.cpp src/vtrasterizer/Renderer.hpp src/vtrasterizer/Renderer.cpp
git add src/vtbackend/render/RenderBuffer.hpp src/vtrasterizer/GutterCells.hpp src/vtrasterizer/GutterCells_test.cpp src/vtrasterizer/Renderer.hpp src/vtrasterizer/Renderer.cpp src/vtrasterizer/CMakeLists.txt
git commit -F - <<'EOF'
vtrasterizer: draw gutter cells at their column, and annotations

A gutter cell now carries the negative column it is drawn at, so the strip
can be several columns wide; RenderBuffer gains render-only annotations
that are drawn over the page but never enter RenderBuffer::cells. The
placement lives in a pure header and is tested without fonts.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.3: ▸ ◆ ● are drawn by the box-drawing renderer

**Files:**
- Modify: `src/vtrasterizer/BoxDrawingRenderer.cpp` (new `StatusMark` beside `FoldHead`, before `struct Box` at ~:353; `renderable()` at :2082-2105; the U+25xx cases and the commented list at :2257-2304)
- Test: `src/vtrasterizer/BoxDrawingRenderer_test.cpp` (append at the end)

**Interfaces:**
- Consumes: nothing.
- Produces: `BoxDrawingRenderer::renderable()` answers true for U+25B8, U+25C6, U+25CF, and `buildElements()` draws them. No API change.

Why here and not the font: the block column already draws every fold glyph here (`foldMarkerGlyph`'s comment promises it, and `BoxDrawingRenderer.solidTriangles` checks it), and a font's ▸ ◆ ● differ in size and weight between families — a running command's mark would read heavier than a finished one's in some fonts. Drawn from the cell they are one size, centred on the stem the fold column's `│` runs down. Like ▶ ▼, the claim applies to these codepoints wherever they appear; `font.builtin_box_drawing: false` turns it off for every built-in glyph.

- [ ] **Step 1: Write the failing tests**

Append to `src/vtrasterizer/BoxDrawingRenderer_test.cpp`:

```cpp
TEST_CASE("BoxDrawingRenderer.gutterStatusMarks", "[boxdrawing]")
{
    // The block column's status marks, drawn here for the reason the fold heads are: one size and weight
    // in every font, centred on the row the fold column's stem runs down. A realistic 9x20 cell.
    auto constexpr Cell = ImageSize { Width(9), Height(20) };
    auto const build = [&](char32_t codepoint) {
        auto buffer = BoxDrawingRendererTest::buildElements(codepoint, Cell, 1);
        REQUIRE(buffer.has_value());
        return *std::move(buffer);
    };

    SECTION("renderable() claims them, so the font is never consulted")
    {
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x25B8 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x25C6 }));
        CHECK(BoxDrawingRenderer::renderable(char32_t { 0x25CF }));
    }

    SECTION("each fills its cell's buffer, draws something, and sits on the row's middle")
    {
        for (auto const codepoint: { char32_t { 0x25B8 }, char32_t { 0x25C6 }, char32_t { 0x25CF } })
        {
            CAPTURE(static_cast<uint32_t>(codepoint));
            auto const buffer = build(codepoint);
            CHECK(buffer.size() == size_t { 9 * 20 });
            CHECK(countLitPixels(buffer) > 0);
            CHECK(buffer == verticalMirror(buffer, Cell));
        }
    }

    SECTION("the diamond and the disc are symmetric left to right; the triangle points right")
    {
        auto const diamond = build(0x25C6);
        auto const disc = build(0x25CF);
        auto const triangle = build(0x25B8);
        CHECK(diamond == horizontalMirror(diamond, Cell));
        CHECK(disc == horizontalMirror(disc, Cell));

        auto const litInColumns = [](atlas::Buffer const& buffer, size_t from, size_t to) {
            auto lit = size_t { 0 };
            for (auto const y: std::views::iota(size_t { 0 }, size_t { 20 }))
                for (auto const x: std::views::iota(from, to))
                    lit += buffer[(y * 9) + x] > 0 ? 1 : 0;
            return lit;
        };
        // Base on the left, apex on the right: more of it lies left of the middle column.
        CHECK(litInColumns(triangle, 0, 4) > litInColumns(triangle, 5, 9));
    }

    SECTION("the disc covers more of the cell than the diamond inscribed in it")
    {
        CHECK(countLitPixels(build(0x25CF)) > countLitPixels(build(0x25C6)));
    }
}

TEST_CASE("BoxDrawingRenderer.gutterStatusMarksInTinyCells", "[boxdrawing]")
{
    // A small font at a low DPI hands the builder a cell a few device pixels wide; the marks must still
    // come out as a buffer of exactly that size rather than as undefined behaviour.
    for (auto const width: std::views::iota(1, 12))
    {
        for (auto const height: std::views::iota(1, 12))
        {
            CAPTURE(width, height);
            auto const cell =
                ImageSize { Width(static_cast<unsigned>(width)), Height(static_cast<unsigned>(height)) };
            for (auto const codepoint: { char32_t { 0x25B8 }, char32_t { 0x25C6 }, char32_t { 0x25CF } })
            {
                auto const buffer = BoxDrawingRendererTest::buildElements(codepoint, cell, 1);
                REQUIRE(buffer.has_value());
                CHECK(buffer->size() == static_cast<size_t>(width * height));
            }
        }
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtrasterizer_test` then `out/build/clangcl-debug/bin/vtrasterizer_test.exe "BoxDrawingRenderer.gutterStatusMarks*"`
Expected: FAIL — `renderable(0x25B8)` is false and `buildElements` returns `nullopt` (the `REQUIRE(buffer.has_value())` fails).

- [ ] **Step 3: Add the `StatusMark` builder**

In `src/vtrasterizer/BoxDrawingRenderer.cpp`, replace:

```cpp
        struct Box
        {
            Line upval = NoLine;
```

with:

```cpp
        /// Which of the gutter's status marks a cell draws.
        enum class StatusMarkShape : uint8_t
        {
            SmallTriangle = 0, ///< U+25B8, a block whose command is still running.
            Diamond,           ///< U+25C6, a row the user marked.
            Disc,              ///< U+25CF, a finished block with no fold control on its head.
        };

        /// The block column's status marks: a small right-pointing triangle, a diamond and a disc.
        ///
        /// Drawn here rather than left to the font for the reason FoldHead is: they share a column with
        /// the fold controls, and a font's marks differ in size and weight between families. Each shape
        /// is a membership test on the cell's centre, sampled four times per axis and box-filtered down,
        /// which is what anti-aliases its slanted and curved edges.
        struct StatusMark
        {
            ImageSize size {};
            StatusMarkShape shape = StatusMarkShape::Disc;

            /// How much of the cell's shorter side the mark spans.
            static constexpr double Extent = 0.6;

            /// Whether the point (@p dx, @p dy), relative to the cell centre, lies in @p shape of
            /// half-extent @p radius.
            [[nodiscard]] static bool contains(StatusMarkShape shape, double dx, double dy, double radius) noexcept
            {
                switch (shape)
                {
                    case StatusMarkShape::SmallTriangle:
                        // Base on the left at -0.8r, apex on the centre line at +0.8r.
                        return dx >= -radius * 0.8 && std::abs(dy) <= ((radius * 0.8) - dx) / 2.0;
                    case StatusMarkShape::Diamond: return std::abs(dx) + std::abs(dy) <= radius;
                    case StatusMarkShape::Disc: return (dx * dx) + (dy * dy) <= radius * radius;
                }
                return false;
            }

            operator atlas::Buffer() const
            {
                constexpr auto Supersampling = size_t { 4 };
                auto pixmap = blockElement<Supersampling>(size);
                auto const w = unbox<int>(pixmap.size.width);
                auto const h = unbox<int>(pixmap.size.height);
                auto const radius = std::min(w, h) * Extent / 2.0;
                for (auto const y: std::views::iota(0, h))
                    for (auto const x: std::views::iota(0, w))
                        if (contains(shape, (x + 0.5) - (w / 2.0), (y + 0.5) - (h / 2.0), radius))
                            pixmap.paint(x, y);
                return pixmap.take();
            }
        };

        struct Box
        {
            Line upval = NoLine;
```

- [ ] **Step 4: Claim the codepoints and build them**

In `BoxDrawingRenderer::renderable()`, replace:

```cpp
           || codepoint == 0x25BC                   // ▼ BLACK DOWN-POINTING TRIANGLE
```

with:

```cpp
           || codepoint == 0x25BC                   // ▼ BLACK DOWN-POINTING TRIANGLE
           || codepoint == 0x25B8                   // ▸ BLACK RIGHT-POINTING SMALL TRIANGLE
           || codepoint == 0x25C6                   // ◆ BLACK DIAMOND
           || codepoint == 0x25CF                   // ● BLACK CIRCLE
```

In `buildElements()`, replace:

```cpp
        case 0x25BC: return /* ▼ */ triangle<Dir::Top, Inverted::No>(size);
```

with:

```cpp
        case 0x25BC: return /* ▼ */ triangle<Dir::Top, Inverted::No>(size);
        // The gutter's status marks (@see StatusMark): a running command, a user's mark, a finished
        // command with no fold control on its head.
        case 0x25B8: return /* ▸ */ StatusMark { .size = size, .shape = StatusMarkShape::SmallTriangle };
        case 0x25C6: return /* ◆ */ StatusMark { .size = size, .shape = StatusMarkShape::Diamond };
        case 0x25CF: return /* ● */ StatusMark { .size = size, .shape = StatusMarkShape::Disc };
```

Then delete these three lines from the commented list of not-yet-drawn U+25xx shapes that follows, since they are drawn now: the line naming `U+25B8  BLACK RIGHT-POINTING SMALL TRIANGLE`, the line naming `U+25C6  BLACK DIAMOND`, and the line naming `U+25CF  BLACK CIRCLE`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtrasterizer_test` then `out/build/clangcl-debug/bin/vtrasterizer_test.exe "[boxdrawing]"`
Expected: PASS, including the existing `BoxDrawingRenderer.solidTriangles` (its neighbours U+25B2 and U+25C0 still fall through to the font).

- [ ] **Step 6: Format and commit**

```bash
clang-format -i src/vtrasterizer/BoxDrawingRenderer.cpp src/vtrasterizer/BoxDrawingRenderer_test.cpp
git add src/vtrasterizer/BoxDrawingRenderer.cpp src/vtrasterizer/BoxDrawingRenderer_test.cpp
git commit -F - <<'EOF'
vtrasterizer: draw the gutter's status marks

U+25B8, U+25C6 and U+25CF are drawn by the box-drawing renderer, as the
fold column's glyphs already are, so a running command's triangle, a
user's diamond and a finished command's dot have one size and weight in
every font and sit on the fold column's stem.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.4: The palette resolves the block-status and gutter-text colours

**Files:**
- Modify: `src/vtbackend/core/ColorPalette.hpp` (includes; new members after `foldMarkerHoverColors()` at :347-351)
- Test: `src/vtbackend/core/ColorPalette_test.cpp` (append)

**Interfaces:**
- Consumes (C1, phase 1 Task 1.1): `enum class CommandBlockOutcome : uint8_t { Success = 0, Failure, Running };` in `src/vtbackend/core/CommandBlockOutcome.hpp` (phase 1 already puts it in `core/`, so `ColorPalette` may include it).
- Produces (C2): `std::optional<RGBColor> ColorPalette::blockStatusSuccess, blockStatusFailure, blockStatusRunning, gutterText;` `[[nodiscard]] RGBColor ColorPalette::blockStatusColor(CommandBlockOutcome outcome) const noexcept;` `[[nodiscard]] RGBColor ColorPalette::gutterTextColor() const noexcept;`

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/core/ColorPalette_test.cpp`:

```cpp
TEST_CASE("ColorPalette.blockStatus.derivesWhenUnset", "[ColorPalette]")
{
    auto const palette = ColorPalette {};

    // A success looks like the fold column always did; a failure and a running command take the
    // palette's own red and yellow, which every scheme already tunes to read on its background.
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Success) == palette.foldMarkerColors().foreground);
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == palette.normalColor(1));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == palette.normalColor(3));

    // Line numbers and timestamps are as quiet as the resting fold column.
    CHECK(palette.gutterTextColor() == palette.foldMarkerColors().foreground);
}

TEST_CASE("ColorPalette.blockStatus.followsThePalettesOwnColours", "[ColorPalette]")
{
    auto palette = ColorPalette {};
    palette.palette[1] = RGBColor(0x11, 0x22, 0x33);
    palette.palette[3] = RGBColor(0x44, 0x55, 0x66);
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == RGBColor(0x11, 0x22, 0x33));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == RGBColor(0x44, 0x55, 0x66));
}

TEST_CASE("ColorPalette.blockStatus.explicitColorsBypassTheDerivation", "[ColorPalette]")
{
    auto palette = ColorPalette {};
    palette.blockStatusSuccess = RGBColor(0x01, 0x02, 0x03);
    palette.blockStatusFailure = RGBColor(0x04, 0x05, 0x06);
    palette.blockStatusRunning = RGBColor(0x07, 0x08, 0x09);
    palette.gutterText = RGBColor(0x0A, 0x0B, 0x0C);

    CHECK(palette.blockStatusColor(CommandBlockOutcome::Success) == RGBColor(0x01, 0x02, 0x03));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == RGBColor(0x04, 0x05, 0x06));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == RGBColor(0x07, 0x08, 0x09));
    CHECK(palette.gutterTextColor() == RGBColor(0x0A, 0x0B, 0x0C));
}

TEST_CASE("ColorPalette.blockStatus.defaultSchemeMatchesTheDocumentedColors", "[ColorPalette]")
{
    // The commented example `contour generate config` writes (Task 4.10) quotes these values.
    auto const palette = ColorPalette {};
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Failure) == RGBColor(0xC6, 0x39, 0x39));
    CHECK(palette.blockStatusColor(CommandBlockOutcome::Running) == RGBColor(0xA0, 0xA0, 0x00));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL to compile — `'blockStatusColor': is not a member of 'vtbackend::ColorPalette'`.

- [ ] **Step 3: Confirm `CommandBlockOutcome` is in `core/`**

Run: `git grep -n "enum class CommandBlockOutcome" src/vtbackend`
Expected: exactly one hit, in `src/vtbackend/core/CommandBlockOutcome.hpp` (phase 1, Task 1.1). Anything else is a stop-and-report: `ColorPalette` may only include `core/` headers.

- [ ] **Step 4: Add the slots and resolvers**

In `src/vtbackend/core/ColorPalette.hpp`, add `#include <vtbackend/core/CommandBlockOutcome.hpp>` after `#include <vtbackend/core/Color.hpp>`. After the closing brace of `foldMarkerHoverColors()` (the line after `RGBColorPair { .foreground = defaultForeground, .background = defaultBackground });`) insert:

```cpp

    /// The block column's colours by outcome (spec §5.2), each unset unless the scheme says otherwise.
    ///
    /// Derived when unset, for the reason foldMarker is: a status that a scheme predating it cannot
    /// colour would be a status nobody sees. @see blockStatusColor for what each derives from.
    std::optional<RGBColor> blockStatusSuccess;
    std::optional<RGBColor> blockStatusFailure;
    std::optional<RGBColor> blockStatusRunning;

    /// The colour of the gutter's text -- its line numbers and timestamps -- unset unless the scheme says
    /// otherwise.
    std::optional<RGBColor> gutterText;

    /// The colour a block's gutter glyphs take for @p outcome, with the unset case resolved.
    ///
    /// Success derives from the fold column's resting colour -- neutral, so a column of successful
    /// commands looks exactly as the fold column always did -- while failure and running derive from the
    /// palette's own red and yellow, which every scheme already tunes to read on its background.
    [[nodiscard]] RGBColor blockStatusColor(CommandBlockOutcome outcome) const noexcept
    {
        switch (outcome)
        {
            case CommandBlockOutcome::Success: return blockStatusSuccess.value_or(foldMarkerColors().foreground);
            case CommandBlockOutcome::Failure: return blockStatusFailure.value_or(normalColor(1));
            case CommandBlockOutcome::Running: return blockStatusRunning.value_or(normalColor(3));
        }
        return foldMarkerColors().foreground;
    }

    /// The gutter's text colour with the unset case resolved: the scheme's foreground faded toward its
    /// background exactly as the resting fold column is, so numbers and times stay quieter than the
    /// output beside them in any theme.
    [[nodiscard]] RGBColor gutterTextColor() const noexcept
    {
        return gutterText.value_or(mixColor(defaultForeground, defaultBackground, FoldMarkerRestFade));
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[ColorPalette]"`
Expected: PASS.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i src/vtbackend/core/ColorPalette.hpp src/vtbackend/core/ColorPalette_test.cpp
git add src/vtbackend/core/ColorPalette.hpp src/vtbackend/core/ColorPalette_test.cpp
git commit -F - <<'EOF'
vtbackend: resolve block-status and gutter-text colours in the palette

Four optional scheme slots, each derived when unset: success from the fold
column's resting colour, failure and running from the palette's red and
yellow, the gutter's text from the faded foreground.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.5: Timestamps — the local-time seam and the formatter

**Files:**
- Create: `src/vtbackend/screen/LocalTime.hpp`, `src/vtbackend/screen/LocalTime.cpp`
- Create: `src/vtbackend/screen/Gutter.hpp`, `src/vtbackend/screen/Gutter.cpp` (the timestamp section)
- Modify: `src/vtbackend/screen/TerminalClocks.hpp` (phase 1: one member)
- Modify: `src/vtbackend/screen/Terminal.hpp` (`localTimeOf()` after `currentTime()` at :843; it reads Task 1.7's `TerminalClocks _clocks` member, so no new member)
- Modify: `src/vtbackend/CMakeLists.txt` (headers, sources, tests)
- Test: `src/vtbackend/screen/Gutter_test.cpp`, `src/vtbackend/screen/Terminal_gutter_test.cpp` (both new)

**Interfaces:**
- Consumes (C1): `struct TerminalClocks { core::platform::IClock const& steady; core::platform::WallClockRef wall; static TerminalClocks system() noexcept; };` and the `Terminal(Events&, core::Environment const&, std::unique_ptr<vtpty::Pty>, Settings, TerminalClocks clocks, std::chrono::steady_clock::time_point)` constructor.
- Produces (additions):
  - `using LocalTimeConverter = std::chrono::local_seconds (*)(std::chrono::system_clock::time_point) noexcept;`
  - `[[nodiscard]] std::chrono::local_seconds systemLocalTime(std::chrono::system_clock::time_point when) noexcept;`
  - `LocalTimeConverter TerminalClocks::localTime = &systemLocalTime;`
  - `[[nodiscard]] std::chrono::local_seconds Terminal::localTimeOf(std::chrono::system_clock::time_point when) const noexcept;`
  - In `Gutter.hpp`: `DefaultTimestampFormat`, `MaxTimestampColumns`, `enum class TimestampFormatError : uint8_t { Empty = 0, Malformed, TooWide };`, `[[nodiscard]] std::string_view timestampFormatErrorText(TimestampFormatError) noexcept;`, `[[nodiscard]] std::expected<int, TimestampFormatError> measureTimestampFormat(std::string_view format);`, `[[nodiscard]] std::string_view effectiveTimestampFormat(std::string_view requested);`, `[[nodiscard]] std::string formatGutterTimestamp(std::string_view format, std::chrono::local_seconds when);`, `class TimestampRun { explicit TimestampRun(std::string_view format); [[nodiscard]] std::string next(std::optional<std::chrono::local_seconds> when); };`

- [ ] **Step 1: Write the failing pure tests**

Create `src/vtbackend/screen/Gutter_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Gutter.hpp>
#include <vtbackend/screen/LocalTime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace
{
/// 14:03:07 local civil time on an ordinary day -- a local_seconds, so no time zone is consulted.
constexpr auto Afternoon = chrono::local_days { chrono::year { 2026 } / chrono::October / 9 } + 14h + 3min + 7s;
} // namespace

// {{{ timestamps
TEST_CASE("Gutter.timestamps.formatLocalTimeWithTheConfiguredSpec", "[gutter]")
{
    CHECK(formatGutterTimestamp("%H:%M:%S", Afternoon) == "14:03:07");
    CHECK(formatGutterTimestamp("%H:%M", Afternoon) == "14:03");
    // A specification the formatter rejects spells nothing rather than throwing into the render pass.
    CHECK(formatGutterTimestamp("%K", Afternoon).empty());
}

TEST_CASE("Gutter.timestamps.measuringIsValidating", "[gutter]")
{
    CHECK(measureTimestampFormat("%H:%M:%S") == 8);
    CHECK(measureTimestampFormat("%H:%M") == 5);
    // Measured at the longest weekday, so a %A gutter is wide enough on every day of the week.
    CHECK(measureTimestampFormat("%A") == 9);

    CHECK(measureTimestampFormat("") == std::unexpected { TimestampFormatError::Empty });
    CHECK(measureTimestampFormat("%K") == std::unexpected { TimestampFormatError::Malformed });
    CHECK(measureTimestampFormat("%H}") == std::unexpected { TimestampFormatError::Malformed });
    CHECK(measureTimestampFormat("%H%n") == std::unexpected { TimestampFormatError::Malformed });
    CHECK(measureTimestampFormat("%A %A %A %A") == std::unexpected { TimestampFormatError::TooWide });
}

TEST_CASE("Gutter.timestamps.anUnusableFormatFallsBackToTheDefault", "[gutter]")
{
    CHECK(effectiveTimestampFormat("%H:%M") == "%H:%M");
    CHECK(effectiveTimestampFormat("%K") == DefaultTimestampFormat);
    CHECK(effectiveTimestampFormat("") == DefaultTimestampFormat);
}

TEST_CASE("Gutter.timestamps.aRowShowsItsTimeOnlyWhenItDiffersFromTheRowAbove", "[gutter]")
{
    auto run = TimestampRun { "%H:%M:%S" };
    CHECK(run.next(Afternoon) == "14:03:07");
    CHECK(run.next(Afternoon).empty()); // the same second as the row above
    CHECK(run.next(Afternoon + 1s) == "14:03:08");
    CHECK(run.next(std::nullopt).empty());         // a row the cursor never reached
    CHECK(run.next(Afternoon + 1s) == "14:03:08"); // the row above showed nothing to repeat

    // The SPELLED text is what is compared: at minute resolution, seconds apart read the same.
    auto minutes = TimestampRun { "%H:%M" };
    CHECK(minutes.next(Afternoon) == "14:03");
    CHECK(minutes.next(Afternoon + 30s).empty());
    CHECK(minutes.next(Afternoon + 60s) == "14:04");
}

TEST_CASE("Gutter.timestamps.theSystemZoneMovesWholeMinutesOnly", "[gutter]")
{
    // The production converter reads the process's own zone, which a test cannot pin -- but every zone
    // in use is a whole number of minutes off UTC, so the seconds survive the conversion everywhere.
    for (auto const instant: { chrono::sys_days { chrono::year { 2026 } / chrono::January / 15 } + 7h + 8min + 9s,
                               chrono::sys_days { chrono::year { 2026 } / chrono::July / 15 } + 19h + 20min + 21s })
    {
        auto const offset = systemLocalTime(instant).time_since_epoch() - instant.time_since_epoch();
        CHECK(offset % 1min == 0s);
        CHECK(chrono::abs(offset) <= 14h);
    }
}
// }}}
```

- [ ] **Step 2: Write the failing Terminal-level test and its clocked fixture**

Create `src/vtbackend/screen/Terminal_gutter_test.cpp` (later tasks append to it):

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Gutter.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Environment.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace
{
/// 10:00:00 UTC on 2026-10-09 -- the instant every clocked test's session starts at.
constexpr auto SessionStart =
    chrono::system_clock::time_point { chrono::sys_days { chrono::year { 2026 } / chrono::October / 9 } + 10h };

/// Reads an instant's UTC fields as local civil time, so a clocked test spells the same text in every
/// time zone it runs in.
[[nodiscard]] chrono::local_seconds utcAsLocal(chrono::system_clock::time_point when) noexcept
{
    return chrono::local_seconds { chrono::floor<chrono::seconds>(when).time_since_epoch() };
}

/// A Terminal whose clocks the test drives: steady, wall, and the time-zone conversion.
///
/// MockTerm's clocks are manual but start at the epoch, and it converts through the process's own time zone
/// (systemLocalTime) -- wrong for the tests about WHEN a line was written, which need a fixed zone and a real date.
class ClockedTerminal: public Terminal::NullEvents
{
  public:
    explicit ClockedTerminal(Settings const& settings):
        terminal { *this,
                   core::defaultEnvironment(),
                   std::make_unique<vtpty::MockPty>(settings.pageSize),
                   settings,
                   TerminalClocks { .steady = steady, .wall = wall, .localTime = &utcAsLocal },
                   chrono::steady_clock::time_point {} }
    {
    }

    /// Feeds @p text through the PTY and parses it -- one read batch, so one wall-clock sample.
    void write(std::string_view text)
    {
        auto& pty = static_cast<vtpty::MockPty&>(terminal.device());
        pty.appendStdOutBuffer(text);
        while (pty.isStdoutDataAvailable())
            terminal.processInputOnce();
    }

    core::platform::ManualClock steady;
    core::platform::ManualWallClock wall { SessionStart };
    Terminal terminal;
};

/// MockTerm's settings, for a ClockedTerminal of @p pageSize.
[[nodiscard]] Settings clockedSettings(PageSize pageSize)
{
    return MockTerm<>::createSettings(pageSize, HistoryLimits {}, 1024);
}
} // namespace

TEST_CASE("Gutter.terminal.localTimeGoesThroughTheInjectedConverter", "[gutter]")
{
    auto ct = ClockedTerminal { clockedSettings(PageSize { LineCount(5), ColumnCount(20) }) };
    CHECK(ct.terminal.localTimeOf(SessionStart)
          == chrono::local_days { chrono::year { 2026 } / chrono::October / 9 } + 10h);
}
```

In `src/vtbackend/CMakeLists.txt`: in `vtbackend_HEADERS` add `    screen/Gutter.hpp` after `    grid/Grid.hpp` and `    screen/LocalTime.hpp` after `    grid/LineSoA.hpp`; in `vtbackend_SOURCES` add `    screen/Gutter.cpp` after `    grid/Grid.cpp` and `    screen/LocalTime.cpp` after `    grid/LineSoA.cpp`; in the `vtbackend_test` sources add `        screen/Gutter_test.cpp` after `        shell/Folding_test.cpp` and `        screen/Terminal_gutter_test.cpp` after `        screen/Terminal_input_test.cpp`.

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL to compile — `vtbackend/screen/Gutter.hpp` not found.

- [ ] **Step 4: Create the local-time seam**

Create `src/vtbackend/screen/LocalTime.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

namespace vtbackend
{

/// Converts a wall-clock instant to the civil time a person reads where they are.
///
/// The time zone is ambient process state, so it is reached through this seam rather than called: the
/// production code passes @ref systemLocalTime, a test passes a converter with a fixed offset and gets
/// the same text on every machine (@see TerminalClocks::localTime).
using LocalTimeConverter = std::chrono::local_seconds (*)(std::chrono::system_clock::time_point) noexcept;

/// The production converter: the process's own time zone, as the C library reports it.
/// @param when The instant to convert.
/// @return @p when in local civil time, to the second -- or read as UTC if the zone lookup fails.
[[nodiscard]] std::chrono::local_seconds systemLocalTime(std::chrono::system_clock::time_point when) noexcept;

} // namespace vtbackend
```

Create `src/vtbackend/screen/LocalTime.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/LocalTime.hpp>

#include <ctime>

namespace vtbackend
{

std::chrono::local_seconds systemLocalTime(std::chrono::system_clock::time_point when) noexcept
{
    namespace chrono = std::chrono;

    auto const whole = chrono::floor<chrono::seconds>(when);
    auto const asUtc = chrono::local_seconds { whole.time_since_epoch() };
    auto const timeT = chrono::system_clock::to_time_t(whole);
    auto civil = std::tm {};
#if defined(_WIN32)
    if (localtime_s(&civil, &timeT) != 0)
        return asUtc;
#else
    if (localtime_r(&timeT, &civil) == nullptr)
        return asUtc;
#endif
    auto const date = chrono::year { civil.tm_year + 1900 } / chrono::month { static_cast<unsigned>(civil.tm_mon + 1) }
                      / chrono::day { static_cast<unsigned>(civil.tm_mday) };
    return chrono::local_days { date } + chrono::hours { civil.tm_hour } + chrono::minutes { civil.tm_min }
           + chrono::seconds { civil.tm_sec };
}

} // namespace vtbackend
```

In `src/vtbackend/screen/TerminalClocks.hpp` (phase 1's file), add `#include <vtbackend/screen/LocalTime.hpp>` to its includes and this member after `core::platform::WallClockRef wall;`:

```cpp
    /// Converts a wall-clock instant to local civil time for display -- the gutter's timestamps, the
    /// block tooltip. The time zone is ambient process state, so it is injected like the clocks are.
    LocalTimeConverter localTime = &systemLocalTime;
```

(`TerminalClocks` stays an aggregate; `TerminalClocks::system()` and every `TerminalClocks { .steady = …, .wall = … }` keep compiling with the default.)

- [ ] **Step 5: Hand the converter to the terminal**

The converter needs no member of its own: Task 1.7 already stores the whole `TerminalClocks` as `_clocks`, which carries `localTime` from Step 4 on — one copy of one injected dependency.

In `src/vtbackend/screen/Terminal.hpp`, after the `currentTime()` accessor (:843) insert:

```cpp

    /// @p when in local civil time, through the converter the terminal was constructed with
    /// (@see TerminalClocks::localTime).
    [[nodiscard]] std::chrono::local_seconds localTimeOf(std::chrono::system_clock::time_point when) const noexcept
    {
        return _clocks.localTime(when);
    }
```

- [ ] **Step 6: Create `Gutter.hpp`/`Gutter.cpp` with the timestamp section**

Create `src/vtbackend/screen/Gutter.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

// {{{ timestamps (spec §5.4)

/// The timestamp format a gutter falls back to: hours, minutes and seconds, in local time.
inline constexpr std::string_view DefaultTimestampFormat = "%H:%M:%S";

/// The widest a timestamp segment may be, in columns. A format that spells more is a mistake rather
/// than a preference: the gutter takes its width out of the page.
inline constexpr int MaxTimestampColumns = 32;

/// Why a timestamp format cannot be used.
enum class TimestampFormatError : uint8_t
{
    Empty = 0, ///< Nothing to format with.
    Malformed, ///< Not a std::format chrono specification, or it spells a control character.
    TooWide,   ///< It spells more than MaxTimestampColumns columns.
};

/// What to tell a user about @p error, in a log line that already names the setting.
[[nodiscard]] std::string_view timestampFormatErrorText(TimestampFormatError error) noexcept;

/// The number of columns @p format spells a timestamp in, measured by formatting a sample.
///
/// The sample is the widest-spelled instant there is -- a Wednesday in September, at 23:59:59 -- so a
/// format naming the weekday or the month is measured at its longest, and the gutter, whose width
/// never follows its content, is wide enough on every day of the year.
///
/// Formatting is also the validation: it is the only test of what the renderer will do with the string,
/// which a parser of our own would merely guess at.
///
/// @param format A std::format chrono specification without the braces, e.g. "%H:%M:%S".
/// @return The width in columns, or why the format cannot be used.
[[nodiscard]] std::expected<int, TimestampFormatError> measureTimestampFormat(std::string_view format);

/// @p requested when it is usable, DefaultTimestampFormat otherwise.
[[nodiscard]] std::string_view effectiveTimestampFormat(std::string_view requested);

/// @p when, spelled by @p format.
/// @return The text, or empty when @p format cannot spell it.
[[nodiscard]] std::string formatGutterTimestamp(std::string_view format, std::chrono::local_seconds when);

/// Spells the timestamps of a run of rows, top to bottom, leaving out each one that reads the same as
/// the row above -- so a command's output does not repeat the second it was printed in on every line.
///
/// Compares the SPELLED text, not the instants: with "%H:%M" two rows seconds apart read the same, and
/// showing the second would say nothing new.
class TimestampRun
{
  public:
    /// @param format The configured format; an unusable one is replaced by DefaultTimestampFormat.
    explicit TimestampRun(std::string_view format);

    /// The text the next row down shows.
    /// @param when The row's birth time in local civil time; nullopt for a row never stamped.
    /// @return The time, or empty when the row has none or reads like the one above.
    [[nodiscard]] std::string next(std::optional<std::chrono::local_seconds> when);

  private:
    std::string _format;
    std::string _above;
};

// }}}

} // namespace vtbackend
```

Create `src/vtbackend/screen/Gutter.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Gutter.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string>

namespace vtbackend
{

namespace
{
    /// The instant a timestamp format is measured at: the longest-spelled weekday (Wednesday) and month
    /// (September) there are, at the widest time of day.
    constexpr auto TimestampSample =
        std::chrono::local_days { std::chrono::year { 2000 } / std::chrono::September / std::chrono::day { 27 } }
        + std::chrono::hours { 23 } + std::chrono::minutes { 59 } + std::chrono::seconds { 59 };

    /// The top two bits of a UTF-8 continuation byte, and the mask that selects them.
    constexpr auto Utf8ContinuationMask = 0b1100'0000U;
    constexpr auto Utf8ContinuationBits = 0b1000'0000U;

    /// The last C0 control and DEL: what `%n` and `%t` spell, and what a gutter cell must never hold.
    constexpr auto LastC0Control = 0x1FU;
    constexpr auto DeleteControl = 0x7FU;

    /// How many codepoints @p text holds -- the columns a timestamp takes, every codepoint a chrono
    /// specification spells being one column wide.
    [[nodiscard]] int countCodepoints(std::string_view text) noexcept
    {
        return static_cast<int>(std::ranges::count_if(text, [](char ch) {
            return (static_cast<unsigned char>(ch) & Utf8ContinuationMask) != Utf8ContinuationBits;
        }));
    }

    /// Whether @p text holds a C0 control or DEL.
    [[nodiscard]] bool containsControl(std::string_view text) noexcept
    {
        return std::ranges::any_of(text, [](char ch) {
            auto const code = static_cast<unsigned char>(ch);
            return code <= LastC0Control || code == DeleteControl;
        });
    }
} // namespace

std::string_view timestampFormatErrorText(TimestampFormatError error) noexcept
{
    constexpr auto Texts = std::array<std::string_view, 3> {
        "it is empty",
        "it is not a std::format chrono specification, or it spells a control character",
        "it spells more than the gutter has room for",
    };
    static_assert(Texts.size() == static_cast<size_t>(TimestampFormatError::TooWide) + 1);
    return Texts[static_cast<size_t>(error)];
}

std::expected<int, TimestampFormatError> measureTimestampFormat(std::string_view format)
{
    if (format.empty())
        return std::unexpected { TimestampFormatError::Empty };

    auto const sample = formatGutterTimestamp(format, TimestampSample);
    if (sample.empty() || containsControl(sample))
        return std::unexpected { TimestampFormatError::Malformed };

    auto const columns = countCodepoints(sample);
    if (columns > MaxTimestampColumns)
        return std::unexpected { TimestampFormatError::TooWide };
    return columns;
}

std::string_view effectiveTimestampFormat(std::string_view requested)
{
    return measureTimestampFormat(requested) ? requested : DefaultTimestampFormat;
}

std::string formatGutterTimestamp(std::string_view format, std::chrono::local_seconds when)
{
    // std::vformat reports a bad specification by throwing, and the specification is user input -- so
    // this is the boundary where that becomes "nothing to show".
    try
    {
        return std::vformat(std::format("{{:{}}}", format), std::make_format_args(when));
    }
    catch (std::format_error const&)
    {
        return {};
    }
}

TimestampRun::TimestampRun(std::string_view format): _format { effectiveTimestampFormat(format) }
{
}

std::string TimestampRun::next(std::optional<std::chrono::local_seconds> when)
{
    auto const spelled = when ? formatGutterTimestamp(_format, *when) : std::string {};
    if (spelled == _above)
        return {};
    _above = spelled;
    return spelled;
}

} // namespace vtbackend
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[gutter]"`
Expected: PASS (six cases).

- [ ] **Step 8: Format and commit**

```bash
clang-format -i src/vtbackend/screen/LocalTime.hpp src/vtbackend/screen/LocalTime.cpp src/vtbackend/screen/Gutter.hpp src/vtbackend/screen/Gutter.cpp src/vtbackend/screen/Gutter_test.cpp src/vtbackend/screen/Terminal_gutter_test.cpp src/vtbackend/screen/TerminalClocks.hpp src/vtbackend/screen/Terminal.hpp
git add src/vtbackend/screen/LocalTime.hpp src/vtbackend/screen/LocalTime.cpp src/vtbackend/screen/Gutter.hpp src/vtbackend/screen/Gutter.cpp src/vtbackend/screen/Gutter_test.cpp src/vtbackend/screen/Terminal_gutter_test.cpp src/vtbackend/screen/TerminalClocks.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: format gutter timestamps through an injected time zone

The time zone joins the clocks a Terminal is constructed with, so tests
spell the same local time in every zone. Timestamp formats are validated
by formatting a widest-case sample, and a run of rows shows a time only
where it reads differently from the row above.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.6: Gutter settings, the segment table, line numbers — and the `Settings::foldMarkers` migration

**Files:**
- Modify: `src/vtbackend/screen/Gutter.hpp`, `src/vtbackend/screen/Gutter.cpp` (new sections)
- Modify: `src/vtbackend/screen/Settings.hpp` (include; `foldMarkers` at :151-155)
- Modify: `src/vtbackend/screen/Terminal.cpp` (the guard in `fillGutter` at :2654)
- Modify: `src/contour/config/Config.cpp` (`emulationSettings` at :468-469)
- Modify: `src/vtbackend/shell/Folding_test.cpp` (six `settings().foldMarkers` uses)
- Test: `src/vtbackend/screen/Gutter_test.cpp` (append)

**Interfaces:**
- Consumes: Task 4.5's timestamp section.
- Produces (C2): `enum class LineNumberMode : uint8_t { Off = 0, Absolute, Relative, Hybrid };`, `enum class GutterSegmentKind : uint8_t { Timestamp = 0, LineNumber, BlockColumn, Count };`, `struct GutterSettings` (exactly C2's fields and defaults), `struct GutterSegment { GutterSegmentKind kind; int firstColumn; int columns; };`, `struct GutterLayout { std::vector<GutterSegment> segments; int totalColumns = 0; [[nodiscard]] std::optional<GutterSegment> segment(GutterSegmentKind kind) const noexcept; };`, `[[nodiscard]] GutterLayout gutterLayoutFor(GutterSettings const& settings);`, `GutterSettings Settings::gutter {};`
- Produces (additions): `GutterSegment` gets `{}` default initialisers and a defaulted `operator==`; `MinLineNumberWidth = 3`, `MaxLineNumberWidth = 10`; `[[nodiscard]] std::optional<GutterSegmentKind> gutterSegmentAt(GutterLayout const& layout, int column) noexcept;` `[[nodiscard]] uint64_t lineNumberFor(LineNumberMode mode, uint64_t absoluteNumber, int64_t cursorDistance) noexcept;` `[[nodiscard]] std::u32string lineNumberText(uint64_t number, int width);`

`gutterLayoutFor` takes only `GutterSettings` (C2): the fold switch is `GutterSettings::foldMarkers`, so the spec's two-argument form collapses into one.

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/screen/Gutter_test.cpp`:

```cpp
// {{{ layout
TEST_CASE("Gutter.layout.segmentsFollowTheTable", "[gutter]")
{
    auto const layout = gutterLayoutFor(GutterSettings { .foldMarkers = true,
                                                         .exitStatus = true,
                                                         .userMarks = true,
                                                         .lineNumbers = LineNumberMode::Absolute,
                                                         .lineNumberWidth = 6,
                                                         .timestamps = true,
                                                         .timestampFormat = "%H:%M:%S" });

    // Timestamps (eight columns and a gap), then line numbers, then the block column beside the grid.
    REQUIRE(layout.segments.size() == 3);
    CHECK(layout.segments[0]
          == GutterSegment { .kind = GutterSegmentKind::Timestamp, .firstColumn = 0, .columns = 9 });
    CHECK(layout.segments[1]
          == GutterSegment { .kind = GutterSegmentKind::LineNumber, .firstColumn = 9, .columns = 6 });
    CHECK(layout.segments[2]
          == GutterSegment { .kind = GutterSegmentKind::BlockColumn, .firstColumn = 15, .columns = 1 });
    CHECK(layout.totalColumns == 16);
    CHECK(layout.segment(GutterSegmentKind::LineNumber)->firstColumn == 9);
}

TEST_CASE("Gutter.layout.theDefaultIsTheOneColumnItAlwaysWas", "[gutter]")
{
    // contour's defaults (gutterSettingsFor) turn exit_status and user_marks on beside the fold markers, and that is
    // still the one column the fold strip always took -- a user who changes nothing keeps their page width (spec §5.1).
    auto const layout = gutterLayoutFor(GutterSettings { .foldMarkers = true });
    REQUIRE(layout.segments.size() == 1);
    CHECK(layout.segments[0].kind == GutterSegmentKind::BlockColumn);
    CHECK(layout.totalColumns == 1);
}

TEST_CASE("Gutter.layout.anyOfTheBlockColumnsJobsReservesIt", "[gutter]")
{
    auto const off = GutterSettings { .foldMarkers = false, .exitStatus = false, .userMarks = false };
    CHECK(gutterLayoutFor(off).totalColumns == 0);
    CHECK(gutterLayoutFor(off).segments.empty());

    auto foldOnly = off;
    foldOnly.foldMarkers = true;
    auto statusOnly = off;
    statusOnly.exitStatus = true;
    auto marksOnly = off;
    marksOnly.userMarks = true;
    for (auto const& settings: { foldOnly, statusOnly, marksOnly })
        CHECK(gutterLayoutFor(settings).totalColumns == 1);
}

TEST_CASE("Gutter.layout.theLineNumberWidthIsHeldToItsRange", "[gutter]")
{
    auto settings = GutterSettings {
        .foldMarkers = false, .exitStatus = false, .userMarks = false, .lineNumbers = LineNumberMode::Relative
    };
    settings.lineNumberWidth = 1;
    CHECK(gutterLayoutFor(settings).totalColumns == MinLineNumberWidth);
    settings.lineNumberWidth = 40;
    CHECK(gutterLayoutFor(settings).totalColumns == MaxLineNumberWidth);
}

TEST_CASE("Gutter.layout.anUnusableTimestampFormatIsMeasuredAsTheDefault", "[gutter]")
{
    auto const layout = gutterLayoutFor(GutterSettings {
        .foldMarkers = false, .exitStatus = false, .userMarks = false, .timestamps = true, .timestampFormat = "%K" });
    CHECK(layout.totalColumns == 9); // "%H:%M:%S", and the gap
}

TEST_CASE("Gutter.layout.theWidthFollowsTheConfigurationNotTheContent", "[gutter]")
{
    auto settings = GutterSettings { .foldMarkers = true };
    auto const before = gutterLayoutFor(settings).totalColumns;
    settings.lineNumbers = LineNumberMode::Hybrid;
    CHECK(gutterLayoutFor(settings).totalColumns == before + 6);

    // A different format of the same width moves nothing.
    settings.timestamps = true;
    auto const withTimes = gutterLayoutFor(settings).totalColumns;
    settings.timestampFormat = "%M:%H:%S";
    CHECK(gutterLayoutFor(settings).totalColumns == withTimes);
}

TEST_CASE("Gutter.layout.aColumnNamesItsSegment", "[gutter]")
{
    auto const layout = gutterLayoutFor(
        GutterSettings { .foldMarkers = true, .lineNumbers = LineNumberMode::Absolute, .timestamps = true });
    CHECK(gutterSegmentAt(layout, 0) == GutterSegmentKind::Timestamp);
    CHECK(gutterSegmentAt(layout, 8) == GutterSegmentKind::Timestamp);
    CHECK(gutterSegmentAt(layout, 9) == GutterSegmentKind::LineNumber);
    CHECK(gutterSegmentAt(layout, 14) == GutterSegmentKind::LineNumber);
    CHECK(gutterSegmentAt(layout, 15) == GutterSegmentKind::BlockColumn);
    CHECK(gutterSegmentAt(layout, 16) == std::nullopt);
    CHECK(gutterSegmentAt(layout, -1) == std::nullopt);
}
// }}}

// {{{ line numbers
TEST_CASE("Gutter.lineNumbers.followVim", "[gutter]")
{
    // The mode, the row's own number, its distance from the cursor row, and what it shows.
    struct Row
    {
        LineNumberMode mode;
        uint64_t absolute;
        int64_t distance;
        uint64_t shown;
    };
    auto const rows = std::array {
        Row { LineNumberMode::Absolute, 42, 0, 42 }, Row { LineNumberMode::Absolute, 42, 5, 42 },
        Row { LineNumberMode::Relative, 42, 0, 0 },  Row { LineNumberMode::Relative, 42, -5, 5 },
        Row { LineNumberMode::Relative, 42, 7, 7 },  Row { LineNumberMode::Hybrid, 42, 0, 42 },
        Row { LineNumberMode::Hybrid, 42, -3, 3 },
    };
    for (auto const& row: rows)
    {
        CAPTURE(static_cast<int>(row.mode), row.absolute, row.distance);
        CHECK(lineNumberFor(row.mode, row.absolute, row.distance) == row.shown);
    }
}

TEST_CASE("Gutter.lineNumbers.areRightAlignedAndKeepTheirTail", "[gutter]")
{
    CHECK(lineNumberText(42, 6) == U"    42");
    CHECK(lineNumberText(7, 3) == U"  7");
    CHECK(lineNumberText(123456, 6) == U"123456");
    // Too wide: the last width-1 digits, behind an ellipsis -- the tail is what tells nearby rows apart.
    CHECK(lineNumberText(1234567, 6) == U"…34567");
    CHECK(lineNumberText(1000, 3) == U"…00");
}
// }}}
```

Add `#include <array>` to the test file's standard includes.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL to compile — `'GutterSettings': undeclared identifier`.

- [ ] **Step 3: Declare the settings, the layout and the line-number helpers**

In `src/vtbackend/screen/Gutter.hpp`, insert before the closing `} // namespace vtbackend`:

```cpp
// {{{ settings and layout (spec §5.1)

/// Which line numbers the gutter shows, matching vim's `nu` / `rnu` combinations (spec §5.3).
enum class LineNumberMode : uint8_t
{
    Off = 0,  ///< `nonu nornu`: none.
    Absolute, ///< `nu`: every row's own number, counted from the session's first row.
    Relative, ///< `rnu`: every row's distance from the cursor row; the cursor row reads 0.
    Hybrid,   ///< `nu rnu`: the cursor row's own number, every other row's distance.
};

/// The gutter's segments, left to right.
enum class GutterSegmentKind : uint8_t
{
    Timestamp = 0, ///< The time each row was first reached.
    LineNumber,    ///< The row's number, or its distance from the cursor.
    BlockColumn,   ///< Fold controls, exit status and user marks.
    Count,         ///< Not a segment; pins the tables keyed by kind.
};

/// The narrowest and widest line-number segment `gutter.line_number_width` may ask for.
inline constexpr int MinLineNumberWidth = 3;
inline constexpr int MaxLineNumberWidth = 10;

/// What the gutter shows (spec §5): which segments it has and what each draws.
///
/// Plain bools mirroring the YAML keys they come from one to one -- `folding.show_markers`,
/// `gutter.exit_status`, `gutter.user_marks`, `gutter.timestamps` -- the configuration carve-out in
/// AGENT.md, carried into vtbackend unchanged. They are read only as table rows, by gutterLayoutFor()
/// and blockColumnGlyph(); nothing else branches on them.
struct GutterSettings
{
    // Every switch defaults OFF here, so a default-constructed vtbackend::Settings reserves no gutter --
    // exactly what embedders and the existing render tests got before this phase. The user-facing defaults
    // (exit status and user marks on) live in contour::config::GutterConfig and reach here through
    // gutterSettingsFor().
    bool foldMarkers = false;                                             ///< The block column draws fold controls.
    bool exitStatus = false;                                              ///< ...colours by outcome, and draws ▸ / ●.
    bool userMarks = false;                                               ///< ...draws ◆ on rows marked with vi `mm`.
    LineNumberMode lineNumbers = LineNumberMode::Off;                     ///< Which numbers, if any.
    uint8_t lineNumberWidth = 6;                                          ///< Columns the numbers take, 3..10.
    bool timestamps = false;                                              ///< Show when each row was first reached.
    std::string timestampFormat = std::string { DefaultTimestampFormat }; ///< std::format chrono spec, local time.

    bool operator==(GutterSettings const&) const = default;
};

/// One segment's place in the gutter. Columns count from the gutter's left edge.
struct GutterSegment
{
    GutterSegmentKind kind {}; ///< What it shows.
    int firstColumn {};        ///< Its leftmost column; 0 is the gutter's left edge.
    int columns {};            ///< How many columns it takes.

    bool operator==(GutterSegment const&) const = default;
};

/// The gutter's segments in drawing order, and the width they add up to.
///
/// THE description of the strip: the window geometry reserves totalColumns cells for it, the hit-test maps
/// a column back to a segment through it, and Terminal::fillGutter paints through it -- so the width
/// reserved and the width drawn cannot disagree.
struct GutterLayout
{
    std::vector<GutterSegment> segments; ///< Left to right; only the segments that are present.
    int totalColumns = 0;                ///< The sum of their widths; 0 means there is no gutter.

    /// The segment of @p kind, when the layout has one.
    [[nodiscard]] std::optional<GutterSegment> segment(GutterSegmentKind kind) const noexcept;
};

/// The layout @p settings ask for (spec §5.1).
///
/// Constant for a given configuration, never a function of content: a gutter that widened with its line
/// numbers would resize the page, and reflow it, in the middle of a command's output.
/// @param settings What the gutter shows.
/// @return Its segments, left to right, and their total width.
[[nodiscard]] GutterLayout gutterLayoutFor(GutterSettings const& settings);

/// The segment under gutter column @p column (0 = the gutter's left edge), for the hit-test.
/// @return Its kind, or nullopt when @p column is outside the gutter.
[[nodiscard]] std::optional<GutterSegmentKind> gutterSegmentAt(GutterLayout const& layout, int column) noexcept;

// }}}

// {{{ line numbers (spec §5.3)

/// What a row's line number reads under @p mode.
///
/// @param mode Which numbers the gutter shows. Off never reaches here: there is no segment to fill.
/// @param absoluteNumber The row's own number -- Grid::evictedRowCount() plus its distance from the
///                       addressable top, plus one.
/// @param cursorDistance The signed VISIBLE-row distance from the cursor row (Terminal::visibleDistance),
///                       so a collapsed fold counts as one row -- exactly what `j` / `k` count.
/// @return The number to show.
[[nodiscard]] uint64_t lineNumberFor(LineNumberMode mode, uint64_t absoluteNumber, int64_t cursorDistance) noexcept;

/// @p number, right-aligned in exactly @p width columns.
///
/// A number wider than that keeps its LAST width - 1 digits behind a `…`: the tail tells two nearby rows
/// apart, while the head is the same for a screenful of them.
[[nodiscard]] std::u32string lineNumberText(uint64_t number, int width);

// }}}
```

- [ ] **Step 4: Implement them as tables**

In `src/vtbackend/screen/Gutter.cpp`, insert before the closing `} // namespace vtbackend`:

```cpp
namespace
{
    /// One blank column after the timestamps, so a right-aligned line number that fills its own width
    /// never runs into the time beside it.
    constexpr auto TimestampGap = 1;

    [[nodiscard]] int timestampColumns(GutterSettings const& settings)
    {
        if (!settings.timestamps)
            return 0;
        return measureTimestampFormat(effectiveTimestampFormat(settings.timestampFormat)).value_or(0)
               + TimestampGap;
    }

    [[nodiscard]] int lineNumberColumns(GutterSettings const& settings)
    {
        if (settings.lineNumbers == LineNumberMode::Off)
            return 0;
        return std::clamp(static_cast<int>(settings.lineNumberWidth), MinLineNumberWidth, MaxLineNumberWidth);
    }

    [[nodiscard]] int blockColumnColumns(GutterSettings const& settings)
    {
        // One column whenever any of its three jobs is on. contour's defaults turn exit status and user marks on,
        // and fold markers alone already reserved this column, so a user who changes nothing keeps the width they had.
        return settings.foldMarkers || settings.exitStatus || settings.userMarks ? 1 : 0;
    }

    /// One row of the gutter's table: a segment, and how wide the settings make it.
    struct SegmentRule
    {
        GutterSegmentKind kind;
        int (*columns)(GutterSettings const&);
    };

    /// THE gutter, left to right (spec §5.1). A fourth segment is a row here, and a field of the row
    /// Terminal::fillGutter hands the painter.
    constexpr auto SegmentRules = std::array {
        SegmentRule { .kind = GutterSegmentKind::Timestamp, .columns = &timestampColumns },
        SegmentRule { .kind = GutterSegmentKind::LineNumber, .columns = &lineNumberColumns },
        SegmentRule { .kind = GutterSegmentKind::BlockColumn, .columns = &blockColumnColumns },
    };
    static_assert(SegmentRules.size() == static_cast<size_t>(GutterSegmentKind::Count));

    /// Whether every rule sits at its kind's index, which is what lets a rule be looked up by kind.
    [[nodiscard]] consteval bool segmentRulesAreIndexedByKind()
    {
        auto index = size_t { 0 };
        for (auto const& rule: SegmentRules)
        {
            if (static_cast<size_t>(rule.kind) != index)
                return false;
            ++index;
        }
        return true;
    }
    static_assert(segmentRulesAreIndexedByKind());

    /// Where a row's line number comes from.
    enum class LineNumberSource : uint8_t
    {
        Absolute = 0, ///< The row's own number.
        Distance,     ///< Its distance from the cursor row.
    };

    /// One row of vim's numbering table (spec §5.3): what the cursor row and every other row show.
    struct LineNumberRule
    {
        LineNumberMode mode;
        LineNumberSource cursorRow;
        LineNumberSource otherRows;
    };

    constexpr auto LineNumberRules = std::array {
        LineNumberRule { .mode = LineNumberMode::Off,
                         .cursorRow = LineNumberSource::Absolute,
                         .otherRows = LineNumberSource::Absolute },
        LineNumberRule { .mode = LineNumberMode::Absolute,
                         .cursorRow = LineNumberSource::Absolute,
                         .otherRows = LineNumberSource::Absolute },
        LineNumberRule { .mode = LineNumberMode::Relative,
                         .cursorRow = LineNumberSource::Distance,
                         .otherRows = LineNumberSource::Distance },
        LineNumberRule { .mode = LineNumberMode::Hybrid,
                         .cursorRow = LineNumberSource::Absolute,
                         .otherRows = LineNumberSource::Distance },
    };
} // namespace

std::optional<GutterSegment> GutterLayout::segment(GutterSegmentKind kind) const noexcept
{
    auto const found = std::ranges::find(segments, kind, &GutterSegment::kind);
    if (found == segments.end())
        return std::nullopt;
    return *found;
}

GutterLayout gutterLayoutFor(GutterSettings const& settings)
{
    auto layout = GutterLayout {};
    for (auto const& rule: SegmentRules)
    {
        auto const columns = rule.columns(settings);
        if (columns <= 0)
            continue;
        layout.segments.push_back(
            GutterSegment { .kind = rule.kind, .firstColumn = layout.totalColumns, .columns = columns });
        layout.totalColumns += columns;
    }
    return layout;
}

std::optional<GutterSegmentKind> gutterSegmentAt(GutterLayout const& layout, int column) noexcept
{
    auto const found = std::ranges::find_if(layout.segments, [column](GutterSegment const& segment) {
        return column >= segment.firstColumn && column < segment.firstColumn + segment.columns;
    });
    if (found == layout.segments.end())
        return std::nullopt;
    return found->kind;
}

uint64_t lineNumberFor(LineNumberMode mode, uint64_t absoluteNumber, int64_t cursorDistance) noexcept
{
    auto const rule = std::ranges::find(LineNumberRules, mode, &LineNumberRule::mode);
    if (rule == LineNumberRules.end())
        return absoluteNumber;
    auto const source = cursorDistance == 0 ? rule->cursorRow : rule->otherRows;
    if (source == LineNumberSource::Absolute)
        return absoluteNumber;
    return static_cast<uint64_t>(cursorDistance < 0 ? -cursorDistance : cursorDistance);
}

std::u32string lineNumberText(uint64_t number, int width)
{
    auto const digits = std::format("{}", number);
    auto const columns = static_cast<size_t>(std::max(width, 2));
    if (digits.size() <= columns)
        return std::u32string(columns - digits.size(), U' ') + std::u32string(digits.begin(), digits.end());

    auto const tail = std::string_view { digits }.substr(digits.size() - (columns - 1));
    return U"…" + std::u32string(tail.begin(), tail.end());
}
```

- [ ] **Step 5: Migrate `Settings::foldMarkers` to `Settings::gutter`**

In `src/vtbackend/screen/Settings.hpp`, add `#include <vtbackend/screen/Gutter.hpp>` after `#include <vtbackend/input/InputGenerator.hpp> // Modifier`, and replace:

```cpp
    /// Whether a gutter column is reserved left of the grid and a fold marker drawn in it on every
    /// foldable prompt line. The window geometry must agree: the gutter's width is taken out of the
    /// space available to cells, so a page fitted with one and a renderer drawing without it (or vice
    /// versa) put the grid in two different places.
    bool foldMarkers = false;
```

with:

```cpp
    /// What the strip left of the grid shows (@see GutterSettings). The window geometry must agree: the
    /// gutter's width -- gutterLayoutFor(gutter).totalColumns cells -- is taken out of the space available
    /// to cells, so a page fitted for one layout and a renderer drawing another put the grid in two
    /// different places.
    GutterSettings gutter {};
```

In `src/vtbackend/screen/Terminal.cpp` (`fillGutter`), replace:

```cpp
    if (!_settings.foldMarkers || !foldingAppliesToDisplayedPage())
```

with:

```cpp
    if (!_settings.gutter.foldMarkers || !foldingAppliesToDisplayedPage())
```

In `src/contour/config/Config.cpp` (`emulationSettings`), replace:

```cpp
    auto const& folding = config.folding.value();
    settings.foldMarkers = folding.markersVisible();
```

with:

```cpp
    auto const& folding = config.folding.value();
    // Until the `gutter` section is read, the block column exists exactly where the fold column did, so
    // the page the window fits and the strip the terminal draws keep agreeing.
    auto const blockColumn = folding.markersVisible();
    settings.gutter = vtbackend::GutterSettings { .foldMarkers = blockColumn,
                                                  .exitStatus = blockColumn,
                                                  .userMarks = blockColumn };
```

Then update the tests:

Run: `sed -i 's/settings()\.foldMarkers/settings().gutter.foldMarkers/g' src/vtbackend/shell/Folding_test.cpp`
Run: `git grep -n "foldMarkers" -- src`
Expected: only `GutterSettings::foldMarkers`, its uses through `.gutter.foldMarkers` / `settings.gutter`, and `blockColumnColumns` — no bare `Settings::foldMarkers` left.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test contour_gui_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[gutter]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[folding]"`
Expected: PASS — the new `[gutter]` cases, and `[folding]` unchanged (only the field moved).

- [ ] **Step 7: Format and commit**

```bash
clang-format -i src/vtbackend/screen/Gutter.hpp src/vtbackend/screen/Gutter.cpp src/vtbackend/screen/Gutter_test.cpp src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.cpp src/contour/config/Config.cpp src/vtbackend/shell/Folding_test.cpp
git add src/vtbackend/screen/Gutter.hpp src/vtbackend/screen/Gutter.cpp src/vtbackend/screen/Gutter_test.cpp src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.cpp src/contour/config/Config.cpp src/vtbackend/shell/Folding_test.cpp
git commit -F - <<'EOF'
vtbackend: describe the gutter as a table of segments

GutterSettings replaces Settings::foldMarkers, and one table turns it into
a layout -- timestamps, line numbers, block column -- whose width is fixed
by the configuration. Line numbers follow vim's nu/rnu table: absolute,
relative and hybrid, right-aligned, keeping the tail when too wide.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.7: The block column, its colours, the painter and the fold label — as pure units

**Files:**
- Modify: `src/vtbackend/screen/Gutter.hpp`, `src/vtbackend/screen/Gutter.cpp` (new sections)
- Test: `src/vtbackend/screen/Gutter_test.cpp` (append)

**Interfaces:**
- Consumes: C1 `CommandBlockRecord`, `CommandBlockState`; C2 `ColorPalette::blockStatusColor()`, `blockStatusSuccess` (Task 4.4); `FoldMarker`, `foldMarkerGlyph()` (`shell/Folding.hpp`); `RenderGutterCell::column`, `RenderAnnotation` (Task 4.2).
- Produces (additions, all in `Gutter.hpp`):
  - `enum class BlockColumnGlyph : uint8_t { None = 0, FoldExpanded, FoldCollapsed, FoldBody, FoldBodyEnd, UserMark, Running, Finished, Count };`
  - `enum class BlockHeadState : uint8_t { None = 0, Running, Finished };`, `enum class UserMarkPresence : uint8_t { Absent = 0, Present };`, `enum class GutterHover : uint8_t { Rest = 0, Hovered };`
  - `struct BlockColumnFacts { FoldMarker fold; BlockHeadState head; UserMarkPresence mark; };`
  - `[[nodiscard]] BlockColumnGlyph blockColumnGlyph(BlockColumnFacts const& facts, GutterSettings const& settings) noexcept;`
  - `[[nodiscard]] char32_t blockColumnCodepoint(BlockColumnGlyph glyph) noexcept;`
  - `[[nodiscard]] BlockHeadState blockHeadStateOf(CommandBlockRecord const* record, int64_t stableId, uint64_t stableIdGeneration) noexcept;`
  - `inline constexpr float BlockStatusHoverLift = 0.5F;` and `[[nodiscard]] RGBColorPair blockColumnColors(ColorPalette const& palette, BlockColumnGlyph glyph, std::optional<CommandBlockOutcome> outcome, GutterHover hover) noexcept;`
  - `struct GutterRow { LineOffset screenLine; std::string timestamp; std::u32string lineNumber; BlockColumnGlyph glyph; RGBColorPair glyphColors; };` and `void appendGutterCells(GutterLayout const& layout, GutterRow const& row, RGBColorPair textColors, std::vector<RenderGutterCell>& output);`
  - `[[nodiscard]] std::u32string collapsedFoldLabel(uint64_t hiddenLines);` and `[[nodiscard]] std::optional<RenderAnnotation> foldLabelAnnotation(LineOffset screenLine, ColumnCount textColumns, ColumnCount pageColumns, std::u32string label, RenderAttributes attributes);`

- [ ] **Step 1: Write the failing tests**

Add `#include <vtbackend/core/ColorPalette.hpp>`, `#include <vtbackend/render/RenderBuffer.hpp>` and `#include <vtbackend/shell/CommandBlock.hpp>` to `Gutter_test.cpp`'s vtbackend includes and `#include <utility>` to its standard includes, then append:

```cpp
// {{{ block column
TEST_CASE("Gutter.blockColumn.followsThePrecedenceTable", "[gutter]")
{
    auto const everything = GutterSettings { .foldMarkers = true, .exitStatus = true, .userMarks = true };

    struct Row
    {
        char const* name;
        BlockColumnFacts facts;
        BlockColumnGlyph glyph;
    };
    auto const rows = std::array {
        Row { "nothing", {}, BlockColumnGlyph::None },
        Row { "expanded head",
              { .fold = FoldMarker::Expanded, .head = BlockHeadState::Finished },
              BlockColumnGlyph::FoldExpanded },
        Row { "collapsed head",
              { .fold = FoldMarker::Collapsed, .head = BlockHeadState::Finished },
              BlockColumnGlyph::FoldCollapsed },
        // On a head row the fold control wins: a prompt is already a navigation target.
        Row { "marked head",
              { .fold = FoldMarker::Expanded, .head = BlockHeadState::Finished, .mark = UserMarkPresence::Present },
              BlockColumnGlyph::FoldExpanded },
        // Over a fold body the mark replaces the bar for that row.
        Row { "marked body", { .fold = FoldMarker::Body, .mark = UserMarkPresence::Present }, BlockColumnGlyph::UserMark },
        Row { "marked body end",
              { .fold = FoldMarker::BodyEnd, .mark = UserMarkPresence::Present },
              BlockColumnGlyph::UserMark },
        Row { "body", { .fold = FoldMarker::Body }, BlockColumnGlyph::FoldBody },
        Row { "body end", { .fold = FoldMarker::BodyEnd }, BlockColumnGlyph::FoldBodyEnd },
        Row { "running head", { .head = BlockHeadState::Running }, BlockColumnGlyph::Running },
        // A finished command that printed nothing has no fold to draw; its head says that it finished.
        Row { "finished head, nothing folded", { .head = BlockHeadState::Finished }, BlockColumnGlyph::Finished },
        Row { "marked row outside any block", { .mark = UserMarkPresence::Present }, BlockColumnGlyph::UserMark },
        Row { "marked running head",
              { .head = BlockHeadState::Running, .mark = UserMarkPresence::Present },
              BlockColumnGlyph::Running },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.name);
        CHECK(blockColumnGlyph(row.facts, everything) == row.glyph);
    }
}

TEST_CASE("Gutter.blockColumn.withFoldMarkersOffEveryFinishedHeadShowsTheDot", "[gutter]")
{
    auto const statusOnly = GutterSettings { .foldMarkers = false, .exitStatus = true, .userMarks = true };

    // The block HAS output, and a fold -- it is just not drawn. Its status still must be, or a user who
    // hid the fold column would see the status of exactly the commands that printed nothing.
    CHECK(blockColumnGlyph({ .fold = FoldMarker::Expanded, .head = BlockHeadState::Finished }, statusOnly)
          == BlockColumnGlyph::Finished);
    CHECK(blockColumnGlyph({ .fold = FoldMarker::Collapsed, .head = BlockHeadState::Finished }, statusOnly)
          == BlockColumnGlyph::Finished);
    CHECK(blockColumnGlyph({ .fold = FoldMarker::Body }, statusOnly) == BlockColumnGlyph::None);
}

TEST_CASE("Gutter.blockColumn.withExitStatusOffNoStatusGlyphIsDrawn", "[gutter]")
{
    auto const noStatus = GutterSettings { .foldMarkers = true, .exitStatus = false, .userMarks = true };
    CHECK(blockColumnGlyph({ .head = BlockHeadState::Running }, noStatus) == BlockColumnGlyph::None);
    CHECK(blockColumnGlyph({ .head = BlockHeadState::Finished }, noStatus) == BlockColumnGlyph::None);
    CHECK(blockColumnGlyph({ .fold = FoldMarker::Expanded, .head = BlockHeadState::Finished }, noStatus)
          == BlockColumnGlyph::FoldExpanded);
}

TEST_CASE("Gutter.blockColumn.withUserMarksOffNoDiamondIsDrawn", "[gutter]")
{
    auto const noMarks = GutterSettings { .foldMarkers = true, .exitStatus = true, .userMarks = false };
    CHECK(blockColumnGlyph({ .fold = FoldMarker::Body, .mark = UserMarkPresence::Present }, noMarks)
          == BlockColumnGlyph::FoldBody);
    CHECK(blockColumnGlyph({ .mark = UserMarkPresence::Present }, noMarks) == BlockColumnGlyph::None);
}

TEST_CASE("Gutter.blockColumn.glyphsAreTheirCodepoints", "[gutter]")
{
    CHECK(blockColumnCodepoint(BlockColumnGlyph::None) == U'\0');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldExpanded) == foldMarkerGlyph(FoldMarker::Expanded));
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldCollapsed) == foldMarkerGlyph(FoldMarker::Collapsed));
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldBody) == foldMarkerGlyph(FoldMarker::Body));
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldBodyEnd) == foldMarkerGlyph(FoldMarker::BodyEnd));
    // Drawn by the box-drawing renderer at their Unicode codepoints (Task 4.3).
    CHECK(blockColumnCodepoint(BlockColumnGlyph::UserMark) == U'◆');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::Running) == U'▸');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::Finished) == U'●');
}

TEST_CASE("Gutter.blockColumn.onlyTheRecordedHeadIsAHead", "[gutter]")
{
    auto record = CommandBlockRecord {};
    record.headStableId = 42;
    record.headIdGeneration = 3;

    record.state = CommandBlockState::Running;
    CHECK(blockHeadStateOf(&record, 42, 3) == BlockHeadState::Running);
    CHECK(blockHeadStateOf(&record, 43, 3) == BlockHeadState::None); // a row of its output
    CHECK(blockHeadStateOf(&record, 42, 4) == BlockHeadState::None); // an id another generation minted

    record.state = CommandBlockState::Finished;
    CHECK(blockHeadStateOf(&record, 42, 3) == BlockHeadState::Finished);

    // The prompt being typed at says nothing yet.
    record.state = CommandBlockState::Prompting;
    CHECK(blockHeadStateOf(&record, 42, 3) == BlockHeadState::None);

    CHECK(blockHeadStateOf(nullptr, 42, 3) == BlockHeadState::None);
}
// }}}

// {{{ colours
TEST_CASE("Gutter.colors.anUncolouredSuccessIsTodaysFoldColumn", "[gutter]")
{
    auto const palette = ColorPalette {};
    for (auto const outcome:
         { std::optional<CommandBlockOutcome> {}, std::optional { CommandBlockOutcome::Success } })
    {
        auto const rest = blockColumnColors(palette, BlockColumnGlyph::FoldBody, outcome, GutterHover::Rest);
        CHECK(rest.foreground == palette.foldMarkerColors().foreground);
        CHECK(rest.background == palette.foldMarkerColors().background);

        auto const hovered = blockColumnColors(palette, BlockColumnGlyph::FoldBody, outcome, GutterHover::Hovered);
        CHECK(hovered.foreground == palette.foldMarkerHoverColors().foreground);
        CHECK(hovered.background == palette.foldMarkerHoverColors().background);
    }
}

TEST_CASE("Gutter.colors.anOutcomeTakesItsColourAndIsLiftedWhenHovered", "[gutter]")
{
    auto const palette = ColorPalette {};
    auto const failure = palette.blockStatusColor(CommandBlockOutcome::Failure);

    auto const rest =
        blockColumnColors(palette, BlockColumnGlyph::FoldExpanded, CommandBlockOutcome::Failure, GutterHover::Rest);
    CHECK(rest.foreground == failure);
    CHECK(rest.background == palette.defaultBackground);

    auto const hovered = blockColumnColors(
        palette, BlockColumnGlyph::FoldExpanded, CommandBlockOutcome::Failure, GutterHover::Hovered);
    CHECK(hovered.foreground == mixColor(failure, palette.foldMarkerHoverColors().foreground, BlockStatusHoverLift));
    CHECK(hovered.background == palette.defaultBackground);

    // A success the scheme DOES colour is an outcome like any other.
    auto coloured = ColorPalette {};
    coloured.blockStatusSuccess = RGBColor(0x10, 0x80, 0x10);
    CHECK(blockColumnColors(coloured, BlockColumnGlyph::FoldBody, CommandBlockOutcome::Success, GutterHover::Rest)
              .foreground
          == RGBColor(0x10, 0x80, 0x10));
}

TEST_CASE("Gutter.colors.aUserMarkIsAlwaysAtFullStrength", "[gutter]")
{
    auto const palette = ColorPalette {};
    for (auto const hover: { GutterHover::Rest, GutterHover::Hovered })
        for (auto const outcome:
             { std::optional<CommandBlockOutcome> {}, std::optional { CommandBlockOutcome::Failure } })
            CHECK(blockColumnColors(palette, BlockColumnGlyph::UserMark, outcome, hover).foreground
                  == palette.foldMarkerHoverColors().foreground);
}
// }}}

// {{{ painting
TEST_CASE("Gutter.paint.eachSegmentLandsAtItsOwnNegativeColumns", "[gutter]")
{
    auto const layout = gutterLayoutFor(GutterSettings { .foldMarkers = true,
                                                         .lineNumbers = LineNumberMode::Absolute,
                                                         .lineNumberWidth = 3,
                                                         .timestamps = true,
                                                         .timestampFormat = "%H:%M" });
    // [0..4 time | 5 gap | 6..8 number | 9 block]: column c is drawn at c - 10.
    REQUIRE(layout.totalColumns == 10);

    auto const text = RGBColorPair { .foreground = RGBColor(1, 2, 3), .background = RGBColor(4, 5, 6) };
    auto const glyph = RGBColorPair { .foreground = RGBColor(7, 8, 9), .background = RGBColor(4, 5, 6) };
    auto cells = std::vector<RenderGutterCell> {};
    appendGutterCells(layout,
                      GutterRow { .screenLine = LineOffset(2),
                                  .timestamp = "14:03",
                                  .lineNumber = U" 42",
                                  .glyph = BlockColumnGlyph::Running,
                                  .glyphColors = glyph },
                      text,
                      cells);

    // A blank is no cell: the five characters of the time, the two digits, the glyph.
    REQUIRE(cells.size() == 8);
    auto const at = [&](size_t index) {
        return std::pair { cells[index].codepoint, unbox<int>(cells[index].column) };
    };
    CHECK(at(0) == std::pair { U'1', -10 });
    CHECK(at(4) == std::pair { U'3', -6 });
    CHECK(at(5) == std::pair { U'4', -3 });
    CHECK(at(6) == std::pair { U'2', -2 });
    CHECK(at(7) == std::pair { U'▸', -1 });
    for (auto const& cell: cells)
        CHECK(cell.lineOffset == LineOffset(2));
    CHECK(cells[0].attributes.foregroundColor == RGBColor(1, 2, 3));
    CHECK(cells[7].attributes.foregroundColor == RGBColor(7, 8, 9));
}

TEST_CASE("Gutter.paint.anEmptyRowPaintsNothing", "[gutter]")
{
    auto const layout =
        gutterLayoutFor(GutterSettings { .foldMarkers = true, .lineNumbers = LineNumberMode::Absolute });
    auto cells = std::vector<RenderGutterCell> {};
    appendGutterCells(layout, GutterRow { .screenLine = LineOffset(0) }, RGBColorPair {}, cells);
    CHECK(cells.empty());
}
// }}}

// {{{ fold label
TEST_CASE("Gutter.foldLabel.countsTheHiddenLines", "[gutter]")
{
    CHECK(collapsedFoldLabel(1) == U"⋯ 1 line");
    CHECK(collapsedFoldLabel(12) == U"⋯ 12 lines");
    CHECK(collapsedFoldLabel(1234) == U"⋯ 1,234 lines");
    CHECK(collapsedFoldLabel(1234567) == U"⋯ 1,234,567 lines");
}

TEST_CASE("Gutter.foldLabel.sitsAfterTheTextAndIsClippedAtThePageEdge", "[gutter]")
{
    auto const attributes = RenderAttributes { .foregroundColor = RGBColor(1, 2, 3) };

    auto const label =
        foldLabelAnnotation(LineOffset(4), ColumnCount(5), ColumnCount(40), U"⋯ 3 lines", attributes);
    REQUIRE(label.has_value());
    // One blank column after the row's text.
    CHECK(label->position == CellLocation { .line = LineOffset(4), .column = ColumnOffset(6) });
    CHECK(label->text == U"⋯ 3 lines");
    CHECK(label->attributes.foregroundColor == RGBColor(1, 2, 3));

    // Nine columns left on the page: cut to fit, never wrapped onto the next row.
    auto const clipped =
        foldLabelAnnotation(LineOffset(0), ColumnCount(30), ColumnCount(40), U"⋯ 1,234 lines", attributes);
    REQUIRE(clipped.has_value());
    CHECK(clipped->text == U"⋯ 1,234 l");

    // No room at all: no label.
    CHECK_FALSE(
        foldLabelAnnotation(LineOffset(0), ColumnCount(39), ColumnCount(40), U"⋯ 1 line", attributes).has_value());
}
// }}}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL to compile — `'BlockColumnFacts': undeclared identifier`.

- [ ] **Step 3: Declare the units**

In `src/vtbackend/screen/Gutter.hpp`, add to the include block (vtbackend group first, above the standard headers):

```cpp
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/Folding.hpp>
```

and insert before the closing `} // namespace vtbackend`:

```cpp
// {{{ block column (spec §5.2)

/// What one row of the block column shows. Every glyph is drawn by vtrasterizer's box-drawing renderer,
/// so all of them are one size and weight in every font.
enum class BlockColumnGlyph : uint8_t
{
    None = 0,      ///< Nothing.
    FoldExpanded,  ///< The head of an expanded fold.
    FoldCollapsed, ///< The head of a collapsed fold.
    FoldBody,      ///< A row inside an expanded fold.
    FoldBodyEnd,   ///< The last row of an expanded fold.
    UserMark,      ///< ◆ -- a row the user marked with vi `mm`.
    Running,       ///< ▸ -- the head of a command that has not finished.
    Finished,      ///< ● -- the head of a finished command with no fold drawn beside it.
    Count,         ///< Not a glyph; pins the codepoint table.
};

/// Whether a row heads a command block, and in which state.
enum class BlockHeadState : uint8_t
{
    None = 0, ///< Not a head -- or the head of a prompt still being typed at, which says nothing yet.
    Running,  ///< The head of a command that is running.
    Finished, ///< The head of a command that has finished.
};

/// Whether a row carries the user's own mark (LineFlag::UserMark).
enum class UserMarkPresence : uint8_t
{
    Absent = 0, ///< The row carries no user mark.
    Present,    ///< vi `mm` marked the row.
};

/// Whether the pointer is over the fold a gutter row belongs to.
enum class GutterHover : uint8_t
{
    Rest = 0, ///< The pointer is elsewhere.
    Hovered,  ///< The pointer is over the fold the row belongs to.
};

/// The facts about one row that decide its block-column glyph, gathered by Terminal::fillGutter.
struct BlockColumnFacts
{
    FoldMarker fold = FoldMarker::None;               ///< Which part of a fold's column the row carries.
    BlockHeadState head = BlockHeadState::None;       ///< Whether it heads a running or a finished block.
    UserMarkPresence mark = UserMarkPresence::Absent; ///< Whether the user marked it.

    bool operator==(BlockColumnFacts const&) const = default;
};

/// What the block column shows beside a row with @p facts (spec §5.2), by precedence: a fold head, a user
/// mark on any other row, a fold body, a running command's head, a finished command's head.
///
/// The precedence alone gives the spec's "finished block with no foldable output": a finished head that
/// has a fold drawn shows the fold control, which outranks the dot. With fold markers off no fold is
/// drawn, so every finished head shows the dot.
/// @param facts What is true of the row.
/// @param settings Which of the column's jobs are switched on.
/// @return The glyph, or BlockColumnGlyph::None.
[[nodiscard]] BlockColumnGlyph blockColumnGlyph(BlockColumnFacts const& facts, GutterSettings const& settings) noexcept;

/// The character @p glyph is drawn as; U+0000 for BlockColumnGlyph::None.
[[nodiscard]] char32_t blockColumnCodepoint(BlockColumnGlyph glyph) noexcept;

/// Whether the row with stable id @p stableId heads @p record's block, and in which state.
/// @param record The block the row belongs to (Terminal::commandBlockAt), or nullptr for none.
/// @param stableId The row's stable id.
/// @param stableIdGeneration The grid's Grid::stableIdGeneration(), which the record's head id must match.
/// @return The row's head state; None for a row that is not the recorded head.
[[nodiscard]] BlockHeadState blockHeadStateOf(CommandBlockRecord const* record,
                                              int64_t stableId,
                                              uint64_t stableIdGeneration) noexcept;

/// How far a scheme-set status colour is lifted toward the text colour while its fold is hovered.
inline constexpr float BlockStatusHoverLift = 0.5F;

/// The colours a block-column glyph is drawn in (spec §5.2).
///
/// A success the scheme does not colour -- and every glyph when exit status is off -- is drawn exactly as
/// the fold column always was: faded at rest, full strength hovered. A coloured outcome takes its colour
/// at rest and is lifted halfway toward the text colour hovered, so hovering still says "this is the run
/// a click acts on" without washing the status out. A user mark is full strength always.
/// @param palette The colour scheme.
/// @param glyph The glyph being drawn.
/// @param outcome How the row's block ended; nullopt for no block, or with exit status off.
/// @param hover Whether the pointer is over the row's fold.
/// @return The glyph's foreground and the page's own background.
[[nodiscard]] RGBColorPair blockColumnColors(ColorPalette const& palette,
                                             BlockColumnGlyph glyph,
                                             std::optional<CommandBlockOutcome> outcome,
                                             GutterHover hover) noexcept;

// }}}

// {{{ painting

/// Everything one gutter row shows, gathered by Terminal::fillGutter and painted by appendGutterCells().
struct GutterRow
{
    LineOffset screenLine {};                        ///< The screen row it is drawn beside.
    std::string timestamp;                           ///< The time to show, UTF-8; empty for none.
    std::u32string lineNumber;                       ///< The number, already aligned to its segment; empty for none.
    BlockColumnGlyph glyph = BlockColumnGlyph::None; ///< The block column's glyph.
    RGBColorPair glyphColors {};                     ///< The glyph's colours (@see blockColumnColors).
};

/// Appends the cells @p row is drawn as -- one per non-blank character, each at its own negative column,
/// the gutter's left edge being column -layout.totalColumns.
/// @param layout The gutter's segments.
/// @param row What the row shows; text wider than its segment is cut at the segment's edge.
/// @param textColors The timestamps' and line numbers' colours (@see ColorPalette::gutterTextColor).
/// @param output Receives the cells.
void appendGutterCells(GutterLayout const& layout,
                       GutterRow const& row,
                       RGBColorPair textColors,
                       std::vector<RenderGutterCell>& output);

// }}}

// {{{ collapsed-fold label (spec §5.5)

/// The label a collapsed fold's head row carries: `⋯ 1,234 lines`.
/// @param hiddenLines How many rows the fold hides.
[[nodiscard]] std::u32string collapsedFoldLabel(uint64_t hiddenLines);

/// The annotation that places @p label one blank column after a row's text, clipped at the page edge.
/// @param screenLine The screen row of the fold's head.
/// @param textColumns How many columns of that row hold text (Line::trimmedColumns()).
/// @param pageColumns The page's width.
/// @param label The label (@see collapsedFoldLabel).
/// @param attributes Its colours.
/// @return The annotation, or nullopt when not one character of it fits.
[[nodiscard]] std::optional<RenderAnnotation> foldLabelAnnotation(LineOffset screenLine,
                                                                  ColumnCount textColumns,
                                                                  ColumnCount pageColumns,
                                                                  std::u32string label,
                                                                  RenderAttributes attributes);

// }}}
```

- [ ] **Step 4: Implement them**

In `src/vtbackend/screen/Gutter.cpp`, add `#include <libunicode/convert.h>` after the vtbackend include, and insert before the closing `} // namespace vtbackend`:

```cpp
namespace
{
    /// The block-column glyph each fold marker is drawn as.
    constexpr auto FoldGlyphs = std::array {
        BlockColumnGlyph::None,          // FoldMarker::None
        BlockColumnGlyph::FoldExpanded,  // FoldMarker::Expanded
        BlockColumnGlyph::FoldCollapsed, // FoldMarker::Collapsed
        BlockColumnGlyph::FoldBody,      // FoldMarker::Body
        BlockColumnGlyph::FoldBodyEnd,   // FoldMarker::BodyEnd
    };
    static_assert(FoldGlyphs.size() == static_cast<size_t>(FoldMarker::Count));

    [[nodiscard]] constexpr bool isFoldHead(FoldMarker marker) noexcept
    {
        return marker == FoldMarker::Expanded || marker == FoldMarker::Collapsed;
    }

    /// One step of the block column's precedence: the glyph it claims the row for, or None to pass the row
    /// on to the next step.
    using BlockColumnRule = BlockColumnGlyph (*)(BlockColumnFacts const&, GutterSettings const&) noexcept;

    [[nodiscard]] BlockColumnGlyph foldHeadRule(BlockColumnFacts const& facts,
                                                GutterSettings const& settings) noexcept
    {
        if (!settings.foldMarkers || !isFoldHead(facts.fold))
            return BlockColumnGlyph::None;
        return FoldGlyphs[static_cast<size_t>(facts.fold)];
    }

    [[nodiscard]] BlockColumnGlyph userMarkRule(BlockColumnFacts const& facts,
                                                GutterSettings const& settings) noexcept
    {
        // Never on a head row, fold control drawn or not: a prompt is a navigation target already, and
        // its status is what its column is for.
        if (!settings.userMarks || facts.mark == UserMarkPresence::Absent || facts.head != BlockHeadState::None
            || isFoldHead(facts.fold))
            return BlockColumnGlyph::None;
        return BlockColumnGlyph::UserMark;
    }

    [[nodiscard]] BlockColumnGlyph foldBodyRule(BlockColumnFacts const& facts,
                                                GutterSettings const& settings) noexcept
    {
        if (!settings.foldMarkers || isFoldHead(facts.fold))
            return BlockColumnGlyph::None;
        return FoldGlyphs[static_cast<size_t>(facts.fold)];
    }

    [[nodiscard]] BlockColumnGlyph runningHeadRule(BlockColumnFacts const& facts,
                                                   GutterSettings const& settings) noexcept
    {
        return settings.exitStatus && facts.head == BlockHeadState::Running ? BlockColumnGlyph::Running
                                                                             : BlockColumnGlyph::None;
    }

    [[nodiscard]] BlockColumnGlyph finishedHeadRule(BlockColumnFacts const& facts,
                                                    GutterSettings const& settings) noexcept
    {
        return settings.exitStatus && facts.head == BlockHeadState::Finished ? BlockColumnGlyph::Finished
                                                                              : BlockColumnGlyph::None;
    }

    /// THE block column (spec §5.2), highest precedence first. A sixth glyph is a rule here and a row in
    /// blockColumnCodepoint()'s table.
    constexpr auto BlockColumnPrecedence = std::array<BlockColumnRule, 5> {
        &foldHeadRule, &userMarkRule, &foldBodyRule, &runningHeadRule, &finishedHeadRule,
    };

    /// The blank column between a row's text and its fold label.
    constexpr auto FoldLabelGap = 1;

    /// Digits per thousands group in a fold label's count.
    constexpr auto DigitsPerGroup = size_t { 3 };
} // namespace

BlockColumnGlyph blockColumnGlyph(BlockColumnFacts const& facts, GutterSettings const& settings) noexcept
{
    for (auto const rule: BlockColumnPrecedence)
        if (auto const glyph = rule(facts, settings); glyph != BlockColumnGlyph::None)
            return glyph;
    return BlockColumnGlyph::None;
}

char32_t blockColumnCodepoint(BlockColumnGlyph glyph) noexcept
{
    constexpr auto Codepoints = std::array {
        U'\0',                                  // None
        foldMarkerGlyph(FoldMarker::Expanded),  // FoldExpanded
        foldMarkerGlyph(FoldMarker::Collapsed), // FoldCollapsed
        foldMarkerGlyph(FoldMarker::Body),      // FoldBody
        foldMarkerGlyph(FoldMarker::BodyEnd),   // FoldBodyEnd
        U'◆',                              // UserMark: BLACK DIAMOND
        U'▸',                              // Running:  BLACK RIGHT-POINTING SMALL TRIANGLE
        U'●',                              // Finished: BLACK CIRCLE
    };
    static_assert(Codepoints.size() == static_cast<size_t>(BlockColumnGlyph::Count));
    return Codepoints[static_cast<size_t>(glyph)];
}

BlockHeadState blockHeadStateOf(CommandBlockRecord const* record,
                                int64_t stableId,
                                uint64_t stableIdGeneration) noexcept
{
    if (record == nullptr || record->headStableId != stableId || record->headIdGeneration != stableIdGeneration)
        return BlockHeadState::None;

    switch (record->state)
    {
        case CommandBlockState::Prompting: return BlockHeadState::None;
        case CommandBlockState::Running: return BlockHeadState::Running;
        case CommandBlockState::Finished: return BlockHeadState::Finished;
    }
    return BlockHeadState::None;
}

RGBColorPair blockColumnColors(ColorPalette const& palette,
                               BlockColumnGlyph glyph,
                               std::optional<CommandBlockOutcome> outcome,
                               GutterHover hover) noexcept
{
    auto const fold = hover == GutterHover::Hovered ? palette.foldMarkerHoverColors() : palette.foldMarkerColors();
    auto const fullStrength = palette.foldMarkerHoverColors().foreground;

    // The user's own mark is about the row, not about how its command ended: it stands out the same in
    // every block and in both states, in the colour the scrollbar draws marks in.
    if (glyph == BlockColumnGlyph::UserMark)
        return RGBColorPair { .foreground = fullStrength, .background = fold.background };

    // No status to show, or a success the scheme leaves to the fold column: exactly today's column.
    if (!outcome || (*outcome == CommandBlockOutcome::Success && !palette.blockStatusSuccess))
        return fold;

    auto const status = palette.blockStatusColor(*outcome);
    return RGBColorPair { .foreground = hover == GutterHover::Hovered
                                            ? mixColor(status, fullStrength, BlockStatusHoverLift)
                                            : status,
                          .background = fold.background };
}

void appendGutterCells(GutterLayout const& layout,
                       GutterRow const& row,
                       RGBColorPair textColors,
                       std::vector<RenderGutterCell>& output)
{
    auto const columnOf = [&](int gutterColumn) {
        return ColumnOffset::cast_from(gutterColumn - layout.totalColumns);
    };
    auto const textAttributes = RenderAttributes { .foregroundColor = textColors.foreground,
                                                   .backgroundColor = textColors.background,
                                                   .decorationColor = textColors.foreground };

    auto const appendText = [&](GutterSegmentKind kind, std::u32string_view text) {
        auto const segment = layout.segment(kind);
        if (!segment)
            return;
        auto const fitting = std::min(text.size(), static_cast<size_t>(segment->columns));
        for (auto const index: std::views::iota(size_t { 0 }, fitting))
        {
            // A blank is the page's own background, which is what the strip shows without a cell.
            if (text[index] == U' ')
                continue;
            output.push_back(
                RenderGutterCell { .lineOffset = row.screenLine,
                                   .codepoint = text[index],
                                   .attributes = textAttributes,
                                   .column = columnOf(segment->firstColumn + static_cast<int>(index)) });
        }
    };
    appendText(GutterSegmentKind::Timestamp, unicode::convert_to<char32_t>(std::string_view { row.timestamp }));
    appendText(GutterSegmentKind::LineNumber, row.lineNumber);

    auto const block = layout.segment(GutterSegmentKind::BlockColumn);
    if (!block || row.glyph == BlockColumnGlyph::None)
        return;
    output.push_back(RenderGutterCell { .lineOffset = row.screenLine,
                                        .codepoint = blockColumnCodepoint(row.glyph),
                                        .attributes = RenderAttributes { .foregroundColor = row.glyphColors.foreground,
                                                                         .backgroundColor = row.glyphColors.background,
                                                                         .decorationColor = row.glyphColors.foreground },
                                        .column = columnOf(block->firstColumn) });
}

std::u32string collapsedFoldLabel(uint64_t hiddenLines)
{
    auto const digits = std::format("{}", hiddenLines);
    auto label = std::u32string { U"⋯ " };
    for (auto const index: std::views::iota(size_t { 0 }, digits.size()))
    {
        if (index != 0 && (digits.size() - index) % DigitsPerGroup == 0)
            label += U',';
        label += static_cast<char32_t>(digits[index]);
    }
    label += hiddenLines == 1 ? U" line" : U" lines";
    return label;
}

std::optional<RenderAnnotation> foldLabelAnnotation(LineOffset screenLine,
                                                    ColumnCount textColumns,
                                                    ColumnCount pageColumns,
                                                    std::u32string label,
                                                    RenderAttributes attributes)
{
    auto const first = unbox<int>(textColumns) + FoldLabelGap;
    auto const room = unbox<int>(pageColumns) - first;
    if (room <= 0 || label.empty())
        return std::nullopt;

    if (label.size() > static_cast<size_t>(room))
        label.resize(static_cast<size_t>(room));
    return RenderAnnotation { .position = CellLocation { .line = screenLine, .column = ColumnOffset(first) },
                              .text = std::move(label),
                              .attributes = attributes };
}
```

Add `#include <utility>` and `#include <vector>` to `Gutter.cpp`'s standard includes.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[gutter]"`
Expected: PASS.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i src/vtbackend/screen/Gutter.hpp src/vtbackend/screen/Gutter.cpp src/vtbackend/screen/Gutter_test.cpp
git add src/vtbackend/screen/Gutter.hpp src/vtbackend/screen/Gutter.cpp src/vtbackend/screen/Gutter_test.cpp
git commit -F - <<'EOF'
vtbackend: decide and paint the gutter's block column as pure units

The block column's glyph comes from one precedence table -- fold head,
user mark, fold body, running head, finished head -- and its colour from
the block's outcome. A painter places every segment at its own negative
column, and a collapsed fold gets a clipped size label.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.8: `Terminal::fillGutter` paints the whole gutter; collapsed folds are labelled

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp` (the `fillGutter` declaration at :2997-3001)
- Modify: `src/vtbackend/screen/Terminal.cpp` (`fillRenderBufferInternal` at :685-687; `fillGutter` at :2650-2719)
- Modify: `src/vtbackend/shell/Folding_test.cpp` (`Folding.terminal.noGutterWithoutMarkersOrWithoutFolds`)
- Test: `src/vtbackend/screen/Terminal_gutter_test.cpp` (append)

**Interfaces:**
- Consumes: Tasks 4.1 (`Grid::evictedRowCount()`), 4.5 (`TimestampRun`, `Terminal::localTimeOf()`), 4.6 (layout, line numbers), 4.7 (block column, painter, label); C1 `Terminal::commandBlockAt()`, `Terminal::lineBirthTime()`, `outcomeOf()`, `LineFlag::UserMark`.
- Produces: private `void Terminal::fillFoldLabels(RenderBuffer&, LineOffset) const;`, `[[nodiscard]] LineOffset Terminal::drawnRowLimit() const;`, `[[nodiscard]] LineOffset Terminal::gutterCursorLine() const noexcept;` — no public surface.

- [ ] **Step 1: Write the failing terminal tests**

In `src/vtbackend/screen/Terminal_gutter_test.cpp` add `#include <map>` to the standard includes, then append:

```cpp
namespace
{
/// Starts @p command as a shell with OSC 133 integration would, and leaves it running.
void startCommand(MockTerm<>& mc, std::string_view command)
{
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
    mc.writeToScreen(std::string { command } + "\r\n");
    mc.writeToScreen("\033]133;C\033\\");
}

/// Runs @p command to completion: its output, then the end mark a precmd hook emits with @p exitCode.
void runCommand(MockTerm<>& mc,
                std::string_view command,
                std::vector<std::string> const& output,
                int exitCode = 0)
{
    startCommand(mc, command);
    for (auto const& line: output)
        mc.writeToScreen(line + "\r\n");
    mc.writeToScreen(std::format("\033]133;D;{}\033\\", exitCode));
}

/// A freshly filled render buffer.
[[nodiscard]] RenderBuffer renderOf(Terminal& terminal)
{
    auto buffer = RenderBuffer {};
    terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);
    return buffer;
}

/// The gutter as text, one string per screen row that has any cell: character c of a row is the cell at
/// column c - totalColumns, and a space where there is none.
[[nodiscard]] std::map<int, std::u32string> gutterRows(RenderBuffer const& buffer, int totalColumns)
{
    auto rows = std::map<int, std::u32string> {};
    for (auto const& cell: buffer.gutter)
    {
        auto& row = rows[unbox<int>(cell.lineOffset)];
        row.resize(static_cast<size_t>(totalColumns), U' ');
        row[static_cast<size_t>(unbox<int>(cell.column) + totalColumns)] = cell.codepoint;
    }
    return rows;
}

/// @p prefix followed by the one character @p glyph.
[[nodiscard]] std::u32string withGlyph(std::u32string prefix, char32_t glyph)
{
    prefix.push_back(glyph);
    return prefix;
}
} // namespace

TEST_CASE("Gutter.terminal.aBlockIsColouredByHowItEnded", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true, .exitStatus = true };
    runCommand(mc, "true", { "ok" }, 0);    // head on row 0, output on row 1
    runCommand(mc, "false", { "bad" }, 1); // head on row 2, output on row 3

    auto const buffer = renderOf(mc.terminal);
    auto const& palette = mc.terminal.colorPalette();
    auto const colourOf = [&](int row) {
        auto const cell = std::ranges::find(buffer.gutter, LineOffset(row), &RenderGutterCell::lineOffset);
        REQUIRE(cell != buffer.gutter.end());
        return cell->attributes.foregroundColor;
    };

    // A success the scheme does not colour looks like the fold column always did.
    CHECK(colourOf(0) == palette.foldMarkerColors().foreground);
    CHECK(colourOf(1) == palette.foldMarkerColors().foreground);
    // Every glyph of a failed block takes the failure colour, its body as much as its head.
    CHECK(colourOf(2) == palette.blockStatusColor(CommandBlockOutcome::Failure));
    CHECK(colourOf(3) == palette.blockStatusColor(CommandBlockOutcome::Failure));
}

TEST_CASE("Gutter.terminal.aRunningCommandShowsATriangle", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true, .exitStatus = true };
    startCommand(mc, "sleep 9");
    mc.writeToScreen("still going\r\n");

    auto const buffer = renderOf(mc.terminal);
    // Only the head: a running command has nothing to fold yet.
    REQUIRE(buffer.gutter.size() == 1);
    CHECK(buffer.gutter[0].lineOffset == LineOffset(0));
    CHECK(buffer.gutter[0].column == ColumnOffset(-1));
    CHECK(buffer.gutter[0].codepoint == U'▸');
    CHECK(buffer.gutter[0].attributes.foregroundColor
          == mc.terminal.colorPalette().blockStatusColor(CommandBlockOutcome::Running));
}

TEST_CASE("Gutter.terminal.withFoldMarkersOffAFinishedHeadShowsTheDot", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = false, .exitStatus = true };
    runCommand(mc, "ls", { "file1", "file2" });

    auto const buffer = renderOf(mc.terminal);
    REQUIRE(buffer.gutter.size() == 1);
    CHECK(buffer.gutter[0].lineOffset == LineOffset(0));
    CHECK(buffer.gutter[0].codepoint == U'●');
}

TEST_CASE("Gutter.terminal.aUserMarkShowsADiamondOverTheFoldBody", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true, .userMarks = true };
    runCommand(mc, "ls", { "file1", "file2" });
    mc.terminal.primaryScreen().setLogicalLineFlags(LineOffset(1), LineFlag::UserMark, true);

    auto const rows = gutterRows(renderOf(mc.terminal), 1);
    CHECK(rows.at(0) == withGlyph(U"", foldMarkerGlyph(FoldMarker::Expanded)));
    CHECK(rows.at(1) == U"◆");
    CHECK(rows.at(2) == withGlyph(U"", foldMarkerGlyph(FoldMarker::BodyEnd)));

    mc.terminal.settings().gutter.userMarks = false;
    CHECK(gutterRows(renderOf(mc.terminal), 1).at(1) == withGlyph(U"", foldMarkerGlyph(FoldMarker::Body)));
}

TEST_CASE("Gutter.terminal.absoluteNumbersCountEveryRowTheSessionHad", "[gutter]")
{
    // Three rows of page and two of history: ten rows written leave five evicted.
    auto mc = MockTerm { PageSize { LineCount(3), ColumnCount(10) }, LineCount(2) };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = false,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Absolute,
                                                     .lineNumberWidth = 3 };
    mc.writeToScreen("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10");

    auto const rows = gutterRows(renderOf(mc.terminal), 3);
    CHECK(rows.at(0) == U"  8");
    CHECK(rows.at(1) == U"  9");
    CHECK(rows.at(2) == U" 10");

    // The numbers belong to the rows, so scrolling into the history takes them along.
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(2)));
    auto const scrolled = gutterRows(renderOf(mc.terminal), 3);
    CHECK(scrolled.at(0) == U"  6");
    CHECK(scrolled.at(2) == U"  8");
}

TEST_CASE("Gutter.terminal.relativeNumbersCountVisibleRowsFromTheCursor", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .lineNumbers = LineNumberMode::Relative, .lineNumberWidth = 3 };
    runCommand(mc, "a", { "1", "2", "3" }); // head on row 0, output on rows 1-3
    REQUIRE(mc.terminal.primaryScreen().cursor().position.line == LineOffset(4));

    // Three columns of numbers, then the block column.
    auto const expanded = gutterRows(renderOf(mc.terminal), 4);
    CHECK(expanded.at(0) == withGlyph(U"  4", foldMarkerGlyph(FoldMarker::Expanded)));
    CHECK(expanded.at(3) == withGlyph(U"  1", foldMarkerGlyph(FoldMarker::BodyEnd)));
    CHECK(expanded.at(4) == U"  0 ");

    // Collapsed, the head is ONE visible row above the cursor -- exactly where `1k` lands.
    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);
    auto const collapsed = gutterRows(renderOf(mc.terminal), 4);
    auto const head = std::ranges::find_if(collapsed, [](auto const& row) {
        return row.second.back() == foldMarkerGlyph(FoldMarker::Collapsed);
    });
    REQUIRE(head != collapsed.end());
    CHECK(head->second == withGlyph(U"  1", foldMarkerGlyph(FoldMarker::Collapsed)));
}

TEST_CASE("Gutter.terminal.inViModeTheNumbersFollowTheViCursor", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = false,
                                                     .exitStatus = false,
                                                     .userMarks = false,
                                                     .lineNumbers = LineNumberMode::Hybrid,
                                                     .lineNumberWidth = 3 };
    mc.writeToScreen("a\r\nb\r\nc\r\nd");
    mc.terminal.inputHandler().setMode(ViMode::Normal);
    mc.terminal.moveNormalModeCursorTo(CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });

    // Hybrid: the Vi cursor's row shows its own number, every other row its distance from it.
    auto const rows = gutterRows(renderOf(mc.terminal), 3);
    CHECK(rows.at(0) == U"  1");
    CHECK(rows.at(1) == U"  2");
    CHECK(rows.at(3) == U"  2");
}

TEST_CASE("Gutter.terminal.aRowShowsWhenItWasFirstReached", "[gutter]")
{
    auto settings = clockedSettings(PageSize { LineCount(5), ColumnCount(20) });
    settings.gutter =
        GutterSettings { .foldMarkers = false, .exitStatus = false, .userMarks = false, .timestamps = true };
    auto ct = ClockedTerminal { settings };

    ct.write("a\r\n"); // rows 0 and 1 are reached at 10:00:00
    ct.wall.advance(5s);
    ct.write("b\r\nc"); // row 2 at 10:00:05

    // "%H:%M:%S" and the gap after it.
    auto const rows = gutterRows(renderOf(ct.terminal), 9);
    CHECK(rows.at(0) == U"10:00:00 ");
    CHECK_FALSE(rows.contains(1)); // the same second as the row above: not repeated
    CHECK(rows.at(2) == U"10:00:05 ");
    CHECK_FALSE(rows.contains(3)); // never reached
}

TEST_CASE("Gutter.terminal.theAlternateScreenHasAnEmptyGutter", "[gutter]")
{
    // Review Focus #2: vim, less and htop own the page; their rows have no history to number, no birth
    // time worth showing and no command blocks.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .lineNumbers = LineNumberMode::Absolute, .timestamps = true };
    runCommand(mc, "ls", { "file1", "file2" });
    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    auto const primary = renderOf(mc.terminal);
    REQUIRE_FALSE(primary.gutter.empty());
    REQUIRE(primary.annotations.size() == 1);

    mc.writeToScreen("\033[?1049h");
    auto const alternate = renderOf(mc.terminal);
    CHECK(alternate.gutter.empty());
    CHECK(alternate.annotations.empty());

    // And leaving it gives everything back.
    mc.writeToScreen("\033[?1049l");
    auto const back = renderOf(mc.terminal);
    CHECK(back.gutter.size() == primary.gutter.size());
    CHECK(back.annotations.size() == 1);
}

TEST_CASE("Gutter.terminal.withoutShellIntegrationTheBlockColumnStaysEmpty", "[gutter]")
{
    // Review Focus #5: a shell that emits no OSC 133 gets today's behaviour -- numbers, which need no
    // shell, and an empty block column beside them.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter =
        GutterSettings { .foldMarkers = true, .lineNumbers = LineNumberMode::Absolute, .lineNumberWidth = 3 };
    mc.writeToScreen("just some output\r\nand more\r\n");

    auto const buffer = renderOf(mc.terminal);
    CHECK(mc.terminal.commandBlocks().size() == 0);
    CHECK(std::ranges::none_of(buffer.gutter,
                               [](RenderGutterCell const& cell) { return cell.column == ColumnOffset(-1); }));
    CHECK(gutterRows(buffer, 4).at(0) == U"  1 ");
    CHECK(buffer.annotations.empty());
}

TEST_CASE("Gutter.terminal.aCollapsedFoldIsLabelledWithWhatItHides", "[gutter]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mc.terminal.settings().gutter = GutterSettings { .foldMarkers = true };
    runCommand(mc, "ls", { "file1", "file2", "file3" });
    CHECK(renderOf(mc.terminal).annotations.empty()); // expanded: nothing hidden, nothing to say

    auto const ranges = mc.terminal.foldRanges();
    REQUIRE(ranges.size() == 1);
    mc.terminal.foldState().collapse(ranges[0].headStableId);

    auto const buffer = renderOf(mc.terminal);
    REQUIRE(buffer.annotations.size() == 1);
    auto const head =
        std::ranges::find(buffer.gutter, foldMarkerGlyph(FoldMarker::Collapsed), &RenderGutterCell::codepoint);
    REQUIRE(head != buffer.gutter.end());
    CHECK(buffer.annotations[0].position.line == head->lineOffset);
    CHECK(buffer.annotations[0].position.column == ColumnOffset(5)); // after "$ ls" and one blank
    CHECK(buffer.annotations[0].text == U"⋯ 3 lines");
    CHECK(buffer.annotations[0].attributes.foregroundColor == mc.terminal.colorPalette().gutterTextColor());
    // Never a cell: selection, copying and the accessibility bridge walk the cells and never meet it.
    CHECK(std::ranges::none_of(buffer.cells, [](RenderCell const& cell) { return cell.codepoints == U"⋯"; }));

    // With the fold column hidden, the label is the only sign left that output was folded away.
    mc.terminal.settings().gutter.foldMarkers = false;
    CHECK(renderOf(mc.terminal).annotations.size() == 1);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "Gutter.terminal.*"`
Expected: FAIL — e.g. `aRunningCommandShowsATriangle` reports `buffer.gutter.size() == 1` as `0 == 1` (today's gutter draws folds only), and `absoluteNumbersCountEveryRowTheSessionHad` throws `std::out_of_range` from `rows.at(0)`.

- [ ] **Step 3: Declare the helpers**

In `src/vtbackend/screen/Terminal.hpp`, replace:

```cpp
    /// Fills @p output's gutter with one marker per visible row that a fold hangs off.
    /// @param output The buffer being built.
    /// @param baseLine The screen row the main page starts at -- non-zero with a top status line, and
    ///                 the same shift RenderBufferBuilder applies to every cell it emits.
    void fillGutter(RenderBuffer& output, LineOffset baseLine) const;
```

with:

```cpp
    /// Fills @p output's gutter: per drawn row its timestamp, line number and block-column glyph, each at
    /// its own negative column (@see GutterLayout, appendGutterCells).
    /// @param output The buffer being built.
    /// @param baseLine The screen row the main page starts at -- non-zero with a top status line, and
    ///                 the same shift RenderBufferBuilder applies to every cell it emits.
    void fillGutter(RenderBuffer& output, LineOffset baseLine) const;

    /// Fills @p output's annotations with the `⋯ N lines` label of every collapsed fold on display.
    /// @param output The buffer being built.
    /// @param baseLine As for fillGutter().
    void fillFoldLabels(RenderBuffer& output, LineOffset baseLine) const;

    /// How many screen rows down the main page rows are drawn: the whole page, or fewer where collapsed
    /// folds hide more rows than the history can backfill and the rest of the page is blank.
    [[nodiscard]] LineOffset drawnRowLimit() const;

    /// The row relative line numbers count from: the Vi cursor's in a Vi mode, the terminal cursor's
    /// otherwise (spec §5.3).
    [[nodiscard]] LineOffset gutterCursorLine() const noexcept;
```

- [ ] **Step 4: Rewrite `fillGutter`, add the label pass**

In `src/vtbackend/screen/Terminal.cpp`, add `#include <vtbackend/screen/Gutter.hpp>` to the vtbackend includes (and `#include <ranges>` to the standard ones if absent). In `fillRenderBufferInternal` replace:

```cpp
    // After both branches: the gutter belongs to the main display whichever page it is showing, and
    // fillGutter is a no-op wherever there is nothing foldable to point at.
    fillGutter(output, baseLine);
```

with:

```cpp
    // After both branches: the gutter and the fold labels belong to the main display whichever page it is
    // showing, and each is a no-op on a page folding does not apply to.
    fillGutter(output, baseLine);
    fillFoldLabels(output, baseLine);
```

Replace the whole of `void Terminal::fillGutter(RenderBuffer& output, LineOffset baseLine) const { … }` (:2650-2719) with:

```cpp
LineOffset Terminal::drawnRowLimit() const
{
    // How far down the page the projection actually reaches, which is NOT always a page-full: collapsed
    // folds hiding more rows than the history can backfill leave it short, and the rest of the page is
    // blank. Mapping a blank row anyway extrapolates it onto the last visible line -- every one of them
    // onto the SAME line -- so each would be handed that line's gutter, a column of duplicates hanging
    // below content Grid::render draws nothing on.
    //
    // A count rather than the span it comes from, because the callers' loops re-enter the cache that span
    // points into. An empty projection means nothing is collapsed and the whole page is drawn.
    //
    // The MAIN page, not the total: _settings.pageSize includes the status line, and nothing drawn here
    // belongs beside it.
    auto const pageLines = boxed_cast<LineOffset>(pageSize().lines);
    auto const projectedRows = foldProjection().size();
    return projectedRows == 0 ? pageLines
                              : std::min(pageLines, foldProjectionTopRow() + LineOffset::cast_from(projectedRows));
}

LineOffset Terminal::gutterCursorLine() const noexcept
{
    if (_inputHandler.mode() == ViMode::Insert)
        return primaryScreen().cursor().position.line;
    return _viCommands.cursorPosition.line;
}

void Terminal::fillGutter(RenderBuffer& output, LineOffset baseLine) const
{
    // Built per frame rather than cached: Settings is mutable through settings(), and the layout formats a
    // sample only when timestamps are on.
    auto const layout = gutterLayoutFor(_settings.gutter);

    // The same predicate the projection and the render pass use. On every other page -- the alternate
    // screen of vim, less or htop among them -- the strip stays reserved, its width being the
    // configuration's and never the content's, and stays empty: those rows have no history to number, no
    // birth time worth showing and no command blocks.
    if (layout.totalColumns == 0 || !foldingAppliesToDisplayedPage())
        return;

    auto const& gutter = _settings.gutter;
    auto const& grid = primaryScreen().grid();
    auto const stableBase = grid.stableLineIdOf(LineOffset(0));
    auto const numbers = layout.segment(GutterSegmentKind::LineNumber);
    auto const showTimes = layout.segment(GutterSegmentKind::Timestamp).has_value();
    auto const cursorLine = gutterCursorLine();
    auto timestamps = TimestampRun { gutter.timestampFormat };
    auto const textColors = RGBColorPair { .foreground = _colorPalette.gutterTextColor(),
                                           .background = _colorPalette.defaultBackground };

    // The whole run of the hovered fold lights up, not the one row under the pointer: that run is what a
    // click acts on, so it is what the highlight has to describe.
    auto const hoveredHead = _gutterHoverLine
                                 ? foldCovering(foldRanges(), stableBase + unbox<int64_t>(*_gutterHoverLine))
                                 : std::nullopt;

    for (auto const y: std::views::iota(0, unbox<int>(drawnRowLimit())))
    {
        auto const screenRow = LineOffset(y);
        auto const gridLine = _viewport.translateScreenToGridLine(screenRow);
        auto const id = stableBase + unbox<int64_t>(gridLine);
        auto const& line = grid.lineAt(gridLine);
        auto const* record = commandBlockAt(gridLine);

        // Asked for per row rather than held across the loop: foldRanges() hands back a span over a
        // CACHE, and the translation above re-enters that cache once per row. Holding it survives only as
        // long as no key input can move inside a frame -- one that could, and the remaining rows would be
        // reading freed memory. A cache hit here is three integer comparisons.
        auto const range = foldCovering(foldRanges(), id);

        auto const facts = BlockColumnFacts {
            .fold = range ? markerIn(*range, id) : FoldMarker::None,
            .head = blockHeadStateOf(record, id, grid.stableIdGeneration()),
            .mark = line.isFlagEnabled(LineFlag::UserMark) ? UserMarkPresence::Present : UserMarkPresence::Absent,
        };
        auto const glyph = blockColumnGlyph(facts, gutter);
        auto const outcome =
            record != nullptr && gutter.exitStatus ? std::optional { outcomeOf(*record) } : std::nullopt;
        auto const hover = hoveredHead && range && hoveredHead->headStableId == range->headStableId
                               ? GutterHover::Hovered
                               : GutterHover::Rest;

        // Absolute numbers count every row the session ever had, the evicted ones included; the distance
        // counts VISIBLE rows, so a collapsed fold is one row -- what `j` and `k` count (spec §5.3).
        auto const lineNumber =
            numbers ? lineNumberText(lineNumberFor(gutter.lineNumbers,
                                                   grid.evictedRowCount()
                                                       + static_cast<uint64_t>(
                                                           unbox<int64_t>(gridLine - grid.addressableTop()))
                                                       + 1,
                                                   visibleDistance(cursorLine, gridLine)),
                                     numbers->columns)
                    : std::u32string {};
        auto const birth = showTimes ? lineBirthTime(line).transform([this](auto when) { return localTimeOf(when); })
                                     : std::nullopt;

        appendGutterCells(layout,
                          GutterRow { .screenLine = baseLine + screenRow,
                                      .timestamp = showTimes ? timestamps.next(birth) : std::string {},
                                      .lineNumber = lineNumber,
                                      .glyph = glyph,
                                      .glyphColors = blockColumnColors(_colorPalette, glyph, outcome, hover) },
                          textColors,
                          output.gutter);
    }
}

void Terminal::fillFoldLabels(RenderBuffer& output, LineOffset baseLine) const
{
    // Only a COLLAPSED fold is labelled, and nothing is collapsed in the common case -- which then costs
    // one comparison. Independent of the gutter: with the fold column hidden, the label is the only sign
    // left that output was folded away.
    if (!foldingAppliesToDisplayedPage() || _foldState.empty())
        return;

    auto const& grid = primaryScreen().grid();
    auto const stableBase = grid.stableLineIdOf(LineOffset(0));
    auto const textColor = _colorPalette.gutterTextColor();
    auto const attributes = RenderAttributes { .foregroundColor = textColor,
                                               .backgroundColor = _colorPalette.defaultBackground,
                                               .decorationColor = textColor };

    for (auto const y: std::views::iota(0, unbox<int>(drawnRowLimit())))
    {
        auto const gridLine = _viewport.translateScreenToGridLine(LineOffset(y));
        auto const id = stableBase + unbox<int64_t>(gridLine);
        auto const range = foldCovering(foldRanges(), id);
        if (!range || markerIn(*range, id) != FoldMarker::Collapsed)
            continue;

        auto const hidden = static_cast<uint64_t>(range->lastStableId - range->firstStableId + 1);
        if (auto label = foldLabelAnnotation(baseLine + LineOffset(y),
                                             grid.lineAt(gridLine).trimmedColumns(),
                                             pageSize().columns,
                                             collapsedFoldLabel(hidden),
                                             attributes))
            output.annotations.push_back(std::move(*label));
    }
}
```

- [ ] **Step 5: Keep the folding test's meaning**

The block column now also carries the exit status and user marks (off in the engine, on in contour's configuration); pin every job off so this section keeps meaning 'no block column'. In `src/vtbackend/shell/Folding_test.cpp`, `Folding.terminal.noGutterWithoutMarkersOrWithoutFolds`, section `"markers off"`, replace:

```cpp
        mc.terminal.settings().gutter.foldMarkers = false;
```

with:

```cpp
        // Every job of the block column off, explicitly: with exit status on, a finished head would show
        // its dot (@see Gutter.terminal.withFoldMarkersOffAFinishedHeadShowsTheDot).
        mc.terminal.settings().gutter =
            GutterSettings { .foldMarkers = false, .exitStatus = false, .userMarks = false };
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[gutter]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[folding]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe`
Expected: PASS for all three — the `[folding]` gutter cases unchanged (a successful block's colours are today's, and an expanded or collapsed head outranks the dot).

- [ ] **Step 7: Format and commit**

```bash
clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_gutter_test.cpp src/vtbackend/shell/Folding_test.cpp
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_gutter_test.cpp src/vtbackend/shell/Folding_test.cpp
git commit -F - <<'EOF'
vtbackend: paint timestamps, line numbers and block status in the gutter

fillGutter gathers each drawn row's facts -- fold marker, block head,
user mark, birth time, absolute number and visible distance from the
cursor -- and hands them to the pure painter. Collapsed folds get a
render-only size label. The alternate screen keeps an empty strip.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.9: Vim's fold keys — `za zo zc zM zR`

**Files:**
- Modify: `src/vtbackend/shell/Folding.hpp` (new enum after `FoldJumpBehavior`, ~:254-258)
- Modify: `src/vtbackend/screen/Settings.hpp` (after `foldJumpBehavior` at :175)
- Modify: `src/vtbackend/screen/Terminal.hpp` (after `expandFoldContaining` at :1130), `src/vtbackend/screen/Terminal.cpp` (after `Terminal::expandFoldContaining`)
- Modify: `src/vtbackend/input/vi/ViInputHandler.hpp` (enum after `ViOperator` at :114-122; `Executor` at :158-190), `src/vtbackend/input/vi/ViInputHandler.cpp` (`registerAllCommands`, after the `gH` registration at :154)
- Modify: `src/vtbackend/input/vi/ViCommands.hpp` (after `enterHintMode` at :38), `src/vtbackend/input/vi/ViCommands.cpp` (after `ViCommands::enterHintMode`)
- Modify: `src/contour/config/Config.hpp` (`FoldingConfig`, after `markersVisible()`), `src/contour/config/Config.cpp` (`emulationSettings`)
- Modify: `src/contour/session/TerminalSession.cpp` (`configureTerminal`, end of body)
- Test: `src/vtbackend/shell/Folding_test.cpp` (new section after the Vi normal mode section), `src/contour/config/Config_test.cpp` (append)

**Interfaces:**
- Consumes: `Terminal::toggleFoldContaining`, `expandFoldContaining`, `collapseAllFolds`, `expandAllFolds` (existing).
- Produces (additions): `enum class FoldingAvailability : uint8_t { Disabled = 0, Enabled };` (`Folding.hpp`); `FoldingAvailability Settings::foldingAvailability = FoldingAvailability::Enabled;`; `[[nodiscard]] constexpr vtbackend::FoldingAvailability FoldingConfig::availability() const noexcept;`; `void Terminal::collapseFoldContaining(LineOffset gridLine);`; `enum class ViFoldCommand : uint8_t { Toggle = 0, Open, Close, CloseAll, OpenAll };`; `virtual void ViInputHandler::Executor::fold(ViFoldCommand command) = 0;` and `void ViCommands::fold(ViFoldCommand command) override;`.

The `z` keys are handled inside the engine and never pass `TerminalSession::withFolding()`, which is where `folding.enabled` is honoured today — so the switch has to reach the engine as a setting. `collapseFoldContaining` returns nothing: unlike its siblings, nothing needs to know whether it changed anything.

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/shell/Folding_test.cpp`, after the `// }}}` that closes the Vi normal mode section:

```cpp
// {{{ vim's z fold keys

TEST_CASE("Folding.vi.zaTogglesTheBlockUnderTheCursor", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ false);
    auto const head = mc.terminal.foldRanges()[0].headStableId;
    placeViCursor(mc, LineOffset(2)); // inside the output, not on the prompt

    mc.sendCharSequence("za");
    CHECK(mc.terminal.foldState().isCollapsed(head));
    // The row it stood on is hidden now, so the cursor was moved out of the block.
    CHECK_FALSE(mc.terminal.isLineHiddenByFold(mc.terminal.normalModeCursorPosition().line));

    mc.sendCharSequence("za");
    CHECK_FALSE(mc.terminal.foldState().isCollapsed(head));
}

TEST_CASE("Folding.vi.zoOpensAndZcCloses", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ false);
    auto const head = mc.terminal.foldRanges()[0].headStableId;
    placeViCursor(mc, LineOffset(0));

    mc.sendCharSequence("zo"); // already open
    CHECK_FALSE(mc.terminal.foldState().isCollapsed(head));
    mc.sendCharSequence("zc");
    CHECK(mc.terminal.foldState().isCollapsed(head));
    mc.sendCharSequence("zc"); // already closed: stays closed, where a toggle would have opened it
    CHECK(mc.terminal.foldState().isCollapsed(head));
    mc.sendCharSequence("zo");
    CHECK_FALSE(mc.terminal.foldState().isCollapsed(head));
}

TEST_CASE("Folding.vi.zMClosesEveryBlockAndZROpensThemAll", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ false);
    runCommand(mc, "pwd", { "/tmp" });
    auto const reach = mc.terminal.foldRanges();
    auto const ranges = std::vector<FoldRange>(reach.begin(), reach.end());
    REQUIRE(ranges.size() == 2);

    mc.sendCharSequence("zM");
    for (auto const& range: ranges)
        CHECK(mc.terminal.foldState().isCollapsed(range.headStableId));

    mc.sendCharSequence("zR");
    CHECK(mc.terminal.foldState().empty());
}

TEST_CASE("Folding.vi.zKeysHonourFoldingAvailability", "[folding][vi]")
{
    auto mc = setupViMock(/*collapse*/ false);
    auto const head = mc.terminal.foldRanges()[0].headStableId;
    mc.terminal.settings().foldingAvailability = FoldingAvailability::Disabled;
    placeViCursor(mc, LineOffset(1));

    // Nothing that hides output runs while folding is switched off...
    for (auto const keys: { "za", "zc", "zM" })
    {
        mc.sendCharSequence(keys);
        CHECK_FALSE(mc.terminal.foldState().isCollapsed(head));
    }

    // ...but what was folded while it was on can still be opened, as ExpandAllFolds can.
    mc.terminal.foldState().collapse(head);
    placeViCursor(mc, LineOffset(0));
    mc.sendCharSequence("zo");
    CHECK_FALSE(mc.terminal.foldState().isCollapsed(head));

    mc.terminal.foldState().collapse(head);
    mc.sendCharSequence("zR");
    CHECK(mc.terminal.foldState().empty());
}

// }}}
```

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: folding.enabled reaches the engine as its availability", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    enabled: false
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);

    CHECK(contour::config::FoldingConfig {}.availability() == vtbackend::FoldingAvailability::Enabled);
    CHECK(config.folding.value().availability() == vtbackend::FoldingAvailability::Disabled);
    // The vi `z` keys reach the engine without passing the session's gate, so the engine is told.
    CHECK(contour::config::emulationSettings(config, *profile).foldingAvailability
          == vtbackend::FoldingAvailability::Disabled);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test contour_gui_test`
Expected: FAIL to compile — `'FoldingAvailability': undeclared identifier`.

- [ ] **Step 3: Put the switch where the engine can read it**

In `src/vtbackend/shell/Folding.hpp`, after the closing `};` of `enum class FoldJumpBehavior`, insert:

```cpp

/// Whether output folding is available at all -- `folding.enabled`, converted at the configuration
/// boundary.
///
/// The frontend gates its own fold actions on the configuration; the engine reads this for the requests
/// that reach it directly, vim's `z` keys among them.
enum class FoldingAvailability : uint8_t
{
    Disabled = 0, ///< Nothing folds; what is already folded can still be opened.
    Enabled,      ///< Folding is available.
};
```

In `src/vtbackend/screen/Settings.hpp`, after `FoldJumpBehavior foldJumpBehavior = FoldJumpBehavior::Expand;` insert:

```cpp

    /// Whether folding is available at all (`folding.enabled`). Read where a fold request reaches the
    /// engine without passing the frontend's gate -- vim's `z` keys.
    FoldingAvailability foldingAvailability = FoldingAvailability::Enabled;
```

In `src/contour/config/Config.hpp`, inside `struct FoldingConfig`, after `markersVisible()` insert:

```cpp

    /// `enabled`, as the engine reads it (@see vtbackend::Settings::foldingAvailability).
    [[nodiscard]] constexpr vtbackend::FoldingAvailability availability() const noexcept
    {
        return enabled ? vtbackend::FoldingAvailability::Enabled : vtbackend::FoldingAvailability::Disabled;
    }
```

In `src/contour/config/Config.cpp` (`emulationSettings`), after `settings.foldJumpBehavior = folding.onJumpIntoFold;` insert:

```cpp
    settings.foldingAvailability = folding.availability();
```

In `src/contour/session/TerminalSession.cpp`, at the end of `configureTerminal()` (after `_terminal.settings().momentumScrolling = _profile.momentumScrolling.value();`) insert:

```cpp

    // Re-read on every reload like everything above: the vi `z` keys reach the engine without passing
    // withFolding(), so the engine holds the switch itself.
    _terminal.settings().foldingAvailability = _config.folding.value().availability();
```

- [ ] **Step 4: Add `collapseFoldContaining`**

In `src/vtbackend/screen/Terminal.hpp`, after `bool expandFoldContaining(LineOffset gridLine);` insert:

```cpp

    /// Collapses the fold covering @p gridLine, leaving an already-collapsed one alone -- vim's `zc`,
    /// which unlike a toggle never opens what it is pointed at.
    /// @param gridLine A grid line (0 = page top, negative = into the scrollback).
    void collapseFoldContaining(LineOffset gridLine);
```

In `src/vtbackend/screen/Terminal.cpp`, after the closing brace of `Terminal::expandFoldContaining` insert:

```cpp

void Terminal::collapseFoldContaining(LineOffset gridLine)
{
    auto const range = foldContaining(gridLine);
    if (!range || _foldState.isCollapsed(range->headStableId))
        return;

    _foldState.collapse(range->headStableId);
    onFoldStateChanged();
}
```

- [ ] **Step 5: Name the commands and register the keys**

In `src/vtbackend/input/vi/ViInputHandler.hpp`, after the closing `};` of `enum class ViOperator` insert:

```cpp

/// Vim's fold commands, the ones its `z` prefix names (spec §5.5).
enum class ViFoldCommand : uint8_t
{
    Toggle = 0, ///< `za`: open the block under the cursor if it is closed, close it otherwise.
    Open,       ///< `zo`: open the block under the cursor.
    Close,      ///< `zc`: close the block under the cursor.
    CloseAll,   ///< `zM`: close every block within reach.
    OpenAll,    ///< `zR`: open every block.
};
```

and in `class Executor`, after `virtual void enterHintMode(HintAction action) = 0;` insert:

```cpp

        /// Runs one of vim's `z` fold commands on the block under the Vi cursor, or on all of them.
        virtual void fold(ViFoldCommand command) = 0;
```

In `src/vtbackend/input/vi/ViInputHandler.cpp`, after the line registering `"gH"` insert:

```cpp

    // Vim's fold keys. One row each, so a sixth is a row here and an enumerator in ViFoldCommand.
    auto constexpr FoldMappings = std::array<std::pair<std::string_view, ViFoldCommand>, 5> { {
        { "za", ViFoldCommand::Toggle },
        { "zo", ViFoldCommand::Open },
        { "zc", ViFoldCommand::Close },
        { "zM", ViFoldCommand::CloseAll },
        { "zR", ViFoldCommand::OpenAll },
    } };
    for (auto const& [keys, command]: FoldMappings)
        registerCommand(ModeSelect::Normal, keys, [this, command = command]() { _executor->fold(command); });
```

- [ ] **Step 6: Run them**

In `src/vtbackend/input/vi/ViCommands.hpp`, after `void enterHintMode(HintAction action) override;` insert:

```cpp
    void fold(ViFoldCommand command) override;
```

In `src/vtbackend/input/vi/ViCommands.cpp`, add `#include <algorithm>` and `#include <array>` to the standard includes, and after the closing brace of `ViCommands::enterHintMode` insert:

```cpp

namespace
{
    /// Whether a `z` fold command may run while folding is switched off.
    enum class FoldCommandGate : uint8_t
    {
        Configured = 0, ///< Only while folding is available.
        Always,         ///< Regardless: it can only ever REVEAL output, and switching folding off must
                        ///< still let a user open what was folded while it was on.
    };

    /// One of vim's `z` fold keys: when it may run, and what it does.
    struct FoldCommandRule
    {
        ViFoldCommand command;
        FoldCommandGate gate;
        void (*run)(Terminal& terminal, LineOffset cursorLine);
    };

    /// THE fold keys' behaviour, one row per command.
    constexpr auto FoldCommandRules = std::array {
        FoldCommandRule { .command = ViFoldCommand::Toggle,
                          .gate = FoldCommandGate::Configured,
                          .run = [](Terminal& terminal, LineOffset line) { terminal.toggleFoldContaining(line); } },
        FoldCommandRule { .command = ViFoldCommand::Open,
                          .gate = FoldCommandGate::Always,
                          .run = [](Terminal& terminal, LineOffset line) { terminal.expandFoldContaining(line); } },
        FoldCommandRule { .command = ViFoldCommand::Close,
                          .gate = FoldCommandGate::Configured,
                          .run = [](Terminal& terminal, LineOffset line) { terminal.collapseFoldContaining(line); } },
        FoldCommandRule { .command = ViFoldCommand::CloseAll,
                          .gate = FoldCommandGate::Configured,
                          .run = [](Terminal& terminal, LineOffset) { terminal.collapseAllFolds(); } },
        FoldCommandRule { .command = ViFoldCommand::OpenAll,
                          .gate = FoldCommandGate::Always,
                          .run = [](Terminal& terminal, LineOffset) { terminal.expandAllFolds(); } },
    };
} // namespace

void ViCommands::fold(ViFoldCommand command)
{
    auto const rule = std::ranges::find(FoldCommandRules, command, &FoldCommandRule::command);
    if (rule == FoldCommandRules.end())
        return;

    if (rule->gate == FoldCommandGate::Configured
        && _terminal->settings().foldingAvailability == FoldingAvailability::Disabled)
        return;

    rule->run(*_terminal, cursorPosition.line);

    // Folding moves the scrollable range, and may have moved this cursor out of a block it hid. The
    // frontend republishes both on a screen update, as after every other Vi command here.
    _terminal->screenUpdated();
}
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test contour_gui_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[folding]"` and `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: PASS — the four `Folding.vi.z*` cases, every existing `[folding]` case (`zz` still centres: the trie only gained siblings), and the config case.

- [ ] **Step 8: Format and commit**

```bash
clang-format -i src/vtbackend/shell/Folding.hpp src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/input/vi/ViInputHandler.hpp src/vtbackend/input/vi/ViInputHandler.cpp src/vtbackend/input/vi/ViCommands.hpp src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/shell/Folding_test.cpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession.cpp
git add src/vtbackend/shell/Folding.hpp src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/input/vi/ViInputHandler.hpp src/vtbackend/input/vi/ViInputHandler.cpp src/vtbackend/input/vi/ViCommands.hpp src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/shell/Folding_test.cpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession.cpp
git commit -F - <<'EOF'
vtbackend: give vi mode vim's z fold keys

za, zo, zc, zM and zR act on the block under the vi cursor, or on all
of them, through one table of commands. folding.enabled now reaches the
engine as Settings::foldingAvailability, because these keys never pass
the session's folding gate; opening stays possible while it is off.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.10: Colour-scheme keys `block_status.{success,failure,running}` and `gutter_text`

**Files:**
- Modify: `src/contour/config/ConfigDocumentation.hpp` (literals after `ContextTintEntryConfig` at :1462; `using`s after `using ContextTintEntry` at :2837)
- Modify: `src/contour/config/Config.cpp` (scheme loader after the `fold_marker` block at :1553-1565; `emitColorPaletteBody` after the fold-marker writer at :3920-3933)
- Test: `src/contour/config/Config_test.cpp` (append)

**Interfaces:**
- Consumes (C2): `ColorPalette::blockStatusSuccess`, `blockStatusFailure`, `blockStatusRunning`, `gutterText` (Task 4.4).
- Produces (C9): scheme keys `block_status.success`, `block_status.failure`, `block_status.running`, `gutter_text`; `documentation::BlockStatus`, `BlockStatusHeader`, `BlockStatusSuccess`, `BlockStatusFailure`, `BlockStatusRunning`, `GutterText`, `GutterTextSet`.

One literal per `block_status` key rather than one `"    {}: {}\n"` row as `ContextTintEntry` uses: `YAMLConfigWriter::process(doc, name, val...)` drops its `name`, so a row whose key travels as the first value reaches `std::vformat` one argument short. (That is a live defect in `ContextTintEntry` for a scheme that sets a tint; it is noted for review, not fixed here.)

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: the gutter's status and text colors load and are written back", "[config]")
{
    QTemporaryDir dir;

    auto const schemeOf = [](contour::config::Config const& config) {
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(profile->colors.value()));
        return std::get<contour::config::SimpleColorConfig>(profile->colors.value()).colors;
    };

    SECTION("unset by default, so vtbackend derives them")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: coverage
color_schemes:
    coverage:
        default:
            background: '#102030'
            foreground: '#D0D0D0'
)"sv));
        CHECK_FALSE(scheme.blockStatusSuccess.has_value());
        CHECK_FALSE(scheme.blockStatusFailure.has_value());
        CHECK_FALSE(scheme.blockStatusRunning.has_value());
        CHECK_FALSE(scheme.gutterText.has_value());
    }

    SECTION("every key loads")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: coverage
color_schemes:
    coverage:
        default:
            background: '#102030'
            foreground: '#D0D0D0'
        block_status:
            success: '#20A020'
            failure: '#E02020'
            running: '#E0E020'
        gutter_text: '#606060'
)"sv));
        CHECK(scheme.blockStatusSuccess == vtbackend::RGBColor(0x20, 0xA0, 0x20));
        CHECK(scheme.blockStatusFailure == vtbackend::RGBColor(0xE0, 0x20, 0x20));
        CHECK(scheme.blockStatusRunning == vtbackend::RGBColor(0xE0, 0xE0, 0x20));
        CHECK(scheme.gutterText == vtbackend::RGBColor(0x60, 0x60, 0x60));
    }

    SECTION("one key alone loads, and the others stay unset")
    {
        auto const scheme = schemeOf(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: coverage
color_schemes:
    coverage:
        default:
            background: '#102030'
            foreground: '#D0D0D0'
        block_status:
            failure: '#E02020'
)"sv));
        CHECK_FALSE(scheme.blockStatusSuccess.has_value());
        CHECK(scheme.blockStatusFailure == vtbackend::RGBColor(0xE0, 0x20, 0x20));
        CHECK_FALSE(scheme.blockStatusRunning.has_value());
        CHECK_FALSE(scheme.gutterText.has_value());
    }

    SECTION("the writer emits what was set, and only that")
    {
        auto config = contour::config::Config {};
        auto& scheme = config.colorschemes.value()["default"];
        scheme.blockStatusFailure = vtbackend::RGBColor(0xE0, 0x20, 0x20);
        scheme.gutterText = vtbackend::RGBColor(0x60, 0x60, 0x60);

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("block_status:"));

        auto const reloaded = schemeOf(loadFromYaml(dir, written));
        CHECK(reloaded.blockStatusFailure == vtbackend::RGBColor(0xE0, 0x20, 0x20));
        CHECK(reloaded.gutterText == vtbackend::RGBColor(0x60, 0x60, 0x60));
        // Writing back a derived colour would pin what the scheme deliberately left to the palette.
        CHECK_FALSE(reloaded.blockStatusSuccess.has_value());
        CHECK_FALSE(reloaded.blockStatusRunning.has_value());
    }

    SECTION("the generated default config carries the commented examples")
    {
        auto const generated = contour::config::defaultConfigString();
        CHECK(generated.contains("# block_status:"));
        CHECK(generated.contains("# gutter_text:"));
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `out/build/clangcl-debug/bin/contour_gui_test.exe "Config: the gutter's status and text colors load and are written back"`
Expected: FAIL — section "every key loads" reports `scheme.blockStatusSuccess == …` with an empty optional.

- [ ] **Step 3: Document the keys**

In `src/contour/config/ConfigDocumentation.hpp`, after `constexpr StringLiteral ContextTintEntryConfig { "    {}: {}\n" };` insert:

```cpp

constexpr StringLiteral BlockStatusConfig {
    "\n"
    "{comment} Colors of the gutter's block column by how each command ended: its fold controls, the\n"
    "{comment} triangle beside a running command and the dot beside a finished one all take them.\n"
    "{comment} Unset by default: success then looks exactly like the fold column (fold_marker above),\n"
    "{comment} failure takes this scheme's red (normal color 1) and running its yellow (normal color 3),\n"
    "{comment} so a scheme that has never heard of them still shows them legibly. Uncomment to override.\n"
    "{comment} The failure and running values shown are what the default scheme derives. A success set\n"
    "{comment} here is shown like the other two: in its color at rest, lifted under the pointer.\n"
    "{comment} block_status:\n"
    "{comment}     success: '#7e7c7c'\n"
    "{comment}     failure: '#c63939'\n"
    "{comment}     running: '#a0a000'\n"
};

constexpr StringLiteral BlockStatusHeaderConfig {
    "\n"
    "{comment} Colors of the gutter's block column by how each command ended. Written back because this\n"
    "{comment} scheme sets them; remove a key to derive it again -- success from the fold column, failure\n"
    "{comment} from normal color 1, running from normal color 3.\n"
    "block_status:\n"
};

constexpr StringLiteral BlockStatusSuccessConfig { "    success: {}\n" };
constexpr StringLiteral BlockStatusFailureConfig { "    failure: {}\n" };
constexpr StringLiteral BlockStatusRunningConfig { "    running: {}\n" };

constexpr StringLiteral GutterTextConfig {
    "\n"
    "{comment} Color of the gutter's line numbers and timestamps. Unset by default: this scheme's own\n"
    "{comment} foreground faded toward its background, as quiet as the resting fold column. The value\n"
    "{comment} shown is what the default scheme derives.\n"
    "{comment} gutter_text: '#7e7c7c'\n"
};

constexpr StringLiteral GutterTextSetConfig {
    "\n"
    "{comment} Color of the gutter's line numbers and timestamps. Remove it to derive the color from\n"
    "{comment} this scheme's own foreground and background again.\n"
    "gutter_text: {}\n"
};
```

and after `using ContextTintEntry = DocumentationEntry<ContextTintEntryConfig, Dummy>;` insert:

```cpp
using BlockStatus = DocumentationEntry<BlockStatusConfig, Dummy>;
using BlockStatusHeader = DocumentationEntry<BlockStatusHeaderConfig, Dummy>;
using BlockStatusSuccess = DocumentationEntry<BlockStatusSuccessConfig, Dummy>;
using BlockStatusFailure = DocumentationEntry<BlockStatusFailureConfig, Dummy>;
using BlockStatusRunning = DocumentationEntry<BlockStatusRunningConfig, Dummy>;
using GutterText = DocumentationEntry<GutterTextConfig, Dummy>;
using GutterTextSet = DocumentationEntry<GutterTextSetConfig, Dummy>;
```

- [ ] **Step 4: Read and write them**

In `src/contour/config/Config.cpp`, in the colour-scheme loader, after the closing brace of the `if (auto const folding = child["fold_marker"]) { … }` block insert:

```cpp

    // The block column's status colours and the gutter's text colour, each left unset unless the scheme
    // says otherwise, for the fold column's reason: vtbackend derives them from the palette (@see
    // ColorPalette::blockStatusColor), so a scheme that has never heard of them still shows them.
    if (auto const status = child["block_status"])
    {
        logger()("*** loading block_status");
        for (auto const& [key, target]: { pair { "success", &where.blockStatusSuccess },
                                          pair { "failure", &where.blockStatusFailure },
                                          pair { "running", &where.blockStatusRunning } })
        {
            if (!status[key])
                continue;
            auto color = vtbackend::RGBColor {};
            loadFromEntry(status, key, color);
            *target = color;
        }
    }
    if (child["gutter_text"])
    {
        auto color = vtbackend::RGBColor {};
        loadFromEntry(child, "gutter_text", color);
        where.gutterText = color;
    }
```

In `emitColorPaletteBody`, after the fold-marker block (the `else processWithDoc(documentation::FoldMarker {});` line) insert:

```cpp

    // Same rule, same reason: only what the scheme set, and the commented example otherwise. One literal
    // per key rather than one `{}: {}` row: Writer::process() has an overload that takes a name and drops
    // it, which would leave such a row one argument short.
    if (entry.blockStatusSuccess || entry.blockStatusFailure || entry.blockStatusRunning)
    {
        processWithDoc(documentation::BlockStatusHeader {});
        if (entry.blockStatusSuccess)
            processWithDoc(documentation::BlockStatusSuccess {}, *entry.blockStatusSuccess);
        if (entry.blockStatusFailure)
            processWithDoc(documentation::BlockStatusFailure {}, *entry.blockStatusFailure);
        if (entry.blockStatusRunning)
            processWithDoc(documentation::BlockStatusRunning {}, *entry.blockStatusRunning);
    }
    else
        processWithDoc(documentation::BlockStatus {});

    if (entry.gutterText)
        processWithDoc(documentation::GutterTextSet {}, *entry.gutterText);
    else
        processWithDoc(documentation::GutterText {});
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: PASS — the new case and every existing `[config]` case, the generated-config round trip with its empty error log among them.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i src/contour/config/ConfigDocumentation.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp
git add src/contour/config/ConfigDocumentation.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
contour: read and write the gutter's colour-scheme keys

block_status.success/failure/running and gutter_text load into the
palette's optional slots and are written back only when a scheme sets
them; the generated config shows them as a commented example with the
colours the default scheme derives.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.11: The `gutter` configuration section

**Files:**
- Modify: `src/vtbackend/screen/Gutter.hpp` (a `std::formatter<vtbackend::LineNumberMode>` after the namespace)
- Modify: `src/contour/config/ConfigEnum.hpp` (include; table; specialization)
- Modify: `src/contour/config/ConfigDocumentation.hpp` (literals after `FoldingConfig` at :371-391 and `FoldingWeb` at :2108; `using Gutter` after `using Folding` at :2658)
- Modify: `src/contour/config/Config.hpp` (include; `GutterConfig` after `FoldingConfig` at :343-361; `ConfigEntry` after `folding` at :1308; reader declarations after the `FoldingConfig&` one at :1626; writer `format` after `format(…, FoldingConfig const&)` at :1930; `gutterSettingsFor` after `emulationSettings` at :2238)
- Modify: `src/contour/config/Config.cpp` (`load()` after `loadFromEntry("folding", c.folding);` at :1056; readers after the `FoldingConfig&` reader at :2193-2203; `gutterSettingsFor` after `emulationSettings`)
- Test: `src/contour/config/Config_test.cpp` (append)

**Interfaces:**
- Consumes: Task 4.5 `measureTimestampFormat()`, `timestampFormatErrorText()`, `DefaultTimestampFormat`; Task 4.6 `GutterSettings`, `LineNumberMode`, `MinLineNumberWidth`, `MaxLineNumberWidth`.
- Produces (C9): `struct GutterConfig { bool exitStatus; bool userMarks; vtbackend::LineNumberMode lineNumbers; int lineNumberWidth; bool timestamps; std::string timestampFormat; };` (global key `gutter`); `void YAMLConfigReader::loadFromEntry(YAML::Node const&, std::string const&, GutterConfig&);` (writes only the children present, as phase 10 requires).
- Produces (additions): `bool GutterConfig::operator==(GutterConfig const&) const = default;`; `[[nodiscard]] vtbackend::GutterSettings gutterSettingsFor(Config const& config);`; `configEnumValues<vtbackend::LineNumberMode>()`; `std::formatter<vtbackend::LineNumberMode>`; `documentation::Gutter`.

The section is loaded, written and tested here, but the terminal and the window geometry switch to it together in Task 4.12 — switching one alone would reserve one width and draw another.

- [ ] **Step 1: Write the failing tests**

Add `#include <vtbackend/screen/Gutter.hpp>` and `#include <contour/config/ConfigEnum.hpp>` to `Config_test.cpp`'s includes (if absent), then append:

```cpp
TEST_CASE("Config: the gutter section loads from YAML", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
gutter:
    exit_status: false
    user_marks: false
    line_numbers: hybrid
    line_number_width: 8
    timestamps: true
    timestamp_format: '%H:%M'
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const& gutter = config.gutter.value();
    CHECK(gutter.exitStatus == false);
    CHECK(gutter.userMarks == false);
    CHECK(gutter.lineNumbers == vtbackend::LineNumberMode::Hybrid);
    CHECK(gutter.lineNumberWidth == 8);
    CHECK(gutter.timestamps == true);
    CHECK(gutter.timestampFormat == "%H:%M");
}

TEST_CASE("Config: gutter defaults keep the one column the fold markers always took", "[config]")
{
    auto const config = contour::config::Config {};
    auto const& gutter = config.gutter.value();
    CHECK(gutter.exitStatus);
    CHECK(gutter.userMarks);
    CHECK(gutter.lineNumbers == vtbackend::LineNumberMode::Off);
    CHECK(gutter.lineNumberWidth == 6);
    CHECK_FALSE(gutter.timestamps);
    CHECK(gutter.timestampFormat == "%H:%M:%S");

    // Spec §5.1: nobody's column count changes on upgrade.
    CHECK(vtbackend::gutterLayoutFor(contour::config::gutterSettingsFor(config)).totalColumns == 1);
}

TEST_CASE("Config: an unusable gutter width or timestamp format keeps the default and says why", "[config]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadFromYaml(dir, R"(
default_profile: main
gutter:
    line_numbers: absolute
    line_number_width: 40
    timestamp_format: '%K'
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const& gutter = config.gutter.value();
    CHECK(gutter.lineNumbers == vtbackend::LineNumberMode::Absolute); // the siblings still load
    CHECK(gutter.lineNumberWidth == 6);
    CHECK(gutter.timestampFormat == "%H:%M:%S");
    CHECK(capture.text().contains("line_number_width"));
    CHECK(capture.text().contains("timestamp_format"));
}

TEST_CASE("Config: an unknown line_numbers value keeps the default", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
gutter:
    line_numbers: sideways
profiles:
    main:
        shell: /bin/sh
)"sv);
    CHECK(config.gutter.value().lineNumbers == vtbackend::LineNumberMode::Off);
}

TEST_CASE("Config: gutterSettingsFor joins the gutter section and the fold switch", "[config]")
{
    auto config = contour::config::Config {};
    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    config.gutter.value().lineNumberWidth = 4;
    config.gutter.value().timestamps = true;
    config.gutter.value().timestampFormat = "%H:%M";

    auto const settings = contour::config::gutterSettingsFor(config);
    CHECK(settings.foldMarkers); // folding.enabled and folding.show_markers, both on by default
    CHECK(settings.exitStatus);
    CHECK(settings.userMarks);
    CHECK(settings.lineNumbers == vtbackend::LineNumberMode::Relative);
    CHECK(settings.lineNumberWidth == 4);
    CHECK(settings.timestamps);
    CHECK(settings.timestampFormat == "%H:%M");

    config.folding.value().enabled = false;
    CHECK_FALSE(contour::config::gutterSettingsFor(config).foldMarkers);
}

TEST_CASE("Config: the gutter section survives a write and a read", "[config]")
{
    QTemporaryDir dir;
    auto capture = core::log::ScopedCapture { "error" };

    SECTION("the generated defaults read back as the defaults")
    {
        auto const generated = contour::config::defaultConfigString();
        REQUIRE(generated.contains("gutter:"));
        CHECK(loadFromYaml(dir, generated).gutter.value() == contour::config::GutterConfig {});
    }

    SECTION("set values, a quote in the format among them, read back as set")
    {
        auto config = contour::config::Config {};
        config.gutter.value() = contour::config::GutterConfig { .exitStatus = false,
                                                                .userMarks = true,
                                                                .lineNumbers = vtbackend::LineNumberMode::Relative,
                                                                .lineNumberWidth = 9,
                                                                .timestamps = true,
                                                                .timestampFormat = "%H:%M 'x'" };
        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        CHECK(loadFromYaml(dir, written).gutter.value() == config.gutter.value());
    }

    CHECK(capture.text().empty());
}

TEST_CASE("Config: the line_numbers tokens are what the writer spells", "[config]")
{
    // The reader takes the table's tokens and the writer formats the enum: one disagreement would write
    // a file the reader then rejects.
    for (auto const& info: contour::config::configEnumValues<vtbackend::LineNumberMode>())
        CHECK(std::format("{}", info.value) == info.token);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL to compile — `'gutter': is not a member of 'contour::config::Config'`.

- [ ] **Step 3: Spell the enum, and table it**

In `src/vtbackend/screen/Gutter.hpp`, after the closing `} // namespace vtbackend` append:

```cpp

/// The configuration spelling, so a log line or a written config reads like the file it came from. Kept
/// equal to contour's token table (ConfigEnum.hpp) by the test that checks every row.
template <>
struct std::formatter<vtbackend::LineNumberMode>: formatter<std::string_view>
{
    auto format(vtbackend::LineNumberMode value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::LineNumberMode::Off: name = "off"; break;
            case vtbackend::LineNumberMode::Absolute: name = "absolute"; break;
            case vtbackend::LineNumberMode::Relative: name = "relative"; break;
            case vtbackend::LineNumberMode::Hybrid: name = "hybrid"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
```

(add `#include <format>` to its standard includes).

In `src/contour/config/ConfigEnum.hpp`, add `#include <vtbackend/screen/Gutter.hpp> // LineNumberMode` after the `Folding.hpp` include; inside `namespace detail`, after `SearchCaseSensitivityTable`, insert:

```cpp

    /// Which line numbers the gutter shows, in vim's terms (spec §5.3).
    inline constexpr auto LineNumberModeTable = std::array {
        ConfigEnumInfo<vtbackend::LineNumberMode> { vtbackend::LineNumberMode::Off, "off", "None" },
        ConfigEnumInfo<vtbackend::LineNumberMode> {
            vtbackend::LineNumberMode::Absolute, "absolute", "Absolute (vim: number)" },
        ConfigEnumInfo<vtbackend::LineNumberMode> {
            vtbackend::LineNumberMode::Relative, "relative", "Relative to the cursor (vim: relativenumber)" },
        ConfigEnumInfo<vtbackend::LineNumberMode> {
            vtbackend::LineNumberMode::Hybrid, "hybrid", "Hybrid (vim: number relativenumber)" },
    };
```

and after the `SearchCaseSensitivity` specialization:

```cpp

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::LineNumberMode> const> configEnumValues() noexcept
{
    return detail::LineNumberModeTable;
}
```

- [ ] **Step 4: Document the section**

In `src/contour/config/ConfigDocumentation.hpp`, after the closing `};` of `constexpr StringLiteral FoldingConfig` insert:

```cpp

constexpr StringLiteral GutterConfig {

    "gutter:\n"
    "    {comment} The strip left of the grid. Its width follows this configuration and never the\n"
    "    {comment} content, so a growing line number cannot resize the page mid-command. The block\n"
    "    {comment} column beside the grid is reserved while fold markers (folding.show_markers), exit\n"
    "    {comment} status or user marks are on -- one column, as the fold markers always took.\n"
    "    {comment} Colour each command's fold controls by how it ended, and draw a triangle beside a\n"
    "    {comment} running command and a dot beside a finished one that has nothing to fold.\n"
    "    exit_status: {}\n"
    "    {comment} Draw a diamond beside every row marked with vi-mode mm.\n"
    "    user_marks: {}\n"
    "    {comment} Line numbers, as in vim:\n"
    "    {comment}   off      - none\n"
    "    {comment}   absolute - every row's number, counted from the session's first row (nu)\n"
    "    {comment}   relative - every row's distance from the cursor row (rnu)\n"
    "    {comment}   hybrid   - the cursor row's own number, every other row's distance (nu rnu)\n"
    "    line_numbers: {}\n"
    "    {comment} Columns the line numbers take, 3 to 10. A longer number keeps its last digits.\n"
    "    line_number_width: {}\n"
    "    {comment} Show the time each row was first reached, wherever it differs from the row above.\n"
    "    timestamps: {}\n"
    "    {comment} A std::format chrono specification in local time, such as '%H:%M' or '%a %H:%M:%S'.\n"
    "    timestamp_format: '{}'\n"
    "\n"

};
```

after the closing `};` of `constexpr StringLiteral FoldingWeb` insert:

```cpp

constexpr StringLiteral GutterWeb {
    "configuration controls the gutter: the strip left of the grid that carries per-line timestamps, "
    "line numbers and the block column.\n"
    "``` yaml\n"
    "gutter:\n"
    "  exit_status: true\n"
    "  user_marks: true\n"
    "  line_numbers: off\n"
    "  line_number_width: 6\n"
    "  timestamps: false\n"
    "  timestamp_format: '%H:%M:%S'\n"
    "```\n"
    "The gutter's width follows this configuration and never its content, so a growing line number "
    "cannot resize the page in the middle of a command's output. The block column is reserved while "
    "fold markers (`folding.show_markers`), exit status or user marks are on -- one column, exactly as "
    "the fold markers always took.\n"
    ":octicons-horizontal-rule-16: ==exit_status== Colour each command's fold controls by how it ended "
    "-- the colour scheme's `block_status` colours -- and draw a triangle beside a running command and "
    "a dot beside a finished one that has nothing to fold. Hovering the block column shows the exit "
    "code, duration, start time, working directory and command line. <br/>\n"
    ":octicons-horizontal-rule-16: ==user_marks== Draw a diamond beside every row marked with vi-mode "
    "`mm`. <br/>\n"
    ":octicons-horizontal-rule-16: ==line_numbers== `off`, `absolute` (every row's number, counted from "
    "the session's first row, so the numbers stay put while history scrolls away), `relative` (every "
    "row's distance from the cursor row, counting a collapsed block as one row -- exactly what `j` and "
    "`k` count) or `hybrid` (the cursor row's own number, every other row's distance), as vim's `nu` and "
    "`rnu`. In vi mode the distance counts from the vi cursor. <br/>\n"
    ":octicons-horizontal-rule-16: ==line_number_width== Columns the line numbers take, 3 to 10. A "
    "longer number shows its last digits after an ellipsis. <br/>\n"
    ":octicons-horizontal-rule-16: ==timestamps== Show the time each row was first reached. A row shows "
    "it only where it differs from the row above, so a long output does not repeat one second. <br/>\n"
    ":octicons-horizontal-rule-16: ==timestamp_format== A `std::format` chrono specification in local "
    "time. A format that cannot be drawn is reported when the configuration loads, and the default is "
    "kept. <br/>\n"
    "\n"
    "In vi normal mode, `za`, `zo`, `zc`, `zM` and `zR` toggle, open and close the block under the "
    "cursor, and close or open all of them, as in vim.\n"
};
```

and after `using Folding = DocumentationEntry<FoldingConfig, FoldingWeb>;` insert:

```cpp
using Gutter = DocumentationEntry<GutterConfig, GutterWeb>;
```

- [ ] **Step 5: Model, read and write the section**

In `src/contour/config/Config.hpp`, add `#include <vtbackend/screen/Gutter.hpp>` to the vtbackend includes. After the closing `};` of `struct FoldingConfig` insert:

```cpp

/// The gutter left of the grid (spec §5): what its block column shows, its line numbers and timestamps.
///
/// GLOBAL for FoldingConfig's reason: it describes how the user reads output, not how one shell writes
/// it. Plain bools and an int because these are YAML schema fields, converted at the boundary by
/// gutterSettingsFor() -- the documented carve-out in AGENT.md. Whether the block column draws fold
/// controls stays where it always was, `folding.show_markers`.
struct GutterConfig
{
    bool exitStatus { true };  ///< Colour the block column by outcome, and draw ▸ and ●.
    bool userMarks { true };   ///< Draw ◆ beside rows marked with vi `mm`.
    vtbackend::LineNumberMode lineNumbers { vtbackend::LineNumberMode::Off }; ///< Which numbers, if any.
    int lineNumberWidth { 6 }; ///< Columns the numbers take, MinLineNumberWidth..MaxLineNumberWidth.
    bool timestamps { false }; ///< Show when each row was first reached.
    std::string timestampFormat { vtbackend::DefaultTimestampFormat }; ///< std::format chrono spec, local time.

    bool operator==(GutterConfig const&) const = default;
};
```

After `ConfigEntry<FoldingConfig, documentation::Folding> folding {};` insert:

```cpp
    ConfigEntry<GutterConfig, documentation::Gutter> gutter {};
```

After `void loadFromEntry(YAML::Node const& node, std::string const& entry, FoldingConfig& where);` insert:

```cpp
    void loadFromEntry(YAML::Node const& node, std::string const& entry, GutterConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::LineNumberMode& where);
```

After `format(std::string_view doc, FoldingConfig const& v)` in `struct Writer` insert:

```cpp

    [[nodiscard]] std::string format(std::string_view doc, GutterConfig const& v)
    {
        // Single-quoted in the document -- a plain YAML scalar cannot begin with `%` -- so a quote inside
        // the format is doubled, the YAML way.
        auto quoted = std::string {};
        for (auto const ch: v.timestampFormat)
        {
            quoted += ch;
            if (ch == '\'')
                quoted += '\'';
        }
        return format(doc, v.exitStatus, v.userMarks, v.lineNumbers, v.lineNumberWidth, v.timestamps, quoted);
    }
```

After the `emulationSettings` declaration insert:

```cpp

/// What the terminal's gutter shows under @p config: the `gutter` section, with the fold switch taken from
/// `folding` (@see FoldingConfig::markersVisible).
///
/// The one conversion: the terminal is configured with it and the window geometry reserves its width from
/// it (@see session::configuredGutterLayout), so the strip reserved and the strip drawn cannot disagree.
/// @param config The loaded configuration.
/// @return The gutter settings vtbackend draws with.
[[nodiscard]] vtbackend::GutterSettings gutterSettingsFor(Config const& config);
```

In `src/contour/config/Config.cpp`, in `YAMLConfigReader::load`, after `loadFromEntry("folding", c.folding);` insert:

```cpp
        loadFromEntry("gutter", c.gutter);
```

After the closing brace of the `FoldingConfig&` reader insert:

```cpp

void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, GutterConfig& where)
{
    // Only the children present are written: a settings-page override of one key must leave its siblings
    // as contour.yml set them.
    auto const child = node[entry];
    if (!child)
        return;

    loadFromEntry(child, "exit_status", where.exitStatus);
    loadFromEntry(child, "user_marks", where.userMarks);
    loadFromEntry(child, "line_numbers", where.lineNumbers);
    loadFromEntry(child, "timestamps", where.timestamps);

    // Validated rather than clamped or trusted: a width out of range and a format the gutter cannot draw
    // are typos, and either taken as written would resize the page or blank the column.
    if (child["line_number_width"])
    {
        auto width = where.lineNumberWidth;
        loadFromEntry(child, "line_number_width", width);
        if (width >= vtbackend::MinLineNumberWidth && width <= vtbackend::MaxLineNumberWidth)
            where.lineNumberWidth = width;
        else
            errorLog()("gutter.line_number_width must be between {} and {}, not {}; keeping {}.",
                       vtbackend::MinLineNumberWidth,
                       vtbackend::MaxLineNumberWidth,
                       width,
                       where.lineNumberWidth);
    }

    if (child["timestamp_format"])
    {
        auto spec = where.timestampFormat;
        loadFromEntry(child, "timestamp_format", spec);
        if (auto const measured = vtbackend::measureTimestampFormat(spec); measured)
            where.timestampFormat = std::move(spec);
        else
            errorLog()("gutter.timestamp_format '{}' cannot be used: {}; keeping '{}'.",
                       spec,
                       vtbackend::timestampFormatErrorText(measured.error()),
                       where.timestampFormat);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::LineNumberMode& where)
{
    // Through the shared token table, so a value this reader accepts is one the settings page can offer
    // and the writer can spell back (@see configEnumValues).
    (void) loadConfigEnum(node, entry, where, logger);
}
```

After the closing brace of `emulationSettings` insert:

```cpp

vtbackend::GutterSettings gutterSettingsFor(Config const& config)
{
    auto const& gutter = config.gutter.value();
    return vtbackend::GutterSettings {
        .foldMarkers = config.folding.value().markersVisible(),
        .exitStatus = gutter.exitStatus,
        .userMarks = gutter.userMarks,
        .lineNumbers = gutter.lineNumbers,
        // Clamped here too: the loader validates the file, but a settings page can assign anything.
        .lineNumberWidth = static_cast<uint8_t>(
            std::clamp(gutter.lineNumberWidth, vtbackend::MinLineNumberWidth, vtbackend::MaxLineNumberWidth)),
        .timestamps = gutter.timestamps,
        .timestampFormat = gutter.timestampFormat,
    };
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test contour_test vtbackend_test` then `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[gutter]"`
Expected: PASS — including the existing generated-config round trip, whose error log stays empty.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i src/vtbackend/screen/Gutter.hpp src/contour/config/ConfigEnum.hpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp
git add src/vtbackend/screen/Gutter.hpp src/contour/config/ConfigEnum.hpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
contour: add the gutter configuration section

gutter.exit_status, user_marks, line_numbers, line_number_width,
timestamps and timestamp_format load, validate (an out-of-range width or
an undrawable format is logged and the default kept), document and write
back. gutterSettingsFor() joins the section with the fold switch.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.12: One layout for the geometry, the hit-test and the terminal

**Files:**
- Modify: `src/contour/geometry/WindowGeometry.hpp` (after `isInGutter` at :469-474)
- Modify: `src/contour/session/FontControl.hpp` (`gutterWidthFor` at :42-57), `src/contour/session/FontControl.cpp` (`applyResize` at :111; new definition)
- Modify: `src/contour/display/TerminalDisplay.cpp` (:928, :1618), `src/contour/window/WindowController.cpp` (:1022, :1149, :1277)
- Modify: `src/contour/session/SessionInput.cpp` (`gutterLineAt` at :112-146; call sites at :697, :744, :766)
- Modify: `src/contour/session/TerminalSession.hpp` (`GutterHit` after `ConsumedByGutter` at :86-90; `sendGutterHoverEvent` / `sendGutterPressEvent` at :698-719), `src/contour/session/TerminalSession.cpp` (:2137-2187; `configureTerminal`)
- Modify: `src/contour/config/Config.cpp` (`emulationSettings`)
- Modify: `src/contour/CMakeLists.txt` (`contour_gui_test` sources)
- Test: `src/contour/geometry/WindowGeometry_test.cpp` (append), `src/contour/session/FontControl_test.cpp` (new), `src/contour/session/TerminalSession_test.cpp` (append), `src/contour/config/Config_test.cpp` (append)

**Interfaces:**
- Consumes: `gutterLayoutFor()`, `gutterSegmentAt()` (Task 4.6); `config::gutterSettingsFor()` (Task 4.11).
- Produces (C2): `[[nodiscard]] geometry::GutterWidth gutterWidthFor(vtbackend::GutterLayout const& layout, vtbackend::ImageSize cellSize)` in `FontControl.hpp`, replacing the `FoldingConfig` overload (declared `inline … noexcept` in addition).
- Produces (additions): `[[nodiscard]] vtbackend::GutterLayout session::configuredGutterLayout(config::Config const& config);`; `[[nodiscard]] constexpr std::optional<int> geometry::gutterColumnAt(int xDevicePx, int pageMarginLeft, GutterWidth gutterDevicePx, int cellWidthPx) noexcept;`; `struct session::GutterHit { vtbackend::LineOffset gridLine; vtbackend::LineOffset screenRow; vtbackend::GutterSegmentKind segment; };`; `ConsumedByGutter TerminalSession::sendGutterHoverEvent(std::optional<GutterHit> hit);` and `ConsumedByGutter TerminalSession::sendGutterPressEvent(std::optional<GutterHit> hit, vtbackend::MouseButton button);` (were `std::optional<vtbackend::LineOffset>`). `SessionInput.cpp`'s internal `gutterLineAt` becomes `gutterHitAt`.

Every switch happens in this one commit — the terminal's settings, the six geometry call sites and the hit-test — so no commit reserves one gutter and draws another.

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/geometry/WindowGeometry_test.cpp`:

```cpp
TEST_CASE("WindowGeometry.gutterColumnAt.namesTheColumnFromTheGuttersLeftEdge", "[contour][geometry]")
{
    // A three-column gutter of 10 px cells, ending where the grid starts at 35 px.
    constexpr auto PageMarginLeft = 35;
    constexpr auto Gutter = 30;
    constexpr auto Cell = 10;

    CHECK(gutterColumnAt(4, PageMarginLeft, Gutter, Cell) == std::nullopt); // the configured margin
    CHECK(gutterColumnAt(5, PageMarginLeft, Gutter, Cell) == 0);
    CHECK(gutterColumnAt(14, PageMarginLeft, Gutter, Cell) == 0);
    CHECK(gutterColumnAt(15, PageMarginLeft, Gutter, Cell) == 1);
    CHECK(gutterColumnAt(34, PageMarginLeft, Gutter, Cell) == 2);
    CHECK(gutterColumnAt(35, PageMarginLeft, Gutter, Cell) == std::nullopt); // column 0 of the grid
    CHECK(gutterColumnAt(20, PageMarginLeft, 0, Cell) == std::nullopt);      // no gutter at all
    CHECK(gutterColumnAt(20, PageMarginLeft, Gutter, 0) == std::nullopt);    // no font yet
}
```

Create `src/contour/session/FontControl_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <contour/config/Config.hpp>
#include <contour/session/FontControl.hpp>

#include <vtbackend/screen/Gutter.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::session::configuredGutterLayout;
using contour::session::gutterWidthFor;

TEST_CASE("FontControl.gutterWidthFor.reservesEveryColumnOfTheLayout", "[contour][geometry][gutter]")
{
    auto const cell = vtbackend::ImageSize { .width = vtbackend::Width(9), .height = vtbackend::Height(18) };
    CHECK(gutterWidthFor(vtbackend::GutterLayout {}, cell) == 0);

    auto const layout = vtbackend::gutterLayoutFor(vtbackend::GutterSettings {
        .foldMarkers = true, .lineNumbers = vtbackend::LineNumberMode::Absolute, .lineNumberWidth = 4 });
    REQUIRE(layout.totalColumns == 5);
    CHECK(gutterWidthFor(layout, cell) == 5 * 9);
}

TEST_CASE("FontControl.configuredGutterLayout.followsTheConfiguration", "[contour][geometry][gutter]")
{
    auto config = contour::config::Config {};
    // The default: one column, exactly as the fold markers always took.
    CHECK(configuredGutterLayout(config).totalColumns == 1);

    config.gutter.value().lineNumbers = vtbackend::LineNumberMode::Relative;
    CHECK(configuredGutterLayout(config).totalColumns == 7);

    // No job left for the block column and no numbers: no strip, and no width taken from the page.
    config.gutter.value() = contour::config::GutterConfig { .exitStatus = false, .userMarks = false };
    config.folding.value().showMarkers = false;
    CHECK(configuredGutterLayout(config).totalColumns == 0);
}
```

In `src/contour/CMakeLists.txt`, in the `contour_gui_test` source list add `            session/FontControl_test.cpp` after `            session/HyperlinkTooltip_test.cpp`.

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: the terminal draws the gutter the window geometry reserves", "[config]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
folding:
    show_markers: false
gutter:
    line_numbers: absolute
profiles:
    main:
        shell: /bin/sh
)"sv);

    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    auto const settings = contour::config::emulationSettings(config, *profile);
    CHECK(settings.gutter == contour::config::gutterSettingsFor(config));
    CHECK(settings.gutter.lineNumbers == vtbackend::LineNumberMode::Absolute);
    CHECK_FALSE(settings.gutter.foldMarkers);
    CHECK(settings.gutter.exitStatus); // the block column outlives the fold markers
}
```

Append to `src/contour/session/TerminalSession_test.cpp` (add `#include <vtbackend/screen/Gutter.hpp>` to its includes):

```cpp
namespace
{
/// Runs a failing command through @p session's terminal as a shell with OSC 133 integration would:
/// prompt on row 0, its output on row 1, and the end mark with exit code 2.
void runFailingCommand(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen(
        "\033]133;A\033\\$ \033]133;B\033\\make\r\n\033]133;C\033\\boom\r\n\033]133;D;2\033\\");
}

/// A hit on @p segment beside grid line @p line, the page starting at screen row 0.
[[nodiscard]] contour::session::GutterHit gutterHit(int line, vtbackend::GutterSegmentKind segment)
{
    return { .gridLine = vtbackend::LineOffset(line), .screenRow = vtbackend::LineOffset(line), .segment = segment };
}
} // namespace

TEST_CASE("TerminalSession: a press beside the line numbers is the gutter's, and does nothing",
          "[contour][session][gutter]")
{
    using contour::session::ConsumedByGutter;
    using vtbackend::GutterSegmentKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    auto const& folds = session->terminal().foldState();

    CHECK(session->sendGutterPressEvent(gutterHit(0, GutterSegmentKind::LineNumber), vtbackend::MouseButton::Left)
          == ConsumedByGutter::Yes);
    CHECK(session->sendGutterReleaseEvent() == ConsumedByGutter::Yes);
    CHECK(folds.empty());                             // nothing toggled
    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // and the child saw neither half

    // Every other button still belongs to the application, wherever it lands.
    CHECK(session->sendGutterPressEvent(gutterHit(0, GutterSegmentKind::Timestamp), vtbackend::MouseButton::Right)
          == ConsumedByGutter::No);

    // The block column keeps today's behaviour: a press on a fold toggles it.
    CHECK(session->sendGutterPressEvent(gutterHit(0, GutterSegmentKind::BlockColumn), vtbackend::MouseButton::Left)
          == ConsumedByGutter::Yes);
    CHECK_FALSE(folds.empty());
    CHECK(session->sendGutterReleaseEvent() == ConsumedByGutter::Yes);
}

TEST_CASE("TerminalSession: hovering the line numbers is the gutter's, with nothing to click",
          "[contour][session][gutter]")
{
    using contour::input::MouseCursorShape;
    using contour::session::ConsumedByGutter;
    using vtbackend::GutterSegmentKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);

    CHECK(session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn)) == ConsumedByGutter::Yes);
    REQUIRE_FALSE(session.surface->cursorShapes.empty());
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::PointingHand);

    CHECK(session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::LineNumber)) == ConsumedByGutter::Yes);
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::IBeam);

    CHECK(session->sendGutterHoverEvent(std::nullopt) == ConsumedByGutter::No);
}

TEST_CASE("TerminalSession: the terminal draws the gutter the configuration asks for, after a switch too",
          "[contour][session][gutter]")
{
    TestApp testApp;
    testApp.app().config().gutter.value().lineNumbers = vtbackend::LineNumberMode::Absolute;
    registerProfile(testApp.app(), "night", [](contour::config::TerminalProfile&) {});
    auto session = makeDisplaylessSession(testApp.app());

    CHECK(session->terminal().settings().gutter == contour::config::gutterSettingsFor(session->config()));
    CHECK(session->terminal().settings().gutter.lineNumbers == vtbackend::LineNumberMode::Absolute);

    // A profile switch -- the path a configuration reload takes -- re-applies it over whatever was there.
    session->terminal().settings().gutter = vtbackend::GutterSettings {};
    CHECK((*session)(contour::actions::ChangeProfile { "night" }));
    CHECK(session->terminal().settings().gutter.lineNumbers == vtbackend::LineNumberMode::Absolute);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL to compile — `'gutterColumnAt': identifier not found`, `'configuredGutterLayout': is not a member of 'contour::session'`, `'GutterHit': is not a member of 'contour::session'`.

- [ ] **Step 3: The geometry speaks columns**

In `src/contour/geometry/WindowGeometry.hpp`, after the closing brace of `isInGutter` insert:

```cpp

/// The gutter column under @p xDevicePx, counted from the gutter's LEFT edge -- the column
/// vtbackend::gutterSegmentAt() names a segment for.
///
/// Built on isInGutter() so the two cannot disagree about where the strip is.
/// @param xDevicePx       The x coordinate, in device pixels, relative to the item's left edge.
/// @param pageMarginLeft  The renderer's grid origin (@c GridMetrics::pageMargin.left), in device pixels.
/// @param gutterDevicePx  The reserved gutter width in device pixels; 0 means there is no gutter.
/// @param cellWidthPx     The cell width in device pixels.
/// @return The column, or nullopt when the coordinate is not over the gutter.
[[nodiscard]] constexpr std::optional<int> gutterColumnAt(int xDevicePx,
                                                          int pageMarginLeft,
                                                          GutterWidth gutterDevicePx,
                                                          int cellWidthPx) noexcept
{
    if (cellWidthPx <= 0 || !isInGutter(xDevicePx, pageMarginLeft, gutterDevicePx))
        return std::nullopt;
    return (xDevicePx - (pageMarginLeft - gutterDevicePx)) / cellWidthPx;
}
```

In `src/contour/session/FontControl.hpp`, add `#include <vtbackend/screen/Gutter.hpp>` to the vtbackend includes and replace the whole `gutterWidthFor(config::FoldingConfig const& folding, …)` function and its comment with:

```cpp
/// The gutter width, in device pixels, @p layout reserves.
///
/// THE gutter decision, in one place: every geometry call site reads it from here, and the layout it is
/// handed comes from configuredGutterLayout() -- the very gutterSettingsFor() the terminal draws with --
/// because a page fitted with one gutter and a renderer drawing another put the grid in two places.
///
/// @param layout The gutter's segments (@see vtbackend::gutterLayoutFor).
/// @param cellSize The cell size in device pixels.
/// @return The width in device pixels; 0 when the layout has no columns.
[[nodiscard]] inline geometry::GutterWidth gutterWidthFor(vtbackend::GutterLayout const& layout,
                                                          vtbackend::ImageSize cellSize) noexcept
{
    return layout.totalColumns * unbox<int>(cellSize.width);
}

/// The gutter layout @p config asks for: what the window reserves, what the hit-test reads, and -- through
/// the same config::gutterSettingsFor() -- what the terminal draws.
/// @param config The loaded configuration.
/// @return vtbackend::gutterLayoutFor(config::gutterSettingsFor(config)).
[[nodiscard]] vtbackend::GutterLayout configuredGutterLayout(config::Config const& config);
```

In `src/contour/session/FontControl.cpp`, before `void applyResize(` insert:

```cpp
vtbackend::GutterLayout configuredGutterLayout(config::Config const& config)
{
    return vtbackend::gutterLayoutFor(config::gutterSettingsFor(config));
}

```

and in `applyResize` replace:

```cpp
        gutterWidthFor(session.config().folding.value(), cellSize));
```

with:

```cpp
        gutterWidthFor(configuredGutterLayout(session.config()), cellSize));
```

- [ ] **Step 4: Switch the other geometry call sites**

`src/contour/display/TerminalDisplay.cpp` — replace

```cpp
            session::gutterWidthFor(_session->config().folding.value(), gridMetrics().cellSize));
```

with

```cpp
            session::gutterWidthFor(session::configuredGutterLayout(_session->config()), gridMetrics().cellSize));
```

and replace

```cpp
        session::gutterWidthFor(_session->config().folding.value(), cellSize));
```

with

```cpp
        session::gutterWidthFor(session::configuredGutterLayout(_session->config()), cellSize));
```

`src/contour/window/WindowController.cpp` — replace

```cpp
        session::gutterWidthFor(session->config().folding.value(), display->cellSize()));
```

with

```cpp
        session::gutterWidthFor(session::configuredGutterLayout(session->config()), display->cellSize()));
```

replace

```cpp
        session::gutterWidthFor(requester.session().config().folding.value(), cellSize);
```

with

```cpp
        session::gutterWidthFor(session::configuredGutterLayout(requester.session().config()), cellSize);
```

and replace

```cpp
        session::gutterWidthFor(requester.session().config().folding.value(), requester.cellSize()));
```

with

```cpp
        session::gutterWidthFor(session::configuredGutterLayout(requester.session().config()),
                                requester.cellSize()));
```

(In `WindowController.cpp` the local variable `session` does not hide the namespace: the name before `::` is looked up among namespaces and types only, which is why the existing `session::gutterWidthFor(session->…)` compiles.)

Run: `git grep -n "folding.value(), " -- src/contour`
Expected: no output — no geometry site reads the fold switch on its own any more.

In `src/contour/config/Config.cpp` (`emulationSettings`), replace:

```cpp
    // Until the `gutter` section is read, the block column exists exactly where the fold column did, so
    // the page the window fits and the strip the terminal draws keep agreeing.
    auto const blockColumn = folding.markersVisible();
    settings.gutter = vtbackend::GutterSettings { .foldMarkers = blockColumn,
                                                  .exitStatus = blockColumn,
                                                  .userMarks = blockColumn };
```

with:

```cpp
    // Through the one conversion the window geometry reserves the strip's width from.
    settings.gutter = gutterSettingsFor(config);
```

- [ ] **Step 5: The hit-test names a segment**

In `src/contour/session/TerminalSession.hpp`, after the closing `};` of `enum class ConsumedByGutter` insert:

```cpp

/// Where a pointer over the gutter is: the row it names and the segment it is over.
struct GutterHit
{
    vtbackend::LineOffset gridLine {};       ///< The grid line beside the pointer.
    vtbackend::LineOffset screenRow {};      ///< The screen row that line is drawn on, a top status line counted.
    vtbackend::GutterSegmentKind segment {}; ///< The segment under the pointer.
};
```

and replace the two declarations

```cpp
    ConsumedByGutter sendGutterHoverEvent(std::optional<vtbackend::LineOffset> gridLine);
```

```cpp
    ConsumedByGutter sendGutterPressEvent(std::optional<vtbackend::LineOffset> gridLine,
                                          vtbackend::MouseButton button);
```

with

```cpp
    ConsumedByGutter sendGutterHoverEvent(std::optional<GutterHit> hit);
```

```cpp
    ConsumedByGutter sendGutterPressEvent(std::optional<GutterHit> hit, vtbackend::MouseButton button);
```

changing each doc comment's `@param gridLine The grid line under the pointer, or nullopt when it is not over the gutter.` to `@param hit Where over the gutter the pointer is, or nullopt when it is not over the gutter.`, and adding to the press comment: `A left press beside the timestamps or the line numbers is consumed and does nothing.`

In `src/contour/session/TerminalSession.cpp`, replace the bodies of the two functions (:2137-2187) with:

```cpp
ConsumedByGutter TerminalSession::sendGutterHoverEvent(std::optional<GutterHit> hit)
{
    // A display-less session has no cursor to change and no gutter to be over.
    if (_display == nullptr)
        return ConsumedByGutter::No;

    // The overwhelmingly common case -- the pointer is over the GRID. Answered before taking the terminal
    // lock, because this runs on every pointer motion and the ordinary move path is about to take that
    // same lock itself.
    if (!hit)
    {
        clearGutterHover();
        return ConsumedByGutter::No;
    }

    _gutterHovered = true;

    // Only the block column is a control. Beside the timestamps and the numbers there is nothing to light
    // and nothing to click -- but the pointer is not over the grid either, so the event is consumed.
    auto const onBlockColumn = hit->segment == vtbackend::GutterSegmentKind::BlockColumn;
    auto const overFold = core::locked(_terminal, [&] {
        _terminal.setGutterHoverLine(onBlockColumn ? std::optional { hit->gridLine } : std::nullopt);
        return onBlockColumn && _terminal.foldContaining(hit->gridLine).has_value();
    });

    // Only the shape says whether there is anything here to click.
    if (overFold)
        _display->setMouseCursorShape(input::MouseCursorShape::PointingHand);
    else
        setDefaultCursor();

    // Leaving the grid ends any hyperlink hover the pointer left behind on its way out.
    clearHyperlinkHover();
    return ConsumedByGutter::Yes;
}

ConsumedByGutter TerminalSession::sendGutterPressEvent(std::optional<GutterHit> hit, vtbackend::MouseButton button)
{
    // Only a left click acts on the gutter; every other button belongs to the application, gutter or not.
    if (button != vtbackend::MouseButton::Left || !hit)
        return ConsumedByGutter::No;

    // Beside the timestamps or the line numbers there is nothing to act on, but the press is the
    // gutter's: neither the child nor a selection may see it, nor its release.
    if (hit->segment != vtbackend::GutterSegmentKind::BlockColumn)
    {
        _gutterClickPending = true;
        return ConsumedByGutter::Yes;
    }

    // Through the one gate every folding action passes: it takes the lock, honours the folding setting
    // and republishes the scrollable count, none of which a click on the column wants to restate.
    if (!withFolding([&](auto& terminal) { return terminal.toggleFoldContaining(hit->gridLine); }))
        return ConsumedByGutter::No;

    // Remembered here rather than by the caller, so the two halves of the handshake cannot drift: a
    // release reported for a press the child never saw leaves it holding a button down that was never
    // pressed.
    _gutterClickPending = true;
    return ConsumedByGutter::Yes;
}
```

In `src/contour/session/SessionInput.cpp`, replace the whole `gutterLineAt` function and its comment (:112-146) with:

```cpp
    /// Where over the gutter @p positionPx is: the grid line beside it and the segment under it.
    ///
    /// The one place that decides where the gutter IS and what a point on it names -- shared by the click
    /// that toggles a fold, the hover that highlights one and the tooltip, because those disagreeing would
    /// describe one block and act on another. The layout is the configured one, the very layout the window
    /// geometry reserved the strip for.
    ///
    /// Deliberately NOT routed through makeMouseCellLocation(): that clamps the column onto the page,
    /// so a gutter position would come back reading as column 0 of the grid.
    ///
    /// @param positionPx The pointer position in logical pixels, relative to the item's top-left.
    /// @param session The session it belongs to; must have a display.
    /// @return The hit, or nullopt when the position is not over the gutter.
    [[nodiscard]] std::optional<GutterHit> gutterHitAt(QPointF positionPx, TerminalSession const& session)
    {
        auto const& display = *session.display();
        auto const dpr = display.devicePixelRatio();
        auto const cellSize = display.gridMetrics().cellSize;
        auto const pageMargin = display.gridMetrics().pageMargin;
        auto const layout = configuredGutterLayout(session.config());

        auto const sx = static_cast<int>(positionPx.x() * dpr);
        auto const column = geometry::gutterColumnAt(
            sx, pageMargin.left, gutterWidthFor(layout, cellSize), unbox<int>(cellSize.width));
        auto const segment = column ? vtbackend::gutterSegmentAt(layout, *column) : std::nullopt;
        if (!segment)
            return std::nullopt;

        // Through the shared row hit-test, so the gutter and the grid agree about where the page
        // begins. They did not: this used to feed a raw SCREEN row to a translation that speaks
        // main-page rows, so with a status line above the grid every fold click landed one block off.
        auto const row = mouseGridRow(positionPx.y(), session);
        if (!row)
            return std::nullopt;

        auto const l = std::scoped_lock { session.terminal() };
        return GutterHit { .gridLine = session.terminal().viewport().translateScreenToGridLine(*row),
                           .screenRow = session.terminal().mainPageTopRow() + *row,
                           .segment = *segment };
    }
```

and replace each of the three `gutterLineAt(event->position(), session)` with `gutterHitAt(event->position(), session)`.

- [ ] **Step 6: A reload re-reads the gutter**

In `src/contour/session/TerminalSession.cpp`, at the end of `configureTerminal()` (after the `foldingAvailability` line Task 4.9 added) insert:

```cpp

    // The gutter too -- a reload that changed it used to leave the old strip drawn. A different width is
    // a different page, so the grid is refitted: posted by resizeTerminalToDisplaySize(), it runs once
    // this function has let go of the lock.
    auto const gutter = config::gutterSettingsFor(_config);
    auto const widthChanged = vtbackend::gutterLayoutFor(gutter).totalColumns
                              != vtbackend::gutterLayoutFor(_terminal.settings().gutter).totalColumns;
    _terminal.settings().gutter = gutter;
    if (widthChanged)
        resizeTerminalToDisplaySize();
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test contour` then `out/build/clangcl-debug/bin/contour_gui_test.exe "[gutter]"`, `out/build/clangcl-debug/bin/contour_gui_test.exe "[geometry]"` and `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: PASS. (If the full `contour` target fails in `src/contour/display/*` on `yaml-cpp/emitter.h`, that break is pre-existing on this machine — compile `TerminalDisplay.cpp` and `WindowController.cpp` alone via `ninja -t commands` as the README describes.)

- [ ] **Step 8: clang-tidy hand audit, format, commit**

`src/contour/**` cannot be linted on this machine. For every added line check: `gutterHitAt` stays inside `SessionInput.cpp`'s anonymous namespace (`misc-use-internal-linkage`); every local that is not modified is `const` (`misc-const-correctness` — `widthChanged`, `gutter`, `layout`, `column`, `segment`); `layout.totalColumns * unbox<int>(cellSize.width)` is `int * int` returned as `int`, not widened (`bugprone-implicit-widening-of-multiplication-result`).

```bash
clang-format -i src/contour/geometry/WindowGeometry.hpp src/contour/geometry/WindowGeometry_test.cpp src/contour/session/FontControl.hpp src/contour/session/FontControl.cpp src/contour/session/FontControl_test.cpp src/contour/display/TerminalDisplay.cpp src/contour/window/WindowController.cpp src/contour/session/SessionInput.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp
git add src/contour/geometry/WindowGeometry.hpp src/contour/geometry/WindowGeometry_test.cpp src/contour/session/FontControl.hpp src/contour/session/FontControl.cpp src/contour/session/FontControl_test.cpp src/contour/display/TerminalDisplay.cpp src/contour/window/WindowController.cpp src/contour/session/SessionInput.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp src/contour/CMakeLists.txt
git commit -F - <<'EOF'
contour: size, hit-test and draw the gutter from one layout

gutterWidthFor() takes the gutter layout, and every geometry site, the
hit-test and the terminal's settings derive it from gutterSettingsFor().
The hit-test now names the segment: presses beside the timestamps or
line numbers are consumed and do nothing. A reload re-reads the gutter
and refits the page when its width changed.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.13a: One duration formatter — `vtbackend::formatCommandDuration` (do this before Task 4.13)

The tooltip (Task 4.13), phase 6's finish notification and phase 7's sticky-header chip all print how long a command ran. One formatter, created here in vtbackend where all three can reach it; phases 6 and 7 consume it and write none of their own.

**Files:**
- Modify: `src/vtbackend/shell/CommandBlock.hpp` (after the `summaryOf` declaration)
- Modify: `src/vtbackend/shell/CommandBlock.cpp` (standard includes; before the closing `} // namespace vtbackend`)
- Test: `src/vtbackend/shell/CommandBlock_test.cpp` (append at the end of the file)

**Interfaces:**
- Consumes: nothing new.
- Produces (addition): `[[nodiscard]] std::string vtbackend::formatCommandDuration(std::chrono::steady_clock::duration duration);` — negative clamps to zero; below one second `"{n}ms"` (`850ms`, `0ms`); below a minute whole seconds `"{n}s"` (`12s`, `59s`); below an hour `"{m}m {ss}s"` with the seconds zero-padded (`3m 05s`); from an hour `"{h}h {mm}m"` with the minutes zero-padded (`1h 02m`, `25h 00m`). Consumed by Task 4.13, phase 6 (`finishNotificationBody`) and phase 7 (`stickyHeaderChip`).

- [ ] **Step 1: Write the failing test**

Append to `src/vtbackend/shell/CommandBlock_test.cpp` (it already includes `<array>`, `<chrono>`, `<string_view>` and has `using namespace vtbackend;` and `using namespace std::chrono_literals;`):

```cpp

// {{{ formatCommandDuration

TEST_CASE("CommandBlock.formatCommandDuration.readsAtTheCoarsestUsefulUnit", "[semanticblocks]")
{
    struct Row
    {
        std::chrono::steady_clock::duration duration;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { 0ms, "0ms" },       Row { 850ms, "850ms" },     Row { 999ms, "999ms" },
        Row { 1s, "1s" },         Row { 12s, "12s" },         Row { 59999ms, "59s" },
        Row { 60s, "1m 00s" },    Row { 185s, "3m 05s" },     Row { 192s, "3m 12s" },
        Row { 3599s, "59m 59s" }, Row { 3600s, "1h 00m" },    Row { 3725s, "1h 02m" },
        Row { 90000s, "25h 00m" },
    };
    for (auto const& row: rows)
    {
        INFO(row.expected);
        CHECK(formatCommandDuration(row.duration) == row.expected);
    }
}

TEST_CASE("CommandBlock.formatCommandDuration.aNegativeDurationReadsAsZero", "[semanticblocks]")
{
    // A wall clock stepped backwards under a running command (phase 7's chip) must not print a negative.
    CHECK(formatCommandDuration(-5s) == "0ms");
}

// }}}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL to compile — `'formatCommandDuration': identifier not found`.

- [ ] **Step 3: Declare it**

In `src/vtbackend/shell/CommandBlock.hpp`, after the declaration `[[nodiscard]] CommandBlockSummary summaryOf(CommandBlockRecord const& record);` insert:

```cpp

/// How long a command ran, as every surface prints it -- the gutter tooltip, the finish notification, the
/// sticky header's chip: `850ms`, `12s`, `3m 05s`, `1h 02m`.
///
/// Whole units below the coarsest one shown are dropped, not rounded, and hours are never folded into
/// days (a build that ran 25 hours reads `25h 00m`). A negative duration -- a wall clock stepped
/// backwards under a running command -- reads as `0ms`.
/// @param duration How long the command ran.
/// @return The text: below 1 s `{n}ms`; below 1 min `{n}s`; below 1 h `{m}m {ss}s`; else `{h}h {mm}m`.
[[nodiscard]] std::string formatCommandDuration(std::chrono::steady_clock::duration duration);
```

- [ ] **Step 4: Implement it**

In `src/vtbackend/shell/CommandBlock.cpp`, add `#include <format>` to the standard includes (between `#include <functional>` and `#include <limits>`), and before the closing `} // namespace vtbackend` insert:

```cpp

std::string formatCommandDuration(std::chrono::steady_clock::duration duration)
{
    namespace chrono = std::chrono;
    using Duration = chrono::steady_clock::duration;

    auto const clamped = duration < Duration::zero() ? Duration::zero() : duration;
    if (clamped < chrono::seconds(1))
        return std::format("{}ms", chrono::duration_cast<chrono::milliseconds>(clamped).count());

    auto const parts = chrono::hh_mm_ss { chrono::duration_cast<chrono::seconds>(clamped) };
    if (clamped < chrono::minutes(1))
        return std::format("{}s", parts.seconds().count());
    if (clamped < chrono::hours(1))
        return std::format("{}m {:02}s", parts.minutes().count(), parts.seconds().count());
    return std::format("{}h {:02}m", parts.hours().count(), parts.minutes().count());
}
```

- [ ] **Step 5: Run it to verify it passes**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "CommandBlock.formatCommandDuration*"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 6: Format and commit**

```bash
clang-format -i src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp
git add src/vtbackend/shell/CommandBlock.hpp src/vtbackend/shell/CommandBlock.cpp src/vtbackend/shell/CommandBlock_test.cpp
git commit -F - <<'EOF'
vtbackend: format a command's duration once for every surface

formatCommandDuration() prints 850ms, 12s, 3m 05s, 1h 02m; a negative
duration reads as 0ms. The gutter tooltip, the finish notification and
the sticky header's chip all print it, so they cannot disagree.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.13: The block column's tooltip

**Files:**
- Create: `src/contour/session/CommandBlockTooltip.hpp`, `src/contour/session/CommandBlockTooltip.cpp`
- Test: `src/contour/session/CommandBlockTooltip_test.cpp`
- Modify: `src/contour/CMakeLists.txt` (`_header_files`, `_core_source_files`, `contour_gui_test` sources)
- Modify: `src/contour/session/TerminalSession.hpp`, `src/contour/session/TerminalSession.cpp`
- Modify: `src/contour/qml/SessionChrome.qml` (after the `hyperlinkTip` item)
- Modify: `src/contour/test/QmlComponents_test.cpp` (`MockSession` at :266-389; new case after the hyperlink-tooltip case at :3513)
- Test: `src/contour/session/TerminalSession_test.cpp` (append)

**Interfaces:**
- Consumes: C1 `CommandBlockRecord`, `CommandBlockStore::find()`, `Terminal::commandBlockAt()`, `Terminal::commandBlocks()`; C6 `sanitizeCommandLine(std::string_view, SanitizePurpose::Display)`; Task 4.5 `Terminal::localTimeOf()`; Task 4.12 `GutterHit`; Task 4.13a `[[nodiscard]] std::string vtbackend::formatCommandDuration(std::chrono::steady_clock::duration duration);`.
- Produces (additions): `[[nodiscard]] std::string session::commandBlockTooltipText(vtbackend::CommandBlockRecord const& record, std::chrono::local_seconds startedAt);`; `Q_INVOKABLE QString TerminalSession::commandBlockTooltip() const;`; `Q_PROPERTY(QRectF commandBlockTooltipAnchor READ commandBlockTooltipAnchor NOTIFY commandBlockHoverChanged)` with `[[nodiscard]] QRectF commandBlockTooltipAnchor() const noexcept;`; signal `void commandBlockHoverChanged();`.

The text is read through the invocable when the anchor moves rather than carried as a property: the record is the terminal's, read under its lock at the moment the tooltip shows, and a hover that never happens formats nothing. (`commandBlockTooltip()` carries no `[[nodiscard]]`: moc reads its declaration.)

- [ ] **Step 1: Write the failing pure tests**

Create `src/contour/session/CommandBlockTooltip_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandBlockTooltip.hpp>

#include <vtbackend/core/TerminalContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using contour::session::commandBlockTooltipText;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace
{
/// 2026-10-09 14:03:07, local civil time.
constexpr auto Started = chrono::local_days { chrono::year { 2026 } / chrono::October / 9 } + 14h + 3min + 7s;

/// A command that finished with @p exitCode after 4.25 s, in /home/user/src.
[[nodiscard]] vtbackend::CommandBlockRecord finished(int exitCode)
{
    auto record = vtbackend::CommandBlockRecord {};
    record.state = vtbackend::CommandBlockState::Finished;
    record.exitCode = exitCode;
    record.duration = 4250ms;
    record.commandLine = "make -j8";
    record.workingDirectory.path = "/home/user/src";
    return record;
}
} // namespace

TEST_CASE("CommandBlockTooltip.saysHowWhenWhereAndWhat", "[contour][gutter]")
{
    // The duration is vtbackend::formatCommandDuration's (Task 4.13a): 4.25 s reads "4s".
    CHECK(commandBlockTooltipText(finished(2), Started)
          == "Exit code 2\nTook 4s\nStarted 2026-10-09 14:03:07\nIn /home/user/src\n$ make -j8");
}

TEST_CASE("CommandBlockTooltip.namesTheSignalThatEndedACommand", "[contour][gutter]")
{
    // From a shell's 128 + n...
    CHECK(commandBlockTooltipText(finished(130), Started).starts_with("Exit code 130 (SIGINT)\n"));

    // ...and from an OSC 3008 outcome that says so outright.
    auto record = finished(139);
    record.outcome.signal = vtbackend::ContextSignal::Segv;
    CHECK(commandBlockTooltipText(record, Started).starts_with("Exit code 139 (SIGSEGV)\n"));

    // A code above 128 that names no signal we know is just a code.
    CHECK(commandBlockTooltipText(finished(200), Started).starts_with("Exit code 200\n"));
}

TEST_CASE("CommandBlockTooltip.leavesOutWhatTheRecordDoesNotKnow", "[contour][gutter]")
{
    auto record = vtbackend::CommandBlockRecord {};
    record.state = vtbackend::CommandBlockState::Running;
    CHECK(commandBlockTooltipText(record, Started) == "Running\nStarted 2026-10-09 14:03:07");

    record.state = vtbackend::CommandBlockState::Finished;
    CHECK(commandBlockTooltipText(record, Started) == "Finished, exit code unknown\nStarted 2026-10-09 14:03:07");

    // A prompt still being typed at has run nothing, and so has nothing to say.
    record.state = vtbackend::CommandBlockState::Prompting;
    CHECK(commandBlockTooltipText(record, Started).empty());
}

TEST_CASE("CommandBlockTooltip.showsTheCommandLineSanitised", "[contour][gutter]")
{
    // Review Focus #3, at the tooltip: both strings are whatever arrived on the PTY.
    auto record = finished(0);
    record.commandLine = "echo \033]0;owned\007 done";
    record.workingDirectory.path = "/tmp/\xE2\x80\xAE" "evil"; // U+202E RIGHT-TO-LEFT OVERRIDE

    auto const text = commandBlockTooltipText(record, Started);
    CHECK_FALSE(text.contains('\033'));
    CHECK_FALSE(text.contains('\007'));
    CHECK_FALSE(text.contains("\xE2\x80\xAE"));
    CHECK(text.contains("\xEF\xBF\xBD")); // U+FFFD in their place
}
```

In `src/contour/CMakeLists.txt`: add `    session/CommandBlockTooltip.hpp` before `    session/HyperlinkTooltip.hpp` in `_header_files`, `    session/CommandBlockTooltip.cpp` before `    session/HyperlinkTooltip.cpp` in `_core_source_files`, and `            session/CommandBlockTooltip_test.cpp` before `            session/HyperlinkTooltip_test.cpp` in the `contour_gui_test` sources.

- [ ] **Step 2: Run them to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL to compile — `contour/session/CommandBlockTooltip.hpp` not found.

- [ ] **Step 3: Write the text**

Create `src/contour/session/CommandBlockTooltip.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <chrono>
#include <string>

namespace contour::session
{

/// What the gutter's block-column tooltip says about @p record (spec §5.2): how it ended -- exit code and
/// signal --, how long it took (@see vtbackend::formatCommandDuration), when it started, where it ran and
/// what it was, each line only when the record knows it.
///
/// The command line and the directory are sanitised for display (@see vtbackend::sanitizeCommandLine):
/// both are whatever the shell, or any program writing to the PTY, reported.
/// @param record The block under the pointer.
/// @param startedAt When it started, in local civil time (@see vtbackend::Terminal::localTimeOf).
/// @return The text, one fact per line; empty for a prompt still being typed at, which has none.
[[nodiscard]] std::string commandBlockTooltipText(vtbackend::CommandBlockRecord const& record,
                                                  std::chrono::local_seconds startedAt);

} // namespace contour::session
```

Create `src/contour/session/CommandBlockTooltip.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandBlockTooltip.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <format>
#include <string_view>

namespace contour::session
{

namespace
{
    /// What a POSIX shell adds to a signal's number to report the command it ended: 128 + n.
    constexpr auto SignalExitBase = 128;

    /// The largest exit code a shell reports.
    constexpr auto MaxExitCode = 255;

    /// The name of the signal that ended @p record's command, or empty when no signal did.
    ///
    /// From the OSC 3008 outcome when one said so, and otherwise from a shell's 128 + n exit code -- which
    /// is how every POSIX shell spells a signal, though a program may exit with such a code itself.
    [[nodiscard]] std::string_view signalNameOf(vtbackend::CommandBlockRecord const& record) noexcept
    {
        if (record.outcome.signal != vtbackend::ContextSignal::None)
            return vtbackend::contextSignalName(record.outcome.signal);
        if (!record.exitCode || *record.exitCode <= SignalExitBase || *record.exitCode > MaxExitCode)
            return {};
        auto const number = static_cast<uint8_t>(*record.exitCode - SignalExitBase);
        if (!vtbackend::isKnownContextSignal(number))
            return {};
        return vtbackend::contextSignalName(static_cast<vtbackend::ContextSignal>(number));
    }

    /// The first line: how the command ended, or that it has not.
    [[nodiscard]] std::string statusLineOf(vtbackend::CommandBlockRecord const& record)
    {
        if (record.state == vtbackend::CommandBlockState::Running)
            return "Running";
        if (!record.exitCode)
            return "Finished, exit code unknown";
        auto const signal = signalNameOf(record);
        if (signal.empty())
            return std::format("Exit code {}", *record.exitCode);
        return std::format("Exit code {} ({})", *record.exitCode, signal);
    }

    /// @p text, safe to show: controls and bidi overrides replaced.
    [[nodiscard]] std::string displayable(std::string_view text)
    {
        return vtbackend::sanitizeCommandLine(text, vtbackend::SanitizePurpose::Display);
    }
} // namespace

std::string commandBlockTooltipText(vtbackend::CommandBlockRecord const& record, std::chrono::local_seconds startedAt)
{
    if (record.state == vtbackend::CommandBlockState::Prompting)
        return {};

    auto text = statusLineOf(record);
    if (record.duration)
        text += std::format("\nTook {}", vtbackend::formatCommandDuration(*record.duration));
    text += std::format("\nStarted {:%Y-%m-%d %H:%M:%S}", startedAt);
    if (!record.workingDirectory.path.empty())
        text += std::format("\nIn {}", displayable(record.workingDirectory.path));
    if (!record.commandLine.empty())
        text += std::format("\n$ {}", displayable(record.commandLine));
    return text;
}

} // namespace contour::session
```

- [ ] **Step 4: Run them to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `out/build/clangcl-debug/bin/contour_gui_test.exe "CommandBlockTooltip.*"`
Expected: PASS.

- [ ] **Step 5: Write the failing session and QML tests**

Append to `src/contour/session/TerminalSession_test.cpp` (add `#include <QtTest/QSignalSpy>` to its includes if absent):

```cpp
TEST_CASE("TerminalSession: hovering a block's gutter row describes the block", "[contour][session][gutter]")
{
    using contour::session::ConsumedByGutter;
    using contour::session::TerminalSession;
    using vtbackend::GutterSegmentKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    auto spy = QSignalSpy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

    REQUIRE(session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn)) == ConsumedByGutter::Yes);
    CHECK(spy.count() == 1);
    auto const text = session->commandBlockTooltip().toStdString();
    CHECK(text.starts_with("Exit code 2\n"));
    CHECK(text.contains("$ make"));
    CHECK(session->commandBlockTooltipAnchor().width() > 0);

    // Another row of the same block: the tooltip stays where the block was entered.
    session->sendGutterHoverEvent(gutterHit(1, GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 1);

    // The line numbers beside it are not the block column.
    session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::LineNumber));
    CHECK(spy.count() == 2);
    CHECK(session->commandBlockTooltip().isEmpty());

    // Back on the block, then off the gutter altogether.
    session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn));
    session->sendGutterHoverEvent(std::nullopt);
    CHECK(session->commandBlockTooltip().isEmpty());
    CHECK(session->commandBlockTooltipAnchor().isEmpty());
}

TEST_CASE("TerminalSession: without shell integration the block column shows no tooltip",
          "[contour][session][gutter]")
{
    // Review Focus #5, at the tooltip.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("plain output\r\n");
    auto spy = QSignalSpy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 0);
    CHECK(session->commandBlockTooltip().isEmpty());
}
```

In `src/contour/test/QmlComponents_test.cpp`, extend `class MockSession`: after its `hyperlinkTooltipAnchor` `Q_PROPERTY` add

```cpp
    // Mirrors TerminalSession's block-column tooltip: the anchor, and the invocable its text is read
    // through whenever the anchor moves.
    Q_PROPERTY(QRectF commandBlockTooltipAnchor READ commandBlockTooltipAnchor NOTIFY commandBlockHoverChanged)
```

after `hyperlinkTooltipAnchor()` add

```cpp
    [[nodiscard]] QRectF commandBlockTooltipAnchor() const { return _commandBlockTooltipAnchor; }
    Q_INVOKABLE QString commandBlockTooltip() const { return _commandBlockTooltipText; }
```

after `setHyperlinkHover(…)` add

```cpp
    /// Publishes a hovered command block, the way TerminalSession does when the pointer reaches one.
    void setCommandBlockHover(QString text, QRectF anchor)
    {
        _commandBlockTooltipText = std::move(text);
        _commandBlockTooltipAnchor = anchor;
        emit commandBlockHoverChanged();
    }
```

after `void hyperlinkHoverChanged();` in its `signals:` add `    void commandBlockHoverChanged();`, and after `QRectF _hyperlinkTooltipAnchor;` add

```cpp
    QString _commandBlockTooltipText;
    QRectF _commandBlockTooltipAnchor;
```

Then append, after `TEST_CASE("SessionChrome shows the hyperlink tooltip only while there is a link under the pointer", …)`:

```cpp
TEST_CASE("SessionChrome shows the command block tooltip only while a block is hovered",
          "[contour][gui][qml][gutter]")
{
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto session = createScrollableSession();
    host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(session.get())));
    QCoreApplication::processEvents();

    auto* tip = host.chrome->findChild<QQuickItem*>(QStringLiteral("commandBlockTooltip"));
    REQUIRE(tip != nullptr);
    CHECK(tip->property("tipText").toString().isEmpty());

    SECTION("a hovered block publishes its text, beside its gutter cell")
    {
        session->setCommandBlockHover("Exit code 2\n$ make", QRectF(4, 80, 8, 16));
        QCoreApplication::processEvents();

        CHECK(tip->property("tipText").toString() == "Exit code 2\n$ make");
        CHECK(tip->x() == Catch::Approx(12)); // on the grid side of the cell, not over the strip
    }

    SECTION("leaving the block withdraws it")
    {
        session->setCommandBlockHover("Exit code 2", QRectF(4, 80, 8, 16));
        QCoreApplication::processEvents();
        REQUIRE_FALSE(tip->property("tipText").toString().isEmpty());

        session->setCommandBlockHover(QString(), QRectF());
        QCoreApplication::processEvents();
        CHECK(tip->property("tipText").toString().isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
```

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL to compile — `'commandBlockHoverChanged': is not a member of 'contour::session::TerminalSession'`.

- [ ] **Step 6: Publish the hovered block**

In `src/contour/session/TerminalSession.hpp`, after the two `hyperlinkTooltip…` `Q_PROPERTY` lines insert:

```cpp
    // The command block under the pointer in the gutter's block column; an empty anchor means no tooltip.
    // The text is read through commandBlockTooltip() whenever the anchor moves.
    Q_PROPERTY(QRectF commandBlockTooltipAnchor READ commandBlockTooltipAnchor NOTIFY commandBlockHoverChanged)
```

after `hyperlinkTooltipAnchor()` insert:

```cpp

    /// The gutter cell the hovered command block was entered at, in the display's item-local logical
    /// coordinates; empty while no block is hovered. The ENTRY cell, so the tooltip stays put while the
    /// pointer traces the block's rows.
    [[nodiscard]] QRectF commandBlockTooltipAnchor() const noexcept { return _commandBlockTooltipAnchor; }

    /// What the block-column tooltip says about the hovered command block, or empty for "show nothing"
    /// (@see commandBlockTooltipText). Reads the record under the terminal lock, at the moment it is asked.
    Q_INVOKABLE QString commandBlockTooltip() const;
```

in `signals:` after `void hyperlinkHoverChanged();` insert:

```cpp
    /// The hovered command block changed, or the pointer left it: re-read commandBlockTooltip().
    void commandBlockHoverChanged();
```

in the private section, after `void clearGutterHover();` insert:

```cpp

    /// Points the block-column tooltip at @p block, entered at screen row @p screenRow, or withdraws it.
    /// Announces only a change of BLOCK: tracing one block's rows keeps the tooltip where it was entered.
    void updateCommandBlockHover(std::optional<vtbackend::CommandBlockId> block, vtbackend::LineOffset screenRow);
```

and after `QRectF _hyperlinkTooltipAnchor;` insert:

```cpp

    /// The command block whose tooltip is showing, if any (@see commandBlockTooltip).
    std::optional<vtbackend::CommandBlockId> _hoveredBlock {};
    QRectF _commandBlockTooltipAnchor {};
```

In `src/contour/session/TerminalSession.cpp`, add `#include <contour/session/CommandBlockTooltip.hpp>` to the contour includes; in `sendGutterHoverEvent` replace:

```cpp
    auto const overFold = core::locked(_terminal, [&] {
        _terminal.setGutterHoverLine(onBlockColumn ? std::optional { hit->gridLine } : std::nullopt);
        return onBlockColumn && _terminal.foldContaining(hit->gridLine).has_value();
    });
```

with:

```cpp
    auto const [overFold, block] = core::locked(_terminal, [&] {
        _terminal.setGutterHoverLine(onBlockColumn ? std::optional { hit->gridLine } : std::nullopt);
        auto const* record = onBlockColumn ? _terminal.commandBlockAt(hit->gridLine) : nullptr;
        return std::pair { onBlockColumn && _terminal.foldContaining(hit->gridLine).has_value(),
                           record != nullptr ? std::optional { record->id } : std::nullopt };
    });

    // The tooltip describes a BLOCK, so it follows the block column only, and moves on only when the
    // pointer reaches another block.
    updateCommandBlockHover(block, hit->screenRow);
```

In `clearGutterHover()`, after `_gutterHovered = false;` insert:

```cpp
    updateCommandBlockHover(std::nullopt, vtbackend::LineOffset(0));
```

In `onScrollOffsetChanged`, after `clearHyperlinkHover();` insert:

```cpp
    // Likewise the block tooltip: a different block may now sit beside the stationary pointer.
    updateCommandBlockHover(std::nullopt, vtbackend::LineOffset(0));
```

After the closing brace of `TerminalSession::clearGutterHover` insert:

```cpp

void TerminalSession::updateCommandBlockHover(std::optional<vtbackend::CommandBlockId> block,
                                              vtbackend::LineOffset screenRow)
{
    if (_hoveredBlock == block)
        return;

    _hoveredBlock = block;
    // The block column is the gutter's last column, -1, whatever else the gutter shows.
    _commandBlockTooltipAnchor =
        block && _display != nullptr
            ? geometry::cellRectangle(_display->gridMetrics().pageMargin,
                                      _display->cellSize(),
                                      vtbackend::CellLocation { .line = screenRow, .column = vtbackend::ColumnOffset(-1) },
                                      1,
                                      _display->devicePixelRatio())
            : QRectF {};
    emit commandBlockHoverChanged();
}

QString TerminalSession::commandBlockTooltip() const
{
    if (!_hoveredBlock)
        return {};

    auto const lock = std::scoped_lock { _terminal };
    auto const* record = _terminal.commandBlocks().find(*_hoveredBlock);
    if (record == nullptr)
        return {};

    // When the command started; a block that never reached its command has only its prompt's time.
    auto const startedAt = _terminal.localTimeOf(record->commandStartedAt.value_or(record->promptStartedAt));
    return QString::fromStdString(commandBlockTooltipText(*record, startedAt));
}
```

- [ ] **Step 7: Show it**

In `src/contour/qml/SessionChrome.qml`, after the closing `}` of the `hyperlinkTip` item insert:

```qml

    // The gutter's block-column tooltip: how the hovered command ended, how long it took, when and where it
    // ran, and what it was. The anchor is a NOTIFY property; the text is read through the session's
    // commandBlockTooltip() each time the anchor moves, so a hover that never happens formats nothing.
    // Guarded against mock sessions without the members, as hyperlinkTip is.
    Item {
        id: blockTip
        objectName: "commandBlockTooltip"

        readonly property rect anchorRect: (chrome.session && chrome.session.commandBlockTooltipAnchor !== undefined)
                                           ? chrome.session.commandBlockTooltipAnchor
                                           : Qt.rect(0, 0, 0, 0)
        readonly property string tipText: (anchorRect.width > 0
                                           && typeof chrome.session.commandBlockTooltip === "function")
                                          ? chrome.session.commandBlockTooltip()
                                          : ""
        readonly property bool showAbove: anchorRect.y > height + 8

        width: 1
        height: 1
        // On the grid side of the gutter cell: over the strip it would cover the column being read.
        x: Math.max(0, Math.min(anchorRect.x + anchorRect.width, chrome.width - 1))
        y: showAbove ? anchorRect.y : anchorRect.y + anchorRect.height

        ToolTip.text: blockTip.tipText
        ToolTip.visible: blockTip.tipText !== ""
                         && chrome.Window.window !== null
                         && chrome.Window.window.active
        ToolTip.delay: 600
    }
```

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `out/build/clangcl-debug/bin/contour_gui_test.exe "[gutter]"` and `out/build/clangcl-debug/bin/contour_gui_test.exe "[qml]"`
Expected: PASS, with no QML diagnostics — every other `[qml]` case binds `SessionChrome` to a mock without the new members, which the guards absorb.

- [ ] **Step 9: clang-tidy hand audit, format, commit**

Check the added `src/contour/**` lines: `signalNameOf`, `statusLineOf`, `displayable` sit in `CommandBlockTooltip.cpp`'s anonymous namespace (`misc-use-internal-linkage`); `text` in `commandBlockTooltipText` is the only mutable local; `_hoveredBlock` and `_commandBlockTooltipAnchor` carry default initialisers (`cppcoreguidelines-pro-type-member-init`).

```bash
clang-format -i src/contour/session/CommandBlockTooltip.hpp src/contour/session/CommandBlockTooltip.cpp src/contour/session/CommandBlockTooltip_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/test/QmlComponents_test.cpp
git add src/contour/session/CommandBlockTooltip.hpp src/contour/session/CommandBlockTooltip.cpp src/contour/session/CommandBlockTooltip_test.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp src/contour/qml/SessionChrome.qml src/contour/test/QmlComponents_test.cpp src/contour/CMakeLists.txt
git commit -F - <<'EOF'
contour: describe a command block in a tooltip over the gutter

Hovering the block column publishes the block under the pointer; the
tooltip reads its record -- exit code and signal, duration, start time,
working directory, command line -- with both strings sanitised for
display. It stays where the block was entered and withdraws on scroll.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

---

### Task 4.14: Phase gate

**Files:** whatever `/simplify` and the review change; nothing else.

- [ ] **Step 1: Build everything, warning-free**

Run: `cmake --build --preset clangcl-debug`
Expected: success with zero warnings. On this machine's pre-existing `src/contour/display/*` `yaml-cpp/emitter.h` failure, build the test targets instead and record the break as pre-existing:
`cmake --build --preset clangcl-debug --target vtbackend_test vtparser_test vtrasterizer_test vthost_test vtworkspace_test contour_test contour_gui_test`

- [ ] **Step 2: Run the phase's tests, then the suite**

Run, in order:
- `out/build/clangcl-debug/bin/vtbackend_test.exe "[gutter]"`, `"[grid][evicted]"`, `"[folding]"`, `"[ColorPalette]"`, `"CommandBlock.formatCommandDuration*"`
- `out/build/clangcl-debug/bin/vtrasterizer_test.exe "[gutter]"` and `"BoxDrawingRenderer.gutterStatusMarks*"`
- `out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`, `"[gutter]"`, `"[geometry]"`, `"[qml]"`
- `ctest --test-dir out/build/clangcl-debug --output-on-failure`
- `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`

Expected: every test green except the failures recorded in the phase-0 baseline (e.g. `vtconformance_vttest` without `vttest -c`); spelling PASS (or SKIP when `typos` is absent, as at phase 0). Note the `N tests passed, M failed` line.

- [ ] **Step 3: Review Focus self-check** — confirm, by name, that the tests the README assigns to this phase are present and green:
  2. #2 `Gutter.terminal.theAlternateScreenHasAnEmptyGutter` (no gutter content and no fold label on the alternate screen; both back on leaving it);
  5. #5 `Gutter.terminal.withoutShellIntegrationTheBlockColumnStaysEmpty`, `Folding.terminal.noGutterWithoutMarkersOrWithoutFolds` (section "no shell integration") and `TerminalSession: without shell integration the block column shows no tooltip`.
  Also run `git grep -n "foldMarkers" -- src`: only `GutterSettings::foldMarkers` and its uses remain — `Settings::foldMarkers` is gone.

- [ ] **Step 4: `/simplify`** over `git diff <phase-4-start>..HEAD` (the hash noted in "Before you start"). Candidates it should weigh: the row walk `fillGutter` and `fillFoldLabels` both make (one walk handing each row to both, if it stays readable); the `runCommand`/`startCommand` helpers now in both `Folding_test.cpp` and `Terminal_gutter_test.cpp` (one home in `src/vtbackend/testing/`); `RenderAttributes` built from an `RGBColorPair` in three places in `Gutter.cpp` and `Terminal.cpp`; the two `updateCommandBlockHover(std::nullopt, LineOffset(0))` call sites. Rebuild, rerun Step 2, then commit its fixes:

```bash
git add -u
git commit -F - <<'EOF'
vtbackend: simplify the gutter

<one line per change /simplify made>

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(No commit when it changed nothing.)

- [ ] **Step 5: Code review at xhigh** — `/code-review xhigh` on `<phase-4-start>..HEAD` (or a review subagent dispatched with `effort: "xhigh"`). Point the reviewer at: `Grid::syncStableFloor` counting every floor advance and `Grid::resize` putting a column reflow's advance back (does any path move `_stableFloor` without `syncStableFloor()`?); the per-frame cost of `fillGutter` (one `commandBlockAt` and one cached `foldRanges()` lookup per drawn row; `gutterLayoutFor` per frame formats a sample only with timestamps on); `Settings{}` still reserving no gutter (every `GutterSettings` switch defaults off, Decision 8) while `config::GutterConfig{}` reserves one column, and every vtbackend test setting the switches it relies on explicitly; the C2 deviation (`CommandBlockOutcome` in `core/`); the `std::formatter<LineNumberMode>` spelling kept equal to the `ConfigEnum` table by a test rather than by construction; the hover and tooltip paths' locking and threads (`commandBlockTooltip()` takes the terminal lock on the GUI thread; `onScrollOffsetChanged()`, which can run on the parser thread during auto-scroll, now resets `_hoveredBlock` exactly as it already resets the hyperlink hover — run `contour_gui_test "[gutter]"` under the `clang-tsan` preset in WSL if in doubt); `configureTerminal()` posting a refit; the `ContextTintEntry` writer defect noted in Task 4.10; and the "Decisions this phase makes" list at the top of this file. Fix every confirmed finding test-first, rebuild, rerun Step 2, and commit:

```bash
git add -u
git commit -F - <<'EOF'
vtbackend: address the phase 4 review

<one line per finding fixed>

ctest: <N> passed, <M> failed (<names of the baseline failures>)

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

The last commit of the phase carries the `ctest` summary line in its body. If neither Step 4 nor Step 5 changed anything, make no empty commit — report the summary line to the coordinator instead (phase 0's rule).

- [ ] **Step 6: Report** to the coordinating session: the ctest summary, the commits of this phase (`git log --oneline <phase-4-start>..HEAD`), any finding left open, and the contract notes at the top of this file. The coordinator records progress and propagates the contract notes into the README.

## Notes for later phases

- **Phase 5 (block actions):** `ClearToPrompt` must drop rows through `Grid::syncStableFloor()` — never by assigning `_stableFloor` — and `Grid::evictedRowCount()` then counts them with no further change. `Terminal::collapseFoldContaining()` exists if a block action wants it.
- **Phase 7 (sticky header):** `SessionInput.cpp`'s `gutterLineAt` is now `gutterHitAt`, returning `std::optional<GutterHit>`; `TerminalSession::sendGutterHoverEvent` and `sendGutterPressEvent` take that type. `vtrasterizer::annotationRenderCells()` (`GutterCells.hpp`) is the u32-text → `RenderCell` helper the header may reuse; `Terminal::drawnRowLimit()` says how far down the page rows are drawn.
- **Phases 6 and 7 (notification body, sticky-header chip):** `vtbackend::formatCommandDuration(std::chrono::steady_clock::duration)` (`shell/CommandBlock.hpp`, Task 4.13a) is the one duration formatter — `850ms`, `12s`, `3m 05s`, `1h 02m`, negative reads `0ms`. Call it; do not write another.
- **Phase 8 (scrollbar marks):** the gutter draws a user mark in `foldMarkerHoverColors().foreground` and a block in `blockStatusColor(outcomeOf(record))`, which is what the ticks should match.
- **Phase 10 (settings and docs):** `configEnumValues<vtbackend::LineNumberMode>()` exists; the gutter formats a `std::chrono::local_seconds`, and `vtbackend::measureTimestampFormat()` is the check the loader uses, and the settings row calls it and `timestampFormatErrorText()` directly (no wrapper); the `GutterConfig` loader writes only the children present; `configureTerminal()` re-reads the gutter on every reload and refits the page when its width changed.
