// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/SoftGlyphRef.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <ranges>

using namespace vtbackend;
using namespace vtbackend::test;

namespace
{

constexpr auto TestRef = SoftGlyphRef { .font = 1, .position = 0x41 };

/// Lists the columns of line 0 that carry a soft glyph reference.
std::vector<int> softGlyphColumns(Screen& screen, int columns)
{
    auto result = std::vector<int> {};
    for (auto const column: std::views::iota(0, columns))
        if (screen.at(LineOffset(0), ColumnOffset(column)).softGlyph())
            result.push_back(column);
    return result;
}

constexpr auto TestPage = PageSize { LineCount(2), ColumnCount(10) };

} // namespace

TEST_CASE("SoftGlyph.ICH and DCH shift soft glyphs with their cells", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH");
    screen.at(LineOffset(0), ColumnOffset(3)).setSoftGlyph(TestRef);

    mock.writeToScreen("\033[1;1H\033[2@"); // ICH 2 at column 0
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 5 });
    CHECK(screen.at(LineOffset(0), ColumnOffset(5)).softGlyph() == TestRef);

    mock.writeToScreen("\033[1;1H\033[2P"); // DCH 2
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 3 });
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).softGlyph() == TestRef);
}

TEST_CASE("SoftGlyph.DCH drops the glyph of a deleted cell", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH");
    screen.at(LineOffset(0), ColumnOffset(1)).setSoftGlyph(TestRef);
    screen.at(LineOffset(0), ColumnOffset(7)).setSoftGlyph(TestRef);

    mock.writeToScreen("\033[1;1H\033[2P"); // deletes columns 0 and 1
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 5 });
}

TEST_CASE("SoftGlyph.DECIC and DECDC shift soft glyphs", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH\r\nabcdefgh");
    screen.at(LineOffset(0), ColumnOffset(3)).setSoftGlyph(TestRef);

    mock.writeToScreen("\033[1;1H\033[2'}"); // DECIC 2
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 5 });

    mock.writeToScreen("\033[1;1H\033[2'~"); // DECDC 2
    CHECK(softGlyphColumns(screen, 10) == std::vector<int> { 3 });
}

TEST_CASE("SoftGlyph.ICH shifts image fragments too", "[screen][SoftGlyph]")
{
    auto mock = MockTerm { TestPage };
    auto& screen = mock.terminal.primaryScreen();
    mock.writeToScreen("ABCDEFGH");

    auto const image = std::make_shared<Image>(
        ImageId(1), ImageFormat::RGBA, Image::Data(4, 0xFF), ImageSize { Width(1), Height(1) }, [](auto) {});
    auto const rasterized = std::make_shared<RasterizedImage>(image,
                                                              ImageAlignment::TopStart,
                                                              ImageResize::NoResize,
                                                              RGBAColor { 0, 0, 0, 0xFF },
                                                              GridSize { LineCount(1), ColumnCount(1) },
                                                              ImageSize { Width(1), Height(1) });
    screen.at(LineOffset(0), ColumnOffset(3)).setImageFragment(rasterized, CellLocation {});

    mock.writeToScreen("\033[1;1H\033[2@");
    CHECK(!screen.at(LineOffset(0), ColumnOffset(3)).imageFragment());
    CHECK(screen.at(LineOffset(0), ColumnOffset(5)).imageFragment());

    mock.writeToScreen("\033[1;1H\033[2P");
    CHECK(!screen.at(LineOffset(0), ColumnOffset(5)).imageFragment());
    CHECK(screen.at(LineOffset(0), ColumnOffset(3)).imageFragment());
}
