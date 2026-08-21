// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for encodeSixel(), the inverse of SixelParser.
//
// Most of these are ROUND TRIPS: encode an image, decode it with our own parser, compare. That is the
// property that actually matters -- the two halves have to agree about the same wire format -- and it
// is a far stronger check than asserting against a hand-written expected string, which only says the
// encoder still does what it did yesterday. The few tests that DO read the bytes are the ones where a
// round trip cannot see the difference: framing, run-length encoding, and the trailing-run trim, all
// of which decode identically whether or not the encoder got them right.

#include <vtbackend/graphics/SixelEncoder.hpp>
#include <vtbackend/graphics/SixelParser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;
using namespace vtbackend;

namespace
{

/// Channel values that are whole percentages, and therefore survive sixel's percent-space colors
/// exactly. @see encodeSixel.
constexpr auto Exact = std::array<uint8_t, 6> { 0, 51, 102, 153, 204, 255 };

/// A tightly-packed RGBA image a test paints cell by cell.
struct TestImage
{
    ImageSize size;
    std::vector<uint8_t> pixels;

    TestImage(unsigned width, unsigned height):
        size { Width(width), Height(height) }, pixels(static_cast<size_t>(width) * height * 4, 0)
    {
    }

    void set(unsigned x, unsigned y, RGBAColor color)
    {
        auto const at = ((static_cast<size_t>(y) * unbox<size_t>(size.width)) + x) * 4;
        pixels[at] = color.red();
        pixels[at + 1] = color.green();
        pixels[at + 2] = color.blue();
        pixels[at + 3] = color.alpha();
    }

    void fill(RGBAColor color)
    {
        for (auto const y: std::views::iota(0u, unbox<unsigned>(size.height)))
            for (auto const x: std::views::iota(0u, unbox<unsigned>(size.width)))
                set(x, y, color);
    }

    [[nodiscard]] std::string encode() const { return encodeSixel(pixels, size); }
};

/// Strips the DCS envelope encodeSixel() wraps its data in, asserting its shape on the way through --
/// which is how the envelope itself gets checked, since the parser below never sees it.
///
/// P1 = 7 is a 1:1 pixel aspect and P2 = 1 leaves unpainted pixels alone; a terminal reading a stream
/// that said otherwise would stretch the image or paint its transparent pixels with the background.
[[nodiscard]] std::string_view sixelDataOf(std::string_view sequence)
{
    REQUIRE(sequence.starts_with("\033P7;1;0q"sv));
    REQUIRE(sequence.ends_with("\033\\"sv));
    return sequence.substr(8, sequence.size() - 8 - 2);
}

/// @return How many color registers @p sequence defines, which is the palette the encoder chose.
[[nodiscard]] size_t colorDefinitionsIn(std::string_view sequence)
{
    auto count = size_t { 0 };
    for (auto at = sequence.find(";2;"); at != std::string_view::npos; at = sequence.find(";2;", at + 1))
        ++count;
    return count;
}

/// Decodes @p sequence the way a terminal would, and hands back the image it drew.
[[nodiscard]] SixelImageBuilder decode(std::string_view sequence, ImageSize canvas)
{
    auto builder = SixelImageBuilder {
        canvas, SixelAspectRatio {}, RGBAColor { 0, 0, 0, 0 }, std::make_shared<SixelColorPalette>(16, 256)
    };
    SixelParser::parse(sixelDataOf(sequence), builder);
    return builder;
}

} // namespace

TEST_CASE("SixelEncoder round-trips an image our own parser reads back", "[sixel][screenshot]")
{
    // A deliberately awkward size: 7 rows is one full band plus one row, so the last band is partial,
    // and 5 columns is short enough that every column is checked.
    auto image = TestImage { 5, 7 };
    for (auto const y: std::views::iota(0u, 7u))
        for (auto const x: std::views::iota(0u, 5u))
            image.set(x, y, RGBAColor { Exact[(x + y) % Exact.size()], Exact[y % Exact.size()], 51, 255 });

    auto const decoded = decode(image.encode(), ImageSize { Width(64), Height(64) });

    CHECK(decoded.size() == image.size);
    for (auto const y: std::views::iota(0u, 7u))
        for (auto const x: std::views::iota(0u, 5u))
        {
            auto const at = decoded.at(CellLocation { LineOffset::cast_from(y), ColumnOffset::cast_from(x) });
            CHECK(at.red() == Exact[(x + y) % Exact.size()]);
            CHECK(at.green() == Exact[y % Exact.size()]);
            CHECK(at.blue() == 51);
        }
}

TEST_CASE("SixelEncoder states the image's extent up front", "[sixel][screenshot]")
{
    // The raster attribute, so a decoder sizes its canvas once instead of growing it band by band.
    auto image = TestImage { 12, 9 };
    image.fill(RGBAColor { 255, 0, 0, 255 });

    CHECK(sixelDataOf(image.encode()).starts_with("\"1;1;12;9"sv));
    CHECK(decode(image.encode(), ImageSize { Width(64), Height(64) }).size()
          == ImageSize { Width(12), Height(9) });
}

TEST_CASE("SixelEncoder collapses colors the wire cannot tell apart", "[sixel][screenshot]")
{
    // Sixel states a color in percent, so an 8-bit channel maps onto one of 101 values and several
    // 8-bit values share each: 50, 51 and 52 are all 20%. Counting distinct colors in 8-bit space
    // instead would spend three registers where the wire can express only one.
    auto image = TestImage { 3, 1 };
    image.set(0, 0, RGBAColor { 50, 0, 0, 255 });
    image.set(1, 0, RGBAColor { 51, 0, 0, 255 });
    image.set(2, 0, RGBAColor { 52, 0, 0, 255 });

    CHECK(colorDefinitionsIn(image.encode()) == 1);

    // And all three columns come back as that one color.
    auto const decoded = decode(image.encode(), ImageSize { Width(64), Height(64) });
    for (auto const x: std::views::iota(0u, 3u))
        CHECK(decoded.at(CellLocation { LineOffset(0), ColumnOffset::cast_from(x) })
              == RGBAColor { 51, 0, 0, 255 });
}

TEST_CASE("SixelEncoder falls back to a fixed cut when the palette overflows", "[sixel][screenshot]")
{
    // 1024 pixels, each a distinct color IN PERCENT SPACE -- so collapsing cannot rescue this one and
    // the fallback is genuinely exercised. It must still produce a decodable image of the right size
    // rather than refusing, truncating, or naming a register a decoder need not have.
    auto image = TestImage { 64, 16 };
    auto const percentToByte = [](unsigned percent) {
        return static_cast<uint8_t>((percent * 255u) / 100u);
    };
    for (auto const pixel: std::views::iota(0u, 64u * 16u))
        image.set(pixel % 64,
                  pixel / 64,
                  RGBAColor { percentToByte(pixel % 101), percentToByte((pixel / 101) % 101), 0, 255 });

    auto const sequence = image.encode();
    CHECK(colorDefinitionsIn(sequence) <= MaxSixelPaletteSize);
    CHECK(colorDefinitionsIn(sequence) == 6u * 7u * 6u); // the fixed cut, and nothing else

    auto const decoded = decode(sequence, ImageSize { Width(256), Height(256) });
    CHECK(decoded.size() == image.size);

    // The cut is coarse but not arbitrary: full red lands on the bucket whose centre IS full red.
    // Pixel 100 is (255, 0, 0), which is column 36 of row 1.
    CHECK(decoded.at(CellLocation { LineOffset(1), ColumnOffset(36) }) == RGBAColor { 255, 0, 0, 255 });
}

TEST_CASE("SixelEncoder leaves a transparent pixel unpainted", "[sixel][screenshot]")
{
    // Sixel has no alpha, so the only honest rendering of "nothing here" is to not paint at all --
    // painting it black would put a hole in the screenshot that was not on the screen.
    auto image = TestImage { 4, 6 };
    image.fill(RGBAColor { 255, 0, 0, 255 });
    image.set(1, 1, RGBAColor { 0, 0, 0, 0 });
    image.set(2, 4, RGBAColor { 255, 255, 255, 10 }); // below half opaque

    auto const decoded = decode(image.encode(), ImageSize { Width(64), Height(64) });

    // The canvas was cleared transparent, so an unpainted pixel is still transparent.
    CHECK(decoded.at(CellLocation { LineOffset(1), ColumnOffset(1) }).alpha() == 0);
    CHECK(decoded.at(CellLocation { LineOffset(4), ColumnOffset(2) }).alpha() == 0);
    // Its neighbours were painted.
    CHECK(decoded.at(CellLocation { LineOffset(1), ColumnOffset(0) }) == RGBAColor { 255, 0, 0, 255 });
}

TEST_CASE("SixelEncoder run-length encodes a repeated column", "[sixel][screenshot]")
{
    // A solid row is one repeat instruction, not a hundred characters. Invisible to a round trip,
    // which is why this one reads the bytes.
    auto image = TestImage { 100, 6 };
    image.fill(RGBAColor { 0, 255, 0, 255 });

    auto const data = std::string { sixelDataOf(image.encode()) };
    CHECK(data.contains("!100~")); // 100 columns of all six rows set: 63 + 63 = '~'
    CHECK(decode(image.encode(), ImageSize { Width(128), Height(64) }).size() == image.size);
}

TEST_CASE("SixelEncoder drops a trailing run of unpainted columns", "[sixel][screenshot]")
{
    // Whatever follows returns to the left edge anyway, so trailing `?`s are pure cost. A round trip
    // cannot see this either: the pixels are transparent whether or not they were transmitted.
    auto image = TestImage { 40, 6 };
    for (auto const y: std::views::iota(0u, 6u))
        image.set(0, y, RGBAColor { 0, 0, 255, 255 });

    auto const data = std::string { sixelDataOf(image.encode()) };
    CHECK(!data.contains('?'));
    CHECK(decode(image.encode(), ImageSize { Width(64), Height(64) })
              .at(CellLocation { LineOffset(0), ColumnOffset(0) })
          == RGBAColor { 0, 0, 255, 255 });
}

TEST_CASE("SixelEncoder overlays several colors within one band", "[sixel][screenshot]")
{
    // Two colors in the same six-row band means two passes over it, separated by `$` -- back to the
    // left edge WITHOUT advancing. Using `-` there instead would push the second color a band down,
    // which a round trip catches precisely because the image would come out twice as tall.
    auto image = TestImage { 2, 6 };
    image.fill(RGBAColor { 255, 0, 0, 255 });
    image.set(1, 3, RGBAColor { 0, 0, 255, 255 });

    auto const sequence = image.encode();
    CHECK(std::string { sixelDataOf(sequence) }.contains('$'));

    auto const decoded = decode(sequence, ImageSize { Width(64), Height(64) });
    CHECK(decoded.size() == ImageSize { Width(2), Height(6) });
    CHECK(decoded.at(CellLocation { LineOffset(3), ColumnOffset(1) }) == RGBAColor { 0, 0, 255, 255 });
    CHECK(decoded.at(CellLocation { LineOffset(3), ColumnOffset(0) }) == RGBAColor { 255, 0, 0, 255 });
}

TEST_CASE("SixelEncoder separates bands but does not trail one", "[sixel][screenshot]")
{
    // A `-` after the last band would leave the decoder's cursor on a seventh, empty band, which some
    // readers turn into a taller image than the raster attribute promised.
    auto image = TestImage { 2, 12 };
    image.fill(RGBAColor { 255, 255, 255, 255 });

    auto const data = std::string { sixelDataOf(image.encode()) };
    CHECK(data.contains('-'));
    CHECK(!data.ends_with('-'));
    CHECK(decode(image.encode(), ImageSize { Width(64), Height(64) }).size() == image.size);
}
