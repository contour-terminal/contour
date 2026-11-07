// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFont.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;

namespace
{

/// A sixel column with the given pixel rows (0 = top) set.
[[nodiscard]] char sixel(std::initializer_list<int> rows)
{
    auto value = 0;
    for (auto const row: rows)
        value |= 1 << row;
    return static_cast<char>('?' + value);
}

[[nodiscard]] std::string repeat(char ch, size_t count)
{
    auto text = std::string {};
    text.assign(count, ch);
    return text;
}

/// Pe=0, Pcn=1, Pcmw=0, Pss=0, Pt=2 (full cell), 94 characters: the VT320 style header.
[[nodiscard]] SoftFontDeclaration fullCellDeclaration(int pe = 0, int pcn = 1)
{
    return decodeDecdld({ .pcn = pcn, .pe = pe, .pt = 2 }).value();
}

[[nodiscard]] std::vector<uint8_t> row(SoftGlyph const& glyph, size_t y)
{
    auto const begin = glyph.coverage.begin() + static_cast<std::ptrdiff_t>(y * glyph.width);
    return { begin, begin + glyph.width };
}

} // namespace

TEST_CASE("decodeDecdld: Pcn is a position, not a code point", "[SoftFont]")
{
    CHECK(decodeDecdld({ .pfn = 0, .pcn = 1 }).value().startPosition == 0x21);
    CHECK(decodeDecdld({ .pcn = 0 }).value().startPosition == 0x21); // absent in a 94-set means 1
    CHECK(decodeDecdld({ .pcn = 0, .pcss = 1 }).value().startPosition == 0x20);
    CHECK(decodeDecdld({ .pcn = 95 }).value().startPosition == 0x7F);
    CHECK(decodeDecdld({ .pcn = 96 }).error() == SoftFontError::TooLarge);
}

TEST_CASE("decodeDecdld: rejects what DEC rejects", "[SoftFont]")
{
    auto const [parameters, error] = GENERATE(table<DecdldParameters, SoftFontError>({
        { DecdldParameters { .pfn = 3 }, SoftFontError::FontNumber },
        { DecdldParameters { .pe = 3 }, SoftFontError::EraseControl },
        { DecdldParameters { .pcmw = 1 }, SoftFontError::CellMatrix },
        { DecdldParameters { .pss = 5 }, SoftFontError::FontSet },
        { DecdldParameters { .pt = 3 }, SoftFontError::Usage },
        { DecdldParameters { .pcss = 2 }, SoftFontError::CharsetSize },
        { DecdldParameters { .pcmw = 2, .pt = 2 }, SoftFontError::CellMatrix },
        { DecdldParameters { .pcmw = 17 }, SoftFontError::TooLarge },
        { DecdldParameters { .pcmh = 33 }, SoftFontError::TooLarge },
    }));
    auto const result = decodeDecdld(parameters);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == error);
}

TEST_CASE("decodeDecdld: sizes and formats", "[SoftFont]")
{
    auto const matrix = decodeDecdld({ .pcmw = 3 }).value();
    CHECK(matrix.sizeSource == SoftFontSizeSource::Matrix);
    CHECK(matrix.declaredWidth == 6);
    CHECK(matrix.declaredHeight == 10);

    // 6x10 is allowed for full-cell fonts at 132 columns only.
    CHECK(decodeDecdld({ .pcmw = 3, .pss = 2, .pt = 2 }).has_value());
    CHECK_FALSE(decodeDecdld({ .pcmw = 3, .pss = 0, .pt = 2 }).has_value());

    auto const pixels = decodeDecdld({ .pcmw = 12, .pss = 22, .pt = 2, .pcmh = 20 }).value();
    CHECK(pixels.sizeSource == SoftFontSizeSource::Pixels);
    CHECK(pixels.declaredWidth == 12);
    CHECK(pixels.declaredHeight == 20);
    CHECK(pixels.columnsPerPage == 132);
    CHECK(pixels.linesPerPage == 48);
    CHECK(pixels.usage == SoftFontUsage::FullCell);

    CHECK(decodeDecdld({ .pt = 1 }).value().usage == SoftFontUsage::Text);
    CHECK(decodeDecdld({ .pss = 11 }).value().linesPerPage == 36);
    CHECK(decodeDecdld({ .pcss = 1 }).value().charsetSize == SoftCharsetSize::Size96);
}

TEST_CASE("detectTargetCell: one row per terminal", "[SoftFont]")
{
    struct Case
    {
        DecdldParameters parameters {};
        uint8_t usedWidth = 0;
        uint8_t usedHeight = 0;
        SoftCellGeometry expected {};
    };
    auto const check = [](Case const& c) {
        auto const actual = detectTargetCell(decodeDecdld(c.parameters).value(), c.usedWidth, c.usedHeight);
        CHECK(int { actual.fullWidth } == int { c.expected.fullWidth });
        CHECK(int { actual.fullHeight } == int { c.expected.fullHeight });
        CHECK(int { actual.textWidth } == int { c.expected.textWidth });
    };

    SECTION("real fonts")
    {
        // vt100.net / cmatrix header Pfn=0;Pcn=1;Pe=1;Pcmw=0;Pss=0;Pt=2, 15x12 used.
        check({ { .pcn = 1, .pe = 1, .pt = 2 }, 15, 12, { 15, 12, 0 } });
        // larsbrinkhoff header P1;1;1{ @, 8x12 used.
        check({ { .pfn = 1, .pcn = 1, .pe = 1 }, 8, 12, { 10, 10, 8 } });
    }

    SECTION("80 columns")
    {
        check({ { .pt = 2 }, 8, 6, { 10, 10, 8 } });
        check({ { .pt = 2 }, 15, 12, { 15, 12, 0 } });
        check({ { .pt = 2 }, 10, 18, { 10, 16, 0 } }); // 16 rounds up to 18
        check({ { .pt = 2 }, 10, 24, { 10, 20, 0 } });
        check({ { .pt = 2 }, 10, 12, { 15, 12, 0 } });
        check({ { .pt = 2 }, 12, 30, { 12, 30, 0 } });
        check({ { .pt = 2 }, 16, 32, { 16, 32, 0 } });
        check({ { .pcmw = 9 }, 9, 12, { 15, 12, 9 } }); // declared width, text font
    }

    SECTION("132 columns")
    {
        check({ { .pss = 2, .pt = 2 }, 6, 6, { 6, 10, 0 } });
        check({ { .pss = 2, .pt = 2 }, 9, 12, { 9, 12, 0 } });
        check({ { .pss = 2, .pt = 2 }, 6, 18, { 6, 16, 0 } });
        check({ { .pss = 2, .pt = 2 }, 6, 24, { 6, 20, 0 } });
        check({ { .pss = 2, .pt = 2 }, 7, 30, { 7, 30, 0 } });
        check({ { .pss = 2, .pt = 2 }, 16, 32, { 16, 32, 0 } });
        check({ { .pcmw = 8, .pss = 2, .pcmh = 12 }, 8, 12, { 9, 12, 8 } });
    }

    SECTION("overrides")
    {
        // A matrix is a VT2xx cell.
        check({ { .pcmw = 4 }, 7, 12, { 10, 10, 8 } });
        check({ { .pcmw = 3, .pss = 2 }, 6, 12, { 6, 10, 0 } });
        check({ { .pcmw = 4, .pss = 2 }, 7, 12, { 10, 10, 8 } });
        // Declared width and height on a full-cell font are the exact cell.
        check({ { .pcmw = 11, .pt = 2, .pcmh = 14 }, 11, 18, { 11, 14, 0 } });
        // Anything but 24 lines is a VT420/VT5xx.
        check({ { .pss = 11, .pt = 2 }, 15, 12, { 10, 10, 0 } });
        check({ { .pss = 21, .pt = 2 }, 15, 12, { 10, 8, 0 } });
        check({ { .pss = 12, .pt = 2 }, 15, 12, { 6, 10, 0 } });
        check({ { .pcmw = 6, .pss = 22 }, 6, 12, { 6, 8, 6 } });
    }
}

TEST_CASE("buildSoftFont: a VT320 full-cell glyph is stored at 15x12 and at position 0x21", "[SoftFont]")
{
    // Two glyphs of 15 columns and two sixel rows, with CR/LF and a stray byte in the data.
    auto const body = "B" + std::string(1, sixel({ 0, 1, 2, 3, 4, 5 })) + repeat(sixel({ 0 }), 14) + "\r\n/"
                      + repeat(sixel({ 5 }), 15) + ";" + repeat(sixel({ 0 }), 15) + "/"
                      + repeat(sixel({ 5 }), 15);
    auto const font = buildSoftFont(fullCellDeclaration(), body, nullptr).value();

    CHECK(font.designator == "B");
    CHECK(int { font.geometry.fullWidth } == 15);
    CHECK(int { font.geometry.fullHeight } == 12);
    CHECK(font.glyphs[0] == nullptr);
    REQUIRE(font.glyphs[1] != nullptr);
    REQUIRE(font.glyphs[2] != nullptr);
    CHECK(font.glyphs[3] == nullptr);

    auto const& glyph = *font.glyphs[1];
    CHECK(int { glyph.width } == 15);
    CHECK(int { glyph.height } == 12);
    REQUIRE(glyph.coverage.size() == size_t { 180 });

    auto const topRow = std::vector<uint8_t>(15, 1);
    auto const leftOnly = std::vector<uint8_t> { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    auto const blank = std::vector<uint8_t>(15, 0);
    CHECK(row(glyph, 0) == std::vector<uint8_t>(15, 1));
    CHECK(row(glyph, 0) == topRow);
    for (auto const y: { 1, 2, 3, 4, 5 })
        CHECK(row(glyph, y) == leftOnly);
    for (auto const y: { 6, 7, 8, 9, 10 })
        CHECK(row(glyph, y) == blank);
    CHECK(row(glyph, 11) == topRow);

    CHECK(font.glyphs[2]->id != glyph.id);
}

TEST_CASE("buildSoftFont: columns past 16 do not spill into the next row", "[SoftFont]")
{
    // 20 columns of "top pixel set": only the first 16 may land, and row 1 must stay empty.
    auto const body = "B" + repeat(sixel({ 0 }), 20);
    auto const font = buildSoftFont(fullCellDeclaration(), body, nullptr).value();
    REQUIRE(font.glyphs[1] != nullptr);
    auto const& glyph = *font.glyphs[1];
    // 16 used columns exceed every cell but the fallback, which is the full 16x32.
    CHECK(int { glyph.width } == 16);
    CHECK(int { glyph.height } == 32);
    CHECK(row(glyph, 0) == std::vector<uint8_t>(16, 1));
    CHECK(row(glyph, 1) == std::vector<uint8_t>(16, 0));
}

TEST_CASE("buildSoftFont: a VT220 text glyph is clipped to 8 and centred in 10", "[SoftFont]")
{
    // A 7x10 matrix is a VT2xx cell. Ten columns are drawn; only the first 8 are kept, shifted right by one.
    auto const body = "B" + std::string(1, sixel({ 0, 1, 2, 3, 4, 5 })) + repeat(sixel({}), 6)
                      + repeat(sixel({ 0 }), 3) + "/" + repeat(sixel({}), 10);
    auto const declaration = decodeDecdld({ .pcn = 1, .pe = 1, .pcmw = 4 }).value();
    auto const font = buildSoftFont(declaration, body, nullptr).value();

    CHECK(int { font.geometry.fullWidth } == 10);
    CHECK(int { font.geometry.fullHeight } == 10);
    CHECK(int { font.geometry.textWidth } == 8);
    REQUIRE(font.glyphs[1] != nullptr);
    auto const& glyph = *font.glyphs[1];
    for (auto const y: std::views::iota(size_t { 0 }, size_t { 10 }))
    {
        auto const cells = row(glyph, y);
        CHECK(cells[0] == 0);                // centring margin
        CHECK(cells[1] == (y < 6 ? 1 : 0));  // input column 0
        CHECK(cells[8] == (y == 0 ? 1 : 0)); // input column 7, the last one kept
        CHECK(cells[9] == 0);                // input columns 8 and 9 were clipped
    }
}

TEST_CASE("buildSoftFont: two-intermediate designator", "[SoftFont]")
{
    auto const declaration = fullCellDeclaration();
    CHECK(buildSoftFont(declaration, " @~", nullptr).value().designator == " @");
    CHECK(buildSoftFont(declaration, "%5~", nullptr).value().designator == "%5");
    CHECK(buildSoftFont(declaration, "B~", nullptr).value().designator == "B");
    CHECK(buildSoftFont(declaration, " ", nullptr).error() == SoftFontError::Designator);
    CHECK(buildSoftFont(declaration, "", nullptr).error() == SoftFontError::Designator);
    CHECK(buildSoftFont(declaration, "   B~", nullptr).error() == SoftFontError::Designator);
}

TEST_CASE("buildSoftFont: Pe=1 keeps other glyphs, Pe=0 drops them", "[SoftFont]")
{
    auto const first = buildSoftFont(fullCellDeclaration(),
                                     "B" + repeat(sixel({ 0 }), 15) + ";" + repeat(sixel({ 1 }), 15),
                                     nullptr)
                           .value();
    REQUIRE(first.glyphs[1] != nullptr);
    REQUIRE(first.glyphs[2] != nullptr);

    SECTION("Pe=1 redefines only what it reloads")
    {
        auto const merged =
            buildSoftFont(fullCellDeclaration(1, 2), "B" + repeat(sixel({ 2 }), 15), &first).value();
        CHECK(merged.glyphs[1] == first.glyphs[1]);
        REQUIRE(merged.glyphs[2] != nullptr);
        CHECK(merged.glyphs[2]->id != first.glyphs[2]->id);
        CHECK(merged.glyphs[2]->coverage[size_t { 30 }] == 1);
    }

    SECTION("Pe=0 starts from nothing")
    {
        auto const fresh =
            buildSoftFont(fullCellDeclaration(0, 2), "B" + repeat(sixel({ 2 }), 15), &first).value();
        CHECK(fresh.glyphs[1] == nullptr);
        CHECK(fresh.glyphs[2] != nullptr);
    }

    SECTION("Pe=1 with a different designator starts from nothing")
    {
        auto const other =
            buildSoftFont(fullCellDeclaration(1, 2), "C" + repeat(sixel({ 2 }), 15), &first).value();
        CHECK(other.glyphs[1] == nullptr);
    }

    SECTION("Pe=1 with a different usage starts from nothing")
    {
        auto const text = decodeDecdld({ .pcn = 2, .pe = 1 }).value();
        auto const other = buildSoftFont(text, "B" + repeat(sixel({ 2 }), 8), &first).value();
        CHECK(other.glyphs[1] == nullptr);
    }
}

TEST_CASE("buildSoftFont: undefined positions are null and the error glyph is full-size", "[SoftFont]")
{
    auto const font = buildSoftFont(fullCellDeclaration(), "B" + repeat(sixel({ 0 }), 15), nullptr).value();
    CHECK(font.glyphs[0] == nullptr);
    CHECK(font.glyphs[50] == nullptr);
    CHECK(font.glyphs[95] == nullptr);
    REQUIRE(font.errorGlyph != nullptr);
    CHECK(font.errorGlyph->width == font.geometry.fullWidth);
    CHECK(font.errorGlyph->height == font.geometry.fullHeight);
    CHECK(font.errorGlyph->coverage.size() == size_t { 180 });
    CHECK(std::ranges::count(font.errorGlyph->coverage, 1) > 0);
}

TEST_CASE("buildSoftFont: positions past 0x7F are dropped", "[SoftFont]")
{
    // Start at 0x7F: the first glyph lands, the second does not.
    auto const declaration = decodeDecdld({ .pcn = 95, .pt = 2 }).value();
    auto const font =
        buildSoftFont(declaration, "B" + repeat(sixel({ 0 }), 15) + ";" + repeat(sixel({ 1 }), 15), nullptr)
            .value();
    CHECK(font.glyphs[95] != nullptr);
    CHECK(std::ranges::count(font.glyphs, nullptr) == 95);
}

TEST_CASE("makeSoftFont: the error glyph at 15x23 is the reference pattern", "[SoftFont]")
{
    auto const font = makeSoftFont(fullCellDeclaration(), "B", { 15, 23, 0 }, {});
    auto const& glyph = *font.errorGlyph;
    CHECK(row(glyph, 0) == std::vector<uint8_t>(15, 0));
    CHECK(row(glyph, 6) == std::vector<uint8_t> { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 });
    CHECK(row(glyph, 19) == std::vector<uint8_t>(15, 0));
}

TEST_CASE("makeSoftGlyph: identity is the content", "[SoftFont]")
{
    auto const a = makeSoftGlyph(2, 2, { 1, 0, 0, 1 });
    auto const b = makeSoftGlyph(2, 2, { 1, 0, 0, 1 });
    auto const c = makeSoftGlyph(2, 2, { 0, 1, 1, 0 });
    auto const d = makeSoftGlyph(4, 1, { 1, 0, 0, 1 });
    CHECK(a->id == b->id);
    CHECK(a->id != c->id);
    CHECK(a->id != d->id);
}

TEST_CASE("buildSoftFont: a flood of character separators stops at position 0x7F", "[SoftFont]")
{
    auto const declaration = decodeDecdld({ .pcn = 1, .pt = 2 }).value();
    auto const font =
        buildSoftFont(declaration, "B" + repeat(sixel({ 0 }), 15) + repeat(';', 100000), nullptr).value();
    CHECK(font.glyphs[1] != nullptr);
    CHECK(std::ranges::count(font.glyphs, nullptr) == 1);
}
