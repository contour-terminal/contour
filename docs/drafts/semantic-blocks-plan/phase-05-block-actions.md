# Phase 5 — Acting on a block

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a command block something the user can act on: select it (Ctrl+triple-click, an action, a context-menu row), copy its output or its command line, open its output in a pager (Ctrl+Shift+G), clear everything above the current prompt, and resolve a path printed by a command against the directory that command ran in.

**Architecture:** Every action resolves a *target* (`pointer`, `cursor`, `last`, or a block id pinned by a menu row) to an anchor row on the primary grid, and from it, through one pure function over an injected row source (`commandBlockRows`), to the inclusive rows of the requested part. `Terminal` turns those rows into a selection or text; `TerminalSession` turns that into the clipboard, a pager pane or a launched program. Multi-click counting moves out of `Terminal` into a `ClickCounter` with an injected clock, so `TerminalSession` can offer the click count to the user's bindings *before* the terminal's own word/line selection runs. Clear-to-prompt is a `Grid` operation, so the eviction counter (C2), the stable floor and fold pruning follow from the grid's own bookkeeping. Pager files live in a private per-process directory reached through `core::platform::FileSystem`.

**Tech Stack:** C++23, Catch2 (`vtbackend_test`, `contour_test`, `contour_gui_test`), Qt 6 (`QProcess` in the launcher adapter only), yaml-cpp, `core::platform` (`IClock`, `ManualClock`, `FileSystem`, `InMemoryFileSystem`), CMake presets (`clangcl-debug`).

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §7.1–7.4 (all of it), §11 (the `command_blocks.pager` row), §13.2 (owner-only output files, foreign paths), §13.4 (what returns `std::expected`), §14 (the Actions test row), §16.1 (the Ctrl+Shift+G check). Global constraints: [README](README.md#global-constraints). Interface contract: [README C1, C2, C5, C6, C9](README.md#interface-contract) — this phase produces **C5** (`ClickCounter`) and the `pager` half of **C9**; it consumes C1 (block store, `Terminal::commandBlockAt`, `CommandBlockTarget`, `WorkingDirectorySnapshot`), C2 (`Grid::evictedRowCount`, advanced by `syncStableFloor()`) and C6 (`sanitizeCommandLine`).

## Before you start

- [ ] Record the phase start: run `git rev-parse HEAD` and note the hash as `PHASE5_START` (the phase gate diffs against it).
- [ ] Confirm the symbols this phase calls exist on the branch. Run each and expect at least one hit; a missing one is a stop-and-report to the coordinator, never a reason to invent it:
  - `git grep -n "CommandBlockRecord const\* commandBlockAt" src/vtbackend/screen/Terminal.hpp`
  - `git grep -n "enum class CommandBlockTarget" src/vtbackend/shell/CommandBlock.hpp`
  - `git grep -n "struct WorkingDirectorySnapshot" src/vtbackend/shell/CommandBlock.hpp`
  - `git grep -n "CommandBlockId blockId() const" src/vtbackend/grid/Line.hpp`
  - `git grep -n "void adoptBlock" src/vtbackend/grid/Line.hpp`
  - `git grep -n "adopt(CommandBlockRecord" src/vtbackend/shell/CommandBlock.hpp`
  - `git grep -n "uint64_t evictedRowCount() const" src/vtbackend/grid/Grid.hpp`
  - `git grep -n "_evictedRowCount +=" src/vtbackend/grid/Grid.hpp` (the count lives in `syncStableFloor()`; this phase never touches the field)
  - `git grep -n "sanitizeCommandLine" src/vtbackend/shell/CommandLineSanitizer.hpp`
  - `git grep -n "struct CommandBlocksConfig" src/contour/config/Config.hpp` and `git grep -n "ConfigEntry<CommandBlocksConfig" src/contour/config/Config.hpp` — note the `Config` member's name. This plan writes `commandBlocks`; if phase 1 chose another, use that spelling everywhere this plan writes `commandBlocks`.
  - `git grep -n "TerminalClocks clocks" src/vtbackend/screen/Terminal.hpp`
  - `git grep -n "core::platform" src/vtbackend/CMakeLists.txt` (phase 1 links it for `TerminalClocks`)
  - `git grep -n "steadyClock" src/vtbackend/testing/MockTerm.hpp` (phase 1: a `core::platform::ManualClock`)
- [ ] Every new test carries the tag `[semanticblocks]`, so `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"` and `out/build/clangcl-debug/bin/contour_gui_test.exe "[semanticblocks]"` run the whole phase.
- [ ] `contour_gui_test` runs with `QT_QPA_PLATFORM=offscreen` (Git Bash: `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "<tag>"`). If it exits with `0xC0000135`, apply the README's Qt-DLL `PATH` workaround.

## Contract notes (deviations and additions — the coordinator propagates these to the README)

1. **C5 as produced:** `vtbackend::ClickCounter(core::platform::IClock const& clock, std::chrono::milliseconds interval)`, `[[nodiscard]] uint8_t press(CellLocation position, MouseButton button) noexcept`, plus `void reset() noexcept`, `static constexpr std::chrono::milliseconds DefaultInterval { 1000 }` and `static constexpr uint8_t MaxClickCount = 4`. Each of Left/Right/Middle keeps its own sequence (a right-click does not end a left double-click — today's behaviour); wheel and release presses always count 1.
2. **The launcher error is `SpawnError`, not `LaunchError`.** `LaunchError` exists but is URL-only (`InvalidUrl`, `NoHandler`); every program-launching sibling returns `SpawnError`. Added: `[[nodiscard]] virtual std::expected<void, SpawnError> runWithStdin(QString const& program, QStringList const& arguments, QByteArray input) = 0;` on `contour::platform::ExternalLauncher`.
3. **Actions carry a pinned block.** `SelectCommandBlock`, `CopyCommandBlock`, `CopyCommandLine` and `OpenCommandOutput` gain `vtbackend::CommandBlockId block {}` (zero = resolve `target`; non-zero = this block, which is how a context-menu row acts on the block that was right-clicked). Defaults: `target = Last`, `part = Output`.
4. **New actions beyond the spec's list:** `CopyCommandBlock { target, part, block }` (the "Copy output" row) and `CopyCommandLine { target, block }` (the "Copy command" row).
5. **Click counts reach the binding layer:** `vtbackend::InputBinding` (shared by every `*InputMapping` alias) gains a last member `std::optional<uint8_t> clickCount {}` (and `match()` a trailing `std::optional<uint8_t> clickCount = std::nullopt`); `config::apply(...)` and the **mouse** `applyBuiltinFallback(...)` gain a trailing `std::optional<uint8_t> clickCount = std::nullopt`. An absent count matches every press no click-count row claimed — the spec's "absent = any count".
6. **New defaults are built-in fallbacks, not defaults** (the codebase's rule: a default never reaches a user whose `contour.yml` lists the defaults). Ctrl+Left×3 joins `builtinFallbackMouseMappings()`. Ctrl+Shift+G arrives as a *character*, so a character table is added: `using FallbackCharMapping = FallbackMapping<char32_t>;`, `[[nodiscard]] std::vector<FallbackCharMapping> const& builtinFallbackCharMappings();`, `[[nodiscard]] ActionList const* applyBuiltinFallback(Config const&, char32_t codepoint, vtbackend::Modifiers, uint8_t actualModeFlags);`, consulted by `TerminalSession::sendCharEvent` after the user's character bindings.
7. **Terminal additions:** `enum class MousePressClaim : uint8_t { Unclaimed = 0, Claimed }`, `using MousePressClaimant = std::function<MousePressClaim(uint8_t clickCount)>`, a 6-argument `sendMousePressEvent(..., MousePressClaimant const& claimant)`; `commandBlockRows`, `selectCommandBlock`, `commandBlockText`, `targetedCommandBlock` (Task 5.2), `clearToPrompt` (Task 5.10). New `vtbackend::CommandBlockActionError : uint8_t { NoBlock = 0, EmptyPart }`, `CommandBlockRows`, `CommandBlockRowSource`, `commandBlockRows()`, `bottomRowOfBlock()` (Task 5.1) and `localPathBaseFor()` (Task 5.19).
8. **Grid/Screen additions:** `[[nodiscard]] LineCount Grid::dropRowsAbove(LineOffset head)` and `[[nodiscard]] LineCount Screen::dropRowsAbove(LineOffset head)`. The eviction count follows from `syncStableFloor()`, as C2 requires; `_evictedRowCount` is never written here.
9. **`HintPattern::validator` / `transformer` gain the row:** `std::function<bool(std::string const&, LineOffset)>` and `std::function<std::string(std::string const&, LineOffset)>`.
10. **GUI wiring:** `ContourGuiApp` gains a 7th constructor parameter `std::unique_ptr<session::CommandOutputFiles> commandOutputFiles = nullptr` and `commandOutputFiles()`; `TerminalSessionManager` gains `enum class CommandPanePlacement : uint8_t { Split = 0, Tab }` and `[[nodiscard]] TerminalSession* openCommandPane(TerminalSession* acting, CommandPanePlacement placement, vtpty::Process::ExecInfo const& command)`; `createSessionInBackground` gains a trailing `std::optional<vtpty::Process::ExecInfo> const& command = std::nullopt`; `contour::command::ContextMenuState` gains `vtbackend::CommandBlockId blockUnderCursor {}` and `bool blockUnderCursorHasCommandLine = false`; `TestApp` gains an in-memory filesystem (`fileSystem()`, `OutputDirectory`).
11. **C9 as produced:** `std::string pager { defaultCommandOutputPager() }` with `[[nodiscard]] constexpr std::string_view defaultCommandOutputPager() noexcept` (`"less -R"`, `"more"` on Windows), written single-quoted. The `Config` member is assumed to be `commandBlocks` (C9 names only the key).

## Decisions made here (reviewers may reject any of them)

- **"Copy command" copies the `Insert`-sanitized command line**, not the raw record and not the `Display` form. The clipboard's next stop is a paste, usually into a shell: a raw line can carry `ESC [ 201 ~`, which ends bracketed paste early and runs the rest as typed input; `Display` would put U+FFFD replacement glyphs into what the user pastes. `Insert` strips exactly what is unsafe to paste and keeps the rest byte for byte.
- **Only the primary screen has blocks.** Every target resolves on the primary grid; with an alternate-screen application up, `pointer` resolves to nothing (the pointer is over the alternate screen) while `last` and `cursor` still answer from the primary screen, and selecting refuses (the selection would land on the screen on display).
- **A detached pager always gets plain text.** A program reading stdin may not interpret SGR; `format` applies to split/tab placements only.
- **A Ctrl+triple-click that the binding claims never falls through to word selection**, even over a row that belongs to no block — falling through would make the same gesture do two unrelated things depending on where it landed.
- **Output spans rows, not columns.** A block's output is every row stamped with its id from its output-start row down; an output tail the next prompt was printed onto belongs to that prompt's block (phase 1 stamps whole rows).
- **A split pager is stacked below** the acting pane (`SplitState::Horizontal`): output reads top to bottom and keeps the full width.
- **Path bases:** a block's recorded directory wins whenever it has one (Local *or* Unknown locality — a plain OSC 7 shell reports Unknown); Foreign refuses even absolute paths; a block without a directory, and a row outside any block, keep today's OSC 7 base.
- **ClearToPrompt keeps the block records**, as ED 3 and eviction do; only rows, folds and marks go.
- **The pager file is written before its permissions are narrowed**; that window exposes nothing because the directory admits only its owner. A stale directory with this process's id is removed and recreated; a symlink in its place is refused.

---

### Task 5.1: Block row lookup (`CommandBlockRows`)

**Files:**
- Create: `src/vtbackend/shell/CommandBlockRows.hpp`
- Create: `src/vtbackend/shell/CommandBlockRows.cpp`
- Test: create `src/vtbackend/shell/CommandBlockRows_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (headers after `shell/CommandBlocks.hpp` :23, sources after `shell/CommandBlocks.cpp` :103, tests after `shell/CommandBlocks_test.cpp` :204)

**Interfaces:**
- Consumes: C1 `CommandBlockId` (`core/CommandBlockId.hpp`), `LineFlag::Marked`, `LineFlag::OutputStart` (`core/LineFlags.hpp`), `CommandBlockPart` (`shell/CommandBlocks.hpp`).
- Produces:
  ```cpp
  enum class CommandBlockActionError : uint8_t { NoBlock = 0, EmptyPart };
  [[nodiscard]] constexpr std::string_view describe(CommandBlockActionError error) noexcept;
  struct CommandBlockRows { LineOffset first {}; LineOffset last {}; bool operator==(CommandBlockRows const&) const = default; };
  class CommandBlockRowSource; // topRow(), bottomRow(), blockIdAt(LineOffset), flagsAt(LineOffset)
  [[nodiscard]] std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(CommandBlockRowSource const& rows, LineOffset anchor, CommandBlockPart part);
  [[nodiscard]] std::optional<LineOffset> bottomRowOfBlock(CommandBlockRowSource const& rows, CommandBlockId id);
  ```

- [ ] **Step 1: Write the failing test** — create `src/vtbackend/shell/CommandBlockRows_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockRows.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace vtbackend;

namespace
{

/// One row of a FakeRows grid.
struct FakeRow
{
    uint32_t block = 0;
    LineFlags flags {};
};

/// A grid whose first row is @ref _top, as a scrollback's is (negative offsets are history).
class FakeRows final: public CommandBlockRowSource
{
  public:
    FakeRows(LineOffset top, std::vector<FakeRow> rows): _top { top }, _rows { std::move(rows) } {}

    [[nodiscard]] LineOffset topRow() const override { return _top; }

    [[nodiscard]] LineOffset bottomRow() const override
    {
        return _top + LineOffset::cast_from(_rows.size()) - LineOffset(1);
    }

    [[nodiscard]] CommandBlockId blockIdAt(LineOffset row) const override
    {
        return CommandBlockId(at(row).block);
    }

    [[nodiscard]] LineFlags flagsAt(LineOffset row) const override { return at(row).flags; }

  private:
    [[nodiscard]] FakeRow const& at(LineOffset row) const
    {
        return _rows.at(static_cast<size_t>(unbox(row - _top)));
    }

    LineOffset _top;
    std::vector<FakeRow> _rows;
};

auto const PromptRow = LineFlags { LineFlag::Marked };
auto const OutputRow = LineFlags { LineFlag::OutputStart };

/// Rows -2..4: an unstamped line, then `$ ls` with two lines of output, `$ pwd` with one, and the live prompt.
[[nodiscard]] FakeRows threeBlocks()
{
    return FakeRows { LineOffset(-2),
                      {
                          FakeRow { .block = 0, .flags = {} },        // -2: before shell integration
                          FakeRow { .block = 1, .flags = PromptRow }, // -1: $ ls
                          FakeRow { .block = 1, .flags = OutputRow }, //  0: a
                          FakeRow { .block = 1, .flags = {} },        //  1: b
                          FakeRow { .block = 2, .flags = PromptRow }, //  2: $ pwd
                          FakeRow { .block = 2, .flags = OutputRow }, //  3: /tmp
                          FakeRow { .block = 3, .flags = PromptRow }, //  4: $ (live)
                      } };
}

[[nodiscard]] CommandBlockRows rowsOf(int first, int last)
{
    return CommandBlockRows { .first = LineOffset(first), .last = LineOffset(last) };
}

} // namespace

TEST_CASE("CommandBlockRows.theOutputStartsAtTheOutputStartRow", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    // Any row of the block anchors it: the prompt row, the first and the last output row.
    for (auto const anchor: { -1, 0, 1 })
    {
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Output) == rowsOf(0, 1));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::Prompt) == rowsOf(-1, -1));
        CHECK(commandBlockRows(rows, LineOffset(anchor), CommandBlockPart::PromptAndOutput) == rowsOf(-1, 1));
    }

    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::Output) == rowsOf(3, 3));
    CHECK(commandBlockRows(rows, LineOffset(3), CommandBlockPart::Prompt) == rowsOf(2, 2));
}

TEST_CASE("CommandBlockRows.aBlockWithoutOutputHasOnlyAPrompt", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    CHECK(commandBlockRows(rows, LineOffset(4), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(4), CommandBlockPart::Prompt) == rowsOf(4, 4));
    CHECK(commandBlockRows(rows, LineOffset(4), CommandBlockPart::PromptAndOutput) == rowsOf(4, 4));
}

TEST_CASE("CommandBlockRows.anUnstampedOrMissingRowIsNoBlock", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    CHECK(commandBlockRows(rows, LineOffset(-2), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(commandBlockRows(rows, LineOffset(-3), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(commandBlockRows(rows, LineOffset(5), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
}

TEST_CASE("CommandBlockRows.anEmptyOutputLeavesItsFlagOnTheNextPromptRow", "[semanticblocks][blockrows]")
{
    // `cd /tmp` prints nothing: its ;C marks the row the next ;A then claims, so that row carries both
    // flags and belongs to the NEXT block. Neither block may read that stray OutputStart as its own.
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow },
                                     FakeRow { .block = 2, .flags = PromptRow | OutputRow },
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(1), CommandBlockPart::Prompt) == rowsOf(1, 1));
}

TEST_CASE("CommandBlockRows.aPromptMaySpanSeveralRows", "[semanticblocks][blockrows]")
{
    auto const rows = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = PromptRow }, // ~/src (main)
                                     FakeRow { .block = 1, .flags = {} },        // $ make
                                     FakeRow { .block = 1, .flags = OutputRow }, // built
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(2), CommandBlockPart::Prompt) == rowsOf(0, 1));
    CHECK(commandBlockRows(rows, LineOffset(0), CommandBlockPart::Output) == rowsOf(2, 2));
}

TEST_CASE("CommandBlockRows.aBlockWhoseHeadWasEvictedIsAllOutput", "[semanticblocks][blockrows]")
{
    // The scrollback dropped the prompt and the output-start row; what survives is output.
    auto const rows = FakeRows { LineOffset(-2),
                                 {
                                     FakeRow { .block = 1, .flags = {} },
                                     FakeRow { .block = 1, .flags = {} },
                                     FakeRow { .block = 2, .flags = PromptRow },
                                 } };

    CHECK(commandBlockRows(rows, LineOffset(-2), CommandBlockPart::Output) == rowsOf(-2, -1));
    CHECK(commandBlockRows(rows, LineOffset(-1), CommandBlockPart::Prompt)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(commandBlockRows(rows, LineOffset(-1), CommandBlockPart::PromptAndOutput) == rowsOf(-2, -1));

    // The prompt went, the output-start row survived.
    auto const kept = FakeRows { LineOffset(0),
                                 {
                                     FakeRow { .block = 1, .flags = OutputRow },
                                     FakeRow { .block = 1, .flags = {} },
                                 } };
    CHECK(commandBlockRows(kept, LineOffset(1), CommandBlockPart::Output) == rowsOf(0, 1));
    CHECK(commandBlockRows(kept, LineOffset(1), CommandBlockPart::Prompt)
          == std::unexpected(CommandBlockActionError::EmptyPart));
}

TEST_CASE("CommandBlockRows.bottomRowOfBlockFindsTheLowestStampedRow", "[semanticblocks][blockrows]")
{
    auto const rows = threeBlocks();

    CHECK(bottomRowOfBlock(rows, CommandBlockId(1)) == LineOffset(1));
    CHECK(bottomRowOfBlock(rows, CommandBlockId(2)) == LineOffset(3));
    CHECK(bottomRowOfBlock(rows, CommandBlockId(3)) == LineOffset(4));
    CHECK_FALSE(bottomRowOfBlock(rows, CommandBlockId(9)).has_value());
    CHECK_FALSE(bottomRowOfBlock(rows, CommandBlockId {}).has_value());
}

TEST_CASE("CommandBlockRows.describeNamesEveryError", "[semanticblocks][blockrows]")
{
    STATIC_CHECK(!describe(CommandBlockActionError::NoBlock).empty());
    STATIC_CHECK(!describe(CommandBlockActionError::EmptyPart).empty());
}
```

Then register it in `src/vtbackend/CMakeLists.txt`, in the `vtbackend_test` list, after `        shell/CommandBlocks_test.cpp` (:204):

```cmake
        shell/CommandBlockRows_test.cpp
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `cannot open include file: 'vtbackend/shell/CommandBlockRows.hpp'`.

- [ ] **Step 3: Write the header** — create `src/vtbackend/shell/CommandBlockRows.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlocks.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace vtbackend
{

/// Why an action on a command block found nothing to act on.
enum class CommandBlockActionError : uint8_t
{
    NoBlock = 0, ///< The targeted row belongs to no block, or the block has left the grid.
    EmptyPart,   ///< The block exists but has none of the requested part (no output yet, prompt evicted).
};

/// A short, human-readable reason for @p error, for logs and status messages.
/// @param error The error.
/// @return A lower-case phrase without a trailing period.
[[nodiscard]] constexpr std::string_view describe(CommandBlockActionError error) noexcept
{
    switch (error)
    {
        case CommandBlockActionError::NoBlock: return "no command block there";
        case CommandBlockActionError::EmptyPart: return "the command block has none of the requested part";
    }
    return "unknown command block error";
}

/// An inclusive range of grid rows (0 = page top, negative = into the scrollback).
struct CommandBlockRows
{
    LineOffset first {}; ///< Topmost row.
    LineOffset last {};  ///< Bottommost row; never above @ref first.

    bool operator==(CommandBlockRows const&) const = default;
};

/// The physical rows a block lookup walks, with the block id and the flags each one carries.
///
/// The seam that keeps commandBlockRows() a pure function: Terminal adapts the primary grid to it, the
/// tests a vector. Only rows in [topRow(), bottomRow()] are ever asked about.
class CommandBlockRowSource
{
  public:
    CommandBlockRowSource() = default;
    CommandBlockRowSource(CommandBlockRowSource&&) = default;
    CommandBlockRowSource(CommandBlockRowSource const&) = default;
    CommandBlockRowSource& operator=(CommandBlockRowSource&&) = default;
    CommandBlockRowSource& operator=(CommandBlockRowSource const&) = default;
    virtual ~CommandBlockRowSource() = default;

    /// The topmost row that still holds valid data (Grid::addressableTop()).
    [[nodiscard]] virtual LineOffset topRow() const = 0;

    /// The bottommost row (the page's last one).
    [[nodiscard]] virtual LineOffset bottomRow() const = 0;

    /// The id of the block @p row belongs to; zero when it belongs to none.
    [[nodiscard]] virtual CommandBlockId blockIdAt(LineOffset row) const = 0;

    /// The line flags of @p row.
    [[nodiscard]] virtual LineFlags flagsAt(LineOffset row) const = 0;
};

/// The rows of one part of the block that @p anchor belongs to.
///
/// A block is the run of adjacent rows stamped with the anchor's id. Its prompt starts at its head, the
/// top row when that row carries LineFlag::Marked; its output starts at the first row below the head
/// carrying LineFlag::OutputStart and runs to the block's last row. An OutputStart on the head row itself
/// is ignored: a command that printed nothing leaves its ;C flag on the row the next ;A claims. A block
/// whose head has been evicted is output from its top row unless an OutputStart row survives.
/// @param rows The grid to walk.
/// @param anchor Any row of the block.
/// @param part Which part of the block to return.
/// @return The rows, or NoBlock (anchor outside the grid or unstamped) or EmptyPart.
[[nodiscard]] std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(
    CommandBlockRowSource const& rows, LineOffset anchor, CommandBlockPart part);

/// The lowest row stamped with @p id, scanning upwards from the bottom of the grid.
/// @param rows The grid to walk.
/// @param id The block; zero never matches.
/// @return The row, or nullopt when no row carries @p id any more.
[[nodiscard]] std::optional<LineOffset> bottomRowOfBlock(CommandBlockRowSource const& rows, CommandBlockId id);

} // namespace vtbackend
```

- [ ] **Step 4: Write the implementation** — create `src/vtbackend/shell/CommandBlockRows.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockRows.hpp>

#include <ranges>

namespace vtbackend
{

namespace
{
    /// The topmost row of the run of rows carrying @p id that contains @p anchor.
    [[nodiscard]] LineOffset topOfRun(CommandBlockRowSource const& rows, LineOffset anchor, CommandBlockId id)
    {
        auto top = anchor;
        for (auto const distance: std::views::iota(1, unbox(anchor - rows.topRow()) + 1))
        {
            auto const row = anchor - LineOffset(distance);
            if (rows.blockIdAt(row) != id)
                break;
            top = row;
        }
        return top;
    }

    /// The bottommost row of the run of rows carrying @p id that contains @p anchor.
    [[nodiscard]] LineOffset bottomOfRun(CommandBlockRowSource const& rows, LineOffset anchor, CommandBlockId id)
    {
        auto bottom = anchor;
        for (auto const distance: std::views::iota(1, unbox(rows.bottomRow() - anchor) + 1))
        {
            auto const row = anchor + LineOffset(distance);
            if (rows.blockIdAt(row) != id)
                break;
            bottom = row;
        }
        return bottom;
    }

    /// The first row in [@p from, @p to] carrying LineFlag::OutputStart.
    [[nodiscard]] std::optional<LineOffset> firstOutputRow(CommandBlockRowSource const& rows,
                                                           LineOffset from,
                                                           LineOffset to)
    {
        for (auto const row: std::views::iota(unbox(from), unbox(to) + 1))
            if (rows.flagsAt(LineOffset(row)).test(LineFlag::OutputStart))
                return LineOffset(row);
        return std::nullopt;
    }
} // namespace

std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(CommandBlockRowSource const& rows,
                                                                          LineOffset anchor,
                                                                          CommandBlockPart part)
{
    if (anchor < rows.topRow() || anchor > rows.bottomRow())
        return std::unexpected(CommandBlockActionError::NoBlock);

    auto const id = rows.blockIdAt(anchor);
    if (id.value == 0)
        return std::unexpected(CommandBlockActionError::NoBlock);

    auto const block = CommandBlockRows { .first = topOfRun(rows, anchor, id), .last = bottomOfRun(rows, anchor, id) };
    auto const headSurvives = rows.flagsAt(block.first).test(LineFlag::Marked);

    auto const outputStart = [&]() -> std::optional<LineOffset> {
        if (!headSurvives)
            return firstOutputRow(rows, block.first, block.last).value_or(block.first);
        if (block.first == block.last)
            return std::nullopt;
        return firstOutputRow(rows, block.first + LineOffset(1), block.last);
    }();

    switch (part)
    {
        case CommandBlockPart::Output:
            if (!outputStart)
                return std::unexpected(CommandBlockActionError::EmptyPart);
            return CommandBlockRows { .first = *outputStart, .last = block.last };
        case CommandBlockPart::Prompt:
            if (!headSurvives)
                return std::unexpected(CommandBlockActionError::EmptyPart);
            if (!outputStart)
                return block;
            return CommandBlockRows { .first = block.first, .last = *outputStart - LineOffset(1) };
        case CommandBlockPart::PromptAndOutput: return block;
    }
    return std::unexpected(CommandBlockActionError::NoBlock);
}

std::optional<LineOffset> bottomRowOfBlock(CommandBlockRowSource const& rows, CommandBlockId id)
{
    if (id.value == 0)
        return std::nullopt;

    auto const height = unbox(rows.bottomRow() - rows.topRow()) + 1;
    for (auto const distance: std::views::iota(0, height))
    {
        auto const row = rows.bottomRow() - LineOffset(distance);
        if (rows.blockIdAt(row) == id)
            return row;
    }
    return std::nullopt;
}

} // namespace vtbackend
```

Register both in `src/vtbackend/CMakeLists.txt`: after `    shell/CommandBlocks.hpp` (:23) add `    shell/CommandBlockRows.hpp`, and after `    shell/CommandBlocks.cpp` (:103) add `    shell/CommandBlockRows.cpp`. Run `clang-format -i src/vtbackend/shell/CommandBlockRows.hpp src/vtbackend/shell/CommandBlockRows.cpp src/vtbackend/shell/CommandBlockRows_test.cpp`.

Note on the evicted-head rule: `firstOutputRow(...).value_or(block.first)` — with the head gone, a surviving OutputStart row splits off a prompt remnant that cannot be shown as a prompt (its head is gone), so the remnant above it is simply not part of `Output`; without one, every surviving row is output.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockrows]"`
Expected: build with zero warnings; `All tests passed` (8 test cases).

- [ ] **Step 6: Commit**

```bash
git add src/vtbackend/shell/CommandBlockRows.hpp src/vtbackend/shell/CommandBlockRows.cpp src/vtbackend/shell/CommandBlockRows_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: find the rows of a command block's prompt and output

Every block action (select, copy, open in a pager, clear to prompt) starts
from one row of a block and needs the rows of one part of it. Do that once,
as a pure function over a row source, so the rule is tested without a grid:
a block is the run of rows stamped with one id, its prompt starts at a
Marked head and its output at the first OutputStart row below it.

An OutputStart on the head row is ignored -- a command that printed nothing
leaves its flag on the row the next prompt claims -- and a block whose head
was evicted reads as output.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.2: Select and render a block by target (`Terminal`)

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp` (include block :4-30; public API next to `lastCommandBlock()` :1729; private helpers next to `handleMouseSelection` :2327)
- Modify: `src/vtbackend/screen/Terminal.cpp` (anonymous namespace :63-165; `selectAll()` :2445-2473)
- Test: create `src/vtbackend/screen/Terminal_block_actions_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (tests after `screen/Terminal_selection_test.cpp` :240)

**Interfaces:**
- Consumes: Task 5.1; C1 `Terminal::commandBlocks()`, `CommandBlockStore::lastFinished()`/`find()`, `Terminal::commandBlockAt()`, `Line::blockId()`, `CommandBlockTarget`; `Grid::renderRange`, `CaptureRendition`, `CaptureTrailingSpaces`, `Grid::addressableTop()`, `Grid::isLineWrapped()`.
- Produces (public on `vtbackend::Terminal`):
  ```cpp
  [[nodiscard]] std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(CommandBlockTarget target, CommandBlockPart part, CommandBlockId pinned = {}) const;
  [[nodiscard]] std::expected<void, CommandBlockActionError> selectCommandBlock(CommandBlockTarget target, CommandBlockPart part, CommandBlockId pinned = {});
  [[nodiscard]] std::expected<std::string, CommandBlockActionError> commandBlockText(CommandBlockTarget target, CommandBlockPart part, CaptureRendition rendition, CommandBlockId pinned = {}) const;
  [[nodiscard]] CommandBlockRecord const* targetedCommandBlock(CommandBlockTarget target, CommandBlockId pinned = {}) const;
  ```

- [ ] **Step 1: Write the failing test** — create `src/vtbackend/screen/Terminal_block_actions_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtpty/MockPty.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

/// Rows 0..5: `$ ls` printing a and b (block 1), `$ pwd` printing /tmp (block 2), the live prompt (block 3).
void twoCommandsAndAPrompt(MockTerm<>& mock)
{
    mock.writeToScreen("\033]133;A\033\\$ ls\r\n\033]133;C\033\\a\r\nb\r\n\033]133;D;0\033\\"sv);
    mock.writeToScreen("\033]133;A\033\\$ pwd\r\n\033]133;C\033\\/tmp\r\n\033]133;D;0\033\\"sv);
    mock.writeToScreen("\033]133;A\033\\$ "sv);
}

[[nodiscard]] MockTerm<> makeTerm()
{
    return MockTerm<> { PageSize { LineCount(8), ColumnCount(20) }, LineCount(10) };
}

/// Moves the mouse pointer over @p line (column 0), as hovering would.
void pointAt(MockTerm<>& mock, int line)
{
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(line), .column = ColumnOffset(0) },
                                     PixelCoordinate {},
                                     false);
}

[[nodiscard]] std::string textOf(MockTerm<>& mock,
                                 CommandBlockTarget target,
                                 CommandBlockPart part,
                                 CommandBlockId pinned = {})
{
    return mock.terminal.commandBlockText(target, part, CaptureRendition::PlainText, pinned).value_or("<error>");
}

} // namespace

TEST_CASE("Terminal.commandBlockText.thePointerNamesTheBlockUnderIt", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    pointAt(mock, 1);
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::Output) == "a\nb");
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::Prompt) == "$ ls");
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::PromptAndOutput) == "$ ls\na\nb");

    pointAt(mock, 3);
    CHECK(textOf(mock, CommandBlockTarget::Pointer, CommandBlockPart::PromptAndOutput) == "$ pwd\n/tmp");
}

TEST_CASE("Terminal.commandBlockRows.lastIsTheMostRecentlyFinishedBlock", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Last, CommandBlockPart::Output)
          == CommandBlockRows { .first = LineOffset(4), .last = LineOffset(4) });
    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output) == "/tmp");

    auto const* record = mock.terminal.targetedCommandBlock(CommandBlockTarget::Last);
    REQUIRE(record != nullptr);
    CHECK(record == mock.terminal.commandBlocks().lastFinished());
}

TEST_CASE("Terminal.commandBlockRows.theCursorNamesTheLivePrompt", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Cursor, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::EmptyPart));
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Cursor, CommandBlockPart::Prompt)
          == CommandBlockRows { .first = LineOffset(5), .last = LineOffset(5) });

    // In vi normal mode the vi cursor is the cursor.
    mock.terminal.inputHandler().setMode(ViMode::Normal);
    mock.terminal.moveNormalModeCursorTo(CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });
    CHECK(textOf(mock, CommandBlockTarget::Cursor, CommandBlockPart::Output) == "a\nb");
}

TEST_CASE("Terminal.commandBlockRows.withoutShellIntegrationThereIsNoBlock", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    mock.writeToScreen("hello\r\nworld\r\n"sv);

    pointAt(mock, 0);
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Pointer, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Last, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mock.terminal.targetedCommandBlock(CommandBlockTarget::Last) == nullptr);
}

TEST_CASE("Terminal.selectCommandBlock.selectsWholeLines", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    pointAt(mock, 2);
    REQUIRE(mock.terminal.selectCommandBlock(CommandBlockTarget::Pointer, CommandBlockPart::Output).has_value());
    REQUIRE(mock.terminal.selectionAvailable());
    CHECK(mock.terminal.selector()->state() == Selection::State::Complete);
    // A full-line selection copies with a terminating newline (SelectionRenderer::finish()).
    CHECK(mock.terminal.extractSelectionText() == "a\nb\n");

    // A target with nothing to select leaves the selection alone.
    CHECK_FALSE(mock.terminal.selectCommandBlock(CommandBlockTarget::Cursor, CommandBlockPart::Output).has_value());
    CHECK(mock.terminal.extractSelectionText() == "a\nb\n");
}

TEST_CASE("Terminal.commandBlockText.joinsSoftWrapsAndKeepsColours", "[semanticblocks][blockactions]")
{
    // Five columns: "abcd fgh" wraps after the space, which is content and must survive the join.
    auto mock = MockTerm<> { PageSize { LineCount(6), ColumnCount(5) }, LineCount(10) };
    mock.writeToScreen("\033]133;A\033\\$ x\r\n\033]133;C\033\\\033[31mabcd fgh\033[m\r\nred\r\n"sv);
    mock.writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ "sv);

    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output) == "abcd fgh\nred");

    auto const coloured =
        mock.terminal.commandBlockText(CommandBlockTarget::Last, CommandBlockPart::Output, CaptureRendition::WithSgr);
    REQUIRE(coloured.has_value());
    CHECK(coloured->contains("\033["));
    CHECK(coloured->contains("red"));
}

TEST_CASE("Terminal.commandBlockText.aPinnedBlockWinsOverTheTarget", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);

    auto const* first = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(first != nullptr);

    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output, first->id) == "a\nb");
    CHECK(mock.terminal.targetedCommandBlock(CommandBlockTarget::Last, first->id) == first);
}

TEST_CASE("Terminal.selectCommandBlock.refusesOnTheAlternateScreen", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);
    mock.writeToScreen("\033[?1049h"sv);
    REQUIRE_FALSE(mock.terminal.isPrimaryScreen());

    pointAt(mock, 1);
    CHECK(mock.terminal.selectCommandBlock(CommandBlockTarget::Last, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mock.terminal.commandBlockRows(CommandBlockTarget::Pointer, CommandBlockPart::Output)
          == std::unexpected(CommandBlockActionError::NoBlock));

    // The history still answers: "last" reads the primary screen.
    CHECK(textOf(mock, CommandBlockTarget::Last, CommandBlockPart::Output) == "/tmp");
}
```

Register it in `src/vtbackend/CMakeLists.txt`, after `        screen/Terminal_selection_test.cpp` (:240):

```cmake
        screen/Terminal_block_actions_test.cpp
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `'commandBlockText': is not a member of 'vtbackend::Terminal'` (and the same for `commandBlockRows`, `selectCommandBlock`, `targetedCommandBlock`).

- [ ] **Step 3: Declare the API** — in `src/vtbackend/screen/Terminal.hpp`, add `#include <vtbackend/shell/CommandBlockRows.hpp>` to the `vtbackend/` include block and `#include <expected>` to the standard-library block (clang-format sorts both). After the `lastCommandBlock()` declaration (:1729) insert:

```cpp

    /// The rows of one part of a command block on the primary screen.
    ///
    /// Blocks live on the primary screen only, so every target resolves there; Pointer resolves to
    /// nothing while an alternate screen is up, because the pointer is over that screen.
    /// Does NOT take the lock -- the caller holds it, as for lastCommandBlock().
    /// @param target Which block: under the pointer, at the cursor (the vi cursor outside Insert mode),
    ///               or the most recently finished one.
    /// @param part Which part of the block.
    /// @param pinned When non-zero, this block regardless of @p target (a context-menu row's block).
    /// @return The rows, or why there are none.
    [[nodiscard]] std::expected<CommandBlockRows, CommandBlockActionError> commandBlockRows(
        CommandBlockTarget target, CommandBlockPart part, CommandBlockId pinned = {}) const;

    /// Selects one part of a command block as whole lines, completed as a mouse selection would be.
    /// @param target Which block (@see commandBlockRows).
    /// @param part Which part of the block.
    /// @param pinned When non-zero, this block regardless of @p target.
    /// @return Nothing on success; NoBlock while an alternate screen is up (the selection would land on
    ///         the screen on display, not the one holding the block).
    [[nodiscard]] std::expected<void, CommandBlockActionError> selectCommandBlock(CommandBlockTarget target,
                                                                                  CommandBlockPart part,
                                                                                  CommandBlockId pinned = {});

    /// The text of one part of a command block: one line per row, except that a row a soft wrap
    /// continues is joined to the next with its trailing blanks kept (they are content).
    /// @param target Which block (@see commandBlockRows).
    /// @param part Which part of the block.
    /// @param rendition Plain text, or text with the SGR sequences its cells wear (for `less -R`).
    /// @param pinned When non-zero, this block regardless of @p target.
    /// @return The text without a trailing newline, or why there is none.
    [[nodiscard]] std::expected<std::string, CommandBlockActionError> commandBlockText(
        CommandBlockTarget target,
        CommandBlockPart part,
        CaptureRendition rendition,
        CommandBlockId pinned = {}) const;

    /// The record of the block a target names. Last and a pinned id answer from the store, so they keep
    /// answering after the block's rows have left the grid; Pointer and Cursor need a row to stand on.
    /// @param target Which block (@see commandBlockRows).
    /// @param pinned When non-zero, the stored record with this id, even if its rows have left the grid.
    /// @return The record, or nullptr.
    [[nodiscard]] CommandBlockRecord const* targetedCommandBlock(CommandBlockTarget target,
                                                                 CommandBlockId pinned = {}) const;
```

After `    bool handleMouseSelection(Modifiers modifiers);` (:2327) insert:

```cpp

    /// The row a block target resolves to on the primary grid, or nullopt (@see commandBlockRows).
    [[nodiscard]] std::optional<LineOffset> commandBlockAnchor(CommandBlockTarget target, CommandBlockId pinned) const;

    /// Selects the whole lines [@p top, @p bottom] of the current screen, completed in Insert mode.
    void selectLines(LineOffset top, LineOffset bottom);
```

- [ ] **Step 4: Implement it** — in `src/vtbackend/screen/Terminal.cpp`, inside the anonymous namespace, after `raiseToMinimum` (:160-163) insert:

```cpp

    /// A grid as the row source a command-block lookup walks.
    class GridBlockRows final: public CommandBlockRowSource
    {
      public:
        explicit GridBlockRows(Grid const& grid) noexcept: _grid { grid } {}

        [[nodiscard]] LineOffset topRow() const override { return _grid.addressableTop(); }

        [[nodiscard]] LineOffset bottomRow() const override
        {
            return boxed_cast<LineOffset>(_grid.pageSize().lines) - LineOffset(1);
        }

        [[nodiscard]] CommandBlockId blockIdAt(LineOffset row) const override
        {
            return _grid.lineAt(row).blockId();
        }

        [[nodiscard]] LineFlags flagsAt(LineOffset row) const override { return _grid.lineAt(row).flags(); }

      private:
        Grid const& _grid;
    };
```

Replace `selectAll()` (:2445-2473) with:

```cpp
void Terminal::selectAll()
{
    auto const& grid = _currentScreen->grid();
    selectLines(-boxed_cast<LineOffset>(grid.historyLineCount()),
                boxed_cast<LineOffset>(pageSize().lines) - LineOffset(1));
}

void Terminal::selectLines(LineOffset top, LineOffset bottom)
{
    auto const from = CellLocation { .line = top, .column = ColumnOffset(0) };
    auto const to = CellLocation { .line = bottom,
                                   .column = boxed_cast<ColumnOffset>(pageSize().columns) - ColumnOffset(1) };

    // FullLineSelection rather than LinearSelection: it normalizes the columns to whole lines and follows
    // wrapped ones, which is what "all" and "this block" mean (and what ViMode's VisualLine already does).
    setSelector(std::make_unique<FullLineSelection>(_selectionHelper, from, selectionUpdatedHelper()));
    (void) _selection->extend(to);

    // Completing a selection is Insert mode's business — exactly the gate sendMouseReleaseEvent() applies
    // to a finished drag. In a Visual mode the Vi layer owns the selection and every motion extends it
    // (ViCommands::moveCursorTo → Selection::extend, whose first statement is an assert that the state is
    // not Complete), so handing it a completed one aborts on the next keystroke.
    if (_inputHandler.mode() == ViMode::Insert)
        _selection->complete();

    // Deliberately NOT updateSelectionMatches(): that serializes the selection into a search pattern to
    // highlight the other occurrences of a selected WORD. For a selection that spans many lines the
    // pattern is megabytes built under the terminal lock, for a search whose only match is what is already
    // selected, and which then disables the trivial-line render fast path for every frame that follows.
    // The quadruple-click full-line selection skips it for the same reason.
    onSelectionUpdated();
}

std::optional<LineOffset> Terminal::commandBlockAnchor(CommandBlockTarget target, CommandBlockId pinned) const
{
    auto const rows = GridBlockRows { primaryScreen().grid() };
    if (pinned.value != 0)
        return bottomRowOfBlock(rows, pinned);

    switch (target)
    {
        case CommandBlockTarget::Pointer:
            if (!isPrimaryScreen())
                return std::nullopt;
            return currentMouseGridPosition().transform([](CellLocation position) { return position.line; });
        case CommandBlockTarget::Cursor:
            if (isPrimaryScreen() && _inputHandler.mode() != ViMode::Insert)
                return _viCommands.cursorPosition.line;
            return primaryScreen().realCursorPosition().line;
        case CommandBlockTarget::Last:
            if (auto const* record = commandBlocks().lastFinished())
                return bottomRowOfBlock(rows, record->id);
            return std::nullopt;
    }
    return std::nullopt;
}

std::expected<CommandBlockRows, CommandBlockActionError> Terminal::commandBlockRows(CommandBlockTarget target,
                                                                                    CommandBlockPart part,
                                                                                    CommandBlockId pinned) const
{
    auto const anchor = commandBlockAnchor(target, pinned);
    if (!anchor)
        return std::unexpected(CommandBlockActionError::NoBlock);
    // Qualified: the member of the same name hides the free function.
    return vtbackend::commandBlockRows(GridBlockRows { primaryScreen().grid() }, *anchor, part);
}

std::expected<void, CommandBlockActionError> Terminal::selectCommandBlock(CommandBlockTarget target,
                                                                         CommandBlockPart part,
                                                                         CommandBlockId pinned)
{
    if (!isPrimaryScreen())
        return std::unexpected(CommandBlockActionError::NoBlock);
    return commandBlockRows(target, part, pinned).transform([this](CommandBlockRows rows) {
        selectLines(rows.first, rows.last);
    });
}

std::expected<std::string, CommandBlockActionError> Terminal::commandBlockText(CommandBlockTarget target,
                                                                               CommandBlockPart part,
                                                                               CaptureRendition rendition,
                                                                               CommandBlockId pinned) const
{
    return commandBlockRows(target, part, pinned).transform([&](CommandBlockRows rows) {
        auto const& grid = primaryScreen().grid();
        auto text = std::string {};
        for (auto const offset: std::views::iota(unbox(rows.first), unbox(rows.last) + 1))
        {
            auto const row = LineOffset(offset);
            auto const continued = row < rows.last && grid.isLineWrapped(row + LineOffset(1));
            auto const trailing = continued ? CaptureTrailingSpaces::Keep : CaptureTrailingSpaces::Trim;
            for (auto const& line: grid.renderRange(row, row, rendition, trailing))
                text += line;
            if (row < rows.last && !continued)
                text += '\n';
        }
        return text;
    });
}

CommandBlockRecord const* Terminal::targetedCommandBlock(CommandBlockTarget target, CommandBlockId pinned) const
{
    if (pinned.value != 0)
        return commandBlocks().find(pinned);
    // The record outlives its rows: a command line can still be copied once its output scrolled away.
    if (target == CommandBlockTarget::Last)
        return commandBlocks().lastFinished();
    auto const anchor = commandBlockAnchor(target, pinned);
    if (!anchor)
        return nullptr;
    return commandBlockAt(*anchor);
}
```

Run `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_block_actions_test.cpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockactions]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[terminal]"`
Expected: zero warnings; both runs `All tests passed` (the second proves `selectAll` still behaves).

- [ ] **Step 6: Commit**

```bash
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_block_actions_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: select and render a command block by target

Resolve a block target -- the row under the pointer, the cursor (the vi
cursor outside Insert mode), the last finished block, or a pinned id -- to
an anchor on the primary grid, and from it select or render one part of the
block. The text joins soft-wrapped rows and keeps their trailing blanks, and
can carry SGR for a pager that understands it.

selectAll() and the new block selection share selectLines(), so both keep
the same Insert-mode completion rule.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.3: The `SelectCommandBlock` action

**Files:**
- Create: `src/contour/config/BlockActionEnums.hpp`
- Modify: `src/contour/config/CMakeLists.txt` (`contour_config` sources, after `ConfigEnum.hpp`)
- Modify: `src/contour/config/Actions.hpp` (includes :4; structs before `// clang-format on` :159; `Action` variant end :264; `documentation` namespace end :622; `actionCatalog()` end :799; `formatActionArguments` :875-915)
- Modify: `src/contour/config/Config.cpp` (new anonymous namespace before `parseAction` :3214; branch after `SetTabBarPosition` :3260)
- Modify: `src/contour/command/Command.cpp` (anonymous namespace :19-39; `commandArguments` visitor :122)
- Modify: `src/contour/session/TerminalSession.hpp` (after `bool operator()(actions::CopyHyperlink const& action);` :841)
- Modify: `src/contour/session/TerminalSession.cpp` (after `copyLastCommandBlock` :2516-2533)
- Test: `src/contour/config/Actions_test.cpp`, `src/contour/command/CommandCatalog_test.cpp` (both `contour_test`), `src/contour/config/Config_test.cpp`, `src/contour/session/TerminalSession_test.cpp` (both `contour_gui_test`)

**Interfaces:**
- Consumes: Task 5.2 `Terminal::selectCommandBlock`; C1 `CommandBlockTarget`, `CommandBlockId`; `CommandBlockPart`; `ConfigEnumInfo`/`configEnumValues`/`configEnumFromToken`/`configEnumToken` (`ConfigEnum.hpp`).
- Produces:
  ```cpp
  // contour::actions
  struct SelectCommandBlock { vtbackend::CommandBlockTarget target = Last; vtbackend::CommandBlockPart part = Output; vtbackend::CommandBlockId block {}; };
  // global, Actions.hpp
  [[nodiscard]] inline std::string formatPinnedBlock(vtbackend::CommandBlockId block);
  // contour::config, BlockActionEnums.hpp: configEnumValues<vtbackend::CommandBlockTarget>() (pointer|cursor|last),
  //                                         configEnumValues<vtbackend::CommandBlockPart>() (input|output|all)
  ```
  YAML: `{ ..., action: SelectCommandBlock, target: pointer|cursor|last, part: output|input|all, block: N }` (all optional; `block` is what a context-menu row pins).

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/config/Actions_test.cpp`:

```cpp
TEST_CASE("actions: SelectCommandBlock writes its arguments as config tokens", "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::SelectCommandBlock {} })
          == "SelectCommandBlock, target: last, part: output");
    CHECK(std::format("{}",
                      actions::Action { actions::SelectCommandBlock {
                          .target = vtbackend::CommandBlockTarget::Cursor,
                          .part = vtbackend::CommandBlockPart::PromptAndOutput,
                          .block = vtbackend::CommandBlockId(7) } })
          == "SelectCommandBlock, target: cursor, part: all, block: 7");
    CHECK(std::format("{}",
                      actions::Action { actions::SelectCommandBlock {
                          .target = vtbackend::CommandBlockTarget::Pointer,
                          .part = vtbackend::CommandBlockPart::Prompt } })
          == "SelectCommandBlock, target: pointer, part: input");
    CHECK_FALSE(actions::isParameterized(actions::Action { actions::SelectCommandBlock {} }));
}
```

Add `#include <format>` to that file's standard-library includes.

Append to `src/contour/command/CommandCatalog_test.cpp`:

```cpp
TEST_CASE("A block action's id carries its target, part and pinned block", "[contour][palette][semanticblocks]")
{
    using contour::command::commandId;
    namespace actions = contour::actions;

    // The default instance is the catalog's row: its id is its bare name.
    CHECK(commandId(actions::SelectCommandBlock {}) == "SelectCommandBlock");

    // Anything else is a different command, and a pinned row differs from an unpinned one.
    CHECK(commandId(actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer })
          == "SelectCommandBlock:pointer:output:0");
    CHECK(commandId(actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer,
                                                  .block = vtbackend::CommandBlockId(7) })
          == "SelectCommandBlock:pointer:output:7");
}
```

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: SelectCommandBlock reads its target, part and pinned block", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'F1', action: SelectCommandBlock }
    - { mods: [Control], key: 'F2', action: SelectCommandBlock, target: cursor, part: input }
    - { mods: [Control], key: 'F3', action: SelectCommandBlock, target: POINTER, part: all, block: 7 }
    - { mods: [Control], key: 'F4', action: SelectCommandBlock, part: ouput }
)"sv);

    auto const& keys = config.inputMappings.value().keyMappings;
    auto const bound = [&](vtbackend::Key key) -> std::optional<contour::actions::SelectCommandBlock> {
        auto const it = std::ranges::find_if(keys, [key](auto const& mapping) { return mapping.input == key; });
        if (it == keys.end() || it->binding.empty())
            return std::nullopt;
        return std::get<contour::actions::SelectCommandBlock>(it->binding.at(0));
    };

    auto const plain = bound(vtbackend::Key::F1);
    REQUIRE(plain.has_value());
    CHECK(plain->target == vtbackend::CommandBlockTarget::Last);
    CHECK(plain->part == vtbackend::CommandBlockPart::Output);
    CHECK(plain->block.value == 0);

    auto const atCursor = bound(vtbackend::Key::F2);
    REQUIRE(atCursor.has_value());
    CHECK(atCursor->target == vtbackend::CommandBlockTarget::Cursor);
    CHECK(atCursor->part == vtbackend::CommandBlockPart::Prompt);

    auto const pinned = bound(vtbackend::Key::F3);
    REQUIRE(pinned.has_value());
    CHECK(pinned->target == vtbackend::CommandBlockTarget::Pointer); // tokens ignore case
    CHECK(pinned->part == vtbackend::CommandBlockPart::PromptAndOutput);
    CHECK(pinned->block.value == 7);

    // A misspelt part drops the binding rather than silently selecting something else.
    CHECK_FALSE(bound(vtbackend::Key::F4).has_value());
}
```

Add `#include <optional>` and `#include <ranges>` to that file's standard-library includes if absent.

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: SelectCommandBlock selects a part of a block", "[contour][session][actions][semanticblocks]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("\033]133;A\033\\$ ls\r\n\033]133;C\033\\file1\r\nfile2\r\n");
    session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    CHECK((*session)(contour::actions::SelectCommandBlock {}));
    CHECK(session->terminal().extractSelectionText() == "file1\nfile2\n");

    CHECK((*session)(contour::actions::SelectCommandBlock { .part = vtbackend::CommandBlockPart::Prompt }));
    CHECK(session->terminal().extractSelectionText() == "$ ls\n");

    // The live prompt has no output yet: nothing to select, and the action says so.
    CHECK_FALSE(
        (*session)(contour::actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Cursor }));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'SelectCommandBlock': is not a member of 'contour::actions'`.

- [ ] **Step 3: Add the token tables** — create `src/contour/config/BlockActionEnums.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnum.hpp>

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandBlocks.hpp>

#include <array>
#include <span>

namespace contour::config
{

namespace detail
{
    /// Which block a block action addresses: `target:` in input_mapping.
    inline constexpr auto CommandBlockTargetTable = std::array {
        ConfigEnumInfo<vtbackend::CommandBlockTarget> {
            vtbackend::CommandBlockTarget::Pointer, "pointer", "Under the mouse pointer" },
        ConfigEnumInfo<vtbackend::CommandBlockTarget> {
            vtbackend::CommandBlockTarget::Cursor, "cursor", "At the cursor" },
        ConfigEnumInfo<vtbackend::CommandBlockTarget> {
            vtbackend::CommandBlockTarget::Last, "last", "The last finished command" },
    };

    /// Which part of a block: `part:` in input_mapping. `input` is the prompt with the command typed at
    /// it -- the user's word for that part, where the engine's is "prompt".
    inline constexpr auto CommandBlockPartTable = std::array {
        ConfigEnumInfo<vtbackend::CommandBlockPart> {
            vtbackend::CommandBlockPart::Prompt, "input", "The prompt and the command" },
        ConfigEnumInfo<vtbackend::CommandBlockPart> { vtbackend::CommandBlockPart::Output, "output", "The output" },
        ConfigEnumInfo<vtbackend::CommandBlockPart> {
            vtbackend::CommandBlockPart::PromptAndOutput, "all", "The command and its output" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::CommandBlockTarget> const> configEnumValues() noexcept
{
    return detail::CommandBlockTargetTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::CommandBlockPart> const> configEnumValues() noexcept
{
    return detail::CommandBlockPartTable;
}

} // namespace contour::config
```

In `src/contour/config/CMakeLists.txt`, after `    ConfigEnum.hpp` add `    BlockActionEnums.hpp` (the list is not sorted; keep it next to its sibling).

- [ ] **Step 4: Add the action** — in `src/contour/config/Actions.hpp`:

After `#include <contour/config/TabBarMode.hpp>` (:4) add `#include <contour/config/BlockActionEnums.hpp>` (clang-format sorts the pair).

Before `// clang-format on` (:159) insert:

```cpp
// OSC 133: select one part of a command block; a non-zero block pins it (a context-menu row), else target picks it
struct SelectCommandBlock{ vtbackend::CommandBlockTarget target = vtbackend::CommandBlockTarget::Last; vtbackend::CommandBlockPart part = vtbackend::CommandBlockPart::Output; vtbackend::CommandBlockId block {}; };
```

Append `SelectCommandBlock` as the **last** alternative of `Action`: replace `                            StopSpeaking>;` (:264) with

```cpp
                            StopSpeaking,
                            SelectCommandBlock>;
```

(If an earlier phase already appended after `StopSpeaking`, append after the current last alternative instead; the rule is "last", because `actionCatalog()` is indexed by alternative and Actions_test pins the order.)

Before `} // namespace documentation` (:622) insert:

```cpp
    constexpr inline std::string_view SelectCommandBlock {
        "Selects one part of a command block as whole lines (target: pointer, cursor or last; part: "
        "output, input or all). Requires a shell that emits OSC 133 marks."
    };
```

As the last row of `actionCatalog()` (after the `StopSpeaking` row :797) insert:

```cpp
        ActionCatalogEntry {
            "SelectCommandBlock", Action { SelectCommandBlock {} }, documentation::SelectCommandBlock },
```

Immediately above the doc comment of `formatActionArguments` (:865) insert:

```cpp
/// The `, block: N` sibling key of an action pinned to one command block.
/// @param block The pinned block; zero means the action is not pinned.
/// @return The key, ready to append; empty when not pinned.
[[nodiscard]] inline std::string formatPinnedBlock(vtbackend::CommandBlockId block)
{
    if (block.value == 0)
        return {};
    return std::format(", block: {}", block.value);
}

```

In `formatActionArguments`, after the `SetTabBarPosition` lambda (:903-905) insert:

```cpp
            [](SelectCommandBlock const& a) {
                return std::format(", target: {}, part: {}{}",
                                   contour::config::configEnumToken(a.target),
                                   contour::config::configEnumToken(a.part),
                                   formatPinnedBlock(a.block));
            },
```

- [ ] **Step 5: Give it a command identity** — in `src/contour/command/Command.cpp`, add `#include <contour/config/BlockActionEnums.hpp>` after `#include <contour/command/Command.hpp>`, and inside the anonymous namespace, after `startsWord` (:38), insert:

```cpp

    /// What a block action adds to its command identity: nothing for its defaults (target last, part
    /// output, not pinned), else all three -- a pinned row and an unpinned one are different commands.
    /// @param target The action's target.
    /// @param part The action's part.
    /// @param block The pinned block; zero when not pinned.
    [[nodiscard]] CommandArguments blockArguments(vtbackend::CommandBlockTarget target,
                                                  vtbackend::CommandBlockPart part,
                                                  vtbackend::CommandBlockId block)
    {
        if (target == vtbackend::CommandBlockTarget::Last && part == vtbackend::CommandBlockPart::Output
            && block.value == 0)
            return {};
        auto const targetToken = config::configEnumToken(target);
        auto const partToken = config::configEnumToken(part);
        return { .id = std::format("{}:{}:{}", targetToken, partToken, block.value),
                 .title = std::format(" ({}, {})", partToken, targetToken) };
    }
```

In `commandArguments`, after the `CopySelection` lambda (:122-126) insert:

```cpp
            [](SelectCommandBlock const& a) -> CommandArguments {
                return blockArguments(a.target, a.part, a.block);
            },
```

- [ ] **Step 6: Parse it** — in `src/contour/config/Config.cpp`, immediately above `// NOLINTNEXTLINE(readability-function-cognitive-complexity)` (:3214) insert:

```cpp
namespace
{
    /// The enum-valued argument @p key of an action, read through its ConfigEnum token table.
    /// @param node The action's YAML map.
    /// @param key The argument's key.
    /// @param fallback What an absent key means.
    /// @return The value, or nullopt (logged) when the key names no value of @p Enum.
    template <typename Enum>
    [[nodiscard]] std::optional<Enum> enumActionArgument(YAML::Node const& node, char const* key, Enum fallback)
    {
        auto const value = node[key];
        if (!value)
            return fallback;
        auto const token = value.IsScalar() ? value.as<std::string>() : std::string {};
        if (auto const parsed = configEnumFromToken<Enum>(token))
            return *parsed;
        errorLog()("Invalid {} '{}' in action '{}'; ignoring the binding.", key, token, node["action"].as<std::string>());
        return std::nullopt;
    }

    /// The `block:` argument that pins an action to one command block (a context-menu row's).
    /// @param node The action's YAML map.
    /// @return The block; zero (none) when absent or not a number.
    [[nodiscard]] vtbackend::CommandBlockId pinnedBlockArgument(YAML::Node const& node)
    {
        auto const value = node["block"];
        if (!value || !value.IsScalar())
            return vtbackend::CommandBlockId {};
        return vtbackend::CommandBlockId(value.as<uint32_t>(0));
    }
} // namespace

```

In `parseAction`, after the `SetTabBarPosition` branch (:3254-3260) insert:

```cpp

        if (holds_alternative<actions::SelectCommandBlock>(action))
        {
            auto const target = enumActionArgument(node, "target", vtbackend::CommandBlockTarget::Last);
            auto const part = enumActionArgument(node, "part", vtbackend::CommandBlockPart::Output);
            if (!target || !part)
                return std::nullopt;
            return actions::SelectCommandBlock { .target = *target, .part = *part, .block = pinnedBlockArgument(node) };
        }
```

- [ ] **Step 7: Run it** — in `src/contour/session/TerminalSession.hpp`, after `    bool operator()(actions::CopyHyperlink const& action);` (:841) add:

```cpp
    bool operator()(actions::SelectCommandBlock const& action);
```

In `src/contour/session/TerminalSession.cpp`, after `copyLastCommandBlock` (ends :2533) insert:

```cpp

bool TerminalSession::operator()(actions::SelectCommandBlock const& action)
{
    return core::locked(_terminal, [&]() {
        return terminal().selectCommandBlock(action.target, action.part, action.block).has_value();
    });
}
```

Run `clang-format -i` on every file touched in this task.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test`, then `out/build/clangcl-debug/bin/contour_test.exe "[semanticblocks],[actions],[palette]"` and `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[semanticblocks],[config]"`
Expected: zero warnings; `All tests passed` in both (the catalog-order and round-trip cases in `[actions]` cover the new row).

- [ ] **Step 9: Commit**

```bash
git add src/contour/config/BlockActionEnums.hpp src/contour/config/CMakeLists.txt src/contour/config/Actions.hpp src/contour/config/Config.cpp src/contour/command/Command.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/config/Actions_test.cpp src/contour/command/CommandCatalog_test.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
actions: add SelectCommandBlock

Select one part of a command block -- output, input (the prompt with its
command) or all -- of the block under the pointer, at the cursor, or the
last finished one. A `block:` argument pins a specific block, which is how a
context-menu row will act on the block that was right-clicked.

The YAML tokens come from one ConfigEnum table per enum, so the reader, the
writer and the palette identity spell them the same way. A misspelt token
drops the binding instead of silently doing something else.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.4: `ClickCounter` with an injected clock (C5)

**Files:**
- Create: `src/vtbackend/input/ClickCounter.hpp`
- Create: `src/vtbackend/input/ClickCounter.cpp`
- Test: create `src/vtbackend/input/ClickCounter_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (headers before `input/InputBinding.hpp` :36, sources before `input/InputGenerator.cpp` :111, tests before `input/InputGenerator_test.cpp` :209)

**Interfaces:**
- Consumes: `core::platform::IClock`, `core::platform::ManualClock`, `core::platform::SteadyTimePoint`/`SteadyDuration` (`vendor/core-cpp/src/core/platform/Clock.hpp`); `CellLocation`; `MouseButton` (`input/InputGenerator.hpp:353`).
- Produces (C5):
  ```cpp
  class ClickCounter {
    public:
      static constexpr std::chrono::milliseconds DefaultInterval { 1000 };
      static constexpr uint8_t MaxClickCount = 4;
      ClickCounter(core::platform::IClock const& clock, std::chrono::milliseconds interval) noexcept;
      [[nodiscard]] uint8_t press(CellLocation position, MouseButton button) noexcept;
      void reset() noexcept;
  };
  ```

- [ ] **Step 1: Write the failing test** — create `src/vtbackend/input/ClickCounter_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/ClickCounter.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace
{
constexpr auto Here = CellLocation { .line = LineOffset(2), .column = ColumnOffset(3) };
constexpr auto There = CellLocation { .line = LineOffset(2), .column = ColumnOffset(4) };
} // namespace

TEST_CASE("ClickCounter.countsPressesOnOneCellUpToFourThenStartsOver", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 3);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 4);
    clock.advance(200ms);
    CHECK(counter.press(Here, MouseButton::Left) == 1);
}

TEST_CASE("ClickCounter.theIntervalIsInclusive", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    clock.advance(1000ms);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    clock.advance(1001ms);
    CHECK(counter.press(Here, MouseButton::Left) == 1);
}

TEST_CASE("ClickCounter.anotherCellStartsANewSequence", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(There, MouseButton::Left) == 1);
    CHECK(counter.press(There, MouseButton::Left) == 2);
}

TEST_CASE("ClickCounter.eachButtonKeepsItsOwnSequence", "[semanticblocks][clickcounter]")
{
    // A right-click for the context menu between two left clicks does not end the double-click.
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(There, MouseButton::Right) == 1);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    CHECK(counter.press(Here, MouseButton::Middle) == 1);
    CHECK(counter.press(There, MouseButton::Right) == 2);
}

TEST_CASE("ClickCounter.wheelStepsAndReleasesAreAlwaysSingle", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::WheelUp) == 1);
    CHECK(counter.press(Here, MouseButton::WheelUp) == 1);
    CHECK(counter.press(Here, MouseButton::Release) == 1);
    CHECK(counter.press(Here, MouseButton::Release) == 1);
}

TEST_CASE("ClickCounter.resetEndsEverySequence", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock {};
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(Here, MouseButton::Left) == 2);
    CHECK(counter.press(Here, MouseButton::Right) == 1);
    counter.reset();
    CHECK(counter.press(Here, MouseButton::Left) == 1);
    CHECK(counter.press(Here, MouseButton::Right) == 1);
}

TEST_CASE("ClickCounter.aClockThatStepsBackStartsANewSequence", "[semanticblocks][clickcounter]")
{
    auto clock = core::platform::ManualClock { core::platform::SteadyTimePoint {} + 5s };
    auto counter = ClickCounter { clock, ClickCounter::DefaultInterval };

    CHECK(counter.press(Here, MouseButton::Left) == 1);
    clock.setNow(core::platform::SteadyTimePoint {} + 4s);
    CHECK(counter.press(Here, MouseButton::Left) == 1);
}
```

Register it in `src/vtbackend/CMakeLists.txt`, before `        input/InputGenerator_test.cpp` (:209):

```cmake
        input/ClickCounter_test.cpp
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `cannot open include file: 'vtbackend/input/ClickCounter.hpp'`.

- [ ] **Step 3: Write the header** — create `src/vtbackend/input/ClickCounter.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/input/InputGenerator.hpp>

#include <core/platform/Clock.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace vtbackend
{

/// Counts multi-clicks: how many presses of one button landed on one cell, each within an interval of
/// the press before it.
///
/// The count picks between a drag, a word, an extended word and a whole line (1..4), and it is what a
/// `clicks:` mouse binding matches on. The clock is injected so a test sets the gap between presses
/// instead of sleeping it.
class ClickCounter
{
  public:
    /// The longest gap between two presses that still continues a sequence.
    static constexpr std::chrono::milliseconds DefaultInterval { 1000 };

    /// The count after which a sequence starts over at 1 (a fifth click is a single click again).
    static constexpr uint8_t MaxClickCount = 4;

    /// @param clock Where "now" comes from. Borrowed: it must outlive the counter.
    /// @param interval The longest gap between presses that continues a sequence (inclusive).
    ClickCounter(core::platform::IClock const& clock, std::chrono::milliseconds interval) noexcept;

    /// Records a press and returns its place in its sequence.
    /// @param position The cell the press landed on.
    /// @param button The button. Left, Right and Middle each keep their own sequence; any other
    ///               button (a wheel step, a release) is always a single click.
    /// @return 1 for a new sequence, up to MaxClickCount, then 1 again.
    [[nodiscard]] uint8_t press(CellLocation position, MouseButton button) noexcept;

    /// Ends every sequence, so the next press of any button counts 1 (a pointer move, a Shift+click).
    void reset() noexcept;

  private:
    /// One button's sequence so far.
    struct Sequence
    {
        CellLocation position {};
        core::platform::SteadyTimePoint lastPress {};
        uint8_t count = 0; ///< 0 = no sequence.
    };

    /// The slot of @p button in _sequences, or nullopt for a button whose presses are not counted.
    [[nodiscard]] static std::optional<size_t> slotOf(MouseButton button) noexcept;

    core::platform::IClock const& _clock;
    std::chrono::milliseconds _interval;
    std::array<Sequence, 3> _sequences {};
};

} // namespace vtbackend
```

- [ ] **Step 4: Write the implementation** — create `src/vtbackend/input/ClickCounter.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/ClickCounter.hpp>

namespace vtbackend
{

ClickCounter::ClickCounter(core::platform::IClock const& clock, std::chrono::milliseconds interval) noexcept:
    _clock { clock }, _interval { interval }
{
}

std::optional<size_t> ClickCounter::slotOf(MouseButton button) noexcept
{
    switch (button)
    {
        case MouseButton::Left: return 0;
        case MouseButton::Right: return 1;
        case MouseButton::Middle: return 2;
        case MouseButton::Release:
        case MouseButton::WheelUp:
        case MouseButton::WheelDown:
        case MouseButton::WheelLeft:
        case MouseButton::WheelRight: return std::nullopt;
    }
    return std::nullopt;
}

uint8_t ClickCounter::press(CellLocation position, MouseButton button) noexcept
{
    auto const slot = slotOf(button);
    if (!slot)
        return 1;

    auto& sequence = _sequences[*slot];
    auto const now = _clock.now();
    auto const gap = now - sequence.lastPress;

    // A gap below zero is a clock that stepped back; trusting it would continue a sequence forever.
    auto const continues = sequence.count != 0 && position == sequence.position
                           && gap >= core::platform::SteadyDuration::zero() && gap <= _interval;

    sequence.count = continues ? static_cast<uint8_t>((sequence.count % MaxClickCount) + 1) : uint8_t { 1 };
    sequence.position = position;
    sequence.lastPress = now;
    return sequence.count;
}

void ClickCounter::reset() noexcept
{
    _sequences = {};
}

} // namespace vtbackend
```

Register both in `src/vtbackend/CMakeLists.txt`: before `    input/InputBinding.hpp` (:36) add `    input/ClickCounter.hpp`; before `    input/InputGenerator.cpp` (:111) add `    input/ClickCounter.cpp`. Run `clang-format -i src/vtbackend/input/ClickCounter.hpp src/vtbackend/input/ClickCounter.cpp src/vtbackend/input/ClickCounter_test.cpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[clickcounter]"`
Expected: zero warnings; `All tests passed` (7 test cases).

- [ ] **Step 6: Commit**

```bash
git add src/vtbackend/input/ClickCounter.hpp src/vtbackend/input/ClickCounter.cpp src/vtbackend/input/ClickCounter_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: count multi-clicks in a ClickCounter with an injected clock

The click count decides between drag, word, extended word and line
selection, and is about to become an input a mouse binding can match on.
Give it a home of its own: per-button sequences on one cell, an inclusive
interval, a wrap after four, and a reset for the pointer moves and
Shift+clicks that end a sequence. The clock is injected, so the timing is
tested by setting it rather than by sleeping.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.5: Let a binding claim a press before the multi-click selection (`Terminal`)

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp` (after `enum class WrapPending` :221-225; `sendMousePressEvent` :933-937; `handleMouseSelection` :2327; mouse state :2435-2437; includes)
- Modify: `src/vtbackend/screen/Terminal.cpp` (constructor init list `_pty { std::move(pty) },` :187; `sendMousePressEvent` :1110-1146; `handleMouseSelection` :1239-1286; `sendMouseMoveEvent` :1428-1446)
- Modify: `src/contour/display/DisplayRendering_test.cpp` (two comments naming the old counter, :1437-1438 and :1572-1577; comments only)
- Test: `src/vtbackend/screen/Terminal_selection_test.cpp` (append before `// NOLINTEND(misc-const-correctness)` :730; one comment at :237-238)

**Interfaces:**
- Consumes: Task 5.4 `ClickCounter`; C1 `TerminalClocks::steady` (constructor parameter `clocks`).
- Produces:
  ```cpp
  enum class MousePressClaim : uint8_t { Unclaimed = 0, Claimed };
  using MousePressClaimant = std::function<MousePressClaim(uint8_t clickCount)>;
  Handled Terminal::sendMousePressEvent(Modifiers modifiers, MouseButton button, CellLocation newPosition,
                                        PixelCoordinate pixelPosition, bool uiHandledHint,
                                        MousePressClaimant const& claimant);
  ```
  The existing 5-argument overload keeps its signature and delegates with a claimant that never claims.

- [ ] **Step 1: Write the failing test** — append to `src/vtbackend/screen/Terminal_selection_test.cpp`, before `// NOLINTEND(misc-const-correctness)`:

```cpp
TEST_CASE("Terminal.a_binding_is_offered_the_click_count_before_the_selection", "[terminal][semanticblocks]")
{
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.terminal.setWordDelimiters(" ");
    mock.writeToScreen("hello world");

    using namespace vtbackend;
    auto constexpr UiHandledHint = false;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto const at = 0_lineOffset + 1_columnOffset;

    auto offered = std::vector<int> {};
    auto const claimThird = [&](uint8_t clickCount) {
        offered.push_back(clickCount);
        return clickCount == 3 ? MousePressClaim::Claimed : MousePressClaim::Unclaimed;
    };

    for ([[maybe_unused]] auto const _: std::views::iota(0, 2))
    {
        mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, at, PixelCoordinate, UiHandledHint, claimThird);
        mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, UiHandledHint);
    }
    REQUIRE(mock.terminal.extractSelectionText() == "hello");

    mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, at, PixelCoordinate, UiHandledHint, claimThird);

    CHECK(offered == std::vector<int> { 1, 2, 3 });
    // The claimed press neither extended the word selection nor started a drag.
    CHECK(mock.terminal.extractSelectionText() == "hello");
    CHECK_FALSE(mock.terminal.leftMouseButtonPressed());
}

TEST_CASE("Terminal.a_press_an_application_asked_for_is_never_offered_to_a_binding", "[terminal][semanticblocks]")
{
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.writeToScreen("\033[?1000h"); // what vim and tmux turn on

    using namespace vtbackend;
    auto offered = 0;
    mock.terminal.sendMousePressEvent(Modifier::None,
                                      MouseButton::Left,
                                      0_lineOffset + 1_columnOffset,
                                      vtbackend::PixelCoordinate {},
                                      false,
                                      [&](uint8_t) {
                                          ++offered;
                                          return MousePressClaim::Claimed;
                                      });
    CHECK(offered == 0);
}

TEST_CASE("Terminal.a_pointer_move_between_presses_starts_a_new_count", "[terminal][semanticblocks]")
{
    auto mock = MockTerm { ColumnCount(11), LineCount(2) };
    mock.writeToScreen("hello world");

    using namespace vtbackend;
    auto constexpr PixelCoordinate = vtbackend::PixelCoordinate {};
    auto offered = std::vector<int> {};
    auto const record = [&](uint8_t clickCount) {
        offered.push_back(clickCount);
        return MousePressClaim::Unclaimed;
    };

    mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, false, record);
    mock.terminal.sendMouseReleaseEvent(Modifier::None, MouseButton::Left, PixelCoordinate, false);
    mock.terminal.sendMouseMoveEvent(Modifier::None, 0_lineOffset + 2_columnOffset, PixelCoordinate, false);
    mock.terminal.sendMouseMoveEvent(Modifier::None, 0_lineOffset + 1_columnOffset, PixelCoordinate, false);
    mock.terminal.sendMousePressEvent(Modifier::None, MouseButton::Left, 0_lineOffset + 1_columnOffset, PixelCoordinate, false, record);

    CHECK(offered == std::vector<int> { 1, 1 });
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `'MousePressClaim': undeclared identifier` and no `sendMousePressEvent` overload taking 6 arguments.

- [ ] **Step 3: Declare the claim** — in `src/vtbackend/screen/Terminal.hpp`, add `#include <vtbackend/input/ClickCounter.hpp>` to the `vtbackend/` include block. After `enum class WrapPending` (:221-225) insert:

```cpp

/// Whether a mouse binding took a press before the terminal's own multi-click selection saw it.
enum class MousePressClaim : uint8_t
{
    Unclaimed = 0, ///< The terminal handles the press as it always has.
    Claimed,       ///< A binding acts on it: the terminal neither selects nor starts a drag.
};

/// Asked, with a press's click count (1..ClickCounter::MaxClickCount), whether a binding takes the press.
/// @see Terminal::sendMousePressEvent
using MousePressClaimant = std::function<MousePressClaim(uint8_t clickCount)>;
```

After the existing `sendMousePressEvent` declaration (:933-937) insert:

```cpp

    /// Handles a mouse press, first offering it, with its click count, to @p claimant.
    ///
    /// The claimant is asked only when the press would reach the terminal's own selection -- no mouse
    /// protocol, or one the bypass modifier overrides -- so a binding never takes a click an application
    /// asked for. A claimed press starts no selection and no drag; everything else (hyperlink hover,
    /// DEC locator, passive tracking) proceeds as for any press.
    /// @param uiHandledHint As in the 5-argument overload: a pre-existing bool, kept so the two overloads stay
    ///                      call-compatible; converting it to an enum is outside this feature.
    /// @param claimant Asked at most once, under the caller's lock; it must not take the lock itself.
    /// @return As the 5-argument overload.
    Handled sendMousePressEvent(Modifiers modifiers,
                                MouseButton button,
                                CellLocation newPosition,
                                PixelCoordinate pixelPosition,
                                bool uiHandledHint,
                                MousePressClaimant const& claimant);
```

Replace `    bool handleMouseSelection(Modifiers modifiers);` (:2327) with:

```cpp
    bool handleMouseSelection(Modifiers modifiers, uint8_t clickCount);
```

Replace the two members (:2436-2437)

```cpp
    std::chrono::steady_clock::time_point _lastClick {};
    unsigned int _speedClicks = 0;
```

with:

```cpp
    ClickCounter _clickCounter; ///< Multi-click sequences (drag, word, extended word, line).
```

- [ ] **Step 4: Construct it** — in `src/vtbackend/screen/Terminal.cpp`'s constructor init list, after `    _pty { std::move(pty) },` (:187) insert:

```cpp
    _clickCounter { clocks.steady, ClickCounter::DefaultInterval },
```

(The member sits between `_pty` and `_lastCursorBlink`, so this is its declaration order.)

- [ ] **Step 5: Count once, offer, then select** — replace the 5-argument `Terminal::sendMousePressEvent` from its signature through the closing brace of `if (button == MouseButton::Left) { ... }` (:1110-1146) with:

```cpp
Handled Terminal::sendMousePressEvent(Modifiers modifiers,
                                      MouseButton button,
                                      CellLocation newPosition,
                                      PixelCoordinate pixelPosition,
                                      bool uiHandledHint)
{
    return sendMousePressEvent(
        modifiers, button, newPosition, pixelPosition, uiHandledHint, [](uint8_t /*clickCount*/) {
            return MousePressClaim::Unclaimed;
        });
}

Handled Terminal::sendMousePressEvent(Modifiers modifiers,
                                      MouseButton button,
                                      CellLocation newPosition,
                                      PixelCoordinate pixelPosition,
                                      bool uiHandledHint,
                                      MousePressClaimant const& claimant)
{
    // A press is not necessarily preceded by a MouseMove landing exactly here: the windowing system
    // usually delivers one, which is why a slow, deliberate drag never showed this, but a fast click
    // can arrive as a bare press while _currentMousePosition is still wherever the pointer last idled
    // (a previous selection, a different pane, ...). Without this, handleMouseSelection() below
    // anchors the drag on that stale position instead of where the button actually went down, and the
    // whole selection appears to jump the moment the drag starts. Mirrors the same update
    // sendMouseMoveEvent already does for exactly this reason.
    if (newPosition != _currentMousePosition)
    {
        _currentMousePosition = newPosition;
        updateHoveringHyperlinkState();
    }

    // Counted once, for every button, before anyone looks at it: the count is both what a `clicks:`
    // binding matches on and what picks drag, word or line below. A press on another cell starts a new
    // sequence by the counter's own rule -- which is what this function used to reset by hand -- and
    // each button keeps its own, so a right-click for the context menu does not end a double-click.
    auto const clickCount = _clickCounter.press(_currentMousePosition, button);

    // The terminal's own selection -- and so a binding standing in for it -- sees a press only when no
    // application asked for the mouse, or the bypass modifier overrides that.
    auto const reachesSelection =
        !allowPassMouseEventToApp(modifiers) || isModeEnabled(DECMode::MousePassiveTracking);
    auto const claim =
        reachesSelection && claimant ? claimant(clickCount) : MousePressClaim::Unclaimed;

    if (button == MouseButton::Left && claim == MousePressClaim::Unclaimed)
    {
        _leftMouseButtonPressed = true;
        _lastMousePixelPositionOnLeftClick = pixelPosition;
        if (reachesSelection)
            uiHandledHint = handleMouseSelection(modifiers, clickCount) || uiHandledHint;
    }
```

(The rest of the function, from `    verifyState();` down, stays as it is.)

In `handleMouseSelection` (:1239-1286): change the signature to `bool Terminal::handleMouseSelection(Modifiers modifiers, uint8_t clickCount)`; delete its three counting lines

```cpp
    double const diffMs = chrono::duration<double, std::milli>(_currentTime - _lastClick).count();
    _lastClick = _currentTime;
    _speedClicks = ((diffMs >= 0.0 && diffMs <= 1000.0 ? _speedClicks : 0) % 4) + 1;
```

replace `        _speedClicks = 0; // Don't count Shift+Click in the speed-click sequence.` with

```cpp
        _clickCounter.reset(); // Don't count Shift+Click in the multi-click sequence.
```

and replace `    switch (_speedClicks)` with `    switch (clickCount)`.

In `sendMouseMoveEvent` (:1440-1446) replace

```cpp
        // Speed-clicks are only counted when not moving the mouse in between, so reset on mouse move here.
        _speedClicks = 0;
```

with

```cpp
        // Multi-clicks are only counted when not moving the mouse in between, so reset on mouse move here.
        _clickCounter.reset();
```

and, in the comment at the top of `sendMouseMoveEvent`, replace `    // - the internal speed-clicks counter (for tracking rapid multi click) is reset` with

```cpp
    // - the multi-click counter (_clickCounter, for tracking rapid multi click) is reset
```

The old counter's name survives in three test comments; reword them so they name `_clickCounter`, which reads the terminal's injected steady clock. In `src/vtbackend/screen/Terminal_selection_test.cpp`, `Terminal.a_press_at_a_new_cell_starts_a_fresh_click_sequence` (:238), replace `WITHOUT resetting the speed-click counter that` with `WITHOUT resetting the multi-click counter that`. In `src/contour/display/DisplayRendering_test.cpp`, replace (:1437-1438)

```cpp
    // Double-click on "hello" (column 2, line 0): press, release, then a second press at the same
    // spot within the terminal's 1000ms speed-click window.
```

with

```cpp
    // Double-click on "hello" (column 2, line 0): press, release, then a second press at the same
    // spot within the terminal's 1000ms multi-click window (Terminal::_clickCounter).
```

and replace (:1572-1577)

```cpp
    // The terminal's own click-speed counter (_speedClicks in Terminal::handleMouseSelection) counts
    // presses within 1s of REAL elapsed time (Terminal::_currentTime, advanced from steady_clock by the
    // live display's render loop -- unlike the headless MockTerm tests, which advance a simulated clock
    // explicitly). Without a real pause here, this press lands within that window of the deselect click
    // above and the one before it, so it reads as a double/triple-click and selects a word or a whole
    // line instead of starting a plain drag.
```

with

```cpp
    // The terminal's multi-click counter (Terminal::_clickCounter, a ClickCounter on the terminal's
    // injected steady clock) counts presses within 1s of REAL elapsed time here: this session runs on
    // TerminalClocks::system(), unlike the headless MockTerm tests, whose manual clock moves only when a
    // test advances it. Without a real pause here, this press lands within that window of the deselect
    // click above and the one before it, so it reads as a double/triple-click and selects a word or a
    // whole line instead of starting a plain drag.
```

Run `git grep -n -e _speedClicks -e _lastClick -e speed-click src` — expected: no output. Run `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_selection_test.cpp src/contour/display/DisplayRendering_test.cpp`.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[terminal]"`
Expected: zero warnings; `All tests passed` — the three new cases and every existing multi-click, Shift+click and fast-click regression in `Terminal_selection_test.cpp` (they reset by position and by move, never by the clock, so a `MockTerm` clock that does not advance keeps them green).

- [ ] **Step 7: Commit**

```bash
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_selection_test.cpp src/contour/display/DisplayRendering_test.cpp
git commit -F - <<'EOF'
vtbackend: let a binding claim a press before the multi-click selection

A Ctrl+triple-click bound to SelectCommandBlock has to be decided before the
terminal turns the third click into an extended-word selection, and only
the terminal knows the click count. Count each press once with the
ClickCounter, offer the count to a claimant the caller supplies, and skip
the selection and the drag when the claimant takes it.

The claimant is asked only where the terminal's own selection would run, so
a press an application asked for never reaches a binding. The 5-argument
overload keeps its behaviour by passing a claimant that never claims.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.6: Bind mouse buttons by click count (`clicks:`)

**Files:**
- Modify: `src/vtbackend/input/InputBinding.hpp` (the whole struct, `match`, `operator==`, `operator<`, the formatter)
- Modify: `src/contour/config/Config.hpp` (`apply()` :251-269; mouse `applyBuiltinFallback` :277-280; `appendOrCreateBinding` :1527-1545; `tryAddMouse` :1672-1676; `Writer::format(MouseInputMapping)` :1731-1738)
- Modify: `src/contour/config/Config.cpp` (includes :1-38; `builtinFallbackMouseMappings` :83-105; `applyFallbackTable` :154-166; mouse `applyBuiltinFallback` :169-175; anonymous namespace after `describeInputMappingRow` :256-270; the `InputMappings` loader :2699-2746; `tryAddMouse` :2862-2874)
- Modify: `docs/configuration/key-mapping.md` (built-in bindings table :18-23; mouse-button paragraph :94-96)
- Test: `src/contour/config/Config_test.cpp`

**Interfaces:**
- Consumes: Task 5.3 `SelectCommandBlock`; Task 5.4 `ClickCounter::MaxClickCount`.
- Produces:
  ```cpp
  // vtbackend::InputBinding<Input, Binding> gains (last member):
  std::optional<uint8_t> clickCount {};
  bool match(InputBinding<I, B> const&, MatchModes, Modifiers, I input, std::optional<uint8_t> clickCount = std::nullopt);
  // contour::config
  std::vector<actions::Action> const* apply(Mappings&&, Input, Modifiers, uint8_t actualModeFlags, std::optional<uint8_t> clickCount = std::nullopt);
  [[nodiscard]] ActionList const* applyBuiltinFallback(Config const&, vtbackend::MouseButton, vtbackend::Modifiers, uint8_t actualModeFlags, std::optional<uint8_t> clickCount = std::nullopt);
  ```
  YAML: `- { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer }`. `clicks` is 1..4, mouse rows only. A row without `clicks` matches only presses that no click-count row claimed.
  Built-in fallback: `Ctrl+Left` × 3 → `SelectCommandBlock { .target = Pointer, .part = Output }`.

- [ ] **Step 1: Write the failing tests** — append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: a mouse binding may name a click count", "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], mouse: Left, action: FollowHyperlink }
    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer }
    - { mods: [Control], mouse: Left, clicks: 9, action: SelectAll }
    - { mods: [Control], key: 'F5', clicks: 2, action: SelectAll }
)"sv);

    auto const& mappings = config.inputMappings.value();

    // The click-count row is a binding of its own, not a second action on the plain Ctrl+Left one; the
    // out-of-range count and the count on a key are dropped.
    REQUIRE(mappings.mouseMappings.size() == 2);
    CHECK(mappings.keyMappings.empty());

    auto const ctrl = Modifiers { Modifier::Control };
    auto const* plain = contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 });
    REQUIRE(plain != nullptr);
    CHECK(std::holds_alternative<contour::actions::FollowHyperlink>(plain->at(0)));

    auto const* third =
        contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 });
    REQUIRE(third != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(third->at(0)));
    CHECK(std::get<contour::actions::SelectCommandBlock>(third->at(0)).target
          == vtbackend::CommandBlockTarget::Pointer);

    CHECK(contour::config::apply(mappings.mouseMappings, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 2 })
          == nullptr);
}

TEST_CASE("Config: Ctrl+triple-click selects the output under the pointer by default", "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;

    auto const config = contour::config::Config {};
    auto const ctrl = Modifiers { Modifier::Control };

    auto const* bound = contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 3 });
    REQUIRE(bound != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::SelectCommandBlock>(bound->at(0)));
    auto const& select = std::get<contour::actions::SelectCommandBlock>(bound->at(0));
    CHECK(select.target == vtbackend::CommandBlockTarget::Pointer);
    CHECK(select.part == vtbackend::CommandBlockPart::Output);

    // Only the third click: a plain Ctrl+click and a Ctrl+double-click are not claimed by it.
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }) == nullptr);
    CHECK(contour::config::applyBuiltinFallback(config, MouseButton::Left, ctrl, uint8_t { 0 }, uint8_t { 2 })
          == nullptr);

    // A fallback, not a default (see builtinFallbackMouseMappings): no default row names a click count.
    CHECK(std::ranges::none_of(config.inputMappings.value().mouseMappings,
                               [](auto const& mapping) { return mapping.clickCount.has_value(); }));
}

TEST_CASE("Config: a click count survives the writer", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto config = contour::config::Config {};
    auto mappings = config.inputMappings.value();
    mappings.mouseMappings.push_back(contour::config::MouseInputMapping {
        .modes { vtbackend::MatchModes {} },
        .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
        .input = vtbackend::MouseButton::Middle,
        .binding = { { contour::actions::SelectAll {} } },
        .clickCount = uint8_t { 2 } });
    config.inputMappings = mappings;

    auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
    REQUIRE(written.contains("clicks: 2"));

    auto const reloaded = loadFromYaml(dir, written);
    CHECK(std::ranges::any_of(reloaded.inputMappings.value().mouseMappings, [](auto const& mapping) {
        return mapping.input == vtbackend::MouseButton::Middle && mapping.clickCount == uint8_t { 2 };
    }));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `'clickCount': is not a member of 'vtbackend::InputBinding<...>'` and no `apply` overload taking five arguments.

- [ ] **Step 3: Give a binding a click count** — replace the body of `src/vtbackend/input/InputBinding.hpp` from `#include <format>` down to the end of the file with:

```cpp
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace vtbackend
{

template <typename Input, typename Binding>
struct InputBinding
{
    MatchModes modes;
    Modifiers modifiers;
    Input input;
    Binding binding;

    /// For a mouse button: the multi-click count (1..4) this binding fires on. nullopt fires on any
    /// press no click-count binding claimed. Always nullopt for keys and characters.
    std::optional<uint8_t> clickCount {};
};

template <typename Input, typename Binding>
bool match(InputBinding<Input, Binding> const& binding,
           MatchModes modes,
           Modifiers modifiers,
           Input input,
           std::optional<uint8_t> clickCount = std::nullopt)
{
    return binding.modes == modes && binding.modifiers == modifiers && binding.input == input
           && binding.clickCount == clickCount;
}

template <typename I, typename O>
bool operator==(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return a.modes == b.modes && a.modifiers == b.modifiers && a.input == b.input
           && a.clickCount == b.clickCount;
}

template <typename I, typename O>
bool operator!=(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    return !(a == b);
}

template <typename I, typename O>
bool operator<(InputBinding<I, O> const& a, InputBinding<I, O> const& b) noexcept
{
    if (a.modes < b.modes)
        return true;
    if (a.modes != b.modes)
        return false;

    if (a.modifiers < b.modifiers)
        return true;
    if (a.modifiers != b.modifiers)
        return false;

    if (a.input < b.input)
        return true;
    if (a.input != b.input)
        return false;

    return a.clickCount < b.clickCount;
}

} // namespace vtbackend

template <typename I, typename O>
struct std::formatter<vtbackend::InputBinding<I, O>>
{
    auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(vtbackend::InputBinding<I, O> const& binding, auto& ctx) const
    {
        auto const clicks =
            binding.clickCount ? std::format(" x{}", static_cast<int>(*binding.clickCount)) : std::string {};
        return std::format_to(
            ctx.out(), "{} {} {}{}", binding.modes, binding.modifiers, binding.input, clicks);
    }
};
```

- [ ] **Step 4: Match on it** — in `src/contour/config/Config.hpp`, replace `apply()` (:251-269) with:

```cpp
template <typename Input, std::ranges::input_range Mappings>
    requires std::same_as<std::ranges::range_value_t<Mappings>, vtbackend::InputBinding<Input, ActionList>>
std::vector<actions::Action> const* apply(Mappings&& mappings,
                                          Input input,
                                          vtbackend::Modifiers modifiers,
                                          uint8_t actualModeFlags,
                                          std::optional<uint8_t> clickCount = std::nullopt)
{
    // Forwarded rather than taken by const&: std::views::filter is not const-iterable, so a filtered
    // view over the gated fallback table could not be walked through a const reference.
    //
    // The click count must match exactly: a lookup without one (an ordinary press) never reaches a
    // `clicks:` row, and a click-count lookup (TerminalSession's claimant) never reaches a plain one.
    for (vtbackend::InputBinding<Input, ActionList> const& mapping: std::forward<Mappings>(mappings))
    {
        if (mapping.modifiers == modifiers && mapping.input == input && mapping.clickCount == clickCount
            && helper::testMatchMode(actualModeFlags, mapping.modes))
        {
            return &mapping.binding;
        }
    }
    return nullptr;
}
```

and add `@param clickCount The press's multi-click count for a click-count lookup; nullopt for an ordinary one.` to the doc comment above it. Replace the mouse `applyBuiltinFallback` declaration (:277-280) with:

```cpp
[[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config,
                                                     vtbackend::MouseButton button,
                                                     vtbackend::Modifiers modifiers,
                                                     uint8_t actualModeFlags,
                                                     std::optional<uint8_t> clickCount = std::nullopt);
```

Replace `appendOrCreateBinding` (:1527-1545) with:

```cpp
    template <typename Input>
    void appendOrCreateBinding(std::vector<vtbackend::InputBinding<Input, ActionList>>& bindings,
                               vtbackend::MatchModes modes,
                               vtbackend::Modifiers modifier,
                               Input input,
                               actions::Action action,
                               std::optional<uint8_t> clickCount = std::nullopt)
    {
        for (auto& binding: bindings)
        {
            if (match(binding, modes, modifier, input, clickCount))
            {
                binding.binding.emplace_back(std::move(action));
                return;
            }
        }

        bindings.emplace_back(vtbackend::InputBinding<Input, ActionList> {
            modes, modifier, input, ActionList { std::move(action) }, clickCount });
    }
```

Replace the `tryAddMouse` declaration (:1672-1676) with:

```cpp
    bool tryAddMouse(std::vector<MouseInputMapping>& bindings,
                     vtbackend::MatchModes modes,
                     vtbackend::Modifiers modifier,
                     YAML::Node const& node,
                     actions::Action action,
                     std::optional<uint8_t> clickCount);
```

Replace `Writer::format(MouseInputMapping v)` (:1731-1738) with:

```cpp
    [[nodiscard]] std::string format(MouseInputMapping v)
    {
        auto actionAndModes = format(" action: {} }}", v.binding[0]);
        auto button = v.clickCount ? format(" mouse: {}, clicks: {}", v.input, static_cast<int>(*v.clickCount))
                                   : format(" mouse: {}", v.input);
        return format("{:<30},{:<30},{:<30}\n", format("- {{ mods: [{}]", format(v.modifiers)), button, actionAndModes);
    }
```

- [ ] **Step 5: Read it, and add the default** — in `src/contour/config/Config.cpp`, add `#include <vtbackend/input/ClickCounter.hpp>` to the `vtbackend/` includes and `#include <charconv>` to the standard ones.

Append this row to `builtinFallbackMouseMappings()`'s table, after the `WheelRight` row (:104):

```cpp
        // Ctrl+triple-click selects the output of the command under the pointer (spec §7.1). Offered to
        // the click-count claimant TerminalSession hands the terminal, so it is decided BEFORE the
        // terminal would turn the third click into an extended-word selection.
        FallbackMouseMapping {
            .mapping = { .modes { vtbackend::MatchModes {} },
                         .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                         .input = vtbackend::MouseButton::Left,
                         .binding = { { actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer,
                                                                      .part = vtbackend::CommandBlockPart::Output } } },
                         .clickCount = uint8_t { 3 } } },
```

Replace `applyFallbackTable` and the mouse `applyBuiltinFallback` (:154-175) with:

```cpp
    template <typename Input>
    [[nodiscard]] ActionList const* applyFallbackTable(std::vector<FallbackMapping<Input>> const& table,
                                                       Config const& config,
                                                       Input input,
                                                       vtbackend::Modifiers modifiers,
                                                       uint8_t actualModeFlags,
                                                       std::optional<uint8_t> clickCount)
    {
        auto enabledMappings =
            table
            | std::views::filter([&config](FallbackMapping<Input> const& row) { return row.enabled(config); })
            | std::views::transform(&FallbackMapping<Input>::mapping);
        return apply(enabledMappings, input, modifiers, actualModeFlags, clickCount);
    }
} // namespace

ActionList const* applyBuiltinFallback(Config const& config,
                                       vtbackend::MouseButton button,
                                       vtbackend::Modifiers modifiers,
                                       uint8_t actualModeFlags,
                                       std::optional<uint8_t> clickCount)
{
    return applyFallbackTable(
        builtinFallbackMouseMappings(), config, button, modifiers, actualModeFlags, clickCount);
}
```

(keep the doc comment above `applyFallbackTable`), and in the key `applyBuiltinFallback` change the call to `applyFallbackTable(builtinFallbackKeyMappings(), config, key, modifiers, actualModeFlags, std::nullopt)`.

In `describeInputMappingRow` (:259) replace the `Fields` line with:

```cpp
        static constexpr auto Fields =
            std::array { "action"sv, "key"sv, "mouse"sv, "clicks"sv, "mods"sv, "mode"sv };
```

and after `describeInputMappingRow`'s closing brace (:270) insert:

```cpp

    /// The `clicks:` field of an input_mapping row: the multi-click count its mouse button fires on.
    /// @param row The mapping node.
    /// @return An empty optional when the row names none; the count when it is 1..MaxClickCount; the
    ///         offending text as the error otherwise.
    [[nodiscard]] std::expected<std::optional<uint8_t>, std::string> parseClickCount(YAML::Node const& row)
    {
        auto const node = row["clicks"];
        if (!node)
            return std::optional<uint8_t> {};

        auto const text = node.IsScalar() ? node.as<std::string>() : std::string {};
        auto value = 0;
        auto const* const end = text.data() + text.size();
        auto const [parsedUpTo, error] = std::from_chars(text.data(), end, value);
        if (error != std::errc {} || parsedUpTo != end || value < 1
            || value > vtbackend::ClickCounter::MaxClickCount)
            return std::unexpected(text);
        return std::optional<uint8_t> { static_cast<uint8_t>(value) };
    }
```

In the `InputMappings` loader replace :2701-2746 (from `auto action = parseAction(mapping);` to the end of the `else` that builds `unparsed`) with:

```cpp
                auto action = parseAction(mapping);
                auto mods = parseModifier(mapping);
                auto mode = parseMatchModes(mapping);
                auto const clicks = parseClickCount(mapping);
                if (action && mods && mode && clicks)
                {
                    if (clicks->has_value() && !mapping["mouse"])
                    {
                        errorLog()("Dropping input_mapping entry [{}]: 'clicks' applies to mouse buttons only.",
                                   describeInputMappingRow(mapping));
                    }
                    else if (tryAddKey(where, *mode, *mods, mapping["key"], *action))
                    {
                        logger()("Adding input mapping: mods: {:<20} modifiers: {:<20} key: {:<20} "
                                 "action: {:<20}",
                                 *mods,
                                 *mode,
                                 mapping["key"].as<std::string>(),
                                 *action);
                    }
                    else if (tryAddMouse(where.mouseMappings, *mode, *mods, mapping["mouse"], *action, *clicks))
                    {
                        logger()("Adding input mapping: mods: {:<20} modifiers: {:<20} mouse: {:<18} action: "
                                 "{:<20}",
                                 *mods,
                                 *mode,
                                 mapping["mouse"].as<std::string>(),
                                 *action);
                    }
                    else
                    {
                        errorLog()("Dropping input_mapping entry [{}]: it names neither a bindable "
                                   "'key' nor a 'mouse' button.",
                                   describeInputMappingRow(mapping));
                    }
                }
                else
                {
                    // Say which row died and which field killed it. Before this, one misspelled
                    // modifier made an entire binding vanish with no output at all -- see issue
                    // #1987, where `mods: [Shift,Alt,Ctrl]` produced nothing but silence.
                    auto unparsed = std::vector<std::string_view> {};
                    if (!action)
                        unparsed.emplace_back("action");
                    if (!mods)
                        unparsed.emplace_back("mods");
                    if (!mode)
                        unparsed.emplace_back("mode");
                    if (!clicks)
                        unparsed.emplace_back("clicks (expected 1 to 4)");
                    errorLog()("Dropping input_mapping entry [{}]: could not parse its {}.",
                               describeInputMappingRow(mapping),
                               unparsed | core::views::joinWith(", "));
                }
```

Replace `tryAddMouse` (:2862-2874) with:

```cpp
bool YAMLConfigReader::tryAddMouse(std::vector<MouseInputMapping>& bindings,
                                   vtbackend::MatchModes modes,
                                   vtbackend::Modifiers modifier,
                                   YAML::Node const& node,
                                   actions::Action action,
                                   std::optional<uint8_t> clickCount)
{
    auto mouseButton = parseMouseButton(node);
    if (!mouseButton)
        return false;

    appendOrCreateBinding(bindings, modes, modifier, *mouseButton, std::move(action), clickCount);
    return true;
}
```

- [ ] **Step 6: Document it** — in `docs/configuration/key-mapping.md`, add a row to the built-in bindings table after the `Right` mouse button row:

```markdown
| `Ctrl` + triple-click (left button) | Select the output of the command under the pointer (needs a shell that emits OSC 133 marks) |
```

and after the mouse-button list (:94-96) append:

````markdown

A mouse binding may also name a click count with `clicks` (1 to 4). Such a binding fires on that
click of a multi-click sequence only, and it is consulted *before* Contour's own double-, triple- and
quadruple-click selection, so it replaces that selection for the chord it names:

```yaml
input_mapping:
    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer, part: output }
```

A binding without `clicks` fires on every press that no `clicks` binding claimed.
````

Run `clang-format -i src/vtbackend/input/InputBinding.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp`.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test contour_test contour_gui_test`, then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[input]"`
Expected: zero warnings; `All tests passed` — including the existing round trip of the generated default config (its mouse rows name no count, so the writer's output for them is unchanged) and `config.builtinFallbackMouseMappings` ("it is a FALLBACK, not a default").

- [ ] **Step 8: Commit**

```bash
git add src/vtbackend/input/InputBinding.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/Config_test.cpp docs/configuration/key-mapping.md
git commit -F - <<'EOF'
config: bind mouse buttons by click count

A mouse binding may now name `clicks: 1..4`. It is a binding of its own --
appending to the plain binding of the same chord would make Ctrl+click and
Ctrl+triple-click one row -- and the lookup matches the count exactly, so an
ordinary press never reaches a click-count row and the click-count lookup
never reaches a plain one.

Ctrl+triple-click selecting the output under the pointer is a built-in
fallback rather than a default, for the reason every new binding is one: a
default never reaches a user whose contour.yml already lists the bindings.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.7: Consult click-count bindings before the terminal's selection (`TerminalSession`)

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp` (after `bool applyFallbackMouseBinding(vtbackend::MouseButton button);` :548)
- Modify: `src/contour/session/TerminalSession.cpp` (`sendMousePressEvent` :2025-2048; new function after `applyFallbackMouseBinding` :2081-2089)
- Test: `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: Task 5.5 `Terminal::sendMousePressEvent(..., MousePressClaimant const&)`, `MousePressClaim`; Task 5.6 `config::apply(..., clickCount)`, `config::applyBuiltinFallback(..., clickCount)`.
- Produces (private): `[[nodiscard]] config::ActionList const* clickCountBinding(vtbackend::MouseButton button, vtbackend::Modifiers modifiers, uint8_t clickCount) const;`

- [ ] **Step 1: Write the failing test** — append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: Ctrl+triple-click selects the output under the pointer",
          "[contour][session][input][semanticblocks]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("\033]133;A\033\\$ ls\r\n\033]133;C\033\\alpha beta\r\ngamma\r\n");
    session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    auto const overOutput = vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(2) };
    auto const pixels = vtbackend::PixelCoordinate {};
    auto const clickThrice = [&](Modifiers modifiers) {
        for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
        {
            session->sendMousePressEvent(modifiers, vtbackend::MouseButton::Left, overOutput, pixels);
            session->sendMouseReleaseEvent(modifiers, vtbackend::MouseButton::Left, pixels);
        }
    };

    SECTION("the third Ctrl+click selects the block's output")
    {
        clickThrice(Modifiers { vtbackend::Modifier::Control });
        CHECK(session->terminal().extractSelectionText() == "alpha beta\ngamma\n");
    }

    SECTION("without Ctrl the third click is still the terminal's own selection")
    {
        clickThrice(Modifiers {});
        CHECK(session->terminal().extractSelectionText() != "alpha beta\ngamma\n");
    }

    SECTION("an application that asked for the mouse keeps its clicks")
    {
        session->terminal().writeToScreen("\033[?1000h");
        clickThrice(Modifiers { vtbackend::Modifier::Control });
        CHECK_FALSE(session->terminal().selectionAvailable());
        CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "TerminalSession: Ctrl+triple-click selects the output under the pointer"`
Expected: FAIL in "the third Ctrl+click selects the block's output" — the selection is the extended word (`alpha`), because nothing consults the click-count bindings yet.

- [ ] **Step 3: Declare the lookup** — in `src/contour/session/TerminalSession.hpp`, after `    bool applyFallbackMouseBinding(vtbackend::MouseButton button);` (:548) insert:

```cpp

    /// The actions a `clicks:` mouse binding has for this press: the user's mappings first, then the
    /// built-in fallbacks. Only looks: it runs under the terminal lock, inside the claimant.
    /// @param button The pressed button.
    /// @param modifiers The chord, with the bypass modifier already removed.
    /// @param clickCount The press's place in its multi-click sequence.
    /// @return The bound actions, or nullptr (also while key mappings are toggled off).
    [[nodiscard]] config::ActionList const* clickCountBinding(vtbackend::MouseButton button,
                                                              vtbackend::Modifiers modifiers,
                                                              uint8_t clickCount) const;
```

- [ ] **Step 4: Offer the count, then act** — in `src/contour/session/TerminalSession.cpp` replace the start of `sendMousePressEvent` (:2030-2042, from `    auto const uiHandledHint = false;` through the `sanitizedModifier` definition) with:

```cpp
    auto const uiHandledHint = false;
    input::inputLog()("Mouse press received: {} {}\n", modifiers, button);

    terminal().tick(steady_clock::now());

    auto const sanitizedModifier = modifiers.contains(_config.bypassMouseProtocolModifiers.value())
                                       ? modifiers.without(_config.bypassMouseProtocolModifiers.value())
                                       : modifiers;

    // A binding that names a click count is offered the press BEFORE the terminal's own multi-click
    // selection acts on it -- only the terminal knows the count, and only until it has used it. The
    // claimant only looks the binding up (it runs under the lock); the actions run after it is released.
    config::ActionList const* claimed = nullptr;
    auto const handledByApp = core::locked(_terminal, [&]() {
        return _terminal.sendMousePressEvent(
            modifiers, button, pos, pixelPosition, uiHandledHint, [&](uint8_t clickCount) {
                claimed = clickCountBinding(button, sanitizedModifier, clickCount);
                return claimed != nullptr ? vtbackend::MousePressClaim::Claimed
                                          : vtbackend::MousePressClaim::Unclaimed;
            });
    });

    if (claimed != nullptr)
    {
        executeAllActions(*claimed);
        return;
    }

    if (handledByApp)
        return;
```

(The rest of the function — the user `mouseMappings` lookup, the horizontal-wheel gesture and the built-in fallback — is unchanged.) After `applyFallbackMouseBinding` (ends :2089) insert:

```cpp

config::ActionList const* TerminalSession::clickCountBinding(MouseButton button,
                                                             Modifiers modifiers,
                                                             uint8_t clickCount) const
{
    // A binding that will not run must not claim: with key mappings toggled off, executeAllActions()
    // refuses it, and a claimed press would have swallowed the click for nothing.
    if (!_allowKeyMappings)
        return nullptr;

    auto const flags = matchModeFlags();
    if (auto const* actions =
            config::apply(_config.inputMappings.value().mouseMappings, button, modifiers, flags, clickCount))
        return actions;
    return config::applyBuiltinFallback(_config, button, modifiers, flags, clickCount);
}
```

Run `clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[session]"`
Expected: zero warnings; `All tests passed` — the new case and the existing right-click/middle-click/context-menu routing cases.

- [ ] **Step 6: Commit**

```bash
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
session: consult click-count bindings before the terminal's selection

Hand the terminal a claimant with every press: it looks the press's click
count up in the user's mouse mappings and then the built-in fallbacks, and
the terminal skips its own word/line selection when a binding claims it.
The actions run once the terminal lock is released.

With this, Ctrl+triple-click selects the output of the command under the
pointer, while a press an application asked for still goes to the
application and an unmodified triple-click still selects as before.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.8: `CopyCommandBlock` and `CopyCommandLine`

**Files:**
- Modify: `src/contour/config/Actions.hpp` (structs, variant, documentation, catalog, `formatActionArguments` — the same anchors as Task 5.3, now after `SelectCommandBlock`)
- Modify: `src/contour/config/Config.cpp` (`parseAction`, after the `SelectCommandBlock` branch of Task 5.3)
- Modify: `src/contour/command/Command.cpp` (`commandArguments`, after the `SelectCommandBlock` lambda)
- Modify: `src/contour/session/TerminalSession.hpp`, `src/contour/session/TerminalSession.cpp` (after the `SelectCommandBlock` handler of Task 5.3)
- Test: `src/contour/config/Actions_test.cpp`, `src/contour/command/CommandCatalog_test.cpp`, `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: Task 5.2 `Terminal::commandBlockText`, `Terminal::targetedCommandBlock`; C6 `vtbackend::sanitizeCommandLine(std::string_view, SanitizePurpose)` (`vtbackend/shell/CommandLineSanitizer.hpp`); C1 `CommandBlockRecord::commandLine` (raw).
- Produces:
  ```cpp
  struct CopyCommandBlock { vtbackend::CommandBlockTarget target = Last; vtbackend::CommandBlockPart part = Output; vtbackend::CommandBlockId block {}; };
  struct CopyCommandLine { vtbackend::CommandBlockTarget target = Last; vtbackend::CommandBlockId block {}; };
  // TerminalSession (private)
  [[nodiscard]] std::expected<void, vtbackend::CommandBlockActionError> copyCommandBlock(vtbackend::CommandBlockTarget target, vtbackend::CommandBlockPart part, vtbackend::CommandBlockId block);
  ```
  YAML: `action: CopyCommandBlock, target: ..., part: ..., block: N`; `action: CopyCommandLine, target: ..., block: N`.

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/config/Actions_test.cpp`:

```cpp
TEST_CASE("actions: the block copy actions write their arguments as config tokens", "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::CopyCommandBlock {} })
          == "CopyCommandBlock, target: last, part: output");
    CHECK(std::format("{}",
                      actions::Action { actions::CopyCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer,
                                                                    .part = vtbackend::CommandBlockPart::Prompt,
                                                                    .block = vtbackend::CommandBlockId(4) } })
          == "CopyCommandBlock, target: pointer, part: input, block: 4");
    CHECK(std::format("{}", actions::Action { actions::CopyCommandLine {} }) == "CopyCommandLine, target: last");
    CHECK(std::format("{}",
                      actions::Action { actions::CopyCommandLine { .target = vtbackend::CommandBlockTarget::Cursor,
                                                                   .block = vtbackend::CommandBlockId(2) } })
          == "CopyCommandLine, target: cursor, block: 2");
}
```

Append to `src/contour/command/CommandCatalog_test.cpp`:

```cpp
TEST_CASE("The block copy commands carry their target in their id", "[contour][palette][semanticblocks]")
{
    using contour::command::commandId;
    namespace actions = contour::actions;

    CHECK(commandId(actions::CopyCommandBlock {}) == "CopyCommandBlock");
    CHECK(commandId(actions::CopyCommandBlock { .part = vtbackend::CommandBlockPart::Prompt })
          == "CopyCommandBlock:last:input:0");
    CHECK(commandId(actions::CopyCommandLine {}) == "CopyCommandLine");
    CHECK(commandId(actions::CopyCommandLine { .block = vtbackend::CommandBlockId(3) }) == "CopyCommandLine:last:3");
}
```

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: the block copy actions copy one part of a block",
          "[contour][session][clipboard][semanticblocks]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);
    clipboard->setText(QStringLiteral("untouched"));

    session->terminal().writeToScreen("\033]133;A\033\\$ make test\r\n\033]133;C;cmdline_url=make%20test\033\\ok\r\n");
    session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    SECTION("the output of the last command")
    {
        CHECK((*session)(contour::actions::CopyCommandBlock {}));
        CHECK(clipboard->text().toStdString() == "ok");
    }

    SECTION("its prompt")
    {
        CHECK((*session)(contour::actions::CopyCommandBlock { .part = vtbackend::CommandBlockPart::Prompt }));
        CHECK(clipboard->text().toStdString() == "$ make test");
    }

    SECTION("its command line")
    {
        CHECK((*session)(contour::actions::CopyCommandLine {}));
        CHECK(clipboard->text().toStdString() == "make test");
    }

    SECTION("a part that is not there leaves the clipboard alone")
    {
        CHECK_FALSE(
            (*session)(contour::actions::CopyCommandBlock { .target = vtbackend::CommandBlockTarget::Cursor }));
        CHECK(clipboard->text().toStdString() == "untouched");
    }

    clipboard->clear();
}

TEST_CASE("TerminalSession: CopyCommandLine copies a line that is safe to paste",
          "[contour][session][clipboard][semanticblocks]")
{
    // A command line arrives from whatever wrote to the pty. One carrying ESC [ 201 ~ would end a
    // bracketed paste early and run the rest as typed input in the shell it is pasted into, so the copy
    // is sanitised for insertion: the ESC goes, the harmless rest stays.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    session->terminal().writeToScreen("\033]133;A\033\\$ \r\n\033]133;C;cmdline_url=ls%1b%5b201~%3bid\033\\");
    session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    CHECK((*session)(contour::actions::CopyCommandLine {}));
    CHECK(clipboard->text().toStdString() == "ls[201~;id");

    clipboard->clear();
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'CopyCommandBlock': is not a member of 'contour::actions'`.

- [ ] **Step 3: Add the actions** — in `src/contour/config/Actions.hpp`, after the `SelectCommandBlock` struct (Task 5.3) insert:

```cpp
// OSC 133: copy one part of a command block as plain text; a non-zero block pins it
struct CopyCommandBlock{ vtbackend::CommandBlockTarget target = vtbackend::CommandBlockTarget::Last; vtbackend::CommandBlockPart part = vtbackend::CommandBlockPart::Output; vtbackend::CommandBlockId block {}; };
// OSC 133: copy a block's command line, made safe to paste; a non-zero block pins it
struct CopyCommandLine{ vtbackend::CommandBlockTarget target = vtbackend::CommandBlockTarget::Last; vtbackend::CommandBlockId block {}; };
```

Append both to `Action` after `SelectCommandBlock`:

```cpp
                            SelectCommandBlock,
                            CopyCommandBlock,
                            CopyCommandLine>;
```

Documentation, after `SelectCommandBlock`'s entry:

```cpp
    constexpr inline std::string_view CopyCommandBlock {
        "Copies one part of a command block into the clipboard as plain text (target: pointer, cursor or "
        "last; part: output, input or all). Requires a shell that emits OSC 133 marks."
    };
    constexpr inline std::string_view CopyCommandLine {
        "Copies the command line of a command block into the clipboard, with the control characters that "
        "would be unsafe to paste removed (target: pointer, cursor or last). Requires a shell that reports "
        "its command lines (OSC 133;C cmdline_url)."
    };
```

Catalog rows, after the `SelectCommandBlock` row:

```cpp
        ActionCatalogEntry {
            "CopyCommandBlock", Action { CopyCommandBlock {} }, documentation::CopyCommandBlock },
        ActionCatalogEntry { "CopyCommandLine", Action { CopyCommandLine {} }, documentation::CopyCommandLine },
```

`formatActionArguments`, after the `SelectCommandBlock` lambda:

```cpp
            [](CopyCommandBlock const& a) {
                return std::format(", target: {}, part: {}{}",
                                   contour::config::configEnumToken(a.target),
                                   contour::config::configEnumToken(a.part),
                                   formatPinnedBlock(a.block));
            },
            [](CopyCommandLine const& a) {
                return std::format(
                    ", target: {}{}", contour::config::configEnumToken(a.target), formatPinnedBlock(a.block));
            },
```

- [ ] **Step 4: Identity and parsing** — in `src/contour/command/Command.cpp`, after the `SelectCommandBlock` lambda insert:

```cpp
            [](CopyCommandBlock const& a) -> CommandArguments {
                return blockArguments(a.target, a.part, a.block);
            },
            [](CopyCommandLine const& a) -> CommandArguments {
                if (a.target == vtbackend::CommandBlockTarget::Last && a.block.value == 0)
                    return {};
                auto const target = config::configEnumToken(a.target);
                return { .id = std::format("{}:{}", target, a.block.value), .title = std::format(" ({})", target) };
            },
```

In `src/contour/config/Config.cpp`'s `parseAction`, after the `SelectCommandBlock` branch insert:

```cpp

        if (holds_alternative<actions::CopyCommandBlock>(action))
        {
            auto const target = enumActionArgument(node, "target", vtbackend::CommandBlockTarget::Last);
            auto const part = enumActionArgument(node, "part", vtbackend::CommandBlockPart::Output);
            if (!target || !part)
                return std::nullopt;
            return actions::CopyCommandBlock { .target = *target, .part = *part, .block = pinnedBlockArgument(node) };
        }

        if (holds_alternative<actions::CopyCommandLine>(action))
        {
            auto const target = enumActionArgument(node, "target", vtbackend::CommandBlockTarget::Last);
            if (!target)
                return std::nullopt;
            return actions::CopyCommandLine { .target = *target, .block = pinnedBlockArgument(node) };
        }
```

- [ ] **Step 5: Run them** — in `src/contour/session/TerminalSession.hpp`, after `bool operator()(actions::SelectCommandBlock const& action);` add:

```cpp
    bool operator()(actions::CopyCommandBlock const& action);
    bool operator()(actions::CopyCommandLine const& action);
```

and next to the `copyLastCommandBlock` declaration (:1036-1039) add (and `#include <expected>` to the standard includes, after `<cstdint>`, if absent):

```cpp

    /// Copies one part of a command block as plain text; refuses (and leaves the clipboard alone) when
    /// there is nothing to copy.
    /// @param target Which block.
    /// @param part Which part of it.
    /// @param block A pinned block (zero: resolve @p target).
    /// @return Nothing once copied; otherwise why nothing was -- no such block, or an empty part.
    [[nodiscard]] std::expected<void, vtbackend::CommandBlockActionError> copyCommandBlock(
        vtbackend::CommandBlockTarget target, vtbackend::CommandBlockPart part, vtbackend::CommandBlockId block);
```

In `src/contour/session/TerminalSession.cpp` add `#include <vtbackend/shell/CommandLineSanitizer.hpp>` to the `vtbackend/` includes, and after the `SelectCommandBlock` handler insert:

```cpp

std::expected<void, vtbackend::CommandBlockActionError> TerminalSession::copyCommandBlock(
    vtbackend::CommandBlockTarget target, vtbackend::CommandBlockPart part, vtbackend::CommandBlockId block)
{
    auto const text = core::locked(_terminal, [&]() {
        return terminal().commandBlockText(target, part, vtbackend::CaptureRendition::PlainText, block);
    });

    // As in copyLastCommandBlock(): copying nothing would only destroy what the clipboard held.
    if (!text)
        return std::unexpected(text.error());
    if (text->empty())
        return std::unexpected(vtbackend::CommandBlockActionError::EmptyPart);

    copyToClipboard(*text);
    return {};
}

bool TerminalSession::operator()(actions::CopyCommandBlock const& action)
{
    return copyCommandBlock(action.target, action.part, action.block).has_value();
}

bool TerminalSession::operator()(actions::CopyCommandLine const& action)
{
    auto const commandLine = core::locked(_terminal, [&]() {
        auto const* record = terminal().targetedCommandBlock(action.target, action.block);
        return record != nullptr ? record->commandLine : std::string {};
    });

    // Insert, not Display: the clipboard's next stop is a paste, usually into a shell. A raw line may
    // carry ESC [ 201 ~, which ends a bracketed paste early and runs the rest as typed input; Display
    // would keep it harmless on screen but put U+FFFD into what the user pastes.
    auto const text = vtbackend::sanitizeCommandLine(commandLine, vtbackend::SanitizePurpose::Insert);
    if (text.empty())
        return false;

    copyToClipboard(text);
    return true;
}
```

Run `clang-format -i` on every file touched in this task.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test`, then `out/build/clangcl-debug/bin/contour_test.exe "[semanticblocks],[actions],[palette]"` and `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[semanticblocks]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/contour/config/Actions.hpp src/contour/config/Config.cpp src/contour/command/Command.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/config/Actions_test.cpp src/contour/command/CommandCatalog_test.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
actions: add CopyCommandBlock and CopyCommandLine

Copy one part of any command block -- the one under the pointer, at the
cursor, the last one, or a pinned one -- and copy a block's command line.

The command line is copied sanitised for insertion: its next stop is a
paste, usually into a shell, and a raw line can carry the ESC [ 201 ~ that
ends a bracketed paste early. Both actions refuse rather than copy an empty
string over whatever the clipboard held.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.9: Let `CopySelection` fall back to the last command's output

**Files:**
- Modify: `src/contour/config/BlockActionEnums.hpp` (Task 5.3)
- Modify: `src/contour/config/Actions.hpp` (`CopySelection` struct :58; move the `std::formatter<contour::actions::CopyFormat>` specialization :933-948 above `formatActionArguments`; `formatActionArguments`)
- Modify: `src/contour/config/Config.cpp` (`CopySelection` branch of `parseAction` :3364-3389)
- Modify: `src/contour/command/Command.cpp` (`CopySelection` lambda :122-126)
- Modify: `src/contour/session/TerminalSession.cpp` (`operator()(actions::CopySelection)` :2555-2574)
- Test: `src/contour/config/Actions_test.cpp`, `src/contour/command/CommandCatalog_test.cpp`, `src/contour/config/Config_test.cpp`, `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: Task 5.8 `TerminalSession::copyCommandBlock`.
- Produces:
  ```cpp
  namespace contour::actions { enum class CopyFallback : uint8_t { None = 0, LastCommandOutput }; }
  struct CopySelection { CopyFormat format = CopyFormat::Text; CopyFallback fallback = CopyFallback::None; };
  // contour::config: configEnumValues<contour::actions::CopyFallback>() (none|last_command_output)
  ```
  YAML: `action: CopySelection, fallback: last_command_output`. `formatActionArguments` now also writes `format:` (it never did, so a bound `format: HTML` was lost on every config write).

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/config/Actions_test.cpp`:

```cpp
TEST_CASE("actions: CopySelection writes the arguments it carries", "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::CopySelection {} }) == "CopySelection");
    CHECK(std::format("{}", actions::Action { actions::CopySelection { .format = actions::CopyFormat::HTML } })
          == "CopySelection, format: HTML");
    CHECK(std::format("{}",
                      actions::Action { actions::CopySelection {
                          .fallback = actions::CopyFallback::LastCommandOutput } })
          == "CopySelection, fallback: last_command_output");
}
```

Append to `src/contour/command/CommandCatalog_test.cpp`:

```cpp
TEST_CASE("CopySelection's fallback is part of its identity", "[contour][palette][semanticblocks]")
{
    using contour::command::commandId;
    namespace actions = contour::actions;

    CHECK(commandId(actions::CopySelection {}) == "CopySelection");
    CHECK(commandId(actions::CopySelection { .format = actions::CopyFormat::HTML }) == "CopySelection:HTML");
    CHECK(commandId(actions::CopySelection { .fallback = actions::CopyFallback::LastCommandOutput })
          == "CopySelection:Text:last_command_output");
}
```

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: CopySelection reads its fallback", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'F1', action: CopySelection, fallback: last_command_output }
    - { mods: [Control], key: 'F2', action: CopySelection, format: HTML }
    - { mods: [Control], key: 'F3', action: CopySelection, fallback: everything }
)"sv);

    auto const& keys = config.inputMappings.value().keyMappings;
    auto const bound = [&](vtbackend::Key key) -> std::optional<contour::actions::CopySelection> {
        auto const it = std::ranges::find_if(keys, [key](auto const& mapping) { return mapping.input == key; });
        if (it == keys.end() || it->binding.empty())
            return std::nullopt;
        return std::get<contour::actions::CopySelection>(it->binding.at(0));
    };

    REQUIRE(bound(vtbackend::Key::F1).has_value());
    CHECK(bound(vtbackend::Key::F1)->fallback == contour::actions::CopyFallback::LastCommandOutput);
    CHECK(bound(vtbackend::Key::F1)->format == contour::actions::CopyFormat::Text);

    REQUIRE(bound(vtbackend::Key::F2).has_value());
    CHECK(bound(vtbackend::Key::F2)->format == contour::actions::CopyFormat::HTML);
    CHECK(bound(vtbackend::Key::F2)->fallback == contour::actions::CopyFallback::None);

    CHECK_FALSE(bound(vtbackend::Key::F3).has_value());
}
```

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: CopySelection may fall back to the last command's output",
          "[contour][session][clipboard][semanticblocks]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    session->terminal().writeToScreen("\033]133;A\033\\$ make\r\n\033]133;C\033\\built\r\n");
    session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
    auto const copyOrLastOutput =
        contour::actions::CopySelection { .fallback = contour::actions::CopyFallback::LastCommandOutput };

    SECTION("with nothing selected it copies what the last command printed")
    {
        CHECK((*session)(copyOrLastOutput));
        CHECK(clipboard->text().toStdString() == "built");
    }

    SECTION("a selection wins over the fallback")
    {
        CHECK((*session)(contour::actions::SelectCommandBlock { .part = vtbackend::CommandBlockPart::Prompt }));
        CHECK((*session)(copyOrLastOutput));
        CHECK(clipboard->text().toStdString() == "$ make\n");
    }

    clipboard->clear();
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'CopyFallback': is not a member of 'contour::actions'`.

- [ ] **Step 3: Add the enum and its tokens** — in `src/contour/config/BlockActionEnums.hpp`, add `#include <cstdint>` and, before `namespace contour::config`, insert:

```cpp
namespace contour::actions
{

/// What CopySelection copies when nothing is selected.
enum class CopyFallback : uint8_t
{
    None = 0,          ///< Nothing new: the empty selection is copied, as it always was.
    LastCommandOutput, ///< The output of the most recently finished command (OSC 133).
};

} // namespace contour::actions

```

Inside `namespace detail` add:

```cpp

    /// What CopySelection copies when nothing is selected: `fallback:` in input_mapping.
    inline constexpr auto CopyFallbackTable = std::array {
        ConfigEnumInfo<actions::CopyFallback> { actions::CopyFallback::None, "none", "Nothing" },
        ConfigEnumInfo<actions::CopyFallback> {
            actions::CopyFallback::LastCommandOutput, "last_command_output", "The last command's output" },
    };
```

and after the last specialization:

```cpp

template <>
constexpr std::span<ConfigEnumInfo<actions::CopyFallback> const> configEnumValues() noexcept
{
    return detail::CopyFallbackTable;
}
```

- [ ] **Step 4: Carry, write and identify it** — in `src/contour/config/Actions.hpp`, replace `struct CopySelection{ CopyFormat format = CopyFormat::Text; };` (:58) with

```cpp
struct CopySelection{ CopyFormat format = CopyFormat::Text; CopyFallback fallback = CopyFallback::None; };
```

Cut the `template <> struct std::formatter<contour::actions::CopyFormat> ... };` specialization (:933-948) and paste it unchanged directly after the `std::formatter<contour::actions::Direction>` specialization (it must be declared before `formatActionArguments` uses it). In `formatActionArguments`, after the `SetTabBarPosition` lambda insert:

```cpp
            [](CopySelection const& a) {
                auto arguments = std::string {};
                if (a.format != CopyFormat::Text)
                    arguments += std::format(", format: {}", a.format);
                if (a.fallback != CopyFallback::None)
                    arguments += std::format(", fallback: {}", contour::config::configEnumToken(a.fallback));
                return arguments;
            },
```

In `src/contour/command/Command.cpp` replace the `CopySelection` lambda (:122-126) with:

```cpp
            [](CopySelection const& a) -> CommandArguments {
                if (a.fallback == CopyFallback::None)
                {
                    if (a.format == CopyFormat::Text)
                        return {};
                    return { .id = std::format("{}", a.format), .title = std::format(" as {}", a.format) };
                }
                return { .id = std::format("{}:{}", a.format, config::configEnumToken(a.fallback)),
                         .title = std::format(" as {}, else the last output", a.format) };
            },
```

- [ ] **Step 5: Parse it** — in `src/contour/config/Config.cpp`, in the `CopySelection` branch of `parseAction` (:3364-3389): insert as the branch's first statements

```cpp
            // Read first: it is independent of the format, and a misspelt one drops the binding.
            auto const fallback = enumActionArgument(node, "fallback", actions::CopyFallback::None);
            if (!fallback)
                return std::nullopt;
```

replace `return actions::CopySelection { p->second };` with `return actions::CopySelection { p->second, *fallback };`, replace `return actions::CopySelection { actions::CopyFormat::Text };` with `return actions::CopySelection { actions::CopyFormat::Text, *fallback };`, and before the branch's closing brace (after the `if (auto nodeFormat = ...) { ... }` block) add

```cpp
            return actions::CopySelection { actions::CopyFormat::Text, *fallback };
```

- [ ] **Step 6: Fall back** — in `src/contour/session/TerminalSession.cpp`, replace the `case actions::CopyFormat::Text:` arm of `operator()(actions::CopySelection copySelection)` (:2560-2563) with:

```cpp
        case actions::CopyFormat::Text: {
            // Copy the selection in pure text, plus whitespaces and newline.
            auto const text = core::locked(_terminal, [&]() { return terminal().extractSelectionText(); });

            // Nothing selected: with the fallback, copy what the last command printed instead -- so the
            // copy chord right after a command does something useful rather than emptying the clipboard.
            if (text.empty() && copySelection.fallback == actions::CopyFallback::LastCommandOutput)
                return copyCommandBlock(vtbackend::CommandBlockTarget::Last, vtbackend::CommandBlockPart::Output, {})
                    .has_value();

            copyToClipboard(text);
            break;
        }
```

Run `clang-format -i` on every file touched in this task.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test`, then `out/build/clangcl-debug/bin/contour_test.exe` and `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config],[semanticblocks],[clipboard]"`
Expected: zero warnings; `All tests passed` (the existing `CopySelection` id checks in `CommandCatalog_test.cpp` still hold).

- [ ] **Step 8: Commit**

```bash
git add src/contour/config/BlockActionEnums.hpp src/contour/config/Actions.hpp src/contour/config/Config.cpp src/contour/command/Command.cpp src/contour/session/TerminalSession.cpp src/contour/config/Actions_test.cpp src/contour/command/CommandCatalog_test.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
actions: let CopySelection fall back to the last command's output

`fallback: last_command_output` makes the copy chord copy what the last
command printed when nothing is selected, instead of copying an empty
selection over the clipboard.

CopySelection's arguments are now also written back: the writer never wrote
`format:`, so a bound `format: HTML` was lost on every config save.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.10: Clear everything above the current prompt (`Grid`, `Screen`, `Terminal`)

**Files:**
- Modify: `src/vtbackend/grid/Grid.hpp` (after `void clearHistory();` :751), `src/vtbackend/grid/Grid.cpp` (after `Grid::clearHistory()` :230-237)
- Modify: `src/vtbackend/screen/Screen.hpp` (after `void clearScreen();` :436), `src/vtbackend/screen/Screen.cpp` (after `Screen::clearScreen()` :1475-1493)
- Modify: `src/vtbackend/screen/Terminal.hpp` (after the `targetedCommandBlock` declaration of Task 5.2), `src/vtbackend/screen/Terminal.cpp` (after `Terminal::targetedCommandBlock`)
- Test: `src/vtbackend/grid/Grid_test.cpp` (append before the final `// }}}`), `src/vtbackend/screen/Terminal_block_actions_test.cpp`

**Interfaces:**
- Consumes: C2 `Grid::evictedRowCount()` — advanced inside `syncStableFloor()`, so this task never writes `_evictedRowCount`; Task 5.1 `commandBlockRows`; `Grid::scrollUp`, `Grid::clearHistory`, `Grid::addressableTop`; `Terminal::invalidateFoldRanges()`, `refreshFoldState()`, `clampToHistory()`, `scrollbackBufferCleared()`.
- Produces:
  ```cpp
  [[nodiscard]] LineCount Grid::dropRowsAbove(LineOffset head);
  [[nodiscard]] LineCount Screen::dropRowsAbove(LineOffset head);
  [[nodiscard]] std::expected<void, CommandBlockActionError> Terminal::clearToPrompt();
  ```

- [ ] **Step 1: Write the failing Grid tests** — in `src/vtbackend/grid/Grid_test.cpp`, before the file's last line (`// }}}`), insert:

```cpp
// }}}

// {{{ dropRowsAbove (ClearToPrompt)
namespace
{
/// Page of 3 rows, room for 5 in the history: A and B scrolled into it, C, D, E on the page.
[[nodiscard]] Grid fiveRowsOnAThreeRowPage()
{
    return setupGrid(PageSize { LineCount(3), ColumnCount(5) },
                     false,
                     LineCount(5),
                     { "AAAAA", "BBBBB", "CCCCC", "DDDDD", "EEEEE" });
}
} // namespace

TEST_CASE("Grid.dropRowsAbove.aPageHeadMovesToTheTopAndTheHistoryGoes", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    REQUIRE(grid.historyLineCount() == LineCount(2));
    auto const evictedBefore = grid.evictedRowCount();
    auto const idOfD = grid.stableLineIdOf(LineOffset(1));

    CHECK(grid.dropRowsAbove(LineOffset(1)) == LineCount(1));

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "DDDDD");
    CHECK(grid.lineText(LineOffset(1)) == "EEEEE");
    CHECK(grid.lineTextTrimmed(LineOffset(2)).empty());
    CHECK(grid.evictedRowCount() == evictedBefore + 3); // A, B and C
    CHECK(grid.lineOffsetOf(idOfD) == LineOffset(0));   // the kept rows keep their identity
    CHECK(grid.stableRangeFloor() == idOfD);
}

TEST_CASE("Grid.dropRowsAbove.aHistoryHeadDropsOnlyTheOlderRows", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    auto const evictedBefore = grid.evictedRowCount();

    CHECK(grid.dropRowsAbove(LineOffset(-1)) == LineCount(0));

    CHECK(grid.historyLineCount() == LineCount(1));
    CHECK(grid.addressableTop() == LineOffset(-1));
    CHECK(grid.lineText(LineOffset(-1)) == "BBBBB");
    CHECK(grid.lineText(LineOffset(0)) == "CCCCC");
    CHECK(grid.evictedRowCount() == evictedBefore + 1); // A
}

TEST_CASE("Grid.dropRowsAbove.theTopmostRowDropsNothing", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    auto const evictedBefore = grid.evictedRowCount();

    CHECK(grid.dropRowsAbove(grid.addressableTop()) == LineCount(0));

    CHECK(grid.historyLineCount() == LineCount(2));
    CHECK(grid.evictedRowCount() == evictedBefore);
}

TEST_CASE("Grid.dropRowsAbove.thePageTopDropsTheHistoryOnly", "[grid][semanticblocks]")
{
    auto grid = fiveRowsOnAThreeRowPage();
    auto const evictedBefore = grid.evictedRowCount();

    CHECK(grid.dropRowsAbove(LineOffset(0)) == LineCount(0));

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineText(LineOffset(0)) == "CCCCC");
    CHECK(grid.evictedRowCount() == evictedBefore + 2);
}
```

(The inserted `// }}}` closes the section the file ends in; the file's own last `// }}}` then closes the new one.)

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `'dropRowsAbove': is not a member of 'vtbackend::Grid'`.

- [ ] **Step 3: Implement the grid operation** — in `src/vtbackend/grid/Grid.hpp`, after `    void clearHistory();` (:751) insert:

```cpp

    /// Drops every row above @p head -- the scrollback and the page rows alike -- so that @p head
    /// becomes the topmost row (ClearToPrompt).
    ///
    /// Rows leave through the grid's own bookkeeping -- scrollUp() then clearHistory() for a head on
    /// the page, a shrunken history for one in the scrollback -- so the stable floor and the eviction
    /// count follow by construction (@see evictedRowCount).
    /// @param head The row to keep at the top; in [addressableTop(), the page's last row].
    /// @return How far the page scrolled up: page rows sit this many rows higher than before. Zero
    ///         for a head in the scrollback, whose rows keep their offsets.
    [[nodiscard]] LineCount dropRowsAbove(LineOffset head);
```

In `src/vtbackend/grid/Grid.cpp`, after `Grid::clearHistory()` (:237) insert:

```cpp

LineCount Grid::dropRowsAbove(LineOffset head)
{
    Require(addressableTop() <= head && head < boxed_cast<LineOffset>(_pageSize.lines));

    if (head >= LineOffset(0))
    {
        // On the page: scroll the head to the top, which moves the rows above it into the history,
        // then drop the history -- the ED 2 + ED 3 pair, stopped at the head.
        auto const shift = LineCount::cast_from(unbox(head));
        if (shift > LineCount(0))
            scrollUp(shift);
        clearHistory();
        return shift;
    }

    // In the history: drop the oldest rows down to the head by shrinking the used range, exactly as
    // clampHistory() drops them; syncStableFloor() then counts every row the floor passes.
    _linesUsed -= LineCount::cast_from(unbox(head) + unbox(historyLineCount()));
    syncStableFloor();
    verifyState();
    return LineCount(0);
}
```

- [ ] **Step 4: Run the Grid tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[grid]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 5: Write the failing Terminal tests** — append to `src/vtbackend/screen/Terminal_block_actions_test.cpp`:

```cpp
TEST_CASE("Terminal.clearToPrompt.leavesTheLivePromptAtTheTop", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);
    auto const& grid = mock.terminal.primaryScreen().grid();
    auto const evictedBefore = grid.evictedRowCount();
    REQUIRE(mock.terminal.selectCommandBlock(CommandBlockTarget::Last, CommandBlockPart::Output).has_value());

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    CHECK(grid.historyLineCount() == LineCount(0));
    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "$");
    CHECK(mock.terminal.primaryScreen().realCursorPosition().line == LineOffset(0));
    CHECK(grid.evictedRowCount() == evictedBefore + 5); // rows 0..4 went
    CHECK_FALSE(mock.terminal.selectionAvailable());    // a selection of dropped rows cannot survive

    // The records stay, as on any eviction; no row is theirs any more.
    CHECK(mock.terminal.commandBlocks().size() == 3);
    auto const* live = mock.terminal.commandBlockAt(LineOffset(0));
    REQUIRE(live != nullptr);
    CHECK(live == mock.terminal.commandBlocks().current());
}

TEST_CASE("Terminal.clearToPrompt.keepsARunningCommandsPromptAndOutput", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    mock.writeToScreen("old\r\n\033]133;A\033\\$ sleep\r\n\033]133;C\033\\zzz\r\n"sv); // no ;D: still running

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    auto const& grid = mock.terminal.primaryScreen().grid();
    CHECK(grid.lineTextTrimmed(LineOffset(0)) == "$ sleep");
    CHECK(grid.lineTextTrimmed(LineOffset(1)) == "zzz");
    CHECK(mock.terminal.primaryScreen().realCursorPosition().line == LineOffset(2));
}

TEST_CASE("Terminal.clearToPrompt.aHeadInTheScrollbackKeepsItsRows", "[semanticblocks][blockactions]")
{
    // Four rows of page: the running command's prompt has scrolled into the history.
    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(20) }, LineCount(10) };
    mock.writeToScreen("old\r\nolder\r\n\033]133;A\033\\$ seq 6\r\n\033]133;C\033\\1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n"sv);
    auto const& grid = mock.terminal.primaryScreen().grid();
    REQUIRE(grid.historyLineCount() == LineCount(6));
    REQUIRE(grid.lineTextTrimmed(LineOffset(-4)) == "$ seq 6");
    auto const evictedBefore = grid.evictedRowCount();

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    CHECK(grid.historyLineCount() == LineCount(4));
    CHECK(grid.lineTextTrimmed(LineOffset(-4)) == "$ seq 6");
    CHECK(grid.evictedRowCount() == evictedBefore + 2); // old, older
    CHECK(mock.terminal.primaryScreen().realCursorPosition().line == LineOffset(3));
}

TEST_CASE("Terminal.clearToPrompt.dropsTheFoldOfADroppedBlock", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    twoCommandsAndAPrompt(mock);
    auto const headId = mock.terminal.primaryScreen().grid().stableLineIdOf(LineOffset(0));
    mock.terminal.foldState().collapse(headId);

    REQUIRE(mock.terminal.clearToPrompt().has_value());

    mock.terminal.refreshFoldState();
    CHECK_FALSE(mock.terminal.foldState().isCollapsed(headId));
}

TEST_CASE("Terminal.clearToPrompt.withoutAPromptChangesNothing", "[semanticblocks][blockactions]")
{
    auto mock = makeTerm();
    mock.writeToScreen("a\r\nb\r\nc\r\n"sv);

    CHECK(mock.terminal.clearToPrompt() == std::unexpected(CommandBlockActionError::NoBlock));
    CHECK(mock.terminal.primaryScreen().grid().lineTextTrimmed(LineOffset(0)) == "a");

    twoCommandsAndAPrompt(mock);
    mock.writeToScreen("\033[?1049h"sv);
    CHECK(mock.terminal.clearToPrompt() == std::unexpected(CommandBlockActionError::NoBlock));
}
```

- [ ] **Step 6: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `'clearToPrompt': is not a member of 'vtbackend::Terminal'`.

- [ ] **Step 7: Implement it on the screen and the terminal** — in `src/vtbackend/screen/Screen.hpp`, after `    void clearScreen();` (:436) insert:

```cpp

    /// Drops every row above @p head (@see Grid::dropRowsAbove) and moves the cursor, and the cursor
    /// DECSC saved, up with the rows they stood on (never above the page top).
    /// @param head The row to keep at the top.
    /// @return How far the page scrolled up.
    [[nodiscard]] LineCount dropRowsAbove(LineOffset head);
```

In `src/vtbackend/screen/Screen.cpp`, after `Screen::clearScreen()` (:1493) insert:

```cpp

LineCount Screen::dropRowsAbove(LineOffset head)
{
    auto const shift = _grid.dropRowsAbove(head);
    auto const raised = [shift](LineOffset line) {
        return std::max(line - boxed_cast<LineOffset>(shift), LineOffset(0));
    };
    _cursor.position.line = raised(_cursor.position.line);
    _savedCursor.position.line = raised(_savedCursor.position.line);
    updateCursorIterator();
    return shift;
}
```

In `src/vtbackend/screen/Terminal.hpp`, after the `targetedCommandBlock` declaration insert:

```cpp

    /// Drops every row above the current prompt's head -- scrollback and page alike -- so the prompt
    /// (and a running command's output) starts at the top of the page (kitty's `to_cursor`).
    ///
    /// A Grid operation, so the eviction count, the stable floor and the fold state follow as on any
    /// eviction; the block records stay, as they do for ED 3.
    /// Does NOT take the lock -- the caller holds it.
    /// @return Nothing on success; NoBlock while an alternate screen is up or when the cursor's row
    ///         belongs to no block (no shell integration).
    [[nodiscard]] std::expected<void, CommandBlockActionError> clearToPrompt();
```

In `src/vtbackend/screen/Terminal.cpp`, after `Terminal::targetedCommandBlock` insert:

```cpp

std::expected<void, CommandBlockActionError> Terminal::clearToPrompt()
{
    if (!isPrimaryScreen())
        return std::unexpected(CommandBlockActionError::NoBlock);

    auto& screen = primaryScreen();
    auto const block = vtbackend::commandBlockRows(
        GridBlockRows { screen.grid() }, screen.realCursorPosition().line, CommandBlockPart::PromptAndOutput);
    if (!block)
        return std::unexpected(block.error());

    auto const shift = boxed_cast<LineOffset>(screen.dropRowsAbove(block->first));

    // The vi cursor rides along with its row, or lands on the new top when its row is gone.
    _viCommands.cursorPosition.line =
        std::max(_viCommands.cursorPosition.line - shift, screen.grid().addressableTop());

    invalidateFoldRanges();
    refreshFoldState();
    clampToHistory();
    scrollbackBufferCleared();
    return {};
}
```

Run `clang-format -i src/vtbackend/grid/Grid.hpp src/vtbackend/grid/Grid.cpp src/vtbackend/grid/Grid_test.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_block_actions_test.cpp`.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[semanticblocks]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "[grid]"`
Expected: zero warnings; `All tests passed` in both.

- [ ] **Step 9: Commit**

```bash
git add src/vtbackend/grid/Grid.hpp src/vtbackend/grid/Grid.cpp src/vtbackend/grid/Grid_test.cpp src/vtbackend/screen/Screen.hpp src/vtbackend/screen/Screen.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_block_actions_test.cpp
git commit -F - <<'EOF'
vtbackend: clear everything above the current prompt

Drop every row above the head of the block the cursor is in, scrollback and
page alike, so the prompt -- and a running command's output -- starts at
the top of the page.

It is a Grid operation built from the grid's own bookkeeping: a head on the
page is scrolled to the top and the history cleared, a head in the history
shrinks the used range as clampHistory() does. The stable floor, the
eviction count behind the gutter's line numbers and the fold pruning
therefore follow without a special case. The cursor, the DECSC cursor and
the vi cursor move up with their rows.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.11: The `ClearToPrompt` action

**Files:**
- Modify: `src/contour/config/Actions.hpp` (struct, variant after `CopyCommandLine`, documentation, catalog)
- Modify: `src/contour/session/TerminalSession.hpp`, `src/contour/session/TerminalSession.cpp` (after the `ClearHistoryAndReset` handler :2382-2389)
- Test: `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: Task 5.10 `Terminal::clearToPrompt()`; `TerminalSession::announceScrollableLineCount(vtbackend::LineCount)` (:995).
- Produces: `struct ClearToPrompt {};` — YAML `action: ClearToPrompt`, no default binding.

- [ ] **Step 1: Write the failing test** — append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: ClearToPrompt leaves the prompt at the top of an empty scrollback",
          "[contour][session][actions][semanticblocks]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    for (auto const i: std::views::iota(0, 40))
        session->terminal().writeToScreen(std::format("line {}\r\n", i));

    SECTION("with a prompt, everything above it goes")
    {
        session->terminal().writeToScreen("\033]133;A\033\\$ ");
        REQUIRE(session->terminal().primaryScreen().historyLineCount() > vtbackend::LineCount(0));

        CHECK((*session)(contour::actions::ClearToPrompt {}));

        CHECK(session->terminal().primaryScreen().historyLineCount() == vtbackend::LineCount(0));
        CHECK(session->terminal().primaryScreen().grid().lineTextTrimmed(vtbackend::LineOffset(0)) == "$");
    }

    SECTION("without one, nothing is cleared")
    {
        auto const history = session->terminal().primaryScreen().historyLineCount();
        CHECK_FALSE((*session)(contour::actions::ClearToPrompt {}));
        CHECK(session->terminal().primaryScreen().historyLineCount() == history);
    }
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `'ClearToPrompt': is not a member of 'contour::actions'`.

- [ ] **Step 3: Add the action** — in `src/contour/config/Actions.hpp`: after the `CopyCommandLine` struct insert

```cpp
struct ClearToPrompt{}; // OSC 133: drop every row above the current prompt, scrollback and page alike
```

append `ClearToPrompt` to `Action` after `CopyCommandLine`; add to `documentation`

```cpp
    constexpr inline std::string_view ClearToPrompt {
        "Clears the scrollback and the screen above the current prompt, leaving the prompt (and a running "
        "command's output) at the top. Requires a shell that emits OSC 133 marks."
    };
```

and append the catalog row after `CopyCommandLine`'s:

```cpp
        ActionCatalogEntry { "ClearToPrompt", Action { ClearToPrompt {} }, documentation::ClearToPrompt },
```

- [ ] **Step 4: Run it** — in `src/contour/session/TerminalSession.hpp`, after `    bool operator()(actions::ClearHistoryAndReset);` (:740) add `    bool operator()(actions::ClearToPrompt);`. In `src/contour/session/TerminalSession.cpp`, after the `ClearHistoryAndReset` handler (:2389) insert:

```cpp

bool TerminalSession::operator()(actions::ClearToPrompt)
{
    auto const [cleared, scrollable] =
        core::locked(_terminal, [&]() -> std::pair<bool, vtbackend::LineCount> {
            auto const result = terminal().clearToPrompt();
            return { result.has_value(), _terminal.viewport().scrollableLineCount() };
        });

    // Outside the lock, as withFolding() does: the QML binding this wakes reaches back into the session.
    announceScrollableLineCount(scrollable);
    return cleared;
}
```

Run `clang-format -i src/contour/config/Actions.hpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test`, then `out/build/clangcl-debug/bin/contour_test.exe "[actions]"` and `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[semanticblocks]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/contour/config/Actions.hpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
actions: add ClearToPrompt

Bindable as `action: ClearToPrompt`: clears the scrollback and the screen
above the current prompt. A shell without OSC 133 marks gets a refusal, not
a cleared screen -- there is no prompt to clear back to. The new scrollable
range is announced after the lock is released, as for the folding actions.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.12: Pipe input into a launched program (`ExternalLauncher::runWithStdin`)

**Files:**
- Modify: `src/contour/platform/ExternalLauncher.hpp` (includes; after `execute` in `class ExternalLauncher`)
- Modify: `src/contour/platform/QtExternalLauncher.hpp`, `src/contour/platform/QtExternalLauncher.cpp`
- Modify: `src/contour/platform/PortalExternalLauncher.hpp` (after `execute` :83-84), `src/contour/platform/PortalExternalLauncher.cpp` (after `execute` :100-104)
- Modify: `src/contour/test/LauncherFixtures.hpp` (the test double)
- Test: `src/contour/platform/ExternalLauncher_test.cpp` (`contour_gui_test`)

**Interfaces:**
- Consumes: `SpawnError`, `isReachableProgram` (`ExternalLauncher.hpp`).
- Produces:
  ```cpp
  // contour::platform::ExternalLauncher
  [[nodiscard]] virtual std::expected<void, SpawnError> runWithStdin(QString const& program,
                                                                     QStringList const& arguments,
                                                                     QByteArray input) = 0;
  // contour::test::RecordingExternalLauncher
  struct PipedExecution { QString program; QStringList arguments; QByteArray input; };
  std::vector<PipedExecution> piped; std::optional<contour::platform::SpawnError> pipedError;
  ```
  Deviation from C5 (recorded in the contract notes): `SpawnError` rather than `LaunchError`, which exists and is URL-only, and the Qt types every sibling of this interface takes.

- [ ] **Step 1: Write the failing test** — append to `src/contour/platform/ExternalLauncher_test.cpp`:

```cpp
TEST_CASE("QtExternalLauncher: runWithStdin refuses a program that is not there", "[contour][launcher][semanticblocks]")
{
    auto launcher = contour::platform::QtExternalLauncher {};
    auto const result = launcher.runWithStdin(
        QStringLiteral("contour-test-no-such-program-4711"), QStringList {}, QByteArrayLiteral("x"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == contour::platform::SpawnError::NotFound);
}

#if !defined(_WIN32)
TEST_CASE("QtExternalLauncher: runWithStdin hands the bytes to the program's stdin", "[contour][launcher][semanticblocks]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto const target = dir.filePath(QStringLiteral("piped.txt"));

    auto launcher = contour::platform::QtExternalLauncher {};
    // sh -c 'cat > "$0"' TARGET: the first argument after the script is its $0.
    auto const started = launcher.runWithStdin(QStringLiteral("sh"),
                                               QStringList { QStringLiteral("-c"), QStringLiteral("cat > \"$0\""), target },
                                               QByteArrayLiteral("built\n"));
    REQUIRE(started.has_value());

    // The launcher keeps the child (and the bytes it still has to write) alive after returning.
    CHECK(QTest::qWaitFor(
        [&] {
            QFile file(target);
            return file.open(QIODevice::ReadOnly) && file.readAll() == QByteArrayLiteral("built\n");
        },
        5000));
}
#endif
```

and add to the file's includes:

```cpp
#include <contour/platform/QtExternalLauncher.hpp>

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `'runWithStdin': is not a member of 'contour::platform::QtExternalLauncher'`.

- [ ] **Step 3: Add it to the interface** — in `src/contour/platform/ExternalLauncher.hpp`, add `#include <QtCore/QByteArray>` and `#include <QtCore/QString>` to the Qt includes, and after the `execute` declaration insert:

```cpp

    /// Starts @p program with @p arguments and writes @p input to its standard input, then closes it
    /// -- for a program that consumes text rather than a file (`wl-copy`, a notes script).
    ///
    /// Like runDetached(), the report is about starting: the child keeps running after this returns,
    /// and the launcher keeps it (and whatever of @p input is still to be written) alive until it ends.
    /// Its output goes nowhere.
    ///
    /// @param program The executable path or bare program name.
    /// @param arguments The argument list.
    /// @param input The bytes for its standard input.
    /// @return Nothing once started, or why it was not.
    [[nodiscard]] virtual std::expected<void, SpawnError> runWithStdin(QString const& program,
                                                                       QStringList const& arguments,
                                                                       QByteArray input) = 0;
```

- [ ] **Step 4: Implement it for Qt** — replace the body of `class QtExternalLauncher` in `src/contour/platform/QtExternalLauncher.hpp` with:

```cpp
class QtExternalLauncher final: public ExternalLauncher
{
  public:
    QtExternalLauncher();
    QtExternalLauncher(QtExternalLauncher const&) = delete;
    QtExternalLauncher& operator=(QtExternalLauncher const&) = delete;
    QtExternalLauncher(QtExternalLauncher&&) = delete;
    QtExternalLauncher& operator=(QtExternalLauncher&&) = delete;
    /// Children still running when the launcher goes (at exit) are ended with it, as QProcess does.
    ~QtExternalLauncher() override;

    [[nodiscard]] std::expected<void, LaunchError> openUrl(QUrl const& url) override;
    [[nodiscard]] std::expected<void, SpawnError> runDetached(QString const& program,
                                                              QStringList const& arguments) override;
    [[nodiscard]] std::expected<int, SpawnError> execute(QString const& program,
                                                         QStringList const& arguments) override;
    [[nodiscard]] std::expected<void, SpawnError> runWithStdin(QString const& program,
                                                               QStringList const& arguments,
                                                               QByteArray input) override;

  private:
    /// The children runWithStdin() started: a QProcess must outlive the bytes it still has to write.
    /// Finished ones are released on the next call rather than on a signal, so this needs no QObject.
    std::vector<std::unique_ptr<QProcess>> _children;
};
```

and add, above `namespace contour::platform`, `#include <memory>`, `#include <vector>` and a forward declaration `class QProcess;`. In `src/contour/platform/QtExternalLauncher.cpp`, after `namespace contour::platform {` insert:

```cpp

QtExternalLauncher::QtExternalLauncher() = default;

QtExternalLauncher::~QtExternalLauncher() = default;
```

and at the end of the namespace:

```cpp

std::expected<void, SpawnError> QtExternalLauncher::runWithStdin(QString const& program,
                                                                 QStringList const& arguments,
                                                                 QByteArray input)
{
    if (!isReachableProgram(program))
        return std::unexpected(SpawnError::NotFound);

    std::erase_if(_children, [](auto const& child) { return child->state() == QProcess::NotRunning; });

    auto child = std::make_unique<QProcess>();
    child->setStandardOutputFile(QProcess::nullDevice());
    child->setStandardErrorFile(QProcess::nullDevice());
    child->start(program, arguments);
    if (!child->waitForStarted())
        return std::unexpected(SpawnError::StartFailed);

    child->write(input);
    child->closeWriteChannel(); // after the queued bytes: the program sees them, then end of input
    _children.push_back(std::move(child));
    return {};
}
```

- [ ] **Step 5: Delegate it on the portal path, and record it in the double** — in `src/contour/platform/PortalExternalLauncher.hpp`, after the `execute` declaration add:

```cpp
    [[nodiscard]] std::expected<void, SpawnError> runWithStdin(QString const& program,
                                                               QStringList const& arguments,
                                                               QByteArray input) override;
```

and in `src/contour/platform/PortalExternalLauncher.cpp`, after `PortalExternalLauncher::execute`:

```cpp

std::expected<void, SpawnError> PortalExternalLauncher::runWithStdin(QString const& program,
                                                                     QStringList const& arguments,
                                                                     QByteArray input)
{
    return _processes->runWithStdin(program, arguments, std::move(input));
}
```

In `src/contour/test/LauncherFixtures.hpp`, add `#include <QtCore/QByteArray>`; after the `Execution` struct add

```cpp

    struct PipedExecution
    {
        QString program;
        QStringList arguments;
        QByteArray input;
    };
```

after `execute()` add

```cpp

    [[nodiscard]] std::expected<void, contour::platform::SpawnError> runWithStdin(
        QString const& program, QStringList const& arguments, QByteArray input) override
    {
        piped.push_back({ program, arguments, std::move(input) });
        if (pipedError)
            return std::unexpected(*pipedError);
        return {};
    }
```

and after `std::vector<Execution> executed;` add

```cpp
    std::vector<PipedExecution> piped;
```

and after `executeError`:

```cpp

    /// What runWithStdin() fails with; empty means the child started.
    std::optional<contour::platform::SpawnError> pipedError;
```

Run `clang-format -i` on every file touched in this task.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[launcher]"`
Expected: zero warnings; `All tests passed` (on Windows the `sh` case is compiled out; the not-found case runs everywhere).

- [ ] **Step 7: Commit**

```bash
git add src/contour/platform/ExternalLauncher.hpp src/contour/platform/QtExternalLauncher.hpp src/contour/platform/QtExternalLauncher.cpp src/contour/platform/PortalExternalLauncher.hpp src/contour/platform/PortalExternalLauncher.cpp src/contour/test/LauncherFixtures.hpp src/contour/platform/ExternalLauncher_test.cpp
git commit -F - <<'EOF'
platform: pipe input into a launched program

ExternalLauncher::runWithStdin() starts a program and writes bytes to its
standard input -- what opening a command's output in `wl-copy` or a notes
script needs, with no pane. It reports on starting, as runDetached() does;
the Qt launcher keeps each QProcess until it ends, because the child must
outlive the bytes it still has to write.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.13: Private per-process files for a pager (`CommandOutputFiles`)

**Files:**
- Create: `src/contour/session/CommandOutputFiles.hpp`
- Create: `src/contour/session/CommandOutputFiles.cpp`
- Test: create `src/contour/session/CommandOutputFiles_test.cpp`
- Modify: `src/contour/CMakeLists.txt` (`contour_core` headers after `session/SpawnCommand.hpp` :241, sources after `session/SpawnCommand.cpp` :273, `PUBLIC` links after `core::net` :544; `contour_gui_test` sources after `session/TerminalSession_test.cpp` :646)
- Modify: `src/contour/ContourGuiApp.hpp` (constructor :155-160 and its doc comment; accessor after `speechSynthesizer()` :255; member before `_sessionManager` :366), `src/contour/ContourGuiApp.cpp` (anonymous namespace :116-140; constructor :170-185)
- Modify: `src/contour/test/GuiTestFixtures.hpp` (`TestApp` :221-302)

**Interfaces:**
- Consumes: `core::platform::FileSystem`, `core::platform::NativeFileSystem::instance()`, `core::platform::testing::InMemoryFileSystem`/`FileEntry`; `core::Environment`, `core::testing::FakeEnvironment`.
- Produces (`contour::session`):
  ```cpp
  enum class OutputFileError : uint8_t { DirectoryUnavailable = 0, WriteFailed, PermissionsFailed };
  [[nodiscard]] constexpr std::string_view describe(OutputFileError error) noexcept;
  [[nodiscard]] std::filesystem::path commandOutputDirectory(core::Environment const& environment,
                                                             std::filesystem::path const& temporaryDirectory,
                                                             int64_t processId);
  class CommandOutputFiles {
    public:
      CommandOutputFiles(core::platform::FileSystem const& fileSystem, std::filesystem::path directory);
      ~CommandOutputFiles();  // removes the directory it created
      [[nodiscard]] std::expected<std::filesystem::path, OutputFileError> create(std::string_view content);
      void release(std::filesystem::path const& file);
      [[nodiscard]] std::filesystem::path const& directory() const noexcept;
  };
  // ContourGuiApp: 7th constructor parameter std::unique_ptr<session::CommandOutputFiles> commandOutputFiles = nullptr;
  //                [[nodiscard]] session::CommandOutputFiles& commandOutputFiles() noexcept;
  // TestApp: static constexpr std::string_view OutputDirectory; core::platform::testing::InMemoryFileSystem& fileSystem() noexcept;
  ```

- [ ] **Step 1: Write the failing test** — create `src/contour/session/CommandOutputFiles_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandOutputFiles.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <string>

using contour::session::CommandOutputFiles;
using contour::session::OutputFileError;
using core::platform::testing::FileEntry;
using core::platform::testing::InMemoryFileSystem;
namespace fs = std::filesystem;

namespace
{
auto const Directory = fs::path("/run/contour-output-42");
} // namespace

TEST_CASE("CommandOutputFiles: files live in an owner-only directory made on first use",
          "[contour][session][semanticblocks]")
{
    auto fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
    {
        auto files = CommandOutputFiles { fileSystem, Directory };
        CHECK_FALSE(fileSystem.exists(Directory)); // nothing until a file is needed

        auto const first = files.create("hello");
        REQUIRE(first.has_value());
        CHECK(first->parent_path() == Directory);
        CHECK(fileSystem.readFile(*first).value_or("") == "hello");
        CHECK(fileSystem.permissions(Directory).value_or(fs::perms::none) == fs::perms::owner_all);
        CHECK(fileSystem.permissions(*first).value_or(fs::perms::none)
              == (fs::perms::owner_read | fs::perms::owner_write));

        auto const second = files.create("world");
        REQUIRE(second.has_value());
        CHECK(*second != *first);
    }
    CHECK_FALSE(fileSystem.exists(Directory)); // gone with the object, files and all
}

TEST_CASE("CommandOutputFiles: release deletes only the files it handed out", "[contour][session][semanticblocks]")
{
    auto fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
    auto files = CommandOutputFiles { fileSystem, Directory };
    auto const file = files.create("x");
    REQUIRE(file.has_value());

    files.release(*file);
    CHECK_FALSE(fileSystem.exists(*file));

    REQUIRE(fileSystem.writeFile("/run/other.txt", "keep").has_value());
    files.release("/run/other.txt");
    CHECK(fileSystem.exists("/run/other.txt"));
}

TEST_CASE("CommandOutputFiles: a stale directory is replaced, a link is never used",
          "[contour][session][semanticblocks]")
{
    SECTION("a directory an earlier process with this pid left behind")
    {
        auto fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
        fileSystem.addDirectory(Directory);
        REQUIRE(fileSystem.writeFile(Directory / "old.txt", "stale").has_value());

        auto files = CommandOutputFiles { fileSystem, Directory };
        REQUIRE(files.create("new").has_value());
        CHECK_FALSE(fileSystem.exists(Directory / "old.txt"));
    }

    SECTION("a symlink in its place")
    {
        auto fileSystem = InMemoryFileSystem { FileEntry { .path = "/run", .isDirectory = true } };
        fileSystem.addSymlink(Directory, "/run/somewhere-else");

        auto files = CommandOutputFiles { fileSystem, Directory };
        CHECK(files.create("secret") == std::unexpected(OutputFileError::DirectoryUnavailable));
    }

    SECTION("no parent to create it in, or no place at all")
    {
        auto fileSystem = InMemoryFileSystem {};
        CHECK(CommandOutputFiles { fileSystem, Directory }.create("x")
              == std::unexpected(OutputFileError::DirectoryUnavailable));
        CHECK(CommandOutputFiles { fileSystem, fs::path {} }.create("x")
              == std::unexpected(OutputFileError::DirectoryUnavailable));
    }
}

TEST_CASE("commandOutputDirectory: the user's runtime directory first, else the temporary one",
          "[contour][session][semanticblocks]")
{
    using contour::session::commandOutputDirectory;

    auto const runtime = core::testing::FakeEnvironment { { { "XDG_RUNTIME_DIR", "/run/user/1000" } } };
    CHECK(commandOutputDirectory(runtime, "/tmp", 42) == fs::path("/run/user/1000") / "contour-output-42");

    auto const none = core::testing::FakeEnvironment {};
    CHECK(commandOutputDirectory(none, "/tmp", 42) == fs::path("/tmp") / "contour-output-42");
    CHECK(commandOutputDirectory(none, fs::path {}, 42).empty());
}

TEST_CASE("describe(OutputFileError) names every cause", "[contour][session][semanticblocks]")
{
    using contour::session::describe;
    STATIC_CHECK(!describe(OutputFileError::DirectoryUnavailable).empty());
    STATIC_CHECK(!describe(OutputFileError::WriteFailed).empty());
    STATIC_CHECK(!describe(OutputFileError::PermissionsFailed).empty());
}
```

Register it in `src/contour/CMakeLists.txt`'s `contour_gui_test` list, after `            session/TerminalSession_test.cpp` (:646):

```cmake
            session/CommandOutputFiles_test.cpp
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `cannot open include file: 'contour/session/CommandOutputFiles.hpp'`.

- [ ] **Step 3: Write the header** — create `src/contour/session/CommandOutputFiles.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <set>
#include <string_view>

namespace contour::session
{

/// Why a command's output could not be put into a file for a pager.
enum class OutputFileError : uint8_t
{
    DirectoryUnavailable = 0, ///< The private directory could not be created, or a link is in its place.
    WriteFailed,              ///< The file could not be written.
    PermissionsFailed,        ///< The directory or the file could not be made owner-only.
};

/// A short reason for @p error, for logs and user-facing notices.
/// @param error The failure.
/// @return A lower-case phrase without a trailing period.
[[nodiscard]] constexpr std::string_view describe(OutputFileError error) noexcept
{
    switch (error)
    {
        case OutputFileError::DirectoryUnavailable: return "the private output directory is unavailable";
        case OutputFileError::WriteFailed: return "the output could not be written";
        case OutputFileError::PermissionsFailed: return "the output could not be made private";
    }
    return "unknown error";
}

/// Where this process keeps the files it hands to a pager: `contour-output-<pid>` under
/// $XDG_RUNTIME_DIR when that is set (per user, owner-only, cleared at logout), else under
/// @p temporaryDirectory.
/// @param environment Where XDG_RUNTIME_DIR is read.
/// @param temporaryDirectory The system's temporary directory; empty when unknown.
/// @param processId This process's id, which makes the name unique among running instances.
/// @return The directory; empty when there is nowhere to put it.
[[nodiscard]] std::filesystem::path commandOutputDirectory(core::Environment const& environment,
                                                           std::filesystem::path const& temporaryDirectory,
                                                           int64_t processId);

/// The private files a command's output is written to for a pager pane (OpenCommandOutput).
///
/// All of them live in one directory this object creates on first use, readable by the owner only;
/// each file is deleted when the pane showing it closes (release()), and the directory, with
/// anything left in it, when this object goes -- at exit. The filesystem is injected, so tests run
/// over an in-memory one.
class CommandOutputFiles
{
  public:
    /// @param fileSystem Where the files are written. Borrowed: it must outlive this object.
    /// @param directory The private directory (@see commandOutputDirectory); empty disables the files.
    CommandOutputFiles(core::platform::FileSystem const& fileSystem, std::filesystem::path directory);
    CommandOutputFiles(CommandOutputFiles const&) = delete;
    CommandOutputFiles& operator=(CommandOutputFiles const&) = delete;
    CommandOutputFiles(CommandOutputFiles&&) = delete;
    CommandOutputFiles& operator=(CommandOutputFiles&&) = delete;
    ~CommandOutputFiles();

    /// Writes @p content to a new file in the private directory, creating the directory first if
    /// needed. A directory left by an earlier process with the same id (a crash skips the cleanup) is
    /// removed and made afresh; a link in its place is never followed.
    /// @param content The bytes to write.
    /// @return The file's path, or why there is none.
    [[nodiscard]] std::expected<std::filesystem::path, OutputFileError> create(std::string_view content);

    /// Deletes @p file, if create() handed it out; any other path is left alone.
    /// @param file A path create() returned.
    void release(std::filesystem::path const& file);

    /// The private directory.
    [[nodiscard]] std::filesystem::path const& directory() const noexcept { return _directory; }

  private:
    /// Whether the private directory has been made by this object.
    enum class DirectoryState : uint8_t
    {
        Absent = 0,
        Created,
    };

    /// Creates the private directory, owner-only, unless this object already has.
    [[nodiscard]] std::expected<void, OutputFileError> ensureDirectory();

    core::platform::FileSystem const& _fileSystem;
    std::filesystem::path _directory;
    DirectoryState _directoryState = DirectoryState::Absent;
    uint64_t _filesCreated = 0;
    std::set<std::filesystem::path> _files;
};

} // namespace contour::session
```

- [ ] **Step 4: Write the implementation** — create `src/contour/session/CommandOutputFiles.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <contour/session/CommandOutputFiles.hpp>

#include <core/log/LogStore.hpp>

#include <format>
#include <utility>

namespace contour::session
{

namespace fs = std::filesystem;

fs::path commandOutputDirectory(core::Environment const& environment,
                                fs::path const& temporaryDirectory,
                                int64_t processId)
{
    auto const leaf = std::format("contour-output-{}", processId);
    if (auto const runtime = environment.get("XDG_RUNTIME_DIR"); runtime && !runtime->empty())
        return fs::path(*runtime) / leaf;
    if (temporaryDirectory.empty())
        return {};
    return temporaryDirectory / leaf;
}

CommandOutputFiles::CommandOutputFiles(core::platform::FileSystem const& fileSystem, fs::path directory):
    _fileSystem { fileSystem }, _directory { std::move(directory) }
{
}

CommandOutputFiles::~CommandOutputFiles()
{
    if (_directoryState != DirectoryState::Created)
        return;
    if (auto const removed = _fileSystem.removeAll(_directory); !removed)
        errorLog()("Could not remove the command output directory: {}", removed.error());
}

std::expected<void, OutputFileError> CommandOutputFiles::ensureDirectory()
{
    if (_directoryState == DirectoryState::Created)
        return {};
    if (_directory.empty())
        return std::unexpected(OutputFileError::DirectoryUnavailable);

    // Never follow or reuse a link: whoever placed it would be handed the output.
    if (_fileSystem.isSymlink(_directory))
        return std::unexpected(OutputFileError::DirectoryUnavailable);

    // A directory one cannot remove is somebody else's, and is not used.
    if (_fileSystem.exists(_directory))
        if (auto const removed = _fileSystem.removeAll(_directory); !removed)
            return std::unexpected(OutputFileError::DirectoryUnavailable);

    if (auto const created = _fileSystem.createDirectory(_directory); !created)
        return std::unexpected(OutputFileError::DirectoryUnavailable);

    if (auto const restricted = _fileSystem.setPermissions(_directory, fs::perms::owner_all); !restricted)
    {
        if (auto const removed = _fileSystem.removeAll(_directory); !removed)
            errorLog()("Could not remove the command output directory: {}", removed.error());
        return std::unexpected(OutputFileError::PermissionsFailed);
    }

    _directoryState = DirectoryState::Created;
    return {};
}

std::expected<fs::path, OutputFileError> CommandOutputFiles::create(std::string_view content)
{
    if (auto const ready = ensureDirectory(); !ready)
        return std::unexpected(ready.error());

    // The directory admits only its owner, so the moment between writing the file and narrowing its
    // own permissions exposes nothing.
    auto const file = _directory / std::format("output-{}.txt", ++_filesCreated);
    if (auto const written = _fileSystem.writeFile(file, content); !written)
        return std::unexpected(OutputFileError::WriteFailed);

    if (auto const restricted = _fileSystem.setPermissions(file, fs::perms::owner_read | fs::perms::owner_write);
        !restricted)
    {
        if (auto const removed = _fileSystem.remove(file); !removed)
            errorLog()("Could not remove a command output file: {}", removed.error());
        return std::unexpected(OutputFileError::PermissionsFailed);
    }

    _files.insert(file);
    return file;
}

void CommandOutputFiles::release(fs::path const& file)
{
    if (_files.erase(file) == 0)
        return;
    if (auto const removed = _fileSystem.remove(file); !removed)
        errorLog()("Could not remove a command output file: {}", removed.error());
}

} // namespace contour::session
```

In `src/contour/CMakeLists.txt`: after `        session/SpawnCommand.hpp` (:241) add `        session/CommandOutputFiles.hpp`; after `        session/SpawnCommand.cpp` (:273) add `        session/CommandOutputFiles.cpp`; in `contour_core`'s `PUBLIC` links, after `    core::net` (:544) add `    core::platform`.

- [ ] **Step 5: Run the unit tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[session][semanticblocks]"`
Expected: zero warnings; `All tests passed` (the five new `CommandOutputFiles`/`commandOutputDirectory`/`describe` cases among them).

- [ ] **Step 6: Give the app one** — in `src/contour/ContourGuiApp.hpp`, add `#include <contour/session/CommandOutputFiles.hpp>`; extend the constructor's doc comment with

```cpp
    /// @param commandOutputFiles Where OpenCommandOutput writes the files a pager pane shows. Null (the
    ///                       default) wires the native filesystem under $XDG_RUNTIME_DIR or the system
    ///                       temporary directory; tests pass one over an in-memory filesystem.
```

and replace the declaration's last parameter line `                           std::unique_ptr<platform::SpeechSynthesizer> speechSynthesizer = nullptr);` with

```cpp
                           std::unique_ptr<platform::SpeechSynthesizer> speechSynthesizer = nullptr,
                           std::unique_ptr<session::CommandOutputFiles> commandOutputFiles = nullptr);
```

After the `speechSynthesizer()` accessor (:255) insert:

```cpp

    /// The private files OpenCommandOutput hands to pager panes; one per app, removed at exit.
    [[nodiscard]] session::CommandOutputFiles& commandOutputFiles() noexcept { return *_commandOutputFiles; }
```

Before `    session::TerminalSessionManager _sessionManager;` (:366) insert:

```cpp
    // Declared before _sessionManager: a pager pane's session releases its file here when it closes,
    // which during teardown happens while the manager is being destroyed.
    std::unique_ptr<session::CommandOutputFiles> _commandOutputFiles;
```

In `src/contour/ContourGuiApp.cpp`, add `#include <core/platform/NativeFileSystem.hpp>` and `#include <QtCore/QCoreApplication>` (if absent) to the includes; inside the anonymous namespace (:116-140), after `namesAnExecutableFile`, insert:

```cpp

    /// The production CommandOutputFiles: the native filesystem, in a directory named after this process.
    [[nodiscard]] std::unique_ptr<session::CommandOutputFiles> makeCommandOutputFiles(core::Environment const& env)
    {
        auto ec = std::error_code {};
        auto temporary = fs::temp_directory_path(ec);
        if (ec)
            temporary.clear();
        return std::make_unique<session::CommandOutputFiles>(
            core::platform::NativeFileSystem::instance(),
            session::commandOutputDirectory(env, temporary, QCoreApplication::applicationPid()));
    }
```

In the constructor definition replace `                             std::unique_ptr<platform::SpeechSynthesizer> speechSynthesizer):` with

```cpp
                             std::unique_ptr<platform::SpeechSynthesizer> speechSynthesizer,
                             std::unique_ptr<session::CommandOutputFiles> commandOutputFiles):
```

and after the `_speechSynthesizer(...)` initializer insert:

```cpp
    _commandOutputFiles(commandOutputFiles ? std::move(commandOutputFiles) : makeCommandOutputFiles(env)),
```

- [ ] **Step 7: Give the test app an in-memory one** — in `src/contour/test/GuiTestFixtures.hpp`, add `#include <contour/session/CommandOutputFiles.hpp>` and `#include <core/platform/testing/InMemoryFileSystem.hpp>`. In `TestApp`, in both constructors' `_app(...)` initializer, append the argument `makeOutputFiles()` (the first constructor: after `speech ? std::move(speech) : defaultSpeech()`; the second: after `defaultSpeech()`). In the public section, after `launcher()`, add:

```cpp

    /// Where OpenCommandOutput's files go in a test app: a directory on the in-memory filesystem.
    static constexpr std::string_view OutputDirectory = "/run/contour-test-output";

    /// The in-memory filesystem the app's command output files are written to.
    [[nodiscard]] core::platform::testing::InMemoryFileSystem& fileSystem() noexcept { return _fileSystem; }
```

In the private section, after `defaultSpeech()`, add:

```cpp

    /// The app's command output files, on the fixture's in-memory filesystem.
    std::unique_ptr<contour::session::CommandOutputFiles> makeOutputFiles()
    {
        return std::make_unique<contour::session::CommandOutputFiles>(_fileSystem,
                                                                      std::filesystem::path(OutputDirectory));
    }
```

and before `    contour::ContourGuiApp _app;` add:

```cpp
    /// Declared before _app, which borrows it for its command output files.
    core::platform::testing::InMemoryFileSystem _fileSystem {
        core::platform::testing::FileEntry { .path = "/run", .isDirectory = true }
    };
```

Run `clang-format -i` on every file touched in this task.

- [ ] **Step 8: Run the GUI suite to verify nothing else moved**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` (it compiles `ContourGuiApp.cpp` through `contour_core`) then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe`
Expected: zero warnings; `All tests passed` (every `TestApp` now builds the app with the in-memory files; nothing writes to them yet).

- [ ] **Step 9: Commit**

```bash
git add src/contour/session/CommandOutputFiles.hpp src/contour/session/CommandOutputFiles.cpp src/contour/session/CommandOutputFiles_test.cpp src/contour/CMakeLists.txt src/contour/ContourGuiApp.hpp src/contour/ContourGuiApp.cpp src/contour/test/GuiTestFixtures.hpp
git commit -F - <<'EOF'
session: keep command output in private per-process files

A pager pane is a program reading a file, so OpenCommandOutput needs one:
CommandOutputFiles writes it into a directory of this process's own --
under $XDG_RUNTIME_DIR when set, else the temporary directory -- created
owner-only on first use, never through a link, and removed with everything
in it at exit. Each file is owner read/write and deleted when released.

The filesystem is injected: the app passes the native one, the test app an
in-memory one, so no test writes to the real disk.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.14: `command_blocks.pager` (C9)

**Files:**
- Modify: `src/contour/config/Config.hpp` (`struct CommandBlocksConfig`, added by phase 1; its `Writer::format` overload)
- Modify: `src/contour/config/Config.cpp` (phase 1's `YAMLConfigReader::loadFromEntry(..., CommandBlocksConfig& where)`)
- Modify: `src/contour/config/ConfigDocumentation.hpp` (phase 1's `command_blocks:` template and its web text)
- Test: `src/contour/config/Config_test.cpp`

**Interfaces:**
- Consumes: C9 `CommandBlocksConfig { size_t maxRecords = 1000; }` behind `Config::commandBlocks` (global key `command_blocks`).
- Produces (C9):
  ```cpp
  [[nodiscard]] constexpr std::string_view defaultCommandOutputPager() noexcept; // "less -R"; "more" on Windows
  struct CommandBlocksConfig { size_t maxRecords = 1000; std::string pager { defaultCommandOutputPager() }; };
  ```
  YAML: `command_blocks: { max_records: 1000, pager: 'less -R' }`.

- [ ] **Step 1: Locate phase 1's pieces** — run `git grep -n "CommandBlocksConfig" src/contour/config` and `git grep -n "max_records" src/contour/config`. Expected: the struct in `Config.hpp`, a `loadFromEntry` overload in `Config.cpp` reading `"max_records"`, a `Writer::format(std::string_view doc, CommandBlocksConfig const& v)` in `Config.hpp`, and a `max_records: {}` template line plus a `==max_records==` web paragraph in `ConfigDocumentation.hpp`. Every hunk below is anchored on one of these lines.

- [ ] **Step 2: Write the failing test** — append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: command_blocks.pager", "[config][semanticblocks]")
{
    QTemporaryDir dir;

    SECTION("defaults to the platform's pager")
    {
        auto const config = contour::config::Config {};
#if defined(_WIN32)
        CHECK(config.commandBlocks.value().pager == "more");
#else
        CHECK(config.commandBlocks.value().pager == "less -R");
#endif
        CHECK(config.commandBlocks.value().pager == contour::config::defaultCommandOutputPager());
    }

    SECTION("is read from command_blocks")
    {
        auto const config = loadFromYaml(dir,
                                         "command_blocks:\n"
                                         "    pager: 'bat --paging=always'\n"
                                         "profiles:\n  main:\n    shell: \"/bin/bash\"\n");
        CHECK(config.commandBlocks.value().pager == "bat --paging=always");
    }

    SECTION("round-trips through the generated config")
    {
        auto config = contour::config::Config {};
        auto blocks = config.commandBlocks.value();
        blocks.pager = "most -s";
        config.commandBlocks = blocks;

        auto const written = contour::config::createString<contour::config::YAMLConfigWriter>(config);
        REQUIRE(written.contains("pager: 'most -s'"));
        CHECK(loadFromYaml(dir, written).commandBlocks.value().pager == "most -s");
    }
}
```

- [ ] **Step 3: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `'pager': is not a member of 'contour::config::CommandBlocksConfig'`.

- [ ] **Step 4: Add the field** — in `src/contour/config/Config.hpp`, immediately above `struct CommandBlocksConfig` insert:

```cpp
/// The pager OpenCommandOutput runs when its binding names no program: `less -R`, which keeps the
/// colours an `sgr` capture carries, wherever there is one; `more` on Windows, where there is not.
/// @return The default of `command_blocks.pager`.
[[nodiscard]] constexpr std::string_view defaultCommandOutputPager() noexcept
{
#if defined(_WIN32)
    return "more";
#else
    return "less -R";
#endif
}

```

and inside the struct, after the `maxRecords` member, insert:

```cpp

    /// The program OpenCommandOutput opens a block's output with when its binding names none. Split
    /// like a shell command line (`less -R`); the output file is appended as its last argument.
    std::string pager { defaultCommandOutputPager() };
```

If phase 1 declared `bool operator==(CommandBlocksConfig const&) const = default;`, it covers the new member unchanged.

In `src/contour/config/Config.cpp`, in `loadFromEntry(..., CommandBlocksConfig& where)`, directly after the statement that reads `"max_records"` add, using the same child-node variable that statement uses:

```cpp
        loadFromEntry(child, "pager", where.pager);
```

In `src/contour/config/Config.hpp`, in `Writer::format(std::string_view doc, CommandBlocksConfig const& v)`, change `format(doc, v.maxRecords)` to:

```cpp
        return format(doc, v.maxRecords, v.pager);
```

In `src/contour/config/ConfigDocumentation.hpp`, in the `command_blocks:` template, directly after the line ending in `max_records: {}\n"` insert:

```cpp
    "    {comment} The program \"Open Output in Pager\" (the OpenCommandOutput action) runs when its\n"
    "    {comment} binding names none. The file holding the output is appended as its last argument.\n"
    "    pager: '{}'\n"
```

and in the matching web text, after the `==max_records==` paragraph, insert:

```cpp
    ":octicons-horizontal-rule-16: ==pager== The program the OpenCommandOutput action (\"Open Output in "
    "Pager\", Ctrl+Shift+G) opens a command's output with when its binding names none, split like a shell "
    "command line. The output is written to a private file that is appended as the last argument and "
    "deleted when the pane closes. Defaults to `less -R` (`more` on Windows). <br/>\n"
```

plus `"  pager: less -R\n"` after the `max_records:` line of that text's yaml example. Run `clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: zero warnings; `All tests passed` — including "the generated default config loads back into the defaults" (its error log must stay empty with the new key in it).

- [ ] **Step 6: Commit**

```bash
git add src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
config: add command_blocks.pager

The program OpenCommandOutput opens a command's output with when its
binding names none: `less -R` -- which keeps the colours of an SGR
capture -- or `more` on Windows. Written single-quoted, so a pager with a
`#` or a `:` in its arguments survives a config save.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.15: Open a command in a new split or tab (`TerminalSessionManager`)

**Files:**
- Modify: `src/contour/session/TerminalSessionManager.hpp` (before `class TerminalSessionManager`; `createSessionInBackground` :120-124; after `splitActivePane` :338; private section after :671)
- Modify: `src/contour/session/TerminalSessionManager.cpp` (`createSessionInBackground` :156-207; `splitActivePane` :1423-1494)
- Test: `src/contour/session/TerminalSessionManager_test.cpp`

**Interfaces:**
- Consumes: `createBackingSession(..., commandOverride, ...)` (:209); `paneActionTargetTab`, `windowHostingSession`; `vtworkspace::SplitState`.
- Produces:
  ```cpp
  enum class CommandPanePlacement : uint8_t { Split = 0, Tab };
  [[nodiscard]] TerminalSession* openCommandPane(TerminalSession* acting, CommandPanePlacement placement,
                                                 vtpty::Process::ExecInfo const& command);
  TerminalSession* createSessionInBackground(vtworkspace::WindowId window,
                                             std::optional<std::string> const& profileName = std::nullopt,
                                             std::optional<vtpty::Process::ExecInfo> const& command = std::nullopt);
  // private
  TerminalSession* splitPane(TerminalSession* acting, vtworkspace::SplitState direction, double ratio,
                             std::optional<vtpty::Process::ExecInfo> const& command);
  ```

- [ ] **Step 1: Write the failing test** — append to `src/contour/session/TerminalSessionManager_test.cpp`:

```cpp
TEST_CASE("TerminalSessionManager: openCommandPane runs a command beside the acting pane or in a new tab",
          "[manager][semanticblocks]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    contour::test::TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    REQUIRE(win->count() == 1);

    auto const pager = vtpty::Process::ExecInfo { .program = "less", .arguments = { "-R", "/run/out.txt" } };

    SECTION("a split joins the acting pane's tab")
    {
        auto* pane = app.manager().openCommandPane(acting, contour::session::CommandPanePlacement::Split, pager);
        REQUIRE(pane != nullptr);
        CHECK(pane != acting);
        CHECK(win->count() == 1);
    }

    SECTION("a tab is a tab of its own")
    {
        auto* pane = app.manager().openCommandPane(acting, contour::session::CommandPanePlacement::Tab, pager);
        REQUIRE(pane != nullptr);
        CHECK(win->count() == 2);
    }

    REQUIRE(factory->requestedCommandOverrides.back().has_value());
    CHECK(factory->requestedCommandOverrides.back()->program == "less");
    CHECK(factory->requestedCommandOverrides.back()->arguments == std::vector<std::string> { "-R", "/run/out.txt" });
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `'openCommandPane': is not a member of 'contour::session::TerminalSessionManager'`.

- [ ] **Step 3: Declare it** — in `src/contour/session/TerminalSessionManager.hpp`, after `enum class SessionEnd` (:50-60) insert:

```cpp

/// Where openCommandPane() puts the pane running the command.
enum class CommandPanePlacement : uint8_t
{
    Split = 0, ///< A new pane stacked below the acting one, in its tab.
    Tab,       ///< A new tab in the acting pane's window.
};
```

Replace the `createSessionInBackground` declaration (:120-124) with:

```cpp
    /// Creates a backing session + its model tab in @p window (the calling controller's window).
    /// @param window      The target window.
    /// @param profileName Profile to launch the session with, or std::nullopt for the app default.
    /// @param command     A command to run instead of the profile's shell, or std::nullopt.
    TerminalSession* createSessionInBackground(vtworkspace::WindowId window,
                                               std::optional<std::string> const& profileName = std::nullopt,
                                               std::optional<vtpty::Process::ExecInfo> const& command = std::nullopt);
```

After the `splitActivePane` declaration (:338) insert:

```cpp

    /// Opens a pane running @p command next to @p acting: a split in its tab, or a tab in its window
    /// (OpenCommandOutput's pager). The pane is a session like any other; it ends when the command does.
    /// @param acting The session the action came from.
    /// @param placement Split or tab.
    /// @param command The program and arguments to run instead of a shell.
    /// @return The new pane's session, or nullptr when none could be made (no session can be created
    ///         right now -- attach mode -- or @p acting is in no tab).
    [[nodiscard]] TerminalSession* openCommandPane(TerminalSession* acting,
                                                   CommandPanePlacement placement,
                                                   vtpty::Process::ExecInfo const& command);
```

After `  private:` (:671) insert:

```cpp
    /// Splits the acting session's tab's active pane, running @p command in the new leaf (a shell when
    /// none). The local half of splitActivePane(), shared with openCommandPane().
    /// @return The new leaf's session, or nullptr when no split happened.
    TerminalSession* splitPane(TerminalSession* acting,
                               vtworkspace::SplitState direction,
                               double ratio,
                               std::optional<vtpty::Process::ExecInfo> const& command);

```

- [ ] **Step 4: Implement it** — in `src/contour/session/TerminalSessionManager.cpp`, change `createSessionInBackground`'s definition head (:156-157) to

```cpp
TerminalSession* TerminalSessionManager::createSessionInBackground(
    vtworkspace::WindowId window,
    std::optional<std::string> const& profileName,
    std::optional<vtpty::Process::ExecInfo> const& command)
```

and its `createBackingSession` call (:200) to

```cpp
    auto* session = createBackingSession(sessionId, ptyPath, pageSize, command, profileName);
```

Replace `splitActivePane` (:1423-1494) with:

```cpp
void TerminalSessionManager::splitActivePane(bool vertical, TerminalSession* acting, double ratio)
{
    // Attach mode: author the split on the daemon (B3-Qt); its layout re-push
    // reconciles the new pane in. A reconciliation-driven split (during realization)
    // returns false and builds locally below, as does a local factory.
    if (acting != nullptr && _sessionFactory.requestRemoteSplit(&acting->terminal().device(), vertical))
        return;

    auto const direction = vertical ? vtworkspace::SplitState::Vertical : vtworkspace::SplitState::Horizontal;
    splitPane(acting, direction, ratio, std::nullopt);
}

TerminalSession* TerminalSessionManager::splitPane(TerminalSession* acting,
                                                   vtworkspace::SplitState direction,
                                                   double ratio,
                                                   std::optional<vtpty::Process::ExecInfo> const& command)
{
    if (!_sessionFactory.canCreateSession())
    {
        managerLog()("Refusing to split: the session factory cannot back a new session right now.");
        return nullptr;
    }

    auto* tab = paneActionTargetTab(acting);
    if (tab == nullptr)
        return nullptr;
```

followed by the original body from the comment `// Split beside the pane the keybinding actually fired from.` through `auto const newSessionId = vtworkspace::SessionId { _nextSessionId++ };`, unchanged, and then:

```cpp
    auto* session = createBackingSession(newSessionId, std::move(cwd), pageSize, command);
    if (session == nullptr)
        return nullptr; // no backing session, no split

    auto* newLeaf = _model->splitActivePane(tab->id(), direction, ratio);
    _pendingSessionId.reset(); // consumed by the allocator; clear any leftover
    if (newLeaf == nullptr)
    {
        // The split did not happen; the backing session we created has no model pane, so terminate
        // it to drop the orphaned session/registry entries rather than leak them. In attach mode it
        // is a real daemon session too, so end it upstream — nothing will ever show it.
        if (auto* orphan = sessionForId(newSessionId))
        {
            authorRemoteEnd(orphan);
            orphan->terminate();
        }
        return nullptr;
    }

    // The new leaf joins the same tab as the pane it split from.
    _tabBySession[newSessionId.value] = tab->id();
    return session;
}

TerminalSession* TerminalSessionManager::openCommandPane(TerminalSession* acting,
                                                         CommandPanePlacement placement,
                                                         vtpty::Process::ExecInfo const& command)
{
    switch (placement)
    {
        case CommandPanePlacement::Split:
            // Stacked below the pane it came from: output reads top to bottom, and keeps the full width.
            return splitPane(acting, vtworkspace::SplitState::Horizontal, 0.5, command);
        case CommandPanePlacement::Tab:
            if (auto* window = windowHostingSession(acting))
                return createSessionInBackground(window->id(), std::nullopt, command);
            return nullptr;
    }
    return nullptr;
}
```

(The old `auto const direction = vertical ? ... ;` line inside the body is gone: `direction` is now the parameter.) Run `clang-format -i src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe`
Expected: zero warnings; `All tests passed` — the whole GUI suite, because every keyboard split now goes through `splitPane()`.

- [ ] **Step 6: Commit**

```bash
git add src/contour/session/TerminalSessionManager.hpp src/contour/session/TerminalSessionManager.cpp src/contour/session/TerminalSessionManager_test.cpp
git commit -F - <<'EOF'
session: open a command in a new split or tab

openCommandPane() runs a given command in a new pane beside the acting
one -- stacked below it in its tab, or in a tab of its own -- through
createBackingSession()'s existing command override. The local half of
splitActivePane() becomes splitPane(), so the keyboard split and the pager
split are one code path.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.16: The `OpenCommandOutput` action

**Files:**
- Modify: `src/contour/config/BlockActionEnums.hpp` (Tasks 5.3, 5.9)
- Modify: `src/contour/config/Actions.hpp` (struct, variant after `ClearToPrompt`, `NonRepeatableActionConcept` :280-301, documentation, catalog, `formatActionArguments`)
- Modify: `src/contour/config/Config.cpp` (`parseAction`, after the `CopyCommandLine` branch)
- Modify: `src/contour/command/Command.cpp` (`commandArguments`, after the `CopyCommandLine` lambda)
- Modify: `src/contour/session/TerminalSession.hpp`, `src/contour/session/TerminalSession.cpp`
- Test: `src/contour/config/Actions_test.cpp`, `src/contour/command/CommandCatalog_test.cpp`, `src/contour/config/Config_test.cpp`, `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: Task 5.2 `Terminal::commandBlockText`; Task 5.12 `ExternalLauncher::runWithStdin`; Task 5.13 `ContourGuiApp::commandOutputFiles()`, `CommandOutputFiles::create/release`, `describe(OutputFileError)`; Task 5.14 `CommandBlocksConfig::pager`; Task 5.15 `TerminalSessionManager::openCommandPane`, `CommandPanePlacement`; `config::shellSplit` (`Config.hpp:2396`); `TerminalSession::showNotification` signal (:932).
- Produces (C5):
  ```cpp
  namespace contour::actions {
  enum class OutputPlacement : uint8_t { Split = 0, Tab, Detached };
  enum class OutputFormat : uint8_t { Sgr = 0, Plain };
  struct OpenCommandOutput { vtbackend::CommandBlockTarget target = Last; std::string program; OutputPlacement placement = Split;
                             OutputFormat format = Sgr; vtbackend::CommandBlockId block {}; };
  }
  // TerminalSession (private): void reportOpenOutputFailure(std::string_view reason);
  ```
  YAML: `action: OpenCommandOutput, target: last, program: 'less -R', placement: split|tab|detached, format: sgr|plain, block: N` (all optional).

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/config/Actions_test.cpp`:

```cpp
TEST_CASE("actions: OpenCommandOutput writes its arguments and is not repeated", "[actions][semanticblocks]")
{
    CHECK(std::format("{}", actions::Action { actions::OpenCommandOutput {} })
          == "OpenCommandOutput, target: last, placement: split, format: sgr");
    CHECK(std::format("{}",
                      actions::Action { actions::OpenCommandOutput {
                          .target = vtbackend::CommandBlockTarget::Pointer,
                          .program = "wl-copy",
                          .placement = actions::OutputPlacement::Detached,
                          .format = actions::OutputFormat::Plain,
                          .block = vtbackend::CommandBlockId(9) } })
          == "OpenCommandOutput, target: pointer, program: 'wl-copy', placement: detached, format: plain, block: 9");

    // Every fire forks a pager pane, as a split does: a held key must not open a burst of them.
    CHECK(actions::isNonRepeatable(actions::Action { actions::OpenCommandOutput {} }));
}
```

Append to `src/contour/command/CommandCatalog_test.cpp`:

```cpp
TEST_CASE("OpenCommandOutput's arguments are part of its identity", "[contour][palette][semanticblocks]")
{
    using contour::command::commandId;
    namespace actions = contour::actions;

    CHECK(commandId(actions::OpenCommandOutput {}) == "OpenCommandOutput");
    CHECK(commandId(actions::OpenCommandOutput { .placement = actions::OutputPlacement::Tab })
          == "OpenCommandOutput:last:tab:sgr::0");
    CHECK(commandId(actions::OpenCommandOutput { .program = "wl-copy", .placement = actions::OutputPlacement::Detached })
          == "OpenCommandOutput:last:detached:sgr:wl-copy:0");
}
```

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: OpenCommandOutput reads its arguments", "[config][semanticblocks]")
{
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], key: 'F1', action: OpenCommandOutput }
    - { mods: [Control], key: 'F2', action: OpenCommandOutput, program: 'wl-copy', placement: detached, format: plain }
    - { mods: [Control], key: 'F3', action: OpenCommandOutput, placement: window }
)"sv);

    auto const& keys = config.inputMappings.value().keyMappings;
    auto const bound = [&](vtbackend::Key key) -> std::optional<contour::actions::OpenCommandOutput> {
        auto const it = std::ranges::find_if(keys, [key](auto const& mapping) { return mapping.input == key; });
        if (it == keys.end() || it->binding.empty())
            return std::nullopt;
        return std::get<contour::actions::OpenCommandOutput>(it->binding.at(0));
    };

    auto const plain = bound(vtbackend::Key::F1);
    REQUIRE(plain.has_value());
    CHECK(plain->target == vtbackend::CommandBlockTarget::Last);
    CHECK(plain->program.empty());
    CHECK(plain->placement == contour::actions::OutputPlacement::Split);
    CHECK(plain->format == contour::actions::OutputFormat::Sgr);

    auto const piped = bound(vtbackend::Key::F2);
    REQUIRE(piped.has_value());
    CHECK(piped->program == "wl-copy");
    CHECK(piped->placement == contour::actions::OutputPlacement::Detached);
    CHECK(piped->format == contour::actions::OutputFormat::Plain);

    CHECK_FALSE(bound(vtbackend::Key::F3).has_value()); // no such placement
}
```

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: OpenCommandOutput shows a command's output in a pager",
          "[contour][session][actions][semanticblocks]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    // Before the session exists: a session takes its own copy of the configuration when it is made.
    app.app().config().commandBlocks.value().pager = "less -R";

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    acting->terminal().writeToScreen("\033]133;A\033\\$ make\r\n\033]133;C\033\\\033[32mbuilt\033[m\r\n");
    acting->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

    auto& fileSystem = app.fileSystem();
    auto const directory = std::filesystem::path(TestApp::OutputDirectory);

    SECTION("in a split running the pager over a private file, which goes with the pane")
    {
        REQUIRE((*acting)(contour::actions::OpenCommandOutput {}));
        CHECK(win->count() == 1);

        auto const& command = factory->requestedCommandOverrides.back();
        REQUIRE(command.has_value());
        CHECK(command->program == "less");
        REQUIRE(command->arguments.size() == 2);
        CHECK(command->arguments[0] == "-R");

        auto const file = std::filesystem::path(command->arguments[1]);
        CHECK(file.parent_path() == directory);
        auto const content = fileSystem.readFile(file).value_or("");
        CHECK(content.contains("built"));
        CHECK(content.contains("\033[")); // format: sgr is the default
        CHECK(fileSystem.permissions(file).value_or(std::filesystem::perms::none)
              == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
        CHECK(fileSystem.permissions(directory).value_or(std::filesystem::perms::none)
              == std::filesystem::perms::owner_all);

        auto* pager = app.manager().sessionForId(vtworkspace::SessionId { acting->modelSessionId().value + 1 });
        REQUIRE(pager != nullptr);
        emit pager->sessionClosed(*pager);
        CHECK_FALSE(fileSystem.exists(file));
    }

    SECTION("in a tab, as plain text")
    {
        REQUIRE((*acting)(contour::actions::OpenCommandOutput { .placement = contour::actions::OutputPlacement::Tab,
                                                                 .format = contour::actions::OutputFormat::Plain }));
        CHECK(win->count() == 2);

        auto const& command = factory->requestedCommandOverrides.back();
        REQUIRE(command.has_value());
        CHECK(fileSystem.readFile(command->arguments.back()).value_or("") == "built");
    }

    SECTION("detached: piped into the program as plain text, with no pane and no file")
    {
        auto const panesBefore = factory->requestedCommandOverrides.size();
        REQUIRE((*acting)(contour::actions::OpenCommandOutput {
            .program = "wl-copy --type text/plain", .placement = contour::actions::OutputPlacement::Detached }));

        REQUIRE(app.launcher().piped.size() == 1);
        CHECK(app.launcher().piped[0].program == QStringLiteral("wl-copy"));
        CHECK(app.launcher().piped[0].arguments == QStringList { QStringLiteral("--type"), QStringLiteral("text/plain") });
        CHECK(app.launcher().piped[0].input == QByteArrayLiteral("built"));
        CHECK(factory->requestedCommandOverrides.size() == panesBefore);
        CHECK_FALSE(fileSystem.exists(directory));
    }

    SECTION("a program that cannot be started is reported")
    {
        auto notices = 0;
        QObject::connect(acting, &contour::session::TerminalSession::showNotification, [&] { ++notices; });
        app.launcher().pipedError = contour::platform::SpawnError::NotFound;

        CHECK_FALSE((*acting)(contour::actions::OpenCommandOutput { .program = "nope",
                                                                     .placement = contour::actions::OutputPlacement::Detached }));
        CHECK(notices == 1);
    }

    SECTION("nothing to show opens nothing")
    {
        auto const panesBefore = factory->requestedCommandOverrides.size();
        CHECK_FALSE((*acting)(contour::actions::OpenCommandOutput { .target = vtbackend::CommandBlockTarget::Cursor }));
        CHECK(factory->requestedCommandOverrides.size() == panesBefore);
    }
}
```

Add `#include <contour/platform/ExternalLauncher.hpp>` to that file's includes if absent.

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'OpenCommandOutput': is not a member of 'contour::actions'`.

- [ ] **Step 3: Add the enums and their tokens** — in `src/contour/config/BlockActionEnums.hpp`, inside `namespace contour::actions` after `CopyFallback`, insert:

```cpp

/// Where OpenCommandOutput shows a command's output.
enum class OutputPlacement : uint8_t
{
    Split = 0, ///< A new pane stacked below, running the pager over a private file.
    Tab,       ///< A new tab, likewise.
    Detached,  ///< No pane: the output is piped as plain text into the program's standard input.
};

/// Whether the file OpenCommandOutput writes keeps the output's colours.
enum class OutputFormat : uint8_t
{
    Sgr = 0, ///< With SGR sequences, for a pager that renders them (`less -R`).
    Plain,   ///< Text only.
};
```

inside `namespace detail`:

```cpp

    /// Where OpenCommandOutput shows the output: `placement:` in input_mapping.
    inline constexpr auto OutputPlacementTable = std::array {
        ConfigEnumInfo<actions::OutputPlacement> { actions::OutputPlacement::Split, "split", "In a split pane" },
        ConfigEnumInfo<actions::OutputPlacement> { actions::OutputPlacement::Tab, "tab", "In a new tab" },
        ConfigEnumInfo<actions::OutputPlacement> {
            actions::OutputPlacement::Detached, "detached", "Piped into a program" },
    };

    /// Whether OpenCommandOutput keeps the colours: `format:` in input_mapping.
    inline constexpr auto OutputFormatTable = std::array {
        ConfigEnumInfo<actions::OutputFormat> { actions::OutputFormat::Sgr, "sgr", "With colours" },
        ConfigEnumInfo<actions::OutputFormat> { actions::OutputFormat::Plain, "plain", "Plain text" },
    };
```

and after the last specialization:

```cpp

template <>
constexpr std::span<ConfigEnumInfo<actions::OutputPlacement> const> configEnumValues() noexcept
{
    return detail::OutputPlacementTable;
}

template <>
constexpr std::span<ConfigEnumInfo<actions::OutputFormat> const> configEnumValues() noexcept
{
    return detail::OutputFormatTable;
}
```

- [ ] **Step 4: Add the action** — in `src/contour/config/Actions.hpp`: after the `ClearToPrompt` struct insert

```cpp
// OSC 133: show a block's output in a pager (a split or a tab over a private file), or pipe it into a program
struct OpenCommandOutput{ vtbackend::CommandBlockTarget target = vtbackend::CommandBlockTarget::Last; std::string program; OutputPlacement placement = OutputPlacement::Split; OutputFormat format = OutputFormat::Sgr; vtbackend::CommandBlockId block {}; };
```

append `OpenCommandOutput` to `Action` after `ClearToPrompt`; in `NonRepeatableActionConcept` replace `                                                 SaveLayout>;` with

```cpp
                                                 SaveLayout,
                                                 OpenCommandOutput>;
```

and add `/// OpenCommandOutput joins them for the reason the splits do: each fire forks a pager pane.` as the last line of the concept's doc comment. Documentation:

```cpp
    constexpr inline std::string_view OpenCommandOutput {
        "Opens a command's output in a pager (target: pointer, cursor or last; program: the pager, "
        "command_blocks.pager when empty; placement: split, tab or detached; format: sgr or plain). "
        "Detached pipes plain text into the program's standard input instead of opening a pane. Requires "
        "a shell that emits OSC 133 marks."
    };
```

catalog row after `ClearToPrompt`'s:

```cpp
        ActionCatalogEntry {
            "OpenCommandOutput", Action { OpenCommandOutput {} }, documentation::OpenCommandOutput },
```

and in `formatActionArguments`, after the `CopyCommandLine` lambda:

```cpp
            [](OpenCommandOutput const& a) {
                auto const program = a.program.empty() ? std::string {} : std::format(", program: '{}'", a.program);
                return std::format(", target: {}{}, placement: {}, format: {}{}",
                                   contour::config::configEnumToken(a.target),
                                   program,
                                   contour::config::configEnumToken(a.placement),
                                   contour::config::configEnumToken(a.format),
                                   formatPinnedBlock(a.block));
            },
```

- [ ] **Step 5: Identity and parsing** — in `src/contour/command/Command.cpp`, after the `CopyCommandLine` lambda insert:

```cpp
            [](OpenCommandOutput const& a) -> CommandArguments {
                if (a.target == vtbackend::CommandBlockTarget::Last && a.program.empty()
                    && a.placement == OutputPlacement::Split && a.format == OutputFormat::Sgr && a.block.value == 0)
                    return {};
                auto const placement = config::configEnumToken(a.placement);
                auto title = a.program.empty() ? std::format(" ({})", placement)
                                               : std::format(": {} ({})", a.program, placement);
                return { .id = std::format("{}:{}:{}:{}:{}",
                                           config::configEnumToken(a.target),
                                           placement,
                                           config::configEnumToken(a.format),
                                           a.program,
                                           a.block.value),
                         .title = std::move(title) };
            },
```

In `src/contour/config/Config.cpp`'s `parseAction`, after the `CopyCommandLine` branch insert:

```cpp

        if (holds_alternative<actions::OpenCommandOutput>(action))
        {
            auto const target = enumActionArgument(node, "target", vtbackend::CommandBlockTarget::Last);
            auto const placement = enumActionArgument(node, "placement", actions::OutputPlacement::Split);
            auto const format = enumActionArgument(node, "format", actions::OutputFormat::Sgr);
            if (!target || !placement || !format)
                return std::nullopt;

            auto program = std::string {};
            if (auto const programNode = node["program"]; programNode && programNode.IsScalar())
                program = programNode.as<std::string>();

            return actions::OpenCommandOutput { .target = *target,
                                                .program = std::move(program),
                                                .placement = *placement,
                                                .format = *format,
                                                .block = pinnedBlockArgument(node) };
        }
```

- [ ] **Step 6: Run it** — in `src/contour/session/TerminalSession.hpp`, after `bool operator()(actions::CopyCommandLine const& action);` add `    bool operator()(actions::OpenCommandOutput const& action);`, and after the `copyCommandBlock` declaration add:

```cpp

    /// Logs why OpenCommandOutput showed nothing and tells the user through showNotification().
    /// @param reason What went wrong, lower-case and without a trailing period.
    void reportOpenOutputFailure(std::string_view reason);
```

In `src/contour/session/TerminalSession.cpp`, add `#include <contour/session/CommandOutputFiles.hpp>` to the includes; in the file's anonymous namespace (the one ending `} // namespace` just before `TerminalSession::withFolding`, :3000) add:

```cpp

    /// @p path as UTF-8 whatever the platform's native encoding: what an ExecInfo argument carries.
    [[nodiscard]] std::string utf8Path(std::filesystem::path const& path)
    {
        auto const text = path.u8string();
        return { text.begin(), text.end() };
    }
```

and after the `CopyCommandLine` handler insert:

```cpp

void TerminalSession::reportOpenOutputFailure(std::string_view reason)
{
    errorLog()("Could not open the command output: {}.", reason);
    emit showNotification(tr("Open Output"),
                          tr("Could not open the command output: %1.")
                              .arg(QString::fromUtf8(reason.data(), static_cast<qsizetype>(reason.size()))));
}

bool TerminalSession::operator()(actions::OpenCommandOutput const& action)
{
    // A program reading its standard input cannot be assumed to interpret SGR, so detached is plain.
    auto const rendition =
        action.placement != actions::OutputPlacement::Detached && action.format == actions::OutputFormat::Sgr
            ? vtbackend::CaptureRendition::WithSgr
            : vtbackend::CaptureRendition::PlainText;
    auto const output = core::locked(_terminal, [&]() {
        return terminal().commandBlockText(action.target, vtbackend::CommandBlockPart::Output, rendition, action.block);
    });

    // No block there, or no output yet: nothing to show, which is not an error worth a notice.
    if (!output)
    {
        sessionLog()("OpenCommandOutput: {}.", vtbackend::describe(output.error()));
        return false;
    }

    auto words = config::shellSplit(action.program.empty() ? _config.commandBlocks.value().pager : action.program);
    if (words.empty())
    {
        reportOpenOutputFailure("no pager is configured (command_blocks.pager)");
        return false;
    }
    auto const program = words.front();
    auto arguments = std::vector<std::string>(std::next(words.begin()), words.end());

    if (action.placement == actions::OutputPlacement::Detached)
    {
        auto qtArguments = QStringList {};
        for (auto const& argument: arguments)
            qtArguments.append(QString::fromStdString(argument));
        auto const started = _app.externalLauncher().runWithStdin(
            QString::fromStdString(program), qtArguments, QByteArray::fromStdString(*output));
        if (!started)
        {
            reportOpenOutputFailure(platform::describe(started.error()));
            return false;
        }
        return true;
    }

    auto& files = _app.commandOutputFiles();
    auto const file = files.create(*output);
    if (!file)
    {
        reportOpenOutputFailure(describe(file.error()));
        return false;
    }

    arguments.push_back(utf8Path(*file));
    auto const command = vtpty::Process::ExecInfo { .program = program, .arguments = std::move(arguments) };
    auto const placement = action.placement == actions::OutputPlacement::Tab ? CommandPanePlacement::Tab
                                                                             : CommandPanePlacement::Split;
    auto* pane = _manager->openCommandPane(this, placement, command);
    if (pane == nullptr)
    {
        files.release(*file);
        reportOpenOutputFailure("no pane could be opened for it");
        return false;
    }

    // The file lives exactly as long as the pane showing it.
    QObject::connect(pane, &TerminalSession::sessionClosed, pane, [&files, path = *file]() { files.release(path); });
    return true;
}
```

Run `clang-format -i` on every file touched in this task.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test`, then `out/build/clangcl-debug/bin/contour_test.exe` and `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[semanticblocks]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 8: Commit**

```bash
git add src/contour/config/BlockActionEnums.hpp src/contour/config/Actions.hpp src/contour/config/Config.cpp src/contour/command/Command.cpp src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/config/Actions_test.cpp src/contour/command/CommandCatalog_test.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
actions: add OpenCommandOutput

Show a command's output in a pager: in a split or a new tab running
`program FILE` (command_blocks.pager when the binding names none) over a
private file that is deleted when the pane closes, or piped as plain text
into a detached program such as wl-copy. SGR is kept for a pager that
renders it unless `format: plain` says otherwise.

A failure to write the file, open the pane or start the program is logged
and shown as a notification; a target with no output simply opens nothing.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.17: `Ctrl+Shift+G` opens the last output in the pager

**Files:**
- Modify: `src/contour/config/Config.hpp` (after `using FallbackKeyMapping = ...;` :170; after `builtinFallbackKeyMappings()` :184; after the key `applyBuiltinFallback` :284-287)
- Modify: `src/contour/config/Config.cpp` (after `builtinFallbackKeyMappings()` :146; after the key `applyBuiltinFallback` :177-183)
- Modify: `src/contour/session/TerminalSession.cpp` (`sendCharEvent` :2010-2012)
- Modify: `docs/configuration/key-mapping.md` (built-in bindings table)
- Test: `src/contour/config/Config_test.cpp`, `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: Task 5.16 `OpenCommandOutput`; `FallbackMapping`, `applyFallbackTable` (Task 5.6 form).
- Produces:
  ```cpp
  using FallbackCharMapping = FallbackMapping<char32_t>;
  [[nodiscard]] std::vector<FallbackCharMapping> const& builtinFallbackCharMappings();
  [[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config, char32_t codepoint,
                                                       vtbackend::Modifiers modifiers, uint8_t actualModeFlags);
  ```
  A Ctrl+Shift+letter chord arrives as a character (as Ctrl+Shift+P does), and no fallback table for characters exists yet; the binding is a fallback, not a default, for the reason every new binding is (`builtinFallbackMouseMappings`).

- [ ] **Step 1: Verify the chord is free** — run each and compare:
  - `git grep -n "'G'" src/contour/config/Config.hpp src/contour/config/Config.cpp` — expected: no output (no default or fallback row binds the character G).
  - `git grep -n "Key_G\b" src/contour` — expected: exactly `src/contour/session/SessionInput.cpp:472:        pair { Qt::Key_G, 'G' },` (the Qt-key-to-character routing table, not a binding).
  - `git grep -n -i "shift+g\b" src/contour docs/configuration` — expected: no output.
  - `git grep -n "Modifier::Meta" src/contour/config/Config.hpp` — expected: no row binding `'G'` (macOS maps Cmd to Meta; the defaults carry no platform `#ifdef`, so "free" holds on every platform).
  If any of these finds a binding, stop and report it to the coordinator instead of continuing (spec §16.1).

- [ ] **Step 2: Write the failing tests** — append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: Ctrl+Shift+G opens the last command's output by default", "[config][semanticblocks]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    auto const config = contour::config::Config {};
    auto const ctrlShift = Modifiers { Modifier::Control, Modifier::Shift };

    auto const* bound = contour::config::applyBuiltinFallback(config, U'G', ctrlShift, uint8_t { 0 });
    REQUIRE(bound != nullptr);
    REQUIRE(std::holds_alternative<contour::actions::OpenCommandOutput>(bound->at(0)));
    auto const& open = std::get<contour::actions::OpenCommandOutput>(bound->at(0));
    CHECK(open.target == vtbackend::CommandBlockTarget::Last);
    CHECK(open.program.empty()); // command_blocks.pager
    CHECK(open.placement == contour::actions::OutputPlacement::Split);

    // Exactly the chord: not Ctrl+G, which the shell owns.
    CHECK(contour::config::applyBuiltinFallback(config, U'G', Modifiers { Modifier::Control }, uint8_t { 0 })
          == nullptr);

    // A fallback, not a default -- it must reach users whose contour.yml lists the defaults.
    CHECK(std::ranges::none_of(config.inputMappings.value().charMappings,
                               [](auto const& mapping) { return mapping.input == U'G'; }));
}
```

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
namespace
{
/// Presses Ctrl+Shift+G in @p session the way a Qt key event delivers it: as the character 'G'.
void pressCtrlShiftG(contour::session::TerminalSession& session)
{
    session.sendCharEvent(U'G',
                          vtbackend::KeyIdentity { .unshiftedKey = U'g' },
                          Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                          KeyboardEventType::Press,
                          std::chrono::steady_clock::time_point {});
}

/// A finished `make` that printed "built", then a live prompt.
void finishedMake(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033]133;A\033\\$ make\r\n\033]133;C\033\\built\r\n");
    session.terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
}
} // namespace

TEST_CASE("TerminalSession: Ctrl+Shift+G opens the last output in the pager",
          "[contour][session][input][semanticblocks]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    finishedMake(*acting);
    auto const panesBefore = factory->requestedCommandOverrides.size();

    pressCtrlShiftG(*acting);

    CHECK(factory->requestedCommandOverrides.size() == panesBefore + 1);
}

TEST_CASE("TerminalSession: a user binding of Ctrl+Shift+G wins over the built-in one",
          "[contour][session][input][semanticblocks]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    // Before the session exists: a session takes its own copy of the configuration when it is made.
    app.app().config().inputMappings.value().charMappings.push_back(contour::config::CharInputMapping {
        .modes { vtbackend::MatchModes {} },
        .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift } },
        .input = U'G',
        .binding = { { contour::actions::SelectAll {} } } });

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    finishedMake(*acting);
    auto const panesBefore = factory->requestedCommandOverrides.size();

    pressCtrlShiftG(*acting);

    CHECK(factory->requestedCommandOverrides.size() == panesBefore);
    CHECK(acting->terminal().selectionAvailable());
}
```

- [ ] **Step 3: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — no `applyBuiltinFallback` overload accepting a `char32_t`.

- [ ] **Step 4: Add the character fallback table** — in `src/contour/config/Config.hpp`, after `using FallbackKeyMapping = FallbackMapping<vtbackend::Key>;` add `using FallbackCharMapping = FallbackMapping<char32_t>;`; after the `builtinFallbackKeyMappings()` declaration add

```cpp

/// @copydoc builtinFallbackMouseMappings
/// Characters, folded upper-case (@see foldedBindingCodepoint): a Ctrl+Shift+letter chord arrives as one.
[[nodiscard]] std::vector<FallbackCharMapping> const& builtinFallbackCharMappings();
```

and after the key `applyBuiltinFallback` declaration add

```cpp

/// @copydoc applyBuiltinFallback
[[nodiscard]] ActionList const* applyBuiltinFallback(Config const& config,
                                                     char32_t codepoint,
                                                     vtbackend::Modifiers modifiers,
                                                     uint8_t actualModeFlags);
```

In `src/contour/config/Config.cpp`, after `builtinFallbackKeyMappings()` insert:

```cpp

std::vector<FallbackCharMapping> const& builtinFallbackCharMappings()
{
    // Fallbacks rather than defaults for the reason the key table gives. Ctrl+Shift+G was checked to be
    // bound nowhere in the defaults, on any platform, when it was added (spec §16.1); it is kitty's
    // binding for the same thing.
    static auto const mappings = std::vector<FallbackCharMapping> {
        FallbackCharMapping { .mapping = { .modes { vtbackend::MatchModes {} },
                                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control,
                                                                               vtbackend::Modifier::Shift } },
                                           .input = U'G',
                                           .binding = { { actions::OpenCommandOutput {} } } } },
    };
    return mappings;
}
```

and after the key `applyBuiltinFallback` definition:

```cpp

ActionList const* applyBuiltinFallback(Config const& config,
                                       char32_t codepoint,
                                       vtbackend::Modifiers modifiers,
                                       uint8_t actualModeFlags)
{
    return applyFallbackTable(
        builtinFallbackCharMappings(), config, codepoint, modifiers, actualModeFlags, std::nullopt);
}
```

- [ ] **Step 5: Consult it** — in `src/contour/session/TerminalSession.cpp`'s `sendCharEvent`, directly after the un-shifted retry (`actions = config::apply(charMappings, base, modifiers.chord, flags);`, :2010) insert:

```cpp

        // The user's mappings did not claim this character, so fall back to the built-in ones. Second on
        // purpose, as for keys: an explicit binding of the same chord in the user's config wins.
        if (actions == nullptr)
            actions = config::applyBuiltinFallback(_config, folded, modifiers.chord, flags);
```

In `docs/configuration/key-mapping.md`, add to the built-in bindings table after the `Ctrl+Shift+Tab` row:

```markdown
| `Ctrl+Shift+G` | Open the last command's output in the pager (`command_blocks.pager`; needs a shell that emits OSC 133 marks) |
```

Run `clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/session/TerminalSession.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp`.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config],[session]"`
Expected: zero warnings; `All tests passed` — the existing "command palette is bound to Ctrl+Shift+P" and char-binding cases included.

- [ ] **Step 7: Commit**

```bash
git add src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/session/TerminalSession.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp docs/configuration/key-mapping.md
git commit -F - <<'EOF'
config: open the last command's output with Ctrl+Shift+G

kitty's binding, checked to be free in the defaults on every platform. A
Ctrl+Shift+letter chord arrives as a character, and there was no fallback
table for characters, so add one and consult it after the user's own
character bindings -- a default would never reach a user whose contour.yml
already lists them, and a user binding of the same chord still wins.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.18: Command-block rows in the context menu

**Files:**
- Modify: `src/contour/command/ContextMenu.hpp` (`ContextMenuState`, after `foldLine`)
- Modify: `src/contour/command/ContextMenu.cpp` (predicates :17-67; a row helper after `hyperlinkCommand` :69-80; rows after "Toggle Output Fold" :155-159)
- Modify: `src/contour/session/TerminalSession.cpp` (`contextMenuState()` :2435-2483)
- Test: `src/contour/command/ContextMenu_test.cpp` (`contour_test`), `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: Tasks 5.3/5.8/5.16 actions (their `target`/`block` fields); C1 `Terminal::commandBlockAt`, `CommandBlockRecord::commandLine`.
- Produces (`contour::command::ContextMenuState` gains):
  ```cpp
  vtbackend::CommandBlockId blockUnderCursor {};     // zero: the click was on no block's row
  bool blockUnderCursorHasCommandLine = false;       // that block's shell reported its command line
  ```
  Rows "Select Output", "Copy Output", "Copy Command", "Open Output in Pager": shown when `hasLastCommand` (the shell speaks OSC 133), grayed when the click was on no block ("Copy Command" also when the block has no command line), each pinned to the clicked block with `target: pointer`.

- [ ] **Step 1: Write the failing tests** — in `src/contour/command/ContextMenu_test.cpp`, extend `fullyEnabledState()` by inserting after `.foldLine = vtbackend::LineOffset(7),`:

```cpp
        .blockUnderCursor = vtbackend::CommandBlockId(5),
        .blockUnderCursorHasCommandLine = true,
```

and append:

```cpp
TEST_CASE("ContextMenu.blockRows.pinTheRightClickedBlock", "[contextmenu][semanticblocks]")
{
    // Pinned by id, as the fold row pins its line: by the time a row is picked the pointer has left
    // the block for the menu itself.
    auto const menu = buildContextMenu(fullyEnabledState());
    auto const pinned = vtbackend::CommandBlockId(5);

    auto const* select = find(menu, "Select Output");
    REQUIRE(select != nullptr);
    CHECK(select->enabled);
    REQUIRE(std::holds_alternative<actions::SelectCommandBlock>(select->action));
    CHECK(std::get<actions::SelectCommandBlock>(select->action).block == pinned);
    CHECK(std::get<actions::SelectCommandBlock>(select->action).part == vtbackend::CommandBlockPart::Output);

    auto const* copy = find(menu, "Copy Output");
    REQUIRE(copy != nullptr);
    REQUIRE(std::holds_alternative<actions::CopyCommandBlock>(copy->action));
    CHECK(std::get<actions::CopyCommandBlock>(copy->action).block == pinned);

    auto const* command = find(menu, "Copy Command");
    REQUIRE(command != nullptr);
    CHECK(command->enabled);
    REQUIRE(std::holds_alternative<actions::CopyCommandLine>(command->action));
    CHECK(std::get<actions::CopyCommandLine>(command->action).block == pinned);

    auto const* pager = find(menu, "Open Output in Pager");
    REQUIRE(pager != nullptr);
    REQUIRE(std::holds_alternative<actions::OpenCommandOutput>(pager->action));
    CHECK(std::get<actions::OpenCommandOutput>(pager->action).block == pinned);
    CHECK(std::get<actions::OpenCommandOutput>(pager->action).placement == actions::OutputPlacement::Split);
}

TEST_CASE("ContextMenu.blockRows.grayedOffABlock", "[contextmenu][semanticblocks]")
{
    // Grayed, not hidden, for the reason the fold row is: the menu must not change shape with where
    // the click landed.
    auto state = fullyEnabledState();
    state.blockUnderCursor = vtbackend::CommandBlockId {};
    state.blockUnderCursorHasCommandLine = false;
    auto const offBlock = buildContextMenu(state);
    for (auto const* title: { "Select Output", "Copy Output", "Copy Command", "Open Output in Pager" })
    {
        INFO(title);
        auto const* row = find(offBlock, title);
        REQUIRE(row != nullptr);
        CHECK_FALSE(row->enabled);
    }

    // A block whose shell never reported its command line has output, but no command to copy.
    state = fullyEnabledState();
    state.blockUnderCursorHasCommandLine = false;
    auto const noCommandLine = buildContextMenu(state);
    CHECK_FALSE(find(noCommandLine, "Copy Command")->enabled);
    CHECK(find(noCommandLine, "Copy Output")->enabled);
}

TEST_CASE("ContextMenu.blockRows.hiddenWithoutShellIntegration", "[contextmenu][semanticblocks]")
{
    auto state = fullyEnabledState();
    state.hasLastCommand = false;
    auto const menu = buildContextMenu(state);
    for (auto const* title: { "Select Output", "Copy Output", "Copy Command", "Open Output in Pager" })
    {
        INFO(title);
        CHECK(find(menu, title) == nullptr);
    }
}
```

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: the context menu pins the command block that was right-clicked",
          "[contour][session][contextmenu][semanticblocks]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("\033]133;A\033\\$ make\r\n\033]133;C;cmdline_url=make\033\\built\r\n");
    session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
    auto const pixels = vtbackend::PixelCoordinate {};

    SECTION("over a finished command's output")
    {
        session->sendMouseMoveEvent(
            Modifiers {}, vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(0) }, pixels);
        auto const state = session->contextMenuState();

        auto const* make = session->terminal().commandBlockAt(vtbackend::LineOffset(0));
        REQUIRE(make != nullptr);
        CHECK(state.blockUnderCursor == make->id);
        CHECK(state.blockUnderCursorHasCommandLine);

        // The pinned row acts on that block even though the pointer has since moved away.
        session->sendMouseMoveEvent(
            Modifiers {}, vtbackend::CellLocation { vtbackend::LineOffset(2), vtbackend::ColumnOffset(0) }, pixels);
        CHECK((*session)(contour::actions::CopyCommandBlock { .target = vtbackend::CommandBlockTarget::Pointer,
                                                             .block = state.blockUnderCursor }));
        CHECK(QGuiApplication::clipboard()->text().toStdString() == "built");
    }

    SECTION("over the live prompt, whose command is not known yet")
    {
        session->sendMouseMoveEvent(
            Modifiers {}, vtbackend::CellLocation { vtbackend::LineOffset(2), vtbackend::ColumnOffset(0) }, pixels);
        auto const state = session->contextMenuState();
        CHECK(state.blockUnderCursor.value != 0);
        CHECK_FALSE(state.blockUnderCursorHasCommandLine);
    }

    SECTION("below every block")
    {
        session->sendMouseMoveEvent(
            Modifiers {}, vtbackend::CellLocation { vtbackend::LineOffset(10), vtbackend::ColumnOffset(0) }, pixels);
        CHECK(session->contextMenuState().blockUnderCursor.value == 0);
    }

    QGuiApplication::clipboard()->clear();
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_test`
Expected: FAIL — `'blockUnderCursor': is not a member of 'contour::command::ContextMenuState'`.

- [ ] **Step 3: Snapshot the block** — in `src/contour/command/ContextMenu.hpp`, after the `foldLine` member insert:

```cpp
    /// The command block whose row was right-clicked; zero when the click was on no block's row.
    ///
    /// Captured at click time and carried by the block rows, for the reason foldLine is: the pointer
    /// leaves the block for the menu the moment the user reaches for a row.
    vtbackend::CommandBlockId blockUnderCursor {};
    /// Whether that block's shell reported its command line (OSC 133;C cmdline_url), which is what
    /// "Copy Command" copies. A plain bool like every other field here: the struct is a snapshot of the
    /// predicates the menu table reads, and this is one of them.
    bool blockUnderCursorHasCommandLine = false;
```

- [ ] **Step 4: Add the rows** — in `src/contour/command/ContextMenu.cpp`, after `hasFoldUnderCursor` (:64-67) insert:

```cpp

    bool hasBlockUnderCursor(ContextMenuState const& state) noexcept
    {
        return state.blockUnderCursor.value != 0;
    }

    bool hasCommandLineUnderCursor(ContextMenuState const& state) noexcept
    {
        return hasBlockUnderCursor(state) && state.blockUnderCursorHasCommandLine;
    }
```

after `hyperlinkCommand` (:69-80) insert:

```cpp

    /// A row acting on the block that was RIGHT-CLICKED, pinned by its id -- shown where the shell
    /// speaks OSC 133, grayed where the click landed on no block.
    /// @param title The row's text.
    /// @param enabled When the row can be picked.
    template <typename BlockAction>
    [[nodiscard]] Row blockCommand(std::string_view title, Table::Predicate enabled)
    {
        return Table::command(BlockAction {}, title)
            .shownWhen(hasLastCommand)
            .enabledWhen(enabled)
            .actionFrom([](ContextMenuState const& state) -> actions::Action {
                auto action = BlockAction {};
                action.target = vtbackend::CommandBlockTarget::Pointer;
                action.block = state.blockUnderCursor;
                return action;
            });
    }
```

(`Table::Predicate` is `bool (*)(ContextMenuState const&) noexcept`, `ContextMenuTable.hpp:37`; the predicates above convert to it.) After the "Toggle Output Fold" row (ends `}),` at :159) insert:

```cpp

            // The block the user right-clicked. Pinned like the fold line above; its output part is
            // what a user points at, so these are the output rows, with the command line beside them.
            blockCommand<SelectCommandBlock>("Select Output", hasBlockUnderCursor),
            blockCommand<CopyCommandBlock>("Copy Output", hasBlockUnderCursor),
            blockCommand<CopyCommandLine>("Copy Command", hasCommandLineUnderCursor),
            blockCommand<OpenCommandOutput>("Open Output in Pager", hasBlockUnderCursor),
```

- [ ] **Step 5: Fill it in the session** — in `src/contour/session/TerminalSession.cpp`'s `contextMenuState()`, replace the `foldLine` lambda (:2439-2449) with:

```cpp
        // The grid line under the pointer. _currentMousePosition is in MAIN-PAGE rows -- which is the
        // space translateScreenToGridLine() speaks, so it is fed in unadjusted -- and was last written by
        // the move that necessarily preceded this right-click, so it still names the clicked cell. Adding
        // mainPageTopRow() here would reintroduce the off-by-a-status-line the gutter hit-test already
        // had (@see gutterHitAt in SessionInput.cpp, which returns a std::optional<GutterHit>).
        auto const pointerLine = terminal().viewport().translateScreenToGridLine(_currentMousePosition.line);

        // ...when a fold reaches it.
        auto const foldLine = [&]() -> std::optional<vtbackend::LineOffset> {
            if (!_config.folding.value().enabled)
                return std::nullopt;
            return terminal().foldContaining(pointerLine) ? std::optional { pointerLine } : std::nullopt;
        }();

        // ...and the block it belongs to. Only the primary screen has blocks: over an alternate-screen
        // application the pointer is over none.
        auto const* pointedBlock = terminal().isPrimaryScreen() ? terminal().commandBlockAt(pointerLine) : nullptr;
```

and after `.foldLine = foldLine,` (:2479) insert:

```cpp
            .blockUnderCursor = pointedBlock != nullptr ? pointedBlock->id : vtbackend::CommandBlockId {},
            .blockUnderCursorHasCommandLine = pointedBlock != nullptr && !pointedBlock->commandLine.empty(),
```

Run `clang-format -i src/contour/command/ContextMenu.hpp src/contour/command/ContextMenu.cpp src/contour/command/ContextMenu_test.cpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp`.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_test contour_gui_test`, then `out/build/clangcl-debug/bin/contour_test.exe "[contextmenu]"` and `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[contextmenu]"`
Expected: zero warnings; `All tests passed` — the separator sweep and "every command id resolves" cases included.

- [ ] **Step 7: Commit**

```bash
git add src/contour/command/ContextMenu.hpp src/contour/command/ContextMenu.cpp src/contour/command/ContextMenu_test.cpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
command: add command-block rows to the context menu

Right-clicking a command block offers Select Output, Copy Output, Copy
Command and Open Output in Pager next to Toggle Output Fold. Each row is
pinned to the block that was clicked, as the fold row pins its line, so it
acts on that block after the pointer has moved to the menu. The rows are
hidden without shell integration and grayed where the click landed on no
block; Copy Command also where the shell reported no command line.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.19: Resolve paths against the directory of the command that printed them

**Files:**
- Create: `src/vtbackend/shell/CommandBlockPaths.hpp`, `src/vtbackend/shell/CommandBlockPaths.cpp`
- Test: create `src/vtbackend/shell/CommandBlockPaths_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (headers/sources/tests next to `shell/CommandBlockRows.*` of Task 5.1)
- Modify: `src/vtbackend/input/vi/HintModeHandler.hpp` (`HintPattern` :80-91), `src/vtbackend/input/vi/HintModeHandler.cpp` (:137, :145), `src/vtbackend/input/vi/HintModeHandler_test.cpp` (nine lambdas)
- Modify: `src/vtbackend/screen/Terminal.hpp` (private, after `selectLines` of Task 5.2), `src/vtbackend/screen/Terminal.cpp` (`localPathAtMousePosition` :3174-3219; `activateHintMode` :3281-3343)
- Test: `src/vtbackend/screen/Terminal_block_actions_test.cpp`

**Interfaces:**
- Consumes: C1 `CommandBlockRecord::workingDirectory` (`WorkingDirectorySnapshot { path, locality }`), `ContextLocality`, `Terminal::commandBlockAt`, `CommandBlockStore::adopt(record, AdoptMode::Snapshot)` (test only), `Line::adoptBlock` (test only).
- Produces:
  ```cpp
  [[nodiscard]] std::optional<std::string> localPathBaseFor(CommandBlockRecord const* block, std::string terminalCwd);
  // HintPattern
  std::function<bool(std::string const&, LineOffset)> validator;
  std::function<std::string(std::string const&, LineOffset)> transformer;
  // Terminal (private)
  [[nodiscard]] std::optional<std::string> localPathBaseAt(LineOffset row) const;
  ```

- [ ] **Step 1: Write the failing unit test** — create `src/vtbackend/shell/CommandBlockPaths_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockPaths.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace vtbackend;

namespace
{
[[nodiscard]] CommandBlockRecord ranIn(std::string path, ContextLocality locality)
{
    return CommandBlockRecord { .id = CommandBlockId(1),
                                .workingDirectory = WorkingDirectorySnapshot { .path = std::move(path),
                                                                               .locality = locality } };
}
} // namespace

TEST_CASE("localPathBaseFor.aRowOutsideAnyBlockKeepsTheTerminalsDirectory", "[semanticblocks][blockpaths]")
{
    CHECK(localPathBaseFor(nullptr, "/home/u") == "/home/u");
    CHECK(localPathBaseFor(nullptr, "") == "");
}

TEST_CASE("localPathBaseFor.aBlockResolvesAgainstItsOwnDirectory", "[semanticblocks][blockpaths]")
{
    auto const local = ranIn("/src/a", ContextLocality::Local);
    CHECK(localPathBaseFor(&local, "/home/u") == "/src/a");

    // Unknown is what a plain OSC 7 shell reports: the directory is still the command's own.
    auto const unknown = ranIn("/src/b", ContextLocality::Unknown);
    CHECK(localPathBaseFor(&unknown, "/home/u") == "/src/b");
}

TEST_CASE("localPathBaseFor.aBlockThatRanElsewhereRefuses", "[semanticblocks][blockpaths]")
{
    auto const foreign = ranIn("/srv/app", ContextLocality::Foreign);
    CHECK_FALSE(localPathBaseFor(&foreign, "/home/u").has_value());

    auto const foreignWithoutPath = ranIn("", ContextLocality::Foreign);
    CHECK_FALSE(localPathBaseFor(&foreignWithoutPath, "/home/u").has_value());
}

TEST_CASE("localPathBaseFor.aBlockWithoutADirectoryKeepsTheTerminalsDirectory", "[semanticblocks][blockpaths]")
{
    auto const unrecorded = ranIn("", ContextLocality::Unknown);
    CHECK(localPathBaseFor(&unrecorded, "/home/u") == "/home/u");
}
```

Register it in `src/vtbackend/CMakeLists.txt` after `        shell/CommandBlockRows_test.cpp`:

```cmake
        shell/CommandBlockPaths_test.cpp
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `cannot open include file: 'vtbackend/shell/CommandBlockPaths.hpp'`.

- [ ] **Step 3: Implement the rule** — create `src/vtbackend/shell/CommandBlockPaths.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <optional>
#include <string>

namespace vtbackend
{

/// The directory a relative path printed on a row is resolved against (spec §7.4).
///
/// The directory the command that printed it ran in, not the shell's directory now: `cd` between two
/// commands must not re-point the paths an earlier one printed. The same policy as
/// resolveWorkingDirectory()'s OpenLocally: a directory on another machine is never opened here.
/// @param block The record of the block the row belongs to; nullptr for a row outside any block.
/// @param terminalCwd The terminal's working directory (OSC 7) -- the base before blocks existed.
/// @return The block's directory when it has one; @p terminalCwd when there is no block, or the block
///         recorded no directory; nullopt when the command ran elsewhere (ssh, a container), whose paths
///         name nothing on this machine.
[[nodiscard]] std::optional<std::string> localPathBaseFor(CommandBlockRecord const* block, std::string terminalCwd);

} // namespace vtbackend
```

Create `src/vtbackend/shell/CommandBlockPaths.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockPaths.hpp>

#include <utility>

namespace vtbackend
{

std::optional<std::string> localPathBaseFor(CommandBlockRecord const* block, std::string terminalCwd)
{
    if (block == nullptr)
        return terminalCwd;
    if (block->workingDirectory.locality == ContextLocality::Foreign)
        return std::nullopt;
    if (block->workingDirectory.path.empty())
        return terminalCwd;
    return block->workingDirectory.path;
}

} // namespace vtbackend
```

Register both in `src/vtbackend/CMakeLists.txt` next to `shell/CommandBlockRows.hpp` / `shell/CommandBlockRows.cpp`. Run `clang-format -i` on the three new files.

- [ ] **Step 4: Run the unit test to verify it passes**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockpaths]"`
Expected: zero warnings; `All tests passed` (4 test cases).

- [ ] **Step 5: Hand the hint callbacks their row** — in `src/vtbackend/input/vi/HintModeHandler.hpp`, replace the two callback members of `HintPattern` (:84-90) with:

```cpp
    /// Optional post-match validator. When set, only matches for which this returns true are kept.
    /// Used e.g. to check filesystem existence. Told the grid row the match starts on, because what a
    /// match means can depend on where it was printed (a path, on the directory its command ran in).
    std::function<bool(std::string const&, LineOffset)> validator;
    /// Optional post-match transformer. When set, the matched text is rewritten before being stored in
    /// HintMatch, e.g. a relative path resolved to an absolute one; told the row as the validator is.
    /// The overlay still shows the original terminal text.
    std::function<std::string(std::string const&, LineOffset)> transformer;
```

In `src/vtbackend/input/vi/HintModeHandler.cpp` replace `if (pattern.validator && !pattern.validator(matchStr))` (:137) with `if (pattern.validator && !pattern.validator(matchStr, start.line))`, and `.matchedText = pattern.transformer ? pattern.transformer(matchStr) : matchStr,` (:145) with `.matchedText = pattern.transformer ? pattern.transformer(matchStr, start.line) : matchStr,`.

Update the test lambdas mechanically:

```bash
sed -i -E \
  -e 's|\(std::string const& matchStr\) -> bool|(std::string const\& matchStr, LineOffset /*row*/) -> bool|g' \
  -e 's|\(std::string const&\) -> bool|(std::string const\&, LineOffset /*row*/) -> bool|g' \
  -e 's|\(std::string const& matchStr\) -> std::string|(std::string const\& matchStr, LineOffset /*row*/) -> std::string|g' \
  src/vtbackend/input/vi/HintModeHandler_test.cpp
```

Run: `grep -c "LineOffset /\*row\*/" src/vtbackend/input/vi/HintModeHandler_test.cpp`
Expected: `9`.

Keep `Terminal.cpp` compiling until Step 8: in `activateHintMode`, change the validator lambda's `std::string const& matchStr) -> bool {` to `std::string const& matchStr, LineOffset /*row*/) -> bool {`, and replace `pattern.transformer = resolvePath;` with `pattern.transformer = [resolvePath](std::string const& matchStr, LineOffset /*row*/) { return resolvePath(matchStr); };` (Step 8 replaces this whole block).

- [ ] **Step 6: Write the failing Terminal tests** — append to `src/vtbackend/screen/Terminal_block_actions_test.cpp` (add `#include <vtbackend/input/vi/HintModeHandler.hpp>`, `#include <core/Utils.hpp>`, `#include <cstdint>`, `#include <filesystem>`, `#include <format>`, `#include <fstream>`, `#include <initializer_list>`, `#include <random>`, `#include <tuple>` and `#include <utility>` to its includes):

```cpp
namespace
{

/// Records block @p id as having run in @p path with @p locality and stamps @p rows with it -- what a
/// daemon snapshot hands a mirror, so no shell has to report the directory.
void stampBlock(MockTerm<>& mock,
                uint32_t id,
                std::string path,
                ContextLocality locality,
                std::initializer_list<int> rows)
{
    std::ignore = mock.terminal.commandBlocks().adopt(
        CommandBlockRecord { .id = CommandBlockId(id),
                             .state = CommandBlockState::Finished,
                             .workingDirectory = WorkingDirectorySnapshot { .path = std::move(path),
                                                                            .locality = locality } },
        AdoptMode::Snapshot);
    for (auto const row: rows)
        mock.terminal.primaryScreen().grid().lineAt(LineOffset(row)).adoptBlock(CommandBlockId(id));
}

/// A unique directory under the system's temporary one, holding `a/notes.txt` and an empty `b`.
[[nodiscard]] std::filesystem::path makeTwoDirectories(std::string_view tag)
{
    namespace fs = std::filesystem;
    auto const root = fs::temp_directory_path()
                      / std::format("contour-{}-{}", tag, std::random_device {}());
    fs::create_directories(root / "a");
    fs::create_directories(root / "b");
    auto file = std::ofstream(root / "a" / "notes.txt");
    file << "x";
    return root;
}

} // namespace

TEST_CASE("Terminal.localPathAtMousePosition.resolvesAgainstTheDirectoryOfTheCommandThatPrintedIt",
          "[semanticblocks][blockpaths]")
{
    namespace fs = std::filesystem;
    auto const root = makeTwoDirectories("block-path");
    auto const cleanup = core::Finally { [&]() { fs::remove_all(root); } };
    auto const expected = (root / "a" / "notes.txt").lexically_normal().string();

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(80) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory("file://" + (root / "b").generic_string());
    mock.writeToScreen("see notes.txt\r\n"sv);
    // Over the 't' of "notes.txt" (columns 4-12).
    mock.terminal.sendMouseMoveEvent(Modifier::None,
                                     CellLocation { .line = LineOffset(0), .column = ColumnOffset(6) },
                                     PixelCoordinate {},
                                     false);

    SECTION("a row of a local block resolves against that block's directory")
    {
        stampBlock(mock, 7, (root / "a").generic_string(), ContextLocality::Local, { 0 });
        CHECK(mock.terminal.localPathAtMousePosition() == expected);
    }

    SECTION("a row of a block that ran elsewhere is refused, even where the name exists here")
    {
        stampBlock(mock, 7, (root / "a").generic_string(), ContextLocality::Foreign, { 0 });
        CHECK_FALSE(mock.terminal.localPathAtMousePosition().has_value());
    }

    SECTION("a row outside any block keeps the OSC 7 directory")
    {
        CHECK_FALSE(mock.terminal.localPathAtMousePosition().has_value()); // b holds no notes.txt
        mock.terminal.setCurrentWorkingDirectory("file://" + (root / "a").generic_string());
        CHECK(mock.terminal.localPathAtMousePosition() == expected);
    }
}

TEST_CASE("Terminal.hintMode.resolvesPathsAgainstTheDirectoryOfTheCommandThatPrintedThem",
          "[semanticblocks][blockpaths]")
{
    namespace fs = std::filesystem;
    auto const root = makeTwoDirectories("block-hint");
    auto const cleanup = core::Finally { [&]() { fs::remove_all(root); } };

    auto mock = MockTerm<> { PageSize { LineCount(4), ColumnCount(60) }, LineCount(10) };
    mock.terminal.setCurrentWorkingDirectory("file://" + (root / "b").generic_string());
    mock.writeToScreen("local notes.txt\r\nremote notes.txt\r\nplain notes.txt"sv);
    stampBlock(mock, 7, (root / "a").generic_string(), ContextLocality::Local, { 0 });
    stampBlock(mock, 8, (root / "a").generic_string(), ContextLocality::Foreign, { 1 });
    mock.terminal.flushInput();

    mock.terminal.activateHintMode(vtbackend::HintModeRequest {
        .patterns = vtbackend::HintModeHandler::builtinPatterns(), .action = vtbackend::HintAction::Copy });

    // Row 0 resolves into a; row 1 ran on another machine; row 2 resolves into b, which has no notes.txt.
    auto const& matches = mock.terminal.hintMatches();
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].start.line == LineOffset(0));
    CHECK(matches[0].matchedText == (root / "a").generic_string() + "/notes.txt");
}
```

- [ ] **Step 7: Run the build to verify the new tests fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockpaths]"`
Expected: builds; FAIL in "a row of a local block resolves against that block's directory" (today's base is OSC 7's `b`) and in the hint test (`matches.size()` is 0).

- [ ] **Step 8: Resolve per row** — in `src/vtbackend/screen/Terminal.hpp` add `#include <vtbackend/shell/CommandBlockPaths.hpp>` to the `vtbackend/` includes, and after the `selectLines` declaration (Task 5.2) insert:

```cpp

    /// The directory a relative path printed on @p row resolves against (@see localPathBaseFor): its
    /// block's, else OSC 7's; nullopt when the block's command ran on another machine.
    [[nodiscard]] std::optional<std::string> localPathBaseAt(LineOffset row) const;
```

In `src/vtbackend/screen/Terminal.cpp`, before `Terminal::localPathAtMousePosition` (:3174) insert:

```cpp
std::optional<std::string> Terminal::localPathBaseAt(LineOffset row) const
{
    // Blocks live on the primary screen; a row of an alternate screen belongs to none.
    auto const* block = isPrimaryScreen() ? commandBlockAt(row) : nullptr;
    return localPathBaseFor(block, extractPathFromFileUrl(currentWorkingDirectory()));
}

```

In `localPathAtMousePosition` replace `    auto const cwd = extractPathFromFileUrl(currentWorkingDirectory());` (:3182) with

```cpp
    // The directory of the command that printed this row, not the shell's directory now. A command that
    // ran on another machine printed paths that name nothing here -- absolute ones included.
    auto const cwd = localPathBaseAt(mousePosition->line);
    if (!cwd)
        return std::nullopt;
```

and `        if (auto path = resolveExistingLocalPath(cwd, home, match.str()))` (:3214) with `        if (auto path = resolveExistingLocalPath(*cwd, home, match.str()))`.

In `activateHintMode`, replace everything from `    // When CWD is available, attach a filesystem-existence validator to filepath patterns.` (:3288) through the closing brace of `if (!cwd.empty()) { ... }` (:3339) with:

```cpp
    // Paths resolve against the directory of the command that printed them and, outside any block,
    // against OSC 7's -- so the filepath pattern gets a validator wherever either kind of base exists.
    auto const cwd = extractPathFromFileUrl(currentWorkingDirectory());
    if (!cwd.empty() || commandBlocks().size() != 0)
    {
        auto const& home = _homeDirectory;
        for (auto& pattern: mutablePatterns)
        {
            if (pattern.name != "filepath")
                continue;

            // With a base available, broaden the regex to also match bare filenames,
            // extensionless files (e.g. "Makefile"), and directories (e.g. "src").
            // The validator ensures only entries that actually exist on disk are kept.
            pattern.regex =
                std::regex(R"((?:~?/[\w./-]+|\.{1,2}/[\w./-]+|[\w.][\w.-]*/[\w./-]+|[\w.][\w.-]+))",
                           std::regex_constants::ECMAScript | std::regex_constants::optimize);

            // A matched path as an absolute one, resolved against the base of the row it was printed
            // on; nullopt when that row's command ran on another machine, or there is no base at all.
            // When HOME is unset and the path starts with ~/, it is returned unchanged.
            auto const resolvePath = [this, home](std::string const& matchStr,
                                                  LineOffset row) -> std::optional<std::string> {
                auto const base = localPathBaseAt(row);
                if (!base)
                    return std::nullopt;
                if (matchStr.starts_with("/"))
                    return matchStr;
                if (matchStr.starts_with("~/"))
                    return home.empty() ? matchStr : home + matchStr.substr(1);
                if (base->empty())
                    return std::nullopt;
                return *base + '/' + matchStr;
            };

            // Memoized per activation, by the RESOLVED path: the broadened regex matches every bare word,
            // and a scrollback-scope scan of a thousand rows would otherwise stat() each repeated one
            // again -- brutal on a network filesystem. Keyed by the resolved path because one name means
            // different files in the outputs of commands that ran in different directories.
            pattern.validator = [resolvePath, home, cache = std::make_shared<std::map<std::string, bool>>()](
                                    std::string const& matchStr, LineOffset row) -> bool {
                auto const resolved = resolvePath(matchStr, row);
                if (!resolved)
                    return false;
                // Cannot resolve ~/ paths when HOME is unset — let them through unvalidated.
                if (matchStr.starts_with("~/") && home.empty())
                    return true;
                if (auto const cached = cache->find(*resolved); cached != cache->end())
                    return cached->second;
                auto ec = std::error_code {};
                auto const exists = std::filesystem::exists(*resolved, ec);
                cache->emplace(*resolved, exists);
                return exists;
            };

            // Transform matched text to absolute path so Copy/Open actions work correctly.
            pattern.transformer = [resolvePath](std::string const& matchStr, LineOffset row) {
                return resolvePath(matchStr, row).value_or(matchStr);
            };
        }
    }
```

Run `clang-format -i` on every file touched in this task.

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[blockpaths]"`, `out/build/clangcl-debug/bin/vtbackend_test.exe "[hintmode]"` and `out/build/clangcl-debug/bin/vtbackend_test.exe "Terminal.localPathAtMousePosition"`
Expected: zero warnings; `All tests passed` in all three — the existing OSC 7 cases unchanged.

- [ ] **Step 10: Commit**

```bash
git add src/vtbackend/shell/CommandBlockPaths.hpp src/vtbackend/shell/CommandBlockPaths.cpp src/vtbackend/shell/CommandBlockPaths_test.cpp src/vtbackend/CMakeLists.txt src/vtbackend/input/vi/HintModeHandler.hpp src/vtbackend/input/vi/HintModeHandler.cpp src/vtbackend/input/vi/HintModeHandler_test.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_block_actions_test.cpp
git commit -F - <<'EOF'
vtbackend: resolve paths against the directory of the command that printed them

A relative path in a command's output names a file in the directory that
command ran in, not wherever the shell is now: `cd` between two commands
re-pointed every path the first one printed. Ctrl+click and hint mode now
resolve against the clicked row's block record, and fall back to OSC 7
only for rows outside any block.

A block whose command ran on another machine (ssh, a container) refuses
local opening, as OSC 3008 already does. The hint callbacks are told the
row a match starts on, and the validator's cache is keyed by resolved path.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 5.20: Phase gate

**Files:** none new (fixes land where the simplification and the review point).

- [ ] **Step 1: Full build with zero warnings, suite green**

Run: `cmake --build --preset clangcl-debug`
Expected: zero warnings (`-Werror` is on; a warning is a build break). If the pre-existing `src/contour/display/*` `yaml-cpp/emitter.h` break recorded at phase 0 recurs, build the touched targets instead: `cmake --build --preset clangcl-debug --target vtbackend_test contour_test contour_gui_test contour` and record which target could not be built.

Run: `ctest --test-dir out/build/clangcl-debug --output-on-failure`
Expected: green apart from the environmental failures recorded in the phase-0 baseline (no new ones). Note the `N tests passed, M failed` line — it goes into the body of this phase's final commit (Step 3 below, or Step 2 if the review finds nothing).

Run: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: PASS (or SKIP when `typos` is not installed, as at the baseline).

Hand-audit `src/contour/**`, where this Windows build runs no clang-tidy: in `git diff PHASE5_START..HEAD -- src/contour` there is no new `NOLINT`; no `std::expected` result discarded with `(void)`; no nested conditional operator; every new free function in a `.cpp` sits in an anonymous namespace; `const` on every local that is not mutated. Run `git diff PHASE5_START..HEAD -- src | grep -nwE "small|near|far|interface|boolean|hyper|byte|ERROR|DELETE|IN|OUT"` and expect no new identifier spelled like a Windows header macro among the hits. Where WSL is set up (memory: wsl-clang-tidy-22-verify), run CI's clang-tidy-22 over the changed files as well.

- [ ] **Step 2: Simplify** — run `/simplify` over `git diff PHASE5_START..HEAD`. Candidates it should weigh: the per-action `enumActionArgument`/`pinnedBlockArgument` call shapes in `parseAction`, the `blockArguments` identity helper and the two hand-written identity lambdas beside it, and the repeated "finished `make`" setup in the session tests. Rebuild, re-run `ctest --test-dir out/build/clangcl-debug --output-on-failure`, and commit its fixes:

```bash
git add -u
git commit -F - <<'EOF'
semantic blocks: simplify the block actions

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(If `/simplify` changes nothing, there is no commit here.)

- [ ] **Step 3: Code review at xhigh** — run `/code-review xhigh` on the commits `PHASE5_START..HEAD` (or dispatch a review subagent with `effort: "xhigh"`), against the README's Review Focus and AGENT.md: no `bool` in new API except the documented `ContextMenuState` snapshot field; the clock, the filesystem and process launching reached through `IClock`, `FileSystem` and `ExternalLauncher`; every new YAML token from a ConfigEnum table; nothing under `src/vtbackend` including `src/contour`; tests for every behaviour. Fix every confirmed finding, rebuild, re-run ctest, and commit — this is the phase's final commit, so its body carries the ctest line from Step 1 (re-run after the fixes):

```bash
git add -u
git commit -F - <<'EOF'
semantic blocks: address the phase 5 review

ctest: N tests passed, M failed (only the phase-0 environmental failures).

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

with the real counts in place of `N` and `M`. If the review confirms no finding, make the Step 2 commit (or, if that one was empty too, an empty `git commit --allow-empty -F -` with the same subject `semantic blocks: phase 5 gate` and body) the carrier of the ctest line.

- [ ] **Step 4: Report** — report to the coordinating session: the ctest line, the spelling result, any target that could not be built, `git log --oneline PHASE5_START..HEAD`, and the open risks below. The coordinator records progress; this task does not.

## Open risks

- **A pager that exits quickly shows the early-exit notice.** A pane whose program ends within the session's early-exit threshold (5 s) shows "terminated too quickly" and waits for a key, as any command pane does today; `less` on a one-screen output with `-F` would hit it. Not changed here.
- **Ctrl+Shift+G with an existing config.** Delivered as a built-in fallback, so it reaches users whose `input_mapping` lists the defaults; a user who binds Ctrl+Shift+G keeps their binding.
- **Attach mode.** `canCreateSession()` is false there, so split/tab placement reports failure; `detached` works.
- **MockTerm clock.** The click counter reads phase 1's injected steady clock. If phase 1's `MockTerm` hands the terminal a clock that never advances, every press in a test is "within the interval"; the existing multi-click tests reset by position and pointer move, never by time, so they hold — a future test that relies on the interval must advance `mock.steadyClock`.
- **Output tails.** A command whose last line has no newline shares its row with the next prompt, which phase 1 stamps as the next block: that tail is not part of the output.
- **A claimed Ctrl+triple-click over a row in no block** does nothing (by design; see Decisions).
- **Folded blocks.** `SelectCommandBlock` selects grid rows, including rows hidden in a collapsed fold; copying such a selection copies the hidden rows too.
