// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Line.h>
#include <vtbackend/cell/CellConfig.h>

#include <crispy/escape.h>

#include <catch2/catch_test_macros.hpp>

using namespace std;

using namespace vtbackend;
using namespace crispy;

// Default cell type for testing.
using Cell = PrimaryScreenCell;

TEST_CASE("Line.BufferFragment", "[Line]")
{
    auto constexpr TestText = "0123456789ABCDEF"sv;
    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(TestText);
    auto const bufferFragment = bufferObject->ref(0, 10);

    auto const externalView = string_view(bufferObject->data(), 10);
    auto const fragment = buffer_fragment(bufferObject, externalView);
    CHECK(fragment.view() == externalView);
}

TEST_CASE("Line.resize", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(4);
    auto text = "abcd"sv;
    auto pool = buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(text);

    auto const bufferFragment = bufferObject->ref(0, 4);

    auto const sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = DisplayWidth,
                                             .text = bufferFragment };
    CHECK(trivial.text.view() == string_view(text.data()));
    auto lineTrivial = Line<Cell>(LineFlag::None, trivial);
    CHECK(lineTrivial.isTrivialBuffer());

    lineTrivial.resize(ColumnCount(10));
    CHECK(lineTrivial.isTrivialBuffer());

    lineTrivial.resize(ColumnCount(5));
    CHECK(lineTrivial.isTrivialBuffer());

    lineTrivial.resize(ColumnCount(3));
    CHECK(lineTrivial.isTrivialBuffer());
}

TEST_CASE("Line.reflow", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(4);
    auto text = "abcd"sv;
    auto pool = buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(text);

    auto const bufferFragment = bufferObject->ref(0, 4);

    auto const sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = DisplayWidth,
                                             .text = bufferFragment };
    CHECK(trivial.text.view() == string_view(text.data()));
    auto lineTrivial = Line<Cell>(LineFlag::None, trivial);
    CHECK(lineTrivial.isTrivialBuffer());

    (void) lineTrivial.reflow(ColumnCount(5));
    CHECK(lineTrivial.isTrivialBuffer());

    (void) lineTrivial.reflow(ColumnCount(3));
    CHECK(lineTrivial.isInflatedBuffer());
}

TEST_CASE("Line.inflate", "[Line]")
{
    auto constexpr TestText = "0123456789ABCDEF"sv;
    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(TestText);
    auto const bufferFragment = bufferObject->ref(0, 10);

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x123456);
    sgr.backgroundColor = Color::Indexed(IndexedColor::Yellow);
    sgr.underlineColor = Color::Indexed(IndexedColor::Red);
    sgr.flags |= CellFlag::CurlyUnderlined;
    auto const trivial = TrivialLineBuffer { .displayWidth = ColumnCount(10),
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = ColumnCount(10),
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == 10);
    for (size_t i = 0; i < inflated.size(); ++i)
    {
        auto const& cell = inflated[i];
        INFO(std::format("column {} codepoint {}", i, (char) cell.codepoint(0)));
        CHECK(cell.foregroundColor() == sgr.foregroundColor);
        CHECK(cell.backgroundColor() == sgr.backgroundColor);
        CHECK(cell.underlineColor() == sgr.underlineColor);
        CHECK(cell.codepointCount() == 1);
        CHECK(char(cell.codepoint(0)) == TestText[i]);
    }
}

TEST_CASE("Line.inflate.Unicode", "[Line]")
{
    auto constexpr DisplayWidth = ColumnCount(10);
    auto constexpr TestTextUtf32 = U"0\u2705123456789ABCDEF"sv;
    auto const testTextUtf8 = unicode::convert_to<char>(TestTextUtf32);

    auto pool = buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(testTextUtf8);

    // Buffer fragment containing 9 codepoints, with one of them using display width of 2.
    auto const bufferFragment = bufferObject->ref(0, 11);

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x123456);
    sgr.backgroundColor = Color::Indexed(IndexedColor::Yellow);
    sgr.underlineColor = Color::Indexed(IndexedColor::Red);
    sgr.flags |= CellFlag::CurlyUnderlined;
    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = DisplayWidth,
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));
    for (size_t i = 0, k = 0; i < inflated.size();)
    {
        auto const& cell = inflated[i];
        INFO(std::format("column {}, k {}, codepoint U+{:X}", i, k, (unsigned) cell.codepoint(0)));
        REQUIRE(cell.codepointCount() == 1);
        REQUIRE(cell.codepoint(0) == TestTextUtf32[k]);
        REQUIRE(cell.foregroundColor() == sgr.foregroundColor);
        REQUIRE(cell.backgroundColor() == sgr.backgroundColor);
        REQUIRE(cell.underlineColor() == sgr.underlineColor);
        for (int n = 1; std::cmp_less(n, cell.width()); ++n)
        {
            INFO(std::format("column.sub: {}\n", n));
            auto const& fillCell = inflated.at(i + static_cast<size_t>(n));
            REQUIRE(fillCell.codepointCount() == 0);
            REQUIRE(fillCell.foregroundColor() == sgr.foregroundColor);
            REQUIRE(fillCell.backgroundColor() == sgr.backgroundColor);
            REQUIRE(fillCell.underlineColor() == sgr.underlineColor);
        }
        i += cell.width();
        k++;
    }
}

TEST_CASE("Line.inflate.Unicode.FamilyEmoji", "[Line]")
{
    // Ensure inflate() is also working for reaaally complex Unicode grapheme clusters.

    auto constexpr DisplayWidth = ColumnCount(5);
    auto constexpr UsedColumnCount = ColumnCount(4);
    auto constexpr TestTextUtf32 = U"A\U0001F468\u200D\U0001F468\u200D\U0001F467B"sv;
    auto const testTextUtf8 = unicode::convert_to<char>(TestTextUtf32);
    auto const familyEmojiUtf8 = unicode::convert_to<char>(U"\U0001F468\u200D\U0001F468\u200D\U0001F467"sv);

    auto pool = buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(testTextUtf8);

    auto const bufferFragment = bufferObject->ref(0, testTextUtf8.size());

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x123456);
    sgr.backgroundColor = Color::Indexed(IndexedColor::Yellow);
    sgr.underlineColor = Color::Indexed(IndexedColor::Red);
    sgr.flags |= CellFlag::CurlyUnderlined;

    auto fillSGR = GraphicsAttributes {};
    fillSGR.foregroundColor = RGBColor(0x123456);
    fillSGR.backgroundColor = Color::Indexed(IndexedColor::Yellow);
    fillSGR.underlineColor = Color::Indexed(IndexedColor::Red);
    fillSGR.flags |= CellFlag::CurlyUnderlined;

    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = fillSGR,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = UsedColumnCount,
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));

    // Check text in 0..3
    // Check @4 is empty text.
    // Check 0..3 has same SGR.
    // Check @4 has fill-SGR.

    REQUIRE(inflated[0].toUtf8() == "A");
    REQUIRE(inflated[1].toUtf8() == familyEmojiUtf8);
    REQUIRE(inflated[2].toUtf8().empty());
    REQUIRE(inflated[3].toUtf8() == "B");
    REQUIRE(inflated[4].toUtf8().empty());

    for (auto const i: { 0u, 2u, 1u, 3u })
    {
        auto const& cell = inflated[i];
        REQUIRE(cell.foregroundColor() == sgr.foregroundColor);
        REQUIRE(cell.backgroundColor() == sgr.backgroundColor);
        REQUIRE(cell.underlineColor() == sgr.underlineColor);
    }

    auto const& cell = inflated[4];
    REQUIRE(cell.foregroundColor() == fillSGR.foregroundColor);
    REQUIRE(cell.backgroundColor() == fillSGR.backgroundColor);
    REQUIRE(cell.underlineColor() == fillSGR.underlineColor);
}

TEST_CASE("Line.inflate.Empty", "[Line]")
{
    // Test inflating an empty trivial buffer
    auto constexpr DisplayWidth = ColumnCount(5);

    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();
    auto const bufferFragment = bufferObject->ref(0, 0);

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0xABCDEF);

    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = ColumnCount(0),
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));
    for (size_t i = 0; i < inflated.size(); ++i)
    {
        auto const& cell = inflated[i];
        INFO(std::format("column {}", i));
        CHECK(cell.codepointCount() == 0);
        CHECK(cell.foregroundColor() == sgr.foregroundColor);
    }
}

TEST_CASE("Line.inflate.InvalidUtf8", "[Line]")
{
    // Test inflating with invalid UTF-8 sequences - should produce replacement characters
    auto constexpr DisplayWidth = ColumnCount(4);

    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();

    // Invalid UTF-8: 0x80 is a continuation byte without a start byte
    // 0xFF is never valid in UTF-8
    auto invalidUtf8 = std::string { 'A', static_cast<char>(0x80), static_cast<char>(0xFF), 'B' };
    bufferObject->writeAtEnd(invalidUtf8);

    auto const bufferFragment = bufferObject->ref(0, invalidUtf8.size());

    auto sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = DisplayWidth,
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));
    CHECK(inflated[0].codepoint(0) == U'A');
    // Invalid bytes produce replacement character (U+FFFD)
    CHECK(inflated[1].codepoint(0) == 0xFFFD);
    CHECK(inflated[2].codepoint(0) == 0xFFFD);
    CHECK(inflated[3].codepoint(0) == U'B');
}

TEST_CASE("Line.inflate.CJK", "[Line]")
{
    // Test inflating with CJK wide characters
    auto constexpr DisplayWidth = ColumnCount(6);
    // Chinese character U+4E2D (width 2) + U+6587 (width 2) + A (width 1) = 5 display width, 1 fill
    auto constexpr TestTextUtf32 = U"\u4E2D\u6587A"sv; // 中文A
    auto const testTextUtf8 = unicode::convert_to<char>(TestTextUtf32);

    auto pool = buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(testTextUtf8);

    auto const bufferFragment = bufferObject->ref(0, testTextUtf8.size());

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x112233);
    auto fillSGR = GraphicsAttributes {};
    fillSGR.foregroundColor = RGBColor(0x445566);

    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = fillSGR,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = ColumnCount(5),
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));

    // First CJK character U+4E2D at columns 0-1
    CHECK(inflated[0].codepoint(0) == 0x4E2D);
    CHECK(inflated[0].width() == 2);
    CHECK(inflated[0].foregroundColor() == sgr.foregroundColor);
    CHECK(inflated[1].codepointCount() == 0); // continuation cell

    // Second CJK character U+6587 at columns 2-3
    CHECK(inflated[2].codepoint(0) == 0x6587);
    CHECK(inflated[2].width() == 2);
    CHECK(inflated[3].codepointCount() == 0); // continuation cell

    // ASCII character A at column 4
    CHECK(inflated[4].codepoint(0) == U'A');
    CHECK(inflated[4].width() == 1);

    // Fill cell at column 5
    CHECK(inflated[5].codepointCount() == 0);
    CHECK(inflated[5].foregroundColor() == fillSGR.foregroundColor);
}

TEST_CASE("Line.inflate.Hyperlink", "[Line]")
{
    // Test that hyperlinks are correctly preserved during inflation
    auto constexpr DisplayWidth = ColumnCount(5);
    auto constexpr TestText = "hello"sv;

    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(TestText);

    auto const bufferFragment = bufferObject->ref(0, TestText.size());

    auto sgr = GraphicsAttributes {};
    auto const hyperlinkId = HyperlinkId(42);

    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = hyperlinkId,
                                             .usedColumns = DisplayWidth,
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));
    for (size_t i = 0; i < inflated.size(); ++i)
    {
        auto const& cell = inflated[i];
        INFO(std::format("column {}", i));
        CHECK(cell.hyperlink() == hyperlinkId);
    }
}

TEST_CASE("Line.inflate.DifferentTextAndFillAttributes", "[Line]")
{
    // Test that text and fill attributes are applied correctly
    auto constexpr DisplayWidth = ColumnCount(6);
    auto constexpr TestText = "ABC"sv;

    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(TestText);

    auto const bufferFragment = bufferObject->ref(0, TestText.size());

    auto textSGR = GraphicsAttributes {};
    textSGR.foregroundColor = RGBColor(0xFF0000);
    textSGR.backgroundColor = RGBColor(0x00FF00);

    auto fillSGR = GraphicsAttributes {};
    fillSGR.foregroundColor = RGBColor(0x0000FF);
    fillSGR.backgroundColor = RGBColor(0xFFFF00);

    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = textSGR,
                                             .fillAttributes = fillSGR,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = ColumnCount(3),
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));

    // Text cells (0-2) should have textSGR
    for (size_t i = 0; i < 3; ++i)
    {
        auto const& cell = inflated[i];
        INFO(std::format("text column {}", i));
        CHECK(cell.foregroundColor() == textSGR.foregroundColor);
        CHECK(cell.backgroundColor() == textSGR.backgroundColor);
    }

    // Fill cells (3-5) should have fillSGR
    for (size_t i = 3; i < 6; ++i)
    {
        auto const& cell = inflated[i];
        INFO(std::format("fill column {}", i));
        CHECK(cell.foregroundColor() == fillSGR.foregroundColor);
        CHECK(cell.backgroundColor() == fillSGR.backgroundColor);
    }
}

TEST_CASE("Line.inflate.ConsecutiveWideChars", "[Line]")
{
    // Test multiple consecutive wide characters
    auto constexpr DisplayWidth = ColumnCount(8);
    // Four Japanese katakana characters, each width 2
    // U+30A2, U+30A4, U+30A6, U+30A8
    auto constexpr TestTextUtf32 = U"\u30A2\u30A4\u30A6\u30A8"sv;
    auto const testTextUtf8 = unicode::convert_to<char>(TestTextUtf32);

    auto pool = buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(testTextUtf8);

    auto const bufferFragment = bufferObject->ref(0, testTextUtf8.size());

    auto sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = DisplayWidth,
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));

    // Check each wide character and its continuation cell
    for (size_t i = 0; i < 4; ++i)
    {
        size_t mainCol = i * 2;
        size_t contCol = i * 2 + 1;
        INFO(std::format("character {} at columns {}-{}", i, mainCol, contCol));
        CHECK(inflated[mainCol].codepoint(0) == TestTextUtf32[i]);
        CHECK(inflated[mainCol].width() == 2);
        CHECK(inflated[contCol].codepointCount() == 0);
    }
}

TEST_CASE("Line.inflate.CombiningCharacters", "[Line]")
{
    // Test combining characters (e.g., e + combining acute = é)
    auto constexpr DisplayWidth = ColumnCount(3);
    // 'e' + combining acute accent + 'x' = "éx" (2 grapheme clusters, 2 display width)
    auto constexpr TestTextUtf32 = U"e\u0301x"sv;
    auto const testTextUtf8 = unicode::convert_to<char>(TestTextUtf32);

    auto pool = buffer_object_pool<char>(32);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(testTextUtf8);

    auto const bufferFragment = bufferObject->ref(0, testTextUtf8.size());

    auto sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = ColumnCount(2),
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));

    // First cell should contain 'e' + combining acute (2 codepoints)
    CHECK(inflated[0].codepointCount() == 2);
    CHECK(inflated[0].codepoint(0) == U'e');
    CHECK(inflated[0].codepoint(1) == U'\u0301');

    // Second cell should contain 'x'
    CHECK(inflated[1].codepoint(0) == U'x');

    // Third cell should be empty (fill)
    CHECK(inflated[2].codepointCount() == 0);
}

TEST_CASE("Line.inflate.ZeroDisplayWidth", "[Line]")
{
    // Test that zero displayWidth returns empty buffer gracefully
    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd("test"sv);
    auto const bufferFragment = bufferObject->ref(0, 4);

    auto sgr = GraphicsAttributes {};
    auto const trivial = TrivialLineBuffer { .displayWidth = ColumnCount(0),
                                             .textAttributes = sgr,
                                             .fillAttributes = sgr,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = ColumnCount(0),
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    CHECK(inflated.empty());
}

TEST_CASE("Line.inflate.UsedColumnsLargerThanActual", "[Line]")
{
    // Test graceful handling when usedColumns is larger than actual text produces
    // This can happen with malformed input
    auto constexpr DisplayWidth = ColumnCount(10);
    auto constexpr TestText = "AB"sv; // Only 2 characters

    auto pool = buffer_object_pool<char>(16);
    auto bufferObject = pool.allocateBufferObject();
    bufferObject->writeAtEnd(TestText);

    auto const bufferFragment = bufferObject->ref(0, TestText.size());

    auto sgr = GraphicsAttributes {};
    sgr.foregroundColor = RGBColor(0x111111);
    auto fillSGR = GraphicsAttributes {};
    fillSGR.foregroundColor = RGBColor(0x222222);

    // usedColumns claims 5, but text only has 2 characters
    auto const trivial = TrivialLineBuffer { .displayWidth = DisplayWidth,
                                             .textAttributes = sgr,
                                             .fillAttributes = fillSGR,
                                             .hyperlink = HyperlinkId {},
                                             .usedColumns = ColumnCount(5),
                                             .text = bufferFragment };

    auto const inflated = inflate<Cell>(trivial);

    // Should gracefully produce displayWidth cells
    CHECK(inflated.size() == unbox<size_t>(DisplayWidth));

    // First 2 cells have actual text
    CHECK(inflated[0].codepoint(0) == U'A');
    CHECK(inflated[1].codepoint(0) == U'B');

    // Cells 2-4 should be padded (up to usedColumns=5)
    for (size_t i = 2; i < 5; ++i)
    {
        INFO(std::format("padded column {}", i));
        CHECK(inflated[i].foregroundColor() == sgr.foregroundColor);
    }

    // Cells 5-9 should have fill attributes
    for (size_t i = 5; i < 10; ++i)
    {
        INFO(std::format("fill column {}", i));
        CHECK(inflated[i].foregroundColor() == fillSGR.foregroundColor);
    }
}
