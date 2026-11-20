// SPDX-License-Identifier: Apache-2.0
//
// Kitty Unicode placeholders on a screen: placeholder cells resolved to image fragments of a virtual
// placement at the end of each parse batch, whatever order the cells and the image arrive in.

#include <vtbackend/graphics/KittyPlaceholders.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <format>
#include <optional>
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
    explicit PlaceholderTerm(PageSize size, LineCount history = LineCount(0)): MockTerm { size, history }
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

/// Checks that the 2x4 picture at @p topLeft shows image offsets (row, column) in every cell.
void checkPictureResolved(Screen const& screen, CellLocation topLeft = {})
{
    for (auto const row: std::views::iota(0, 2))
        for (auto const column: std::views::iota(0, 4))
        {
            INFO(std::format("cell ({}, {})", row, column));
            auto const fragment =
                screen.at(topLeft.line + LineOffset(row), topLeft.column + ColumnOffset(column))
                    .imageFragment();
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

TEST_CASE("KittyPlaceholders.copy_area.shows_the_picture", "[kitty][placeholder]")
{
    // DECCRA copies the placeholder cells: the copy names the same virtual placement, so it shows the
    // picture too, as reprinting the cells there would.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    mock.writeToScreen("[1;1;2;4;1;5;3;1$v"sv); // DECCRA: rows 1-2, columns 1-4 to row 5, column 3

    checkPictureResolved(mock.screen());
    checkPictureResolved(mock.screen(), CellLocation { .line = LineOffset(4), .column = ColumnOffset(2) });
    CHECK(mock.screen().grid().lineAt(LineOffset(4)).isFlagEnabled(LineFlag::ImagePlaceholders));
}

TEST_CASE("KittyPlaceholders.copy_area.overlapping", "[kitty][placeholder]")
{
    // Copied one row down onto itself: the bottom row of the copy lands on the source's own second row.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    mock.writeToScreen("[1;1;2;4;1;2;1;1$v"sv);

    checkPictureResolved(mock.screen(), CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });
}

TEST_CASE("KittyPlaceholders.copy_area.over_a_picture", "[kitty][placeholder]")
{
    // Blank cells copied over the picture's first row take its tiles with them, and the row holds no
    // placeholder any more.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    mock.writeToScreen("[6;1;6;20;1;1;1;1$v"sv);

    for (auto const column: std::views::iota(0, 4))
        CHECK_FALSE(mock.screen().at(LineOffset(0), ColumnOffset(column)).imageFragment());
    CHECK_FALSE(mock.screen().grid().lineAt(LineOffset(0)).isFlagEnabled(LineFlag::ImagePlaceholders));
    CHECK(mock.screen().at(LineOffset(1), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyPlaceholders.copy_area.to_another_page", "[kitty][placeholder]")
{
    // Virtual placements belong to a page: the copy shows the picture of the target page's own placement.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());
    mock.writeToScreen("[1U"sv); // NP: page 2
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen("[1V"sv); // PP: back to page 1

    mock.writeToScreen("[1;1;2;4;1;3;1;2$v"sv); // DECCRA: page 1's picture to page 2, row 3

    checkPictureResolved(mock.terminal.pageAt(PageIndex(1)),
                         CellLocation { .line = LineOffset(2), .column = ColumnOffset(0) });
}

TEST_CASE("KittyPlaceholders.screen.scrolled_within_left_and_right_margins", "[kitty][placeholder]")
{
    // With left and right margins a scroll moves the cells between the margins, not whole rows: the rows the
    // placeholder cells land on must resolve again when their placement changes, as the rows they left did.
    struct Scroll
    {
        std::string_view name;
        int from;               ///< The row the picture is printed on.
        std::string_view moves; ///< The scroll, issued with the cursor homed inside the margins.
        int to;                 ///< The row the picture lands on.
    };
    auto const scroll = GENERATE(Scroll { .name = "SU", .from = 2, .moves = "\033[2S", .to = 0 },
                                 Scroll { .name = "SD", .from = 0, .moves = "\033[2T", .to = 2 },
                                 Scroll { .name = "IL", .from = 0, .moves = "\033[2L", .to = 2 },
                                 Scroll { .name = "DL", .from = 2, .moves = "\033[2M", .to = 0 });
    INFO(scroll.name);

    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(std::format("\033[{};1H", scroll.from + 1) + placeholderPicture());
    mock.writeToScreen("\033[?69h\033[1;10s\033[H"sv); // DECLRMM, DECSLRM 1;10, cursor home
    mock.writeToScreen(scroll.moves);
    auto const topLeft = CellLocation { .line = LineOffset(scroll.to) };
    checkPictureResolved(mock.screen(), topLeft);

    // A re-transmission replaces the placement every placeholder cell shows, wherever it went.
    mock.writeToScreen(transmitVirtual(4, 4));
    checkPictureResolved(mock.screen(), topLeft);
    for (auto const row: std::views::iota(0, 2))
    {
        auto const fragment =
            mock.screen().at(topLeft.line + LineOffset(row), ColumnOffset(0)).imageFragment();
        REQUIRE(fragment);
        CHECK(fragment->rasterizedImage().image().width() == Width(4));
    }
}

TEST_CASE("KittyPlaceholders.screen.cells_resume_a_held_animation", "[kitty][placeholder]")
{
    // A playing animation no cell shows holds rather than wake the display; placeholder cells that come to
    // show it play it on.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033_Ga=T,U=1,i=7,c=1,r=1,f=24,s=1,v=1,q=2;/wAA\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,r=1,z=10,q=2\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,X=1,s=1,v=1,z=10,q=2;AP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,s=3,q=2\033\\"sv);
    REQUIRE(mock.terminal.kittyAnimationDue().has_value());

    mock.steadyClock.advance(std::chrono::milliseconds(10));
    mock.terminal.tick(mock.steadyClock.now());
    REQUIRE_FALSE(mock.terminal.kittyAnimationDue().has_value());

    mock.writeToScreen(utf8(U"\033[38;5;7m" + placeholderCell(0, 0)));
    REQUIRE(mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
    CHECK(mock.terminal.kittyAnimationDue().has_value());
}

TEST_CASE("KittyPlaceholders.screen.status_line", "[kitty][placeholder]")
{
    // The host-writable status line is a screen of its own, outside the pages, and resolves as they do.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setStatusDisplay(StatusDisplayType::HostWritable);
    mock.writeToScreen("\033[1$}" + transmitVirtual() + utf8(U"\033[38;5;7m" + placeholderCell(1, 2))
                       + "\033[0$}");
    auto const fragment =
        mock.terminal.hostWritableStatusLineDisplay().at(LineOffset(0), ColumnOffset(0)).imageFragment();
    REQUIRE(fragment);
    CHECK(fragment->offset() == CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) });
}

TEST_CASE("KittyPlaceholders.screen.trace_mode", "[kitty][placeholder]")
{
    // In trace mode the parse only queues what it read; the cells are written when the queue is flushed,
    // outside the parse batch, and resolve then.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.terminal.setExecutionMode(ExecutionMode::SingleStep);
    mock.writeToScreen(placeholderPicture());
    mock.terminal.setExecutionMode(ExecutionMode::Normal);
    mock.terminal.processInputOnce();
    checkPictureResolved(mock.screen());
}

TEST_CASE("KittyPlaceholders.screen.cells_shifted_or_erased_within_a_row", "[kitty][placeholder]")
{
    // A cell without a column diacritic continues its left neighbour, so shifting cells within a row, or
    // erasing or overwriting one in the middle of a run, changes what the cells after it show: the row is
    // resolved again, and the fragments the cells carried along are not what they show any more.
    struct Change
    {
        std::string_view name;
        std::string_view sequence;
        std::array<std::optional<int>, 5> shown; ///< The image column each of columns 0..4 shows, if any.
    };
    constexpr auto None = std::optional<int> {};
    auto const change = GENERATE_COPY(
        Change { .name = "ICH", .sequence = "\033[1;3H\033[@", .shown = { 0, 1, None, 0, 1 } },
        Change { .name = "DECIC", .sequence = "\033[1;3H\033['}", .shown = { 0, 1, None, 0, 1 } },
        Change { .name = "IRM", .sequence = "\033[4h\033[1;3Hx\033[4l", .shown = { 0, 1, None, 0, 1 } },
        Change { .name = "DCH", .sequence = "\033[1;2H\033[P", .shown = { 0, 1, 2, None, None } },
        Change { .name = "DECDC", .sequence = "\033[1;2H\033['~", .shown = { 0, 1, 2, None, None } },
        Change { .name = "SL", .sequence = "\033[ @", .shown = { 0, 1, 2, None, None } },
        Change { .name = "SR", .sequence = "\033[?69h\033[2;20s\033[ A", .shown = { 0, None, 0, 1, 2 } },
        Change { .name = "ECH", .sequence = "\033[1;2H\033[X", .shown = { 0, None, 0, 1, None } },
        Change { .name = "text", .sequence = "\033[1;2Hx", .shown = { 0, None, 0, 1, None } },
        Change { .name = "EL1", .sequence = "\033[1;2H\033[1K", .shown = { None, None, 0, 1, None } },
        Change { .name = "DECERA", .sequence = "\033[1;2;1;2$z", .shown = { 0, None, 0, 1, None } },
        Change { .name = "DECSERA", .sequence = "\033[1;2;1;2${", .shown = { 0, None, 0, 1, None } },
        Change { .name = "DECFRA", .sequence = "\033[120;1;2;1;2$x", .shown = { 0, None, 0, 1, None } });
    INFO(change.name);

    // Image row 0: the first cell names its column, the three after it infer theirs.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(
        utf8(U"\033[38;5;7m" + placeholderCell(0, 0) + U"\U0010EEEE\U0010EEEE\U0010EEEE\033[m"));
    mock.writeToScreen(change.sequence);

    for (auto const column: std::views::iota(size_t { 0 }, change.shown.size()))
    {
        auto const expected = change.shown.at(column);
        INFO(std::format("column {}", column));
        auto const fragment =
            mock.screen().at(LineOffset(0), ColumnOffset::cast_from(column)).imageFragment();
        if (!expected)
        {
            CHECK_FALSE(fragment);
            continue;
        }
        REQUIRE(fragment);
        CHECK(fragment->offset()
              == CellLocation { .line = LineOffset(0), .column = ColumnOffset(*expected) });
    }
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

namespace
{
/// @return The transmission of a 2x2 RGB image under @p key (`i=<id>` or `I=<number>`) with a 2x4 virtual
///         placement, answering nothing.
[[nodiscard]] std::string transmitVirtualAs(std::string_view key)
{
    return std::format("\033_Ga=T,U=1,{},c=4,r=2,f=24,s=2,v=2,q=2;{}\033\\",
                       key,
                       core::base64::encode(std::string(12, '\x7F')));
}

/// A picture of image 7 on rows 0 and 1, and a cursor placement of image 9 at row 5.
void showPictureAndCursorPlacement(PlaceholderTerm& mock)
{
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());
    mock.writeToScreen(std::format("\033[6;1H\033_Ga=T,i=9,f=24,s=2,v=2,q=2;{}\033\\",
                                   core::base64::encode(std::string(12, '\x7F'))));
    REQUIRE(mock.screen().at(LineOffset(5), ColumnOffset(0)).imageFragment());
    checkPictureResolved(mock.screen());
}
} // namespace

TEST_CASE("KittyPlaceholders.delete.all_spares_virtual_placements", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    showPictureAndCursorPlacement(mock);

    SECTION("d=a")
    {
        mock.writeToScreen("\033_Ga=d,d=a\033\\"sv);
        CHECK_FALSE(mock.screen().at(LineOffset(5), ColumnOffset(0)).imageFragment());
        checkPictureResolved(mock.screen());
        CHECK(mock.screen().findKittyVirtualPlacement(7, KittyPlacementId {}) != nullptr);
        CHECK(mock.screen().kittyImage(9) != nullptr);
    }

    SECTION("a bare a=d deletes as d=a")
    {
        mock.writeToScreen("\033_Ga=d\033\\"sv);
        CHECK_FALSE(mock.screen().at(LineOffset(5), ColumnOffset(0)).imageFragment());
        checkPictureResolved(mock.screen());
        CHECK(mock.screen().findKittyVirtualPlacement(7, KittyPlacementId {}) != nullptr);
        CHECK(mock.screen().kittyImage(9) != nullptr);
    }

    SECTION("d=A frees no image a virtual placement still shows")
    {
        mock.writeToScreen("\033_Ga=d,d=A\033\\"sv);
        CHECK_FALSE(mock.screen().at(LineOffset(5), ColumnOffset(0)).imageFragment());
        checkPictureResolved(mock.screen());
        CHECK(mock.screen().kittyImage(7) != nullptr);
        CHECK(mock.screen().kittyImage(9) == nullptr);
    }
}

TEST_CASE("KittyPlaceholders.delete.by_id", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    showPictureAndCursorPlacement(mock);
    auto const textBefore = mock.screen().grid().lineText(LineOffset(0));

    SECTION("d=i keeps the image")
    {
        mock.writeToScreen("\033_Ga=d,d=i,i=7\033\\"sv);
        CHECK(mock.screen().findKittyVirtualPlacement(7, KittyPlacementId {}) == nullptr);
        CHECK(mock.screen().kittyImage(7) != nullptr);
    }

    SECTION("d=I frees it")
    {
        mock.writeToScreen("\033_Ga=d,d=I,i=7\033\\"sv);
        CHECK(mock.screen().findKittyVirtualPlacement(7, KittyPlacementId {}) == nullptr);
        CHECK(mock.screen().kittyImage(7) == nullptr);
    }

    // The placeholder cells keep their text; only the picture goes.
    for (auto const row: std::views::iota(0, 2))
        for (auto const column: std::views::iota(0, 4))
            CHECK_FALSE(mock.screen().at(LineOffset(row), ColumnOffset(column)).imageFragment());
    CHECK(mock.screen().grid().lineText(LineOffset(0)) == textBefore);
    // Image 9 is no business of the delete.
    CHECK(mock.screen().at(LineOffset(5), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyPlaceholders.delete.one_placement_keeps_the_image", "[kitty][placeholder]")
{
    // d=I with p= frees the image only once no placement of it remains, the virtual ones included.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());
    mock.writeToScreen("\033_Ga=p,U=1,i=7,p=3,c=1,r=1,q=2\033\\"sv);

    mock.writeToScreen("\033_Ga=d,d=I,i=7,p=3\033\\"sv);

    CHECK(mock.screen().findKittyVirtualPlacement(7, KittyPlacementId { 3 }) == nullptr);
    CHECK(mock.screen().findKittyVirtualPlacement(7, KittyPlacementId {}) != nullptr);
    CHECK(mock.screen().kittyImage(7) != nullptr);
    checkPictureResolved(mock.screen());
}

TEST_CASE("KittyPlaceholders.delete.by_number", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    // Two transmissions under number 3: the terminal picks ids 1 and 2, and number 3 names the newer.
    mock.writeToScreen(transmitVirtualAs("I=3"));
    mock.writeToScreen(transmitVirtualAs("I=3"));
    REQUIRE(mock.screen().findKittyVirtualPlacement(1, KittyPlacementId {}) != nullptr);
    REQUIRE(mock.screen().findKittyVirtualPlacement(2, KittyPlacementId {}) != nullptr);

    SECTION("d=n")
    {
        mock.writeToScreen("\033_Ga=d,d=n,I=3\033\\"sv);
        CHECK(mock.screen().findKittyVirtualPlacement(2, KittyPlacementId {}) == nullptr);
        CHECK(mock.screen().kittyImage(2) != nullptr);
        CHECK(mock.screen().findKittyVirtualPlacement(1, KittyPlacementId {}) != nullptr);
    }

    SECTION("d=N")
    {
        mock.writeToScreen("\033_Ga=d,d=N,I=3\033\\"sv);
        CHECK(mock.screen().kittyImage(2) == nullptr);
        CHECK(mock.screen().kittyImage(1) != nullptr);
    }
}

TEST_CASE("KittyPlaceholders.delete.by_number_falls_back_to_the_older_image", "[kitty][placeholder]")
{
    // Freeing the newest image of a number leaves the number naming the one before it, as kitty's
    // img_by_client_number() finds it.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtualAs("I=3"));
    mock.writeToScreen(transmitVirtualAs("I=3"));
    mock.writeToScreen("\033_Ga=d,d=N,I=3\033\\"sv);
    mock.terminal.flushInput();

    mock.writeToScreen("\033_Ga=p,I=3,U=1,c=1,r=1\033\\"sv);
    CHECK(mock.terminal.peekInput() == "\033_Gi=1,I=3;OK\033\\");
}

TEST_CASE("KittyPlaceholders.delete.by_id_range", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    for (auto const id: { 4, 5, 9, 10 })
        mock.writeToScreen(transmitVirtualAs(std::format("i={}", id)));

    SECTION("d=r")
    {
        mock.writeToScreen("\033_Ga=d,d=r,x=5,y=9\033\\"sv);
        CHECK(mock.screen().findKittyVirtualPlacement(4, KittyPlacementId {}) != nullptr);
        CHECK(mock.screen().findKittyVirtualPlacement(5, KittyPlacementId {}) == nullptr);
        CHECK(mock.screen().findKittyVirtualPlacement(9, KittyPlacementId {}) == nullptr);
        CHECK(mock.screen().findKittyVirtualPlacement(10, KittyPlacementId {}) != nullptr);
        CHECK(mock.screen().kittyImage(5) != nullptr);
        CHECK(mock.screen().kittyImage(9) != nullptr);
    }

    SECTION("d=R")
    {
        mock.writeToScreen("\033_Ga=d,d=R,x=5,y=9\033\\"sv);
        CHECK(mock.screen().kittyImage(4) != nullptr);
        CHECK(mock.screen().kittyImage(5) == nullptr);
        CHECK(mock.screen().kittyImage(9) == nullptr);
        CHECK(mock.screen().kittyImage(10) != nullptr);
    }
}

TEST_CASE("KittyPlaceholders.screen.rows_scrolled_into_history_in_the_same_batch", "[kitty][placeholder]")
{
    // The picture and the lines that push it off the page arrive in one read: its rows are in the history
    // by the time the batch ends, and are resolved there.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(100) };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture() + std::string(9, '\n'));

    REQUIRE(mock.screen().historyLineCount() == LineCount(2));
    for (auto const row: std::views::iota(0, 2))
        for (auto const column: std::views::iota(0, 4))
        {
            INFO(std::format("cell ({}, {})", row, column));
            auto const fragment = mock.screen().at(LineOffset(row - 2), ColumnOffset(column)).imageFragment();
            REQUIRE(fragment);
            CHECK(fragment->offset()
                  == CellLocation { .line = LineOffset(row), .column = ColumnOffset(column) });
        }
}

TEST_CASE("KittyPlaceholders.screen.pending_resolve_walks_only_what_scrolled_since", "[kitty][placeholder]")
{
    // A batch that writes placeholder cells walks the page and the rows it scrolled off, not the history the
    // earlier batches left behind: the per-batch cost stays independent of the scrollback's length.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) }, LineCount(100) };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture() + std::string(40, '\n'));
    REQUIRE(mock.screen().historyLineCount() == LineCount(33));

    SECTION("a later batch at the bottom walks the page and the five lines it scrolled")
    {
        // The cursor sits on the last line: each of the picture's two rows ends in a scroll, then three more.
        mock.writeToScreen(placeholderPicture() + std::string(3, '\n'));
        CHECK(mock.screen().kittyPlaceholderLinesWalked() == 10 + 5);
    }

    SECTION("a change that can resolve old cells walks the whole history")
    {
        mock.writeToScreen(transmitVirtual());
        CHECK(mock.screen().kittyPlaceholderLinesWalked() == 10 + 33);
    }
}

TEST_CASE("KittyPlaceholders.screen.animation_frame_keeps_placeholder_origin", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());

    // A second frame, made current.
    mock.writeToScreen(std::format("\033_Ga=f,i=7,f=24,s=2,v=2,q=2;{}\033\\",
                                   core::base64::encode(std::string(12, '\x20'))));
    mock.writeToScreen("\033_Ga=a,i=7,c=2,q=2\033\\"sv);
    auto const frame2 = mock.screen().kittyFrameImage(7, 2);
    REQUIRE(frame2);

    auto const checkShowsFrame2 = [&]() {
        checkPictureResolved(mock.screen());
        CHECK(
            mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment()->rasterizedImage().imagePointer()
            == frame2);
    };
    checkShowsFrame2();

    SECTION("reprinting keeps the frame")
    {
        mock.writeToScreen("\033[H" + placeholderPicture());
        checkShowsFrame2();
    }

    SECTION("d=a keeps the cells")
    {
        mock.writeToScreen("\033_Ga=d,d=a,q=2\033\\"sv);
        checkShowsFrame2();
    }
}

TEST_CASE("KittyPlaceholders.screen.cursor_placement_replacing_a_virtual_one", "[kitty][placeholder]")
{
    // Cells without a placement id show the image's first virtual placement; when a cursor placement takes
    // that pair over, they show the next one.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(std::format("\033_Ga=t,i=7,f=24,s=2,v=2,q=2;{}\033\\",
                                   core::base64::encode(std::string(12, '\x7F'))));
    mock.writeToScreen("\033_Ga=p,U=1,i=7,p=3,c=4,r=2,q=2\033\\"sv);
    mock.writeToScreen("\033_Ga=p,U=1,i=7,p=5,c=4,r=2,q=2\033\\"sv);
    mock.writeToScreen(placeholderPicture());
    REQUIRE(
        mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment()->rasterizedImage().kittyPlacementId()
        == KittyPlacementId { 3 });

    mock.writeToScreen("\033[9;1H\033_Ga=p,i=7,p=3,q=2\033\\"sv);

    checkPictureResolved(mock.screen());
    CHECK(
        mock.screen().at(LineOffset(0), ColumnOffset(0)).imageFragment()->rasterizedImage().kittyPlacementId()
        == KittyPlacementId { 5 });
}

TEST_CASE("KittyPlaceholders.screen.other_pages", "[kitty][placeholder]")
{
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen("\033[1U"sv); // NP: page 2
    mock.writeToScreen(transmitVirtual());
    mock.writeToScreen(placeholderPicture());
    checkPictureResolved(mock.terminal.pageAt(PageIndex(1)));
}

TEST_CASE("KittyPlaceholders.screen.source_rect_is_ignored", "[kitty][placeholder]")
{
    // kitty's grman_put_cell_image fits the whole image into a virtual placement, whatever x/y/w/h say.
    auto mock = PlaceholderTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.writeToScreen(std::format("\033_Ga=T,U=1,i=7,x=1,y=1,w=1,h=1,f=24,s=2,v=2,q=2;{}\033\\",
                                   core::base64::encode(std::string(12, '\x7F'))));
    auto const* const placement = mock.screen().findKittyVirtualPlacement(7, KittyPlacementId {});
    REQUIRE(placement != nullptr);
    CHECK(placement->raster->imageOffset().x.value == 0);
    CHECK(placement->raster->imageOffset().y.value == 0);
    CHECK(placement->raster->imageSubSize() == ImageSize {});
    CHECK(placement->raster->cellSpan().lines == LineCount(2));
    CHECK(placement->raster->cellSpan().columns == ColumnCount(2));
}
