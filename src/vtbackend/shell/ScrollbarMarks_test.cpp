// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
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
    return std::ranges::to<std::vector>(ticks | std::views::transform(&ScrollbarTick::kind));
}

} // namespace

TEST_CASE("ScrollbarMarks.bucketing.nothingToDraw", "[scrollbar-marks]")
{
    auto const one = std::vector { markAt(5, Kind::Failure, 5) };
    CHECK(scrollbarMarks({}, 100, 50, AllScrollbarMarkSources).empty());
    CHECK(scrollbarMarks(one, 100, 50, ScrollbarMarkSources {}).empty()); // `marks: []`
}

TEST_CASE("ScrollbarMarks.bucketing.noTrackOrNoRowsDrawsNothing", "[scrollbar-marks]")
{
    // A hidden or minimized scrollbar has a track of no pixels, and a range of no rows has no fraction to
    // place a mark at. Neither may index past the track or divide by zero. The mark is on row 0, the one row
    // any range admits first.
    struct Case
    {
        int64_t rows {};
        int track {};
    };
    constexpr auto Cases = std::array {
        Case { .rows = 100, .track = 0 },
        Case { .rows = 100, .track = -1 },
        Case { .rows = 0, .track = 50 },
        Case { .rows = -1, .track = 50 },
    };
    auto const marks = std::array { markAt(0, Kind::Failure, 1) };
    for (auto const& c: Cases)
    {
        INFO(std::format("{} rows over {} pixels", c.rows, c.track));
        CHECK(scrollbarMarks(marks, c.rows, c.track, AllScrollbarMarkSources).empty());
    }
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
    auto const marks = std::vector { markAt(-1, Kind::Failure, 1),
                                     markAt(100, Kind::Failure, 2),
                                     markAt(50, Kind::Command, 3) };
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

    SECTION("on one row, the line further up")
    {
        // Two vi marks inside one collapsed fold both stand on its head's row: the click goes to the upper
        // of the two marked lines.
        for (auto const& marks:
             { std::vector { markAt(5, Kind::UserMark, 50), markAt(5, Kind::UserMark, 52) },
               std::vector { markAt(5, Kind::UserMark, 52), markAt(5, Kind::UserMark, 50) } })
        {
            auto const ticks = scrollbarMarks(marks, 100, 10, AllScrollbarMarkSources);
            REQUIRE(ticks.size() == 1);
            CHECK(ticks[0].targetStableId == 50);
        }
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
    // 100k marks on a 600-pixel track come back as at most 600 ticks, one per pixel, ascending, each the
    // highest kind that landed there -- checked against a brute-force reference.
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

TEST_CASE("ScrollbarMarks.kinds.kindsMapToTheirOutcomesAndSources", "[scrollbar-marks]")
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
    palette.blockStatus[static_cast<size_t>(CommandBlockOutcome::Success)] = RGBColor { 0x10, 0x20, 0x30 };
    palette.blockStatus[static_cast<size_t>(CommandBlockOutcome::Failure)] = RGBColor { 0xF0, 0x00, 0x00 };
    palette.blockStatus[static_cast<size_t>(CommandBlockOutcome::Running)] = RGBColor { 0xF0, 0xF0, 0x00 };

    CHECK(scrollbarMarkColor(palette, Kind::Command) == RGBColor { 0x10, 0x20, 0x30 });
    CHECK(scrollbarMarkColor(palette, Kind::Failure) == RGBColor { 0xF0, 0x00, 0x00 });
    CHECK(scrollbarMarkColor(palette, Kind::Running) == RGBColor { 0xF0, 0xF0, 0x00 });

    // A mark the user placed is drawn apart from every outcome, in the fold column's hovered text colour:
    // the scheme's own where it sets one, and its full-strength text colour where it does not.
    SECTION("a scheme that sets the hovered fold marker")
    {
        palette.foldMarkerHover = RGBColorPair { .foreground = RGBColor { 0x01, 0x02, 0x03 },
                                                 .background = RGBColor { 0x04, 0x05, 0x06 } };
        CHECK(scrollbarMarkColor(palette, Kind::UserMark) == RGBColor { 0x01, 0x02, 0x03 });
    }

    SECTION("a scheme that does not")
    {
        palette.defaultForeground = RGBColor { 0x0A, 0x0B, 0x0C };
        CHECK(scrollbarMarkColor(palette, Kind::UserMark) == RGBColor { 0x0A, 0x0B, 0x0C });
    }
}

TEST_CASE("ScrollbarMarks.overlayHold.column", "[scrollbar-marks]")
{
    // A finished command leaves a mark after every prompt, so only the marks worth a look hold an overlay bar
    // at the bottom of the scrollback.
    auto const expected = std::map<Kind, OverlayHold> {
        { Kind::Command, OverlayHold::Quiet },
        { Kind::UserMark, OverlayHold::KeepsShown },
        { Kind::Running, OverlayHold::KeepsShown },
        { Kind::Failure, OverlayHold::KeepsShown },
    };
    REQUIRE(expected.size() == ScrollbarMarkKindTable.size());
    for (auto const& row: ScrollbarMarkKindTable)
    {
        INFO("kind " << std::format("{}", row.kind));
        CHECK(row.overlay == expected.at(row.kind));
    }
}

TEST_CASE("ScrollbarMarks.marksAbove.onlyHeldDrawnKindsAboveTheRowCount", "[scrollbar-marks]")
{
    auto const marks = std::array {
        markAt(5, Kind::Command, 1),
        markAt(7, Kind::Failure, 2),
        markAt(40, Kind::Failure, 3),
    };
    auto const commandsAndUserMarks =
        ScrollbarMarkSources { ScrollbarMarkSource::Commands, ScrollbarMarkSource::UserMarks };

    // A failure at row 7 is above a viewport whose first row is 30.
    CHECK(marksAbove(marks, 30, AllScrollbarMarkSources) == MarksAbove::Some);
    // Only a command is above row 6: commands do not hold the bar. The failure on row 7 is in view.
    CHECK(marksAbove(marks, 6, AllScrollbarMarkSources) == MarksAbove::None);
    // A failure the bar does not draw holds nothing.
    CHECK(marksAbove(marks, 30, commandsAndUserMarks) == MarksAbove::None);
    // The row itself is not above.
    CHECK(marksAbove(marks, 7, AllScrollbarMarkSources) == MarksAbove::None);
    CHECK(marksAbove({}, 30, AllScrollbarMarkSources) == MarksAbove::None);

    // A user's own mark holds the bar too, unless the bar does not draw user marks.
    auto const userMarked = std::array { markAt(5, Kind::Command, 1), markAt(9, Kind::UserMark, 4) };
    CHECK(marksAbove(userMarked, 30, AllScrollbarMarkSources) == MarksAbove::Some);
    CHECK(marksAbove(userMarked, 30, ScrollbarMarkSources { ScrollbarMarkSource::UserMarks })
          == MarksAbove::Some);
    CHECK(marksAbove(userMarked,
                     30,
                     ScrollbarMarkSources { ScrollbarMarkSource::Commands, ScrollbarMarkSource::Failures })
          == MarksAbove::None);
}
