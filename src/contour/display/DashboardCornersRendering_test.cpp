// SPDX-License-Identifier: Apache-2.0
//
// A display-gated pixel test of the rounded box corners an extension's dashboard draws its panels with. The
// box-drawing renderer's own tests check that a corner's edge pixels meet the line it continues into, glyph
// by glyph in the atlas; this case checks the same in the frame a REAL TerminalDisplay draws through the RHI,
// fed the orchestrator dashboard's `lists` golden, so that whatever stands between the atlas and the screen
// (tile placement, cell geometry, blending) cannot open a step or a notch at a corner either.
//
// Like every [display] case this SKIPs unless CONTOUR_TEST_DISPLAY=1 opts into a real windowing system
// (scripts/run-display-tests.sh runs it under a headless weston).

#include <contour/config/Config.hpp>
#include <contour/test/DisplayHarness.hpp>
#include <contour/test/DisplayPixels.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>

#include <vtrasterizer/BoxDrawingStyle.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace contour::test::pixel;
using namespace std::string_view_literals;

namespace
{

/// The dashboard's `lists` frame at 50×20, the size the harness's window holds: one panel, its four rounded
/// corners.
[[nodiscard]] std::filesystem::path listsGoldenPath()
{
    return std::filesystem::path { CONTOUR_PROJECT_SOURCE_DIR } / "extensions" / "orchestrator" / "view"
           / "testdata" / "lists-50x20.screen";
}

/// The golden's rows, as the VT output that draws them: each row addressed on its own, so that a full row
/// neither wraps nor scrolls the page.
/// @param golden The golden's text, one row per line.
/// @return The output that puts every row in its place, from the top left.
[[nodiscard]] std::string framePainting(std::string_view golden)
{
    auto output = std::string { "\033[?25l\033[H\033[2J" };
    for (auto const [index, row]: core::views::enumerate(std::views::split(golden, '\n')))
    {
        auto const text = std::string_view { row.begin(), row.end() };
        if (text.empty())
            continue;
        output += std::format("\033[{};1H{}", index + 1, text);
    }
    return output;
}

/// A side of a cell.
enum class Edge : uint8_t
{
    Left,
    Right,
    Top,
    Bottom,
};

/// A rounded corner, and the sides along which it continues into a straight line.
struct Corner
{
    char32_t codepoint;     ///< ╭ ╮ ╯ or ╰.
    Edge towardsHorizontal; ///< The side it shares with the ─ it continues into.
    Edge towardsVertical;   ///< The side it shares with the │ it continues into.
};

constexpr auto Corners = std::array {
    Corner { .codepoint = U'╭', .towardsHorizontal = Edge::Right, .towardsVertical = Edge::Bottom },
    Corner { .codepoint = U'╮', .towardsHorizontal = Edge::Left, .towardsVertical = Edge::Bottom },
    Corner { .codepoint = U'╯', .towardsHorizontal = Edge::Left, .towardsVertical = Edge::Top },
    Corner { .codepoint = U'╰', .towardsHorizontal = Edge::Right, .towardsVertical = Edge::Top },
};

/// The arc styles the renderer offers, each of which must meet its lines.
constexpr auto ArcStyles = std::array { vtrasterizer::BoxDrawingStyle::ArcStyle::Elliptic,
                                        vtrasterizer::BoxDrawingStyle::ArcStyle::Round };

/// A cell of the page.
struct CellAt
{
    int row;
    int column;
};

/// The cell beyond @p edge of @p cell.
[[nodiscard]] constexpr CellAt neighbour(CellAt cell, Edge edge) noexcept
{
    switch (edge)
    {
        case Edge::Left: return { .row = cell.row, .column = cell.column - 1 };
        case Edge::Right: return { .row = cell.row, .column = cell.column + 1 };
        case Edge::Top: return { .row = cell.row - 1, .column = cell.column };
        case Edge::Bottom: return { .row = cell.row + 1, .column = cell.column };
    }
    return cell;
}

/// The line of pixels on @p cell's side @p edge, and the one just across it in the neighbouring cell.
struct SeamPixels
{
    std::vector<vtbackend::RGBColor> inside;  ///< The corner cell's outermost pixels on that side.
    std::vector<vtbackend::RGBColor> outside; ///< The neighbour's outermost pixels on the side it shares.
};

[[nodiscard]] SeamPixels seamAt(QImage const& frame, CellGeometry const& g, CellAt cell, Edge edge)
{
    auto const area = g.cells(cell.row, cell.column, cell.column);
    auto const vertical = edge == Edge::Left || edge == Edge::Right;
    auto const [insideFixed, outsideFixed] = [&] {
        switch (edge)
        {
            case Edge::Left: return std::pair { area.left(), area.left() - 1 };
            case Edge::Right: return std::pair { area.right(), area.right() + 1 };
            case Edge::Top: return std::pair { area.top(), area.top() - 1 };
            case Edge::Bottom: return std::pair { area.bottom(), area.bottom() + 1 };
        }
        return std::pair { 0, 0 };
    }();
    auto const along = vertical ? std::views::iota(area.top(), area.bottom() + 1)
                                : std::views::iota(area.left(), area.right() + 1);
    auto seam = SeamPixels {};
    for (auto const at: along)
    {
        auto const pixel = [&](int fixed) {
            return rgbOf(vertical ? frame.pixel(fixed, at) : frame.pixel(at, fixed));
        };
        seam.inside.push_back(pixel(insideFixed));
        seam.outside.push_back(pixel(outsideFixed));
    }
    return seam;
}

/// The page as the frame's buffer draws it, row by row: a trivial row's text, and every other cell's first
/// codepoint at its column. The golden's characters are all one column wide, so a column is an index.
/// @param buffer What the terminal handed the renderer.
/// @return Each drawn screen row's codepoints, by row.
[[nodiscard]] std::map<int, std::u32string> pageText(vtbackend::RenderBuffer const& buffer)
{
    auto page = std::map<int, std::u32string> {};
    for (auto const& line: buffer.lines)
        page[unbox<int>(line.lineOffset)] = line.text;
    for (auto const& cell: buffer.cells)
    {
        if (cell.codepoints.empty())
            continue;
        auto& row = page[unbox<int>(cell.position.line)];
        auto const column = unbox<size_t>(cell.position.column);
        if (row.size() <= column)
            row.resize(column + 1, U' ');
        row[column] = cell.codepoints.front();
    }
    return page;
}

/// The codepoint drawn at @p cell, if any is.
[[nodiscard]] std::optional<char32_t> codepointAt(std::map<int, std::u32string> const& page, CellAt cell)
{
    auto const row = page.find(cell.row);
    if (row == page.end() || cell.column < 0 || std::cmp_greater_equal(cell.column, row->second.size()))
        return std::nullopt;
    return row->second[static_cast<size_t>(cell.column)];
}

/// The text of a colour row, for the failure message.
[[nodiscard]] std::string hexRow(std::vector<vtbackend::RGBColor> const& colors)
{
    auto text = std::string {};
    for (auto const color: colors)
        text += hexOf(color) + ' ';
    return text;
}

} // namespace

TEST_CASE("display: the dashboard's rounded corners meet their lines in the real renderer",
          "[display][boxdrawing]")
{
    REQUIRE_DISPLAY_OR_SKIP();

    auto file = std::ifstream { listsGoldenPath(), std::ios::binary };
    REQUIRE(file.good());
    auto const golden = std::string { std::istreambuf_iterator<char> { file }, {} };

    for (auto const arcStyle: ArcStyles)
    {
        INFO("arc style " << static_cast<int>(arcStyle));
        auto const variant =
            Variant { .scheme = "contour-dark"sv, .style = contour::config::UiStyle::Native };
        auto const h = makeHarness(
            variant, [&](contour::config::Config& config) { config.boxArcStyle.value() = arcStyle; });
        auto const page = h->session->terminal().pageSize();
        REQUIRE(unbox<int>(page.columns) >= 50);
        REQUIRE(unbox<int>(page.lines) >= 20);

        h->feedAndSettle(framePainting(golden));
        auto const frame = frameOf(*h);
        auto const g = geometryOf(*h);
        auto const ground = h->session->terminal().colorPalette().defaultBackground;

        auto cornersSeen = 0;
        auto const text = pageText(frame.buffer);
        for (auto const& [row, codepoints]: text)
            for (auto const [column, codepoint]: core::views::enumerate(codepoints))
            {
                auto const corner = std::ranges::find(Corners, codepoint, &Corner::codepoint);
                if (corner == Corners.end())
                    continue;
                ++cornersSeen;
                auto const at = CellAt { .row = row, .column = static_cast<int>(column) };

                for (auto const [edge, line]: std::array { std::pair { corner->towardsHorizontal, U'─' },
                                                           std::pair { corner->towardsVertical, U'│' } })
                {
                    auto const across = neighbour(at, edge);
                    INFO(std::format("corner U+{:04X} at row {} column {}, towards row {} column {}",
                                     static_cast<uint32_t>(corner->codepoint),
                                     at.row,
                                     at.column,
                                     across.row,
                                     across.column));
                    // The golden draws a box: every corner continues into the line it meets.
                    REQUIRE(codepointAt(text, across) == line);

                    auto const seam = seamAt(frame.image, g, at, edge);
                    INFO("corner side: " << hexRow(seam.inside));
                    INFO("line side:   " << hexRow(seam.outside));

                    // The stroke reaches the side: a seam of nothing but page would meet trivially.
                    CHECK(std::ranges::any_of(seam.inside, [&](auto c) { return !nearlyEqual(c, ground); }));

                    // Pixel for pixel, the arc's end lies over the line's end: no step, no notch, no fringe.
                    CHECK(std::ranges::equal(
                        seam.inside, seam.outside, [](auto a, auto b) { return nearlyEqual(a, b, 2); }));
                }
            }
        // ╭ ╮ ╯ ╰ of the panel.
        CHECK(cornersSeen == 4);
    }
}
