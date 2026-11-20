// SPDX-License-Identifier: Apache-2.0
//
// Kitty Unicode placeholders on a screen: placeholder cells resolved to image fragments of a virtual
// placement at the end of each parse batch, whatever order the cells and the image arrive in.

#include <vtbackend/graphics/KittyPlaceholders.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

using namespace std::string_view_literals;
using namespace vtbackend;
using namespace vtbackend::kitty_graphics;

namespace
{
/// kitty's first row/column diacritics, in order: index n encodes n + 1.
constexpr auto Diacritics = std::array { U'\u0305', U'\u030D', U'\u030E', U'\u0310', U'\u0312' };

/// The terminal these cases run on: 1x1 pixel cells, so an image's pixels are its cells.
class PlaceholderTerm: public MockTerm<vtpty::MockPty>
{
  public:
    explicit PlaceholderTerm(PageSize size): MockTerm { size }
    {
        terminal.setCellPixelSize(ImageSize { Width(1), Height(1) });
    }

    [[nodiscard]] Screen& screen() { return terminal.primaryScreen(); }
};

/// @return @p text in UTF-8.
[[nodiscard]] std::string utf8(std::u32string const& text)
{
    return unicode::convert_to<char>(std::u32string_view { text });
}

/// @return A placeholder cell of image row @p row and column @p column, as Claude Code writes it.
[[nodiscard]] std::u32string placeholderCell(size_t row, size_t column)
{
    return std::u32string { PlaceholderCodepoint, Diacritics.at(row), Diacritics.at(column) };
}

/// @return Image row @p row, @p columns cells wide, in Claude Code's exact bytes (id 7 by palette index).
[[nodiscard]] std::string placeholderRow(size_t row, size_t columns)
{
    auto text = std::u32string { U"\033[38;5;7m" };
    for (auto const column: std::views::iota(size_t { 0 }, columns))
        text += placeholderCell(row, column);
    text += U"\033[m\r\n";
    return utf8(text);
}

/// @return A 2x4 picture's placeholder rows, as Claude Code prints them.
[[nodiscard]] std::string placeholderPicture()
{
    return placeholderRow(0, 4) + placeholderRow(1, 4);
}

/// @return The transmission of a @p width x @p height RGB image as id 7 with a 2x4 virtual placement.
[[nodiscard]] std::string transmitVirtual(int width = 2, int height = 2)
{
    auto const pixels = std::string(static_cast<size_t>(width * height * 3), '\x7F');
    return std::format(
        "\033_Ga=T,U=1,i=7,c=4,r=2,f=24,s={},v={},q=2;{}\033\\", width, height, core::base64::encode(pixels));
}

/// Checks that the 2x4 picture at the page's top-left shows image offsets (row, column) in every cell.
void checkPictureResolved(Screen const& screen)
{
    for (auto const row: std::views::iota(0, 2))
        for (auto const column: std::views::iota(0, 4))
        {
            INFO(std::format("cell ({}, {})", row, column));
            auto const fragment = screen.at(LineOffset(row), ColumnOffset(column)).imageFragment();
            REQUIRE(fragment);
            CHECK(fragment->offset()
                  == CellLocation { .line = LineOffset(row), .column = ColumnOffset(column) });
            CHECK(fragment->rasterizedImage().origin() == ImageOrigin::Placeholder);
        }
}
} // namespace

TEST_CASE("KittyPlaceholders.screen.picture", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    checkPictureResolved(mock.screen());
    CHECK(mock.screen().grid().lineAt(LineOffset(0)).isFlagEnabled(LineFlag::ImagePlaceholders));
    CHECK(mock.screen().grid().lineAt(LineOffset(1)).isFlagEnabled(LineFlag::ImagePlaceholders));
    CHECK_FALSE(mock.screen().grid().lineAt(LineOffset(2)).isFlagEnabled(LineFlag::ImagePlaceholders));
    // Every cell shares the one raster the virtual placement made.
    auto const* const placement = mock.screen().findKittyVirtualPlacement(7, KittyPlacementId {});
    REQUIRE(placement != nullptr);
    CHECK(&mock.screen().at(LineOffset(1), ColumnOffset(3)).imageFragment()->rasterizedImage()
          == placement->raster.get());
}

TEST_CASE("KittyPlaceholders.screen.split_write", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());

    // The base code point in one read and its diacritics in the next: alone, the base reads as the image's
    // first cell; with its diacritics it is row 1, column 2.
    mock.writeToScreen(utf8(U"\033[38;5;7m\U0010EEEE"));
    mock.writeToScreen(utf8(std::u32string { Diacritics[1], Diacritics[2] }));

    auto const fragment = mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment);
    CHECK(fragment->offset() == CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) });
}

TEST_CASE("KittyPlaceholders.screen.cells_before_image", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(placeholderPicture());
    CHECK_FALSE(mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
    // Unresolved, the lines stay marked for the image to come.
    CHECK(mock.screen().grid().lineAt(LineOffset(0)).isFlagEnabled(LineFlag::ImagePlaceholders));

    mock.writeToScreen(transmitVirtual());
    checkPictureResolved(mock.screen());
}

TEST_CASE("KittyPlaceholders.screen.text_overwrite", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    mock.writeToScreen("\033[1;2Hx"sv);
    CHECK_FALSE(mock.screen().at(LineOffset(0), ColumnOffset(1)).imageFragment());
    CHECK(mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
    CHECK(mock.screen().at(LineOffset(0), ColumnOffset(2)).imageFragment());
}

TEST_CASE("KittyPlaceholders.screen.reprinting_rasterizes_nothing", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    auto const before = ImageStats::get().rasterized;
    for ([[maybe_unused]] auto const _: std::views::iota(0, 200))
        mock.writeToScreen("\033[H" + placeholderRow(0, 4));
    CHECK(ImageStats::get().rasterized <= before + 1);
    auto const fragment = mock.screen().at(LineOffset(0), ColumnOffset(3)).imageFragment();
    REQUIRE(fragment);
    CHECK(fragment->offset() == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
}

TEST_CASE("KittyPlaceholders.screen.retransmission", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    mock.writeToScreen(transmitVirtual(4, 4));

    checkPictureResolved(mock.screen());
    auto const fragment = mock.screen().at(LineOffset(1), ColumnOffset(2)).imageFragment();
    REQUIRE(fragment);
    CHECK(fragment->rasterizedImage().image().width() == Width(4));
    CHECK(fragment->rasterizedImage().image().height() == Height(4));
}

TEST_CASE("KittyPlaceholders.screen.resize", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    mock.terminal.resizeScreenKeepingCellSize(PageSize { LineCount(10), ColumnCount(6) });
    checkPictureResolved(mock.screen());
    mock.terminal.resizeScreenKeepingCellSize(PageSize { LineCount(10), ColumnCount(20) });
    checkPictureResolved(mock.screen());
}

TEST_CASE("KittyPlaceholders.screen.resize_wraps_a_row", "[kitty][placeholder]")
{
    // Narrowed below the picture's width, the second half of each row moves to a continuation row of its
    // own, and keeps showing its part of the picture.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(4) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen("\033[38;5;7m"sv);
    mock.writeToScreen(
        utf8(placeholderCell(0, 0) + placeholderCell(0, 1) + placeholderCell(0, 2) + placeholderCell(0, 3)));
    mock.writeToScreen("\033[m"sv);

    mock.terminal.resizeScreenKeepingCellSize(PageSize { LineCount(10), ColumnCount(2) });

    auto const& screen = mock.screen();
    auto const wrapped = screen.at(LineOffset(1), ColumnOffset(1)).imageFragment();
    REQUIRE(wrapped);
    CHECK(wrapped->offset() == CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    CHECK(screen.grid().lineAt(LineOffset(1)).isFlagEnabled(LineFlag::ImagePlaceholders));
}

TEST_CASE("KittyPlaceholders.screen.width", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(utf8(placeholderCell(2, 3)));
    CHECK(mock.screen().cursor().position.column == ColumnOffset(1));
    CHECK(mock.screen().at(LineOffset(0), ColumnOffset(0)).width() == 1);
}

TEST_CASE("KittyPlaceholders.screen.out_of_span", "[kitty][placeholder]")
{
    // The placement is 2x4; a cell naming column 5 lies outside it and shows nothing.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderRow(0, 5));
    CHECK(mock.screen().at(LineOffset(0), ColumnOffset(3)).imageFragment());
    CHECK_FALSE(mock.screen().at(LineOffset(0), ColumnOffset(4)).imageFragment());
}

TEST_CASE("KittyPlaceholders.screen.unknown_image", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen("\033[38;5;8m"sv);
    mock.writeToScreen(utf8(placeholderCell(0, 0)));
    CHECK_FALSE(mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyPlaceholders.screen.erase_clears_the_flag", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());
    mock.writeToScreen("\033[1;1H\033[2K"sv);
    CHECK_FALSE(mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
    CHECK_FALSE(mock.screen().grid().lineAt(LineOffset(0)).isFlagEnabled(LineFlag::ImagePlaceholders));
}
