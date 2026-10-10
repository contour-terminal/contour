# Phase 7 — Sticky command header and evicted-output placeholder

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the user reads a command's output with that command scrolled out of view, the command's input row is pinned over the top row with a status chip (`✓ 3s`, `✗ 2 1m 04s`, `running 12s`); once that command's own line has been evicted from the scrollback, the same overlay reads `⋯ <command> — earlier output evicted` instead — always at the very top of the scrollback, and anywhere else the mode would show a header; a press on it jumps to the command.

**Architecture:** One pure decision, `stickyHeaderFor(StickyHeaderInput)`, in `vtbackend/shell/StickyHeader.hpp`, beside pure chip/placeholder text builders. `Terminal::fillStickyHeader()` gathers the input in O(1) — the top row's `Line::blockId()`, the record's cached `headStableId`/`headIdGeneration` (C1), and a walk of at most `MaxPromptScanLines` rows of the prompt — copies the input row through a new `RenderBufferBuilder::renderDetachedLine()` (the row's real colours, no cursor/selection/highlight) and fills `RenderBuffer::stickyHeader` (C3), remembering the block for `Terminal::stickyHeaderBlock()`. `vtrasterizer::Renderer` draws it in an `execute()` step of its own after the page (a step draws all rectangles before any glyph), unshifted by smooth scrolling. `TerminalSession` consumes a press on row 0 and calls the new `Terminal::scrollToCommandBlockHead()`. Config: profile `sticky_header`, scheme `sticky_header.{background,separator}`.

**Tech Stack:** C++23, Catch2 (`vtbackend_test`, `vtrasterizer_test`, `contour_gui_test`), yaml-cpp, Qt 6 (session input only).

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §6.2 (sticky command header), §6.3 (evicted-output placeholder), §13.1 (per-frame cost), §13.3 (accessibility), §11.1 (keys). Global constraints: [README](README.md#global-constraints). This phase **produces** contract C3 and the C9 rows `sticky_header` (profile) and `sticky_header.{background,separator}` (scheme); it **consumes** C1 (store, records, `Terminal::commandBlockAt`, `outcomeOf`, `CommandBlockOutcome`, `TerminalClocks`), C2 (`ColorPalette::blockStatusColor`, `gutterTextColor`) and C6 (`sanitizeCommandLine`).

---

## Decisions this phase takes

Reviewers check the code against these; each is restated where it is implemented.

1. **Which row decides.** Screen row 0 of the main page — the row the header covers. Header iff primary screen
   AND that row's block is `Running`/`Finished` AND its head is above the viewport AND that row is past the
   block's input row (so a wrapped or multi-line prompt never shows a header that repeats what is on screen)
   AND the mode allows it (`scrolled`: only while `Viewport::scrolled()`; `always`: also live).
2. **Evicted placeholder (owner decision; spec §6.3 as amended).** When row 0's block is `Running`/`Finished`
   and its head cannot be placed (cached generation stale, or `Grid::lineOffsetOf()` nullopt below the stable
   floor), the placeholder takes the place of the copied input row, so the header never vanishes just because
   the block's prompt was evicted. Gated by `showEvicted`: off, such a block shows no header at all. Where it
   shows:
   - at the very top — row 0 is the oldest addressable row (`Grid::addressableTop()`, `TopRowAge::Oldest`) —
     always, independent of `mode`;
   - anywhere else exactly when the mode would show a header at this viewport position for a block whose head
     is above the viewport — the same gate the `Command` case uses (`scrolled`: only while
     `Viewport::scrolled()`; `always`: always; `never`: never). So `mode: never` still shows the placeholder,
     but only at the very top.

   The top row's `TopRowPart` is not consulted for an evicted head: with the head gone there is no input row
   to tell apart from output, and `stickyHeaderInput()` reports the top row as `Output`.
3. **No O(output) work.** The head comes from the record's cache; the input row from a walk *down from the
   head* bounded by `MaxPromptScanLines` (`PromptRegion.hpp:106`). Tested by a block whose head lies 3 ×
   `MaxFoldScanLines` rows above row 0 — beyond every scan budget in the tree.
4. **Colours.** The copied row keeps its SGR colours; a cell whose background resolves to the page default
   takes the header band instead (the same substitution the OSC 3008 tint makes in
   `RenderBufferBuilder.cpp:90`). Line renditions (DECDWL/DECDHL) are not copied. The chip uses
   `blockStatusColor(outcomeOf(record))` (C2); the placeholder text uses `gutterTextColor()` (C2).
   The chip is drawn in the full block-status colour rather than dimmed (spec §6.2 says 'dim'): the colour is
   what carries the outcome at a glance.
5. **Running duration** = injected wall clock minus `record.commandStartedAt` (the record keeps only a wall
   start, C1), clamped at zero; once finished the chip shows the steady `record.duration`. Read through the
   `TerminalClocks` the terminal was constructed with (C1) — never `system_clock::now()`.
6. **Z-order.** `RhiRenderer::execute()` records the rect pass before the text pass
   (`src/contour/display/RhiRenderer.cpp:709-712`), so a band issued in the same step as row 0's glyphs would
   sit *under* them. The renderer flushes (`execute`) and draws the header in a step of its own, opaque
   (alpha 255, whatever the window opacity), before the cursor. A block cursor on row 0 is therefore hidden
   under the header (it is drawn by cell inversion in the page step); a bar/underline cursor stays visible.
7. **Gutter.** The header spans the page columns only, not the gutter strip: the fold column beside row 0 keeps
   describing that row, which belongs to the block the header names; a press on the gutter keeps toggling
   the fold (the gutter is consulted first in `SessionInput.cpp:696`).
8. **Hit-testing.** `Terminal::stickyHeaderBlock()` returns what the *last frame* drew (cached by the fill),
   gated on the displayed page being primary. Every button's press on row 0 is consumed while a header shows
   (it covers a row the user cannot see); only Left jumps. Row 0 is measured without the smooth-scroll shift
   (the header is pinned). Exception (Task 8.6a): while the scrollbar is shown, a press inside its strip
   reaches the QML ScrollBar, not the header.
9. **Accessibility.** The header lives only in `RenderBuffer::stickyHeader`; the bridge reads the grid
   (`TerminalAccessible.cpp:337-345`, `screen.lineTextColumnAlignedAt`) and never `RenderBuffer::cells`, so it
   sees exactly what it sees today. Tested by asserting the header text is in no cell or line of the buffer.

## Contract additions (beyond C3)

Named here so later phases may rely on them; each task's **Interfaces** block repeats the exact signature.

- `vtbackend/shell/StickyHeader.hpp`: `StickyHeadPosition`, `TopRowPart`, `ViewportScroll`, `TopRowAge`,
  `StickyHeaderBlockFacts`, `StickyHeaderInput`, `StickyHeaderDecision`, `stickyHeaderFor()`,
  `stickyHeaderChip()`, `evictedPlaceholderText()`, `StickyHeaderChipInset`. (The chip's duration text is phase 4's
  `vtbackend::formatCommandDuration()`, Task 4.13a — this phase defines no formatter of its own.)
- `vtbackend/render/RenderBuffer.hpp`: `layoutRenderText()`, `renderTextWidth()`.
- `RenderBufferBuilder::renderDetachedLine()` (static).
- `ColorPalette::stickyHeaderBackgroundColor()`, `stickyHeaderSeparatorColor()`, `StickyHeaderColorSlots`.
- `Viewport::scrollLineToTop()`; `CommandBlockJumpError`; `Terminal::scrollToCommandBlockHead()` (phase 8 Task 8.5
  builds `Terminal::revealStableLineAtTop()` on `Viewport::scrollLineToTop()` and re-bases this jump on it, so the
  header press and the scrollbar tick share one reveal path).
- `contour::session::ConsumedByStickyHeader`, `TerminalSession::sendStickyHeaderPressEvent()` /
  `sendStickyHeaderReleaseEvent()`; `contour::config::StickyHeaderConfig::settings()`.

## Dependencies on earlier phases (stop and report if any is missing)

- C1: `CommandBlock.hpp` (`CommandBlockId`, `CommandBlockState`, `CommandBlockRecord`, `CommandBlockOutcome`,
  `outcomeOf`; `CommandBlockId` and `CommandBlockOutcome` are defined in `core/CommandBlockId.hpp` and
  `core/CommandBlockOutcome.hpp` and re-exported by it), `Terminal::commandBlocks()`, `Terminal::commandBlockAt()`, `Line::blockId()`, and **the
  guarantee that a reflow refreshes every record's `headStableId`/`headIdGeneration` before the next frame**
  (Review Focus #1 test in Task 7.6 relies on it). Phase 1 keeps the constructor's `TerminalClocks` in the
  `Terminal` member `_clocks`; if phase 1 named it otherwise, use that name in Task 7.6 Step 6 — the code only
  reads `<member>.wall.now()`.
- C2: `ColorPalette::blockStatusColor(CommandBlockOutcome)`, `ColorPalette::gutterTextColor()`.
- Phase 4, Task 4.13a: `[[nodiscard]] std::string vtbackend::formatCommandDuration(std::chrono::steady_clock::duration duration);`
  in `vtbackend/shell/CommandBlock.hpp` (`850ms`, `12s`, `3m 12s`, `1h 02m`; negative reads `0ms`).
- C6: `vtbackend/shell/CommandLineSanitizer.hpp` (`sanitizeCommandLine`, `SanitizePurpose::Display`).

## Commands used throughout

All paths relative to the worktree root `D:\contour\.claude\worktrees\semantic-blocks-1010`, run in Git Bash.

| Purpose | Command |
|---|---|
| Build vtbackend tests | `cmake --build --preset clangcl-debug --target vtbackend_test` |
| Run sticky-header vtbackend tests | `out/build/clangcl-debug/bin/vtbackend_test.exe "[stickyheader]"` |
| Build/run vtrasterizer tests | `cmake --build --preset clangcl-debug --target vtrasterizer_test` then `out/build/clangcl-debug/bin/vtrasterizer_test.exe "[stickyheader]"` |
| Build/run GUI tests | `cmake --build --preset clangcl-debug --target contour_gui_test` then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[stickyheader]"` (exit `0xC0000135`: prepend the Qt `bin` dir to `PATH`, see README) |

---

### Task 7.1: Sticky-header decision (pure)

**Files:**
- Create: `src/vtbackend/shell/StickyHeader.hpp`, `src/vtbackend/shell/StickyHeader.cpp`
- Create (test): `src/vtbackend/shell/StickyHeader_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (header list after `shell/PromptRegion.hpp`, source list after `shell/PromptRegion.cpp`, `vtbackend_test` list after `shell/PromptRegion_test.cpp`)

**Interfaces:**
- Consumes: `CommandBlockId`, `CommandBlockState` (C1, `vtbackend/shell/CommandBlock.hpp`); `ScreenType` (`core/Primitives.hpp:578`).
- Produces (C3): `enum class StickyHeaderMode : uint8_t { Never = 0, Scrolled, Always };`
  `struct StickyHeaderSettings { StickyHeaderMode mode = StickyHeaderMode::Scrolled; bool showEvicted = true; bool operator==(StickyHeaderSettings const&) const = default; };`
  `enum class StickyHeaderKind : uint8_t { Command = 0, Evicted };`
- Produces (addition): `enum class StickyHeadPosition : uint8_t { Visible = 0, Above, Evicted };`
  `enum class TopRowPart : uint8_t { Input = 0, Output };` `enum class ViewportScroll : uint8_t { Live = 0, Scrolled };`
  `enum class TopRowAge : uint8_t { Newer = 0, Oldest };`
  `struct StickyHeaderBlockFacts { CommandBlockId id; CommandBlockState state; StickyHeadPosition head; TopRowPart topRow; };`
  `struct StickyHeaderInput { StickyHeaderSettings settings; ScreenType screen; ViewportScroll viewport; TopRowAge topRowAge; std::optional<StickyHeaderBlockFacts> block; };`
  `struct StickyHeaderDecision { StickyHeaderKind kind; CommandBlockId block; };`
  `[[nodiscard]] std::optional<StickyHeaderDecision> stickyHeaderFor(StickyHeaderInput const& input) noexcept;`

- [ ] **Step 1: Record the phase start**

Run: `git rev-parse HEAD`
Expected: one SHA. Keep it as `<phase-7-start>` for the phase gate (Task 7.13).

- [ ] **Step 2: Write the failing test**

Create `src/vtbackend/shell/StickyHeader_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the sticky-header decision: whether the command header, the evicted-output placeholder
// or nothing covers the top row. Pure -- every input is a plain value -- so the whole input space is
// walked and checked against the spec restated once (specFor below), beside named rows that read as the
// spec's own sentences.

#include <vtbackend/shell/StickyHeader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <vector>

using namespace vtbackend;

namespace
{

constexpr auto Modes = std::array { StickyHeaderMode::Never, StickyHeaderMode::Scrolled, StickyHeaderMode::Always };
constexpr auto ShowEvicted = std::array { false, true };
constexpr auto Screens = std::array { ScreenType::Primary, ScreenType::Alternate };
constexpr auto Viewports = std::array { ViewportScroll::Live, ViewportScroll::Scrolled };
constexpr auto Ages = std::array { TopRowAge::Newer, TopRowAge::Oldest };
constexpr auto States =
    std::array { CommandBlockState::Prompting, CommandBlockState::Running, CommandBlockState::Finished };
constexpr auto Heads = std::array { StickyHeadPosition::Visible, StickyHeadPosition::Above, StickyHeadPosition::Evicted };
constexpr auto TopRows = std::array { TopRowPart::Input, TopRowPart::Output };

constexpr auto Block = CommandBlockId(7);

/// Every block the top row can belong to, and "none" first.
[[nodiscard]] std::vector<std::optional<StickyHeaderBlockFacts>> everyBlock()
{
    auto blocks = std::vector<std::optional<StickyHeaderBlockFacts>> { std::nullopt };
    for (auto const state: States)
        for (auto const head: Heads)
            for (auto const topRow: TopRows)
                blocks.emplace_back(StickyHeaderBlockFacts { .id = Block, .state = state, .head = head, .topRow = topRow });
    return blocks;
}

/// Every input the decision can be handed: one per combination of its fields.
[[nodiscard]] std::vector<StickyHeaderInput> everyInput()
{
    auto inputs = std::vector<StickyHeaderInput> {};
    auto const blocks = everyBlock();
    for (auto const mode: Modes)
        for (auto const showEvicted: ShowEvicted)
            for (auto const screen: Screens)
                for (auto const viewport: Viewports)
                    for (auto const age: Ages)
                        for (auto const& block: blocks)
                            inputs.push_back(StickyHeaderInput {
                                .settings = StickyHeaderSettings { .mode = mode, .showEvicted = showEvicted },
                                .screen = screen,
                                .viewport = viewport,
                                .topRowAge = age,
                                .block = block,
                            });
    return inputs;
}

/// The spec (§6.2, and §6.3 as the owner amended it), restated once, for every row of the input space to be
/// checked against.
[[nodiscard]] std::optional<StickyHeaderKind> specFor(StickyHeaderInput const& input)
{
    if (input.screen != ScreenType::Primary || !input.block || input.block->state == CommandBlockState::Prompting)
        return std::nullopt;

    auto const& block = *input.block;
    // Whether the mode shows a header at this viewport position for a block whose head is above it.
    auto const modeAllows = input.settings.mode == StickyHeaderMode::Always
                            || (input.settings.mode == StickyHeaderMode::Scrolled
                                && input.viewport == ViewportScroll::Scrolled);

    if (block.head == StickyHeadPosition::Above && block.topRow == TopRowPart::Output && modeAllows)
        return StickyHeaderKind::Command;
    // An evicted head: at the very top whatever the mode, anywhere else wherever the mode shows a header --
    // so the header never vanishes just because the prompt left the scrollback. Never with show_evicted off.
    if (block.head == StickyHeadPosition::Evicted && input.settings.showEvicted
        && (input.topRowAge == TopRowAge::Oldest || modeAllows))
        return StickyHeaderKind::Evicted;
    return std::nullopt;
}

/// The kind a decision names, or nullopt for none -- what the tables compare.
[[nodiscard]] std::optional<StickyHeaderKind> kindOf(std::optional<StickyHeaderDecision> const& decision)
{
    return decision ? std::optional { decision->kind } : std::nullopt;
}

/// A running block whose head is above the viewport and whose output the top row shows.
[[nodiscard]] StickyHeaderInput scrolledIntoOutput(StickyHeaderMode mode)
{
    return StickyHeaderInput {
        .settings = StickyHeaderSettings { .mode = mode, .showEvicted = true },
        .screen = ScreenType::Primary,
        .viewport = ViewportScroll::Scrolled,
        .topRowAge = TopRowAge::Newer,
        .block = StickyHeaderBlockFacts { .id = Block,
                                          .state = CommandBlockState::Running,
                                          .head = StickyHeadPosition::Above,
                                          .topRow = TopRowPart::Output },
    };
}

/// A block whose head left the scrollback, scrolled back into what is left of its output -- but not to the
/// very top: older rows are still there.
[[nodiscard]] StickyHeaderInput scrolledIntoAnEvictedBlock(StickyHeaderMode mode)
{
    auto input = scrolledIntoOutput(mode);
    input.block->head = StickyHeadPosition::Evicted;
    return input;
}

/// A block whose head left the scrollback, seen from the very top of what is left.
[[nodiscard]] StickyHeaderInput atTheTopOfAnEvictedBlock(StickyHeaderMode mode)
{
    auto input = scrolledIntoAnEvictedBlock(mode);
    input.topRowAge = TopRowAge::Oldest;
    return input;
}

} // namespace

TEST_CASE("StickyHeader.decision.matchesTheSpecForEveryInput", "[stickyheader]")
{
    auto const inputs = everyInput();
    REQUIRE(inputs.size() == 3 * 2 * 2 * 2 * 2 * (1 + (3 * 3 * 2)));

    for (auto const& input: inputs)
    {
        auto const decision = stickyHeaderFor(input);
        CHECK(kindOf(decision) == specFor(input));
        // Whatever it shows, it names the block it was handed -- the block a press on it jumps to.
        if (decision)
            CHECK(decision->block == Block);
    }
}

TEST_CASE("StickyHeader.decision.readsAsTheSpec", "[stickyheader]")
{
    SECTION("scrolled (the default): only once scrolled back into history")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Scrolled);
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Command);
        input.viewport = ViewportScroll::Live;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("always: while following live output too, so `make` stays above its own output")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.viewport = ViewportScroll::Live;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Command);
    }

    SECTION("never hides the command header, and the evicted placeholder everywhere but the very top")
    {
        CHECK_FALSE(stickyHeaderFor(scrolledIntoOutput(StickyHeaderMode::Never)).has_value());
        CHECK(kindOf(stickyHeaderFor(atTheTopOfAnEvictedBlock(StickyHeaderMode::Never))) == StickyHeaderKind::Evicted);
        CHECK_FALSE(stickyHeaderFor(scrolledIntoAnEvictedBlock(StickyHeaderMode::Never)).has_value());
    }

    SECTION("the alternate screen never shows either")
    {
        auto command = scrolledIntoOutput(StickyHeaderMode::Always);
        command.screen = ScreenType::Alternate;
        CHECK_FALSE(stickyHeaderFor(command).has_value());

        auto evicted = atTheTopOfAnEvictedBlock(StickyHeaderMode::Always);
        evicted.screen = ScreenType::Alternate;
        CHECK_FALSE(stickyHeaderFor(evicted).has_value());

        auto evictedBelowTheTop = scrolledIntoAnEvictedBlock(StickyHeaderMode::Always);
        evictedBelowTheTop.screen = ScreenType::Alternate;
        CHECK_FALSE(stickyHeaderFor(evictedBelowTheTop).has_value());
    }

    SECTION("a head on screen is never repeated -- a collapsed fold shows its head, so nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block->head = StickyHeadPosition::Visible;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("a top row that is still the command line shows nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block->topRow = TopRowPart::Input;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("a block still at its prompt shows nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block->state = CommandBlockState::Prompting;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }

    SECTION("a finished block shows its header like a running one")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Scrolled);
        input.block->state = CommandBlockState::Finished;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Command);
    }

    SECTION("the evicted placeholder: at the very top in every mode, elsewhere wherever the mode shows a header")
    {
        auto input = atTheTopOfAnEvictedBlock(StickyHeaderMode::Scrolled);
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);

        // Below the very top, scrolled back into history: the header does not vanish just because the
        // block's prompt was evicted -- in the modes that show a header there.
        input.topRowAge = TopRowAge::Newer;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);
        input.settings.mode = StickyHeaderMode::Always;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);
        input.settings.mode = StickyHeaderMode::Never;
        CHECK_FALSE(stickyHeaderFor(input).has_value());

        // Following live output: only `always` shows a header there, so only `always` shows the placeholder.
        input.viewport = ViewportScroll::Live;
        input.settings.mode = StickyHeaderMode::Scrolled;
        CHECK_FALSE(stickyHeaderFor(input).has_value());
        input.settings.mode = StickyHeaderMode::Always;
        CHECK(kindOf(stickyHeaderFor(input)) == StickyHeaderKind::Evicted);
    }

    SECTION("show_evicted off: a block whose head was evicted shows nothing, anywhere, in any mode")
    {
        for (auto const mode: Modes)
        {
            auto atTheTop = atTheTopOfAnEvictedBlock(mode);
            atTheTop.settings.showEvicted = false;
            CHECK_FALSE(stickyHeaderFor(atTheTop).has_value());

            auto belowTheTop = scrolledIntoAnEvictedBlock(mode);
            belowTheTop.settings.showEvicted = false;
            CHECK_FALSE(stickyHeaderFor(belowTheTop).has_value());
        }
    }

    SECTION("no block -- no shell integration, or a dropped record -- shows nothing")
    {
        auto input = scrolledIntoOutput(StickyHeaderMode::Always);
        input.block.reset();
        CHECK_FALSE(stickyHeaderFor(input).has_value());
    }
}
```

Register it in `src/vtbackend/CMakeLists.txt` (`vtbackend_test` sources), before → after:

```cmake
        shell/PromptRegion_test.cpp
```

```cmake
        shell/PromptRegion_test.cpp
        shell/StickyHeader_test.cpp
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `fatal error C1083: Cannot open include file: 'vtbackend/shell/StickyHeader.hpp'`.

- [ ] **Step 4: Write the header**

Create `src/vtbackend/shell/StickyHeader.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <cstdint>
#include <optional>

namespace vtbackend
{

/// When the sticky command header may cover the top row (profile key `sticky_header.mode`).
enum class StickyHeaderMode : uint8_t
{
    Never = 0, ///< No header -- except the evicted-output placeholder at the very top of the scrollback.
    Scrolled,  ///< Only while the viewport is scrolled back into history -- the default.
    Always,    ///< Also while following live output, so a long build keeps its command in view.
};

/// The sticky header's configuration, as the terminal consumes it (contract C3).
struct StickyHeaderSettings
{
    StickyHeaderMode mode = StickyHeaderMode::Scrolled; ///< When the command header may appear.

    /// Whether the evicted-output placeholder may stand in for a block whose head has left the scrollback.
    ///
    /// At the very top of the scrollback it shows whatever @ref mode says; anywhere else it shows wherever
    /// @ref mode would show the command header. Off, such a block shows no header at all.
    ///
    /// A plain bool because it mirrors one YAML schema field (`sticky_header.show_evicted`) one to one --
    /// the carve-out AGENT.md documents for configuration -- and stickyHeaderFor() is its only reader.
    bool showEvicted = true;

    bool operator==(StickyHeaderSettings const&) const = default;
};

/// What the sticky header shows.
enum class StickyHeaderKind : uint8_t
{
    Command = 0, ///< The block's input row, copied from the grid with its own colours.
    Evicted,     ///< `⋯ <command> — earlier output evicted`: the head has left the scrollback.
};

/// Where the head of the viewport's top-row block is, relative to the viewport.
enum class StickyHeadPosition : uint8_t
{
    Visible = 0, ///< On screen -- the top row IS the head -- so a header would only repeat it.
    Above,       ///< Above the viewport, and still in the scrollback.
    Evicted,     ///< Gone from the scrollback; the block's record outlives it.
};

/// Which part of its block the viewport's top row is.
enum class TopRowPart : uint8_t
{
    Input = 0, ///< The prompt, or the command typed at it: already on screen, so no header.
    Output,    ///< What the command printed.
};

/// Whether the viewport follows live output or has been scrolled back.
enum class ViewportScroll : uint8_t
{
    Live = 0, ///< At the bottom, following output.
    Scrolled, ///< Scrolled back into history.
};

/// Whether the viewport's top row is the oldest row the scrollback still holds.
enum class TopRowAge : uint8_t
{
    Newer = 0, ///< Older rows are still there.
    Oldest,    ///< Nothing older survives: the viewport is at the very top.
};

/// The block the viewport's top row belongs to, as much of it as the decision reads.
struct StickyHeaderBlockFacts
{
    CommandBlockId id {};                                   ///< The block.
    CommandBlockState state = CommandBlockState::Prompting; ///< Where it is in its cycle.
    StickyHeadPosition head = StickyHeadPosition::Visible;  ///< Where its head is.
    TopRowPart topRow = TopRowPart::Input;                  ///< What part of it the top row is.

    [[nodiscard]] bool operator==(StickyHeaderBlockFacts const&) const noexcept = default;
};

/// Everything stickyHeaderFor() reads, gathered by the terminal once per frame.
struct StickyHeaderInput
{
    StickyHeaderSettings settings {};               ///< The profile's configuration.
    ScreenType screen = ScreenType::Primary;        ///< The screen of the DISPLAYED page.
    ViewportScroll viewport = ViewportScroll::Live; ///< Following output, or scrolled back.
    TopRowAge topRowAge = TopRowAge::Newer;         ///< Whether the top row is the oldest one kept.

    /// The top row's block; nullopt when it belongs to none -- no shell integration, or a record the
    /// store's bound has already dropped.
    std::optional<StickyHeaderBlockFacts> block {};
};

/// What the sticky header shows on this frame.
struct StickyHeaderDecision
{
    StickyHeaderKind kind = StickyHeaderKind::Command; ///< Which overlay.
    CommandBlockId block {};                           ///< Whose -- what a press on it jumps to.

    [[nodiscard]] bool operator==(StickyHeaderDecision const&) const noexcept = default;
};

/// Decides whether the top row is covered by the sticky command header, the evicted-output placeholder,
/// or nothing (spec §6.2, §6.3).
///
/// Pure: the terminal gathers the input in O(1) per frame (@see Terminal::fillStickyHeader), so the
/// whole policy is this function and its table tests.
///
/// @param input What the viewport shows and what the profile allows.
/// @return The overlay to draw, or nullopt for none.
[[nodiscard]] std::optional<StickyHeaderDecision> stickyHeaderFor(StickyHeaderInput const& input) noexcept;

} // namespace vtbackend
```

- [ ] **Step 5: Write the implementation**

Create `src/vtbackend/shell/StickyHeader.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/StickyHeader.hpp>

#include <array>
#include <cstddef>

namespace vtbackend
{

namespace
{
    /// Whether @p mode shows a header, for a block whose head is above the viewport, while the viewport is
    /// @p viewport. The command header and the evicted placeholder below the very top both pass this gate.
    ///
    /// A table rather than a condition: one row per mode, one column per viewport position, so a fourth
    /// mode is a row here and nothing else.
    [[nodiscard]] constexpr bool modeShowsHeader(StickyHeaderMode mode, ViewportScroll viewport) noexcept
    {
        //                                              Live   Scrolled
        constexpr auto Table = std::array { std::array { false, false },  // Never
                                            std::array { false, true },   // Scrolled
                                            std::array { true, true } };  // Always
        return Table[static_cast<size_t>(mode)][static_cast<size_t>(viewport)];
    }
} // namespace

std::optional<StickyHeaderDecision> stickyHeaderFor(StickyHeaderInput const& input) noexcept
{
    // The alternate screen belongs to a full-screen application: it carries no blocks, and a header
    // there would sit over vim's first line.
    if (input.screen != ScreenType::Primary || !input.block)
        return std::nullopt;

    auto const& block = *input.block;

    // A block still at its prompt has printed nothing to scroll past.
    if (block.state == CommandBlockState::Prompting)
        return std::nullopt;

    switch (block.head)
    {
        case StickyHeadPosition::Visible: return std::nullopt;
        case StickyHeadPosition::Evicted:
            // The placeholder stands in for the input row the grid no longer holds, so the header does not
            // vanish just because the prompt was evicted (owner decision; spec §6.3 as amended): at the very
            // top of the scrollback whatever the mode, anywhere else wherever the mode shows a header. The
            // top row's part is not consulted -- with the head gone there is no input row to tell apart.
            if (input.settings.showEvicted
                && (input.topRowAge == TopRowAge::Oldest || modeShowsHeader(input.settings.mode, input.viewport)))
                return StickyHeaderDecision { .kind = StickyHeaderKind::Evicted, .block = block.id };
            return std::nullopt;
        case StickyHeadPosition::Above:
            // Over the command line itself the header would only repeat what is on screen.
            if (block.topRow == TopRowPart::Output && modeShowsHeader(input.settings.mode, input.viewport))
                return StickyHeaderDecision { .kind = StickyHeaderKind::Command, .block = block.id };
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace vtbackend
```

- [ ] **Step 6: Register the sources**

In `src/vtbackend/CMakeLists.txt`, before → after (header list, line 55):

```cmake
    shell/PromptRegion.hpp
```

```cmake
    shell/PromptRegion.hpp
    shell/StickyHeader.hpp
```

and (source list, line 105):

```cmake
    shell/PromptRegion.cpp
```

```cmake
    shell/PromptRegion.cpp
    shell/StickyHeader.cpp
```

- [ ] **Step 7: Format**

Run: `clang-format -i src/vtbackend/shell/StickyHeader.hpp src/vtbackend/shell/StickyHeader.cpp src/vtbackend/shell/StickyHeader_test.cpp`
Expected: no output.

- [ ] **Step 8: Run the test to verify it passes**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[stickyheader]"`
Expected: build with zero warnings; `All tests passed` (2 test cases).

- [ ] **Step 9: Commit**

```bash
git add src/vtbackend/shell/StickyHeader.hpp src/vtbackend/shell/StickyHeader.cpp \
        src/vtbackend/shell/StickyHeader_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: decide when the sticky command header shows

A pure stickyHeaderFor() over the top row's block, the viewport and the
profile's mode (spec 6.2, 6.3), checked against the spec restated for
every one of its 912 inputs.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.2: Chip and placeholder text (pure)

**Files:**
- Modify: `src/vtbackend/shell/StickyHeader.hpp`, `src/vtbackend/shell/StickyHeader.cpp`
- Test: `src/vtbackend/shell/StickyHeader_test.cpp`

**Interfaces:**
- Consumes: `CommandBlockRecord`, `CommandBlockEnd`, `CommandBlockOutcome`, `[[nodiscard]] CommandBlockOutcome outcomeOf(CommandBlockRecord const&) noexcept` (C1/C2);
  `enum class SanitizePurpose : uint8_t { Display = 0, Insert };` `[[nodiscard]] std::string sanitizeCommandLine(std::string_view text, SanitizePurpose purpose);` (C6);
  `[[nodiscard]] std::string formatCommandDuration(std::chrono::steady_clock::duration duration);` (phase 4, Task 4.13a, `shell/CommandBlock.hpp`).
- Produces (addition):
  `constexpr inline auto StickyHeaderChipInset = ColumnCount(2);`
  `[[nodiscard]] std::u32string stickyHeaderChip(CommandBlockRecord const& record, std::chrono::system_clock::time_point now);`
  `[[nodiscard]] std::u32string evictedPlaceholderText(std::string_view commandLine);`

- [ ] **Step 1: Write the failing tests**

Add to the includes of `src/vtbackend/shell/StickyHeader_test.cpp`:

```cpp
#include <chrono>
#include <string>
```

and append (the durations are `vtbackend::formatCommandDuration`'s, tested on their own in phase 4):

```cpp
TEST_CASE("StickyHeader.chip.namesTheOutcomeAndTheDuration", "[stickyheader]")
{
    using namespace std::chrono_literals;
    auto const start = std::chrono::system_clock::time_point { std::chrono::hours(1000) };

    auto record = CommandBlockRecord {};
    record.id = CommandBlockId(3);
    record.commandStartedAt = start;

    SECTION("running: how long so far")
    {
        record.state = CommandBlockState::Running;
        CHECK(stickyHeaderChip(record, start + 12s) == U"running 12s");
    }

    SECTION("running, with a wall clock stepped backwards: zero rather than a negative")
    {
        record.state = CommandBlockState::Running;
        CHECK(stickyHeaderChip(record, start - 5s) == U"running 0ms");
    }

    SECTION("running for less than a second: milliseconds")
    {
        record.state = CommandBlockState::Running;
        CHECK(stickyHeaderChip(record, start + 850ms) == U"running 850ms");
    }

    SECTION("success, with its duration")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 0;
        record.duration = 3s;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✓ 3s");
    }

    SECTION("failure, with its exit code and its duration")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 2;
        record.duration = 192s;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ 2 3m 12s");
    }

    SECTION("minutes keep two digits of seconds")
    {
        record.state = CommandBlockState::Finished;
        record.exitCode = 2;
        record.duration = 64s;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✗ 2 1m 04s");
    }

    SECTION("an implicit end -- no exit code, no duration -- names only the outcome")
    {
        record.state = CommandBlockState::Finished;
        record.end = CommandBlockEnd::Implicit;
        CHECK(stickyHeaderChip(record, start + 1h) == U"✓");
    }
}

TEST_CASE("StickyHeader.placeholder.namesTheCommandItLost", "[stickyheader]")
{
    CHECK(evictedPlaceholderText("make") == U"⋯ make — earlier output evicted");

    // A command the shell never reported and the grid could not give back.
    CHECK(evictedPlaceholderText("") == U"⋯ earlier output evicted");

    // A command line is untrusted (spec §10.4): a crafted one must not put a control sequence on the
    // screen through the placeholder.
    auto const hostile = evictedPlaceholderText("make\x1b[201~");
    CHECK(hostile.find(U'\x1b') == std::u32string::npos);
    CHECK(hostile.find(U'�') != std::u32string::npos);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C3861: 'stickyHeaderChip': identifier not found` (likewise `evictedPlaceholderText`).

- [ ] **Step 3: Declare the text builders**

In `src/vtbackend/shell/StickyHeader.hpp`, replace

```cpp
#include <cstdint>
#include <optional>
```

with

```cpp
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
```

and insert before the closing `} // namespace vtbackend`:

```cpp
/// Blank columns kept right of the sticky header's chip, so its last glyph ends left of the overlay
/// scrollbar's 12-logical-pixel strip (SessionChrome.qml) instead of under its thumb. Enough for any cell
/// at least 6 px wide.
constexpr inline auto StickyHeaderChipInset = ColumnCount(2);

/// The status chip the sticky header right-aligns: `✓ 3s`, `✗ 2 1m 04s`, `running 12s`.
///
/// A running block's time is @p now less its wall-clock start, the only start a record keeps (C1); once
/// the block finishes, the record's steady duration takes over. Either reads as every other surface
/// prints a duration (@see formatCommandDuration), so a wall clock stepped backwards reads `0ms`.
/// @param record The block's record.
/// @param now The injected wall clock's time (TerminalClocks::wall) -- never system_clock::now().
/// @return The chip text.
[[nodiscard]] std::u32string stickyHeaderChip(CommandBlockRecord const& record,
                                              std::chrono::system_clock::time_point now);

/// The evicted-output placeholder for a block whose command line is @p commandLine.
///
/// The command line is RAW and untrusted, so it is sanitised for display first (C6).
/// @param commandLine The record's command line as stored; may be empty.
/// @return `⋯ <command> — earlier output evicted`, or `⋯ earlier output evicted` without a command.
[[nodiscard]] std::u32string evictedPlaceholderText(std::string_view commandLine);
```

- [ ] **Step 4: Implement them**

In `src/vtbackend/shell/StickyHeader.cpp`, replace

```cpp
#include <vtbackend/shell/StickyHeader.hpp>

#include <array>
#include <cstddef>
```

with

```cpp
#include <vtbackend/shell/CommandLineSanitizer.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <libunicode/convert.h>

#include <array>
#include <cstddef>
#include <format>
```

and insert before the closing `} // namespace vtbackend`:

```cpp
std::u32string stickyHeaderChip(CommandBlockRecord const& record, std::chrono::system_clock::time_point now)
{
    using std::chrono::duration_cast;
    using std::chrono::steady_clock;

    auto chip = std::string {};
    switch (outcomeOf(record))
    {
        case CommandBlockOutcome::Running:
            chip = "running";
            if (record.commandStartedAt)
                chip += " "
                        + formatCommandDuration(
                            duration_cast<steady_clock::duration>(now - *record.commandStartedAt));
            break;
        case CommandBlockOutcome::Success: chip = "✓"; break;
        case CommandBlockOutcome::Failure:
            chip = "✗";
            if (record.exitCode)
                chip += std::format(" {}", *record.exitCode);
            break;
    }

    if (record.state == CommandBlockState::Finished && record.duration)
        chip += " " + formatCommandDuration(*record.duration);

    return unicode::convert_to<char32_t>(std::string_view { chip });
}

std::u32string evictedPlaceholderText(std::string_view commandLine)
{
    auto const command = sanitizeCommandLine(commandLine, SanitizePurpose::Display);
    auto const text = command.empty() ? std::string { "⋯ earlier output evicted" }
                                      : std::format("⋯ {} — earlier output evicted", command);
    return unicode::convert_to<char32_t>(std::string_view { text });
}
```

- [ ] **Step 5: Format**

Run: `clang-format -i src/vtbackend/shell/StickyHeader.hpp src/vtbackend/shell/StickyHeader.cpp src/vtbackend/shell/StickyHeader_test.cpp`
Expected: no output.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[stickyheader]"`
Expected: zero warnings; `All tests passed` (4 test cases).

- [ ] **Step 7: Commit**

```bash
git add src/vtbackend/shell/StickyHeader.hpp src/vtbackend/shell/StickyHeader.cpp src/vtbackend/shell/StickyHeader_test.cpp
git commit -F - <<'EOF'
vtbackend: build the sticky header's chip and evicted placeholder text

The chip names the outcome, the exit code and the duration (a running
block's taken from the injected wall clock), worded by the shared
formatCommandDuration(); the placeholder sanitises the untrusted command
line for display.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.3: `RenderStickyHeader` and text layout in the render buffer

**Files:**
- Modify: `src/vtbackend/render/RenderBuffer.hpp`, `src/vtbackend/render/RenderBuffer.cpp`
- Modify: `src/vtrasterizer/GutterCells.hpp` (phase 4's `annotationRenderCells()` delegates to `layoutRenderText()`, Step 4b)
- Create (test): `src/vtbackend/render/RenderBuffer_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (`vtbackend_test` list)

**Interfaces:**
- Consumes: `StickyHeaderKind` (Task 7.1), `CommandBlockId` (C1), `RenderCell`/`RenderAttributes` (`RenderBuffer.hpp:23-76`).
- Produces (C3): `struct RenderStickyHeader { StickyHeaderKind kind; CommandBlockId block; std::vector<RenderCell> cells; std::u32string chip; RenderAttributes chipAttributes; RGBColor background; RGBColor separator; };`
  and `RenderBuffer::stickyHeader` (`std::optional<RenderStickyHeader>`), reset by `RenderBuffer::clear()`.
- Produces (addition):
  `[[nodiscard]] std::vector<RenderCell> layoutRenderText(std::u32string_view text, RenderAttributes const& attributes, CellLocation origin, ColumnOffset end);`
  `[[nodiscard]] ColumnCount renderTextWidth(std::u32string_view text);`

> Coordinator ruling: phase 4 added `vtrasterizer::annotationRenderCells()` (`src/vtrasterizer/GutterCells.hpp`),
> which lays annotation text out one column per codepoint. `layoutRenderText()` below is the complete
> version (wide glyphs, combining marks, clipping), so it becomes the ONE text-to-cells routine: Step 4b
> makes `annotationRenderCells()` delegate to it. Do not keep two layouts.

- [ ] **Step 1: Write the failing test**

Create `src/vtbackend/render/RenderBuffer_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the render buffer's own helpers: laying out host text (the sticky header's chip and
// placeholder) as render cells, and clear() forgetting the sticky header with everything else.

#include <vtbackend/render/RenderBuffer.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

TEST_CASE("RenderBuffer.layoutRenderText.placesOneCellPerGlyph", "[renderbuffer][stickyheader]")
{
    auto const attributes = RenderAttributes { .foregroundColor = 0x112233_rgb,
                                               .backgroundColor = 0x445566_rgb,
                                               .decorationColor = 0x112233_rgb,
                                               .flags = {},
                                               .lineFlags = LineFlag::None };
    auto const cells = layoutRenderText(
        U"✓ 3s", attributes, CellLocation { .line = LineOffset(2), .column = ColumnOffset(10) }, ColumnOffset(20));

    REQUIRE(cells.size() == 4);
    CHECK(cells[0].codepoints == U"✓");
    CHECK(cells[0].position == CellLocation { .line = LineOffset(2), .column = ColumnOffset(10) });
    CHECK(cells[3].codepoints == U"s");
    CHECK(cells[3].position.column == ColumnOffset(13));
    CHECK(cells[0].attributes.foregroundColor == 0x112233_rgb);
    CHECK(cells[0].attributes.backgroundColor == 0x445566_rgb);
    // One shaping group, so the text renderer places it at its own pen position.
    CHECK(cells.front().groupStart);
    CHECK(cells.back().groupEnd);
    CHECK(renderTextWidth(U"✓ 3s") == ColumnCount(4));
}

TEST_CASE("RenderBuffer.layoutRenderText.wideGlyphsTakeTwoColumnsAndStopAtTheEnd", "[renderbuffer][stickyheader]")
{
    // 'a' takes column 0, '中' columns 1-2; 'b' would start at column 3, which is the end.
    auto const cells = layoutRenderText(U"a中b", RenderAttributes {}, CellLocation {}, ColumnOffset(3));
    REQUIRE(cells.size() == 2);
    CHECK(cells[1].codepoints == U"中");
    CHECK(cells[1].width == 2);
    CHECK(renderTextWidth(U"a中b") == ColumnCount(4));

    // A glyph that would straddle the end is dropped whole, never cut in half.
    CHECK(layoutRenderText(U"a中", RenderAttributes {}, CellLocation {}, ColumnOffset(2)).size() == 1);
}

TEST_CASE("RenderBuffer.layoutRenderText.aCombiningMarkJoinsItsBase", "[renderbuffer][stickyheader]")
{
    auto const cells = layoutRenderText(U"éx", RenderAttributes {}, CellLocation {}, ColumnOffset(10));
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].codepoints == U"é");
    CHECK(cells[1].position.column == ColumnOffset(1));
    CHECK(renderTextWidth(U"éx") == ColumnCount(2));
}

TEST_CASE("RenderBuffer.clear.forgetsTheStickyHeader", "[renderbuffer][stickyheader]")
{
    auto buffer = RenderBuffer {};
    buffer.stickyHeader = RenderStickyHeader { .kind = StickyHeaderKind::Command,
                                               .block = CommandBlockId(1),
                                               .cells = {},
                                               .chip = U"✓",
                                               .chipAttributes = {},
                                               .background = 0x202020_rgb,
                                               .separator = 0x404040_rgb };
    buffer.clear();
    CHECK_FALSE(buffer.stickyHeader.has_value());
}
```

Register it in `src/vtbackend/CMakeLists.txt`, before → after:

```cmake
        shell/StickyHeader_test.cpp
```

```cmake
        shell/StickyHeader_test.cpp
        render/RenderBuffer_test.cpp
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C3861: 'layoutRenderText': identifier not found` and `'RenderStickyHeader': undeclared identifier`.

- [ ] **Step 3: Add the type and the declarations**

In `src/vtbackend/render/RenderBuffer.hpp`, replace the include block lines 4-18

```cpp
#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/TextScale.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/grid/Line.hpp>

#include <gsl/pointers>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>
```

with

```cpp
#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/TextScale.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <gsl/pointers>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
```

Insert before `struct RenderCursor` (line 105):

```cpp
/// The sticky command header drawn over the main page's first row (@see Terminal::fillStickyHeader).
///
/// Beside @ref RenderBuffer::cells rather than in it, for the reason RenderGutterCell gives: everything
/// that walks the cells -- selection, hit-testing, the accessibility bridge -- describes the GRID, and
/// the header repeats a row that is still there, underneath it.
struct RenderStickyHeader
{
    StickyHeaderKind kind = StickyHeaderKind::Command; ///< The command's input row, or the placeholder.
    CommandBlockId block {};                           ///< The block it names; a press on it jumps there.
    std::vector<RenderCell> cells {};  ///< Its text, already on the row it covers, short of the chip.
    std::u32string chip {};            ///< Right-aligned status: `✓ 3s`, `✗ 2 1m 04s`, `running 12s`.
    RenderAttributes chipAttributes {}; ///< The chip's colours.
    RGBColor background {};             ///< The band's colour.
    RGBColor separator {};              ///< The one-pixel line along the band's bottom edge.
};

/// Lays @p text out as render cells from @p origin rightwards, for host-drawn text that has no grid row
/// of its own (the sticky header's chip and placeholder).
///
/// One cell per glyph, as wide as the glyph; a zero-width codepoint (a combining mark, a variation
/// selector) joins the cell before it. A glyph that would reach @p end is dropped whole, and so is
/// everything after it. The cells form one shaping group.
///
/// @param text The text to lay out.
/// @param attributes Every cell's colours and flags.
/// @param origin Where the first cell goes.
/// @param end The first column the text may not reach.
/// @return The cells, left to right; empty when nothing fits.
[[nodiscard]] std::vector<RenderCell> layoutRenderText(std::u32string_view text,
                                                       RenderAttributes const& attributes,
                                                       CellLocation origin,
                                                       ColumnOffset end);

/// How many columns @p text takes when laid out by layoutRenderText() with no end in sight.
/// @param text The text to measure.
/// @return Its width in columns.
[[nodiscard]] ColumnCount renderTextWidth(std::u32string_view text);

```

In `struct RenderBuffer` (lines 116-131), before → after:

```cpp
    std::optional<RenderCursor> cursor {};
```

```cpp
    std::optional<RenderCursor> cursor {};

    /// The sticky command header covering the main page's first row on this frame, if any.
    std::optional<RenderStickyHeader> stickyHeader {};
```

and

```cpp
        cursor.reset();
```

```cpp
        cursor.reset();
        stickyHeader.reset();
```

- [ ] **Step 4: Implement the layout**

In `src/vtbackend/render/RenderBuffer.cpp`, replace

```cpp
#include <vtbackend/render/RenderBuffer.hpp>

#include <format>
#include <mutex>
```

with

```cpp
#include <vtbackend/render/RenderBuffer.hpp>

#include <libunicode/width.h>

#include <algorithm>
#include <format>
#include <mutex>
```

and insert before `bool RenderDoubleBuffer::swapBuffers`:

```cpp
std::vector<RenderCell> layoutRenderText(std::u32string_view text,
                                         RenderAttributes const& attributes,
                                         CellLocation origin,
                                         ColumnOffset end)
{
    auto cells = std::vector<RenderCell> {};
    auto column = origin.column;
    for (auto const codepoint: text)
    {
        auto const width = static_cast<int>(unicode::width(codepoint));

        // A combining mark or a variation selector belongs to the glyph before it.
        if (width == 0 && !cells.empty())
        {
            cells.back().codepoints.push_back(codepoint);
            continue;
        }

        auto const columns = std::max(1, width);
        if (column + ColumnOffset(columns) > end)
            break;

        cells.push_back(RenderCell { .codepoints = std::u32string(1, codepoint),
                                     .image = {},
                                     .position = CellLocation { .line = origin.line, .column = column },
                                     .attributes = attributes,
                                     .width = static_cast<uint8_t>(columns),
                                     .sizing = {},
                                     .groupStart = false,
                                     .groupEnd = false });
        column += ColumnOffset(columns);
    }

    if (!cells.empty())
    {
        cells.front().groupStart = true;
        cells.back().groupEnd = true;
    }
    return cells;
}

ColumnCount renderTextWidth(std::u32string_view text)
{
    auto total = 0;
    for (auto const codepoint: text)
    {
        auto const width = static_cast<int>(unicode::width(codepoint));
        // Joins the glyph before it, as layoutRenderText() places it.
        if (width == 0 && total != 0)
            continue;
        total += std::max(1, width);
    }
    return ColumnCount(total);
}

```

- [ ] **Step 4b: Make phase 4's annotation layout delegate to it**

In `src/vtrasterizer/GutterCells.hpp` (phase 4, Task 4.2), replace the body of
`annotationRenderCells()` so annotations get the same wide-glyph and combining-mark handling as the
header:

```cpp
[[nodiscard]] inline std::vector<vtbackend::RenderCell> annotationRenderCells(
    vtbackend::RenderAnnotation const& annotation)
{
    // The terminal already clipped the annotation to the page when it placed it (@see
    // vtbackend::foldLabelAnnotation), so the end is simply where the text stops.
    auto const end =
        annotation.position.column + boxed_cast<vtbackend::ColumnOffset>(vtbackend::renderTextWidth(annotation.text));
    return vtbackend::layoutRenderText(annotation.text, annotation.attributes, annotation.position, end);
}
```

`<ranges>` may become unused in `GutterCells.hpp`; drop it if nothing else there needs it. Phase 4's
`GutterCells` tests in `vtrasterizer_test` must still pass unchanged: they use single-column text,
for which both layouts agree (one cell per codepoint, the first cell starting the group and the last
ending it).

The layout is width-aware now, so Task 4.2's two comments that still say otherwise change with it. In
`src/vtbackend/render/RenderBuffer.hpp`, replace `RenderAnnotation::text`'s line

```cpp
    std::u32string text;         ///< One column per codepoint.
```

with (a leading comment, since the new one would overrun the 110-column limit as a trailing one)

```cpp
    /// Laid out by layoutRenderText(): a wide glyph takes two columns, a combining mark joins its base.
    std::u32string text;
```

and in `src/vtrasterizer/GutterCells.hpp`, replace `annotationRenderCells()`'s first doc line

```cpp
/// The cells an annotation is drawn as: one per codepoint, rightwards from its position, shaped as one run.
```

with

```cpp
/// The cells an annotation is drawn as, laid out by vtbackend::layoutRenderText() from its position,
/// shaped as one run.
```

- [ ] **Step 5: Format**

Run: `clang-format -i src/vtbackend/render/RenderBuffer.hpp src/vtbackend/render/RenderBuffer.cpp src/vtbackend/render/RenderBuffer_test.cpp src/vtrasterizer/GutterCells.hpp`
Expected: no output.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[renderbuffer]"`
Expected: zero warnings; `All tests passed` (4 test cases). Then `out/build/clangcl-debug/bin/vtbackend_test.exe` — the whole binary still passes (the new include in `RenderBuffer.hpp` is reached by every renderer TU).
Then: `cmake --build --preset clangcl-debug --target vtrasterizer_test && out/build/clangcl-debug/bin/vtrasterizer_test.exe` — phase 4's `GutterCells` cases still pass through the delegated layout.

- [ ] **Step 7: Commit**

```bash
git add src/vtbackend/render/RenderBuffer.hpp src/vtbackend/render/RenderBuffer.cpp \
        src/vtbackend/render/RenderBuffer_test.cpp src/vtbackend/CMakeLists.txt src/vtrasterizer/GutterCells.hpp
git commit -F - <<'EOF'
vtbackend: carry the sticky header in the render buffer

RenderStickyHeader travels beside the cells, never among them, so
selection, hit-testing and the accessibility bridge keep describing the
grid. layoutRenderText() lays host text out as cells for its chip, and
the gutter's annotations now go through it too, so wide glyphs and
combining marks are placed the same way everywhere.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.4: Sticky-header colour slots in the palette

**Files:**
- Modify: `src/vtbackend/core/ColorPalette.hpp`
- Test: `src/vtbackend/core/ColorPalette_test.cpp`

**Interfaces:**
- Consumes: `mixColor` (`core/Color.hpp:95`), `defaultColorPaletteNames()` (`ColorPalette.hpp:415`).
- Produces (C3): `std::optional<RGBColor> stickyHeaderBackground, stickyHeaderSeparator;` with resolvers
  `[[nodiscard]] RGBColor stickyHeaderBackgroundColor() const noexcept;` `[[nodiscard]] RGBColor stickyHeaderSeparatorColor() const noexcept;`
- Produces (addition): `static constexpr float StickyHeaderBackgroundLift = 0.10f;` `static constexpr float StickyHeaderSeparatorLift = 0.35f;`
  `inline constexpr auto StickyHeaderColorSlots` — `std::array<std::pair<std::string_view, std::optional<RGBColor> ColorPalette::*>, 2>` rows `{"background", &ColorPalette::stickyHeaderBackground}`, `{"separator", &ColorPalette::stickyHeaderSeparator}`.

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/core/ColorPalette_test.cpp`:

```cpp
TEST_CASE("ColorPalette.stickyHeader.derivesWhenUnset", "[ColorPalette][stickyheader]")
{
    auto const palette = ColorPalette {};
    REQUIRE_FALSE(palette.stickyHeaderBackground.has_value());
    REQUIRE_FALSE(palette.stickyHeaderSeparator.has_value());

    // A band, not the page: lifted off the background toward the text colour, and an edge further still.
    CHECK(palette.stickyHeaderBackgroundColor() != palette.defaultBackground);
    CHECK(palette.stickyHeaderSeparatorColor() != palette.stickyHeaderBackgroundColor());

    // Transcribed as literals in ConfigDocumentation.hpp (StickyHeaderColorsConfig) and in
    // docs/configuration/colors.md; retuning a lift fails here and names the files to update.
    CHECK(palette.stickyHeaderBackgroundColor() == 0x2c2928_rgb);
    CHECK(palette.stickyHeaderSeparatorColor() == 0x595757_rgb);
}

TEST_CASE("ColorPalette.stickyHeader.explicitColorsBypassTheDerivation", "[ColorPalette][stickyheader]")
{
    auto palette = ColorPalette {};
    palette.stickyHeaderBackground = 0x010203_rgb;
    CHECK(palette.stickyHeaderBackgroundColor() == 0x010203_rgb);
    // The other stays derived: two independent slots, not one block.
    CHECK(palette.stickyHeaderSeparatorColor() == 0x595757_rgb);
}

TEST_CASE("ColorPalette.stickyHeader.isDistinctInTheBuiltinSchemes", "[ColorPalette][stickyheader]")
{
    for (auto const name: defaultColorPaletteNames())
    {
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        INFO("scheme: " << name);

        // The band must read as a band on the page, and the separator as an edge on the band.
        CHECK(distance(palette.stickyHeaderBackgroundColor(), palette.defaultBackground) > 6.0);
        CHECK(distance(palette.stickyHeaderSeparatorColor(), palette.stickyHeaderBackgroundColor()) > 15.0);
    }
}

TEST_CASE("ColorPalette.stickyHeader.theSlotTableNamesBothSlots", "[ColorPalette][stickyheader]")
{
    // The scheme reader and writer walk this table, so a slot missing from it would load from nowhere
    // and be written nowhere.
    REQUIRE(StickyHeaderColorSlots.size() == 2);
    CHECK(StickyHeaderColorSlots[0].first == "background");
    CHECK(StickyHeaderColorSlots[1].first == "separator");

    auto palette = ColorPalette {};
    palette.*StickyHeaderColorSlots[0].second = 0x010203_rgb;
    palette.*StickyHeaderColorSlots[1].second = 0x040506_rgb;
    CHECK(palette.stickyHeaderBackgroundColor() == 0x010203_rgb);
    CHECK(palette.stickyHeaderSeparatorColor() == 0x040506_rgb);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'stickyHeaderBackground': is not a member of 'vtbackend::ColorPalette'`.

- [ ] **Step 3: Add the slots and resolvers**

In `src/vtbackend/core/ColorPalette.hpp`, add `#include <utility>` after `#include <string_view>`. Then, before → after (line 353):

```cpp
    /// The page background under each kind of OSC 3008 context, when the scheme chooses to tint one.
```

```cpp
    /// The sticky command header's band, and the one-pixel line along its bottom edge.
    ///
    /// Optional, for the reason foldMarker is: no constant suits every scheme. Unset derives from the
    /// scheme's own background lifted toward its foreground -- far enough to read as a band over the page,
    /// not so far that it competes with the command copied into it.
    std::optional<RGBColor> stickyHeaderBackground;
    std::optional<RGBColor> stickyHeaderSeparator;

    /// How far the derived band is lifted from the background toward the foreground.
    static constexpr float StickyHeaderBackgroundLift = 0.10f;

    /// How far the derived separator is lifted -- enough to read as an edge on the band.
    static constexpr float StickyHeaderSeparatorLift = 0.35f;

    /// The band's colour, with the unset case resolved.
    [[nodiscard]] RGBColor stickyHeaderBackgroundColor() const noexcept
    {
        return stickyHeaderBackground.value_or(
            mixColor(defaultBackground, defaultForeground, StickyHeaderBackgroundLift));
    }

    /// The separator's colour, with the unset case resolved.
    [[nodiscard]] RGBColor stickyHeaderSeparatorColor() const noexcept
    {
        return stickyHeaderSeparator.value_or(
            mixColor(defaultBackground, defaultForeground, StickyHeaderSeparatorLift));
    }

    /// The page background under each kind of OSC 3008 context, when the scheme chooses to tint one.
```

and before → after (line 401):

```cpp
/// Applies the built-in color scheme @p colorPaletteName to @p palette, if there is one by that name.
```

```cpp
/// The colour scheme's `sticky_header:` map, one row per slot: its YAML key and the palette member.
///
/// The scheme reader and writer walk it, so a third slot is a row here and a member above.
inline constexpr auto StickyHeaderColorSlots = std::array {
    std::pair { std::string_view { "background" }, &ColorPalette::stickyHeaderBackground },
    std::pair { std::string_view { "separator" }, &ColorPalette::stickyHeaderSeparator },
};

/// Applies the built-in color scheme @p colorPaletteName to @p palette, if there is one by that name.
```

- [ ] **Step 4: Format**

Run: `clang-format -i src/vtbackend/core/ColorPalette.hpp src/vtbackend/core/ColorPalette_test.cpp`
Expected: no output.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[ColorPalette]"`
Expected: zero warnings; `All tests passed` (13 cases: the five fold-marker cases, Task 4.4's four block-status cases and the four new ones).

- [ ] **Step 6: Commit**

```bash
git add src/vtbackend/core/ColorPalette.hpp src/vtbackend/core/ColorPalette_test.cpp
git commit -F - <<'EOF'
vtbackend: add the sticky header's colour slots to the palette

Optional band and separator colours, derived from the scheme's own
background and foreground when unset, with one slot table for the scheme
reader and writer to walk.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.5: Copy a grid row as host chrome (`RenderBufferBuilder::renderDetachedLine`)

**Files:**
- Modify: `src/vtbackend/screen/RenderBufferBuilder.hpp`, `src/vtbackend/screen/RenderBufferBuilder.cpp`
- Create (test): `src/vtbackend/screen/RenderBufferBuilder_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (`vtbackend_test` list)

**Interfaces:**
- Consumes: the anonymous `makeColors(...)` (`RenderBufferBuilder.cpp:54-127`, whose `contextTint` stands in for the page background), the private static `makeRenderCell(...)` (`RenderBufferBuilder.cpp:285-329`), `ConstCellProxy` (`grid/CellProxy.hpp`), `Line::isBlank()/size()/storage()` (`grid/Line.hpp:245-543`).
- Produces (addition): `[[nodiscard]] static std::vector<RenderCell> RenderBufferBuilder::renderDetachedLine(Terminal const& terminal, Line const& line, LineOffset screenRow, ColumnCount columns, RGBColor pageBackground);`

Why a separate entry point rather than `Grid::render` over a one-row span: `renderCell()` maps every screen
position back to a grid line through the viewport (`RenderBufferBuilder.cpp:813`) to decide selection,
cursor and yank highlight — for a copied row that names the *top visible* line, not the row being copied,
and the trivial-line path would emit a `RenderLine` where the header needs cells.

- [ ] **Step 1: Write the failing test**

Create `src/vtbackend/screen/RenderBufferBuilder_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for RenderBufferBuilder::renderDetachedLine: a grid row copied as host chrome -- the row's
// own colours and SGR, none of the cursor, selection or highlight that belong to where it really is.

#include <vtbackend/screen/RenderBufferBuilder.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;

namespace
{
constexpr auto Band = RGBColor { 0x11, 0x22, 0x33 };
} // namespace

TEST_CASE("RenderBufferBuilder.detachedLine.keepsTheRowsOwnColours", "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("\033[31mab\033[44mcd\033[m ef");
    auto const& palette = mc.terminal.colorPalette();
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    auto const cells = RenderBufferBuilder::renderDetachedLine(mc.terminal, row, LineOffset(3), ColumnCount(5), Band);

    REQUIRE(cells.size() == 5);
    CHECK(cells[0].codepoints == U"a");
    // Placed on the row it is drawn over, not the one it was copied from.
    CHECK(cells[0].position == CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) });
    CHECK(cells[4].position == CellLocation { .line = LineOffset(3), .column = ColumnOffset(4) });

    // The application's colours survive the copy ...
    CHECK(cells[0].attributes.foregroundColor == palette.normalColor(1));
    CHECK(cells[2].attributes.backgroundColor == palette.normalColor(4));
    // ... and a cell left on the page background takes the band instead.
    CHECK(cells[0].attributes.backgroundColor == Band);

    CHECK(cells.front().groupStart);
    CHECK(cells.back().groupEnd);
}

TEST_CASE("RenderBufferBuilder.detachedLine.paintsNoCursor", "[renderbuffer][stickyheader]")
{
    // The text cursor stands right after "ab" on the copied row. Rendered in place, a block cursor
    // inverts that cell; copied, the cell is the row's own -- the cursor belongs to the row underneath.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("ab");
    REQUIRE(mc.terminal.currentScreen().cursor().position.column == ColumnOffset(2));
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    auto const cells = RenderBufferBuilder::renderDetachedLine(mc.terminal, row, LineOffset(0), ColumnCount(20), Band);

    REQUIRE(cells.size() == 20);
    CHECK(cells[2].attributes.backgroundColor == Band);
    CHECK(cells[2].attributes.foregroundColor == mc.terminal.colorPalette().defaultForeground);
}

TEST_CASE("RenderBufferBuilder.detachedLine.dropsAWideGlyphThatWouldStraddleTheLimit", "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mc.writeToScreen("a中b");
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(0));

    // '中' occupies columns 1-2; a limit of two columns cannot hold it, so the copy stops after 'a'.
    auto const cells = RenderBufferBuilder::renderDetachedLine(mc.terminal, row, LineOffset(0), ColumnCount(2), Band);
    REQUIRE(cells.size() == 1);
    CHECK(cells[0].codepoints == U"a");
}

TEST_CASE("RenderBufferBuilder.detachedLine.aBlankRowCopiesToNothing", "[renderbuffer][stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    auto const& row = mc.terminal.primaryScreen().grid().lineAt(LineOffset(1));
    REQUIRE(row.isBlank());
    CHECK(RenderBufferBuilder::renderDetachedLine(mc.terminal, row, LineOffset(0), ColumnCount(20), Band).empty());
}
```

Register it in `src/vtbackend/CMakeLists.txt`, before → after:

```cmake
        render/RenderBuffer_test.cpp
```

```cmake
        render/RenderBuffer_test.cpp
        screen/RenderBufferBuilder_test.cpp
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'renderDetachedLine': is not a member of 'vtbackend::RenderBufferBuilder'`.

- [ ] **Step 3: Declare it**

In `src/vtbackend/screen/RenderBufferBuilder.hpp`, before → after (line 41):

```cpp
    /// Renders a single grid cell.
```

```cpp
    /// Renders grid row @p line as host chrome copying it: its own colours, SGR and hyperlinks, but no
    /// cursor, selection, search match or yank highlight -- those describe the row where it really is.
    ///
    /// For the sticky command header (@see Terminal::fillStickyHeader). A cell whose background resolves
    /// to the page's default takes @p pageBackground instead, the substitution the OSC 3008 context tint
    /// makes. The row's rendition (DECDWL/DECDHL) is not copied, and an OSC 66 block shows only the part
    /// on this row.
    ///
    /// @param terminal The terminal whose palette, colour mode, blink phase and hyperlinks apply.
    /// @param line The grid row to copy.
    /// @param screenRow The screen row the copy is drawn on.
    /// @param columns How many columns to copy at most; a wide glyph that would straddle the limit is
    ///                dropped whole.
    /// @param pageBackground What stands in for the default background.
    /// @return One cell per copied column, left to right, as one shaping group; empty for a blank row.
    [[nodiscard]] static std::vector<RenderCell> renderDetachedLine(Terminal const& terminal,
                                                                    Line const& line,
                                                                    LineOffset screenRow,
                                                                    ColumnCount columns,
                                                                    RGBColor pageBackground);

    /// Renders a single grid cell.
```

- [ ] **Step 4: Implement it**

In `src/vtbackend/screen/RenderBufferBuilder.cpp`, before → after (line 9):

```cpp
#include <core/Utils.hpp>
```

```cpp
#include <core/Utils.hpp>

#include <ranges>
```

and insert before `std::optional<RGBColor> RenderBufferBuilder::tintFor(ContextId id) const noexcept` (line 162):

```cpp
std::vector<RenderCell> RenderBufferBuilder::renderDetachedLine(Terminal const& terminal,
                                                                Line const& line,
                                                                LineOffset screenRow,
                                                                ColumnCount columns,
                                                                RGBColor pageBackground)
{
    auto cells = std::vector<RenderCell> {};

    // A blank row has no materialised cells to read -- the reason Grid::render keeps one off the
    // per-cell path -- and nothing on it to show anyway.
    if (line.isBlank())
        return cells;

    auto const& palette = terminal.colorPalette();
    auto const reverseVideo = terminal.isModeEnabled(DECMode::ReverseVideo);
    auto const limit = std::min(unbox<size_t>(line.size()), unbox<size_t>(std::max(columns, ColumnCount(0))));
    auto const& storage = line.storage();
    cells.reserve(limit);

    for (auto const column: std::views::iota(size_t { 0 }, limit))
    {
        auto const cell = ConstCellProxy(storage, column);

        // Half a glyph is not a glyph: one that would straddle the limit is dropped whole.
        if (column + std::max<size_t>(1, cell.width()) > limit)
            break;

        // No selection, cursor, cursor line or highlight: those describe the row where it really is,
        // which is still on the page underneath.
        auto const [fg, bg] = makeColors(palette,
                                         palette.colorLookupTable,
                                         cell.flags(),
                                         reverseVideo,
                                         cell.foregroundColor(),
                                         cell.backgroundColor(),
                                         /*selected=*/false,
                                         /*isCursor=*/false,
                                         /*isCursorLine=*/false,
                                         /*isHighlighted=*/false,
                                         terminal.blinkState(),
                                         terminal.rapidBlinkState(),
                                         pageBackground);

        // The row's own line flags stay behind: the copy is drawn single width and single height.
        cells.emplace_back(makeRenderCell(palette,
                                          terminal.hyperlinks(),
                                          cell,
                                          LineFlags {},
                                          fg,
                                          bg,
                                          screenRow,
                                          ColumnOffset::cast_from(column)));
    }

    if (!cells.empty())
    {
        cells.front().groupStart = true;
        cells.back().groupEnd = true;
    }
    return cells;
}

```

- [ ] **Step 5: Format**

Run: `clang-format -i src/vtbackend/screen/RenderBufferBuilder.hpp src/vtbackend/screen/RenderBufferBuilder.cpp src/vtbackend/screen/RenderBufferBuilder_test.cpp`
Expected: no output.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[renderbuffer]"`
Expected: zero warnings; `All tests passed` (8 test cases: Task 7.3's four and these four).

- [ ] **Step 7: Commit**

```bash
git add src/vtbackend/screen/RenderBufferBuilder.hpp src/vtbackend/screen/RenderBufferBuilder.cpp \
        src/vtbackend/screen/RenderBufferBuilder_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: copy a grid row as host chrome

renderDetachedLine() renders one grid row with its own colours and SGR
but none of the cursor, selection or highlight of the row it came from,
substituting a given band for the page background.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.6: Terminal fills the command header

**Files:**
- Modify: `src/vtbackend/screen/Settings.hpp`, `src/vtbackend/screen/Terminal.hpp`, `src/vtbackend/screen/Terminal.cpp`
- Create (test): `src/vtbackend/screen/Terminal_stickyheader_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (`vtbackend_test` list)

**Interfaces:**
- Consumes: `stickyHeaderFor`, `stickyHeaderChip` (Tasks 7.1, 7.2); `RenderStickyHeader`, `renderTextWidth` (Task 7.3);
  `stickyHeaderBackgroundColor()`, `stickyHeaderSeparatorColor()` (Task 7.4); `renderDetachedLine` (Task 7.5);
  C1: `[[nodiscard]] CommandBlockStore& commandBlocks() noexcept;`, `CommandBlockRecord const* find(CommandBlockId) const noexcept`,
  `[[nodiscard]] CommandBlockRecord const* commandBlockAt(LineOffset gridLine) const noexcept;`, `[[nodiscard]] CommandBlockId Line::blockId() const noexcept;`,
  `outcomeOf`, the `TerminalClocks` member (`_clocks.wall.now()`);
  C2: `[[nodiscard]] RGBColor blockStatusColor(CommandBlockOutcome) const noexcept;`;
  existing: `Viewport::topLine()` (`Viewport.hpp:134`), `Viewport::scrolled()` (`:42`), `Grid::addressableTop()` (`Grid.hpp:913`),
  `Grid::lineOffsetOf()` (`:835`), `Grid::stableIdGeneration()` (`:825`), `Grid::isLineWrapped()` (`:682`), `screenTypeFromPage()` (`Primitives.hpp:585`),
  `MaxPromptScanLines` (`PromptRegion.hpp:106`), `fillRenderBufferInternal()` (`Terminal.cpp:598-705`).
- Produces (C3): `Settings::stickyHeader` (`StickyHeaderSettings stickyHeader {};`);
  `[[nodiscard]] std::optional<CommandBlockId> Terminal::stickyHeaderBlock() const noexcept;`
- Produces (private): `void fillStickyHeader(RenderBuffer& output, LineOffset baseLine);`
  `[[nodiscard]] StickyHeaderInput stickyHeaderInput() const;`
  `[[nodiscard]] std::optional<LineOffset> commandBlockHeadLine(CommandBlockRecord const& record) const noexcept;`
  `std::optional<CommandBlockId> _stickyHeaderBlock;`

This task draws `StickyHeaderKind::Command` only; Task 7.7 adds the evicted placeholder.

- [ ] **Step 1: Write the failing tests**

Create `src/vtbackend/screen/Terminal_stickyheader_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
//
// The sticky command header wired to a live terminal: what Terminal::fillRenderBuffer() puts into
// RenderBuffer::stickyHeader for real OSC 133 output, and what Terminal::stickyHeaderBlock() reports for
// a press on row 0. The decision itself is tested pure in shell/StickyHeader_test.cpp.

#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;

namespace
{

/// Starts @p command the way a shell with OSC 133 integration does -- prompt start, the prompt, prompt
/// end, the typed command, output start carrying the command line -- and leaves it running.
void startCommand(MockTerm<>& mc, std::string_view command)
{
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\");
    mc.writeToScreen(command);
    mc.writeToScreen("\r\n");
    mc.writeToScreen(std::format("\033]133;C;cmdline_url={}\033\\", command));
}

/// Prints @p count output lines `<prefix> 0`, `<prefix> 1`, ... in a single write.
void printLines(MockTerm<>& mc, std::string_view prefix, int count)
{
    auto text = std::string {};
    for (auto const i: std::views::iota(0, count))
        text += std::format("{} {}\r\n", prefix, i);
    mc.writeToScreen(text);
}

/// Finishes the running command with @p exitCode, as a shell's precmd hook does.
void finishCommand(MockTerm<>& mc, int exitCode)
{
    mc.writeToScreen(std::format("\033]133;D;{}\033\\", exitCode));
}

/// The sticky header the next frame carries, if any.
[[nodiscard]] std::optional<RenderStickyHeader> headerOf(MockTerm<>& mc)
{
    auto buffer = RenderBuffer {};
    mc.terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);
    return buffer.stickyHeader;
}

/// What @p cells spell, left to right.
[[nodiscard]] std::string textOf(std::vector<RenderCell> const& cells)
{
    auto text = std::u32string {};
    for (auto const& cell: cells)
        text += cell.codepoints;
    return unicode::convert_to<char>(std::u32string_view { text });
}

/// What screen row @p row of @p buffer spells, whichever of the two forms the row took.
[[nodiscard]] std::string rowText(RenderBuffer const& buffer, LineOffset row)
{
    for (auto const& line: buffer.lines)
        if (line.lineOffset == row)
            return unicode::convert_to<char>(std::u32string_view { line.text });
    auto text = std::u32string {};
    for (auto const& cell: buffer.cells)
        if (cell.position.line == row)
            text += cell.codepoints;
    return unicode::convert_to<char>(std::u32string_view { text });
}

/// The block running now.
[[nodiscard]] CommandBlockId runningBlock(MockTerm<>& mc)
{
    auto const* const record = mc.terminal.commandBlocks().current();
    REQUIRE(record != nullptr);
    return record->id;
}

} // namespace

TEST_CASE("StickyHeader.terminal.theModeDecidesWhenTheHeaderShows", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const id = runningBlock(mc);

    SECTION("never: neither live nor scrolled back")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        CHECK_FALSE(headerOf(mc).has_value());
        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
        CHECK_FALSE(headerOf(mc).has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }

    SECTION("scrolled (the default): only once scrolled back into the output")
    {
        REQUIRE(mc.terminal.settings().stickyHeader.mode == StickyHeaderMode::Scrolled);
        CHECK_FALSE(headerOf(mc).has_value());

        REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Command);
        CHECK(header->block == id);
        CHECK(textOf(header->cells) == "$ make");
        // What the frame drew is what a press on row 0 lands on.
        CHECK(mc.terminal.stickyHeaderBlock() == id);
    }

    SECTION("always: while following the live output too")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->block == id);
        CHECK(textOf(header->cells) == "$ make");
    }
}

TEST_CASE("StickyHeader.terminal.noShellIntegrationNoHeader", "[stickyheader]")
{
    // Review Focus #5: a shell without integration leaves no blocks, and the terminal behaves as today.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    printLines(mc, "plain", 20);

    CHECK_FALSE(headerOf(mc).has_value());
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(5)));
    CHECK_FALSE(headerOf(mc).has_value());
    REQUIRE(mc.terminal.viewport().scrollToTop());
    CHECK_FALSE(headerOf(mc).has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
}

TEST_CASE("StickyHeader.terminal.neverOnTheAlternateScreen", "[stickyheader][altscreen]")
{
    // Review Focus #2: a full-screen program owns its first row; leaving it brings the header back.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "vim");
    printLines(mc, "out", 20);
    REQUIRE(headerOf(mc).has_value());

    mc.writeToScreen("\033[?1049h");
    printLines(mc, "~", 3);
    CHECK_FALSE(headerOf(mc).has_value());
    CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());

    mc.writeToScreen("\033[?1049l");
    auto const header = headerOf(mc);
    REQUIRE(header.has_value());
    CHECK(textOf(header->cells) == "$ vim");
}

TEST_CASE("StickyHeader.terminal.aCollapsedBlockShowsItsHeadNotAHeader", "[stickyheader][folding]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "ls");
    printLines(mc, "out", 20);
    finishCommand(mc, 0);
    startCommand(mc, "make");

    auto const* ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = mc.terminal.primaryScreen().grid().lineOffsetOf(ls->headStableId);
    REQUIRE(head.has_value());

    // Expanded, and scrolled into its output: the header names it.
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(5)));
    auto const header = headerOf(mc);
    REQUIRE(header.has_value());
    CHECK(header->block == ls->id);

    // Collapsed, its output is drawn nowhere: the head itself stands on the top row, and a header would
    // only repeat it.
    REQUIRE(mc.terminal.toggleFoldContaining(*head));
    REQUIRE(mc.terminal.viewport().topLine() == *head);
    CHECK_FALSE(headerOf(mc).has_value());
}

TEST_CASE("StickyHeader.terminal.noHeaderOverTheCommandLineItself", "[stickyheader]")
{
    // A command that wraps over three rows and prints nothing. Scrolled so that its own second row is
    // on top, the head is above the viewport -- yet the top row is still the command line, and a header
    // would repeat what is on screen.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, std::string(45, 'x'));
    finishCommand(mc, 0);
    startCommand(mc, "ls");
    printLines(mc, "out", 10);

    auto const* wrapped = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(wrapped != nullptr);
    auto const head = mc.terminal.primaryScreen().grid().lineOffsetOf(wrapped->headStableId);
    REQUIRE(head.has_value());

    auto const target = *head + LineOffset(1);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount::cast_from(unbox(mc.terminal.viewport().topLine() - target))));
    REQUIRE(mc.terminal.viewport().topLine() == target);
    CHECK_FALSE(headerOf(mc).has_value());
}

TEST_CASE("StickyHeader.terminal.theChipNamesTheOutcome", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const& palette = mc.terminal.colorPalette();

    SECTION("running")
    {
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->chip.starts_with(U"running "));
        CHECK(header->chipAttributes.foregroundColor == palette.blockStatusColor(CommandBlockOutcome::Running));
    }

    SECTION("failed, with its exit code")
    {
        finishCommand(mc, 2);
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->chip.starts_with(U"✗ 2"));
        CHECK(header->chipAttributes.foregroundColor == palette.blockStatusColor(CommandBlockOutcome::Failure));
    }
}

TEST_CASE("StickyHeader.terminal.theHeaderKeepsTheRowsColoursOnItsOwnBand", "[stickyheader]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    mc.writeToScreen("\033]133;A\033\\\033[32m$\033[m \033]133;B\033\\make\r\n\033]133;C\033\\");
    printLines(mc, "out", 20);

    auto const& palette = mc.terminal.colorPalette();
    auto const header = headerOf(mc);
    REQUIRE(header.has_value());
    REQUIRE(header->cells.size() >= 6);

    // The prompt's own SGR survives the copy ...
    CHECK(header->cells[0].attributes.foregroundColor == palette.normalColor(2));
    // ... and a cell the shell left on the page background sits on the header's band.
    CHECK(header->cells[2].attributes.foregroundColor == palette.defaultForeground);
    CHECK(header->cells[2].attributes.backgroundColor == palette.stickyHeaderBackgroundColor());

    CHECK(header->background == palette.stickyHeaderBackgroundColor());
    CHECK(header->separator == palette.stickyHeaderSeparatorColor());
    CHECK(header->chipAttributes.backgroundColor == palette.stickyHeaderBackgroundColor());

    // Text and chip never touch: the text stops a blank column short of where the chip begins, and the chip
    // stops StickyHeaderChipInset columns short of the page's end (clear of the overlay scrollbar).
    auto const textEnd = header->cells.back().position.column + ColumnOffset(1);
    CHECK(textEnd + ColumnOffset(1) + boxed_cast<ColumnOffset>(renderTextWidth(header->chip))
              + boxed_cast<ColumnOffset>(StickyHeaderChipInset)
          <= ColumnOffset(30));
}

TEST_CASE("StickyHeader.terminal.theHeaderStaysOutOfTheCells", "[stickyheader][a11y]")
{
    // Selection, hit-testing and the accessibility bridge describe the GRID (the bridge reads
    // Screen::lineTextColumnAlignedAt, TerminalAccessible.cpp). The header repeats a row that is still
    // there underneath, so it travels beside the cells: the covered row keeps its own entry, and no cell
    // or line spells the copied command.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(3)));

    auto buffer = RenderBuffer {};
    mc.terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);
    REQUIRE(buffer.stickyHeader.has_value());

    CHECK(rowText(buffer, LineOffset(0)).starts_with("out "));
    for (auto const row: std::views::iota(0, 5))
        CHECK_FALSE(rowText(buffer, LineOffset(row)).starts_with("$ make"));
}

TEST_CASE("StickyHeader.terminal.aColumnResizeMidOutputKeepsTheHeaderOnItsCommand", "[stickyheader][resize]")
{
    // Review Focus #1. Narrowing reflows every output line into two rows, which destroys stable row
    // identity (a new Grid::stableIdGeneration). The reflow refreshes the record's cached head (C1), so
    // the header still finds its command -- with no scan of the output to do it. A failure here with a
    // stale headIdGeneration is a phase 1 defect: report it, do not add a scan here.
    auto mc = MockTerm { PageSize { LineCount(6), ColumnCount(40) }, LineCount(200) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    startCommand(mc, "make");
    printLines(mc, "compiling translation unit", 30);
    auto const id = runningBlock(mc);
    auto const generation = mc.terminal.primaryScreen().grid().stableIdGeneration();

    mc.terminal.resizeScreen(PageSize { LineCount(6), ColumnCount(24) });
    REQUIRE(mc.terminal.primaryScreen().grid().stableIdGeneration() != generation);

    auto header = headerOf(mc);
    REQUIRE(header.has_value());
    CHECK(header->kind == StickyHeaderKind::Command);
    CHECK(header->block == id);
    CHECK(textOf(header->cells) == "$ make");

    // The command goes on printing at the new width, and the header goes on naming it ...
    printLines(mc, "linking unit", 10);
    header = headerOf(mc);
    REQUIRE(header.has_value());
    CHECK(header->block == id);
    CHECK(textOf(header->cells) == "$ make");

    // ... including scrolled back into the reflowed output.
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Scrolled;
    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(7)));
    header = headerOf(mc);
    REQUIRE(header.has_value());
    CHECK(header->block == id);
}

TEST_CASE("StickyHeader.terminal.findsAHeadFurtherAwayThanAnyScanBudget", "[stickyheader]")
{
    // A build log runs to a million lines. The head is found through the record's cached position, never
    // by walking up from the top row -- so a head this far above it is found, where a walk bounded like
    // the fold scan (MaxFoldScanLines) or the prompt scan (MaxPromptScanLines) would stop long before it.
    auto constexpr OutputLines = static_cast<int>(MaxFoldScanLines) * 3;
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(OutputLines + 100) };
    startCommand(mc, "make");
    printLines(mc, "out", OutputLines);

    REQUIRE(mc.terminal.viewport().scrollUp(LineCount(2)));
    auto const header = headerOf(mc);
    REQUIRE(header.has_value());
    CHECK(textOf(header->cells) == "$ make");
}
```

Register it in `src/vtbackend/CMakeLists.txt`, before → after:

```cmake
        screen/RenderBufferBuilder_test.cpp
```

```cmake
        screen/RenderBufferBuilder_test.cpp
        screen/Terminal_stickyheader_test.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'stickyHeader': is not a member of 'vtbackend::Settings'` and `'stickyHeaderBlock': is not a member of 'vtbackend::Terminal'`.

- [ ] **Step 3: Add the setting**

In `src/vtbackend/screen/Settings.hpp`, before → after (line 8):

```cpp
#include <vtbackend/shell/MarkArbiter.hpp>
```

```cpp
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
```

and (line 157):

```cpp
    /// OSC 3008 hierarchical context signalling.
```

```cpp
    /// The sticky command header and the evicted-output placeholder (profile key `sticky_header`).
    ///
    /// Presentation, so a profile switch or a config reload re-applies it
    /// (TerminalSession::configureTerminal) -- the existing live-reload path every other profile-derived
    /// field here takes.
    StickyHeaderSettings stickyHeader {};

    /// OSC 3008 hierarchical context signalling.
```

- [ ] **Step 4: Declare the terminal API**

In `src/vtbackend/screen/Terminal.hpp`, before → after (line 24):

```cpp
#include <vtbackend/shell/SemanticBlockTracker.hpp>
```

```cpp
#include <vtbackend/shell/SemanticBlockTracker.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
```

Public, before → after (line 1209):

```cpp
    /// Scrolls the viewport and extends the active selection to the boundary cell.
```

```cpp
    // {{{ Sticky command header (OSC 133 blocks)

    /// The block the sticky header covered row 0 with on the last frame, if it showed one -- what a press
    /// on row 0 lands on.
    ///
    /// Recorded by the render pass rather than decided again here: the user clicks what was DRAWN, and a
    /// fresh decision after the viewport moved under the pointer would answer for a frame nobody saw.
    /// Nullopt whenever the displayed page is not the primary one, so a press on a full-screen
    /// application's first row is never taken for one on a header the previous page showed.
    [[nodiscard]] std::optional<CommandBlockId> stickyHeaderBlock() const noexcept
    {
        return foldingAppliesToDisplayedPage() ? _stickyHeaderBlock : std::nullopt;
    }

    // }}}

    /// Scrolls the viewport and extends the active selection to the boundary cell.
```

Private, before → after (line 3016):

```cpp
    DesktopNotificationManager _desktopNotificationManager;
```

```cpp
    // {{{ Sticky command header

    /// Builds this frame's sticky command header into @p output when stickyHeaderFor() decides on one,
    /// and records which block it showed (@see stickyHeaderBlock).
    ///
    /// O(1) in the length of the block's output (spec §13.1): the head comes from the record's cached
    /// position, the input row from a walk of at most MaxPromptScanLines rows down from the head, and the
    /// copy is one row.
    ///
    /// @param output The buffer being built.
    /// @param baseLine The screen row the main page starts at (@see fillGutter).
    void fillStickyHeader(RenderBuffer& output, LineOffset baseLine);

    /// What stickyHeaderFor() reads about this frame, gathered without scanning the output.
    /// @return The decision's input; its block is nullopt when row 0 belongs to no known block.
    [[nodiscard]] StickyHeaderInput stickyHeaderInput() const;

    /// The grid line @p record's head is on, read from the record's cached position.
    ///
    /// Nullopt once the head has left the scrollback: Grid::lineOffsetOf() answers nullopt below the
    /// stable floor, and a cached generation the reflow could not refresh names no row at all.
    /// @param record A record of the primary screen's store.
    /// @return The head's grid line, or nullopt when it cannot be placed.
    [[nodiscard]] std::optional<LineOffset> commandBlockHeadLine(CommandBlockRecord const& record) const noexcept;

    /// The block the last frame's sticky header named (@see stickyHeaderBlock).
    std::optional<CommandBlockId> _stickyHeaderBlock;
    // }}}

    DesktopNotificationManager _desktopNotificationManager;
```

- [ ] **Step 5: Gather the decision's input**

In `src/vtbackend/screen/Terminal.cpp`, insert before `void Terminal::autoCollapseOnNewPrompt()` (line 2747):

```cpp
namespace
{
    /// The grid row holding what the user typed in the block whose head is @p head.
    ///
    /// The first logical line of the block's prompt carrying LineFlag::PromptEnd -- where OSC 133;B handed
    /// the line to the user -- else the head itself, for a shell that never says where its prompt ends.
    /// Walks DOWN from the head and stops at the first sign of output, so it costs the prompt's height and
    /// never the output's length.
    ///
    /// @param grid The primary screen's grid.
    /// @param head The block's head row.
    /// @param id The block, so the walk stops where the next one begins.
    /// @return The input row.
    [[nodiscard]] LineOffset commandInputLine(Grid const& grid, LineOffset head, CommandBlockId id) noexcept
    {
        auto const pageEnd = boxed_cast<LineOffset>(grid.pageSize().lines);
        for (auto const step: std::views::iota(0, static_cast<int>(MaxPromptScanLines)))
        {
            auto const row = head + LineOffset(step);
            if (row >= pageEnd)
                break;

            auto const& line = grid.lineAt(row);
            if (line.blockId() != id)
                break;

            // A continuation carries no marks of its own: they live on its logical line's head.
            if (line.wrapped())
                continue;

            if (line.flags().test(LineFlag::PromptEnd))
                return row;

            if (step > 0 && line.flags().test(LineFlag::OutputStart))
                break;
        }
        return head;
    }

    /// The last physical row of the logical line starting at @p line, looking at most
    /// MaxPromptScanLines rows down.
    [[nodiscard]] LineOffset logicalLineEnd(Grid const& grid, LineOffset line) noexcept
    {
        for (auto const step: std::views::iota(1, static_cast<int>(MaxPromptScanLines) + 1))
            if (!grid.isLineWrapped(line + LineOffset(step)))
                return line + LineOffset(step - 1);
        return line + LineOffset(static_cast<int>(MaxPromptScanLines));
    }
} // namespace

std::optional<LineOffset> Terminal::commandBlockHeadLine(CommandBlockRecord const& record) const noexcept
{
    auto const& grid = primaryScreen().grid();
    if (record.headIdGeneration != grid.stableIdGeneration())
        return std::nullopt;
    return grid.lineOffsetOf(record.headStableId);
}

StickyHeaderInput Terminal::stickyHeaderInput() const
{
    auto input = StickyHeaderInput {
        .settings = _settings.stickyHeader,
        // The DISPLAYED page, as folding and the gutter use: with DECPCCM the cursor's page may differ.
        .screen = screenTypeFromPage(_displayedPage),
        .viewport = _viewport.scrolled() ? ViewportScroll::Scrolled : ViewportScroll::Live,
        .topRowAge = TopRowAge::Newer,
        .block = std::nullopt,
    };

    // Nothing below describes a page that carries no blocks, and the viewport translation would speak
    // for the wrong grid there.
    if (input.screen != ScreenType::Primary)
        return input;

    auto const& grid = primaryScreen().grid();
    auto const topLine = _viewport.topLine();
    input.topRowAge = topLine <= grid.addressableTop() ? TopRowAge::Oldest : TopRowAge::Newer;

    // O(1): the row's own block id, resolved through the store.
    auto const* const record = commandBlockAt(topLine);
    if (record == nullptr)
        return input;

    // A head the grid cannot place has been evicted -- the top row carries this block's id, so its rows
    // are still here and only the head is gone.
    auto facts = StickyHeaderBlockFacts {
        .id = record->id,
        .state = record->state,
        .head = StickyHeadPosition::Evicted,
        .topRow = TopRowPart::Output,
    };
    if (auto const head = commandBlockHeadLine(*record))
    {
        facts.head = *head < topLine ? StickyHeadPosition::Above : StickyHeadPosition::Visible;
        auto const inputEnd = logicalLineEnd(grid, commandInputLine(grid, *head, record->id));
        facts.topRow = topLine > inputEnd ? TopRowPart::Output : TopRowPart::Input;
    }
    input.block = facts;
    return input;
}

```

- [ ] **Step 6: Build the header**

Directly after the code of Step 5 (still before `void Terminal::autoCollapseOnNewPrompt()`), insert:

```cpp
void Terminal::fillStickyHeader(RenderBuffer& output, LineOffset baseLine)
{
    _stickyHeaderBlock.reset();

    // A profile that wants neither overlay pays one comparison per frame and nothing else.
    if (_settings.stickyHeader.mode == StickyHeaderMode::Never && !_settings.stickyHeader.showEvicted)
        return;

    auto const decision = stickyHeaderFor(stickyHeaderInput());
    if (!decision)
        return;

    auto const* const record = commandBlocks().find(decision->block);
    if (record == nullptr)
        return;

    auto const background = _colorPalette.stickyHeaderBackgroundColor();
    auto const statusColor = _colorPalette.blockStatusColor(outcomeOf(*record));
    // The injected wall clock (C1's TerminalClocks), so tests drive the running time.
    auto chip = stickyHeaderChip(*record, _clocks.wall.now());

    // One blank column between the text and the chip, so a long command never runs into it; the chip itself
    // ends StickyHeaderChipInset columns short of the page's end (the renderer's placement, Task 7.9).
    auto const textColumns = std::max(ColumnCount(0),
                                      pageSize().columns - renderTextWidth(chip) - ColumnCount(1)
                                          - StickyHeaderChipInset);

    auto cells = std::vector<RenderCell> {};
    switch (decision->kind)
    {
        case StickyHeaderKind::Command: {
            auto const head = commandBlockHeadLine(*record);
            if (!head)
                return;
            auto const& grid = primaryScreen().grid();
            cells = RenderBufferBuilder::renderDetachedLine(
                *this, grid.lineAt(commandInputLine(grid, *head, record->id)), baseLine, textColumns, background);
            break;
        }
        case StickyHeaderKind::Evicted:
            // No row of the block's prompt survives to be copied.
            return;
    }

    _stickyHeaderBlock = decision->block;
    output.stickyHeader = RenderStickyHeader {
        .kind = decision->kind,
        .block = decision->block,
        .cells = std::move(cells),
        .chip = std::move(chip),
        .chipAttributes = RenderAttributes { .foregroundColor = statusColor,
                                             .backgroundColor = background,
                                             .decorationColor = statusColor,
                                             .flags = {},
                                             .lineFlags = LineFlag::None },
        .background = background,
        .separator = _colorPalette.stickyHeaderSeparatorColor(),
    };
}

```

If phase 1 named the `TerminalClocks` member other than `_clocks`, use its name in `_clocks.wall.now()`.

- [ ] **Step 7: Call it from the render pass**

In `src/vtbackend/screen/Terminal.cpp` (`fillRenderBufferInternal`, line 689), before → after:

```cpp
    // Save the baseLine used for the main screen before the bottom status line shifts it.
    auto const mainScreenLine = baseLine;
```

```cpp
    // After the gutter, with the same baseLine: the header covers the main page's first row, wherever a
    // top status line put it.
    fillStickyHeader(output, baseLine);

    // Save the baseLine used for the main screen before the bottom status line shifts it.
    auto const mainScreenLine = baseLine;
```

- [ ] **Step 8: Verify the accessibility bridge never meets the header**

Run: `grep -n "renderBuffer\|RenderBuffer\|stickyHeader" src/contour/display/TerminalAccessible.cpp`
Expected: no output — the bridge builds its text from `screen.lineTextColumnAlignedAt` (`TerminalAccessible.cpp:337-345`), never from the render buffer, so the header (only in `RenderBuffer::stickyHeader`, asserted absent from `cells`/`lines` by `StickyHeader.terminal.theHeaderStaysOutOfTheCells`) is invisible to it, as spec §6.2/§13.3 require.

- [ ] **Step 9: Format**

Run: `clang-format -i src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_stickyheader_test.cpp`
Expected: no output.

- [ ] **Step 10: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[stickyheader]"`
Expected: zero warnings; `All tests passed`. Then run the whole binary, `out/build/clangcl-debug/bin/vtbackend_test.exe`: every test passes (the fill runs in every existing render-buffer test).

- [ ] **Step 11: Commit**

```bash
git add src/vtbackend/screen/Settings.hpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp \
        src/vtbackend/screen/Terminal_stickyheader_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: fill the sticky command header from the render pass

The top row's block, the record's cached head and a prompt-height walk
decide the header in O(1) per frame; its input row is copied with its
own colours, and the block it names is kept for a press on row 0.
Covers the column-resize, alternate-screen and no-integration cases.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.7: Evicted-output placeholder

**Files:**
- Modify: `src/vtbackend/screen/Terminal.cpp` (the `StickyHeaderKind::Evicted` case of `fillStickyHeader`, Task 7.6 Step 6)
- Test: `src/vtbackend/screen/Terminal_stickyheader_test.cpp`

**Interfaces:**
- Consumes: `evictedPlaceholderText` (Task 7.2), `layoutRenderText` (Task 7.3); C2 `[[nodiscard]] RGBColor gutterTextColor() const noexcept;`
- Produces: `RenderStickyHeader { .kind = StickyHeaderKind::Evicted }` in `RenderBuffer::stickyHeader` (C3).

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/screen/Terminal_stickyheader_test.cpp`:

```cpp
TEST_CASE("StickyHeader.terminal.anEvictedHeadLeavesThePlaceholderAtTheTop", "[stickyheader][evicted]")
{
    // A ten-line scrollback: forty lines of output push the head, and the first output lines, out of it.
    // The record outlives them, so the very top of what is left can still say which command it was.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    startCommand(mc, "make");
    printLines(mc, "out", 40);
    auto const id = runningBlock(mc);
    REQUIRE(mc.terminal.viewport().scrollToTop());

    SECTION("at the very top: the placeholder, with the running chip")
    {
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
        CHECK(header->block == id);
        CHECK(textOf(header->cells) == "⋯ make — earlier output evicted");
        CHECK(header->cells[0].attributes.foregroundColor == mc.terminal.colorPalette().gutterTextColor());
        CHECK(header->chip.starts_with(U"running "));
        CHECK(mc.terminal.stickyHeaderBlock() == id);
    }

    SECTION("show_evicted off: nothing")
    {
        mc.terminal.settings().stickyHeader.showEvicted = false;
        CHECK_FALSE(headerOf(mc).has_value());
    }

    SECTION("mode never does not hide it at the very top")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
    }
}

TEST_CASE("StickyHeader.terminal.anEvictedHeadKeepsThePlaceholderBelowTheTop", "[stickyheader][evicted]")
{
    // The owner's amendment to spec §6.3: a long block whose prompt left the scrollback does not lose its
    // header for that. Scrolled back into its surviving output -- but not to the very top -- the
    // placeholder takes the header's place wherever the mode would show a header there.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    startCommand(mc, "make");
    printLines(mc, "out", 40);
    auto const id = runningBlock(mc);

    REQUIRE(mc.terminal.viewport().scrollToTop());
    REQUIRE(mc.terminal.viewport().scrollDown(LineCount(3)));
    REQUIRE(mc.terminal.viewport().scrolled());
    auto const topLine = mc.terminal.viewport().topLine();
    // Not the oldest row the scrollback holds: older ones are still above it ...
    REQUIRE(topLine > mc.terminal.primaryScreen().grid().addressableTop());
    // ... and it still belongs to the block whose head is gone.
    auto const* const record = mc.terminal.commandBlockAt(topLine);
    REQUIRE(record != nullptr);
    REQUIRE(record->id == id);
    REQUIRE_FALSE(mc.terminal.primaryScreen().grid().lineOffsetOf(record->headStableId).has_value());

    SECTION("scrolled (the default): the placeholder, naming the command")
    {
        REQUIRE(mc.terminal.settings().stickyHeader.mode == StickyHeaderMode::Scrolled);
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
        CHECK(header->block == id);
        CHECK(textOf(header->cells) == "⋯ make — earlier output evicted");
        CHECK(header->chip.starts_with(U"running "));
        CHECK(mc.terminal.stickyHeaderBlock() == id);
    }

    SECTION("never: nothing below the very top")
    {
        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Never;
        CHECK_FALSE(headerOf(mc).has_value());
        CHECK_FALSE(mc.terminal.stickyHeaderBlock().has_value());
    }

    SECTION("show_evicted off: nothing")
    {
        mc.terminal.settings().stickyHeader.showEvicted = false;
        CHECK_FALSE(headerOf(mc).has_value());
    }

    SECTION("back at the live output: scrolled shows nothing there, always shows the placeholder")
    {
        REQUIRE(mc.terminal.viewport().scrollToBottom());
        CHECK_FALSE(headerOf(mc).has_value());

        mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
        auto const header = headerOf(mc);
        REQUIRE(header.has_value());
        CHECK(header->kind == StickyHeaderKind::Evicted);
        CHECK(header->block == id);
    }
}

TEST_CASE("StickyHeader.terminal.thePlaceholderShowsAHostileCommandDefanged", "[stickyheader][evicted]")
{
    // Review Focus #3 on this surface: the command line came from whatever wrote to the pty.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(60) }, LineCount(10) };
    startCommand(mc, "make%1B%5B201~");
    printLines(mc, "out", 40);
    REQUIRE(mc.terminal.viewport().scrollToTop());

    auto const header = headerOf(mc);
    REQUIRE(header.has_value());
    REQUIRE(header->kind == StickyHeaderKind::Evicted);
    auto text = std::u32string {};
    for (auto const& cell: header->cells)
        text += cell.codepoints;
    CHECK(text.find(U'\x1b') == std::u32string::npos);
    CHECK(text.find(U'�') != std::u32string::npos);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[evicted]"`
Expected: FAIL — `REQUIRE( header.has_value() )` fails in all three test cases (Task 7.6 decides on `Evicted` but draws nothing); the sections that expect no header already pass.

- [ ] **Step 3: Build the placeholder**

In `src/vtbackend/screen/Terminal.cpp` (`Terminal::fillStickyHeader`), before → after:

```cpp
        case StickyHeaderKind::Evicted:
            // No row of the block's prompt survives to be copied.
            return;
```

```cpp
        case StickyHeaderKind::Evicted: {
            // No row of the block's prompt survives to be copied, so the record speaks for it: the command
            // line it kept, sanitised, in the gutter's own quieter text colour.
            auto const textColor = _colorPalette.gutterTextColor();
            cells = layoutRenderText(evictedPlaceholderText(record->commandLine),
                                     RenderAttributes { .foregroundColor = textColor,
                                                        .backgroundColor = background,
                                                        .decorationColor = textColor,
                                                        .flags = {},
                                                        .lineFlags = LineFlag::None },
                                     CellLocation { .line = baseLine, .column = ColumnOffset(0) },
                                     boxed_cast<ColumnOffset>(textColumns));
            break;
        }
```

- [ ] **Step 4: Format**

Run: `clang-format -i src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_stickyheader_test.cpp`
Expected: no output.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[stickyheader]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_stickyheader_test.cpp
git commit -F - <<'EOF'
vtbackend: show the evicted-output placeholder for an evicted prompt

When the top row belongs to a block whose head was evicted, the header
names the command from its record instead of copying a row that is
gone: at the very top of the scrollback whatever the header mode, and
anywhere else wherever the mode would show a header, so the header
never vanishes just because its prompt was evicted. Gated by
show_evicted.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.8: Jump to a block's head

**Files:**
- Modify: `src/vtbackend/screen/Viewport.hpp`, `src/vtbackend/screen/Viewport.cpp`, `src/vtbackend/screen/Terminal.hpp`, `src/vtbackend/screen/Terminal.cpp`
- Test: `src/vtbackend/screen/Terminal_stickyheader_test.cpp`

**Interfaces:**
- Consumes: `Viewport::scrollOffsetForTopLine()` (private, `Viewport.cpp:166-181`), `Viewport::scrollTo()` (`:145`), `Terminal::expandFoldContaining()` (`Terminal.hpp:1130`), `commandBlockHeadLine()` (Task 7.6), C1 `CommandBlockStore::find()`.
- Produces (addition): `void Viewport::scrollLineToTop(LineOffset line);`
  `enum class CommandBlockJumpError : uint8_t { UnknownBlock = 0, HeadEvicted };`
  `[[nodiscard]] std::expected<void, CommandBlockJumpError> Terminal::scrollToCommandBlockHead(CommandBlockId id);`

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/screen/Terminal_stickyheader_test.cpp`:

```cpp
TEST_CASE("StickyHeader.jump.bringsTheHeadToTheTop", "[stickyheader][jump]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "make");
    printLines(mc, "out", 20);
    auto const id = runningBlock(mc);

    REQUIRE(mc.terminal.scrollToCommandBlockHead(id).has_value());

    auto const& grid = mc.terminal.primaryScreen().grid();
    CHECK(grid.lineText(mc.terminal.viewport().topLine()).starts_with("$ make"));
    // The head is on screen now, so there is nothing left for a header to say.
    CHECK_FALSE(headerOf(mc).has_value());
}

TEST_CASE("StickyHeader.jump.expandsTheBlocksCollapsedFold", "[stickyheader][jump][folding]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
    startCommand(mc, "ls");
    printLines(mc, "out", 20);
    finishCommand(mc, 0);
    startCommand(mc, "make");
    printLines(mc, "more", 20);

    auto const* ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const head = grid.lineOffsetOf(ls->headStableId);
    REQUIRE(head.has_value());
    REQUIRE(mc.terminal.toggleFoldContaining(*head));
    REQUIRE(mc.terminal.foldState().isCollapsed(ls->headStableId));

    REQUIRE(mc.terminal.scrollToCommandBlockHead(ls->id).has_value());
    CHECK_FALSE(mc.terminal.foldState().isCollapsed(ls->headStableId));
    CHECK(mc.terminal.viewport().topLine() == *head);
}

TEST_CASE("StickyHeader.jump.saysWhyItCannotLand", "[stickyheader][jump]")
{
    SECTION("a block nobody has heard of")
    {
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(100) };
        auto const jump = mc.terminal.scrollToCommandBlockHead(CommandBlockId(9999));
        REQUIRE_FALSE(jump.has_value());
        CHECK(jump.error() == CommandBlockJumpError::UnknownBlock);
    }

    SECTION("a block whose head the scrollback no longer holds")
    {
        auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(30) }, LineCount(10) };
        startCommand(mc, "make");
        printLines(mc, "out", 40);
        auto const jump = mc.terminal.scrollToCommandBlockHead(runningBlock(mc));
        REQUIRE_FALSE(jump.has_value());
        CHECK(jump.error() == CommandBlockJumpError::HeadEvicted);
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'scrollToCommandBlockHead': is not a member of 'vtbackend::Terminal'` and `'CommandBlockJumpError': undeclared identifier`.

- [ ] **Step 3: Add `Viewport::scrollLineToTop`**

In `src/vtbackend/screen/Viewport.hpp`, before → after (line 91):

```cpp
    /// Ensures given line is visible by optionally scrolling the
```

```cpp
    /// Scrolls so that grid line @p line is drawn on the viewport's top row, as far as the scrollback
    /// reaches.
    ///
    /// The public face of scrollOffsetForTopLine(), for a caller that names its target rather than
    /// searching for a mark: the sticky header and the scrollbar marks jump to a block's head.
    /// @param line The grid line to bring to the top.
    void scrollLineToTop(LineOffset line);

    /// Ensures given line is visible by optionally scrolling the
```

In `src/vtbackend/screen/Viewport.cpp`, insert before `bool Viewport::isLineVisible(LineOffset line) const` (line 231):

```cpp
void Viewport::scrollLineToTop(LineOffset line)
{
    ViewportLog()("scrollLineToTop {}", unbox(line));
    scrollTo(scrollOffsetForTopLine(line));
}

```

- [ ] **Step 4: Declare the terminal jump**

In `src/vtbackend/screen/Terminal.hpp`, before → after (line 81):

```cpp
/// Platform-independent scroll gesture phase, mapped from Qt::ScrollPhase.
```

```cpp
/// Why Terminal::scrollToCommandBlockHead() could not bring a block's head to the top.
enum class CommandBlockJumpError : uint8_t
{
    UnknownBlock = 0, ///< No record carries the id: it never existed, or the store's bound dropped it.
    HeadEvicted,      ///< The record outlives its head row, which the scrollback no longer holds.
};

/// Platform-independent scroll gesture phase, mapped from Qt::ScrollPhase.
```

and, inside the `// {{{ Sticky command header (OSC 133 blocks)` section added in Task 7.6, before → after:

```cpp
        return foldingAppliesToDisplayedPage() ? _stickyHeaderBlock : std::nullopt;
    }
```

```cpp
        return foldingAppliesToDisplayedPage() ? _stickyHeaderBlock : std::nullopt;
    }

    /// Scrolls the viewport so block @p id's head is its top row, expanding the fold that hangs off it.
    ///
    /// What a press on the sticky header does, and what a scrollbar tick does. The head is located
    /// through the record's cached position, so the jump costs the same for a block a million rows up.
    /// @param id The block to jump to.
    /// @return Nothing once the viewport moved (or already showed the head on top); why not otherwise.
    [[nodiscard]] std::expected<void, CommandBlockJumpError> scrollToCommandBlockHead(CommandBlockId id);
```

- [ ] **Step 5: Implement the jump**

In `src/vtbackend/screen/Terminal.cpp`, insert directly before `void Terminal::fillStickyHeader(RenderBuffer& output, LineOffset baseLine)`:

```cpp
std::expected<void, CommandBlockJumpError> Terminal::scrollToCommandBlockHead(CommandBlockId id)
{
    auto const* const record = commandBlocks().find(id);
    if (record == nullptr)
        return std::unexpected { CommandBlockJumpError::UnknownBlock };

    auto const head = commandBlockHeadLine(*record);
    if (!head)
        return std::unexpected { CommandBlockJumpError::HeadEvicted };

    // Expanded before scrolling: a collapsed fold changes which offset puts the head on top, and a user
    // who asked to see a command asked to see what it printed as well.
    expandFoldContaining(*head);
    _viewport.scrollLineToTop(*head);
    return {};
}

```

- [ ] **Step 6: Format**

Run: `clang-format -i src/vtbackend/screen/Viewport.hpp src/vtbackend/screen/Viewport.cpp src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_stickyheader_test.cpp`
Expected: no output.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[stickyheader]"`
Expected: zero warnings; `All tests passed`.

- [ ] **Step 8: Commit**

```bash
git add src/vtbackend/screen/Viewport.hpp src/vtbackend/screen/Viewport.cpp src/vtbackend/screen/Terminal.hpp \
        src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_stickyheader_test.cpp
git commit -F - <<'EOF'
vtbackend: jump the viewport to a command block's head

scrollToCommandBlockHead() expands the block's fold and brings its head
to the top row, or says why it cannot: an unknown block, or a head the
scrollback no longer holds.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.9: Renderer draws the header in a step of its own

**Files:**
- Modify: `src/vtrasterizer/Renderer.hpp`, `src/vtrasterizer/Renderer.cpp`
- Modify (test helper): `src/vtrasterizer/RendererTestHelpers.hpp` (`MockRenderTarget` records rectangles and `execute()` steps)
- Test: `src/vtrasterizer/TextRenderer_test.cpp` (it defines the `RendererTest` friend and `ReconfigFixture`; a second TU defining `RendererTest` would break the ODR)

**Interfaces:**
- Consumes: `RenderStickyHeader`, `layoutRenderText`, `renderTextWidth` (Task 7.3); `RenderTarget::renderRectangle()` / `execute()` (`RenderTarget.hpp:119,141`); `GridMetrics::mapTopLeft/mapBottomLeft` (`GridMetrics.hpp:73-97`); `renderCells()` (`Renderer.cpp:882`); `Terminal::mainPageTopRow()`, `pageSize()`.
- Produces (private): `void Renderer::renderStickyHeader(vtbackend::RenderStickyHeader const& header, vtbackend::LineOffset row, vtbackend::ColumnCount columns);`
- Produces (test helper): `MockRenderTarget::RecordedRectangle { int x; int y; Width width; Height height; RGBAColor color; size_t step; }`, `std::vector<RecordedRectangle> rectangles`, `size_t executeCount`; `RendererTest::renderStickyHeader(Renderer&, RenderStickyHeader const&, LineOffset, ColumnCount)`.

- [ ] **Step 1: Make the mock render target record what it is asked to draw**

In `src/vtrasterizer/RendererTestHelpers.hpp`, before → after (line 164):

```cpp
    void renderRectangle(int, int, vtbackend::Width, vtbackend::Height, vtbackend::RGBAColor) override {}
    void setScissorRect(int, int, int, int) override {}
    void clearScissorRect() override {}
    void scheduleScreenshot(ScreenshotCallback) override {}
    void execute(std::chrono::steady_clock::time_point) override {}
```

```cpp
    /// One renderRectangle() call, in the order it was issued.
    struct RecordedRectangle
    {
        int x = 0;
        int y = 0;
        vtbackend::Width width {};
        vtbackend::Height height {};
        vtbackend::RGBAColor color {};

        /// How many execute() calls preceded it. Each execute() is one compositing step, and a step draws
        /// all its rectangles before any of its glyphs -- so only a rectangle in a LATER step than some
        /// glyphs can cover them.
        size_t step = 0;
    };

    std::vector<RecordedRectangle> rectangles;
    size_t executeCount = 0;

    void renderRectangle(
        int x, int y, vtbackend::Width width, vtbackend::Height height, vtbackend::RGBAColor color) override
    {
        rectangles.push_back(RecordedRectangle {
            .x = x, .y = y, .width = width, .height = height, .color = color, .step = executeCount });
    }
    void setScissorRect(int, int, int, int) override {}
    void clearScissorRect() override {}
    void scheduleScreenshot(ScreenshotCallback) override {}
    void execute(std::chrono::steady_clock::time_point) override { ++executeCount; }
```

and add `#include <vtbackend/core/Color.hpp>` and `#include <vtbackend/core/Primitives.hpp>` above `#include <vtrasterizer/RenderTarget.hpp>` (line 4), and `#include <cstddef>` to its standard includes.

- [ ] **Step 2: Write the failing tests**

In `src/vtrasterizer/TextRenderer_test.cpp`, add to the `RendererTest` class (before its closing `};`, line 130):

```cpp
    /// Draws @p header as a frame's sticky-header step does, inside one text frame.
    static void renderStickyHeader(Renderer& renderer,
                                   vtbackend::RenderStickyHeader const& header,
                                   vtbackend::LineOffset row,
                                   vtbackend::ColumnCount columns)
    {
        renderer._textRenderer.beginFrame();
        renderer.renderStickyHeader(header, row, columns);
        renderer._textRenderer.endFrame();
    }
```

add `#include <vtbackend/testing/MockTerm.hpp>` after `#include <vtbackend/core/ColorPalette.hpp>`, and `#include <algorithm>` and `#include <format>` to the standard includes. Then append at the end of the file:

```cpp
TEST_CASE("Renderer.stickyHeader.drawsABandItsCellsTheChipAndASeparator", "[renderer][stickyheader]")
{
    configureMockFont();
    ReconfigFixture fixture;
    fixture.attachRenderTarget();
    auto const& rectangles = fixture.renderTarget.rectangles;

    auto const band = 0x303030_rgb;
    auto const edge = 0x808080_rgb;
    auto const cellBackground = 0x204060_rgb;
    auto header = vtbackend::RenderStickyHeader { .kind = StickyHeaderKind::Command,
                                                  .block = CommandBlockId(1),
                                                  .cells = {},
                                                  .chip = {},
                                                  .chipAttributes = {},
                                                  .background = band,
                                                  .separator = edge };
    for (auto const column: std::views::iota(0, 3))
        header.cells.push_back(vtbackend::RenderCell {
            .codepoints = {},
            .image = {},
            .position = CellLocation { .line = LineOffset(2), .column = ColumnOffset(column) },
            .attributes = RenderAttributes { .foregroundColor = 0xFFFFFF_rgb,
                                             .backgroundColor = cellBackground,
                                             .decorationColor = 0xFFFFFF_rgb,
                                             .flags = {},
                                             .lineFlags = LineFlag::None },
            .width = 1,
            .sizing = {},
            .groupStart = column == 0,
            .groupEnd = column == 2 });

    // Seeded metrics: 10x20 cells, no margin -- so row 2 spans y 40..59, and 80 columns are 800 pixels.
    RendererTest::renderStickyHeader(fixture.renderer, header, LineOffset(2), ColumnCount(80));

    SECTION("an opaque band first, the separator last, the cells in between")
    {
        REQUIRE(rectangles.size() == 5);

        auto const& first = rectangles.front();
        CHECK(first.x == 0);
        CHECK(first.y == 40);
        CHECK(first.width == Width(800));
        CHECK(first.height == Height(20));
        CHECK(first.color == RGBAColor(band, 0xFF));

        CHECK(std::ranges::count_if(rectangles, [&](auto const& r) { return r.color.rgb() == cellBackground; }) == 3);

        auto const& last = rectangles.back();
        CHECK(last.x == 0);
        CHECK(last.y == 59);
        CHECK(last.width == Width(800));
        CHECK(last.height == Height(1));
        CHECK(last.color == RGBAColor(edge, 0xFF));
    }

    SECTION("the chip is right-aligned on the page")
    {
        fixture.renderTarget.rectangles.clear();
        header.cells.clear();
        header.chip = U"✓ 3s";
        header.chipAttributes = RenderAttributes { .foregroundColor = 0xFFFFFF_rgb,
                                                   .backgroundColor = cellBackground,
                                                   .decorationColor = 0xFFFFFF_rgb,
                                                   .flags = {},
                                                   .lineFlags = LineFlag::None };
        RendererTest::renderStickyHeader(fixture.renderer, header, LineOffset(2), ColumnCount(80));

        // Four columns wide and StickyHeaderChipInset (2) columns short of the page's end, so it starts at
        // column 74 -- pixel 740 -- and ends on column 77, leaving the last two columns to the scrollbar.
        auto const chipCell = [&](int x) {
            return std::ranges::any_of(
                rectangles, [&](auto const& r) { return r.x == x && r.y == 40 && r.color.rgb() == cellBackground; });
        };
        CHECK(chipCell(740));
        CHECK(chipCell(770));
        CHECK_FALSE(chipCell(730));
        CHECK_FALSE(chipCell(780));
        CHECK_FALSE(chipCell(790));
    }
}

TEST_CASE("Renderer.stickyHeader.coversTheTopRowInAStepAfterThePage", "[renderer][stickyheader]")
{
    // One execute() draws every rectangle before any glyph, so a band issued in the page's own step would
    // sit UNDER row 0's text. The frame must flush the page first and draw the header in a later step.
    configureMockFont();
    ReconfigFixture fixture;
    fixture.attachRenderTarget();

    auto mc = vtbackend::MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    mc.terminal.settings().stickyHeader.mode = StickyHeaderMode::Always;
    mc.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\make\r\n\033]133;C;cmdline_url=make\033\\");
    for (auto const i: std::views::iota(0, 20))
        mc.writeToScreen(std::format("out {}\r\n", i));

    (void) fixture.renderer.render(mc.terminal, /*pressureHint*/ false);

    auto const header = [&] {
        auto const buffer = mc.terminal.renderBuffer();
        return buffer.get().stickyHeader;
    }();
    REQUIRE(header.has_value());

    auto const& rectangles = fixture.renderTarget.rectangles;
    auto const band = std::ranges::find_if(rectangles, [&](auto const& r) {
        return r.y == 0 && r.width == Width(200) && r.height == Height(20) && r.color == RGBAColor(header->background, 0xFF);
    });
    REQUIRE(band != rectangles.end());
    CHECK(band->step >= 1);

    auto const separator = std::ranges::find_if(rectangles, [&](auto const& r) {
        return r.y == 19 && r.height == Height(1) && r.color == RGBAColor(header->separator, 0xFF);
    });
    REQUIRE(separator != rectangles.end());
    CHECK(separator->step == band->step);
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target vtrasterizer_test`
Expected: FAIL — `error C2039: 'renderStickyHeader': is not a member of 'vtrasterizer::Renderer'`.

- [ ] **Step 4: Declare the drawing**

In `src/vtrasterizer/Renderer.hpp`, before → after (line 399):

```cpp
    void executeImageDiscards();
```

```cpp
    /// Draws the sticky command header over screen row @p row: an opaque band across the page's
    /// @p columns, the copied cells, the chip right-aligned (StickyHeaderChipInset columns short of the
    /// page's end), and a one-pixel separator along the band's bottom edge.
    ///
    /// The page columns only, not the gutter strip: the fold column beside row 0 keeps describing the row
    /// it stands beside, which belongs to the very block the header names. Called in a render step of its
    /// own (@see renderImpl), because one step draws all its rectangles before any of its glyphs.
    ///
    /// @param header The header the render buffer carries.
    /// @param row The screen row it covers -- the main page's first (Terminal::mainPageTopRow()).
    /// @param columns The main page's width.
    void renderStickyHeader(vtbackend::RenderStickyHeader const& header,
                            vtbackend::LineOffset row,
                            vtbackend::ColumnCount columns);

    void executeImageDiscards();
```

- [ ] **Step 5: Implement the drawing**

In `src/vtrasterizer/Renderer.cpp`, insert before `size_t Renderer::findCellPartitionPoint` (line 958)
(`vtbackend::StickyHeaderChipInset`, Task 7.2, reaches it through `Renderer.hpp` → `Terminal.hpp` →
`Settings.hpp` → `shell/StickyHeader.hpp`, Task 7.6):

```cpp
void Renderer::renderStickyHeader(vtbackend::RenderStickyHeader const& header,
                                  vtbackend::LineOffset row,
                                  vtbackend::ColumnCount columns)
{
    ZoneScoped;
    auto const top = _gridMetrics.mapTopLeft(row, vtbackend::ColumnOffset(0));
    auto const bottom = _gridMetrics.mapBottomLeft(row, vtbackend::ColumnOffset(0));
    auto const width = _gridMetrics.cellSize.width * vtbackend::Width::cast_from(columns);

    // Opaque, whatever the window's background opacity: the band exists to hide the row beneath it, and a
    // translucent one would let that row's glyphs show through its own.
    auto constexpr Opaque = uint8_t { 0xFF };

    _renderTarget->renderRectangle(
        top.x, top.y, width, _gridMetrics.cellSize.height, vtbackend::RGBAColor(header.background, Opaque));
    renderCells(header.cells);

    // Right-aligned, StickyHeaderChipInset columns short of the page's end (clear of the overlay
    // scrollbar), and never pushed left of column 0 into the gutter on a page too narrow for it.
    auto const pageEnd = boxed_cast<vtbackend::ColumnOffset>(
        std::max(vtbackend::ColumnCount(0), columns - vtbackend::StickyHeaderChipInset));
    auto const chipStart = std::max(vtbackend::ColumnOffset(0),
                                    pageEnd - boxed_cast<vtbackend::ColumnOffset>(vtbackend::renderTextWidth(header.chip)));
    auto const chip = vtbackend::layoutRenderText(
        header.chip, header.chipAttributes, vtbackend::CellLocation { .line = row, .column = chipStart }, pageEnd);
    renderCells(chip);

    _renderTarget->renderRectangle(
        top.x, bottom.y - 1, width, vtbackend::Height(1), vtbackend::RGBAColor(header.separator, Opaque));
}

```

- [ ] **Step 6: Give the header its own step in the frame**

In `src/vtrasterizer/Renderer.cpp` (`renderImpl`), before → after (lines 752-765):

```cpp
    auto const primaryPressure = pressure && terminal.isPrimaryScreen();

    if (smoothPixelOffset == 0)
    {
        // --- Single-pass rendering: no smooth scroll offset, no scissor needed ---
        setSmoothScrollOffset(0);
        renderPass(primaryPressure, [&] {
            vtbackend::RenderBufferRef const renderBuffer = terminal.renderBuffer();
            cursorOpt = renderBuffer.get().cursor;
            renderCells(std::span(renderBuffer.get().cells));
            renderLines(std::span(renderBuffer.get().lines));
            renderGutter(std::span(renderBuffer.get().gutter));
            renderAnnotations(std::span(renderBuffer.get().annotations));
        });
    }
```

```cpp
    auto const primaryPressure = pressure && terminal.isPrimaryScreen();

    // The sticky command header covers the main page's first row, so it has to composite OVER that row's
    // glyphs -- and one execute() draws every rectangle it was handed before any glyph (RhiRenderer's
    // recordRectPass() precedes recordTextPass()). So the header gets a step of its own, after everything
    // it covers, and is drawn unshifted: it is pinned to the row, not carried by the content scrolling
    // beneath it. Before the cursor, which is therefore never hidden by it.
    auto const renderStickyHeaderStep = [&](vtbackend::RenderStickyHeader const& header) {
        _renderTarget->execute(now);
        setSmoothScrollOffset(0);
        renderPass(false,
                   [&] { renderStickyHeader(header, terminal.mainPageTopRow(), terminal.pageSize().columns); });
    };

    if (smoothPixelOffset == 0)
    {
        // --- Single-pass rendering: no smooth scroll offset, no scissor needed ---
        setSmoothScrollOffset(0);
        // Held across both steps, so the header comes from the very buffer the cells did.
        vtbackend::RenderBufferRef const renderBuffer = terminal.renderBuffer();
        cursorOpt = renderBuffer.get().cursor;
        renderPass(primaryPressure, [&] {
            renderCells(std::span(renderBuffer.get().cells));
            renderLines(std::span(renderBuffer.get().lines));
            renderGutter(std::span(renderBuffer.get().gutter));
            renderAnnotations(std::span(renderBuffer.get().annotations));
        });
        if (auto const& header = renderBuffer.get().stickyHeader)
            renderStickyHeaderStep(*header);
    }
```

(the renderAnnotations line is Task 4.2's; keep it)

and (two-pass path, lines 808-813):

```cpp
            _renderTarget->execute(now);
        }

        // Pass 2: Status line (no scroll offset, no scissor).
```

```cpp
            _renderTarget->execute(now);
        }

        // After the scissored step, so the header is not clipped with the content it sits over.
        if (auto const& header = renderBuffer.get().stickyHeader)
            renderStickyHeaderStep(*header);

        // Pass 2: Status line (no scroll offset, no scissor).
```

- [ ] **Step 7: Format**

Run: `clang-format -i src/vtrasterizer/Renderer.hpp src/vtrasterizer/Renderer.cpp src/vtrasterizer/RendererTestHelpers.hpp src/vtrasterizer/TextRenderer_test.cpp`
Expected: no output.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target vtrasterizer_test && out/build/clangcl-debug/bin/vtrasterizer_test.exe "[stickyheader]"`
Expected: zero warnings; `All tests passed` (2 test cases). Then `out/build/clangcl-debug/bin/vtrasterizer_test.exe` — the whole binary passes (the mock target now records; nothing else asserts on it). If the full-frame case aborts inside the atlas on the BDF test font rather than failing an assertion, stop and report it: it is the first whole-frame test in `vtrasterizer_test`.

- [ ] **Step 9: Commit**

```bash
git add src/vtrasterizer/Renderer.hpp src/vtrasterizer/Renderer.cpp src/vtrasterizer/RendererTestHelpers.hpp \
        src/vtrasterizer/TextRenderer_test.cpp
git commit -F - <<'EOF'
vtrasterizer: draw the sticky command header over the top row

An opaque band, the copied row, a right-aligned chip and a one-pixel
separator, drawn in an execute() step after the page so the band covers
row 0's glyphs, and unshifted by smooth scrolling.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.10: Profile key `sticky_header`

**Files:**
- Modify: `src/contour/config/ConfigEnum.hpp`, `src/contour/config/Config.hpp`, `src/contour/config/Config.cpp`, `src/contour/config/ConfigDocumentation.hpp`
- Modify: `src/contour/session/TerminalSession.cpp` (`configureTerminal`, the live-reload path)
- Modify: `src/contour/test/e2e/coverage-config.yml.in`
- Test: `src/contour/config/Config_test.cpp`, `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: `StickyHeaderMode`, `StickyHeaderSettings` (Task 7.1), `Settings::stickyHeader` (Task 7.6); `ConfigEnumInfo`/`configEnumValues` (`ConfigEnum.hpp:29-98`); `loadConfigEnum` (`Config.cpp:2038`); `sessionSettings()` (`Config.cpp:569-644`); `TerminalSession::configureTerminal()` (`TerminalSession.cpp:3834-3891`).
- Produces (C9): `struct StickyHeaderConfig { vtbackend::StickyHeaderMode mode { vtbackend::StickyHeaderMode::Scrolled }; bool showEvicted { true }; [[nodiscard]] constexpr vtbackend::StickyHeaderSettings settings() const noexcept; };`
  `ConfigEntry<StickyHeaderConfig, documentation::StickyHeader> TerminalProfile::stickyHeader {};` (YAML `sticky_header: { mode: never|scrolled|always, show_evicted: bool }`);
  `template <> constexpr std::span<ConfigEnumInfo<vtbackend::StickyHeaderMode> const> configEnumValues() noexcept;`
  `template <> struct std::formatter<vtbackend::StickyHeaderMode>;`

- [ ] **Step 1: Write the failing config tests**

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: every sticky header mode is described by its table", "[config][stickyheader]")
{
    using namespace contour::config;
    using vtbackend::StickyHeaderMode;

    // The table is what the reader accepts, what the writer spells and what the settings page offers.
    auto constexpr Modes = std::array { StickyHeaderMode::Never, StickyHeaderMode::Scrolled, StickyHeaderMode::Always };
    CHECK(configEnumValues<StickyHeaderMode>().size() == Modes.size());
    for (auto const mode: Modes)
    {
        CHECK(!configEnumLabel(mode).empty());
        CHECK(configEnumFromToken<StickyHeaderMode>(configEnumToken(mode)) == mode);
    }
    CHECK(configEnumToken(StickyHeaderMode::Never) == "never");
    CHECK(configEnumToken(StickyHeaderMode::Scrolled) == "scrolled");
    CHECK(configEnumToken(StickyHeaderMode::Always) == "always");
    CHECK(configEnumFromToken<StickyHeaderMode>("ALWAYS") == StickyHeaderMode::Always);
}

TEST_CASE("Config: sticky_header loads from YAML and reaches the session settings", "[config][stickyheader]")
{
    QTemporaryDir dir;

    SECTION("defaults: scrolled, with the evicted placeholder on")
    {
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(profile->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Scrolled);
        CHECK(profile->stickyHeader.value().showEvicted);
        CHECK(contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark).stickyHeader
              == vtbackend::StickyHeaderSettings {});
    }

    SECTION("explicit values reach vtbackend")
    {
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        sticky_header:
            mode: always
            show_evicted: false
)"sv);
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        CHECK(contour::config::sessionSettings(config, *profile, vtbackend::ColorPreference::Dark).stickyHeader
              == vtbackend::StickyHeaderSettings { .mode = vtbackend::StickyHeaderMode::Always, .showEvicted = false });
    }

    SECTION("an unknown mode keeps the default")
    {
        auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        sticky_header:
            mode: sometimes
)"sv);
        CHECK(config.profile("main")->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Scrolled);
    }

    SECTION("a profile round-trips through the writer")
    {
        auto const source = loadFromYaml(dir, "default_profile: main\n");
        auto profile = *source.findProfile("main");
        profile.stickyHeader = contour::config::StickyHeaderConfig { .mode = vtbackend::StickyHeaderMode::Never,
                                                                     .showEvicted = false };
        writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

        auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
        auto const* roundTripped = reloaded.findProfile("roundtrip");
        REQUIRE(roundTripped != nullptr);
        CHECK(roundTripped->stickyHeader.value().mode == vtbackend::StickyHeaderMode::Never);
        CHECK_FALSE(roundTripped->stickyHeader.value().showEvicted);
    }
}
```

- [ ] **Step 2: Write the failing session test**

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: a profile switch re-applies sticky_header to the terminal",
          "[contour][session][stickyheader]")
{
    TestApp testApp;
    auto const name = registerProfile(testApp.app(), "sticky", [](auto& profile) {
        profile.stickyHeader = contour::config::StickyHeaderConfig { .mode = vtbackend::StickyHeaderMode::Always,
                                                                     .showEvicted = false };
    });
    auto session = makeDisplaylessSession(testApp.app());
    REQUIRE(session->terminal().settings().stickyHeader == vtbackend::StickyHeaderSettings {});

    // The live path a config reload and a profile switch both take (configureTerminal).
    (void) (*session)(contour::actions::ChangeProfile { .name = name });
    CHECK(session->terminal().settings().stickyHeader
          == vtbackend::StickyHeaderSettings { .mode = vtbackend::StickyHeaderMode::Always, .showEvicted = false });
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `error C2039: 'stickyHeader': is not a member of 'contour::config::TerminalProfile'` and `'StickyHeaderConfig': is not a member of 'contour::config'`.

- [ ] **Step 4: Add the token table**

In `src/contour/config/ConfigEnum.hpp`, before → after (line 7):

```cpp
#include <vtbackend/shell/MarkArbiter.hpp>
```

```cpp
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
```

then (line 140) before → after:

```cpp
            vtbackend::SearchCaseSensitivity::Sensitive, "sensitive", "Match case" },
    };
```

```cpp
            vtbackend::SearchCaseSensitivity::Sensitive, "sensitive", "Match case" },
    };

    /// When the sticky command header may cover the top row.
    ///
    /// Owned by vtbackend, so the table hangs off the type from here, as FoldJumpBehavior's does.
    inline constexpr auto StickyHeaderModeTable = std::array {
        ConfigEnumInfo<vtbackend::StickyHeaderMode> { vtbackend::StickyHeaderMode::Never, "never", "Never" },
        ConfigEnumInfo<vtbackend::StickyHeaderMode> {
            vtbackend::StickyHeaderMode::Scrolled, "scrolled", "While scrolled back" },
        ConfigEnumInfo<vtbackend::StickyHeaderMode> {
            vtbackend::StickyHeaderMode::Always, "always", "Also while following output" },
    };
```

and (line 165) before → after:

```cpp
    return detail::SearchCaseSensitivityTable;
}
```

```cpp
    return detail::SearchCaseSensitivityTable;
}

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::StickyHeaderMode> const> configEnumValues() noexcept
{
    return detail::StickyHeaderModeTable;
}
```

- [ ] **Step 5: Add the config struct, the profile entry, the reader and writer declarations, the formatter**

In `src/contour/config/Config.hpp`:

After `#include <vtbackend/shell/MarkArbiter.hpp>` (line 19) add `#include <vtbackend/shell/StickyHeader.hpp>`.

Before → after (line 415):

```cpp
struct MouseConfig
```

```cpp
/// The sticky command header: the command of the block being read, pinned over the top row once it has
/// scrolled out of view above its own output (profile key `sticky_header`).
///
/// A plain bool for `showEvicted` because it is a YAML schema field converted at the boundary -- the
/// documented carve-out in AGENT.md. `mode` is not a bool in disguise: it names three outcomes.
struct StickyHeaderConfig
{
    vtbackend::StickyHeaderMode mode { vtbackend::StickyHeaderMode::Scrolled }; ///< When it shows.
    bool showEvicted { true }; ///< Whether the evicted-output placeholder may stand in for an evicted prompt.

    /// The terminal's view of this configuration.
    [[nodiscard]] constexpr vtbackend::StickyHeaderSettings settings() const noexcept
    {
        return vtbackend::StickyHeaderSettings { .mode = mode, .showEvicted = showEvicted };
    }
};

struct MouseConfig
```

Before → after (line 738):

```cpp
    ConfigEntry<ScrollBarConfig, documentation::Scrollbar> scrollbar {};
```

```cpp
    ConfigEntry<ScrollBarConfig, documentation::Scrollbar> scrollbar {};
    ConfigEntry<StickyHeaderConfig, documentation::StickyHeader> stickyHeader {};
```

Before → after (line 1634):

```cpp
    void loadFromEntry(YAML::Node const& node, std::string const& entry, MouseConfig& where);
```

```cpp
    void loadFromEntry(YAML::Node const& node, std::string const& entry, StickyHeaderConfig& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, vtbackend::StickyHeaderMode& where);
    void loadFromEntry(YAML::Node const& node, std::string const& entry, MouseConfig& where);
```

Before → after (`struct Writer`, line 1961):

```cpp
    [[nodiscard]] std::string format(std::string_view doc, MouseConfig const& v)
```

```cpp
    [[nodiscard]] std::string format(std::string_view doc, StickyHeaderConfig const& v)
    {
        return format(doc, v.mode, v.showEvicted);
    }

    [[nodiscard]] std::string format(std::string_view doc, MouseConfig const& v)
```

Before → after (line 2583):

```cpp
template <>
struct std::formatter<contour::config::ShadowSize>: formatter<std::string_view>
```

```cpp
// vtbackend owns the enum, the TOKEN is a configuration fact -- the reasoning of the
// SearchCaseSensitivity formatter above.
template <>
struct std::formatter<vtbackend::StickyHeaderMode>: formatter<std::string_view>
{
    auto format(vtbackend::StickyHeaderMode value, auto& ctx) const
    {
        return formatter<std::string_view>::format(contour::config::configEnumToken(value), ctx);
    }
};

template <>
struct std::formatter<contour::config::ShadowSize>: formatter<std::string_view>
```

- [ ] **Step 6: Document the key**

In `src/contour/config/ConfigDocumentation.hpp`, before → after (line 436):

```cpp
constexpr StringLiteral AutoScrollOnUpdateConfig {
```

```cpp
constexpr StringLiteral StickyHeaderConfig {

    "sticky_header:\n"
    "    {comment} Pins the command line of the block you are reading over the top row once that\n"
    "    {comment} command has scrolled out of view above its own output. Needs shell integration.\n"
    "    {comment}   never    - never\n"
    "    {comment}   scrolled - only while the viewport is scrolled back into history\n"
    "    {comment}   always   - also while following live output\n"
    "    mode: {}\n"
    "    {comment} Whether the header still names a command whose own line has already been dropped\n"
    "    {comment} from the scrollback (\"earlier output evicted\"): at the very top of the scrollback\n"
    "    {comment} whatever mode says, and anywhere else wherever mode would show a header.\n"
    "    show_evicted: {}\n"
    "\n"

};

constexpr StringLiteral AutoScrollOnUpdateConfig {
```

before → after (line 2162):

```cpp
constexpr StringLiteral MouseWeb {
```

```cpp
constexpr StringLiteral StickyHeaderWeb {
    "configuration pins the command line of the block you are reading over the top row of the terminal, "
    "once that command has scrolled out of view above its own output.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    sticky_header:\n"
    "      mode: scrolled\n"
    "      show_evicted: true\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==mode== When the header appears: `never`, `scrolled` (only while the "
    "viewport is scrolled back into history; the default) or `always` (also while following live output, so "
    "a long build keeps its command in view). It needs shell integration (OSC 133), never appears on the "
    "alternate screen, shows the command's exit status and duration on its right, and a click on it scrolls "
    "to the command. <br/>\n"
    ":octicons-horizontal-rule-16: ==show_evicted== Whether the header still names a command whose own line "
    "has already been dropped from the scrollback, as `⋯ <command> — earlier output evicted`, so a long "
    "output does not lose its header when its prompt is evicted. At the very top of the scrollback it shows "
    "whatever `mode` says (even `never`); anywhere else it shows wherever `mode` would show a header. <br/>\n"
    "\n"
};

constexpr StringLiteral MouseWeb {
```

and before → after (line 2660):

```cpp
using Scrollbar = DocumentationEntry<ScrollbarConfig, ScrollbarWeb>;
```

```cpp
using Scrollbar = DocumentationEntry<ScrollbarConfig, ScrollbarWeb>;
using StickyHeader = DocumentationEntry<StickyHeaderConfig, StickyHeaderWeb>;
```

- [ ] **Step 7: Read the key and map it into the terminal settings**

In `src/contour/config/Config.cpp`:

Before → after (`loadProfileBody`, line 1156):

```cpp
        loadFromEntry(child, "scrollbar", where.scrollbar);
```

```cpp
        loadFromEntry(child, "scrollbar", where.scrollbar);
        loadFromEntry(child, "sticky_header", where.stickyHeader);
```

Insert before `void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, MouseConfig& where)` (line 2285):

```cpp
void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     StickyHeaderConfig& where)
{
    auto const child = node[entry];
    if (!child)
        return;

    loadFromEntry(child, "mode", where.mode);
    loadFromEntry(child, "show_evicted", where.showEvicted);
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::StickyHeaderMode& where)
{
    // Through the shared token table, so the reader, the writer and the settings page cannot disagree.
    (void) loadConfigEnum(node, entry, where, logger);
}

```

Before → after (`sessionSettings`, line 641):

```cpp
    settings.highlightTimeout = profile.highlightTimeout.value();
```

```cpp
    settings.highlightTimeout = profile.highlightTimeout.value();
    settings.stickyHeader = profile.stickyHeader.value().settings();
```

- [ ] **Step 8: Re-apply it on a profile switch and a reload**

In `src/contour/session/TerminalSession.cpp` (`configureTerminal`, line 3890), before → after:

```cpp
    _terminal.settings().momentumScrolling = _profile.momentumScrolling.value();
```

```cpp
    _terminal.settings().momentumScrolling = _profile.momentumScrolling.value();
    _terminal.settings().stickyHeader = _profile.stickyHeader.value().settings();
```

- [ ] **Step 9: Drive the parse path from the e2e coverage config**

In `src/contour/test/e2e/coverage-config.yml.in`, before → after:

```yaml
        scrollbar:
            position: Right
```

```yaml
        scrollbar:
            position: Right
        sticky_header:
            mode: always
            show_evicted: false
```

- [ ] **Step 10: Format**

Run: `clang-format -i src/contour/config/ConfigEnum.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/session/TerminalSession.cpp src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp`
Expected: no output.

- [ ] **Step 11: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test contour_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[stickyheader]"`
Expected: zero warnings; `All tests passed` (3 test cases). Then `QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`: all pass (the default-config round-trip tests now write and read `sticky_header:`).

- [ ] **Step 12: Hand-audit for clang-tidy**

clang-tidy cannot lint `src/contour/**` on this box (README). Read every added line in `Config.hpp`, `Config.cpp`, `ConfigEnum.hpp`, `TerminalSession.cpp` and the two tests for: `misc-const-correctness` (every local that is not modified is `const`), `misc-use-internal-linkage` (no new free function outside a class or an anonymous namespace), `bugprone-implicit-widening-of-multiplication-result` (none — no arithmetic added), `readability-identifier-naming` (types `CamelCase`, members `camelBack`, constexpr locals `CamelCase`). Expected: nothing to change.

- [ ] **Step 13: Commit**

```bash
git add src/contour/config/ConfigEnum.hpp src/contour/config/Config.hpp src/contour/config/Config.cpp \
        src/contour/config/ConfigDocumentation.hpp src/contour/session/TerminalSession.cpp \
        src/contour/test/e2e/coverage-config.yml.in src/contour/config/Config_test.cpp \
        src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
config: add the sticky_header profile key

mode (never | scrolled | always, through the shared token table) and
show_evicted, documented, written back, mapped into the terminal's
settings at session start and re-applied by configureTerminal().

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.11: Colour-scheme keys `sticky_header.{background,separator}`

**Files:**
- Modify: `src/contour/config/Config.cpp` (scheme reader before line 1567, scheme writer `emitColorPaletteBody` before line 3935)
- Modify: `src/contour/config/ConfigDocumentation.hpp`
- Modify: `docs/configuration/colors.md`, `src/contour/test/e2e/coverage-config.yml.in`
- Test: `src/contour/config/Config_test.cpp`

**Interfaces:**
- Consumes: `ColorPalette::stickyHeaderBackground`, `stickyHeaderSeparator`, `StickyHeaderColorSlots` (Task 7.4); `YAMLConfigReader::loadFromEntry(YAML::Node const&, std::string const&, vtbackend::RGBColor&)` (as used for `tint:` at `Config.cpp:1579`); `processWithDoc` (`Config.cpp:3842`).
- Produces (C9): scheme YAML `sticky_header: { background: <colour>, separator: <colour> }`, each optional; documentation entries `documentation::StickyHeaderColors`, `StickyHeaderColorsHeader`, `StickyHeaderColorEntry`.

- [ ] **Step 1: Write the failing test**

Append to `src/contour/config/Config_test.cpp`:

```cpp
TEST_CASE("Config: the sticky header's colors load and are written back", "[config][stickyheader]")
{
    QTemporaryDir dir;

    auto const schemeOf = [](contour::config::Config const& config) {
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        REQUIRE(std::holds_alternative<contour::config::SimpleColorConfig>(profile->colors.value()));
        return std::get<contour::config::SimpleColorConfig>(profile->colors.value()).colors;
    };

    SECTION("unset by default, so both derive from the scheme")
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
        CHECK_FALSE(scheme.stickyHeaderBackground.has_value());
        CHECK_FALSE(scheme.stickyHeaderSeparator.has_value());
    }

    SECTION("both slots load")
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
        sticky_header:
            background: '#202830'
            separator: '#506070'
)"sv));
        REQUIRE(scheme.stickyHeaderBackground.has_value());
        CHECK(*scheme.stickyHeaderBackground == vtbackend::RGBColor(0x20, 0x28, 0x30));
        REQUIRE(scheme.stickyHeaderSeparator.has_value());
        CHECK(*scheme.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }

    SECTION("the writer emits what was set, and only that")
    {
        auto palette = vtbackend::ColorPalette {};
        palette.stickyHeaderSeparator = vtbackend::RGBColor(0x50, 0x60, 0x70);
        auto const written = contour::config::emitColorSchemeYaml(palette);
        // An uncommented key: the commented example of an unset scheme reads "# sticky_header:".
        REQUIRE(written.contains("\nsticky_header:\n"));
        writeSideFile(dir, "colorschemes/mono.yml", written);

        auto const reloaded = schemeOf(loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        colors: mono
)"sv));
        CHECK_FALSE(reloaded.stickyHeaderBackground.has_value());
        REQUIRE(reloaded.stickyHeaderSeparator.has_value());
        CHECK(*reloaded.stickyHeaderSeparator == vtbackend::RGBColor(0x50, 0x60, 0x70));
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "Config: the sticky header's colors load and are written back"`
Expected: FAIL — `REQUIRE( scheme.stickyHeaderBackground.has_value() )` (nothing reads the key yet) and `REQUIRE( written.contains("\nsticky_header:\n") )`.

- [ ] **Step 3: Document the slots**

In `src/contour/config/ConfigDocumentation.hpp`, before → after (line 1434):

```cpp
constexpr StringLiteral ContextTintConfig {
```

```cpp
constexpr StringLiteral StickyHeaderColorsConfig {
    "\n"
    "{comment} Colors of the sticky command header pinned over the top row (profile key sticky_header).\n"
    "{comment} Unset by default: the band is then this scheme's background lifted a tenth of the way\n"
    "{comment} toward its foreground, and the separator along its bottom edge about a third of the way.\n"
    "{comment} The values shown are what the default scheme derives, so uncommenting them as-is\n"
    "{comment} changes nothing -- they are a starting point to edit, not a second opinion.\n"
    "{comment} sticky_header:\n"
    "{comment}     background: '#2c2928'\n"
    "{comment}     separator: '#595757'\n"
};

constexpr StringLiteral StickyHeaderColorsHeaderConfig {
    "\n"
    "{comment} Colors of the sticky command header. Written back because this scheme sets them; remove\n"
    "{comment} the block to go back to deriving them from this scheme's own background and foreground.\n"
    "sticky_header:\n"
};

constexpr StringLiteral StickyHeaderColorEntryConfig { "    {}: {}\n" };

constexpr StringLiteral ContextTintConfig {
```

and before → after (line 2834):

```cpp
using FoldMarkerHover = DocumentationEntry<FoldMarkerHoverConfig, Dummy>;
```

```cpp
using FoldMarkerHover = DocumentationEntry<FoldMarkerHoverConfig, Dummy>;
using StickyHeaderColors = DocumentationEntry<StickyHeaderColorsConfig, Dummy>;
using StickyHeaderColorsHeader = DocumentationEntry<StickyHeaderColorsHeaderConfig, Dummy>;
using StickyHeaderColorEntry = DocumentationEntry<StickyHeaderColorEntryConfig, Dummy>;
```

- [ ] **Step 4: Read and write the slots through the slot table**

In `src/contour/config/Config.cpp`, before → after (line 1567):

```cpp
    if (auto const tints = child["tint"])
```

```cpp
    // The sticky header's band and separator, each left unset unless the scheme says otherwise -- the fold
    // column's rule, for the fold column's reason. Walked from StickyHeaderColorSlots, so a third slot is
    // a row in vtbackend and nothing here.
    if (auto const stickyHeader = child["sticky_header"])
    {
        logger()("*** loading sticky_header");
        for (auto const& [key, slot]: vtbackend::StickyHeaderColorSlots)
        {
            auto const name = std::string { key };
            if (!stickyHeader[name])
                continue;
            auto color = vtbackend::RGBColor {};
            loadFromEntry(stickyHeader, name, color);
            where.*slot = color;
        }
    }

    if (auto const tints = child["tint"])
```

and (in `emitColorPaletteBody`, line 3935) before → after:

```cpp
    // Same rule, same reason: only what the scheme actually set, and the commented example otherwise.
```

```cpp
    // The fold column's rule: only what the scheme actually set, and the commented example otherwise.
    auto const stickyHeaderColorsSet = std::ranges::any_of(
        vtbackend::StickyHeaderColorSlots, [&](auto const& row) { return (entry.*row.second).has_value(); });
    if (stickyHeaderColorsSet)
    {
        processWithDoc(documentation::StickyHeaderColorsHeader {});
        // std::string, not the table's string_view: a string_view first value selects Writer::process()'s
        // overload that takes a name and drops it (Task 4.10), leaving the "{}: {}" row one argument short.
        for (auto const& [key, slot]: vtbackend::StickyHeaderColorSlots)
            if (auto const& color = entry.*slot)
                processWithDoc(documentation::StickyHeaderColorEntry {}, std::string { key }, *color);
    }
    else
        processWithDoc(documentation::StickyHeaderColors {});

    // Same rule, same reason: only what the scheme actually set, and the commented example otherwise.
```

- [ ] **Step 5: Document the slots for users**

In `docs/configuration/colors.md`, before → after (lines 217-219):

~~~markdown
the hovered fold, not just the row beneath it. <br/>

### Palette Presets 
~~~

~~~markdown
the hovered fold, not just the row beneath it. <br/>

### `sticky_header`
Defines the colors of the sticky command header (see the profile key `sticky_header`): the band pinned
over the top row while you read a command's output with the command itself scrolled out of view, and
the one-pixel line along its bottom edge.

Both are **optional**, and unset by default. A missing one is derived from this scheme's own `default`
colors: the band is the background lifted a tenth of the way toward the foreground, the separator about a
third of the way — quiet in every theme, yet distinct from the page.

``` yaml
color_schemes:
  default:
    sticky_header:
        background: '#2c2928'
        separator: '#595757'
```

:octicons-horizontal-rule-16: ==background== The band's color. The command copied into it keeps its own
colors; only the cells the shell left on the default background take this one. <br/>
:octicons-horizontal-rule-16: ==separator== The one-pixel line along the band's bottom edge. <br/>

### Palette Presets 
~~~

and in the full example (lines 366-367), before → after:

```yaml
        #         background: '#1a1716'
        input_method_editor:
```

```yaml
        #         background: '#1a1716'
        # sticky_header:
        #     background: '#2c2928'
        #     separator: '#595757'
        input_method_editor:
```

- [ ] **Step 6: Drive the scheme parse from the e2e coverage config**

In `src/contour/test/e2e/coverage-config.yml.in`, before → after:

```yaml
        tint:
            elevate: '#2a1a1a'
```

```yaml
        sticky_header:
            background: '#202830'
            separator: '#506070'
        tint:
            elevate: '#2a1a1a'
```

- [ ] **Step 7: Format**

Run: `clang-format -i src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp`
Expected: no output.

- [ ] **Step 8: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: zero warnings; `All tests passed` (the existing fold-marker and default-config round trips included).
Then: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure` → PASS (or SKIP without `typos`).

- [ ] **Step 9: Hand-audit for clang-tidy**

In the added `Config.cpp` lines: `name` and `stickyHeaderColorsSet` are `const`; `color` is written through `loadFromEntry`, so not const; the lambda captures by reference only what it reads. Expected: nothing to change.

- [ ] **Step 10: Commit**

```bash
git add src/contour/config/Config.cpp src/contour/config/ConfigDocumentation.hpp docs/configuration/colors.md \
        src/contour/test/e2e/coverage-config.yml.in src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
config: read and write the sticky header's scheme colours

sticky_header.{background,separator} in a colour scheme, both optional
and derived when unset, read and written by walking the palette's slot
table; documented in colors.md and the generated config.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.12: A press on the header jumps to the command

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp`, `src/contour/session/TerminalSession.cpp`
- Modify: `src/contour/session/SessionInput.cpp` (row hit-test beside `gutterHitAt` (phase 4 renamed `gutterLineAt`; locate it by name — line numbers moved); press and release routing, lines 688-726)
- Test: `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: `Terminal::stickyHeaderBlock()` (Task 7.6), `Terminal::scrollToCommandBlockHead()` (Task 7.8); `TerminalSession::withFolding(..., FoldingGate::Always)` (`TerminalSession.cpp:3001-3023`, takes the lock and announces the scrollable count a fold expansion changes); `geometry::mainPageRowAt()` (`WindowGeometry.hpp:422`).
- Produces (addition): `enum class ConsumedByStickyHeader : uint8_t { No = 0, Yes };`
  `[[nodiscard]] ConsumedByStickyHeader TerminalSession::sendStickyHeaderPressEvent(std::optional<vtbackend::LineOffset> mainPageRow, vtbackend::MouseButton button);`
  `[[nodiscard]] ConsumedByStickyHeader TerminalSession::sendStickyHeaderReleaseEvent();`

- [ ] **Step 1: Write the failing tests**

Append to `src/contour/session/TerminalSession_test.cpp`:

```cpp
TEST_CASE("TerminalSession: a press on the sticky header jumps to its command",
          "[contour][session][input][stickyheader]")
{
    using contour::session::ConsumedByStickyHeader;

    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();

    terminal.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\make\r\n\033]133;C;cmdline_url=make\033\\");
    for (auto const i: std::views::iota(0, 60))
        terminal.writeToScreen(std::format("out {}\r\n", i));

    // Scrolled back into the output, where the default `scrolled` mode shows the header -- and one frame,
    // which is what records the block a press on row 0 lands on.
    REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(5)));
    auto buffer = vtbackend::RenderBuffer {};
    terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);
    REQUIRE(buffer.stickyHeader.has_value());

    SECTION("a left press takes the viewport to the command, and its release is the header's too")
    {
        CHECK(session->sendStickyHeaderPressEvent(vtbackend::LineOffset(0), vtbackend::MouseButton::Left)
              == ConsumedByStickyHeader::Yes);
        CHECK(terminal.primaryScreen().grid().lineText(terminal.viewport().topLine()).starts_with("$ make"));

        CHECK(session->sendStickyHeaderReleaseEvent() == ConsumedByStickyHeader::Yes);
        CHECK(session->sendStickyHeaderReleaseEvent() == ConsumedByStickyHeader::No);
    }

    SECTION("another button is consumed -- it would act on the hidden row -- but does not jump")
    {
        auto const before = terminal.viewport().scrollOffset();
        CHECK(session->sendStickyHeaderPressEvent(vtbackend::LineOffset(0), vtbackend::MouseButton::Right)
              == ConsumedByStickyHeader::Yes);
        CHECK(terminal.viewport().scrollOffset() == before);
    }

    SECTION("a press below the header, or off the page, belongs to the grid")
    {
        CHECK(session->sendStickyHeaderPressEvent(vtbackend::LineOffset(1), vtbackend::MouseButton::Left)
              == ConsumedByStickyHeader::No);
        CHECK(session->sendStickyHeaderPressEvent(std::nullopt, vtbackend::MouseButton::Left)
              == ConsumedByStickyHeader::No);
        CHECK(session->sendStickyHeaderReleaseEvent() == ConsumedByStickyHeader::No);
    }
}

TEST_CASE("TerminalSession: without shell integration row 0 belongs to the grid",
          "[contour][session][input][stickyheader]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    for (auto const i: std::views::iota(0, 60))
        terminal.writeToScreen(std::format("plain {}\r\n", i));

    REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(5)));
    auto buffer = vtbackend::RenderBuffer {};
    terminal.fillRenderBuffer(buffer, /*includeSelection*/ true);
    REQUIRE_FALSE(buffer.stickyHeader.has_value());

    CHECK(session->sendStickyHeaderPressEvent(vtbackend::LineOffset(0), vtbackend::MouseButton::Left)
          == contour::session::ConsumedByStickyHeader::No);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `error C2039: 'ConsumedByStickyHeader': is not a member of 'contour::session'`.

- [ ] **Step 3: Declare the session API**

In `src/contour/session/TerminalSession.hpp`, before → after (lines 86-90):

```cpp
enum class ConsumedByGutter : uint8_t
{
    No = 0,
    Yes,
};
```

```cpp
enum class ConsumedByGutter : uint8_t
{
    No = 0,
    Yes,
};

/// Whether the sticky command header took a pointer press, and the grid must therefore not also see it.
///
/// Its own type rather than ConsumedByGutter: the two route independently -- a press beside row 0 can be
/// over the gutter, which the gutter takes, or over the header -- and a shared type would let one
/// handler's answer pass for the other's.
enum class ConsumedByStickyHeader : uint8_t
{
    No = 0,
    Yes,
};
```

before → after (line 723):

```cpp
    ConsumedByGutter sendGutterReleaseEvent();
```

```cpp
    ConsumedByGutter sendGutterReleaseEvent();

    /// Jumps to the command the sticky header shows, when the press landed on it.
    ///
    /// Consulted after the gutter and before the grid, as the gutter's press is: the header covers row 0,
    /// so a press there must not start a selection or reach the mouse protocol at a row the user cannot
    /// see. Every button is consumed while a header shows; only the left one jumps.
    ///
    /// @param mainPageRow The main-page row under the pointer, measured WITHOUT the smooth-scroll shift
    ///                    (the header is pinned, not scrolled), or nullopt when off the page.
    /// @param button The button that went down.
    /// @return Whether the header consumed the event.
    [[nodiscard]] ConsumedByStickyHeader sendStickyHeaderPressEvent(std::optional<vtbackend::LineOffset> mainPageRow,
                                                                    vtbackend::MouseButton button);

    /// Swallows the release matching a press the sticky header consumed.
    /// @return Whether the header consumed the event.
    [[nodiscard]] ConsumedByStickyHeader sendStickyHeaderReleaseEvent();
```

and before → after (line 1164):

```cpp
    bool _gutterClickPending = false;
```

```cpp
    bool _gutterClickPending = false;

    /// What the sticky header did with this pane's last press: Yes means its release is owed to the header
    /// too, for the reason _gutterClickPending gives.
    ConsumedByStickyHeader _stickyHeaderPress = ConsumedByStickyHeader::No;
```

- [ ] **Step 4: Implement it**

In `src/contour/session/TerminalSession.cpp`, insert after the definition of `TerminalSession::sendGutterReleaseEvent()` (ends line 2196):

```cpp

ConsumedByStickyHeader TerminalSession::sendStickyHeaderPressEvent(std::optional<vtbackend::LineOffset> mainPageRow,
                                                                   vtbackend::MouseButton button)
{
    // The header covers row 0 and nothing else.
    if (mainPageRow != vtbackend::LineOffset(0))
        return ConsumedByStickyHeader::No;

    // Through withFolding() for its lock and its announcement of the scrollable count, which the jump
    // changes when it expands a collapsed fold. Ungated: a jump can only ever reveal output.
    auto const consumed = withFolding(
        [button](vtbackend::Terminal& terminal) {
            auto const block = terminal.stickyHeaderBlock();
            if (!block)
                return false;
            // An evicted head has nowhere to go: the jump answers HeadEvicted and the viewport stays. The
            // press is consumed all the same -- the placeholder covers row 0 wherever it shows.
            if (button == vtbackend::MouseButton::Left)
                (void) terminal.scrollToCommandBlockHead(*block);
            return true;
        },
        FoldingGate::Always);

    if (!consumed)
        return ConsumedByStickyHeader::No;

    // Remembered here, as the gutter does, so the release cannot drift from the press.
    _stickyHeaderPress = ConsumedByStickyHeader::Yes;
    return ConsumedByStickyHeader::Yes;
}

ConsumedByStickyHeader TerminalSession::sendStickyHeaderReleaseEvent()
{
    return std::exchange(_stickyHeaderPress, ConsumedByStickyHeader::No);
}
```

Add `#include <utility>` to its standard includes if it is not there already.

- [ ] **Step 5: Route Qt presses and releases through it**

In `src/contour/session/SessionInput.cpp`, insert after the end of `gutterHitAt()` (phase 4's rename of `gutterLineAt`; find it by name, inside the anonymous namespace):

```cpp

    /// The main-page row under @p yLogical as the sticky command header occupies it.
    ///
    /// Not mouseGridRow(): the header is pinned over row 0 and drawn WITHOUT the smooth-scroll shift
    /// (@see vtrasterizer::Renderer::renderStickyHeader), so taking that shift out of the pointer, as the
    /// grid's hit-test must, would move the header's band by up to a cell.
    ///
    /// @param yLogical The pointer's y in logical pixels, relative to the item's top edge.
    /// @param session The session it belongs to; must have a display.
    /// @return The main-page row, or nullopt when the pointer is off the main page.
    [[nodiscard]] std::optional<vtbackend::LineOffset> stickyHeaderRowAt(double yLogical,
                                                                         TerminalSession const& session) noexcept
    {
        auto const& terminal = session.terminal();
        auto const row = geometry::mainPageRowAt(static_cast<int>(yLogical * session.display()->devicePixelRatio()),
                                                 session.display()->gridMetrics().pageMargin.top,
                                                 session.display()->cellSize().height.as<int>(),
                                                 unbox<int>(terminal.mainPageTopRow()),
                                                 unbox<int>(terminal.pageSize().lines));
        return row ? std::optional { vtbackend::LineOffset(*row) } : std::nullopt;
    }
```

In `sendMousePressEvent(QMouseEvent*, TerminalSession&)`, before → after (lines 696-702):

```cpp
    if (session.sendGutterPressEvent(gutterHitAt(event->position(), session),
                                     input::makeMouseButton(event->button()))
        == ConsumedByGutter::Yes)
    {
        event->accept();
        return;
    }
```

```cpp
    if (session.sendGutterPressEvent(gutterHitAt(event->position(), session),
                                     input::makeMouseButton(event->button()))
        == ConsumedByGutter::Yes)
    {
        event->accept();
        return;
    }

    // After the gutter, whose strip beside row 0 keeps its own meaning, and before the grid.
    if (session.sendStickyHeaderPressEvent(stickyHeaderRowAt(event->position().y(), session),
                                           input::makeMouseButton(event->button()))
        == ConsumedByStickyHeader::Yes)
    {
        event->accept();
        return;
    }
```

In `sendMouseReleaseEvent(QMouseEvent*, TerminalSession&)`, before → after (lines 717-721):

```cpp
    if (session.sendGutterReleaseEvent() == ConsumedByGutter::Yes)
    {
        event->accept();
        return;
    }
```

```cpp
    if (session.sendGutterReleaseEvent() == ConsumedByGutter::Yes)
    {
        event->accept();
        return;
    }

    if (session.sendStickyHeaderReleaseEvent() == ConsumedByStickyHeader::Yes)
    {
        event->accept();
        return;
    }
```

- [ ] **Step 6: Format**

Run: `clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/SessionInput.cpp src/contour/session/TerminalSession_test.cpp`
Expected: no output.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[session]"`
Expected: zero warnings; `All tests passed`, including the two new cases and the existing `wheel and mouse event helpers route through the session` (a display-less session still returns before any hit-test).

- [ ] **Step 8: Hand-audit for clang-tidy**

In the added lines: `stickyHeaderRowAt` sits in the anonymous namespace (`misc-use-internal-linkage`); `static_cast<int>(yLogical * dpr)` multiplies doubles, so `bugprone-implicit-widening-of-multiplication-result` does not apply; every local is `const` (`misc-const-correctness`); the lambda captures `button` by value and nothing else. Expected: nothing to change.

- [ ] **Step 9: Commit**

```bash
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp \
        src/contour/session/SessionInput.cpp src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
session: jump to the command when the sticky header is clicked

A press on row 0 while a header shows is consumed, so no selection or
mouse report addresses the hidden row; the left button scrolls the
command's head to the top, expanding its fold. The release follows.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 7.13: Phase gate

**Files:** none beyond what review findings change.

- [ ] **Step 1: Full build and suite**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test vtrasterizer_test contour_test contour_gui_test`
Expected: zero warnings.
Run: `ctest --test-dir out/build/clangcl-debug --output-on-failure`
Expected: the phase-0 baseline's failures and no others; note the `N tests passed, M failed` line.
Run: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: PASS (or SKIP when `typos` is not installed).

- [ ] **Step 2: Simplify**

Run `/simplify` over `git diff <phase-7-start>..HEAD` (the SHA from Task 7.1 Step 1). Re-run Step 1's build and suite after its edits, then:

```bash
git add -u -- src docs
git commit -F - <<'EOF'
vtbackend: simplify the sticky header after review

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(Skip the commit when `/simplify` changed nothing.)

- [ ] **Step 3: Code review at xhigh**

Run `/code-review xhigh` on the commits `<phase-7-start>..HEAD` (or dispatch a review subagent with `effort: "xhigh"`), pointing it at this file's *Decisions* section and README Review Focus #1, #2, #5. Fix every confirmed finding, re-run Step 1, and commit:

```bash
git add -u -- src docs
git commit -F - <<'EOF'
vtbackend: address review of the sticky command header

ctest: <paste the "N tests passed, M failed" line from Step 1>

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

When the review confirms no finding, there is no commit; put the ctest line in the report of Step 4 instead.

- [ ] **Step 4: Report to the coordinator**

Report: the ctest summary line and any failing test names, the spelling-check result, the review's confirmed findings and their fixes, the hand-audit notes from Tasks 7.10–7.12, and the open risks below. The coordinating session records progress — not this session.

## Open risks

- **Phase 1 reflow refresh.** Review Focus #1 (Task 7.6) needs every record's head position refreshed by the
  reflow before the next frame. A stale generation is read as "head evicted", so after a column resize the
  header turns into the evicted placeholder (or vanishes, with `show_evicted: false`);
  `aColumnResizeMidOutputKeepsTheHeaderOnItsCommand` catches it through its `kind == Command` check. The fix
  belongs in phase 1, not in a scan here.
- **Head evicted mid-scroll — decided.** The owner chose the placeholder "anywhere" (Decisions #2; spec §6.3
  as amended): below the very top it shows wherever the mode would show a header, so `always` keeps `make`'s
  header — as `⋯ make — earlier output evicted` — once a long build evicts its prompt, and `never` shows it
  only at the very top. Tested pure (Task 7.1) and on a live terminal (Task 7.7,
  `anEvictedHeadKeepsThePlaceholderBelowTheTop`).
- **A press on the placeholder does not move the viewport.** The head is gone, so `scrollToCommandBlockHead()`
  answers `HeadEvicted` (Task 7.8); the press is still consumed (Task 7.12). Scrolling to the block's oldest
  surviving row instead would be a small follow-up if the owner wants it.
- **Running time from the wall clock.** The record keeps only a wall-clock start (C1); a clock step during a
  running command skews `running Ns` until it finishes. Exact would need a steady start in the record
  (a C1 change, carried by the daemon too).
- **Block cursor on row 0** is hidden under the header (it is drawn by cell inversion in the page step).
- **First whole-frame `vtrasterizer_test` case** (Task 7.9) runs `Renderer::render` over the BDF test font;
  if the atlas rejects it, keep the direct-drawing case and report.
- **Shared helpers (settled).** `layoutRenderText` is the one text-to-cells layout; phase 4's
  `annotationRenderCells` delegates to it (Task 7.3 Step 4b). Duration text: the chip calls phase 4's
  `vtbackend::formatCommandDuration`, as phase 6's notification body does.
- **Daemon clients** get the header only if phase 2's mirror stores head positions valid in the client grid.
