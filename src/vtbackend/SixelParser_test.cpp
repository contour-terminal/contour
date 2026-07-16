// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/SixelParser.h>

#include <crispy/times.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <format>
#include <ranges>
#include <string_view>
#include <tuple>

using namespace vtbackend;

namespace
{

SixelImageBuilder sixelImageBuilder(ImageSize size, RGBAColor defaultColor)
{
    auto ib = SixelImageBuilder(size, 1, 1, defaultColor, std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(1, 1, size);
    return ib;
}

} // namespace

TEST_CASE("SixelParser.ground_000000", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr PinColor = RGBColor { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(ImageSize { Width(4), Height(10) }, DefaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == CellLocation { {}, {} });

    ib.setColor(0, PinColor);
    sp.parseFragment("?");

    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            auto const& actualColor =
                ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) });
            CHECK(actualColor == DefaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_111111", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x40 };

    auto ib = sixelImageBuilder(ImageSize { Width(2), Height(8) }, DefaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });

    ib.setColor(0, PinColor);

    sp.parseFragment("~"); // 0b111111 + 63 == 126 == '~'

    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            auto const& actualColor =
                ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) });
            auto const pinned = x == 0 && y >= 0 && y <= 5;
            INFO(std::format("x={}, y={}, {}", x, y, pinned ? "pinned" : ""));
            if (pinned)
                CHECK(actualColor.rgb() == PinColor);
            else
                CHECK(actualColor == DefaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_000001", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr PinColor = RGBColor { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(ImageSize { Width(4), Height(10) }, DefaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });

    ib.setColor(0, PinColor);

    sp.parseFragment("@");

    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            INFO(std::format("x={}, y={}", x, y));
            auto const& actualColor =
                ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) });
            auto const pinned = x == 0 && y == 0;
            if (pinned)
                CHECK(actualColor.rgb() == PinColor);
            else
                CHECK(actualColor == DefaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_010101", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr PinColor = RGBColor { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(ImageSize { Width(2), Height(8) }, DefaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });

    ib.setColor(0, PinColor);

    sp.parseFragment("T"); // 0b010101 + 63 == 'T'

    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            INFO(std::format("x={}, y={}", x, y));
            auto const& actualColor =
                ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) });
            auto const pinned = x == 0 && (y < 6 && y % 2 == 0);
            if (pinned)
                CHECK(actualColor.rgb() == PinColor);
            else
                CHECK(actualColor == DefaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_101010", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr PinColor = RGBColor { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(ImageSize { Width(2), Height(8) }, DefaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });

    ib.setColor(0, PinColor);

    sp.parseFragment("i"); // 0b101010 + 63 == 'i'

    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            INFO(std::format("x={}, y={}", x, y));
            auto const& actualColor =
                ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) });
            auto const pinned = x == 0 && (y < 6 && y % 2 != 0);
            if (pinned)
                CHECK(actualColor.rgb() == PinColor);
            else
                CHECK(actualColor == DefaultColor);
        }
    }
}

TEST_CASE("SixelParser.raster", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto ib = sixelImageBuilder(ImageSize { Width(640), Height(480) }, DefaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });

    sp.parseFragment("\"12;34;32;24");
    sp.done();

    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(ib.aspectRatio() == 1);
    CHECK(*ib.size().width == 32);
    CHECK(*ib.size().height == 24);
    sp.parseFragment("\"12;34");
    sp.done();
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"");
    sp.done();
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"0;0");
    sp.done();
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"5;0");
    sp.done();
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"15;2");
    sp.done();
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(ib.aspectRatio() == 8);
}

TEST_CASE("SixelParser.rep", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x30 };
    auto ib = sixelImageBuilder(ImageSize { Width(14), Height(8) }, DefaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });

    ib.setColor(0, PinColor);

    sp.parseFragment("!12~");

    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(12) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            auto const& actualColor =
                ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) });
            auto const pinned = x < 12 && y < 6;
            if (pinned)
                CHECK(actualColor.rgb() == PinColor);
            else
                CHECK(actualColor == DefaultColor);
        }
    }
}

TEST_CASE("SixelParser.setAndUseColor", "[sixel]")
{
    auto constexpr PinColors = std::array<RGBAColor, 5> { RGBAColor { 255, 255, 255, 255 },
                                                          RGBAColor { 255, 0, 0, 255 },
                                                          RGBAColor { 0, 255, 0, 255 },
                                                          RGBAColor { 0, 0, 255, 255 },
                                                          RGBAColor { 255, 255, 255, 255 } };

    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto ib = sixelImageBuilder(ImageSize { Width(5), Height(6) }, DefaultColor);
    auto sp = SixelParser { ib };

    sp.parseFragment("#1;2;100;0;0");
    sp.parseFragment("#2;2;0;100;0");
    sp.parseFragment("#3;2;0;0;100");
    sp.parseFragment("#4;2;100;100;100");

    sp.parseFragment("~"); // We paint with the last set color.
    sp.parseFragment("#1~");
    sp.parseFragment("#2~");
    sp.parseFragment("#3~");
    sp.parseFragment("#4~");
    sp.done();

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(5) });

    for (auto const [x, y]:
         crispy::times(ib.size().width.as<int>()) * crispy::times(ib.size().height.as<int>()))
    {
        auto const& expectedColor =
            x < 5 && y < 6 ? PinColors.at(static_cast<size_t>(x ? x : 4)) : DefaultColor;
        auto const& actualColor = ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) });
        // INFO(std::format("at {}, expect {}, actual {}",
        //                  CellLocation { LineOffset(y), ColumnOffset(x) },
        //                  expectedColor,
        //                  actualColor));
        CHECK(actualColor == expectedColor);
    }
}

TEST_CASE("SixelParser.rewind", "[sixel]")
{
    auto constexpr PinColors = std::array<RGBAColor, 4> {
        RGBAColor { 0, 0, 0, 255 },
        RGBAColor { 255, 255, 0, 255 },
        RGBAColor { 0, 255, 255, 255 },
    };

    auto constexpr DefaultColor = PinColors[0];
    auto ib = sixelImageBuilder(ImageSize { Width(4), Height(6) }, DefaultColor);
    auto sp = SixelParser { ib };

    sp.parseFragment("#1;2;100;100;0");
    sp.parseFragment("#2;2;0;100;100");

    sp.parseFragment("#1~~~~"); // 4 sixels in color #1
    sp.parseFragment("$");      // rewind
    sp.parseFragment("#2~~");   // 2 sixels in color #2
    sp.done();

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(2) });

    for (int y = 0; y < 6; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            auto const expectedColor = x < 2 ? PinColors[2] : PinColors[1];
            auto const pos = CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) };
            auto const actualColor = ib.at(pos);

            CHECK(actualColor == expectedColor);
        }
    }
}

TEST_CASE("SixelParser.newline", "[sixel]")
{
    auto constexpr PinColors = std::array<RGBAColor, 4> {
        RGBAColor { 0, 0, 0, 255 },
        RGBAColor { 255, 255, 0, 255 },
        RGBAColor { 0, 255, 255, 255 },
    };

    auto constexpr DefaultColor = PinColors[0];
    auto ib = sixelImageBuilder(ImageSize { Width(5), Height(13) }, DefaultColor);
    auto sp = SixelParser { ib };

    sp.parseFragment("#1;2;100;100;0");
    sp.parseFragment("#2;2;0;100;100");

    sp.parseFragment("#1~~~~"); // 4 sixels in color #1
    sp.parseFragment("-");      // newline
    sp.parseFragment("#2~~~~"); // 4 sixels in color #2
    sp.done();

    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(6), ColumnOffset(4) });

    for (int y = 0; y < ib.size().height.as<int>(); ++y)
    {
        for (int x = 0; x < ib.size().width.as<int>(); ++x)
        {
            auto const expectedColor = [&](int x, int y) -> RGBAColor {
                if (y < 6 && x < 4)
                    return PinColors[1];
                if (y < 12 && x < 4)
                    return PinColors[2];
                return PinColors[0];
            }(x, y);

            auto const pos = CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) };
            auto const actualColor = ib.at(pos);

            CHECK(actualColor == expectedColor);
        }
    }
}

TEST_CASE("SixelParser.vertical_cursor_advance", "[sixel]")
{
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 255 };
    SixelImageBuilder ib(
        { Width(5), Height(30) }, 1, 1, DefaultColor, std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };

    sp.parseFragment("$-$-$-$-");
    sp.done();

    REQUIRE(ib.size() == vtbackend::ImageSize { Width(1), Height(24) });
    REQUIRE(ib.sixelCursor() == CellLocation { LineOffset(24), ColumnOffset { 0 } });
}

TEST_CASE("SixelParser.aspect_ratio_overflow", "[sixel]")
{
    // 3x7 pixel buffer, aspect ratio 2.  '~' sets all 6 sixel bits.
    // The sixth pixel row spans rows 6-7; row 7 is past the end.
    // bit 0: rows 0,1  ok      bit 3: rows 6,7  row 7 overflows
    // bit 1: rows 2,3  ok      bit 4: rows 8,9  skipped
    // bit 2: rows 4,5  ok      bit 5: rows 10,11 skipped
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x40 };

    auto ib = SixelImageBuilder(
        { Width(3), Height(7) }, 2, 1, DefaultColor, std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(2, 1, std::nullopt);

    auto sp = SixelParser { ib };

    ib.setColor(0, PinColor);

    sp.parseFragment("~");
    sp.done();

    REQUIRE(ib.size().width == Width(1));
    REQUIRE(ib.size().height == Height(6));

    // bit 0: rows 0,1 ok     bit 3: rows 6,7 row 7 overflows
    // bit 1: rows 2,3 ok     bit 4: rows 8,9 skipped
    // bit 2: rows 4,5 ok     bit 5: rows 10,11 skipped
    for (int y = 0; y < 6; ++y)
    {
        auto const pos = CellLocation { .line = LineOffset(y), .column = ColumnOffset(0) };
        auto const actualColor = ib.at(pos);
        CHECK(actualColor.rgb() == PinColor);
    }
}

TEST_CASE("SixelParser.explicit_raster_vertical_overflow", "[sixel]")
{
    // An explicit raster shorter than one sixel band must clip, not overflow.
    //
    // setRaster() shrinks the buffer to _size.area()*4 (40 bytes here), but the write guard tests
    // the line against _maxSize.height (480) while the index uses the _size.width stride. A '~'
    // (all six sixel bits) therefore addresses row 5 at byte 5*10*4 = 200 of a 40-byte buffer.
    // Only bit 0 (row 0) is inside the declared raster; bits 1..5 must be dropped.
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x40 };

    auto ib = SixelImageBuilder(ImageSize { Width(640), Height(480) },
                                1,
                                1,
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(1, 1, ImageSize { Width(10), Height(1) });
    ib.setColor(0, PinColor);

    REQUIRE(ib.size() == ImageSize { Width(10), Height(1) });
    REQUIRE(ib.data().size() == 10u * 1u * 4u);

    auto sp = SixelParser { ib };
    sp.parseFragment("~");

    // Storage must not have grown, and only the one row inside the raster may be painted.
    CHECK(ib.data().size() == 10u * 1u * 4u);
    CHECK(ib.at(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }).rgb() == PinColor);
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(1) });
}

TEST_CASE("SixelParser.finalize_is_idempotent", "[sixel]")
{
    // finalize() must be safe to run twice. On the implicit-raster path it compacts rows from the
    // max-canvas stride down to _size.width; re-running it would compact the already-compacted
    // buffer, reading at the wider stride past its end. SixelParser::done() calls finalize()
    // unconditionally, so a second done() reaches this.
    auto ib = SixelImageBuilder(ImageSize { Width(64), Height(64) },
                                1,
                                1,
                                RGBAColor { 0, 0, 0, 0xFF },
                                std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };
    sp.parseFragment("#1;2;100;100;0");
    sp.parseFragment("~~~-~~~");
    sp.done();

    auto const expectedSize = ib.size();
    auto const expectedPixels = ib.data();

    sp.done();

    CHECK(ib.size() == expectedSize);
    CHECK(ib.data() == expectedPixels);
}

TEST_CASE("SixelParser.finalize_compacts_rows", "[sixel]")
{
    // Implicit raster: the builder writes at the max-canvas stride and finalize() must re-lay every
    // row out at _size.width. Colors differ per (band, column) so any stride or row-offset error
    // surfaces as wrong pixel content rather than merely as wasted work.
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr C1 = RGBAColor { 255, 255, 0, 255 };
    auto constexpr C2 = RGBAColor { 0, 255, 255, 255 };
    auto constexpr C3 = RGBAColor { 255, 0, 255, 255 };
    auto constexpr ByIndex = std::array { C1, C2, C3 };

    auto ib = SixelImageBuilder(ImageSize { Width(64), Height(64) },
                                1,
                                1,
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };
    sp.parseFragment("#1;2;100;100;0");
    sp.parseFragment("#2;2;0;100;100");
    sp.parseFragment("#3;2;100;0;100");
    sp.parseFragment("#1~#2~#3~"); // band 0, rows 0..5 : columns 0,1,2 -> C1,C2,C3
    sp.parseFragment("-");
    sp.parseFragment("#3~#2~#1~"); // band 1, rows 6..11: columns 0,1,2 -> C3,C2,C1
    sp.done();

    REQUIRE(ib.size() == ImageSize { Width(3), Height(12) });
    REQUIRE(ib.data().size() == 3u * 12u * 4u); // compacted to exactly the image

    for (auto const y: std::views::iota(0, 12))
        for (auto const x: std::views::iota(0, 3))
        {
            INFO(std::format("x={} y={}", x, y));
            auto const expected =
                y < 6 ? ByIndex.at(static_cast<size_t>(x)) : ByIndex.at(static_cast<size_t>(2 - x));
            CHECK(ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) }) == expected);
        }
}

TEST_CASE("SixelParser.rep_matches_unrolled", "[sixel]")
{
    // The '!' repeat introducer must be exactly equivalent to writing the sixel out N times.
    // Stated as an equivalence rather than against hand-computed pixels, so it keeps pinning the
    // semantics if the repeat path is ever batched.
    auto const build = [](std::string_view input) {
        auto ib = SixelImageBuilder(ImageSize { Width(8), Height(12) },
                                    1,
                                    1,
                                    RGBAColor { 0, 0, 0, 0xFF },
                                    std::make_shared<SixelColorPalette>(16, 256));
        auto sp = SixelParser { ib };
        sp.parseFragment("#1;2;100;100;0");
        sp.parseFragment(input);
        sp.done();
        return std::tuple { ib.size(), ib.sixelCursor(), ib.data() };
    };

    SECTION("exact run")
    {
        CHECK(build("!5~") == build("~~~~~"));
    }
    SECTION("saturating run")
    {
        CHECK(build("!99~") == build("~~~~~~~~"));
    } // cursor stops at width 8
    SECTION("zero run")
    {
        CHECK(build("!0~") == build(""));
    }
    SECTION("bare introducer")
    {
        CHECK(build("!~") == build(""));
    }
    SECTION("blank run")
    {
        CHECK(build("!5?") == build("?????"));
    } // advances but must not widen
    SECTION("run then more")
    {
        CHECK(build("!3~@@") == build("~~~@@"));
    }
    SECTION("run across bands")
    {
        CHECK(build("!3~-!3~") == build("~~~-~~~"));
    }
}
