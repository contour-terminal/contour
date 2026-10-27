// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/shell/BlockRail.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

using namespace vtbackend;

namespace
{

/// A row of block @p block between rows of blocks @p above and @p below, its command finished.
[[nodiscard]] RailRowFacts finishedRow(std::optional<uint32_t> block,
                                       std::optional<uint32_t> above,
                                       std::optional<uint32_t> below)
{
    auto const idOf = [](std::optional<uint32_t> id) {
        return id ? std::optional { CommandBlockId { *id } } : std::nullopt;
    };
    return RailRowFacts { .block = idOf(block),
                          .above = idOf(above),
                          .below = idOf(below),
                          .state = CommandBlockState::Finished,
                          .outcome = CommandBlockOutcome::Success,
                          .mark = UserMarkPresence::Absent,
                          .attention = BlockAttention::Rest };
}

/// @return The HSL hue of @p color in degrees, [0, 360); nullopt for a grey, which has none.
[[nodiscard]] std::optional<double> hueOf(RGBColor color)
{
    auto const r = color.red / 255.0;
    auto const g = color.green / 255.0;
    auto const b = color.blue / 255.0;
    auto const high = std::max({ r, g, b });
    auto const chroma = high - std::min({ r, g, b });
    if (chroma == 0.0)
        return std::nullopt;
    auto sector = ((r - g) / chroma) + 4.0; // blue is the largest
    if (high == r)
        sector = std::fmod((g - b) / chroma, 6.0);
    else if (high == g)
        sector = ((b - r) / chroma) + 2.0;
    auto const degrees = sector * 60.0;
    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

} // namespace

TEST_CASE("blockRail.segments", "[blockrail]")
{
    struct Case
    {
        std::optional<uint32_t> block;
        std::optional<uint32_t> above;
        std::optional<uint32_t> below;
        RailSegment expected;
    };
    for (auto const& [block, above, below, expected]: {
             Case { .block = 7, .above = std::nullopt, .below = 7, .expected = RailSegment::Head },
             Case { .block = 7, .above = 7, .below = 7, .expected = RailSegment::Body },
             Case { .block = 7, .above = 7, .below = 8, .expected = RailSegment::Tail },
             Case { .block = 7, .above = 6, .below = 8, .expected = RailSegment::Single },
             Case { .block = std::nullopt, .above = 7, .below = 7, .expected = RailSegment::None },
         })
    {
        CAPTURE(block.value_or(0), above.value_or(0), below.value_or(0));
        CHECK(railSegmentOf(finishedRow(block, above, below)) == expected);
    }

    // The live prompt draws none, whatever its neighbours.
    for (auto const& [above, below]:
         { std::pair { std::optional<uint32_t> {}, std::optional<uint32_t> {} },
           std::pair { std::optional<uint32_t> { 7 }, std::optional<uint32_t> { 7 } },
           std::pair { std::optional<uint32_t> { 6 }, std::optional<uint32_t> {} } })
    {
        auto prompt = finishedRow(7, above, below);
        prompt.state = CommandBlockState::Prompting;
        CHECK(railSegmentOf(prompt) == RailSegment::None);
    }
}

TEST_CASE("blockRail.marksFollowTheTable", "[blockrail]")
{
    for (auto const i: std::views::iota(std::size_t { 0 }, CommandBlockOutcomeCount))
        CHECK(BlockRailMarkRows[i].outcome == static_cast<CommandBlockOutcome>(i));

    auto head = finishedRow(7, std::nullopt, 7);
    auto const markAt = [&](CommandBlockOutcome outcome, BlockAttention attention) {
        head.outcome = outcome;
        head.attention = attention;
        return railMarkOf(head, railSegmentOf(head));
    };

    // A success says nothing until the pointer or the keyboard is on it.
    CHECK(markAt(CommandBlockOutcome::Success, BlockAttention::Rest) == RailMark::None);
    CHECK(markAt(CommandBlockOutcome::Success, BlockAttention::Hovered) == RailMark::Check);
    CHECK(markAt(CommandBlockOutcome::Success, BlockAttention::Focused) == RailMark::Check);

    // A failure and a running block always say what they are.
    for (auto const attention: { BlockAttention::Rest, BlockAttention::Hovered, BlockAttention::Focused })
    {
        CAPTURE(static_cast<int>(attention));
        CHECK(markAt(CommandBlockOutcome::Failure, attention) == RailMark::Cross);
        CHECK(markAt(CommandBlockOutcome::Running, attention) == RailMark::Running);
    }

    // On the only row of a block too.
    auto single = finishedRow(7, 6, 8);
    single.outcome = CommandBlockOutcome::Failure;
    CHECK(railMarkOf(single, railSegmentOf(single)) == RailMark::Cross);

    // A body row of a failed block carries none: the mark is the head's.
    auto body = finishedRow(7, 7, 7);
    body.outcome = CommandBlockOutcome::Failure;
    CHECK(railMarkOf(body, railSegmentOf(body)) == RailMark::None);

    // A row the user marked shows it where it heads nothing: in a block's body, and outside any block.
    body.mark = UserMarkPresence::Present;
    CHECK(railMarkOf(body, railSegmentOf(body)) == RailMark::UserMark);
    auto loose = finishedRow(std::nullopt, std::nullopt, std::nullopt);
    loose.mark = UserMarkPresence::Present;
    CHECK(railMarkOf(loose, railSegmentOf(loose)) == RailMark::UserMark);
}

TEST_CASE("blockRail.runningIsNeverYellow", "[blockrail]")
{
    // The design record's rule: running is the accent, never yellow -- yellow means search.
    constexpr auto Yellow = 60.0;
    constexpr auto Distance = 30.0;
    for (auto const name: { std::string_view { "contour-dark" }, std::string_view { "contour-light" } })
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        // The shipped schemes' own running colour reads, so it is drawn as it is.
        auto const running = railColor(palette, CommandBlockOutcome::Running, BlockAttention::Rest);
        CHECK(running == palette.blockStatusColor(CommandBlockOutcome::Running));
        CHECK(railColor(palette, CommandBlockOutcome::Running, BlockAttention::Hovered)
              == palette.blockStatusHoverColor(CommandBlockOutcome::Running));
        auto const litHue = hueOf(railColor(palette, CommandBlockOutcome::Running, BlockAttention::Hovered));
        REQUIRE(litHue.has_value());
        CHECK(std::abs(*litHue - Yellow) > Distance);
        // An accent, not a grey -- and not yellow, at rest or lit.
        auto const restHue = hueOf(running);
        REQUIRE(restHue.has_value());
        CHECK(std::abs(*restHue - Yellow) > Distance);
    }
}

TEST_CASE("blockRail.theRailMarksOnEveryScheme", "[blockrail]")
{
    // The bar and its marks are marks: 3:1 against the pane they stand on, at rest and lit, on every bundled
    // scheme -- each one appearance's palette. Where the scheme's own status colour is too faint, it is
    // lifted toward the page's ink; lit still differs from at rest.
    for (auto const name: defaultColorPaletteNames())
    {
        INFO("scheme: " << name);
        auto palette = ColorPalette {};
        REQUIRE(defaultColorPalettes(name, palette));
        for (auto const i: std::views::iota(std::size_t { 0 }, CommandBlockOutcomeCount))
        {
            auto const outcome = static_cast<CommandBlockOutcome>(i);
            CAPTURE(i);
            auto const atRest = railColor(palette, outcome, BlockAttention::Rest);
            auto const lit = railColor(palette, outcome, BlockAttention::Hovered);
            CHECK(contrastRatio(atRest, palette.defaultBackground) >= MinimumMarkContrast);
            CHECK(contrastRatio(lit, palette.defaultBackground) >= MinimumMarkContrast);
            CHECK(lit != atRest);
        }
    }
}
