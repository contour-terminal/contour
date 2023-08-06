/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vtbackend/SixelParser.h>

#include <crispy/times.h>

#include <catch2/catch.hpp>

#include <array>
#include <string_view>

using namespace terminal;

namespace
{

SixelImageBuilder sixelImageBuilder(image_size size, rgba_color defaultColor)
{
    auto ib = SixelImageBuilder(size, 1, 1, defaultColor, std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(1, 1, size);
    return ib;
}

} // namespace

TEST_CASE("SixelParser.ground_000000", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr pinColor = rgb_color { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(image_size { width(4), height(10) }, defaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == cell_location { {}, {} });

    ib.setColor(0, pinColor);
    sp.parseFragment("?");

    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            auto const& actualColor = ib.at(cell_location { line_offset(y), column_offset(x) });
            CHECK(actualColor == defaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_111111", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0, 0, 0, 0xFF };
    auto constexpr pinColor = rgb_color { 0x10, 0x20, 0x40 };

    auto ib = sixelImageBuilder(image_size { width(2), height(8) }, defaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });

    ib.setColor(0, pinColor);

    sp.parseFragment("~"); // 0b111111 + 63 == 126 == '~'

    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            auto const& actualColor = ib.at(cell_location { line_offset(y), column_offset(x) });
            auto const pinned = x == 0 && y >= 0 && y <= 5;
            INFO(fmt::format("x={}, y={}, {}", x, y, pinned ? "pinned" : ""));
            if (pinned)
                CHECK(actualColor.rgb() == pinColor);
            else
                CHECK(actualColor == defaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_000001", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr pinColor = rgb_color { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(image_size { width(4), height(10) }, defaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });

    ib.setColor(0, pinColor);

    sp.parseFragment("@");

    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            INFO(fmt::format("x={}, y={}", x, y));
            auto const& actualColor = ib.at(cell_location { line_offset(y), column_offset(x) });
            auto const pinned = x == 0 && y == 0;
            if (pinned)
                CHECK(actualColor.rgb() == pinColor);
            else
                CHECK(actualColor == defaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_010101", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr pinColor = rgb_color { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(image_size { width(2), height(8) }, defaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });

    ib.setColor(0, pinColor);

    sp.parseFragment("T"); // 0b010101 + 63 == 'T'

    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            INFO(fmt::format("x={}, y={}", x, y));
            auto const& actualColor = ib.at(cell_location { line_offset(y), column_offset(x) });
            auto const pinned = x == 0 && (y < 6 && y % 2 == 0);
            if (pinned)
                CHECK(actualColor.rgb() == pinColor);
            else
                CHECK(actualColor == defaultColor);
        }
    }
}

TEST_CASE("SixelParser.ground_101010", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0x10, 0x20, 0x30, 0xFF };
    auto constexpr pinColor = rgb_color { 0xFF, 0xFF, 0x42 };

    auto ib = sixelImageBuilder(image_size { width(2), height(8) }, defaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });

    ib.setColor(0, pinColor);

    sp.parseFragment("i"); // 0b101010 + 63 == 'i'

    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(1) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            INFO(fmt::format("x={}, y={}", x, y));
            auto const& actualColor = ib.at(cell_location { line_offset(y), column_offset(x) });
            auto const pinned = x == 0 && (y < 6 && y % 2 != 0);
            if (pinned)
                CHECK(actualColor.rgb() == pinColor);
            else
                CHECK(actualColor == defaultColor);
        }
    }
}

TEST_CASE("SixelParser.raster", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0, 0, 0, 0xFF };
    auto ib = sixelImageBuilder(image_size { width(640), height(480) }, defaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });

    sp.parseFragment("\"12;34;32;24");
    sp.done();

    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });
    CHECK(ib.aspectRatio() == 1);
    CHECK(*ib.size().width == 32);
    CHECK(*ib.size().height == 24);
    sp.parseFragment("\"12;34");
    sp.done();
    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"");
    sp.done();
    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"0;0");
    sp.done();
    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"5;0");
    sp.done();
    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });
    CHECK(ib.aspectRatio() == 1);
    sp.parseFragment("\"15;2");
    sp.done();
    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });
    CHECK(ib.aspectRatio() == 8);
}

TEST_CASE("SixelParser.rep", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0, 0, 0, 0xFF };
    auto constexpr pinColor = rgb_color { 0x10, 0x20, 0x30 };
    auto ib = sixelImageBuilder(image_size { width(14), height(8) }, defaultColor);
    auto sp = SixelParser { ib };

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(0) });

    ib.setColor(0, pinColor);

    sp.parseFragment("!12~");

    CHECK(ib.sixelCursor() == cell_location { line_offset(0), column_offset(12) });

    for (int x = 0; x < ib.size().width.as<int>(); ++x)
    {
        for (int y = 0; y < ib.size().height.as<int>(); ++y)
        {
            auto const& actualColor = ib.at(cell_location { line_offset(y), column_offset(x) });
            auto const pinned = x < 12 && y < 6;
            if (pinned)
                CHECK(actualColor.rgb() == pinColor);
            else
                CHECK(actualColor == defaultColor);
        }
    }
}

TEST_CASE("SixelParser.setAndUseColor", "[sixel]")
{
    auto constexpr pinColors = std::array<rgba_color, 5> { rgba_color { 255, 255, 255, 255 },
                                                           rgba_color { 255, 0, 0, 255 },
                                                           rgba_color { 0, 255, 0, 255 },
                                                           rgba_color { 0, 0, 255, 255 },
                                                           rgba_color { 255, 255, 255, 255 } };

    auto constexpr defaultColor = rgba_color { 0, 0, 0, 0xFF };
    auto ib = sixelImageBuilder(image_size { width(5), height(6) }, defaultColor);
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

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(5) });

    for (auto const [x, y]:
         crispy::times(ib.size().width.as<int>()) * crispy::times(ib.size().height.as<int>()))
    {
        auto const& expectedColor =
            x < 5 && y < 6 ? pinColors.at(static_cast<size_t>(x ? x : 4)) : defaultColor;
        auto const& actualColor = ib.at(cell_location { line_offset(y), column_offset(x) });
        // INFO(fmt::format("at {}, expect {}, actual {}",
        //                  CellLocation { LineOffset(y), ColumnOffset(x) },
        //                  expectedColor,
        //                  actualColor));
        CHECK(actualColor == expectedColor);
    }
}

TEST_CASE("SixelParser.rewind", "[sixel]")
{
    auto constexpr pinColors = std::array<rgba_color, 4> {
        rgba_color { 0, 0, 0, 255 },
        rgba_color { 255, 255, 0, 255 },
        rgba_color { 0, 255, 255, 255 },
    };

    auto constexpr defaultColor = pinColors[0];
    auto ib = sixelImageBuilder(image_size { width(4), height(6) }, defaultColor);
    auto sp = SixelParser { ib };

    sp.parseFragment("#1;2;100;100;0");
    sp.parseFragment("#2;2;0;100;100");

    sp.parseFragment("#1~~~~"); // 4 sixels in color #1
    sp.parseFragment("$");      // rewind
    sp.parseFragment("#2~~");   // 2 sixels in color #2
    sp.done();

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(0), column_offset(2) });

    for (int y = 0; y < 6; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            auto const expectedColor = x < 2 ? pinColors[2] : pinColors[1];
            auto const pos = cell_location { line_offset(y), column_offset(x) };
            auto const actualColor = ib.at(pos);

            CHECK(actualColor == expectedColor);
        }
    }
}

TEST_CASE("SixelParser.newline", "[sixel]")
{
    auto constexpr pinColors = std::array<rgba_color, 4> {
        rgba_color { 0, 0, 0, 255 },
        rgba_color { 255, 255, 0, 255 },
        rgba_color { 0, 255, 255, 255 },
    };

    auto constexpr defaultColor = pinColors[0];
    auto ib = sixelImageBuilder(image_size { width(5), height(13) }, defaultColor);
    auto sp = SixelParser { ib };

    sp.parseFragment("#1;2;100;100;0");
    sp.parseFragment("#2;2;0;100;100");

    sp.parseFragment("#1~~~~"); // 4 sixels in color #1
    sp.parseFragment("-");      // newline
    sp.parseFragment("#2~~~~"); // 4 sixels in color #2
    sp.done();

    REQUIRE(ib.sixelCursor() == cell_location { line_offset(6), column_offset(4) });

    for (int y = 0; y < ib.size().height.as<int>(); ++y)
    {
        for (int x = 0; x < ib.size().width.as<int>(); ++x)
        {
            auto const expectedColor = y < 6 && x < 4    ? pinColors[1]
                                       : y < 12 && x < 4 ? pinColors[2]
                                                         : pinColors[0];
            auto const pos = cell_location { line_offset(y), column_offset(x) };
            auto const actualColor = ib.at(pos);

            CHECK(actualColor == expectedColor);
        }
    }
}

TEST_CASE("SixelParser.vertical_cursor_advance", "[sixel]")
{
    auto constexpr defaultColor = rgba_color { 0, 0, 0, 255 };
    SixelImageBuilder ib(
        { width(5), height(30) }, 1, 1, defaultColor, std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };

    sp.parseFragment("$-$-$-$-");
    sp.done();

    REQUIRE(ib.size() == terminal::image_size { width(1), height(24) });
    REQUIRE(ib.sixelCursor() == cell_location { line_offset(24), column_offset { 0 } });
}
