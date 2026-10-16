// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Gutter.hpp>
#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vtbackend;
using namespace std::chrono_literals;

namespace chrono = std::chrono;

namespace
{
/// 14:03:07 local civil time on an ordinary day -- a local_seconds, so no time zone is consulted.
constexpr auto Afternoon =
    chrono::local_days { chrono::year { 2026 } / chrono::October / 9 } + 14h + 3min + 7s;

/// The same instant as a wall-clock time, which a run reads in UTC (fixedOffsetLocalTime<0>).
constexpr auto AfternoonUtc =
    chrono::system_clock::time_point { chrono::sys_days { chrono::year { 2026 } / chrono::October / 9 } + 14h
                                       + 3min + 7s };

/// The number of times countingLocalTime() converted an instant.
int conversions = 0;

/// fixedOffsetLocalTime<0>, counting its calls in `conversions`.
[[nodiscard]] chrono::local_seconds countingLocalTime(chrono::system_clock::time_point when) noexcept
{
    ++conversions;
    return fixedOffsetLocalTime<0>(when);
}
} // namespace

// {{{ timestamps
TEST_CASE("Gutter.timestamps.formatLocalTimeWithTheConfiguredSpec", "[gutter]")
{
    CHECK(formatGutterTimestamp("%H:%M:%S", Afternoon) == "14:03:07");
    CHECK(formatGutterTimestamp("%H:%M", Afternoon) == "14:03");
    // A specification the formatter rejects spells nothing rather than throwing into the render pass.
    CHECK(formatGutterTimestamp("%K", Afternoon).empty());
}

TEST_CASE("Gutter.timestamps.aBraceIsNeverPartOfAFormat", "[gutter]")
{
    // No chrono specification holds a brace, and the format is spliced into a replacement field: a doubled
    // closing brace would end that field early and come out as a literal one.
    for (std::string_view const format: { "%H}}", "%H}}}x}" })
    {
        CAPTURE(format);
        CHECK(formatGutterTimestamp(format, Afternoon).empty());
        CHECK(measureTimestampFormat(format) == std::unexpected { TimestampFormatError::Malformed });
    }
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

TEST_CASE("Gutter.timestamps.aFormatMustBeginWithAConversion", "[gutter]")
{
    // std::format rejects most leading literals by itself, but reads a leading digit as a width and a
    // leading <, > or ^ -- bare, or after a fill character such as % -- as an alignment, and pads the
    // timestamp with them: text meant to be drawn would turn into padding, or into the whole default
    // time point.
    for (std::string_view const format: { "[%H:%M]", "8%H", ">%H:%M", "%<9%H", "%^" })
    {
        CAPTURE(format);
        CHECK(measureTimestampFormat(format)
              == std::unexpected { TimestampFormatError::NoLeadingConversion });
        CHECK(resolveTimestampFormat(format).format == DefaultTimestampFormat);
    }

    // Past the first conversion an alignment character is a literal, and %% is a conversion like any other.
    CHECK(measureTimestampFormat("%H<%M") == 5);
    CHECK(measureTimestampFormat("%%%H") == 3);
}

TEST_CASE("Gutter.timestamps.anUnusableFormatFallsBackToTheDefault", "[gutter]")
{
    CHECK(resolveTimestampFormat("%H:%M").format == "%H:%M");
    CHECK(resolveTimestampFormat("%H:%M").columns == 5);
    CHECK(resolveTimestampFormat("%K").format == DefaultTimestampFormat);
    CHECK(resolveTimestampFormat("").format == DefaultTimestampFormat);
    // The width is the width of what is spelled: the default's, for a format that falls back.
    CHECK(resolveTimestampFormat("%A %A %A %A").columns == 8);
}

TEST_CASE("Gutter.timestamps.aRowShowsItsTimeOnlyWhenItDiffersFromTheRowAbove", "[gutter]")
{
    auto run = TimestampRun { "%H:%M:%S", &fixedOffsetLocalTime<0> };
    CHECK(run.next(AfternoonUtc) == "14:03:07");
    CHECK(run.next(AfternoonUtc).empty()); // the same second as the row above
    CHECK(run.next(AfternoonUtc + 1s) == "14:03:08");
    CHECK(run.next(std::nullopt).empty());            // a row the cursor never reached
    CHECK(run.next(AfternoonUtc + 1s) == "14:03:08"); // the row above showed nothing to repeat

    // The SPELLED text is what is compared: at minute resolution, seconds apart read the same.
    auto minutes = TimestampRun { "%H:%M", &fixedOffsetLocalTime<0> };
    CHECK(minutes.next(AfternoonUtc) == "14:03");
    CHECK(minutes.next(AfternoonUtc + 30s).empty());
    CHECK(minutes.next(AfternoonUtc + 60s) == "14:04");
}

TEST_CASE("Gutter.timestamps.aRunConvertsAnInstantRepeatedFromTheRowAboveNoMore", "[gutter]")
{
    // The rows of one output batch share their instant: only the first of them is converted to local time.
    conversions = 0;
    auto run = TimestampRun { "%H:%M:%S", &countingLocalTime };
    CHECK(run.next(AfternoonUtc) == "14:03:07");
    CHECK(run.next(AfternoonUtc).empty());
    CHECK(run.next(AfternoonUtc).empty());
    CHECK(conversions == 1);

    CHECK(run.next(AfternoonUtc + 1s) == "14:03:08");
    CHECK(conversions == 2);
}

TEST_CASE("Gutter.timestamps.aLayoutCarriesTheFormatItReservedTheWidthOf", "[gutter]")
{
    auto const layoutWith = [](std::string format) {
        return gutterLayoutFor(GutterSettings { .foldMarkers = false,
                                                .exitStatus = false,
                                                .userMarks = false,
                                                .timestamps = true,
                                                .timestampFormat = std::move(format) });
    };
    CHECK(layoutWith("%H:%M").timestampFormat == "%H:%M");

    // An unusable format is spelled, and reserved, as the default.
    auto const fallback = layoutWith("%K");
    CHECK(fallback.timestampFormat == DefaultTimestampFormat);
    auto run = TimestampRun { fallback.timestampFormat, &fixedOffsetLocalTime<0> };
    CHECK(run.next(AfternoonUtc) == "14:03:07");

    CHECK(gutterLayoutFor(GutterSettings { .foldMarkers = true }).timestampFormat.empty());
}

TEST_CASE("Gutter.timestamps.measuredAtTheLongestMonthToo", "[gutter]")
{
    // September is the longest month name, as Wednesday is the longest weekday.
    CHECK(measureTimestampFormat("%B") == 9);
    CHECK(measureTimestampFormat("%a %d %b") == 10);
}

TEST_CASE("Gutter.timestamps.measuringCountsColumnsNotBytes", "[gutter]")
{
    // A literal in the format is spelled as written: U+00B7 MIDDLE DOT is two bytes of UTF-8 and one column.
    CHECK(measureTimestampFormat("%H\xC2\xB7%M") == 5);
}

TEST_CASE("Gutter.timestamps.everyCharacterMustTakeExactlyOneColumn", "[gutter]")
{
    // The gutter gives every character of a timestamp a column of its own. U+6642 (時) needs two, so the text
    // would run a column past the width reserved for it; U+0301 COMBINING ACUTE ACCENT needs none, so it
    // would be drawn in a cell of its own rather than over the M before it.
    CHECK(measureTimestampFormat("%H\xE6\x99\x82%M")
          == std::unexpected { TimestampFormatError::WideOrZeroWidthCharacter });
    CHECK(measureTimestampFormat("%H:%M\xCC\x81")
          == std::unexpected { TimestampFormatError::WideOrZeroWidthCharacter });

    // Refused like any other unusable format: the gutter spells, and reserves the width of, the default.
    auto const gutterWidthWith = [](std::string format) {
        return gutterLayoutFor(GutterSettings { .foldMarkers = false,
                                                .exitStatus = false,
                                                .userMarks = false,
                                                .timestamps = true,
                                                .timestampFormat = std::move(format) })
            .totalColumns;
    };
    CHECK(resolveTimestampFormat("%H\xE6\x99\x82%M").format == DefaultTimestampFormat);
    CHECK(gutterWidthWith("%H\xE6\x99\x82%M") == gutterWidthWith(std::string { DefaultTimestampFormat }));
}

TEST_CASE("Gutter.timestamps.theWidestUsableFormatFillsTheLimit", "[gutter]")
{
    // MaxTimestampColumns is the last width that fits; one column more is too wide.
    auto const widest = "%H" + std::string(static_cast<std::size_t>(MaxTimestampColumns) - 2, '-');
    CHECK(measureTimestampFormat(widest) == MaxTimestampColumns);
    CHECK(measureTimestampFormat(widest + "-") == std::unexpected { TimestampFormatError::TooWide });
}

TEST_CASE("Gutter.timestamps.everyFormatErrorSaysWhy", "[gutter]")
{
    auto const texts = std::array { timestampFormatErrorText(TimestampFormatError::Empty),
                                    timestampFormatErrorText(TimestampFormatError::NoLeadingConversion),
                                    timestampFormatErrorText(TimestampFormatError::Malformed),
                                    timestampFormatErrorText(TimestampFormatError::WideOrZeroWidthCharacter),
                                    timestampFormatErrorText(TimestampFormatError::TooWide) };
    for (auto const text: texts)
    {
        CHECK_FALSE(text.empty());
        CHECK(std::ranges::count(texts, text) == 1); // and each says something the others do not
    }
}

TEST_CASE("Gutter.timestamps.theSystemZoneMovesWholeMinutesOnly", "[gutter]")
{
    // The production converter reads the process's own zone, which a test cannot pin -- but every zone
    // in use is a whole number of minutes off UTC, so the seconds survive the conversion everywhere.
    for (auto const instant:
         { chrono::sys_days { chrono::year { 2026 } / chrono::January / 15 } + 7h + 8min + 9s,
           chrono::sys_days { chrono::year { 2026 } / chrono::July / 15 } + 19h + 20min + 21s })
    {
        auto const offset = systemLocalTime(instant).time_since_epoch() - instant.time_since_epoch();
        CHECK(offset % 1min == 0s);
        CHECK(chrono::abs(offset) <= 14h);
    }
}
// }}}

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
    REQUIRE(layout.segments().size() == 3);
    CHECK(layout.segments()[0]
          == GutterSegment { .kind = GutterSegmentKind::Timestamp, .firstColumn = 0, .columns = 9 });
    CHECK(layout.segments()[1]
          == GutterSegment { .kind = GutterSegmentKind::LineNumber, .firstColumn = 9, .columns = 6 });
    CHECK(layout.segments()[2]
          == GutterSegment { .kind = GutterSegmentKind::BlockColumn, .firstColumn = 15, .columns = 1 });
    CHECK(layout.totalColumns == 16);
    REQUIRE(layout.segment(GutterSegmentKind::LineNumber).has_value());
    CHECK(layout.segment(GutterSegmentKind::LineNumber)->firstColumn == 9);
}

TEST_CASE("Gutter.layout.theDefaultIsTheOneColumnItAlwaysWas", "[gutter]")
{
    // Fold markers alone -- what `folding.show_markers` always reserved -- still take exactly one column,
    // so a user who changes nothing keeps their page width.
    auto const layout = gutterLayoutFor(GutterSettings { .foldMarkers = true });
    REQUIRE(layout.segments().size() == 1);
    CHECK(layout.segments()[0].kind == GutterSegmentKind::BlockColumn);
    CHECK(layout.totalColumns == 1);
}

TEST_CASE("Gutter.layout.anyOfTheBlockColumnsJobsReservesIt", "[gutter]")
{
    auto const off = GutterSettings { .foldMarkers = false, .exitStatus = false, .userMarks = false };
    CHECK(gutterLayoutFor(off).totalColumns == 0);
    CHECK(gutterLayoutFor(off).segments().empty());

    auto foldOnly = off;
    foldOnly.foldMarkers = true;
    auto statusOnly = off;
    statusOnly.exitStatus = true;
    auto marksOnly = off;
    marksOnly.userMarks = true;
    for (auto const& settings: { foldOnly, statusOnly, marksOnly })
        CHECK(gutterLayoutFor(settings).totalColumns == 1);
}

TEST_CASE("Gutter.layout.theBlockColumnTakesItsConfiguredWidth", "[gutter]")
{
    // gutter.block_column_width: two cells, the configuration's default, and held to 1..2 like the line
    // numbers' width is held to theirs -- the one place the range is enforced on the way to the screen.
    auto settings = GutterSettings { .foldMarkers = true, .exitStatus = true };
    for (auto const [asked, taken]: std::array { std::pair { 1, 1 },
                                                 std::pair { 2, 2 },
                                                 std::pair { 0, MinBlockColumnWidth },
                                                 std::pair { 7, MaxBlockColumnWidth } })
    {
        INFO("block_column_width " << asked);
        settings.blockColumnWidth = asked;
        auto const layout = gutterLayoutFor(settings);
        REQUIRE(layout.segment(GutterSegmentKind::BlockColumn).has_value());
        CHECK(layout.segment(GutterSegmentKind::BlockColumn)->columns == taken);
        CHECK(layout.totalColumns == taken);
    }

    // Beside the grid, wherever the line numbers stand.
    settings.blockColumnWidth = 2;
    settings.lineNumbers = LineNumberMode::Absolute;
    settings.lineNumberWidth = 4;
    auto const layout = gutterLayoutFor(settings);
    CHECK(layout.segment(GutterSegmentKind::BlockColumn)
          == GutterSegment { .kind = GutterSegmentKind::BlockColumn, .firstColumn = 4, .columns = 2 });
    CHECK(gutterSegmentAt(layout, 4) == GutterSegmentKind::BlockColumn);
    CHECK(gutterSegmentAt(layout, 5) == GutterSegmentKind::BlockColumn);
}

TEST_CASE("Gutter.layout.aDefaultConstructedSettingsReservesNothing", "[gutter]")
{
    // The engine's defaults are all off, so a terminal configured by hand has no gutter unless it asks.
    auto const layout = gutterLayoutFor(GutterSettings {});
    CHECK(layout.segments().empty());
    CHECK(layout.totalColumns == 0);
    CHECK_FALSE(layout.segment(GutterSegmentKind::BlockColumn).has_value());
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
    auto const layout = gutterLayoutFor(GutterSettings { .foldMarkers = false,
                                                         .exitStatus = false,
                                                         .userMarks = false,
                                                         .timestamps = true,
                                                         .timestampFormat = "%K" });
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
        Row { LineNumberMode::Hybrid, 42, -3, 3 },   Row { LineNumberMode::Hybrid, 42, 4, 4 },
    };
    for (auto const& row: rows)
    {
        CAPTURE(static_cast<int>(row.mode), row.absolute, row.distance);
        CHECK(lineNumberFor(row.mode, row.absolute, row.distance) == row.shown);
    }
}

TEST_CASE("Gutter.lineNumbers.theFarthestDistanceDoesNotOverflow", "[gutter]")
{
    // The magnitude of the most negative distance is one more than any int64_t holds.
    auto const farthest = std::numeric_limits<int64_t>::min();
    CHECK(lineNumberFor(LineNumberMode::Relative, 1, farthest) == uint64_t { 1 } << 63U);
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

TEST_CASE("Gutter.lineNumbers.theTextIsAlwaysExactlyTheWidthAskedFor", "[gutter]")
{
    CHECK(lineNumberText(7, 1) == U"7");
    CHECK(lineNumberText(42, 1) == U"…");
    CHECK(lineNumberText(42, 0).empty());
    CHECK(lineNumberText(42, -3).empty());
    // No segment is wider than MaxLineNumberWidth, and neither is its text; the widest number fits behind
    // the mark.
    CHECK(lineNumberText(42, MaxLineNumberWidth + 2).view().size()
          == static_cast<size_t>(MaxLineNumberWidth));
    CHECK(lineNumberText(std::numeric_limits<uint64_t>::max(), MaxLineNumberWidth) == U"…709551615");
}
// }}}

// {{{ block column
TEST_CASE("Gutter.blockColumn.followsThePrecedenceTable", "[gutter]")
{
    auto const everything = GutterSettings { .foldMarkers = true, .exitStatus = true, .userMarks = true };

    struct Row
    {
        char const* name = nullptr;
        BlockColumnFacts facts {};
        BlockColumnGlyph glyph = BlockColumnGlyph::None;
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
              { .fold = FoldMarker::Expanded,
                .head = BlockHeadState::Finished,
                .mark = UserMarkPresence::Present },
              BlockColumnGlyph::FoldExpanded },
        // Over a fold body the mark replaces the bar for that row.
        Row { "marked body",
              { .fold = FoldMarker::Body, .mark = UserMarkPresence::Present },
              BlockColumnGlyph::UserMark },
        Row { "marked body end",
              { .fold = FoldMarker::BodyEnd, .mark = UserMarkPresence::Present },
              BlockColumnGlyph::UserMark },
        Row { "body", { .fold = FoldMarker::Body }, BlockColumnGlyph::FoldBody },
        Row { "body end", { .fold = FoldMarker::BodyEnd }, BlockColumnGlyph::FoldBodyEnd },
        Row { "running head", { .head = BlockHeadState::Running }, BlockColumnGlyph::Running },
        // A finished command that printed nothing has no fold to draw; its head says that it finished.
        Row { "finished head, nothing folded",
              { .head = BlockHeadState::Finished },
              BlockColumnGlyph::Finished },
        Row { "marked row outside any block",
              { .mark = UserMarkPresence::Present },
              BlockColumnGlyph::UserMark },
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

TEST_CASE("Gutter.blockColumn.aFailureIsAShapeAsWellAsAColour", "[gutter]")
{
    // A reader who cannot tell red from grey still tells a failed command from one that succeeded: a cross
    // where a disc would be, and the body of its fold drawn heavy. Only with exit status on.
    auto const settings = GutterSettings { .foldMarkers = true, .exitStatus = true };
    auto const failed = std::optional { CommandBlockOutcome::Failure };
    auto const succeeded = std::optional { CommandBlockOutcome::Success };

    auto const head = BlockColumnFacts { .head = BlockHeadState::Finished };
    CHECK(blockColumnGlyph(BlockColumnFacts { .head = BlockHeadState::Finished, .outcome = failed }, settings)
          == BlockColumnGlyph::Failed);
    CHECK(blockColumnGlyph(BlockColumnFacts { .head = BlockHeadState::Finished, .outcome = succeeded },
                           settings)
          == BlockColumnGlyph::Finished);
    CHECK(blockColumnGlyph(head, settings) == BlockColumnGlyph::Finished);

    CHECK(blockColumnGlyph(BlockColumnFacts { .fold = FoldMarker::Body, .outcome = failed }, settings)
          == BlockColumnGlyph::FoldBodyFailed);
    CHECK(blockColumnGlyph(BlockColumnFacts { .fold = FoldMarker::BodyEnd, .outcome = failed }, settings)
          == BlockColumnGlyph::FoldBodyEndFailed);
    CHECK(blockColumnGlyph(BlockColumnFacts { .fold = FoldMarker::Body, .outcome = succeeded }, settings)
          == BlockColumnGlyph::FoldBody);

    // The fold control on the head stays the control, whatever the command did: its colour says how it ended.
    CHECK(blockColumnGlyph(BlockColumnFacts { .fold = FoldMarker::Expanded,
                                              .head = BlockHeadState::Finished,
                                              .outcome = failed },
                           settings)
          == BlockColumnGlyph::FoldExpanded);

    // With exit status off nothing tells the outcome, by colour or by shape.
    auto const quiet = GutterSettings { .foldMarkers = true, .exitStatus = false };
    CHECK(blockColumnGlyph(BlockColumnFacts { .fold = FoldMarker::Body, .outcome = failed }, quiet)
          == BlockColumnGlyph::FoldBody);
}

TEST_CASE("Gutter.blockColumn.aPressTogglesAFoldWhereverItReachesAndOpensAStatusMarksMenu", "[gutter]")
{
    using enum BlockColumnGlyph;
    // A fold that may toggle wins on every row it reaches -- a user mark inside its run included.
    for (auto const glyph: { FoldExpanded, FoldBody, FoldBodyEndFailed, UserMark, Finished, None })
        CHECK(blockColumnPressFor(glyph, FoldToggle::Available) == BlockColumnPress::ToggleFold);

    // With none to toggle, a status mark opens its block's menu, and anything else does nothing.
    for (auto const glyph: { Running, Finished, Failed })
        CHECK(blockColumnPressFor(glyph, FoldToggle::Unavailable) == BlockColumnPress::OpenBlockMenu);
    for (auto const glyph:
         { None, FoldExpanded, FoldCollapsed, FoldBody, FoldBodyEnd, UserMark, FoldBodyFailed })
        CHECK(blockColumnPressFor(glyph, FoldToggle::Unavailable) == BlockColumnPress::Swallow);
}

TEST_CASE("Gutter.blockColumn.glyphsAreTheirCodepoints", "[gutter]")
{
    CHECK(blockColumnCodepoint(BlockColumnGlyph::None) == U'\0');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldExpanded) == foldMarkerGlyph(FoldMarker::Expanded));
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldCollapsed) == foldMarkerGlyph(FoldMarker::Collapsed));
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldBody) == foldMarkerGlyph(FoldMarker::Body));
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldBodyEnd) == foldMarkerGlyph(FoldMarker::BodyEnd));
    // Contour's own private-use codepoints, as the fold heads are: vtrasterizer draws them, and the shapes
    // they look like -- U+25C6, U+25B8, U+25CF -- stay the font's wherever a program writes them.
    CHECK(blockColumnCodepoint(BlockColumnGlyph::UserMark) == U'􏀃');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::Running) == U'􏀂');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::Finished) == U'􏀄');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::Failed) == U'\U0010F005');
    // The heavy body needs no codepoint of its own: Unicode's heavy box drawing says it exactly.
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldBodyFailed) == U'\u2503');
    CHECK(blockColumnCodepoint(BlockColumnGlyph::FoldBodyEndFailed) == U'\u2517');
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

        auto const hovered =
            blockColumnColors(palette, BlockColumnGlyph::FoldBody, outcome, GutterHover::Hovered);
        CHECK(hovered.foreground == palette.foldMarkerHoverColors().foreground);
        CHECK(hovered.background == palette.foldMarkerHoverColors().background);
    }
}

TEST_CASE("Gutter.colors.anOutcomeTakesItsColourAndIsLiftedWhenHovered", "[gutter]")
{
    auto const palette = ColorPalette {};
    auto const failure = palette.blockStatusColor(CommandBlockOutcome::Failure);

    auto const rest = blockColumnColors(
        palette, BlockColumnGlyph::FoldExpanded, CommandBlockOutcome::Failure, GutterHover::Rest);
    CHECK(rest.foreground == failure);
    CHECK(rest.background == palette.defaultBackground);

    auto const hovered = blockColumnColors(
        palette, BlockColumnGlyph::FoldExpanded, CommandBlockOutcome::Failure, GutterHover::Hovered);
    CHECK(hovered.foreground
          == mixColor(failure, palette.foldMarkerHoverColors().foreground, BlockStatusHoverLift));
    CHECK(hovered.background == palette.defaultBackground);

    // A success the scheme DOES colour is an outcome like any other.
    auto coloured = ColorPalette {};
    coloured.blockStatus[static_cast<size_t>(CommandBlockOutcome::Success)] = RGBColor(0x10, 0x80, 0x10);
    CHECK(blockColumnColors(
              coloured, BlockColumnGlyph::FoldBody, CommandBlockOutcome::Success, GutterHover::Rest)
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
                                  .lineNumber = LineNumberText { U" 42" },
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
    CHECK(at(7) == std::pair { blockColumnCodepoint(BlockColumnGlyph::Running), -1 });
    for (auto const& cell: cells)
        CHECK(cell.lineOffset == LineOffset(2));
    CHECK(cells[0].attributes.foregroundColor == RGBColor(1, 2, 3));
    CHECK(cells[7].attributes.foregroundColor == RGBColor(7, 8, 9));
}

TEST_CASE("Gutter.paint.aTwoCellBlockColumnDrawsOneGlyphAcrossBoth", "[gutter]")
{
    // Not a cell-sized glyph beside a blank: one glyph, at the column's first cell, rasterised two cells
    // wide -- the renderer draws a DoubleWidth cell's glyph and background across two cells.
    auto const glyph = RGBColorPair { .foreground = RGBColor(7, 8, 9), .background = RGBColor(4, 5, 6) };

    auto const paint = [&](int width) {
        auto const layout =
            gutterLayoutFor(GutterSettings { .foldMarkers = true, .blockColumnWidth = width });
        auto cells = std::vector<RenderGutterCell> {};
        appendGutterCells(layout,
                          GutterRow { .screenLine = LineOffset(0),
                                      .glyph = BlockColumnGlyph::FoldExpanded,
                                      .glyphColors = glyph },
                          RGBColorPair {},
                          cells);
        REQUIRE(cells.size() == 1);
        return cells.front();
    };

    auto const wide = paint(2);
    CHECK(wide.column == ColumnOffset(-2));
    CHECK(wide.attributes.lineFlags.test(LineFlag::DoubleWidth));
    CHECK(wide.attributes.foregroundColor == RGBColor(7, 8, 9));

    auto const narrow = paint(1);
    CHECK(narrow.column == ColumnOffset(-1));
    CHECK_FALSE(narrow.attributes.lineFlags.test(LineFlag::DoubleWidth));
}

TEST_CASE("Gutter.paint.aHoveredBlocksRowCarriesItsBandUnderTheGlyph", "[gutter]")
{
    // A thin line at the block column's far edge from the text, drawn across the column as its glyph is,
    // and ahead of it so the glyph draws over it where the two meet. On the page's own background, so no
    // rectangle is drawn behind it.
    auto const layout = gutterLayoutFor(GutterSettings { .foldMarkers = true, .blockColumnWidth = 2 });
    auto const background = RGBColor(4, 5, 6);
    auto cells = std::vector<RenderGutterCell> {};
    appendGutterCells(
        layout,
        GutterRow { .screenLine = LineOffset(3),
                    .glyph = BlockColumnGlyph::FoldBody,
                    .glyphColors = RGBColorPair { .foreground = RGBColor(7, 8, 9), .background = background },
                    .band = RGBColor(200, 0, 0) },
        RGBColorPair {},
        cells);
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].codepoint == BlockBandGlyph);
    CHECK(cells[0].column == ColumnOffset(-2));
    CHECK(cells[0].attributes.foregroundColor == RGBColor(200, 0, 0));
    CHECK(cells[0].attributes.backgroundColor == background);
    CHECK(cells[0].attributes.lineFlags.test(LineFlag::DoubleWidth));
    CHECK(cells[1].codepoint == blockColumnCodepoint(BlockColumnGlyph::FoldBody));

    // A row of the block with no glyph of its own still carries the band.
    cells.clear();
    appendGutterCells(layout,
                      GutterRow { .screenLine = LineOffset(4),
                                  .glyphColors = RGBColorPair { .foreground = {}, .background = background },
                                  .band = RGBColor(200, 0, 0) },
                      RGBColorPair {},
                      cells);
    REQUIRE(cells.size() == 1);
    CHECK(cells[0].codepoint == BlockBandGlyph);
}

TEST_CASE("Gutter.paint.anEmptyRowPaintsNothing", "[gutter]")
{
    auto const layout =
        gutterLayoutFor(GutterSettings { .foldMarkers = true, .lineNumbers = LineNumberMode::Absolute });
    auto cells = std::vector<RenderGutterCell> {};
    appendGutterCells(layout, GutterRow { .screenLine = LineOffset(0) }, RGBColorPair {}, cells);
    CHECK(cells.empty());
}

TEST_CASE("Gutter.paint.textStopsAtItsSegmentsEdge", "[gutter]")
{
    // Three columns of line numbers and nothing else: no block column to draw a glyph in.
    auto const layout = gutterLayoutFor(GutterSettings { .foldMarkers = false,
                                                         .exitStatus = false,
                                                         .userMarks = false,
                                                         .lineNumbers = LineNumberMode::Absolute,
                                                         .lineNumberWidth = 3 });
    REQUIRE(layout.totalColumns == 3);

    auto cells = std::vector<RenderGutterCell> {};
    appendGutterCells(layout,
                      GutterRow { .screenLine = LineOffset(1),
                                  .lineNumber = LineNumberText { U"12345" },
                                  .glyph = BlockColumnGlyph::Running },
                      RGBColorPair {},
                      cells);

    // The three digits that fit, at -3 .. -1. The two past the edge would land on the grid's own columns,
    // and the glyph has no column at all.
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].codepoint == U'1');
    CHECK(unbox<int>(cells[0].column) == -3);
    CHECK(cells[2].codepoint == U'3');
    CHECK(unbox<int>(cells[2].column) == -1);
}
// }}}

// {{{ fold label
TEST_CASE("Gutter.foldLabel.extentIsWhereItIsDrawn", "[gutter]")
{
    // One blank column after the text, then the label, clipped at the page's edge.
    auto const extent = [](int textColumns, std::size_t labelColumns) {
        return foldLabelExtent(LineOffset(4), ColumnCount(textColumns), ColumnCount(40), labelColumns)
            .transform(
                [](ColumnRange const& range) { return std::pair { range.fromColumn, range.toColumn }; });
    };
    CHECK(extent(6, 9) == std::pair { ColumnOffset(7), ColumnOffset(15) });
    CHECK(extent(30, 15) == std::pair { ColumnOffset(31), ColumnOffset(39) });
    CHECK(foldLabelExtent(LineOffset(4), ColumnCount(6), ColumnCount(40), 9)->line == LineOffset(4));
    // Nothing of it fits: no extent to click.
    CHECK_FALSE(extent(39, 9).has_value());
    CHECK_FALSE(extent(40, 9).has_value());
}

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

    auto const textOf = [](std::vector<RenderCell> const& cells) {
        auto text = std::u32string {};
        for (auto const& cell: cells)
            text += cell.codepoints;
        return text;
    };

    auto label = std::vector<RenderCell> {};
    layoutFoldLabel(label, LineOffset(4), ColumnCount(5), ColumnCount(40), U"⋯ 3 lines", attributes);
    REQUIRE_FALSE(label.empty());
    // One blank column after the row's text.
    CHECK(label.front().position == CellLocation { .line = LineOffset(4), .column = ColumnOffset(6) });
    CHECK(textOf(label) == U"⋯ 3 lines");
    CHECK(label.front().attributes.foregroundColor == RGBColor(1, 2, 3));

    // Nine columns left on the page: cut to fit, never wrapped onto the next row.
    auto clipped = std::vector<RenderCell> {};
    layoutFoldLabel(clipped, LineOffset(0), ColumnCount(30), ColumnCount(40), U"⋯ 1,234 lines", attributes);
    CHECK(textOf(clipped) == U"⋯ 1,234 l");

    // No room at all: no label.
    auto none = std::vector<RenderCell> {};
    layoutFoldLabel(none, LineOffset(0), ColumnCount(39), ColumnCount(40), U"⋯ 1 line", attributes);
    CHECK(none.empty());
}

TEST_CASE("Gutter.foldLabel.isClippedByWidthNotByCodepoints", "[gutter]")
{
    // Four columns left: two wide glyphs fill them, and a third would cross the page's edge.
    auto label = std::vector<RenderCell> {};
    layoutFoldLabel(label, LineOffset(0), ColumnCount(35), ColumnCount(40), U"中中中", RenderAttributes {});
    REQUIRE(label.size() == 2);
    CHECK(label.back().position.column + ColumnOffset(label.back().width) <= ColumnOffset(40));
}
// }}}
