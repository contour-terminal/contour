// SPDX-License-Identifier: Apache-2.0
//
// A display-gated pixel test of the rounded box corners an extension's dashboard draws its panels with. The
// box-drawing renderer's own tests check that a corner's edge pixels meet the line it continues into, glyph
// by glyph in the atlas; this case checks the same in the frame a REAL TerminalDisplay draws through the RHI,
// fed the orchestrator dashboard's goldens, so that whatever stands between the atlas and the screen
// (tile placement, cell geometry, blending) cannot open a step or a notch at a corner either. The goldens
// cover a lone panel, the dispatch form (a popup drawn over the two panels beneath it), an agent's status
// board and a transcript page.
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

/// A dashboard golden the case draws, and what the harness's 800×600 window needs to show it whole.
struct Golden
{
    std::string_view file;          ///< The golden's file name in the view's testdata.
    std::optional<double> fontSize; ///< The font size that fits its page in the window; empty: the profile's.
    vtbackend::PageSize page;       ///< The page the golden was drawn for.
    int corners;                    ///< How many rounded corners it draws.
};

/// The goldens drawn, each in its own window.
/// @return `lists` at 50×20 (one panel, its four corners), the dispatch form over two panels at 120×40 (the
///         panels' eight corners and the popup's four), and a status board and a transcript page at 120×40
///         (one full-page panel each); every 120×40 golden in a font small enough to fit.
[[nodiscard]] std::array<Golden, 4> goldens()
{
    using vtbackend::ColumnCount;
    using vtbackend::LineCount;
    return {
        Golden { .file = "lists-50x20.screen"sv,
                 .fontSize = std::nullopt,
                 .page = vtbackend::PageSize { LineCount(20), ColumnCount(50) },
                 .corners = 4 },
        Golden { .file = "dispatch-pr-120x40.screen"sv,
                 .fontSize = 7.0,
                 .page = vtbackend::PageSize { LineCount(40), ColumnCount(120) },
                 .corners = 12 },
        Golden { .file = "board-120x40.screen"sv,
                 .fontSize = 7.0,
                 .page = vtbackend::PageSize { LineCount(40), ColumnCount(120) },
                 .corners = 4 },
        Golden { .file = "transcript-120x40.screen"sv,
                 .fontSize = 7.0,
                 .page = vtbackend::PageSize { LineCount(40), ColumnCount(120) },
                 .corners = 4 },
    };
}

/// Where @p golden lives.
[[nodiscard]] std::filesystem::path goldenPath(Golden const& golden)
{
    return std::filesystem::path { CONTOUR_PROJECT_SOURCE_DIR } / "extensions" / "orchestrator" / "view"
           / "testdata" / golden.file;
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

    for (auto const& golden: goldens())
    {
        INFO("golden " << golden.file);
        auto file = std::ifstream { goldenPath(golden), std::ios::binary };
        REQUIRE(file.good());
        auto const frameText = std::string { std::istreambuf_iterator<char> { file }, {} };

        for (auto const arcStyle: ArcStyles)
        {
            INFO("arc style " << static_cast<int>(arcStyle));
            auto const variant =
                Variant { .scheme = "contour-dark"sv, .style = contour::config::UiStyle::Native };
            auto const h = makeHarness(variant, [&](contour::config::Config& config) {
                config.boxArcStyle.value() = arcStyle;
                if (golden.fontSize)
                    config.profiles.value().at("main").fonts.value().size.pt = *golden.fontSize;
            });
            auto const page = h->session->terminal().pageSize();
            REQUIRE(page.columns >= golden.page.columns);
            REQUIRE(page.lines >= golden.page.lines);

            h->feedAndSettle(framePainting(frameText));
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

                    for (auto const& [edge, line]: std::array { std::pair { corner->towardsHorizontal, U'─' },
                                                                std::pair { corner->towardsVertical, U'│' } })
                    {
                        auto const across = neighbour(at, edge);
                        INFO(std::format("corner U+{:04X} at row {} column {}, towards row {} column {}",
                                         static_cast<uint32_t>(corner->codepoint),
                                         at.row,
                                         at.column,
                                         across.row,
                                         across.column));
                        // The golden draws boxes: every corner continues into the line it meets.
                        REQUIRE(codepointAt(text, across) == line);

                        auto const seam = seamAt(frame.image, g, at, edge);
                        INFO("corner side: " << hexRow(seam.inside));
                        INFO("line side:   " << hexRow(seam.outside));

                        // The stroke reaches the side: a seam of nothing but page would meet trivially.
                        CHECK(std::ranges::any_of(seam.inside,
                                                  [&](auto c) { return !nearlyEqual(c, ground); }));

                        // Pixel for pixel, the arc's end lies over the line's end: no step, no notch, no
                        // fringe.
                        CHECK(std::ranges::equal(
                            seam.inside, seam.outside, [](auto a, auto b) { return nearlyEqual(a, b, 2); }));
                    }
                }
            // ╭ ╮ ╯ ╰ of every panel, and of the popup over them.
            CHECK(cornersSeen == golden.corners);
        }
    }
}
