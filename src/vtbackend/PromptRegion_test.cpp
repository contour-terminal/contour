// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the live-prompt scan. The complement of the command-block scan: that one reconstructs
// FINISHED blocks, this one finds the prompt the user is standing in right now — the case an
// accessibility client asks about, and the one a CommandEnd-opened block can never see.

#include <vtbackend/PromptRegion.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vtbackend;

namespace
{

/// A prompt-region line source backed by a plain vector — the point of the PromptRegionLineSource seam.
/// Lines are given in the order the scan walks them: the cursor's line first, then upwards, one entry per
/// LOGICAL line.
class FakeLines final: public PromptRegionLineSource
{
  public:
    explicit FakeLines(std::vector<LogicalLineMarks> lines): _lines { std::move(lines) } {}

    [[nodiscard]] bool hasLineAt(size_t index) const override { return index < _lines.size(); }
    [[nodiscard]] LogicalLineMarks marksAt(size_t index) const override { return _lines.at(index); }

  private:
    std::vector<LogicalLineMarks> _lines;
};

constexpr auto DefaultScanBudget = size_t { 64 };

/// A line carrying no semantic mark at all.
LogicalLineMarks plain()
{
    return {};
}

} // namespace

TEST_CASE("PromptRegion.a fresh prompt the user has not submitted yet", "[promptregion]")
{
    // The accessibility case, and the one scanCommandBlocksBackward is blind to: `$ ` has been printed
    // and the user is typing into it. There is no CommandEnd anywhere, so no finished block exists.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::Marked, LineFlag::PromptEnd }, .promptEndOffset = ColumnOffset(2) },
    } };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE(region.has_value());
    CHECK(region->startIndex == 0);
    REQUIRE(region->inputBegin.has_value());
    CHECK(*region->inputBegin == ColumnOffset(2));
    CHECK(region->inputBeginIndex == 0);
}

TEST_CASE("PromptRegion.a multi-line prompt spans up to its start", "[promptregion]")
{
    // A two-line prompt: ;A on the upper line, ;B on the lower one the cursor sits in.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::PromptEnd }, .promptEndOffset = ColumnOffset(2) },
        { .flags = LineFlags { LineFlag::Marked } },
    } };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE(region.has_value());
    CHECK(region->startIndex == 1);
    CHECK(region->inputBeginIndex == 0);
    REQUIRE(region->inputBegin.has_value());
    CHECK(*region->inputBegin == ColumnOffset(2));
}

TEST_CASE("PromptRegion.a running command is not a prompt", "[promptregion]")
{
    SECTION("output started above the cursor")
    {
        // The cursor is somewhere in `make`'s output; the prompt it was launched from is further up.
        auto const lines = FakeLines { {
            plain(),
            { .flags = LineFlags { LineFlag::OutputStart } },
            { .flags = LineFlags { LineFlag::Marked, LineFlag::PromptEnd },
              .promptEndOffset = ColumnOffset(2) },
        } };

        auto const region = findLivePromptRegion(lines, DefaultScanBudget);

        REQUIRE_FALSE(region.has_value());
        CHECK(region.error() == PromptRegionError::InCommandOutput);
    }

    SECTION("output started on the cursor's own line")
    {
        auto const lines = FakeLines { {
            { .flags = LineFlags { LineFlag::OutputStart } },
            { .flags = LineFlags { LineFlag::Marked } },
        } };

        auto const region = findLivePromptRegion(lines, DefaultScanBudget);

        REQUIRE_FALSE(region.has_value());
        CHECK(region.error() == PromptRegionError::InCommandOutput);
    }

    SECTION("a line that is both an output start and a prompt start reads as output")
    {
        // Ambiguous by construction, and the safe reading is "a command is running": announcing a prompt
        // that is actually a command's first output line would put the caret in the wrong place.
        auto const lines = FakeLines { {
            { .flags = LineFlags { LineFlag::Marked, LineFlag::OutputStart } },
        } };

        auto const region = findLivePromptRegion(lines, DefaultScanBudget);

        REQUIRE_FALSE(region.has_value());
        CHECK(region.error() == PromptRegionError::InCommandOutput);
    }
}

TEST_CASE("PromptRegion.a prompt sharing a line with the previous command's end", "[promptregion]")
{
    // The common zsh/bash case: the last command's output did not end in a newline, so precmd's ;D and the
    // new prompt's ;A land on the SAME logical line. That line is still a live prompt.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::CommandEnd, LineFlag::Marked, LineFlag::PromptEnd },
          .promptEndOffset = ColumnOffset(7),
          .commandEndOffset = ColumnOffset(3) },
    } };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE(region.has_value());
    CHECK(region->startIndex == 0);
    REQUIRE(region->inputBegin.has_value());
    // The user's input begins after the prompt, which itself begins after the leftover output.
    CHECK(*region->inputBegin == ColumnOffset(7));
}

TEST_CASE("PromptRegion.only the prompt end closest to the cursor is used", "[promptregion]")
{
    // An older prompt further up must not overwrite this one's border.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::Marked, LineFlag::PromptEnd }, .promptEndOffset = ColumnOffset(2) },
        { .flags = LineFlags { LineFlag::PromptEnd }, .promptEndOffset = ColumnOffset(99) },
    } };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE(region.has_value());
    REQUIRE(region->inputBegin.has_value());
    CHECK(*region->inputBegin == ColumnOffset(2));
    CHECK(region->inputBeginIndex == 0);
}

TEST_CASE("PromptRegion.a shell emitting only ;A still yields a region", "[promptregion]")
{
    // Plenty of setups mark prompt starts but not prompt ends. Degrading to "a prompt, whose input border
    // is unknown" is far more useful than refusing to report one at all.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::Marked } },
    } };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE(region.has_value());
    CHECK(region->startIndex == 0);
    CHECK_FALSE(region->inputBegin.has_value());
}

TEST_CASE("PromptRegion.a scrollback with no marks reports no integration", "[promptregion]")
{
    auto const lines = FakeLines { { plain(), plain(), plain() } };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE_FALSE(region.has_value());
    CHECK(region.error() == PromptRegionError::NoPromptMark);
}

TEST_CASE("PromptRegion.an empty source reports no integration", "[promptregion]")
{
    auto const lines = FakeLines { {} };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE_FALSE(region.has_value());
    CHECK(region.error() == PromptRegionError::NoPromptMark);
}

TEST_CASE("PromptRegion.the prompt start having scrolled away is out of reach", "[promptregion]")
{
    // Scrollback eviction: the ;B is still here but the ;A that began the prompt has been dropped from
    // history. Distinct from NoPromptMark -- there IS integration, so a caller should keep asking.
    auto const lines = FakeLines { {
        { .flags = LineFlags { LineFlag::PromptEnd }, .promptEndOffset = ColumnOffset(2) },
        plain(),
    } };

    auto const region = findLivePromptRegion(lines, DefaultScanBudget);

    REQUIRE_FALSE(region.has_value());
    CHECK(region.error() == PromptRegionError::OutOfReach);
}

TEST_CASE("PromptRegion.the scan budget bounds the walk", "[promptregion]")
{
    // The walk runs under the terminal lock, so the bound is load-bearing: an unbounded climb over a
    // 100'000-line scrollback would stall the render thread.
    auto lines = std::vector<LogicalLineMarks> {};
    lines.resize(50, LogicalLineMarks {});
    lines.push_back({ .flags = LineFlags { LineFlag::Marked } });
    auto const source = FakeLines { lines };

    SECTION("a budget that reaches the prompt finds it")
    {
        auto const region = findLivePromptRegion(source, 64);
        REQUIRE(region.has_value());
        CHECK(region->startIndex == 50);
    }

    SECTION("a budget that stops short gives up rather than walking on")
    {
        auto const region = findLivePromptRegion(source, 10);
        REQUIRE_FALSE(region.has_value());
        // No mark was seen within the budget, so as far as the scan can tell there is no integration.
        CHECK(region.error() == PromptRegionError::NoPromptMark);
    }

    SECTION("a zero budget reads nothing at all")
    {
        auto const region = findLivePromptRegion(source, 0);
        REQUIRE_FALSE(region.has_value());
        CHECK(region.error() == PromptRegionError::NoPromptMark);
    }
}
