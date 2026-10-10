# Phase 8 — Scrollbar marks

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Thin ticks over the scrollbar mark where commands ran, failed or are still running, and where the user put a vi `mm` mark; clicking a tick scrolls that line to the top (opening its fold), selected per profile by `scrollbar.marks`.

**Architecture:** One pure function, `scrollbarMarks()` (C4), buckets marks into at most one tick per track pixel with the priority Failure > Running > UserMark > Command. `Terminal::scrollbarMarkInputs()` feeds it in O(records + marks): block heads come from each record's cached `headStableId`, user marks from a small `UserMarkIndex` of stable ids that the one semantic-mark funnel (`Screen::setLogicalLineFlags`) keeps current, which is rebuilt by one grid scan after a stable-id generation bump and pruned by `Grid::stableRangeFloor()`. Positions are visible (fold-aware) rows counted from the oldest history row, exactly the range `Viewport::scrollableLineCount()` gives the scrollbar. `TerminalSession` recomputes on a frame timer, at most once per frame, and only when a key (store/fold/user-mark revision, grid identity and extent, screen, track, sources, colours) moved; it publishes `scrollbarMarks` / `scrollbarMarkColors` to QML and jumps through `Terminal::revealStableLineAtTop()`. `SessionChrome.qml` draws a `Repeater` of 2-px rectangles inside the stock `ScrollBar`, so the ticks hide whenever the bar does. The bar itself is shown on the right by default from this phase on (Task 8.6a, owner decision), so the ticks are visible out of the box.

**Tech Stack:** C++23, Catch2 (`vtbackend_test`, `contour_gui_test`), Qt 6 / QML + `Qt6::Test`, yaml-cpp.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §6.1 (and §4.3, §4.6, §11.1 for the record, `UserMark` and the `scrollbar.marks` key). Global constraints, build commands and the Interface Contract: [README](README.md#global-constraints). This phase **produces** C4 and the C9 row `scrollbar.marks`; it **consumes** C1 (phase 1), the C2 palette resolvers (phase 4) and phase 7's `Viewport::scrollLineToTop()` / `Terminal::scrollToCommandBlockHead()` (Task 7.8; Task 8.5 builds `revealStableLineAtTop()` on the first and re-bases the second on it), and phase 5's
`TerminalSession::operator()(actions::ClearToPrompt)` (Task 5.11; Task 8.7 requests a tick refresh there).

**Test tag:** every new test carries `[scrollbar-marks]`.

**GUI test runs on this machine:** run `contour_gui_test` as
`QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[scrollbar-marks]"` (Git Bash); if it exits
`0xC0000135`, apply the README's PATH / `QT_QPA_PLATFORM_PLUGIN_PATH` workaround. If phase 0 recorded `contour_gui_test`
as unbuildable here (the pre-existing `yaml-cpp/emitter.h` break in `src/contour/display/*`), compile each edited TU
on its own — `ninja -C out/build/clangcl-debug -t commands contour_gui_test | grep -F "<file>.cpp"` and run that one
line through `cmd /c` — and say in the commit body that the run is left to CI.

---

### Task 8.1: Pure bucketing — `ScrollbarMarks.hpp` (C4)

**Files:**
- Create: `src/vtbackend/shell/ScrollbarMarks.hpp`, `src/vtbackend/shell/ScrollbarMarks.cpp`, `src/vtbackend/shell/ScrollbarMarks_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (header list ~line 64, sources ~line 105, `vtbackend_test` ~line 206)

**Interfaces:**
- Consumes: `vtbackend::CommandBlockOutcome` (C2, declared in `core/CommandBlockOutcome.hpp` by phase 1 Task 1.1 and re-exported by `shell/CommandBlock.hpp`); `ColorPalette::blockStatusColor(CommandBlockOutcome) const noexcept` and the `blockStatusSuccess/Failure/Running` slots (C2, phase 4); `ColorPalette::foldMarkerHoverColors()` (`src/vtbackend/core/ColorPalette.hpp:347`); `core::Flags` (`vendor/core-cpp/src/core/Flags.hpp`).
- Produces — exactly C4, plus additions marked (+):
  ```cpp
  enum class ScrollbarMarkKind : uint8_t { Command = 0, UserMark, Running, Failure };   // ascending priority
  enum class ScrollbarMarkSource : uint8_t { Failures = 1 << 0, Commands = 1 << 1, UserMarks = 1 << 2 };
  using ScrollbarMarkSources = core::Flags<ScrollbarMarkSource>;
  struct ScrollbarMarkInput { int64_t visibleRow {}; ScrollbarMarkKind kind {}; int64_t targetStableId {};
                              bool operator==(ScrollbarMarkInput const&) const noexcept = default; };  // (+ {} and ==)
  struct ScrollbarTick { int pixel {}; ScrollbarMarkKind kind {}; int64_t targetStableId {};
                         bool operator==(ScrollbarTick const&) const noexcept = default; };          // (+ {} and ==)
  [[nodiscard]] std::vector<ScrollbarTick> scrollbarMarks(std::span<ScrollbarMarkInput const> inputs,
                                                          int64_t visibleRowCount, int trackPixels,
                                                          ScrollbarMarkSources sources);
  constexpr inline std::array<ScrollbarMarkKind, 4> ScrollbarMarkKindList;                          // (+)
  constexpr inline ScrollbarMarkSources AllScrollbarMarkSources;                                    // (+)
  [[nodiscard]] constexpr ScrollbarMarkSource scrollbarMarkSourceOf(ScrollbarMarkKind) noexcept;     // (+)
  [[nodiscard]] constexpr ScrollbarMarkKind scrollbarMarkKindOf(CommandBlockOutcome) noexcept;       // (+)
  [[nodiscard]] RGBColor scrollbarMarkColor(ColorPalette const&, ScrollbarMarkKind) noexcept;        // (+)
  template <> struct std::formatter<vtbackend::ScrollbarMarkKind>;                                   // (+)
  ```

- [ ] **Step 1: Record the phase start**

Run: `git rev-parse HEAD`
Expected: one hash. Note it as `PHASE8_START`; the phase gate (Task 8.9) diffs against it.

- [ ] **Step 2: Write the failing test**

Create `src/vtbackend/shell/ScrollbarMarks_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <ranges>
#include <span>
#include <vector>

using namespace vtbackend;

namespace
{

using Kind = ScrollbarMarkKind;

/// A mark on @p row of kind @p kind, whose click would go to @p target.
[[nodiscard]] ScrollbarMarkInput markAt(int64_t row, Kind kind, int64_t target)
{
    return ScrollbarMarkInput { .visibleRow = row, .kind = kind, .targetStableId = target };
}

/// The kinds of @p ticks, top first.
[[nodiscard]] std::vector<Kind> kindsOf(std::span<ScrollbarTick const> ticks)
{
    return ticks | std::views::transform(&ScrollbarTick::kind) | std::ranges::to<std::vector>();
}

} // namespace

TEST_CASE("ScrollbarMarks.bucketing.nothingToDraw", "[scrollbar-marks]")
{
    auto const one = std::vector { markAt(5, Kind::Failure, 5) };
    CHECK(scrollbarMarks({}, 100, 50, AllScrollbarMarkSources).empty());
    CHECK(scrollbarMarks(one, 0, 50, AllScrollbarMarkSources).empty());   // no rows
    CHECK(scrollbarMarks(one, 100, 0, AllScrollbarMarkSources).empty());  // no track
    CHECK(scrollbarMarks(one, 100, 50, ScrollbarMarkSources {}).empty()); // `marks: []`
}

TEST_CASE("ScrollbarMarks.bucketing.aRowLandsOnThePixelItsFractionNames", "[scrollbar-marks]")
{
    // 1000 rows over a 100-pixel track: ten rows to a pixel. A row's pixel is its fraction of the range --
    // the fraction the handle's top reaches once that row is the viewport's top line.
    struct Case
    {
        int64_t row {};
        int pixel {};
    };
    constexpr auto Cases = std::array {
        Case { .row = 0, .pixel = 0 },    Case { .row = 9, .pixel = 0 },    Case { .row = 10, .pixel = 1 },
        Case { .row = 555, .pixel = 55 }, Case { .row = 999, .pixel = 99 },
    };
    for (auto const& c: Cases)
    {
        INFO(std::format("row {}", c.row));
        auto const marks = std::array { markAt(c.row, Kind::Command, 42) };
        auto const ticks = scrollbarMarks(marks, 1000, 100, AllScrollbarMarkSources);
        REQUIRE(ticks.size() == 1);
        CHECK(ticks[0] == ScrollbarTick { .pixel = c.pixel, .kind = Kind::Command, .targetStableId = 42 });
    }
}

TEST_CASE("ScrollbarMarks.bucketing.aShortHistorySpreadsOverTheTrack", "[scrollbar-marks]")
{
    // Ten rows on a 600-pixel track: each row owns sixty pixels and its tick sits at the top of them.
    auto marks = std::vector<ScrollbarMarkInput> {};
    for (auto const row: std::views::iota(int64_t { 0 }, int64_t { 10 }))
        marks.push_back(markAt(row, Kind::Command, row));

    auto const ticks = scrollbarMarks(marks, 10, 600, AllScrollbarMarkSources);
    REQUIRE(ticks.size() == 10);
    for (auto const& tick: ticks)
        CHECK(tick.pixel == static_cast<int>(tick.targetStableId) * 60);
}

TEST_CASE("ScrollbarMarks.bucketing.rowsOutsideTheRangeAreDropped", "[scrollbar-marks]")
{
    auto const marks =
        std::vector { markAt(-1, Kind::Failure, 1), markAt(100, Kind::Failure, 2), markAt(50, Kind::Command, 3) };
    auto const ticks = scrollbarMarks(marks, 100, 10, AllScrollbarMarkSources);
    REQUIRE(ticks.size() == 1);
    CHECK(ticks[0].targetStableId == 3);
}

TEST_CASE("ScrollbarMarks.bucketing.aPixelShowsItsHighestKind", "[scrollbar-marks]")
{
    // Failure > Running > UserMark > Command, whichever order the marks arrive in. Rows 3 and 7 share
    // pixel 0 of a 10-pixel track over 100 rows.
    struct Case
    {
        Kind first {};
        Kind second {};
        Kind winner {};
    };
    constexpr auto Cases = std::array {
        Case { .first = Kind::Command, .second = Kind::UserMark, .winner = Kind::UserMark },
        Case { .first = Kind::UserMark, .second = Kind::Running, .winner = Kind::Running },
        Case { .first = Kind::Running, .second = Kind::Failure, .winner = Kind::Failure },
        Case { .first = Kind::Command, .second = Kind::Running, .winner = Kind::Running },
        Case { .first = Kind::UserMark, .second = Kind::Failure, .winner = Kind::Failure },
        Case { .first = Kind::Command, .second = Kind::Failure, .winner = Kind::Failure },
    };
    for (auto const& c: Cases)
    {
        INFO(std::format("{} on row 3, {} on row 7", c.first, c.second));
        auto const winnerTarget = c.winner == c.first ? int64_t { 1 } : int64_t { 2 };
        for (auto const& marks: { std::vector { markAt(3, c.first, 1), markAt(7, c.second, 2) },
                                  std::vector { markAt(7, c.second, 2), markAt(3, c.first, 1) } })
        {
            auto const ticks = scrollbarMarks(marks, 100, 10, AllScrollbarMarkSources);
            REQUIRE(ticks.size() == 1);
            CHECK(ticks[0] == ScrollbarTick { .pixel = 0, .kind = c.winner, .targetStableId = winnerTarget });
        }
    }
}

TEST_CASE("ScrollbarMarks.bucketing.betweenEqualsTheTopmostWins", "[scrollbar-marks]")
{
    // Two failures on one pixel: the tick sends a click to the earlier of the two blocks, whichever order
    // the terminal happened to report them in.
    for (auto const& marks: { std::vector { markAt(3, Kind::Failure, 30), markAt(7, Kind::Failure, 70) },
                              std::vector { markAt(7, Kind::Failure, 70), markAt(3, Kind::Failure, 30) } })
    {
        auto const ticks = scrollbarMarks(marks, 100, 10, AllScrollbarMarkSources);
        REQUIRE(ticks.size() == 1);
        CHECK(ticks[0].targetStableId == 30);
    }
}

TEST_CASE("ScrollbarMarks.bucketing.sourcesChooseTheKinds", "[scrollbar-marks]")
{
    // One mark of each kind, each on its own pixel.
    auto const marks = std::vector { markAt(0, Kind::Command, 1),
                                     markAt(10, Kind::UserMark, 2),
                                     markAt(20, Kind::Running, 3),
                                     markAt(30, Kind::Failure, 4) };
    struct Case
    {
        ScrollbarMarkSources sources {};
        std::vector<Kind> kinds {};
    };
    auto const cases = std::array {
        Case { .sources = ScrollbarMarkSource::Failures, .kinds = { Kind::Failure } },
        Case { .sources = ScrollbarMarkSource::Commands, .kinds = { Kind::Command, Kind::Running } },
        Case { .sources = ScrollbarMarkSource::UserMarks, .kinds = { Kind::UserMark } },
        Case { .sources = AllScrollbarMarkSources,
               .kinds = { Kind::Command, Kind::UserMark, Kind::Running, Kind::Failure } },
    };
    for (auto const& c: cases)
    {
        INFO(std::format("sources {:#x}", c.sources.value()));
        CHECK(kindsOf(scrollbarMarks(marks, 40, 40, c.sources)) == c.kinds);
    }
}

TEST_CASE("ScrollbarMarks.bucketing.aKindLeftOutHidesNothing", "[scrollbar-marks]")
{
    // Filtering happens BEFORE merging: with failures switched off, the command sharing their pixel shows.
    auto const marks = std::vector { markAt(3, Kind::Failure, 1), markAt(7, Kind::Command, 2) };
    auto const ticks = scrollbarMarks(marks, 100, 10, ScrollbarMarkSource::Commands);
    REQUIRE(ticks.size() == 1);
    CHECK(ticks[0] == ScrollbarTick { .pixel = 0, .kind = Kind::Command, .targetStableId = 2 });
}

TEST_CASE("ScrollbarMarks.bucketing.aFloodCostsTheTrackNotTheScrollback", "[scrollbar-marks]")
{
    // README Review Focus #4: 100k marks on a 600-pixel track come back as at most 600 ticks, one per
    // pixel, ascending, each the highest kind that landed there -- checked against a brute-force reference.
    constexpr auto RowCount = int64_t { 1'000'000 };
    constexpr auto TrackPixels = 600;
    auto marks = std::vector<ScrollbarMarkInput> {};
    marks.reserve(100'000);
    for (auto const i: std::views::iota(int64_t { 0 }, int64_t { 100'000 }))
        marks.push_back(markAt((i * 7919) % RowCount, static_cast<Kind>(i % 4), i));

    auto const ticks = scrollbarMarks(marks, RowCount, TrackPixels, AllScrollbarMarkSources);

    CHECK(ticks.size() <= static_cast<size_t>(TrackPixels));
    CHECK(std::ranges::is_sorted(ticks, std::ranges::less {}, &ScrollbarTick::pixel));
    CHECK(std::ranges::adjacent_find(ticks, std::ranges::equal_to {}, &ScrollbarTick::pixel) == ticks.end());

    auto expected = std::map<int, Kind> {};
    for (auto const& mark: marks)
    {
        auto const pixel = static_cast<int>(mark.visibleRow * TrackPixels / RowCount);
        auto const [it, inserted] = expected.try_emplace(pixel, mark.kind);
        if (!inserted && mark.kind > it->second)
            it->second = mark.kind;
    }
    REQUIRE(ticks.size() == expected.size());
    for (auto const& tick: ticks)
        CHECK(tick.kind == expected.at(tick.pixel));
}

TEST_CASE("ScrollbarMarks.kinds.outcomesAndSourcesMapOneToOne", "[scrollbar-marks]")
{
    CHECK(scrollbarMarkKindOf(CommandBlockOutcome::Success) == Kind::Command);
    CHECK(scrollbarMarkKindOf(CommandBlockOutcome::Failure) == Kind::Failure);
    CHECK(scrollbarMarkKindOf(CommandBlockOutcome::Running) == Kind::Running);

    CHECK(scrollbarMarkSourceOf(Kind::Command) == ScrollbarMarkSource::Commands);
    CHECK(scrollbarMarkSourceOf(Kind::Running) == ScrollbarMarkSource::Commands);
    CHECK(scrollbarMarkSourceOf(Kind::UserMark) == ScrollbarMarkSource::UserMarks);
    CHECK(scrollbarMarkSourceOf(Kind::Failure) == ScrollbarMarkSource::Failures);

    // The list a frontend builds its per-kind tables over is every kind, in enumerator order.
    for (auto const index: std::views::iota(size_t { 0 }, ScrollbarMarkKindList.size()))
        CHECK(static_cast<size_t>(ScrollbarMarkKindList[index]) == index);
}

TEST_CASE("ScrollbarMarks.kinds.colorsComeFromTheScheme", "[scrollbar-marks]")
{
    auto palette = ColorPalette {};
    palette.blockStatusSuccess = RGBColor { 0x10, 0x20, 0x30 };
    palette.blockStatusFailure = RGBColor { 0xF0, 0x00, 0x00 };
    palette.blockStatusRunning = RGBColor { 0xF0, 0xF0, 0x00 };

    CHECK(scrollbarMarkColor(palette, Kind::Command) == RGBColor { 0x10, 0x20, 0x30 });
    CHECK(scrollbarMarkColor(palette, Kind::Failure) == RGBColor { 0xF0, 0x00, 0x00 });
    CHECK(scrollbarMarkColor(palette, Kind::Running) == RGBColor { 0xF0, 0xF0, 0x00 });
    // A mark the user placed is drawn in the scheme's full-strength text colour, apart from every outcome.
    CHECK(scrollbarMarkColor(palette, Kind::UserMark) == palette.foldMarkerHoverColors().foreground);
}
```

- [ ] **Step 3: Register the test**

In `src/vtbackend/CMakeLists.txt`, in the `add_executable(vtbackend_test ...)` list, insert directly after the line
`        shell/PromptRegion_test.cpp`:

```cmake
        shell/ScrollbarMarks_test.cpp
```

- [ ] **Step 4: Run it and watch it fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `fatal error C1083: Cannot open include file: 'vtbackend/shell/ScrollbarMarks.hpp'`.

- [ ] **Step 5: Write the header**

Create `src/vtbackend/shell/ScrollbarMarks.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <core/Flags.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <vector>

namespace vtbackend
{

struct ColorPalette;

/// What a scrollbar tick stands for.
///
/// Declared in ASCENDING priority, and the order is load-bearing: where several marks fall on one pixel
/// of the track, the highest enumerator is the one drawn (spec §6.1), so a failure is never hidden behind
/// the success that ran beside it.
enum class ScrollbarMarkKind : uint8_t
{
    Command = 0, ///< A finished command that succeeded, or whose exit status is unknown.
    UserMark,    ///< A line the user marked with vi `mm` (LineFlag::UserMark).
    Running,     ///< A command that is still running.
    Failure,     ///< A finished command that exited non-zero, or was killed.
};

/// Every kind, in enumerator order.
///
/// What a per-kind list -- the colours a frontend draws its ticks in -- is built over, so a fifth kind is
/// a row here rather than a loop someone has to remember to extend.
constexpr inline auto ScrollbarMarkKindList = std::array {
    ScrollbarMarkKind::Command,
    ScrollbarMarkKind::UserMark,
    ScrollbarMarkKind::Running,
    ScrollbarMarkKind::Failure,
};

/// The groups of marks `scrollbar.marks` switches on, one bit each.
///
/// A bitmask rather than one enum per group because every combination is legal and each is one token of
/// a YAML list -- the shape AGENT.md reserves for flags that genuinely combine.
enum class ScrollbarMarkSource : uint8_t
{
    Failures = 1 << 0,  ///< ScrollbarMarkKind::Failure.
    Commands = 1 << 1,  ///< ScrollbarMarkKind::Command and ScrollbarMarkKind::Running.
    UserMarks = 1 << 2, ///< ScrollbarMarkKind::UserMark.
};

/// A set of ScrollbarMarkSource bits.
using ScrollbarMarkSources = core::Flags<ScrollbarMarkSource>;

/// Every source -- the default, so a user who never heard of the setting sees every tick.
constexpr inline auto AllScrollbarMarkSources = ScrollbarMarkSources {
    ScrollbarMarkSource::Failures,
    ScrollbarMarkSource::Commands,
    ScrollbarMarkSource::UserMarks,
};

/// One mark the scrollbar may draw, as Terminal::scrollbarMarkInputs() reports it.
struct ScrollbarMarkInput
{
    /// The row within the scrollbar's range, the oldest history row being 0, counted in VISIBLE rows: a
    /// collapsed fold is its one head row, exactly as in Viewport::scrollableLineCount().
    int64_t visibleRow {};

    /// What the mark stands for.
    ScrollbarMarkKind kind {};

    /// The line a click brings to the top: a block's head, or the user-marked line itself.
    int64_t targetStableId {};

    [[nodiscard]] bool operator==(ScrollbarMarkInput const&) const noexcept = default;
};

/// One tick of the track, as scrollbarMarks() hands it out.
struct ScrollbarTick
{
    int pixel {};              ///< Offset down the track, in [0, trackPixels).
    ScrollbarMarkKind kind {}; ///< The highest-priority kind among the marks on this pixel.
    int64_t targetStableId {}; ///< That mark's target (@see ScrollbarMarkInput::targetStableId).

    [[nodiscard]] bool operator==(ScrollbarTick const&) const noexcept = default;
};

/// The `scrollbar.marks` source that shows marks of @p kind.
/// @param kind The kind of mark.
/// @return The source whose token switches it on.
[[nodiscard]] constexpr ScrollbarMarkSource scrollbarMarkSourceOf(ScrollbarMarkKind kind) noexcept
{
    switch (kind)
    {
        case ScrollbarMarkKind::Command:
        case ScrollbarMarkKind::Running: return ScrollbarMarkSource::Commands;
        case ScrollbarMarkKind::UserMark: return ScrollbarMarkSource::UserMarks;
        case ScrollbarMarkKind::Failure: return ScrollbarMarkSource::Failures;
    }
    return ScrollbarMarkSource::Commands;
}

/// The tick a command block with @p outcome draws as.
/// @param outcome The block's outcome (@see outcomeOf).
/// @return Its kind of mark.
[[nodiscard]] constexpr ScrollbarMarkKind scrollbarMarkKindOf(CommandBlockOutcome outcome) noexcept
{
    switch (outcome)
    {
        case CommandBlockOutcome::Success: return ScrollbarMarkKind::Command;
        case CommandBlockOutcome::Failure: return ScrollbarMarkKind::Failure;
        case CommandBlockOutcome::Running: return ScrollbarMarkKind::Running;
    }
    return ScrollbarMarkKind::Command;
}

/// Buckets @p inputs into the ticks a scrollbar track of @p trackPixels draws.
///
/// A mark lands on pixel `visibleRow * trackPixels / visibleRowCount` -- the fraction of the track the
/// handle's top reaches once that row is the viewport's top line. Marks on one pixel merge into one tick
/// of the highest kind (@see ScrollbarMarkKind); between two of the same kind the topmost row wins, so a
/// click goes to the first of the blocks the tick stands for. Kinds @p sources leaves out are dropped
/// BEFORE merging, so a hidden failure never hides a visible command.
///
/// One pass over @p inputs and one slot per pixel: the result holds at most @p trackPixels ticks and the
/// memory is the track's, however many marks a flood of OSC 133 cycles produced (README Review Focus #4).
///
/// @param inputs The marks, in any order.
/// @param visibleRowCount The rows the scrollbar spans: its scrollable rows plus the page.
/// @param trackPixels The track's height in pixels.
/// @param sources Which kinds to draw (`scrollbar.marks`).
/// @return The ticks, ascending by pixel and at most one per pixel; empty when there is no track, no row
///         or no source.
[[nodiscard]] std::vector<ScrollbarTick> scrollbarMarks(std::span<ScrollbarMarkInput const> inputs,
                                                        int64_t visibleRowCount,
                                                        int trackPixels,
                                                        ScrollbarMarkSources sources);

/// The colour a tick of @p kind is drawn in under @p palette.
///
/// Commands, failures and running commands take the scheme's `block_status` colours (the gutter's, so a
/// block reads the same in both places); a user mark takes the scheme's full-strength text colour, apart
/// from every outcome.
/// @param palette The session's colour palette.
/// @param kind The kind of tick.
/// @return Its colour.
[[nodiscard]] RGBColor scrollbarMarkColor(ColorPalette const& palette, ScrollbarMarkKind kind) noexcept;

} // namespace vtbackend

template <>
struct std::formatter<vtbackend::ScrollbarMarkKind>: formatter<std::string_view>
{
    auto format(vtbackend::ScrollbarMarkKind value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ScrollbarMarkKind::Command: name = "Command"; break;
            case vtbackend::ScrollbarMarkKind::UserMark: name = "UserMark"; break;
            case vtbackend::ScrollbarMarkKind::Running: name = "Running"; break;
            case vtbackend::ScrollbarMarkKind::Failure: name = "Failure"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
```

- [ ] **Step 6: Write the implementation**

Create `src/vtbackend/shell/ScrollbarMarks.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <optional>
#include <ranges>
#include <vector>

namespace vtbackend
{

namespace
{
    /// What one pixel of the track shows so far.
    struct PixelWinner
    {
        ScrollbarMarkKind kind {};
        int64_t visibleRow {};
        int64_t targetStableId {};
    };

    /// Whether @p candidate takes the pixel from @p incumbent.
    ///
    /// The higher kind wins; between two of one kind the topmost mark does, and between two on one row the
    /// smaller id -- so the result never depends on the order the marks were reported in.
    [[nodiscard]] bool outranks(ScrollbarMarkInput const& candidate, PixelWinner const& incumbent) noexcept
    {
        if (candidate.kind != incumbent.kind)
            return candidate.kind > incumbent.kind;
        if (candidate.visibleRow != incumbent.visibleRow)
            return candidate.visibleRow < incumbent.visibleRow;
        return candidate.targetStableId < incumbent.targetStableId;
    }
} // namespace

std::vector<ScrollbarTick> scrollbarMarks(std::span<ScrollbarMarkInput const> inputs,
                                          int64_t visibleRowCount,
                                          int trackPixels,
                                          ScrollbarMarkSources sources)
{
    if (visibleRowCount <= 0 || trackPixels <= 0 || sources.none())
        return {};

    // One slot per pixel and nothing per input: a flood of commands costs one pass over them and a
    // track's worth of memory, however many there are.
    auto winners = std::vector<std::optional<PixelWinner>>(static_cast<size_t>(trackPixels));

    for (auto const& input: inputs)
    {
        if (!sources.test(scrollbarMarkSourceOf(input.kind)))
            continue;

        // Outside the range the scrollbar spans: no fraction of the track stands for it.
        if (input.visibleRow < 0 || input.visibleRow >= visibleRowCount)
            continue;

        // Rows are LineCount-sized (int), so the product stays far inside int64_t.
        auto const pixel = static_cast<size_t>(input.visibleRow * trackPixels / visibleRowCount);
        auto& winner = winners[pixel];
        if (!winner || outranks(input, *winner))
            winner = PixelWinner { .kind = input.kind,
                                   .visibleRow = input.visibleRow,
                                   .targetStableId = input.targetStableId };
    }

    auto ticks = std::vector<ScrollbarTick> {};
    for (auto const pixel: std::views::iota(0, trackPixels))
        if (auto const& winner = winners[static_cast<size_t>(pixel)])
            ticks.push_back(
                ScrollbarTick { .pixel = pixel, .kind = winner->kind, .targetStableId = winner->targetStableId });
    return ticks;
}

RGBColor scrollbarMarkColor(ColorPalette const& palette, ScrollbarMarkKind kind) noexcept
{
    switch (kind)
    {
        case ScrollbarMarkKind::Command: return palette.blockStatusColor(CommandBlockOutcome::Success);
        case ScrollbarMarkKind::UserMark: return palette.foldMarkerHoverColors().foreground;
        case ScrollbarMarkKind::Running: return palette.blockStatusColor(CommandBlockOutcome::Running);
        case ScrollbarMarkKind::Failure: return palette.blockStatusColor(CommandBlockOutcome::Failure);
    }
    return palette.defaultForeground;
}

} // namespace vtbackend
```

- [ ] **Step 7: Register the sources**

In `src/vtbackend/CMakeLists.txt`: in the header list insert after the line `    screen/ScreenTestFixtures.hpp`:

```cmake
    shell/ScrollbarMarks.hpp
```

and in `set(vtbackend_SOURCES ...)` insert after the line `    shell/PromptRegion.cpp`:

```cmake
    shell/ScrollbarMarks.cpp
```

- [ ] **Step 8: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[scrollbar-marks]"`
Expected: zero warnings; `All tests passed` (11 test cases).

- [ ] **Step 9: Format and commit**

Run: `clang-format -i src/vtbackend/shell/ScrollbarMarks.hpp src/vtbackend/shell/ScrollbarMarks.cpp src/vtbackend/shell/ScrollbarMarks_test.cpp`

```bash
git add src/vtbackend/shell/ScrollbarMarks.hpp src/vtbackend/shell/ScrollbarMarks.cpp \
        src/vtbackend/shell/ScrollbarMarks_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: bucket scrollbar marks into pixel ticks

The pure half of the scrollbar marks (spec §6.1, contract C4): one pass over
the marks, one slot per track pixel, Failure > Running > UserMark > Command
within a pixel and the topmost mark between equals. Sources are filtered
before merging, so a hidden kind never hides a shown one.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.2: `UserMarkIndex` — the user-marked lines by stable id

**Files:**
- Create: `src/vtbackend/shell/UserMarkIndex.hpp`, `src/vtbackend/shell/UserMarkIndex_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing beyond the standard library (the pattern is `FoldState`, `src/vtbackend/shell/Folding.hpp:124-203`).
- Produces (new, not in the contract):
  ```cpp
  enum class UserMarkChange : uint8_t { Removed = 0, Added };
  class UserMarkIndex
  {
    public:
      void record(int64_t stableId, uint64_t generation, UserMarkChange change);
      [[nodiscard]] bool isCurrentFor(uint64_t generation) const noexcept;
      void rebuild(std::span<int64_t const> ids, uint64_t generation);
      void prune(int64_t floorStableId);
      [[nodiscard]] std::set<int64_t> const& ids() const noexcept;
      [[nodiscard]] uint64_t revision() const noexcept;
  };
  ```

Design: the index is a **cache**, the line's `LineFlag::UserMark` the authority. It holds the ids of one
`Grid::stableIdGeneration()`; a change reported for any other generation is ignored, because the owner (Task 8.3)
rebuilds the whole set from one grid scan the moment it sees the generation move. Eviction is handled like
`FoldState::prune` (`Folding.hpp:183`): one prefix erase against `Grid::stableRangeFloor()`.

- [ ] **Step 1: Write the failing test**

Create `src/vtbackend/shell/UserMarkIndex_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/UserMarkIndex.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <set>

using namespace vtbackend;

TEST_CASE("UserMarkIndex.recordsTheMarksOfItsGeneration", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    REQUIRE(index.isCurrentFor(0)); // a fresh grid starts at generation 0, with no marks
    REQUIRE(index.ids().empty());

    index.record(7, 0, UserMarkChange::Added);
    index.record(3, 0, UserMarkChange::Added);
    CHECK(index.ids() == std::set<int64_t> { 3, 7 });

    auto const revision = index.revision();
    index.record(7, 0, UserMarkChange::Removed);
    CHECK(index.ids() == std::set<int64_t> { 3 });
    CHECK(index.revision() > revision);

    // Removing what is not there changes nothing, and says so by leaving the revision alone.
    auto const settled = index.revision();
    index.record(7, 0, UserMarkChange::Removed);
    CHECK(index.revision() == settled);
}

TEST_CASE("UserMarkIndex.anIdFromAnotherGenerationWaitsForTheRescan", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    index.record(5, 0, UserMarkChange::Added);

    // A reflow renamed every row: generation 1's ids must never mix with generation 0's.
    index.record(9, 1, UserMarkChange::Added);
    CHECK_FALSE(index.isCurrentFor(1));
    CHECK(index.ids() == std::set<int64_t> { 5 });

    auto const revision = index.revision();
    auto const rescanned = std::array { int64_t { 12 }, int64_t { 20 } };
    index.rebuild(rescanned, 1);
    CHECK(index.isCurrentFor(1));
    CHECK(index.ids() == std::set<int64_t> { 12, 20 });
    CHECK(index.revision() > revision);
}

TEST_CASE("UserMarkIndex.pruneForgetsEvictedMarks", "[scrollbar-marks]")
{
    auto index = UserMarkIndex {};
    index.record(1, 0, UserMarkChange::Added);
    index.record(5, 0, UserMarkChange::Added);
    index.record(9, 0, UserMarkChange::Added);

    auto const revision = index.revision();
    index.prune(5);
    CHECK(index.ids() == std::set<int64_t> { 5, 9 });
    CHECK(index.revision() > revision);

    // Nothing below the floor any more: a second prune is free and silent.
    auto const settled = index.revision();
    index.prune(5);
    CHECK(index.revision() == settled);
}
```

- [ ] **Step 2: Register the test and watch it fail**

In `src/vtbackend/CMakeLists.txt`, in the `vtbackend_test` list insert after `        shell/ScrollbarMarks_test.cpp`:

```cmake
        shell/UserMarkIndex_test.cpp
```

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `fatal error C1083: Cannot open include file: 'vtbackend/shell/UserMarkIndex.hpp'`.

- [ ] **Step 3: Write the index**

Create `src/vtbackend/shell/UserMarkIndex.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <set>
#include <span>

namespace vtbackend
{

/// Whether a user mark was placed on a line or taken off it.
///
/// Named rather than a bool: at the call site in Screen::setLogicalLineFlags a bare `true` would say
/// nothing about which of the two it reports.
enum class UserMarkChange : uint8_t
{
    Removed = 0, ///< The line lost LineFlag::UserMark.
    Added,       ///< The line gained LineFlag::UserMark.
};

/// The stable ids of the primary screen's lines that carry LineFlag::UserMark (vi `mm`).
///
/// What lets the scrollbar place a tick per user mark in O(marks) instead of walking the scrollback
/// (spec §6.1). A CACHE, never the authority -- the flag on the line is. A reader therefore verifies each
/// id against its line, and the index needs to hear only about marks being toggled: a reset that clears a
/// flag behind its back costs it a stale id, never the scrollbar a wrong tick.
///
/// Ids mean something only within one Grid::stableIdGeneration(); after a reflow they name other rows. So
/// the index remembers the generation its ids belong to, ignores a change reported for any other, and is
/// rebuilt from one scan of the grid by whoever notices the mismatch (Terminal::userMarks()). Evicted ids
/// are pruned against Grid::stableRangeFloor(), exactly as FoldState prunes its heads.
class UserMarkIndex
{
  public:
    /// Records that the logical line headed by @p stableId gained or lost its mark.
    ///
    /// Ignored when @p generation is not the one the index holds: the ids it holds are then already stale,
    /// and the rebuild that replaces them reads the flag this change left on the grid.
    /// @param stableId The head line's stable id.
    /// @param generation The Grid::stableIdGeneration() @p stableId was read in.
    /// @param change Whether the mark was placed or removed.
    void record(int64_t stableId, uint64_t generation, UserMarkChange change)
    {
        if (generation != _generation)
            return;

        auto const changed =
            change == UserMarkChange::Added ? _ids.insert(stableId).second : _ids.erase(stableId) != 0;
        if (changed)
            ++_revision;
    }

    /// Whether the ids held were minted in @p generation, so no rebuild is due.
    [[nodiscard]] bool isCurrentFor(uint64_t generation) const noexcept { return _generation == generation; }

    /// Replaces every id with @p ids -- the result of scanning the grid for LineFlag::UserMark.
    /// @param ids The marked lines' stable ids, in any order.
    /// @param generation The Grid::stableIdGeneration() they were read in.
    void rebuild(std::span<int64_t const> ids, uint64_t generation)
    {
        _ids = std::set<int64_t>(ids.begin(), ids.end());
        _generation = generation;
        ++_revision;
    }

    /// Forgets every id below @p floorStableId: rows evicted from the scrollback.
    ///
    /// Cheap because the set is ordered -- one erase of a prefix, as FoldState::prune.
    /// @param floorStableId The oldest still-addressable id (Grid::stableRangeFloor()).
    void prune(int64_t floorStableId)
    {
        auto const end = _ids.lower_bound(floorStableId);
        if (end == _ids.begin())
            return;

        _ids.erase(_ids.begin(), end);
        ++_revision;
    }

    /// The marked lines' stable ids, ascending -- top of the scrollback first.
    [[nodiscard]] std::set<int64_t> const& ids() const noexcept { return _ids; }

    /// Bumped by every change to ids(); what a cache of anything derived from them keys on.
    [[nodiscard]] uint64_t revision() const noexcept { return _revision; }

  private:
    // Ordered, so prune() is one prefix erase and ids() comes out top-down.
    std::set<int64_t> _ids;
    uint64_t _generation = 0;
    uint64_t _revision = 0;
};

} // namespace vtbackend
```

- [ ] **Step 4: Register the header**

In `src/vtbackend/CMakeLists.txt`, in the header list insert after the line `    vt/TextSizing.hpp`:

```cmake
    shell/UserMarkIndex.hpp
```

- [ ] **Step 5: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[scrollbar-marks]"`
Expected: zero warnings; `All tests passed` (14 test cases).

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtbackend/shell/UserMarkIndex.hpp src/vtbackend/shell/UserMarkIndex_test.cpp`

```bash
git add src/vtbackend/shell/UserMarkIndex.hpp src/vtbackend/shell/UserMarkIndex_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: index the lines a vi mark sits on

A small ordered set of stable ids, valid for one stable-id generation and
pruned by the grid's floor, so the scrollbar can place a tick per user mark
without walking the scrollback (spec §6.1). The flag on the line stays the
authority; the index is a cache its reader verifies.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.3: Keep the user-mark index in step with the grid

**Files:**
- Create: `src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`
- Modify: `src/vtbackend/screen/Terminal.hpp`, `src/vtbackend/screen/Terminal.cpp`, `src/vtbackend/screen/Screen.cpp` (`setLogicalLineFlags`, ~line 6624), `src/vtbackend/input/vi/ViCommands.cpp` (`toggleLineMark`, ~line 293), `src/vtbackend/CMakeLists.txt`

**Interfaces:**
- Consumes: `UserMarkIndex`, `UserMarkChange` (Task 8.2); `LineFlag::UserMark` (C1); the semantic-mark funnel `Screen::setLogicalLineFlags` (`Screen.cpp:6624`) through which vi `mm` writes (`ViCommands.cpp:293`, changed by phase 1 to toggle `UserMark`); `Grid::stableIdGeneration()` / `stableRangeFloor()` / `forEachValidLine()` / `stableLineIdOf()` (`Grid.hpp:825-928`).
- Produces (new):
  ```cpp
  // Terminal (caller holds the terminal lock)
  [[nodiscard]] UserMarkIndex const& userMarks() const;                      // reconciles first
  void noteUserMarkChanged(int64_t headStableId, UserMarkChange change);     // called by Screen
  ```
  and `ViCommands::toggleLineMark()` now raises `Terminal::screenUpdated()`.

Where the set lives and how it stays correct:
- **Owner:** `Terminal`, as `mutable UserMarkIndex _userMarks` beside the fold state (both are caches reconciled from a `const` read, cf. `refreshFoldState()` at `Terminal.cpp:2481`).
- **Writes:** `Screen::setLogicalLineFlags` is the only way a semantic mark is written (`Screen.hpp:1026-1037`), so it reports every `UserMark` toggle on the primary screen. The alternate screen's marks never enter.
- **Eviction:** `userMarks()` prunes against `Grid::stableRangeFloor()` on every read (one `lower_bound`).
- **Reflow / reset / history-limit change:** these bump `Grid::stableIdGeneration()`; `userMarks()` sees the index is not current for it and rebuilds it from one `forEachValidLine` scan. `UserMark` is head-only (C1), so the mark rides the reflow on its logical line's head and the scan finds it there. The cost is the reflow's own order.
- **Anything else that clears a flag** (a line reset): the reader verifies each id against its line (Task 8.4), so the stale id costs nothing visible.

- [ ] **Step 1: Write the failing test**

Create `src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/UserMarkIndex.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <ranges>
#include <string>

using namespace vtbackend;

namespace
{

/// Puts @p mc into vi normal mode with its cursor on the page's top row.
void enterNormalModeAtTop(MockTerm<>& mc)
{
    mc.writeToScreen("\033[H");
    mc.terminal.inputHandler().setMode(ViMode::Normal);
}

/// Whether the line @p stableId names on the primary screen carries @p flag.
[[nodiscard]] bool lineHasFlag(Terminal const& terminal, int64_t stableId, LineFlag flag)
{
    auto const& grid = terminal.primaryScreen().grid();
    auto const line = grid.lineOffsetOf(stableId);
    return line.has_value() && grid.lineAt(*line).isFlagEnabled(flag);
}

/// The text of the line @p stableId names on the primary screen.
[[nodiscard]] std::string lineTextOf(Terminal const& terminal, int64_t stableId)
{
    auto const& grid = terminal.primaryScreen().grid();
    return grid.lineText(*grid.lineOffsetOf(stableId));
}

/// A MockTerm that counts the screen updates it is told about -- the signal a frontend reschedules its
/// scrollbar marks on.
struct ScreenUpdateCountingTerm final: MockTerm<>
{
    using MockTerm<>::MockTerm;

    int screenUpdates = 0;

    void screenUpdated() override { ++screenUpdates; }
};

} // namespace

TEST_CASE("ScrollbarMarks.userMarks.theIndexHearsEveryViMark", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\n");
    enterNormalModeAtTop(mc);

    mc.sendCharSequence("jmm"); // on "two"
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const id = *mc.terminal.userMarks().ids().begin();
    CHECK(lineHasFlag(mc.terminal, id, LineFlag::UserMark));
    CHECK(lineTextOf(mc.terminal, id).starts_with("two"));

    mc.sendCharSequence("mm"); // and off again
    CHECK(mc.terminal.userMarks().ids().empty());
}

TEST_CASE("ScrollbarMarks.userMarks.theAlternateScreenKeepsItsMarksToItself", "[scrollbar-marks]")
{
    // The alternate screen has no scrollback for a tick to point into.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("\033[?1049hvim\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("mm");
    CHECK(mc.terminal.userMarks().ids().empty());
}

TEST_CASE("ScrollbarMarks.userMarks.aColumnResizeRescansThem", "[scrollbar-marks]")
{
    // A column change reflows the grid and renames every row (Grid::stableIdGeneration), so the ids the
    // index held now name other lines. One scan puts it right: the mark itself rode the reflow on its
    // line's head, as every HeadOnlyLineFlags member does.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jmm"); // on "two"
    mc.terminal.inputHandler().setMode(ViMode::Insert);

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(30) });
    REQUIRE(grid.stableIdGeneration() != generation);

    auto const& index = mc.terminal.userMarks();
    CHECK(index.isCurrentFor(grid.stableIdGeneration()));
    REQUIRE(index.ids().size() == 1);
    CHECK(lineTextOf(mc.terminal, *index.ids().begin()).starts_with("two"));
}

TEST_CASE("ScrollbarMarks.userMarks.evictedMarksArePruned", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    mc.writeToScreen("one\r\ntwo\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("mm"); // on "one"
    mc.terminal.inputHandler().setMode(ViMode::Insert);
    REQUIRE(mc.terminal.userMarks().ids().size() == 1);
    auto const marked = *mc.terminal.userMarks().ids().begin();

    mc.writeToScreen("\033[99;1H");
    for (auto const i: std::views::iota(0, 20))
        mc.writeToScreen(std::format("filler {}\r\n", i));

    REQUIRE(marked < mc.terminal.primaryScreen().grid().stableRangeFloor());
    CHECK(mc.terminal.userMarks().ids().empty());
}

TEST_CASE("ScrollbarMarks.userMarks.aViMarkSaysTheScreenChanged", "[scrollbar-marks]")
{
    // A mark moves no cell and no cursor, so unless it says so the frontend learns of it -- and redraws
    // the scrollbar's ticks -- only with the next byte of output.
    auto mc = ScreenUpdateCountingTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\n");
    enterNormalModeAtTop(mc);

    auto const before = mc.screenUpdates;
    mc.sendCharSequence("mm");
    CHECK(mc.screenUpdates > before);
}
```

- [ ] **Step 2: Register the test and watch it fail**

In `src/vtbackend/CMakeLists.txt`, in the `vtbackend_test` list insert after the line `        screen/Terminal_input_test.cpp`:

```cmake
        screen/Terminal_scrollbar_marks_test.cpp
```

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'userMarks': is not a member of 'vtbackend::Terminal'`.

- [ ] **Step 3: Declare the index on `Terminal`**

In `src/vtbackend/screen/Terminal.hpp`, insert after `#include <vtbackend/shell/SemanticBlockTracker.hpp>`:

```cpp
#include <vtbackend/shell/UserMarkIndex.hpp>
```

Insert directly above the line `    /// Scrolls the viewport and extends the active selection to the boundary cell.` (the doc comment of
`performAutoScroll`; since Task 7.6 the sticky-header section's `// }}}` precedes it):

```cpp
    // {{{ Scrollbar marks (spec §6.1)

    /// The stable ids of the primary screen's user-marked lines, reconciled against the grid first.
    ///
    /// Rebuilt by one scan of the grid when the stable-id generation moved since it was last current (a
    /// reflow, a reset, a history-limit change), and pruned of ids evicted below Grid::stableRangeFloor().
    /// const for the reason refreshFoldState() is: reconciling a cache is not a change of state.
    ///
    /// Caller holds the terminal lock.
    /// @return The reconciled index.
    [[nodiscard]] UserMarkIndex const& userMarks() const;

    /// Tells the user-mark index that the logical line headed by @p headStableId gained or lost
    /// LineFlag::UserMark on the primary screen.
    ///
    /// Called by Screen::setLogicalLineFlags(), the one funnel every semantic mark is written through.
    /// @param headStableId The logical line's head, by stable id.
    /// @param change Whether the mark was placed or removed.
    void noteUserMarkChanged(int64_t headStableId, UserMarkChange change);

    // }}}

```

Insert directly above the line `    DesktopNotificationManager _desktopNotificationManager;` (~line 3016):

```cpp
    // {{{ Scrollbar marks
    /// The user-marked lines, by stable id (@see userMarks). Mutable for the reason _foldState is: the
    /// const read reconciles it against the grid.
    mutable UserMarkIndex _userMarks;
    // }}}

```

- [ ] **Step 4: Implement it**

In `src/vtbackend/screen/Terminal.cpp`, insert directly after the `// }}}` that closes the `// {{{ Output folding`
section (the line after `Terminal::snapToVisibleLine`, ~line 2986) and before `Terminal::livePromptSpan()`:

```cpp

// {{{ Scrollbar marks
UserMarkIndex const& Terminal::userMarks() const
{
    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();

    // A generation bump renamed every row, so every id held names some other line now. The marks
    // themselves survived -- UserMark is head-only and rides a reflow on its logical line's head -- so one
    // scan finds them again. Reflow has just walked the whole scrollback itself, so this adds no new order
    // of cost, and it runs once per bump rather than once per read.
    if (!_userMarks.isCurrentFor(generation))
    {
        auto ids = std::vector<int64_t> {};
        grid.forEachValidLine([&](LineOffset offset, Line const& line) {
            if (line.isFlagEnabled(LineFlag::UserMark))
                ids.push_back(grid.stableLineIdOf(offset));
        });
        _userMarks.rebuild(ids, generation);
    }

    // Marks that scrolled out of the scrollback: without this the set grows for the life of the session.
    _userMarks.prune(grid.stableRangeFloor());
    return _userMarks;
}

void Terminal::noteUserMarkChanged(int64_t headStableId, UserMarkChange change)
{
    _userMarks.record(headStableId, primaryScreen().grid().stableIdGeneration(), change);
}
// }}}
```

- [ ] **Step 5: Report every toggle from the funnel**

In `src/vtbackend/screen/Screen.cpp`, `Screen::setLogicalLineFlags` (~line 6624). Before (after phase 1, which only
added `UserMark` to `HeadOnlyLineFlags`):

```cpp
void Screen::setLogicalLineFlags(LineOffset line, LineFlags flags, bool enable) noexcept
{
    enableLineFlags(_grid.logicalLineHead(line), flags, enable);
```

After:

```cpp
void Screen::setLogicalLineFlags(LineOffset line, LineFlags flags, bool enable) noexcept
{
    auto const head = _grid.logicalLineHead(line);
    enableLineFlags(head, flags, enable);
```

and at the end of the same function, after the existing `if ((flags & HeadOnlyLineFlags).any()) _terminal->invalidateFoldRanges();`,
append:

```cpp

    // The scrollbar places one tick per user mark from an index of their stable ids rather than by walking
    // the scrollback, and this funnel is the only way a mark is written -- so it is the one place the index
    // has to hear about it. The primary screen only: the alternate screen has no scrollback to point into.
    if (flags.test(LineFlag::UserMark) && this == &_terminal->primaryScreen())
        _terminal->noteUserMarkChanged(_grid.stableLineIdOf(head),
                                       enable ? UserMarkChange::Added : UserMarkChange::Removed);
```

- [ ] **Step 6: Say the screen changed after `mm`**

In `src/vtbackend/input/vi/ViCommands.cpp`, `ViCommands::toggleLineMark()` (~line 293), append after its last
statement (`screen.setLogicalLineFlags(cursorPosition.line, LineFlag::UserMark, !marked);` since phase 1):

```cpp

    // A mark moves no cell and no cursor, so nothing else says the screen changed -- and the frontend
    // reschedules the scrollbar's ticks on screen updates. Without this a mark would reach them only with
    // the next byte of output.
    _terminal->screenUpdated();
```

- [ ] **Step 7: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[scrollbar-marks]"`
Expected: zero warnings; `All tests passed` (19 test cases). Then run the vi and folding suites, which share the
funnel: `out/build/clangcl-debug/bin/vtbackend_test.exe "[vi],[folding]"` — Expected: `All tests passed`.

- [ ] **Step 8: Format and commit**

Run: `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.cpp src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`

```bash
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Screen.cpp \
        src/vtbackend/input/vi/ViCommands.cpp src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp \
        src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: keep the user-mark index in step with the grid

Screen::setLogicalLineFlags, the one funnel every semantic mark goes
through, reports each UserMark toggle on the primary screen; Terminal
rebuilds the index with one scan after a stable-id generation bump and
prunes it by the grid's floor on every read. `mm` now raises a screen
update, so the frontend can reschedule the scrollbar's ticks.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.4: `Terminal::scrollbarMarkInputs()` — what the scrollbar should mark

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp`, `src/vtbackend/screen/Terminal.cpp`, `src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`

**Interfaces:**
- Consumes:
  - C1: `Terminal::commandBlocks() const`, `CommandBlockStore::forEachRecord(F const&) const` (oldest first), `CommandBlockStore::limits()`, `CommandBlockStore::lastFinished()`, `CommandBlockRecord::{state, headStableId, headIdGeneration}`, `CommandBlockState::Prompting`, `outcomeOf(CommandBlockRecord const&)`, `LineFlag::UserMark`.
  - **Phase 1 behaviour (README Review Focus #1, "head positions rescanned"):** after a stable-id generation bump every record whose head row survived carries `headIdGeneration == Grid::stableIdGeneration()` by the time the terminal lock is next released. A record still on an older generation is treated as having no head row. If `ScrollbarMarks.terminal.blockMarksFollowTheirHeadsThroughAColumnResize` fails with an empty input list, phase 1 does not rescan: stop and report (contract), do not add a second rescan here.
  - Folding: `Terminal::hiddenIntervals()` (`Terminal.hpp:1156`), `vtbackend::snapToVisibleId` / `vtbackend::visibleDistance` (`Folding.hpp:312,355`), `Viewport::scrollableLineCount()` (`Viewport.cpp:317`: history − hidden), which defines the scrollbar's range.
  - Grid: `historyLineCount()`, `addressableTop()`, `stableLineIdOf()`, `stableIdGeneration()`, `stableRangeFloor()`, `lineAt()`, `lineOffsetOf()` (`Grid.hpp:630-917`).
  - Task 8.1: `ScrollbarMarkInput`, `scrollbarMarkKindOf`; Task 8.3: `Terminal::userMarks()`.
- Produces (the function the task names; not in C4):
  ```cpp
  /// Caller holds the terminal lock.
  [[nodiscard]] std::vector<ScrollbarMarkInput> Terminal::scrollbarMarkInputs() const;
  ```
  Order: records oldest first, then user marks top-down. `visibleRow` is counted from the oldest history row
  (grid line `-historyLineCount()`), in visible rows; the range they lie in is
  `scrollableLineCount() + pageSize().lines` — the `historyLineCount` + `pageLineCount` the QML scrollbar sizes itself by
  (`SessionChrome.qml:108-113`).

- [ ] **Step 1: Write the failing tests**

In `src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`, add these includes to the existing include blocks:

```cpp
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
```

and append to the end of the file:

```cpp
namespace
{

/// Runs one shell command the way an OSC 133 integration marks it up: prompt start, prompt end, the echoed
/// command, output start, the output -- and the command end its precmd hook emits, unless the command is
/// still running (@p exitCode is nullopt).
void runCommand(MockTerm<>& mc,
                std::string_view command,
                std::vector<std::string> const& output,
                std::optional<int> exitCode)
{
    mc.writeToScreen(std::format("\033]133;A\033\\$ \033]133;B\033\\{}\r\n\033]133;C\033\\", command));
    for (auto const& line: output)
        mc.writeToScreen(line + "\r\n");
    if (exitCode)
        mc.writeToScreen(std::format("\033]133;D;{}\033\\", *exitCode));
}

/// The kinds of @p inputs, in the order the terminal reported them.
[[nodiscard]] std::vector<ScrollbarMarkKind> kindsOf(std::span<ScrollbarMarkInput const> inputs)
{
    return inputs | std::views::transform(&ScrollbarMarkInput::kind) | std::ranges::to<std::vector>();
}

/// The stable id of the scrollbar's row 0: the oldest history row.
[[nodiscard]] int64_t rangeTopOf(Terminal const& terminal)
{
    auto const& grid = terminal.primaryScreen().grid();
    return grid.stableLineIdOf(LineOffset::cast_from(-unbox<int>(grid.historyLineCount())));
}

/// The rows the scrollbar spans: what Viewport::scrollableLineCount() lets it scroll, plus the page.
[[nodiscard]] int64_t scrollbarRowsOf(Terminal const& terminal)
{
    return unbox<int64_t>(terminal.viewport().scrollableLineCount()) + unbox<int64_t>(terminal.pageSize().lines);
}

} // namespace

TEST_CASE("ScrollbarMarks.terminal.blocksAreMarkedAtTheirHeads", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    runCommand(mc, "ls", { "file1", "file2" }, 0);
    runCommand(mc, "false", {}, 1);
    runCommand(mc, "sleep 9", {}, std::nullopt);

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(inputs)
            == std::vector { ScrollbarMarkKind::Command, ScrollbarMarkKind::Failure, ScrollbarMarkKind::Running });

    // Oldest block first, each on its prompt line: rows 0, 3 and 4 of a page nothing has scrolled yet.
    CHECK(inputs[0].visibleRow == 0);
    CHECK(inputs[1].visibleRow == 3);
    CHECK(inputs[2].visibleRow == 4);
    for (auto const& input: inputs)
    {
        CHECK(lineHasFlag(mc.terminal, input.targetStableId, LineFlag::Marked));
        CHECK(input.visibleRow == input.targetStableId - rangeTopOf(mc.terminal));
        CHECK(input.visibleRow < scrollbarRowsOf(mc.terminal));
    }
}

TEST_CASE("ScrollbarMarks.terminal.positionsCountVisibleRows", "[scrollbar-marks]")
{
    // The scrollbar's own range counts a collapsed block as its one head row (Viewport::scrollableLineCount),
    // so the ticks must as well -- or every tick below a fold sits as many rows too low as the fold hides.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    runCommand(mc, "ls", { "file1", "file2" }, 0);
    runCommand(mc, "false", {}, 1);
    runCommand(mc, "sleep 9", {}, std::nullopt);

    REQUIRE(mc.terminal.foldRanges().size() == 1); // only `ls` printed whole lines of output
    auto const head = mc.terminal.foldRanges().front().headStableId;
    mc.terminal.foldState().collapse(head);
    REQUIRE(mc.terminal.hiddenLineCount() == LineCount(2));

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(inputs.size() == 3);
    CHECK(inputs[0].visibleRow == 0);
    CHECK(inputs[1].visibleRow == 1);
    CHECK(inputs[2].visibleRow == 2);
    CHECK(inputs[2].visibleRow < scrollbarRowsOf(mc.terminal));
}

TEST_CASE("ScrollbarMarks.terminal.blockMarksFollowTheirHeadsThroughAColumnResize", "[scrollbar-marks]")
{
    // README Review Focus #1: the window narrowed while a long command runs. The reflow renames every row;
    // phase 1 re-points each record's head, and the ticks have to follow it there.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    runCommand(mc, "ls", { "file1", "file2" }, 0);
    runCommand(mc, "make", { std::string(35, 'x'), std::string(35, 'y') }, std::nullopt);

    auto const& grid = mc.terminal.primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    mc.terminal.resizeScreen(PageSize { LineCount(10), ColumnCount(20) });
    REQUIRE(grid.stableIdGeneration() != generation);

    // Output keeps arriving after the resize, as it does under a running build.
    mc.writeToScreen("zzz\r\n");

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(inputs) == std::vector { ScrollbarMarkKind::Command, ScrollbarMarkKind::Running });
    for (auto const& input: inputs)
    {
        CHECK(lineHasFlag(mc.terminal, input.targetStableId, LineFlag::Marked));
        CHECK(lineTextOf(mc.terminal, input.targetStableId).starts_with("$ "));
        CHECK(input.visibleRow == input.targetStableId - rangeTopOf(mc.terminal));
    }
}

TEST_CASE("ScrollbarMarks.terminal.userMarksAreMarkedWhereTheyStand", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    mc.writeToScreen("one\r\ntwo\r\nthree\r\n");
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("mm");   // on "one"
    mc.sendCharSequence("jjmm"); // on "three"

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    REQUIRE(kindsOf(inputs) == std::vector { ScrollbarMarkKind::UserMark, ScrollbarMarkKind::UserMark });
    CHECK(inputs[1].visibleRow - inputs[0].visibleRow == 2);
    for (auto const& input: inputs)
        CHECK(input.visibleRow == input.targetStableId - rangeTopOf(mc.terminal));

    SECTION("a flag cleared behind the index's back is not reported")
    {
        // The index is a cache and the flag the authority: a path that clears the flag without the funnel
        // costs the index a stale id, never the scrollbar a stale tick.
        auto const& grid = mc.terminal.primaryScreen().grid();
        mc.terminal.primaryScreen().enableLineFlags(
            *grid.lineOffsetOf(inputs[1].targetStableId), LineFlag::UserMark, false);
        CHECK(kindsOf(mc.terminal.scrollbarMarkInputs()) == std::vector { ScrollbarMarkKind::UserMark });
    }
}

TEST_CASE("ScrollbarMarks.terminal.aMarkInsideACollapsedBlockStandsOnItsHead", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    runCommand(mc, "ls", { "file1", "file2" }, 0);
    runCommand(mc, "pwd", { "/tmp" }, 0);
    enterNormalModeAtTop(mc);
    mc.sendCharSequence("jmm"); // on "file1", inside the output of `ls`
    mc.terminal.inputHandler().setMode(ViMode::Insert);

    auto const before = mc.terminal.scrollbarMarkInputs();
    auto const lsHead = before.front().targetStableId; // oldest block first
    auto const marked = std::ranges::find(before, ScrollbarMarkKind::UserMark, &ScrollbarMarkInput::kind);
    REQUIRE(marked != before.end());
    auto const markedLine = marked->targetStableId;

    mc.terminal.foldState().collapse(lsHead);
    auto const after = mc.terminal.scrollbarMarkInputs();
    auto const mark = std::ranges::find(after, ScrollbarMarkKind::UserMark, &ScrollbarMarkInput::kind);
    REQUIRE(mark != after.end());
    CHECK(mark->targetStableId == markedLine);          // a click still goes to the marked line itself...
    CHECK(mark->visibleRow == after.front().visibleRow); // ...but the tick stands on the head that hides it
}

TEST_CASE("ScrollbarMarks.terminal.evictedHeadsAreDropped", "[scrollbar-marks]")
{
    // Records outlive their rows (spec §4.3); a tick for one whose head is gone would point at nothing.
    auto mc = MockTerm { PageSize { LineCount(4), ColumnCount(20) }, LineCount(4) };
    runCommand(mc, "old", { "a" }, 1);
    auto const* old = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(old != nullptr);
    auto const oldHead = old->headStableId;
    REQUIRE(mc.terminal.scrollbarMarkInputs().size() == 1);

    for (auto const i: std::views::iota(0, 20))
        mc.writeToScreen(std::format("filler {}\r\n", i));

    REQUIRE(oldHead < mc.terminal.primaryScreen().grid().stableRangeFloor());
    CHECK(mc.terminal.scrollbarMarkInputs().empty());
}

TEST_CASE("ScrollbarMarks.terminal.theAlternateScreenHasNone", "[scrollbar-marks]")
{
    // README Review Focus #2: a full-screen program over existing blocks shows no ticks, and leaving it
    // brings them all back.
    auto mc = MockTerm { PageSize { LineCount(10), ColumnCount(40) }, LineCount(100) };
    runCommand(mc, "ls", { "file1" }, 0);
    runCommand(mc, "vim", {}, std::nullopt);

    mc.writeToScreen("\033[?1049h");
    CHECK(mc.terminal.scrollbarMarkInputs().empty());

    mc.writeToScreen("\033[?1049l");
    CHECK(kindsOf(mc.terminal.scrollbarMarkInputs())
          == std::vector { ScrollbarMarkKind::Command, ScrollbarMarkKind::Running });
}

TEST_CASE("ScrollbarMarks.terminal.noShellIntegrationNoMarks", "[scrollbar-marks]")
{
    // README Review Focus #5: with no shell integration there is nothing to mark.
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(40) }, LineCount(100) };

    SECTION("plain output")
    {
        for (auto const i: std::views::iota(0, 50))
            mc.writeToScreen(std::format("plain {}\r\n", i));
        CHECK(mc.terminal.commandBlocks().size() == 0);
        CHECK(mc.terminal.scrollbarMarkInputs().empty());
    }

    SECTION("a prompt nobody has run anything from")
    {
        mc.writeToScreen("\033]133;A\033\\$ ");
        CHECK(mc.terminal.scrollbarMarkInputs().empty());
    }
}

TEST_CASE("ScrollbarMarks.terminal.aFloodOfBlocksStaysBounded", "[scrollbar-marks]")
{
    // README Review Focus #4 at the terminal's edge: 3000 OSC 133 cycles leave at most maxRecords marks,
    // every one inside the scrollbar's range, and the ticks at most a track's worth.
    auto mc = MockTerm { PageSize { LineCount(24), ColumnCount(40) }, LineCount(10'000) };
    for (auto const i: std::views::iota(0, 3000))
        mc.writeToScreen(std::format(
            "\033]133;A\033\\$ \033]133;B\033\\cmd {}\r\n\033]133;C\033\\out\r\n\033]133;D;{}\033\\", i, i % 2));

    auto const inputs = mc.terminal.scrollbarMarkInputs();
    CHECK(inputs.size() <= mc.terminal.commandBlocks().limits().maxRecords);

    auto const rows = scrollbarRowsOf(mc.terminal);
    CHECK(std::ranges::all_of(inputs, [&](ScrollbarMarkInput const& input) {
        return input.visibleRow >= 0 && input.visibleRow < rows;
    }));
    CHECK(scrollbarMarks(inputs, rows, 600, AllScrollbarMarkSources).size() <= 600);
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'scrollbarMarkInputs': is not a member of 'vtbackend::Terminal'`.

- [ ] **Step 3: Declare it**

In `src/vtbackend/screen/Terminal.hpp`, insert after `#include <vtbackend/shell/MarkArbiter.hpp>`:

```cpp
#include <vtbackend/shell/ScrollbarMarks.hpp>
```

In the `// {{{ Scrollbar marks (spec §6.1)` section added by Task 8.3, insert directly above
`    /// The stable ids of the primary screen's user-marked lines, reconciled against the grid first.`:

```cpp
    /// The marks the scrollbar may draw as ticks: one per command block whose head is still in the
    /// scrollback, then one per vi user mark.
    ///
    /// Positions are VISIBLE rows counted from the oldest history row -- the range the scrollbar itself
    /// spans (Viewport::scrollableLineCount() plus the page) -- so a collapsed fold counts as its one head
    /// row, and a user mark a fold hides stands on that head. O(records + marks): heads come from each
    /// record's cached headStableId and marks from userMarks(), never from a walk of the grid.
    ///
    /// Left out: blocks still at their prompt (no command yet), heads evicted below the grid's floor or
    /// minted in an earlier stable-id generation, and everything while the alternate screen is up.
    ///
    /// Caller holds the terminal lock.
    /// @return The marks, records oldest first and then user marks top-down; not yet bucketed nor filtered
    ///         by `scrollbar.marks` (@see scrollbarMarks()).
    [[nodiscard]] std::vector<ScrollbarMarkInput> scrollbarMarkInputs() const;

```

- [ ] **Step 4: Implement it**

In `src/vtbackend/screen/Terminal.cpp`, inside the `// {{{ Scrollbar marks` section added by Task 8.3, insert directly
before `UserMarkIndex const& Terminal::userMarks() const`:

```cpp
std::vector<ScrollbarMarkInput> Terminal::scrollbarMarkInputs() const
{
    // The alternate screen keeps no history and no marks: the scrollbar over it has nothing to point into.
    if (!isPrimaryScreen())
        return {};

    auto const& grid = primaryScreen().grid();
    auto const generation = grid.stableIdGeneration();
    auto const base = grid.stableLineIdOf(LineOffset(0));

    // Row 0 is the oldest HISTORY row, however much of it is still addressable: that is where the viewport
    // measures its own range from (Viewport::scrollableLineCount), and a tick has to agree with the handle
    // about which row a fraction of the track names.
    auto const rangeTop = grid.stableLineIdOf(LineOffset::cast_from(-unbox<int>(grid.historyLineCount())));
    auto const firstAddressable = grid.stableLineIdOf(grid.addressableTop());
    auto const lastAddressable = base + unbox<int64_t>(pageSize().lines) - 1;
    auto const isAddressable = [&](int64_t id) {
        return firstAddressable <= id && id <= lastAddressable;
    };

    // Empty while nothing is collapsed, and then every distance below is plain subtraction.
    auto const hidden = hiddenIntervals();
    auto const visibleRowOf = [&](int64_t id) {
        // A mark a collapsed fold hides is drawn nowhere, so its tick stands on the head that hides it.
        auto const shown = vtbackend::snapToVisibleId(hidden, id, VerticalDirection::Up);
        return vtbackend::visibleDistance(hidden, rangeTop, shown);
    };

    auto inputs = std::vector<ScrollbarMarkInput> {};

    // O(records), never O(scrollback): each record caches its head's stable id.
    commandBlocks().forEachRecord([&](CommandBlockRecord const& record) {
        // A prompt nobody has run anything from is not a command yet.
        if (record.state == CommandBlockState::Prompting)
            return;

        // A head minted before the last reflow names some other row now; one below the floor names none.
        if (record.headIdGeneration != generation || !isAddressable(record.headStableId))
            return;

        inputs.push_back(ScrollbarMarkInput { .visibleRow = visibleRowOf(record.headStableId),
                                              .kind = scrollbarMarkKindOf(outcomeOf(record)),
                                              .targetStableId = record.headStableId });
    });

    for (auto const id: userMarks().ids())
    {
        // The index is a cache and the flag the authority: a line reset behind the index's back costs it a
        // stale id, never the scrollbar a stale tick.
        if (!isAddressable(id)
            || !grid.lineAt(LineOffset::cast_from(id - base)).isFlagEnabled(LineFlag::UserMark))
            continue;

        inputs.push_back(ScrollbarMarkInput {
            .visibleRow = visibleRowOf(id), .kind = ScrollbarMarkKind::UserMark, .targetStableId = id });
    }

    return inputs;
}

```

(`vtbackend::visibleDistance` is qualified on purpose: inside `Terminal` the member `Terminal::visibleDistance(LineOffset, LineOffset)`
would hide the free function, as `ensureFoldProjection()` already works around at `Terminal.cpp:2833`.)

- [ ] **Step 5: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[scrollbar-marks]"`
Expected: zero warnings; `All tests passed` (28 test cases). If only
`blockMarksFollowTheirHeadsThroughAColumnResize` fails, with `kindsOf(inputs)` empty, see the phase-1 note in
**Interfaces** — stop and report.

- [ ] **Step 6: Format and commit**

Run: `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`

```bash
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp \
        src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp
git commit -F - <<'EOF'
vtbackend: report the marks a scrollbar should draw

Terminal::scrollbarMarkInputs() lists one mark per command block whose head
is still addressable and one per vi user mark, positioned in visible rows
from the oldest history row -- the range the scrollbar spans -- so a
collapsed fold is one row and a mark it hides stands on its head.
O(records + marks); nothing on the alternate screen.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.5: Jump target — `Terminal::revealStableLineAtTop()`

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp`, `src/vtbackend/screen/Terminal.cpp`, `src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`

**Interfaces:**
- Consumes: phase 7 (Task 7.8) `void Viewport::scrollLineToTop(LineOffset line);` — the public face of the private
  `Viewport::scrollOffsetForTopLine()`, the only correct "line to top" once folds are collapsed — and
  `enum class CommandBlockJumpError : uint8_t { UnknownBlock = 0, HeadEvicted };`,
  `[[nodiscard]] std::expected<void, CommandBlockJumpError> Terminal::scrollToCommandBlockHead(CommandBlockId id);`
  (refactored in Step 6 below, signature unchanged); `Terminal::expandFoldContaining(LineOffset)`, which opens the
  fold whose range covers a line, the head included; `Terminal::resetSmoothScroll()`; `Grid::lineOffsetOf()`.
- Produces (new):
  ```cpp
  enum class RevealOutcome : uint8_t { Unreachable = 0, Revealed };            // Terminal.hpp, namespace scope
  RevealOutcome Terminal::revealStableLineAtTop(int64_t stableId, uint64_t stableIdGeneration); // lock held
  ```
  **One reveal path.** Phase 7's sticky-header click ("scrolls the viewport to the block's head, expanding a
  collapsed fold", spec §6.2) is the same operation as a tick click. This task does NOT add a second
  `Viewport::scrollLineToTop` — phase 7 created it — and it re-bases phase 7's `Terminal::scrollToCommandBlockHead()`
  on `revealStableLineAtTop()` (Step 6), so the header press and the tick click scroll through one function.

- [ ] **Step 1: Write the failing tests**

Append to `src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`:

```cpp
TEST_CASE("ScrollbarMarks.reveal.putsTheLineOnTopAndOpensItsFold", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(100) };
    runCommand(mc, "ls", { "file1", "file2", "file3" }, 0);
    for (auto const i: std::views::iota(0, 30))
        mc.writeToScreen(std::format("filler {}\r\n", i));

    auto& terminal = mc.terminal;
    auto const& grid = terminal.primaryScreen().grid();
    auto const* ls = terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = ls->headStableId;

    SECTION("an expanded block")
    {
        CHECK(terminal.revealStableLineAtTop(head, grid.stableIdGeneration()) == RevealOutcome::Revealed);
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == head);
    }

    SECTION("a collapsed block is opened first, then brought to the top")
    {
        REQUIRE(std::ranges::any_of(terminal.foldRanges(),
                                    [&](FoldRange const& range) { return range.headStableId == head; }));
        terminal.foldState().collapse(head);

        CHECK(terminal.revealStableLineAtTop(head, grid.stableIdGeneration()) == RevealOutcome::Revealed);
        CHECK_FALSE(terminal.foldState().isCollapsed(head));
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == head);
    }

    SECTION("an id read in another generation is refused, and nothing moves")
    {
        auto const before = terminal.viewport().scrollOffset();
        CHECK(terminal.revealStableLineAtTop(head, grid.stableIdGeneration() + 1) == RevealOutcome::Unreachable);
        CHECK(terminal.viewport().scrollOffset() == before);
    }

    SECTION("the alternate screen is refused")
    {
        mc.writeToScreen("\033[?1049h");
        CHECK(terminal.revealStableLineAtTop(head, grid.stableIdGeneration()) == RevealOutcome::Unreachable);
    }
}

TEST_CASE("ScrollbarMarks.reveal.anEvictedLineIsUnreachable", "[scrollbar-marks]")
{
    auto mc = MockTerm { PageSize { LineCount(5), ColumnCount(20) }, LineCount(5) };
    runCommand(mc, "ls", { "file1" }, 0);
    auto const* ls = mc.terminal.commandBlocks().lastFinished();
    REQUIRE(ls != nullptr);
    auto const head = ls->headStableId;

    for (auto const i: std::views::iota(0, 30))
        mc.writeToScreen(std::format("filler {}\r\n", i));

    auto const& grid = mc.terminal.primaryScreen().grid();
    REQUIRE(head < grid.stableRangeFloor());
    CHECK(mc.terminal.revealStableLineAtTop(head, grid.stableIdGeneration()) == RevealOutcome::Unreachable);
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — `error C2039: 'revealStableLineAtTop': is not a member of 'vtbackend::Terminal'`.

- [ ] **Step 3: Confirm phase 7's "line to top" is there**

Run: `git grep -n "void scrollLineToTop(LineOffset line);" -- src/vtbackend/screen/Viewport.hpp` and
`git grep -n "scrollToCommandBlockHead" -- src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp`
Expected: one declaration in `Viewport.hpp`; the declaration and the definition of
`Terminal::scrollToCommandBlockHead` (phase 7, Task 7.8). Missing either is a stop-and-report — do not add a
`Viewport::scrollLineToTop` here.

- [ ] **Step 4: Declare the reveal on `Terminal`**

In `src/vtbackend/screen/Terminal.hpp`, insert directly after the `ScrollPhase` enum (after its closing `};`, ~line 89):

```cpp

/// What revealing a line by its stable id achieved.
enum class RevealOutcome : uint8_t
{
    Unreachable = 0, ///< The id names no row of the primary screen any more, or the alternate screen is up.
    Revealed,        ///< The line is the viewport's top row, or as near to it as the scrollback allows.
};
```

In the `// {{{ Scrollbar marks (spec §6.1)` section, insert directly above its closing `    // }}}`:

```cpp
    /// Scrolls the viewport so the line @p stableId names is its top row, opening first the collapsed fold
    /// that hides that line or hangs off it.
    ///
    /// What a click on a scrollbar tick does, and -- through scrollToCommandBlockHead() -- a press on the
    /// sticky header: the one reveal path. Refused, rather than guessed at, for an id read in another
    /// stable-id generation: after a reflow it names some other row, and scrolling there would be worse
    /// than doing nothing.
    ///
    /// Caller holds the terminal lock.
    /// @param stableId The line's stable id on the primary screen.
    /// @param stableIdGeneration The Grid::stableIdGeneration() @p stableId was read in.
    /// @return Revealed, or Unreachable when the id no longer names a row or the alternate screen is up.
    RevealOutcome revealStableLineAtTop(int64_t stableId, uint64_t stableIdGeneration);

```

- [ ] **Step 5: Implement it**

In `src/vtbackend/screen/Terminal.cpp`, inside the `// {{{ Scrollbar marks` section, insert directly before its closing `// }}}`:

```cpp

RevealOutcome Terminal::revealStableLineAtTop(int64_t stableId, uint64_t stableIdGeneration)
{
    auto const& grid = primaryScreen().grid();

    // The alternate screen does not scroll; and an id from another generation names some other row now.
    if (!isPrimaryScreen() || stableIdGeneration != grid.stableIdGeneration())
        return RevealOutcome::Unreachable;

    auto const line = grid.lineOffsetOf(stableId);
    if (!line || *line < grid.addressableTop())
        return RevealOutcome::Unreachable;

    // Before scrolling, because opening a fold changes which offset puts the line on top: a block's head
    // stays drawn while collapsed, but a user mark inside one is drawn nowhere until it opens.
    expandFoldContaining(*line);

    resetSmoothScroll();
    _viewport.scrollLineToTop(*line);
    return RevealOutcome::Revealed;
}
```

- [ ] **Step 6: Re-base phase 7's jump on the one reveal path**

Phase 7's `Terminal::scrollToCommandBlockHead()` (Task 7.8) expands the fold and scrolls on its own. Make it delegate
the scrolling to `revealStableLineAtTop()`, keeping its signature, its `CommandBlockJumpError` and its tests.

In `src/vtbackend/screen/Terminal.cpp`, replace phase 7's definition

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

with

```cpp
std::expected<void, CommandBlockJumpError> Terminal::scrollToCommandBlockHead(CommandBlockId id)
{
    // commandBlocks() re-records the heads first, so the record's cached position is current.
    auto const* const record = commandBlocks().find(id);
    if (record == nullptr)
        return std::unexpected { CommandBlockJumpError::UnknownBlock };

    // The one reveal path, shared with the scrollbar ticks: it opens the fold hanging off the head before
    // scrolling (a collapsed fold changes which offset puts the head on top, and a user who asked to see a
    // command asked to see what it printed as well). It refuses a head the scrollback no longer holds --
    // and the alternate screen, where no header is ever drawn (stickyHeaderBlock() is gated on the primary
    // page), so HeadEvicted is the only refusal a header press can meet.
    if (revealStableLineAtTop(record->headStableId, record->headIdGeneration) == RevealOutcome::Unreachable)
        return std::unexpected { CommandBlockJumpError::HeadEvicted };
    return {};
}
```

In `src/vtbackend/screen/Terminal.hpp`, in phase 7's doc comment of `scrollToCommandBlockHead`, replace

```cpp
    /// What a press on the sticky header does, and what a scrollbar tick does. The head is located
    /// through the record's cached position, so the jump costs the same for a block a million rows up.
```

with

```cpp
    /// What a press on the sticky header does. The head is located through the record's cached position,
    /// so the jump costs the same for a block a million rows up; the scrolling itself is
    /// revealStableLineAtTop(), the one reveal path, which a scrollbar tick calls directly.
```

- [ ] **Step 7: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test && out/build/clangcl-debug/bin/vtbackend_test.exe "[scrollbar-marks]"`
Expected: zero warnings; `All tests passed` (30 test cases). Then phase 7's jump and the viewport's own suites, which
prove the re-based `scrollToCommandBlockHead()` unchanged:
`out/build/clangcl-debug/bin/vtbackend_test.exe "[stickyheader]"` and
`out/build/clangcl-debug/bin/vtbackend_test.exe "[folding],[viewport]"` — Expected: `All tests passed` for both.

- [ ] **Step 8: Format and commit**

Run: `clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp`

```bash
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp \
        src/vtbackend/screen/Terminal_scrollbar_marks_test.cpp
git commit -F - <<'EOF'
vtbackend: reveal a line by stable id at the viewport's top

What a click on a scrollbar tick does: open the fold that hides or hangs
off the line, then scroll it to the top through phase 7's fold-aware
Viewport::scrollLineToTop(). An id from another stable-id generation, an
evicted one, or the alternate screen is refused. The sticky header's
scrollToCommandBlockHead() now scrolls through it too, so there is one
reveal path.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.6: Config — `scrollbar.marks` (C9 row)

**Files:**
- Modify: `src/contour/config/Config.hpp` (`ScrollBarConfig` ~line 409, `YAMLConfigReader` declarations ~line 1633, `Writer::format(…, ScrollBarConfig const&)` ~line 1956), `src/contour/config/Config.cpp` (`loadFromEntry(…, ScrollBarConfig&)` ~line 2250), `src/contour/config/ConfigEnum.hpp`, `src/contour/config/ConfigDocumentation.hpp` (`ScrollbarConfig` ~line 425, `ScrollbarWeb` ~line 2143), `src/contour/config/Config_test.cpp`

**Interfaces:**
- Consumes: `vtbackend::ScrollbarMarkSource`, `ScrollbarMarkSources`, `AllScrollbarMarkSources` (Task 8.1); the token-table
  machinery `ConfigEnumInfo` / `configEnumValues` / `configEnumFromToken` (`ConfigEnum.hpp:29-98`); `emitProfileYaml`
  (`Config.hpp:2363`).
- Produces (C9 row `scrollbar.marks`, profile scope):
  ```cpp
  struct ScrollBarConfig { …; vtbackend::ScrollbarMarkSources marks { vtbackend::AllScrollbarMarkSources }; };
  template <> constexpr std::span<ConfigEnumInfo<vtbackend::ScrollbarMarkSource> const> configEnumValues() noexcept;
  void YAMLConfigReader::loadFromEntry(YAML::Node const&, std::string const&, vtbackend::ScrollbarMarkSources&);
  [[nodiscard]] static std::string Writer::format(vtbackend::ScrollbarMarkSources sources);
  ```
  YAML: `scrollbar: { marks: [failures, commands, user_marks] }` (default), `[]` disables, tokens case-insensitive.
  **Mapping:** none into `vtbackend::Settings` — the terminal reports every mark and does not consume the setting;
  `TerminalSession` reads `profile().scrollbar.value().marks` (Task 8.7). Phase 10 builds the settings page's three
  checkboxes from the token table added here.

- [ ] **Step 1: Write the failing tests**

In `src/contour/config/Config_test.cpp`, insert directly after the closing brace of
`TEST_CASE("Config: scrollbar, status line, history and permissions load from YAML", "[config]")` (~line 958):

```cpp
TEST_CASE("Config: scrollbar.marks loads a list of mark sources", "[config][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkSource;
    using vtbackend::ScrollbarMarkSources;
    QTemporaryDir dir;

    // The profile's scrollbar.marks after loading a scrollbar block whose marks line is @p marksLine.
    auto const marksAfter = [&](std::string_view marksLine) {
        auto const config = loadFromYaml(dir,
                                         std::format(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: Right
            {}
)",
                                                     marksLine));
        auto const* profile = config.profile("main");
        REQUIRE(profile != nullptr);
        return profile->scrollbar.value().marks;
    };

    SECTION("absent: every source, the default")
    {
        CHECK(marksAfter("") == vtbackend::AllScrollbarMarkSources);
    }

    SECTION("a list names exactly the sources it lists, in any case")
    {
        CHECK(marksAfter("marks: [failures, user_marks]")
              == ScrollbarMarkSources { ScrollbarMarkSource::Failures, ScrollbarMarkSource::UserMarks });
        CHECK(marksAfter("marks: [Commands]") == ScrollbarMarkSources { ScrollbarMarkSource::Commands });
    }

    SECTION("[] switches every tick off")
    {
        CHECK(marksAfter("marks: []").none());
    }

    SECTION("an unknown token is reported and skipped; the rest still load")
    {
        auto capture = core::log::ScopedCapture { "error" };
        CHECK(marksAfter("marks: [failures, sparkles]") == ScrollbarMarkSources { ScrollbarMarkSource::Failures });
        CHECK(capture.contains("sparkles"));
    }

    SECTION("a scalar is not a list: reported, and the setting is left alone")
    {
        auto capture = core::log::ScopedCapture { "error" };
        CHECK(marksAfter("marks: failures") == vtbackend::AllScrollbarMarkSources);
        CHECK(capture.contains("marks"));
    }
}

TEST_CASE("Config: scrollbar.marks round-trips through the writer", "[config][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkSource;
    using vtbackend::ScrollbarMarkSources;

    // `contour generate config` spells the default out, so a user editing it sees every token.
    CHECK(contour::config::defaultConfigString().contains("marks: [failures, commands, user_marks]"));

    auto const cases = std::array {
        ScrollbarMarkSources {},
        ScrollbarMarkSources { ScrollbarMarkSource::Failures },
        ScrollbarMarkSources { ScrollbarMarkSource::Commands, ScrollbarMarkSource::UserMarks },
        vtbackend::AllScrollbarMarkSources,
    };
    for (auto const marks: cases)
    {
        INFO("marks bits: " << static_cast<int>(marks.value()));
        QTemporaryDir dir;
        auto const source = loadFromYaml(dir, "default_profile: main\n");
        auto profile = *source.findProfile("main");
        profile.scrollbar.value().marks = marks;
        writeSideFile(dir, "profiles/roundtrip.yml", contour::config::emitProfileYaml(profile));

        auto const reloaded = loadFromYaml(dir, "default_profile: main\n");
        auto const* roundTripped = reloaded.findProfile("roundtrip");
        REQUIRE(roundTripped != nullptr);
        CHECK(roundTripped->scrollbar.value().marks == marks);
    }
}
```

Add `#include <array>` to the file's standard-library include block (it is not there yet).

- [ ] **Step 2: Run them and watch them fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `error C2039: 'marks': is not a member of 'contour::config::ScrollBarConfig'`.

- [ ] **Step 3: The token table**

In `src/contour/config/ConfigEnum.hpp`, insert after `#include <vtbackend/shell/MarkArbiter.hpp>`:

```cpp
#include <vtbackend/shell/ScrollbarMarks.hpp>
```

Inside `namespace detail { … }`, directly after the last table in `namespace detail` (`StickyHeaderModeTable` since Task 7.10), insert:

```cpp

    /// The groups of marks the scrollbar can show (`scrollbar.marks`).
    ///
    /// Owned by vtbackend, so the table hangs off the type from here, as FoldJumpBehavior's does: the
    /// reader, the written-back list and the settings page's checkboxes all come from these rows.
    inline constexpr auto ScrollbarMarkSourceTable = std::array {
        ConfigEnumInfo<vtbackend::ScrollbarMarkSource> {
            vtbackend::ScrollbarMarkSource::Failures, "failures", "Failed commands" },
        ConfigEnumInfo<vtbackend::ScrollbarMarkSource> {
            vtbackend::ScrollbarMarkSource::Commands, "commands", "Other commands" },
        ConfigEnumInfo<vtbackend::ScrollbarMarkSource> {
            vtbackend::ScrollbarMarkSource::UserMarks, "user_marks", "Vi line marks" },
    };
```

After the last `configEnumValues` specialization (the `StickyHeaderMode` one since Task 7.10), directly before `} // namespace contour::config`, insert:

```cpp

template <>
constexpr std::span<ConfigEnumInfo<vtbackend::ScrollbarMarkSource> const> configEnumValues() noexcept
{
    return detail::ScrollbarMarkSourceTable;
}
```

- [ ] **Step 4: The field, the reader declaration and the writer**

In `src/contour/config/Config.hpp`, add `#include <vtbackend/shell/ScrollbarMarks.hpp>` after
`#include <vtbackend/shell/MarkArbiter.hpp>`, then replace `struct ScrollBarConfig` (~line 409). Before:

```cpp
struct ScrollBarConfig
{
    ScrollBarPosition position { ScrollBarPosition::Hidden };
    bool hideScrollbarInAltScreen { true };
};
```

After:

```cpp
struct ScrollBarConfig
{
    ScrollBarPosition position { ScrollBarPosition::Hidden };
    bool hideScrollbarInAltScreen { true };

    /// Which marks the scrollbar draws as ticks (`scrollbar.marks`: a YAML list of `failures`, `commands`,
    /// `user_marks`; `[]` draws none).
    ///
    /// Read by TerminalSession, which filters the terminal's marks with it. The terminal itself reports
    /// every mark and never consults this, so it is not mapped into vtbackend::Settings.
    vtbackend::ScrollbarMarkSources marks { vtbackend::AllScrollbarMarkSources };
};
```

In `struct YAMLConfigReader`, directly after
`    void loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where);` (~line 1633), insert:

```cpp
    /// Loads @p entry as a YAML list of scrollbar mark sources; left unchanged when absent or not a list.
    /// @param node The mapping that holds @p entry.
    /// @param entry The key to read (`marks`).
    /// @param where The sources to fill.
    void loadFromEntry(YAML::Node const& node,
                       std::string const& entry,
                       vtbackend::ScrollbarMarkSources& where);
```

In `struct Writer`, replace `format(std::string_view doc, ScrollBarConfig const& v)` (~line 1956). Before:

```cpp
    [[nodiscard]] std::string format(std::string_view doc, ScrollBarConfig const& v)
    {
        return format(doc, v.position, v.hideScrollbarInAltScreen);
    }
```

After:

```cpp
    [[nodiscard]] std::string format(std::string_view doc, ScrollBarConfig const& v)
    {
        return format(doc, v.position, v.hideScrollbarInAltScreen, format(v.marks));
    }

    /// `scrollbar.marks` as the YAML flow list the reader accepts, spelled from the same token table.
    /// @param sources The configured sources.
    /// @return E.g. `[failures, commands, user_marks]`, or `[]`.
    [[nodiscard]] static std::string format(vtbackend::ScrollbarMarkSources sources)
    {
        auto tokens = std::vector<std::string_view> {};
        for (auto const& info: configEnumValues<vtbackend::ScrollbarMarkSource>())
            if (sources.test(info.value))
                tokens.push_back(info.token);
        return std::format("[{}]", core::views::joinWith(tokens, ", "));
    }
```

- [ ] **Step 5: The reader**

In `src/contour/config/Config.cpp`, replace `YAMLConfigReader::loadFromEntry(…, ScrollBarConfig& where)` (~line 2250).
Before:

```cpp
void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "position", where.position);
        loadFromEntry(child, "hide_in_alt_screen", where.hideScrollbarInAltScreen);
    }
}
```

After:

```cpp
void YAMLConfigReader::loadFromEntry(YAML::Node const& node, std::string const& entry, ScrollBarConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "position", where.position);
        loadFromEntry(child, "hide_in_alt_screen", where.hideScrollbarInAltScreen);
        loadFromEntry(child, "marks", where.marks);
    }
}

void YAMLConfigReader::loadFromEntry(YAML::Node const& node,
                                     std::string const& entry,
                                     vtbackend::ScrollbarMarkSources& where)
{
    auto const child = node[entry];
    if (!child)
        return;

    // A LIST of tokens, each naming one group of marks. `[]` is a deliberate "none", not a missing key, so an
    // empty sequence replaces the default rather than leaving it in place.
    if (!child.IsSequence())
    {
        errorLog()("{} must be a list of failures, commands and user_marks; left unchanged.", entry);
        return;
    }

    auto sources = vtbackend::ScrollbarMarkSources {};
    for (auto const& item: child)
    {
        if (!item.IsScalar())
        {
            errorLog()("{} lists something that is not a token; ignored.", entry);
            continue;
        }

        auto const token = item.as<std::string>();
        logger()("Loading entry: {}, value {}", entry, token);
        // Through the shared token table, so a token this accepts is one the writer and the settings page
        // spell the same way.
        if (auto const source = configEnumFromToken<vtbackend::ScrollbarMarkSource>(token))
            sources.enable(*source);
        else
            errorLog()("Unknown value in {}: '{}'; ignored.", entry, token);
    }
    where = sources;
}
```

- [ ] **Step 6: Document it**

In `src/contour/config/ConfigDocumentation.hpp`, replace `constexpr StringLiteral ScrollbarConfig` (~line 425). Before:

```cpp
constexpr StringLiteral ScrollbarConfig {

    "scrollbar:\n"
    "    {comment} scroll bar position: Left, Right, Hidden (ignore-case)\n"
    "    position: {}\n"
    "    {comment} whether or not to hide the scrollbar when in alt-screen.\n"
    "    hide_in_alt_screen: {}\n"
    "\n"

};
```

After:

```cpp
constexpr StringLiteral ScrollbarConfig {

    "scrollbar:\n"
    "    {comment} scroll bar position: Left, Right, Hidden (ignore-case)\n"
    "    position: {}\n"
    "    {comment} whether or not to hide the scrollbar when in alt-screen.\n"
    "    hide_in_alt_screen: {}\n"
    "    {comment} Which marks the scrollbar shows as ticks; clicking one scrolls its line to the top.\n"
    "    {comment} A list of:\n"
    "    {comment}   failures   - commands that exited with an error\n"
    "    {comment}   commands   - every other command, finished or still running\n"
    "    {comment}   user_marks - lines marked with `mm` in vi normal mode\n"
    "    {comment} [] shows none. The first two need shell integration (OSC 133).\n"
    "    marks: {}\n"
    "\n"

};
```

In `constexpr StringLiteral ScrollbarWeb` (~line 2143), replace the two lines

```cpp
    "      hide_in_alt_screen: true\n"
    "```\n"
```

with

```cpp
    "      hide_in_alt_screen: true\n"
    "      marks: [failures, commands, user_marks]\n"
    "```\n"
```

and insert directly before that literal's closing `"\n"` line (after the `==hide_in_alt_screen==` paragraph):

```cpp
    ":octicons-horizontal-rule-16: ==marks== Which marks the scrollbar shows as thin ticks; clicking a tick "
    "scrolls its line to the top of the view, opening it if it is folded. A list of `failures` (commands "
    "that exited with an error), `commands` (every other command, finished or still running) and "
    "`user_marks` (lines marked with `mm` in vi normal mode); `[]` shows none. Command ticks need shell "
    "integration (OSC 133). Ticks are hidden whenever the scrollbar is. <br/>\n"
```

- [ ] **Step 7: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"`
Expected: zero warnings; `All tests passed` — including `the generated default config loads back into the defaults`,
whose empty error log proves the reader accepts what the writer emits. (Unbuildable here? See the note under
**GUI test runs** at the top.)

- [ ] **Step 8: Format and commit**

Run: `clang-format -i src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigEnum.hpp src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp`

```bash
git add src/contour/config/Config.hpp src/contour/config/Config.cpp src/contour/config/ConfigEnum.hpp \
        src/contour/config/ConfigDocumentation.hpp src/contour/config/Config_test.cpp
git commit -F - <<'EOF'
config: add scrollbar.marks

A per-profile YAML list of failures, commands and user_marks (default: all
three; [] disables), read and written through one token table so the
reader, `contour generate config` and the settings page cannot disagree.
Documented in the generated config and the website reference.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.6a: Show the scrollbar on the right by default (owner decision)

The owner decided (spec §6.1 as amended) that `scrollbar.position` defaults to `Right` instead of `Hidden`, so the
marks this phase adds are visible out of the box. `position: hidden` in a profile, or the settings page's
*Scrollbar position* row (`SettingsController.cpp:488-501`, unchanged), turns it off.

What the change does to the GUI — checked against the tree, so no geometry code changes here:
- **No page width is taken.** The bar is an overlay (`SessionChrome.qml:117-120`: "Overlay only — no gutter is
  reserved, so the grid/window geometry is untouched"; `implicitWidth: 12`, `padding: 2`). Nothing in
  `src/contour/geometry/`, `display/` or `window/` reads `ScrollBarConfig`; its only readers are
  `TerminalSession::getIsScrollbarRight/Visible()` (`TerminalSession.hpp:353-367`) and the settings row. Page size,
  window size and the gutter (left edge) are unchanged.
- **Shown only while there is history** (`vbar.hasScrollback`, `SessionChrome.qml:104,142`), and never on the
  alternate screen while `hide_in_alt_screen` keeps its default `true` (`TerminalSession.hpp:358-367`), so vim, less
  and htop keep their last column.
- **What it does cover:** while shown, its 12-logical-pixel strip floats over the right edge of the last column — the
  thumb (opacity 0.28 at rest) is drawn over that column's glyphs where it sits, and a press or hover anywhere in the
  strip goes to the bar, not to the grid (a selection started there, mouse reporting on the primary screen, hyperlink
  hover). The sticky header's chip stays `StickyHeaderChipInset` (2) columns clear of the right edge (phase 7), so the
  thumb never covers it; a press in the strip on row 0 still goes to the bar, as on every other row. Accepted
  with the decision; `position: hidden` restores today's behaviour.

Tests and fixtures that pinned the old default (real tree, before this phase):
- `src/contour/config/Config_test.cpp:1992` (`Config: invalid enum values fall back and do not abort loading`) asserts
  `position == ScrollBarPosition::Hidden` after an unknown `position: Sideways` — it pinned the compiled-in default.
  Adapted in Step 5 to compare against `ScrollBarConfig {}.position`: same intent (an unknown literal leaves the
  default), no second place that pins the value.
- Nothing else pins `hidden`. `Config_test.cpp:917,940` and `src/contour/test/e2e/coverage-config.yml.in:50` set
  `position: Right` explicitly. The QML stand-ins `MockSession` (`src/contour/test/QmlComponents_test.cpp:385`,
  `_scrollbarVisible = false`) and `KeyboardRouting_test.cpp:123` (`isScrollbarVisible()` → `false`) model a
  session's *state*, not the config default, and their tests are about something else (bar geometry, which turns the
  bar on explicitly through `createScrollableSession()`; keyboard focus) — they stay as they are. No GUI test hosts
  `SessionChrome` with a real `TerminalSession` and scrollback (`DisplayRendering_test.cpp` hosts a bare
  `TerminalDisplay`, the `Main.qml` tests use stand-ins), so no test needs `position: hidden` set to keep its intent;
  Step 6 re-checks that against whatever phases 1–8 added. There is no shipped `contour.yml`: the default config is
  generated (`defaultConfigString()`), and the website's profile reference is generated from `ScrollbarWeb`
  (`.github/workflows/docs.yml:82-83`), both changed in Step 4.

**Files:**
- Modify: `src/contour/config/Config.hpp` (`ScrollBarConfig::position`, ~line 411, as Task 8.6 left the struct)
- Modify: `src/contour/config/ConfigDocumentation.hpp` (`ScrollbarConfig` ~line 425, `ScrollbarWeb` ~line 2143)
- Test: `src/contour/config/Config_test.cpp` (new case after Task 8.6's; adapt `:1992`)
- Test: `src/contour/session/TerminalSession_test.cpp` (new case appended at the end of the file)

**Interfaces:**
- Consumes: `ScrollBarConfig` as Task 8.6 left it; `contour::config::defaultConfigString()` (`Config.hpp:2404`);
  `TerminalSession::getIsScrollbarRight()` / `getIsScrollbarVisible()` (`TerminalSession.hpp:353-367`);
  `makeSessionWithSurface()` (`TerminalSession_test.cpp:407`); `TestApp` (`src/contour/test/GuiTestFixtures.hpp:221`).
- Produces: `ScrollBarConfig::position` defaults to `ScrollBarPosition::Right`. No signature changes.

- [ ] **Step 1: Write the failing tests**

In `src/contour/config/Config_test.cpp`, insert directly after the closing brace of
`TEST_CASE("Config: scrollbar.marks round-trips through the writer", "[config][scrollbar-marks]")` (Task 8.6):

```cpp
TEST_CASE("Config: the scrollbar is shown on the right by default", "[config][scrollbar-marks]")
{
    // Owner decision (spec §6.1): the scrollbar -- and with it the command marks -- is visible out of the
    // box. `position: hidden` in a profile, or the settings page, turns it off.
    using contour::config::ScrollBarPosition;

    CHECK(contour::config::ScrollBarConfig {}.position == ScrollBarPosition::Right);
    CHECK(contour::config::TerminalProfile {}.scrollbar.value().position == ScrollBarPosition::Right);

    // A profile that never mentions the scrollbar gets it ...
    QTemporaryDir dir;
    auto const config = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
)"sv);
    auto const* profile = config.profile("main");
    REQUIRE(profile != nullptr);
    CHECK(profile->scrollbar.value().position == ScrollBarPosition::Right);

    // ... `contour generate config` writes it out, so a user editing the file sees where it is ...
    auto const defaults = contour::config::Config {};
    auto const generated = YAML::Load(contour::config::defaultConfigString());
    auto const position = generated["profiles"][defaults.defaultProfileName.value()]["scrollbar"]["position"];
    REQUIRE(position.IsScalar());
    // The writer's spelling (std::formatter<ScrollBarPosition>, Config.hpp:2539); the reader ignores case.
    CHECK(position.as<std::string>() == "Right");

    // ... and `hidden` still turns it off.
    auto const hidden = loadFromYaml(dir, R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
        scrollbar:
            position: hidden
)"sv);
    auto const* hiddenProfile = hidden.profile("main");
    REQUIRE(hiddenProfile != nullptr);
    CHECK(hiddenProfile->scrollbar.value().position == ScrollBarPosition::Hidden);
}
```

At the end of `src/contour/session/TerminalSession_test.cpp`, append:

```cpp
TEST_CASE("TerminalSession: the default profile shows the scrollbar on the right, but not over a full-screen program",
          "[contour][session][scrollbar-marks]")
{
    // What SessionChrome.qml binds the bar's edge and visibility to (isScrollbarRight, isScrollbarVisible), for
    // a session on the default profile.
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());

    CHECK(held->getIsScrollbarRight());
    CHECK(held->getIsScrollbarVisible());

    // hide_in_alt_screen keeps its default, so vim or htop keep their last column.
    held->terminal().writeToScreen("\033[?1049h");
    CHECK_FALSE(held->getIsScrollbarVisible());

    held->terminal().writeToScreen("\033[?1049l");
    CHECK(held->getIsScrollbarVisible());
}
```

(`makeSessionWithSurface` attaches a display, without which `TerminalSession::bufferChanged()` does not record the
screen switch, `TerminalSession.cpp:609-615`.)

- [ ] **Step 2: Run them and watch them fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[scrollbar-marks]"`
Expected: FAIL — `Config: the scrollbar is shown on the right by default` at its first `CHECK` (`Hidden == Right`), and
the session case at `CHECK( held->getIsScrollbarRight() )`. (Config tests live in `contour_gui_test`, not
`contour_test`: `Config_test.cpp` is listed at `src/contour/CMakeLists.txt:606`, and `CMakeLists.txt:800-802` says why.)

- [ ] **Step 3: Change the default**

In `src/contour/config/Config.hpp`, `struct ScrollBarConfig` (as Task 8.6 left it). Before:

```cpp
struct ScrollBarConfig
{
    ScrollBarPosition position { ScrollBarPosition::Hidden };
    bool hideScrollbarInAltScreen { true };
```

After:

```cpp
struct ScrollBarConfig
{
    /// Which edge the scrollbar floats against, or Hidden for none (`scrollbar.position`).
    ///
    /// Right by default, so the command marks (@ref marks) are visible out of the box. The bar is an overlay
    /// (SessionChrome.qml): it takes no column from the page, shows only while there is history to scroll
    /// through, and stays off the alternate screen while @ref hideScrollbarInAltScreen is on.
    ScrollBarPosition position { ScrollBarPosition::Right };
    bool hideScrollbarInAltScreen { true };
```

- [ ] **Step 4: Document the new default**

In `src/contour/config/ConfigDocumentation.hpp`, `constexpr StringLiteral ScrollbarConfig` (as Task 8.6 left it).
Before:

```cpp
    "scrollbar:\n"
    "    {comment} scroll bar position: Left, Right, Hidden (ignore-case)\n"
    "    position: {}\n"
```

After:

```cpp
    "scrollbar:\n"
    "    {comment} scroll bar position: Left, Right (the default), Hidden (ignore-case).\n"
    "    {comment} Hidden also hides the marks below.\n"
    "    position: {}\n"
```

In `constexpr StringLiteral ScrollbarWeb`, before:

```cpp
    "      position: Hidden\n"
```

After:

```cpp
    "      position: Right\n"
```

and before:

```cpp
    ":octicons-horizontal-rule-16: ==position==  This option specifies the position of the scrollbar in the "
    "terminal window. It can be set to one of the following values: Left, Right, Hidden. <br/>\n"
```

After:

```cpp
    ":octicons-horizontal-rule-16: ==position==  This option specifies the position of the scrollbar in the "
    "terminal window. It can be set to one of the following values: Left, Right, Hidden. The default is "
    "Right; Hidden turns the scrollbar, and with it the command marks, off. The scrollbar floats over the "
    "edge of the terminal, takes no column from it, and is shown only while there is history to scroll "
    "through. <br/>\n"
```

- [ ] **Step 5: Stop the fallback test pinning the old default**

In `src/contour/config/Config_test.cpp`, `TEST_CASE("Config: invalid enum values fall back and do not abort loading", "[config]")`
(line 1992 before this phase). Before:

```cpp
    // Unknown enum literals leave the compiled-in defaults in place.
    CHECK(profile->scrollbar.value().position == contour::config::ScrollBarPosition::Hidden);
```

After:

```cpp
    // Unknown enum literals leave the compiled-in defaults in place. Compared with the default itself, not a
    // literal: which edge that is, is pinned once, by "the scrollbar is shown on the right by default".
    CHECK(profile->scrollbar.value().position == contour::config::ScrollBarConfig {}.position);
```

- [ ] **Step 6: Re-check for tests that silently assumed no scrollbar**

Run: `grep -rn -i "ScrollBarPosition::Hidden\|position: *hidden\|isScrollbarVisible()" src --include=*_test.cpp --include=*.yml.in`
Expected: only
- `src/contour/config/Config_test.cpp` — the new case's `== ScrollBarPosition::Hidden` after `position: hidden`;
- `src/contour/session/KeyboardRouting_test.cpp:123` and `src/contour/test/QmlComponents_test.cpp:305` — stand-ins
  modelling session state (see the list above), unchanged;
- `src/contour/test/QmlComponents_test.cpp:339,1830` — comments naming `position: Hidden` as one way the bar hides.

Any other hit is a test added since that assumed the old default while testing something else: keep its intent by
setting the scrollbar off explicitly — `p.scrollbar.value().position = contour::config::ScrollBarPosition::Hidden;` in
its `registerProfile` mutator, or `scrollbar: { position: hidden }` in its YAML — and name it in the commit body.

- [ ] **Step 7: Run the affected tests and watch them pass**

Run:
```bash
clang-format -i src/contour/config/Config.hpp src/contour/config/ConfigDocumentation.hpp \
                src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp \
                <every test file Step 6 changed>
cmake --build --preset clangcl-debug --target contour_gui_test contour_test
QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[config]"
QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[scrollbar-marks]"
QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[session]"
QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[qml]"
QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[settings]"
out/build/clangcl-debug/bin/contour_test.exe
```
Expected: zero warnings; every run `All tests passed` — including `the generated default config loads back into the
defaults` (its empty error log proves the reader accepts `position: Right`), the `SessionChrome` geometry cases (their
stand-ins set visibility themselves), and the settings page's `every enum field's value is one of its own options`
(the *Scrollbar position* row now reads `right`). `contour_test` builds the Qt-free libraries against the new default
(`CommandCatalog_test.cpp` builds `TerminalProfile {}`). (Unbuildable here? See the note under **GUI test runs** at the
top.)

- [ ] **Step 8: Commit**

```bash
git add src/contour/config/Config.hpp src/contour/config/ConfigDocumentation.hpp \
        src/contour/config/Config_test.cpp src/contour/session/TerminalSession_test.cpp \
        <every test file Step 6 changed>
git commit -F - <<'EOF'
config: show the scrollbar on the right by default

scrollbar.position now defaults to Right instead of Hidden, so the
scrollbar -- and the command marks on it -- are visible out of the box
(spec 6.1, owner decision). The bar stays an overlay that takes no
column from the page, shows only while there is history, and keeps off
the alternate screen; `position: hidden` turns it off again. The
fallback test compares with the default instead of pinning Hidden.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.7: `TerminalSession` — publish the ticks, coalesced to one rebuild per frame, and jump

**Files:**
- Modify: `src/contour/session/TerminalSession.hpp`, `src/contour/session/TerminalSession.cpp`, `src/contour/session/TerminalSession_test.cpp`

**Interfaces:**
- Consumes: `Terminal::scrollbarMarkInputs()`, `userMarks()`, `revealStableLineAtTop()` (Tasks 8.3-8.5);
  `vtbackend::scrollbarMarks()`, `scrollbarMarkColor()`, `ScrollbarMarkKindList` (Task 8.1); `ScrollBarConfig::marks`
  (Task 8.6); C1 `Terminal::commandBlocks()` / `CommandBlockStore::revision()`; `FoldState::revision()`;
  `Viewport::scrollableLineCount()`; the existing paths this hooks into: `screenUpdated()` (`TerminalSession.cpp:624`),
  `bufferChanged()` (`:609`), `attachDisplay()` (`:423`), `withFolding()` (`:3001`), `updateColorPreference()` (`:791`),
  `emitProfileDerivedPropertiesChanged()` (`:3762`), phase 5's `operator()(actions::ClearToPrompt)` (Task 5.11), and
  the `_searchTallyTimer` / `_searchTallyPostPending` coalescing pattern (`:274`, `TerminalSession.hpp:1125`).
- Produces (QML-facing; new):
  ```cpp
  Q_PROPERTY(QVariantList scrollbarMarks READ scrollbarMarks NOTIFY scrollbarMarksChanged)
  Q_PROPERTY(QVariantList scrollbarMarkColors READ scrollbarMarkColors NOTIFY scrollbarMarksChanged)
  [[nodiscard]] QVariantList scrollbarMarks() const;       // [{ position: 0..1, kind: int, targetLine: qint64 }]
  [[nodiscard]] QVariantList scrollbarMarkColors() const;  // QColor per ScrollbarMarkKind, enumerator order
  Q_INVOKABLE void setScrollbarMarkTrackPixels(int pixels);
  Q_INVOKABLE void scrollToScrollbarMark(qint64 targetLine);
  void refreshScrollbarMarks();                            // GUI thread; the frame timer's slot
  void scrollbarMarksChanged();                            // signal
  ```

Threading: `screenUpdated()` runs on the parser thread, sometimes **with** the terminal lock held
(`TerminalSession.cpp:630-636`), so it may only *request* a refresh: one atomic `test_and_set`, then one post to the
GUI thread that arms a 16 ms single-shot timer. The timer's slot takes the lock, compares a key, and only on a change
copies the marks out (O(records + marks)), buckets them outside the lock (O(marks + pixels)) and emits. So the work
runs at most once per frame, and not at all while nothing it depends on moved.

Colours: each kind's colour is resolved in C++ from the session's live `ColorPalette` (phase 4's `block_status`
slots, Task 8.1's `scrollbarMarkColor`) and published as `scrollbarMarkColors`, indexed by kind — QML does no colour
logic, and a scheme switch or an OSC palette change reaches the ticks through the same key.

- [ ] **Step 1: Write the failing tests**

In `src/contour/session/TerminalSession_test.cpp`, add `#include <vtbackend/shell/ScrollbarMarks.hpp>` after
`#include <vtbackend/core/Hyperlink.hpp>` and `#include <QtTest/QSignalSpy>` after `#include <QtNetwork/QHostInfo>`
unless it is already there (Task 4.13 adds it).
Then insert directly after the closing brace of
`TEST_CASE("TerminalSession: folding actions collapse and expand command output", …)` (~line 1255):

```cpp
// {{{ Scrollbar marks
namespace
{

/// Registers a profile that shows the scrollbar on the right with @p marks as its `scrollbar.marks`.
std::string registerMarkedScrollbarProfile(contour::ContourGuiApp& app,
                                           std::string const& name,
                                           vtbackend::ScrollbarMarkSources marks)
{
    return registerProfile(app, name, [marks](contour::config::TerminalProfile& p) {
        p.scrollbar.value().position = contour::config::ScrollBarPosition::Right;
        p.scrollbar.value().marks = marks;
    });
}

/// Writes one OSC 133-marked command into @p terminal, as a shell with integration installed would.
void writeCommand(vtbackend::Terminal& terminal, std::string_view command, int outputLines, int exitCode)
{
    terminal.writeToScreen(std::format("\033]133;A\033\\$ \033]133;B\033\\{}\r\n\033]133;C\033\\", command));
    for (auto const i: std::views::iota(0, outputLines))
        terminal.writeToScreen(std::format("out {}\r\n", i));
    terminal.writeToScreen(std::format("\033]133;D;{}\033\\", exitCode));
}

/// The value @p key holds in tick @p index of @p session's published marks.
[[nodiscard]] QVariant markField(contour::session::TerminalSession const& session,
                                 qsizetype index,
                                 QString const& key)
{
    return session.scrollbarMarks().at(index).toMap().value(key);
}

} // namespace

TEST_CASE("TerminalSession: scrollbar marks follow the command blocks, and a click jumps to one",
          "[contour][session][scrollbar-marks]")
{
    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();

    writeCommand(terminal, "make", 3, 2);
    writeCommand(terminal, "ls", 3, 0);
    for (auto const i: std::views::iota(0, 60))
        terminal.writeToScreen(std::format("filler {}\r\n", i));

    pane->setScrollbarMarkTrackPixels(200);
    pane->refreshScrollbarMarks();

    REQUIRE(pane->scrollbarMarks().size() == 2);
    CHECK(markField(*pane, 0, QStringLiteral("kind")).toInt()
          == static_cast<int>(vtbackend::ScrollbarMarkKind::Failure));
    CHECK(markField(*pane, 1, QStringLiteral("kind")).toInt()
          == static_cast<int>(vtbackend::ScrollbarMarkKind::Command));
    CHECK(markField(*pane, 0, QStringLiteral("position")).toDouble()
          < markField(*pane, 1, QStringLiteral("position")).toDouble());
    CHECK(markField(*pane, 1, QStringLiteral("position")).toDouble() < 1.0);
    CHECK(pane->scrollbarMarkColors().size() == static_cast<qsizetype>(vtbackend::ScrollbarMarkKindList.size()));

    auto const& grid = terminal.primaryScreen().grid();
    auto const target = markField(*pane, 0, QStringLiteral("targetLine")).toLongLong();

    SECTION("a click puts that block's head on the viewport's top row")
    {
        pane->scrollToScrollbarMark(target);
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == target);
    }

    SECTION("a click on a collapsed block opens it as well")
    {
        REQUIRE(terminal.foldRanges().size() == 2);
        terminal.foldState().collapse(target);
        pane->scrollToScrollbarMark(target);
        CHECK_FALSE(terminal.foldState().isCollapsed(target));
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == target);
    }

    SECTION("a tick published before a reflow is refused rather than misread")
    {
        // README Review Focus #1 at the session's edge: the column change renamed every row, so the id the
        // tick carries would now name some other line.
        terminal.resizeScreen(vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(60) });
        auto const before = terminal.viewport().scrollOffset();
        pane->scrollToScrollbarMark(target);
        CHECK(terminal.viewport().scrollOffset() == before);
    }

    SECTION("the alternate screen shows none, and leaving it brings them back")
    {
        // README Review Focus #2.
        terminal.writeToScreen("\033[?1049h");
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().isEmpty());

        terminal.writeToScreen("\033[?1049l");
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().size() == 2);
    }

    SECTION("a refresh that finds nothing moved announces nothing; a new track height does")
    {
        auto spy = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 0);

        pane->setScrollbarMarkTrackPixels(300);
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 1);
    }

    SECTION("ClearToPrompt drops the ticks of the blocks it removed")
    {
        // Task 5.11: every row above the last prompt goes (make's block), so its tick goes with it.
        (*pane)(contour::actions::ClearToPrompt {});
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().size() <= 1);
    }
}

TEST_CASE("TerminalSession: no shell integration, or scrollbar.marks: [], leaves the scrollbar unmarked",
          "[contour][session][scrollbar-marks]")
{
    TestApp testApp;

    SECTION("plain output carries no marks, and the scrollbar's travel is untouched")
    {
        // README Review Focus #5.
        auto pane = makeSessionWithSurface(
            testApp.app(),
            registerMarkedScrollbarProfile(testApp.app(), "plain", vtbackend::AllScrollbarMarkSources));
        for (auto const i: std::views::iota(0, 60))
            pane->terminal().writeToScreen(std::format("plain {}\r\n", i));
        auto const travel = pane->historyLineCount();

        pane->setScrollbarMarkTrackPixels(200);
        pane->refreshScrollbarMarks();

        CHECK(pane->scrollbarMarks().isEmpty());
        CHECK(travel > 0);
        CHECK(pane->historyLineCount() == travel);
    }

    SECTION("[] hides every tick even with blocks present")
    {
        auto pane = makeSessionWithSurface(
            testApp.app(), registerMarkedScrollbarProfile(testApp.app(), "unmarked", vtbackend::ScrollbarMarkSources {}));
        writeCommand(pane->terminal(), "make", 3, 2);

        pane->setScrollbarMarkTrackPixels(200);
        pane->refreshScrollbarMarks();

        CHECK(pane->scrollbarMarks().isEmpty());
    }
}
// }}}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test`
Expected: FAIL — `error C2039: 'setScrollbarMarkTrackPixels': is not a member of 'contour::session::TerminalSession'`.

- [ ] **Step 3: Declare the surface**

In `src/contour/session/TerminalSession.hpp`:

(a) Includes — add `#include <QtCore/QVariant>` after `#include <QtCore/QTimer>`, and `#include <array>` before `#include <atomic>`.

(b) Directly after `    Q_PROPERTY(QRectF hyperlinkTooltipAnchor READ hyperlinkTooltipAnchor NOTIFY hyperlinkHoverChanged)` (~line 139):

```cpp

    // The scrollbar's tick marks, top first: { position: 0..1 down the track, kind: a ScrollbarMarkKind as
    // an int, targetLine: what scrollToScrollbarMark() takes }. Empty with nothing to mark, on the
    // alternate screen, and while scrollbar.marks is [] or the scrollbar is hidden. The colours are indexed
    // by kind; both lists are rebuilt together, so one signal serves them.
    Q_PROPERTY(QVariantList scrollbarMarks READ scrollbarMarks NOTIFY scrollbarMarksChanged)
    Q_PROPERTY(QVariantList scrollbarMarkColors READ scrollbarMarkColors NOTIFY scrollbarMarksChanged)
```

(c) Directly after `    void onScrollOffsetChanged(vtbackend::ScrollOffset value) override;` (~line 420), before the `// }}}`:

```cpp

    /// The ticks the scrollbar draws. @see the scrollbarMarks Q_PROPERTY for the shape of each entry.
    [[nodiscard]] QVariantList scrollbarMarks() const { return _scrollbarMarks; }

    /// One colour per ScrollbarMarkKind, in enumerator order, resolved from the session's colour scheme.
    [[nodiscard]] QVariantList scrollbarMarkColors() const { return _scrollbarMarkColors; }

    /// Tells the session how tall the scrollbar's track is, in logical pixels.
    ///
    /// A setter, deliberately: the track's height is externally-driven geometry (AGENT.md's documented
    /// exception) -- the QML layout decides it, and every window resize moves it.
    /// @param pixels The track's height; zero or less means there is no track to mark.
    Q_INVOKABLE void setScrollbarMarkTrackPixels(int pixels);

    /// Scrolls so a tick's target line is the viewport's top row, opening a collapsed fold on the way.
    ///
    /// Does nothing when the target no longer names a row -- evicted, or renamed by a reflow since the ticks
    /// were published; the refresh that reflow scheduled replaces the tick anyway.
    /// @param targetLine A tick's targetLine.
    Q_INVOKABLE void scrollToScrollbarMark(qint64 targetLine);

    /// Rebuilds the ticks if anything they depend on moved, and announces them.
    ///
    /// GUI thread only, since it takes the terminal lock. Production reaches it from the frame timer that
    /// requestScrollbarMarksRefresh() arms, so it runs at most once per frame however fast output arrives;
    /// public so a test can run it without an event loop, as refreshSearchStatus() is.
    void refreshScrollbarMarks();
```

(d) In `signals:`, directly after `    void scrollOffsetChanged(int newValue);` (~line 915):

```cpp
    /// The scrollbarMarks and scrollbarMarkColors properties changed.
    void scrollbarMarksChanged();
```

(e) Directly after `    void announceScrollableLineCount(vtbackend::LineCount scrollable);` (~line 995):

```cpp

    /// Arranges for refreshScrollbarMarks() to run on the GUI thread within a frame.
    ///
    /// Callable from either thread, the terminal lock held or not: it neither locks nor computes, only
    /// posts -- and at most one post is in flight, so a burst of output costs one atomic per PTY read.
    void requestScrollbarMarksRefresh();
```

(f) Directly after `    std::atomic<int> _lastHistoryLineCount = 0;` (~line 1191):

```cpp

    /// Everything the scrollbar's ticks are a function of (spec §6.1: the block store, the folds, the
    /// history, the track) plus what selects and colours them. Compared under the terminal lock, so a
    /// refresh that finds none of it moved neither copies the marks out nor wakes QML.
    struct ScrollbarMarksKey
    {
        uint64_t blockRevision = 0;      ///< CommandBlockStore::revision()
        uint64_t foldRevision = 0;       ///< FoldState::revision()
        uint64_t userMarkRevision = 0;   ///< UserMarkIndex::revision()
        uint64_t stableIdGeneration = 0; ///< Grid::stableIdGeneration(): the ids the ticks carry
        int64_t stableFloor = 0;         ///< Grid::stableRangeFloor(): eviction moves every row up
        int64_t visibleRowCount = 0;     ///< The rows the scrollbar spans
        vtbackend::ScreenType screen = vtbackend::ScreenType::Primary;
        int trackPixels = 0;
        vtbackend::ScrollbarMarkSources sources {};
        std::array<vtbackend::RGBColor, vtbackend::ScrollbarMarkKindList.size()> colors {};

        [[nodiscard]] bool operator==(ScrollbarMarksKey const&) const noexcept = default;
    };

    /// What the scrollbarMarks / scrollbarMarkColors properties report. GUI thread only.
    QVariantList _scrollbarMarks;
    QVariantList _scrollbarMarkColors;

    /// The track's height in logical pixels, as the QML last reported it. GUI thread only.
    int _scrollbarMarkTrackPixels = 0;

    /// The key the published ticks were built from; nullopt before the first refresh. GUI thread only.
    std::optional<ScrollbarMarksKey> _scrollbarMarksKey;

    /// Fires refreshScrollbarMarks() a frame after the first request, so a burst of output costs one
    /// rebuild per frame rather than one per PTY read.
    QTimer _scrollbarMarksTimer;

    /// Collapses repeated requests to at most one post in flight, as _searchTallyPostPending does.
    std::atomic_flag _scrollbarMarksPostPending = ATOMIC_FLAG_INIT;
```

- [ ] **Step 4: The helpers and the timer**

In `src/contour/session/TerminalSession.cpp`, insert directly before the `} // namespace` that closes the first anonymous
namespace (after `class ExitWatcherThread { … };`, ~line 223):

```cpp

    /// How often the scrollbar's ticks may be rebuilt: one frame at 60 Hz. They summarise the whole
    /// scrollback, so recomputing them faster than a display can show them buys nothing (spec §6.1).
    constexpr auto ScrollbarMarksFrameInterval = std::chrono::milliseconds { 16 };

    /// @p ticks as QML reads them: one { position, kind, targetLine } map per tick, top first.
    /// @param ticks The bucketed ticks.
    /// @param trackPixels The track they were bucketed for; each position is a fraction of it.
    /// @return The list the scrollbarMarks property reports.
    [[nodiscard]] QVariantList toScrollbarMarkList(std::span<vtbackend::ScrollbarTick const> ticks,
                                                   int trackPixels)
    {
        auto list = QVariantList {};
        list.reserve(static_cast<qsizetype>(ticks.size()));
        for (auto const& tick: ticks)
            list.append(QVariantMap {
                { QStringLiteral("position"),
                  static_cast<double>(tick.pixel) / static_cast<double>(trackPixels) },
                { QStringLiteral("kind"), static_cast<int>(tick.kind) },
                { QStringLiteral("targetLine"), static_cast<qint64>(tick.targetStableId) },
            });
        return list;
    }

    /// Each mark kind's colour under @p palette, indexed by kind.
    [[nodiscard]] std::array<vtbackend::RGBColor, vtbackend::ScrollbarMarkKindList.size()> scrollbarMarkColorsOf(
        vtbackend::ColorPalette const& palette) noexcept
    {
        auto colors = std::array<vtbackend::RGBColor, vtbackend::ScrollbarMarkKindList.size()> {};
        for (auto const kind: vtbackend::ScrollbarMarkKindList)
            colors[static_cast<size_t>(kind)] = vtbackend::scrollbarMarkColor(palette, kind);
        return colors;
    }

    /// @p colors as the QColor list the scrollbarMarkColors property reports.
    [[nodiscard]] QVariantList toScrollbarMarkColorList(std::span<vtbackend::RGBColor const> colors)
    {
        auto list = QVariantList {};
        for (auto const color: colors)
            list.append(QVariant::fromValue(platform::toQColor(color)));
        return list;
    }
```

In the constructor, directly after the `connect(&_searchTallyTimer, …);` statement (~line 280), insert:

```cpp

    // The scrollbar's ticks summarise the whole scrollback, so they are rebuilt at most once per frame
    // however fast output arrives. @see requestScrollbarMarksRefresh.
    _scrollbarMarksTimer.setSingleShot(true);
    _scrollbarMarksTimer.setInterval(ScrollbarMarksFrameInterval);
    connect(&_scrollbarMarksTimer, &QTimer::timeout, this, &TerminalSession::refreshScrollbarMarks);
```

- [ ] **Step 5: Request a refresh wherever an input moves**

All in `src/contour/session/TerminalSession.cpp`:

- `attachDisplay()`: directly after the `announceScrollableLineCount(core::locked(…));` statement (~line 424), insert
  ```cpp

      // And the scrollbar's ticks, for the same reason: a pane that ran commands in the background has none
      // published yet. A post that a detached display dropped may have left the flag set; clear it first.
      _scrollbarMarksPostPending.clear(std::memory_order_release);
      requestScrollbarMarksRefresh();
  ```
- `bufferChanged()`: directly after `    emit isScrollbarVisibleChanged();` (~line 615), insert
  `    requestScrollbarMarksRefresh(); // the alternate screen shows no ticks`.
- `screenUpdated()`: directly after `    announceScrollableLineCount(scrollable);` (~line 691), insert
  ```cpp

      // New output can add blocks, finish one, or grow the history the ticks are spread over. Only a request:
      // this may run with the terminal lock held, and the refresh takes it.
      requestScrollbarMarksRefresh();
  ```
- `updateColorPreference()`: directly after `        _terminal.resetColorPalette(*colorPalette);` (~line 799), insert
  `        requestScrollbarMarksRefresh(); // the ticks are drawn in the scheme's colours`.
- `emitProfileDerivedPropertiesChanged()`: after the `for (auto const notify: Notifiers) (this->*notify)();` loop
  (~line 3785), insert
  ```cpp

      // scrollbar.marks and the scrollbar's position are profile state too, read by the ticks' refresh.
      requestScrollbarMarksRefresh();
  ```
- `withFolding()`: directly after `    announceScrollableLineCount(scrollable);` inside it (~line 3021), insert
  `    requestScrollbarMarksRefresh(); // folding moves every tick below the fold`.
- `operator()(actions::ClearToPrompt)` (Task 5.11): directly after its `announceScrollableLineCount(scrollable);` insert
  `    requestScrollbarMarksRefresh(); // the rows above the prompt are gone, and their ticks with them`.
  (`Terminal::clearToPrompt()` ends in `scrollbackBufferCleared()`, which never raises `screenUpdated()`.)

- [ ] **Step 6: Refresh, track and jump**

Directly after the closing brace of `void TerminalSession::announceScrollableLineCount(…)` (~line 2245), insert:

```cpp

void TerminalSession::setScrollbarMarkTrackPixels(int pixels)
{
    auto const track = pixels < 0 ? 0 : pixels;
    if (track == _scrollbarMarkTrackPixels)
        return;

    _scrollbarMarkTrackPixels = track;
    requestScrollbarMarksRefresh();
}

void TerminalSession::requestScrollbarMarksRefresh()
{
    if (!_display || _scrollbarMarksPostPending.test_and_set(std::memory_order_acq_rel))
        return;

    // The timer belongs to the GUI thread, so it is started there. A request while it is already running
    // rides on that one: it fires within the frame either way.
    _display->post([this]() {
        if (!_scrollbarMarksTimer.isActive())
            _scrollbarMarksTimer.start();
    });
}

void TerminalSession::refreshScrollbarMarks()
{
    // Cleared before anything is read, so output arriving during this refresh schedules the next one.
    _scrollbarMarksPostPending.clear(std::memory_order_release);

    // The ticks belong to the scrollbar: nothing to mark where it is configured away. Read here, on the GUI
    // thread, which is the only one that swaps the profile.
    auto const& scrollbar = _profile.scrollbar.value();
    auto const sources = scrollbar.position == config::ScrollBarPosition::Hidden ? vtbackend::ScrollbarMarkSources {}
                                                                                 : scrollbar.marks;

    auto inputs = core::locked(_terminal, [&]() -> std::optional<std::vector<vtbackend::ScrollbarMarkInput>> {
        auto const& grid = _terminal.primaryScreen().grid();
        auto const key = ScrollbarMarksKey {
            .blockRevision = _terminal.commandBlocks().revision(),
            .foldRevision = _terminal.foldState().revision(),
            .userMarkRevision = _terminal.userMarks().revision(),
            .stableIdGeneration = grid.stableIdGeneration(),
            .stableFloor = grid.stableRangeFloor(),
            .visibleRowCount = unbox<int64_t>(_terminal.viewport().scrollableLineCount())
                               + unbox<int64_t>(_terminal.pageSize().lines),
            .screen = _terminal.isPrimaryScreen() ? vtbackend::ScreenType::Primary
                                                  : vtbackend::ScreenType::Alternate,
            .trackPixels = _scrollbarMarkTrackPixels,
            .sources = sources,
            .colors = scrollbarMarkColorsOf(_terminal.colorPalette()),
        };
        if (_scrollbarMarksKey == key)
            return std::nullopt;
        _scrollbarMarksKey = key;

        // Nothing would survive the bucketing, so do not copy the marks out only to drop them.
        if (key.sources.none() || key.trackPixels <= 0)
            return std::vector<vtbackend::ScrollbarMarkInput> {};
        return _terminal.scrollbarMarkInputs();
    });
    if (!inputs)
        return;

    // Outside the lock: bucketing reads nothing the parser thread writes, and the signal below wakes QML
    // bindings that reach back into this session.
    auto const& key = *_scrollbarMarksKey;
    auto const ticks = vtbackend::scrollbarMarks(*inputs, key.visibleRowCount, key.trackPixels, key.sources);
    _scrollbarMarks = toScrollbarMarkList(ticks, key.trackPixels);
    _scrollbarMarkColors = toScrollbarMarkColorList(key.colors);
    emit scrollbarMarksChanged();
}
```

Directly after the closing brace of `bool TerminalSession::operator()(actions::ToggleLastFold)` (~line 3062) — below
`withFolding()`'s definition, which a call must follow — insert:

```cpp

void TerminalSession::scrollToScrollbarMark(qint64 targetLine)
{
    // Only within the stable-id generation the ticks were published in: a reflow since then renamed every
    // row, and the refresh it scheduled replaces this tick anyway.
    if (!_scrollbarMarksKey)
        return;
    auto const generation = _scrollbarMarksKey->stableIdGeneration;

    // Through withFolding() for its lock and for republishing the scrollable count, which opening a
    // collapsed block changes -- and ungated, because a jump can only ever REVEAL output.
    withFolding(
        [&](vtbackend::Terminal& terminal) {
            return terminal.revealStableLineAtTop(static_cast<int64_t>(targetLine), generation)
                   == vtbackend::RevealOutcome::Revealed;
        },
        FoldingGate::Always);
}
```

- [ ] **Step 7: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[session]"`
Expected: zero warnings; `All tests passed` — the two new cases and every existing session case (the folding and
attach paths gained a request).

- [ ] **Step 8: Hand-audit for clang-tidy (it cannot lint `src/contour/**` on Windows)**

Check the added lines for: `misc-use-internal-linkage` (the three helpers and the constant sit in the anonymous
namespace), `misc-const-correctness` (every local that is not mutated is `const`; `list`, `colors`, `inputs` are
mutated), `bugprone-implicit-widening-of-multiplication-result` (no multiplication added), and
`readability-identifier-naming` (`ScrollbarMarksFrameInterval` is a constexpr variable: CamelCase; members `_camelBack`).

- [ ] **Step 9: Format and commit**

Run: `clang-format -i src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp src/contour/session/TerminalSession_test.cpp`

```bash
git add src/contour/session/TerminalSession.hpp src/contour/session/TerminalSession.cpp \
        src/contour/session/TerminalSession_test.cpp
git commit -F - <<'EOF'
contour: publish scrollbar marks from the session

TerminalSession exposes the ticks (position, kind, target line) and one
colour per kind to QML, rebuilt by a 16 ms single-shot timer that screen
updates, folding, ClearToPrompt, screen switches, palette and profile
changes arm -- at most once per frame, and only when the store, fold or
user-mark revision, the grid's identity or extent, the track, the sources
or the colours moved.
A click scrolls the tick's line to the top, opening its fold, and is
refused once a reflow has renamed the rows.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.8: QML — draw the ticks over the scrollbar

**Files:**
- Modify: `src/contour/qml/SessionChrome.qml` (properties ~line 43, `ScrollBar` ~lines 85-160, functions ~lines 249-266), `src/contour/test/QmlComponents_test.cpp` (`MockSession` ~lines 266-390, new case after ~line 1843)

**Interfaces:**
- Consumes: `TerminalSession.scrollbarMarks`, `.scrollbarMarkColors`, `.setScrollbarMarkTrackPixels(int)`,
  `.scrollToScrollbarMark(qint64)` (Task 8.7); the stock `ScrollBar`'s `availableHeight`, `availableWidth`,
  `topPadding`, `leftPadding`, `visualPosition`, `visualSize`.
- Produces: object names `scrollbarMarkLayer` (one `Item`) and `scrollbarMark` (one `Rectangle` per tick) for tests.

Layout decisions:
- The tick layer is a **child of the `ScrollBar`**, so it is hidden exactly when the bar is: `position: Hidden`,
  `hide_in_alt_screen` on the alternate screen, or no scrollback (`vbar.visible`, `SessionChrome.qml:104`). On the
  alternate screen the session publishes no ticks anyway (Task 8.7), so `hide_in_alt_screen: false` shows none either.
- It spans the track, `x/y = leftPadding/topPadding`, `width/height = availableWidth/availableHeight` — the span the
  handle travels — and a tick's `y` is `round(position * height)`, the same fraction the handle's top reaches once
  that line is the top row. `z: -0.5` stacks it above the track (`background`, which Controls puts at `z -1`) and
  below the handle (`contentItem`, `z 0`).
- A press on a tick that lies under the handle is not accepted, so it falls through to the bar and starts a drag:
  grabbing the thumb must never turn into a jump.
- `onScrollBarPositionChanged` now rounds the offset it writes back: after a tick jump the bar is still hovered
  (`active`), and `historyLineCount - position * total` can land a hair under the integer, which the `int` property
  would truncate to the row above.

- [ ] **Step 1: Teach `MockSession` the marks**

In `src/contour/test/QmlComponents_test.cpp`, add `#include <vtbackend/shell/ScrollbarMarks.hpp>` after
`#include <contour/window/TabColorScheme.hpp>`, and `#include <vector>` to the standard-library block.

In `class MockSession`, directly after
`    Q_PROPERTY(QRectF hyperlinkTooltipAnchor READ hyperlinkTooltipAnchor NOTIFY hyperlinkHoverChanged)` (~line 288):

```cpp
    // Mirrors TerminalSession's scrollbar marks: ticks as { position, kind, targetLine } and one colour per
    // kind. Both change together, so one signal serves them.
    Q_PROPERTY(QVariantList scrollbarMarks READ scrollbarMarks NOTIFY scrollbarMarksChanged)
    Q_PROPERTY(QVariantList scrollbarMarkColors READ scrollbarMarkColors NOTIFY scrollbarMarksChanged)
```

directly after the `setHyperlinkHover(…)` member function (~line 315):

```cpp

    [[nodiscard]] QVariantList scrollbarMarks() const { return _scrollbarMarks; }
    [[nodiscard]] QVariantList scrollbarMarkColors() const { return _scrollbarMarkColors; }

    /// Publishes ticks, as TerminalSession::refreshScrollbarMarks() does.
    void setScrollbarMarks(QVariantList marks, QVariantList colors)
    {
        _scrollbarMarks = std::move(marks);
        _scrollbarMarkColors = std::move(colors);
        emit scrollbarMarksChanged();
    }

    /// Records the track height the chrome reports, so a test can assert the session was told.
    Q_INVOKABLE void setScrollbarMarkTrackPixels(int pixels) { trackPixels = pixels; }

    /// Records each jump the chrome asks for, so a click on a tick is observable.
    Q_INVOKABLE void scrollToScrollbarMark(qint64 targetLine) { jumpedTo.push_back(targetLine); }

    int trackPixels = 0;
    std::vector<qint64> jumpedTo;
```

in its `signals:` block add `    void scrollbarMarksChanged();`, and in its `private:` block add

```cpp
    QVariantList _scrollbarMarks;
    QVariantList _scrollbarMarkColors;
```

- [ ] **Step 2: Write the failing GUI test**

Insert directly after the closing brace of
`TEST_CASE("SessionChrome scrollbar stays a thin edge overlay, whatever the session does (offscreen)", …)` (~line 1843):

```cpp
namespace
{
/// The tick items a SessionChrome's mark layer currently shows, top first.
[[nodiscard]] std::vector<QQuickItem*> markTicksOf(QQuickItem const& layer)
{
    auto ticks = std::vector<QQuickItem*> {};
    for (auto* item: layer.childItems())
        if (item->objectName() == QStringLiteral("scrollbarMark"))
            ticks.push_back(item);
    std::ranges::sort(ticks, std::ranges::less {}, &QQuickItem::y);
    return ticks;
}

/// A tick as TerminalSession publishes it.
[[nodiscard]] QVariantMap markEntry(double position, vtbackend::ScrollbarMarkKind kind, qint64 targetLine)
{
    return QVariantMap { { QStringLiteral("position"), position },
                         { QStringLiteral("kind"), static_cast<int>(kind) },
                         { QStringLiteral("targetLine"), targetLine } };
}

/// One colour per ScrollbarMarkKind, in enumerator order: Command, UserMark, Running, Failure.
[[nodiscard]] QVariantList markColors()
{
    return QVariantList { QVariant::fromValue(QColor(Qt::gray)),
                          QVariant::fromValue(QColor(Qt::cyan)),
                          QVariant::fromValue(QColor(Qt::yellow)),
                          QVariant::fromValue(QColor(Qt::red)) };
}

/// The centre of @p item, in its window's coordinates.
[[nodiscard]] QPoint centreOf(QQuickItem const& item)
{
    return item.mapToScene(QPointF(item.width() / 2, item.height() / 2)).toPoint();
}
} // namespace

TEST_CASE("SessionChrome draws the scrollbar's marks, and a click on one jumps there (offscreen)",
          "[contour][gui][qml][scrollbar-marks]")
{
    // The tick layer over the scrollbar (spec §6.1): one thin line per published mark, at its fraction of the
    // track, in its kind's colour -- and a click on one asks the session to bring that line to the top.
    QQmlEngine engine;
    MockTabController controller;
    engine.rootContext()->setContextProperty("terminalSessions", &controller);
    contour::test::installChromeStyle(engine);
    contour::test::QmlMessageCapture const warnings;

    auto host = createChromeInWindow(engine);
    auto* layer = host.bar->findChild<QQuickItem*>(QStringLiteral("scrollbarMarkLayer"));
    REQUIRE(layer != nullptr);
    auto* window = qobject_cast<QQuickWindow*>(host.window.get());
    REQUIRE(window != nullptr);

    auto session = createScrollableSession();
    session->setScrollbarMarks(QVariantList { markEntry(0.1, vtbackend::ScrollbarMarkKind::Failure, 42),
                                              markEntry(0.5, vtbackend::ScrollbarMarkKind::Command, 4242) },
                               markColors());
    auto const bind = [&](MockSession& bound) {
        host.chrome->setProperty("session", QVariant::fromValue(static_cast<QObject*>(&bound)));
        QCoreApplication::processEvents();
    };

    SECTION("one tick per mark, at its fraction of the track, in its kind's colour")
    {
        bind(*session);
        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        CHECK(ticks[0]->y() == Catch::Approx(std::round(0.1 * layer->height())));
        CHECK(ticks[1]->y() == Catch::Approx(std::round(0.5 * layer->height())));
        CHECK(ticks[0]->width() == Catch::Approx(layer->width()));
        CHECK(ticks[0]->property("color").value<QColor>() == QColor(Qt::red));  // Failure
        CHECK(ticks[1]->property("color").value<QColor>() == QColor(Qt::gray)); // Command

        // The session was told the track's height, so its pixel buckets are the pixels drawn here.
        CHECK(session->trackPixels == qRound(layer->height()));
    }

    SECTION("a click on a tick asks the session to bring its line to the top")
    {
        bind(*session);
        host.bar->setProperty("position", 0.9); // the handle well clear of the tick at 0.1
        QCoreApplication::processEvents();

        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(*ticks[0]));
        QCoreApplication::processEvents();

        CHECK(session->jumpedTo == std::vector<qint64> { 42 });
    }

    SECTION("a tick under the handle leaves the press to the handle")
    {
        bind(*session);
        host.bar->setProperty("position", 0.1); // the handle now covers the tick at 0.1
        QCoreApplication::processEvents();

        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(*ticks[0]));
        QCoreApplication::processEvents();

        CHECK(session->jumpedTo.empty());
    }

    SECTION("no marks: no ticks, and the bar is exactly the bar it was")
    {
        // README Review Focus #5: without shell integration the scrollbar is unchanged.
        auto plain = createScrollableSession();
        bind(*plain);
        CHECK(markTicksOf(*layer).empty());
        CHECK(host.bar->isVisible());
        CHECK(host.bar->width() == Catch::Approx(host.thin));
        CHECK(host.bar->x() == Catch::Approx(host.paneWidth - host.thin));
    }

    SECTION("hiding the bar -- position: Hidden, or hide_in_alt_screen -- hides its ticks")
    {
        bind(*session);
        auto const ticks = markTicksOf(*layer);
        REQUIRE(ticks.size() == 2);
        REQUIRE(ticks[0]->isVisible());

        session->setScrollbarVisible(false);
        QCoreApplication::processEvents();
        CHECK_FALSE(ticks[0]->isVisible());
    }

    INFO("QML messages:\n" << warnings.messages().join(QStringLiteral("\n")).toStdString());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
```

- [ ] **Step 3: Run it and watch it fail**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[scrollbar-marks]"`
Expected: FAIL — `REQUIRE( layer != nullptr )` in `SessionChrome draws the scrollbar's marks…`.

- [ ] **Step 4: The chrome-level properties**

In `src/contour/qml/SessionChrome.qml`, directly after `    property real _pendingBellVolume: -1`, insert:

```qml

    // The scrollbar's tick marks (`scrollbar.marks`): one { position, kind, targetLine } per tick, top
    // first, and one colour per kind. Guarded against `undefined`, not merely against a null session, for
    // the reason hyperlinkTip gives: other tests bind a mock session that carries no marks at all.
    readonly property var scrollbarMarks: (chrome.session && chrome.session.scrollbarMarks !== undefined)
                                          ? chrome.session.scrollbarMarks
                                          : []
    readonly property var scrollbarMarkColors: (chrome.session && chrome.session.scrollbarMarkColors !== undefined)
                                               ? chrome.session.scrollbarMarkColors
                                               : []
```

- [ ] **Step 5: The tick layer inside the bar**

In the `ScrollBar { id: vbar … }` block, directly after `        onSizeChanged: chrome.updateScrollBarPosition()`, insert:

```qml
        onAvailableHeightChanged: chrome.publishMarkTrack()
```

and directly after the `background: Rectangle { … }` element (after its closing `}`), before the bar's own
closing `}`, insert:

```qml

        // Tick marks over the track (`scrollbar.marks`): where commands ran, failed or are still running, and
        // the lines a vi `mm` marked. Each sits at its mark's fraction of the track -- the fraction the
        // handle's top reaches once that line is the viewport's top row -- so a click on a tick and a drag of
        // the handle to it land on the same place.
        //
        // Stacked between the track (the background, z -1) and the handle (the contentItem, z 0), and a child
        // of the bar, so it is hidden exactly when the bar is: position Hidden, hide_in_alt_screen, or no
        // scrollback at all.
        Item {
            id: markLayer
            objectName: "scrollbarMarkLayer"
            z: -0.5
            x: vbar.leftPadding
            y: vbar.topPadding
            width: vbar.availableWidth
            height: vbar.availableHeight

            Repeater {
                model: chrome.scrollbarMarks

                delegate: Rectangle {
                    id: tick
                    objectName: "scrollbarMark"

                    required property var modelData

                    x: 0
                    y: Math.min(markLayer.height - tick.height,
                                Math.round(tick.modelData.position * markLayer.height))
                    width: markLayer.width
                    height: 2
                    radius: 1
                    color: chrome.markColor(tick.modelData.kind)

                    MouseArea {
                        // A two-pixel line is a hard target, so the hit area reaches a pixel past it on either
                        // side without moving what is drawn.
                        anchors.fill: parent
                        anchors.topMargin: -1
                        anchors.bottomMargin: -1
                        cursorShape: Qt.PointingHandCursor

                        // Under the handle the press belongs to the handle: grabbing the thumb must never turn
                        // into a jump just because a tick happens to lie beneath it.
                        onPressed: (mouse) => { mouse.accepted = !chrome.isUnderHandle(tick.y + tick.height / 2); }
                        onClicked: chrome.jumpToMark(tick.modelData.targetLine)
                    }
                }
            }
        }
```

- [ ] **Step 6: The helpers, the track report, and the rounding fix**

Replace `function onScrollBarPositionChanged()`. Before:

```qml
    // Update the VT's viewport whenever the scrollbar's position changes.
    function onScrollBarPositionChanged() {
        let vt = chrome.session;
        if (vt === null)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        if (vbar.active)
            vt.scrollOffset = vt.historyLineCount - vbar.position * totalLineCount;
    }
```

After:

```qml
    // Update the VT's viewport whenever the scrollbar's position changes.
    //
    // Rounded rather than truncated by the int property: the position is the session's own offset divided
    // by the total, and multiplying back can land a hair under the integer -- the row above. A tick jump
    // leaves the bar hovered (active) while the offset it set echoes back through here, which is where
    // that showed.
    function onScrollBarPositionChanged() {
        let vt = chrome.session;
        if (vt === null)
            return;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);
        if (vbar.active)
            vt.scrollOffset = Math.round(vt.historyLineCount - vbar.position * totalLineCount);
    }

    // The colour of a tick of @p kind, from the session's colour scheme; the handle's own colour while the
    // session has published none (a mock, or the instant before its first refresh).
    function markColor(kind) {
        return kind >= 0 && kind < chrome.scrollbarMarkColors.length ? chrome.scrollbarMarkColors[kind]
                                                                       : vbar.handleColor;
    }

    // Whether @p y, in the mark layer's coordinates, lies under the scrollbar's handle.
    function isUnderHandle(y) {
        let top = vbar.visualPosition * markLayer.height;
        return y >= top && y <= top + vbar.visualSize * markLayer.height;
    }

    // Asks the session to put a tick's line at the top of the viewport.
    function jumpToMark(targetLine) {
        if (chrome.session && chrome.session.scrollToScrollbarMark !== undefined)
            chrome.session.scrollToScrollbarMark(targetLine);
    }

    // Tells the session how tall the track is, so its pixel buckets are the pixels drawn here.
    function publishMarkTrack() {
        if (chrome.session && chrome.session.setScrollbarMarkTrackPixels !== undefined)
            chrome.session.setScrollbarMarkTrackPixels(Math.round(vbar.availableHeight));
    }

    // A rebound session has never been told this pane's track height.
    onSessionChanged: chrome.publishMarkTrack()
    Component.onCompleted: chrome.publishMarkTrack()
```

- [ ] **Step 7: Run the tests and watch them pass**

Run: `cmake --build --preset clangcl-debug --target contour_gui_test && QT_QPA_PLATFORM=offscreen out/build/clangcl-debug/bin/contour_gui_test.exe "[qml]"`
Expected: `All tests passed` — the new case, and every existing `SessionChrome` / split / pane case, whose QML
diagnostic gates (`warnings.count(isQmlDiagnostic) == 0`) prove the new bindings stay silent against sessions
that carry no marks (null, and the `MockPaneProxy`-hosted ones).

- [ ] **Step 8: Hand-audit for clang-tidy**

In the test additions: `markTicksOf`, `markEntry`, `markColors`, `centreOf` are in an anonymous namespace
(`misc-use-internal-linkage`); every non-mutated local is `const` (`misc-const-correctness`); no
multiplication result is widened.

- [ ] **Step 9: Commit**

Run: `clang-format -i src/contour/test/QmlComponents_test.cpp`

```bash
git add src/contour/qml/SessionChrome.qml src/contour/test/QmlComponents_test.cpp
git commit -F - <<'EOF'
contour: draw scrollbar marks over the scrollbar

A Repeater of two-pixel ticks inside the stock ScrollBar, placed at each
mark's fraction of the track and coloured by kind from the session's
scheme, hidden whenever the bar is. A click asks the session to bring the
line to the top; a press under the handle stays a drag. The track height
is reported to the session so its buckets are the pixels drawn, and the
offset written back from the bar is rounded rather than truncated.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 8.9: Phase gate

**Files:** whatever the review steps change; nothing new is planned.

**Interfaces:**
- Consumes: `PHASE8_START` (Task 8.1 Step 1); the README's [Phase gate](README.md#phase-gate-end-of-every-phase).
- Produces: a clean, reviewed phase 8 on `feature/1010-worktree-semantic-blocks`.

- [ ] **Step 1: Build every touched target with zero warnings**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test contour_test contour_gui_test contour`
Expected: success, no warning lines. (`contour_test` covers the Qt-free `contour_config` library that gained
`scrollbar.marks`.) If `contour` / `contour_gui_test` hit the pre-existing `yaml-cpp/emitter.h` break recorded in
phase 0, build the other two and compile the edited GUI TUs one by one as described at the top of this file.

- [ ] **Step 2: Run the whole suite and the spelling check**

Run: `ctest --test-dir out/build/clangcl-debug --output-on-failure`
Expected: the phase-0 baseline's failures and no others. Note the `N tests passed, M failed` line.

Run: `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: PASS (or SKIP when `typos` is not installed — note which).

- [ ] **Step 3: Formatting is clean across the phase**

Run: `git diff --name-only "$PHASE8_START"..HEAD -- '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror`
Expected: no output.

- [ ] **Step 4: Hand-audit the `src/contour/**` lines clang-tidy cannot lint here**

Run: `git diff "$PHASE8_START"..HEAD -- src/contour`
Check every added C++ line for `misc-use-internal-linkage`, `misc-const-correctness`,
`bugprone-implicit-widening-of-multiplication-result`, `readability-identifier-naming`, and that no `bool` entered
an API (README global constraints). The CI clang-tidy job is the oracle; fix anything found now rather than there.

- [ ] **Step 5: `/simplify` over the phase**

Run `/simplify` on `git diff "$PHASE8_START"..HEAD`. Apply its fixes, re-run Steps 1-2, then:

```bash
git add -u src/
git commit -F - <<'EOF'
contour: simplify scrollbar marks

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(Skip the commit when `/simplify` changed nothing — no empty commits.)

- [ ] **Step 6: xhigh code review**

Run `/code-review xhigh` on the phase's commits (`$PHASE8_START..HEAD`), or dispatch a review subagent with
`effort: "xhigh"`. Ask it to look hardest at: the key in `refreshScrollbarMarks()` missing an input (a tick that does
not move when it should), `scrollbarMarkInputs()` disagreeing with `Viewport::scrollableLineCount()` about row 0
under folds and at history capacity, the threading of `requestScrollbarMarksRefresh()` (parser thread, lock held),
and README Review Focus #1, #2, #4, #5. Fix every confirmed finding, re-run Steps 1-3, then commit with the ctest
summary from Step 2 in the body:

```bash
git add -u src/
git commit -F - <<'EOF'
contour: address review of scrollbar marks

ctest: <N> tests passed, <M> failed (baseline failures only: <names>)

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

(When the review confirms nothing, make no commit and hand the ctest line to the coordinator instead.)

- [ ] **Step 7: Report**

Report to the coordinating session: the ctest summary, the spelling result, any target that could not be built
locally, and the commits from `$PHASE8_START..HEAD` (`git log --oneline "$PHASE8_START"..HEAD`). The coordinator records
progress — this phase does not.

## Notes and risks

- **The scrollbar is visible by default — decided.** With `scrollbar.position` defaulting to `Hidden`, every tick
  this phase draws would have been invisible to anyone who never changed the setting. The owner decided the default is
  `Right` (spec §6.1 as amended; Task 8.6a), so the marks show out of the box. The bar is an overlay: no page column,
  window size or gutter changes, it shows only while there is history, and `hide_in_alt_screen` (default on) keeps it
  off full-screen programs. What it does cover — the right edge of the last column, its glyphs under the resting
  handle and its presses and hover, including the sticky header chip's last cell — is accepted with the decision.
  Every existing user's window gains a scrollbar on upgrade, so phase 10's release notes (Task 10.11) say so and how
  to turn it off (`scrollbar: { position: hidden }` in the profile, or the settings page).
- **Tests that silently assumed no scrollbar.** Only `Config_test.cpp:1992` pinned the old default in the tree this
  plan was written against (adapted in Task 8.6a Step 5). Task 8.6a Step 6 re-runs the audit against whatever phases
  1–8 added before the default flips.
