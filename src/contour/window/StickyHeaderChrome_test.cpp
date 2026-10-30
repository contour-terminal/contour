// SPDX-License-Identifier: Apache-2.0
//
// Binding the sticky header data (spec §3.12): what the pane header or the title band shows of the header
// the terminal laid out.

#include <contour/window/StickyHeaderChrome.hpp>

#include <vtbackend/render/RenderBuffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>

using contour::config::StatusColorRole;
using contour::window::bindStickyHeader;
using contour::window::StickyHeaderChrome;
using contour::window::StickyHeaderGlyphRows;
using vtbackend::CommandBlockId;
using vtbackend::CommandBlockOutcome;
using vtbackend::RenderStickyHeader;
using vtbackend::StickyHeaderKind;

namespace
{

/// @p text laid out from column @p column, as the terminal lays out the chip and the copied row.
void layOut(std::vector<vtbackend::RenderCell>& cells, std::u32string_view text, int column)
{
    vtbackend::layoutRenderText(cells,
                                text,
                                vtbackend::RenderAttributes {},
                                vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                                          .column = vtbackend::ColumnOffset(column) },
                                vtbackend::ColumnOffset(80));
}

/// A header for block 4 with @p outcome, reading @p cells and @p chip, and the record's @p commandLine.
[[nodiscard]] RenderStickyHeader headerOf(CommandBlockOutcome outcome,
                                          std::u32string_view cells,
                                          std::u32string_view chip,
                                          std::string commandLine,
                                          StickyHeaderKind kind = StickyHeaderKind::Command)
{
    auto header = RenderStickyHeader {};
    header.kind = kind;
    header.block = CommandBlockId { 4 };
    header.outcome = outcome;
    header.commandLine = std::move(commandLine);
    layOut(header.cells, cells, 0);
    layOut(header.chip, chip, 60);
    return header;
}

} // namespace

TEST_CASE("the glyph rows follow the outcomes, and running is the accent", "[stickyheader]")
{
    for (auto const index: std::views::iota(std::size_t { 0 }, StickyHeaderGlyphRows.size()))
        CHECK(StickyHeaderGlyphRows.at(index).outcome == static_cast<CommandBlockOutcome>(index));
    CHECK(StickyHeaderGlyphRows.at(static_cast<std::size_t>(CommandBlockOutcome::Running)).role
          == StatusColorRole::Running);
}

TEST_CASE("binding the sticky header data", "[stickyheader]")
{
    SECTION("a success names its command and its time")
    {
        auto const bound = bindStickyHeader(headerOf(CommandBlockOutcome::Success,
                                                     U"$ cmake --build --preset clang-debug",
                                                     U"✓ 2m 14s",
                                                     "cmake --build --preset clang-debug"));
        CHECK(bound
              == StickyHeaderChrome { .kind = StickyHeaderKind::Command,
                                      .block = CommandBlockId { 4 },
                                      .outcome = CommandBlockOutcome::Success,
                                      .statusRole = StatusColorRole::Success,
                                      .glyph = "✓",
                                      .command = "cmake --build --preset clang-debug",
                                      .detail = "2m 14s" });
    }

    SECTION("a failure keeps its exit code as the chip spells it")
    {
        auto const bound =
            bindStickyHeader(headerOf(CommandBlockOutcome::Failure, U"$ make", U"✗ 2 1m 04s", "make"));
        CHECK(bound.glyph == "✗");
        CHECK(bound.statusRole == StatusColorRole::Danger);
        CHECK(bound.detail == "2 1m 04s");
    }

    SECTION("a running block has no mark to drop")
    {
        auto const bound =
            bindStickyHeader(headerOf(CommandBlockOutcome::Running, U"$ sleep 9", U"running 8s", "sleep 9"));
        CHECK(bound.glyph == "▸");
        CHECK(bound.statusRole == StatusColorRole::Running);
        CHECK(bound.detail == "running 8s");
    }

    SECTION("with no command line, the copied row speaks for it")
    {
        auto const bound =
            bindStickyHeader(headerOf(CommandBlockOutcome::Success, U"  ~/src > make all  ", U"✓ 3s", {}));
        CHECK(bound.command == "~/src > make all");
    }

    SECTION("an evicted block shows the placeholder")
    {
        auto const bound = bindStickyHeader(headerOf(CommandBlockOutcome::Failure,
                                                     U"⋯ make — earlier output evicted",
                                                     U"✗ 2 1m 04s",
                                                     "make",
                                                     StickyHeaderKind::Evicted));
        CHECK(bound.kind == StickyHeaderKind::Evicted);
        CHECK(bound.glyph == "⋯");
        CHECK(bound.command == "make — earlier output evicted"); // its own ⋯ is the glyph
        CHECK(bound.detail == "2 1m 04s");
    }
}
