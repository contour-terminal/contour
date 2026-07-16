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

// {{{ State table properties
//
// The table is constexpr, so these cost nothing at runtime and fail the build rather than a test
// run. vtparser's equivalent table carries a standing "TODO: verify the above is correct
// (programatically as much as possible)" and never got these; the point of writing them here is that
// a mis-tabled cell is otherwise silent.

using Table = SixelParser::Table;
using State = SixelParser::State;
using Action = SixelParser::Action;

constexpr auto TheTable = Table::get();

/// Every real state, i.e. every enumerator past the Undefined sentinel.
constexpr auto realStates() noexcept
{
    return std::views::iota(size_t { 1 }, SixelParser::StateCount)
           | std::views::transform([](size_t s) { return static_cast<State>(s); });
}

/// @return true if every (state, byte) pair names an action.
///
/// This is what lets the dispatcher omit an error branch: there is no hole to fall into.
constexpr bool isTotal() noexcept
{
    for (auto const state: realStates())
        for (auto const ch: std::views::iota(0u, 256u))
            if (TheTable.events[Table::index(state)][ch] == Action::Undefined)
                return false;
    return true;
}

/// @return true if a sixel data byte always paints and always ends up in Ground, from every state.
///
/// This is the property the old fallback()'s "test isSixel first" ordering hand-maintained. As a
/// table it is checkable rather than merely intended.
constexpr bool sixelsAlwaysPaintAndGround() noexcept
{
    for (auto const state: realStates())
    {
        for (auto const ch: std::views::iota(63u, 127u))
        {
            auto const next = TheTable.transitions[Table::index(state)][ch];
            auto const action = TheTable.events[Table::index(state)][ch];
            // Ground stays put (Undefined == no move), which is already being in Ground.
            auto const endsInGround =
                next == State::Ground || (state == State::Ground && next == State::Undefined);
            auto const paints = action == Action::Render || action == Action::RenderRepeated;
            if (!endsInGround || !paints)
                return false;
        }
    }
    return true;
}

/// @return true if every real state is entered by some (state, byte) pair, Ground aside -- it is
/// where the parser starts.
constexpr bool everyStateReachable() noexcept
{
    for (auto const target: realStates())
    {
        if (target == State::Ground)
            continue;
        auto reachable = false;
        for (auto const from: realStates())
            for (auto const ch: std::views::iota(0u, 256u))
                if (TheTable.transitions[Table::index(from)][ch] == target)
                    reachable = true;
        if (!reachable)
            return false;
    }
    return true;
}

static_assert(isTotal(), "every (state, byte) must name an action, or the dispatcher needs an error branch");
static_assert(sixelsAlwaysPaintAndGround(), "a sixel byte must paint and land in Ground from every state");
static_assert(everyStateReachable(), "a state nothing transitions to is dead weight");

// The builder writes Ground -> Ground self-transitions for '$' and '-' rather than in-state events.
// That is only equivalent to firing the action alone because Ground has neither an entry nor an exit
// action -- so pin it here rather than leave it as a silent assumption.
static_assert(TheTable.entryEvents[Table::index(State::Ground)] == Action::Undefined
                  && TheTable.exitEvents[Table::index(State::Ground)] == Action::Undefined,
              "Ground must stay action-free, or its self-transitions would fire side effects");

// The Undefined sentinel row must stay empty: it is what "no entry" means.
static_assert(TheTable.entryEvents[Table::index(State::Undefined)] == Action::Undefined
                  && TheTable.exitEvents[Table::index(State::Undefined)] == Action::Undefined,
              "the Undefined sentinel is not a state and must carry no actions");
// }}}

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

TEST_CASE("SixelParser.sixelAspectVertical", "[sixel]")
{
    STATIC_CHECK(sixelAspectVertical(0) == 2);
    STATIC_CHECK(sixelAspectVertical(1) == 2);
    STATIC_CHECK(sixelAspectVertical(2) == 5);
    STATIC_CHECK(sixelAspectVertical(3) == 3);
    STATIC_CHECK(sixelAspectVertical(4) == 3);
    STATIC_CHECK(sixelAspectVertical(5) == 2);
    STATIC_CHECK(sixelAspectVertical(6) == 2);
    STATIC_CHECK(sixelAspectVertical(7) == 1);
    STATIC_CHECK(sixelAspectVertical(8) == 1);
    STATIC_CHECK(sixelAspectVertical(9) == 1);
    STATIC_CHECK(sixelAspectVertical(10) == 1); // out of range
    STATIC_CHECK(sixelAspectVertical(9999) == 1);
}

TEST_CASE("SixelParser.currentColor_tracks_palette", "[sixel]")
{
    auto constexpr A = RGBColor { 1, 2, 3 };
    auto constexpr B = RGBColor { 4, 5, 6 };
    auto constexpr C = RGBColor { 7, 8, 9 };
    auto ib = SixelImageBuilder(ImageSize { Width(4), Height(6) },
                                1,
                                1,
                                RGBAColor { 0, 0, 0, 0xFF },
                                std::make_shared<SixelColorPalette>(16, 256));

    SECTION("redefining the selected register is visible immediately")
    {
        // No useColor() in between: the ground_* cases paint straight after a bare setColor().
        ib.setColor(0, A);
        CHECK(ib.currentColor() == A);
        ib.setColor(0, B);
        CHECK(ib.currentColor() == B);
    }

    SECTION("useColor selects")
    {
        ib.setColor(3, C);
        ib.useColor(3);
        CHECK(ib.currentColor() == C);
        ib.setColor(2, A); // a different register must not disturb the selection
        CHECK(ib.currentColor() == C);
    }

    SECTION("useColor wraps modulo the palette size")
    {
        ib.setColor(4, A);
        ib.useColor(20); // 20 % 16 == 4
        CHECK(ib.currentColor() == A);
    }
}

TEST_CASE("SixelParser.storage_is_right_sized", "[sixel]")
{
    // Storage must track the image, never the max canvas. The builder is constructed with the
    // terminal's maximum image size -- in practice the whole monitor -- once per sixel sequence,
    // so allocating (and background-filling) that up front costs ~33 MB per image on a 4K display
    // even for a tiny one.
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr MaxSize = ImageSize { Width(3840), Height(2160) }; // 33 MB if allocated eagerly
    auto const palette = [] {
        return std::make_shared<SixelColorPalette>(16, 256);
    };

    SECTION("the constructor allocates nothing")
    {
        auto ib = SixelImageBuilder(MaxSize, 1, 1, DefaultColor, palette());
        CHECK(ib.data().empty());
        CHECK(ib.canvasSize() == MaxSize); // bounds still span the full canvas
    }

    SECTION("an explicit raster allocates exactly")
    {
        auto ib = SixelImageBuilder(MaxSize, 1, 1, DefaultColor, palette());
        ib.setRaster(1, 1, ImageSize { Width(20), Height(20) });
        CHECK(ib.data().size() == 20u * 20u * 4u);
        CHECK(ib.canvasSize() == ImageSize { Width(20), Height(20) });
    }

    SECTION("an implicit raster grows on demand")
    {
        auto ib = SixelImageBuilder(MaxSize, 1, 1, DefaultColor, palette());
        auto sp = SixelParser { ib };
        sp.parseFragment("#1;2;100;100;0");
        sp.parseFragment("!20~-!20~-!20~");    // 20 x 18 pixels
        CHECK(ib.data().size() < 64u * 1024u); // peak tracks the image, not MaxSize
        sp.done();
        CHECK(ib.size() == ImageSize { Width(20), Height(18) });
        // The contract Screen::sixelImage() relies on when it moves the buffer into an Image.
        CHECK(ib.data().size() == 20u * 18u * 4u);
    }
}

TEST_CASE("SixelParser.param_count_saturates", "[sixel]")
{
    // The parameter list is a fixed array whose count saturates one past the five any sixel command
    // distinguishes. That is only safe because every reader tests the count against a value of at
    // most five, so six must answer identically to seven, or to the thousands an untrusted stream
    // can send. Stated as equivalences rather than against hand-computed pixels: what matters is
    // that a saturated list decides the same way a longer one would.
    auto const build = [](std::string_view input) {
        auto ib = SixelImageBuilder(ImageSize { Width(8), Height(12) },
                                    1,
                                    1,
                                    RGBAColor { 0, 0, 0, 0xFF },
                                    std::make_shared<SixelColorPalette>(16, 256));
        auto sp = SixelParser { ib };
        sp.parseFragment(input);
        sp.done();
        return std::tuple { ib.size(), ib.sixelCursor(), ib.data() };
    };

    SECTION("a colour past five parameters defines nothing, however far past")
    {
        // Six parameters is not a colour definition, so it must leave the painting colour exactly as
        // if the command had never been sent. This is the assertion that pins the saturation point:
        // comparing six against seven would pass even if the count saturated at five, because both
        // would then define the same wrong colour. Only "defines nothing" tells the two apart.
        CHECK(build("#1;2;100;0;0;7~") == build("~"));
        CHECK(build("#1;2;100;0;0;7;7;7;7~") == build("~"));
    }

    SECTION("an untrusted flood of separators cannot corrupt the decision")
    {
        auto flood = std::string { "#1;2;100;0;0" };
        for ([[maybe_unused]] auto const i: std::views::iota(0, 5000))
            flood += ";7";
        flood += "~";
        CHECK(build(flood) == build("~"));
    }

    SECTION("the counts that do mean something still do")
    {
        // Guards the saturation from swallowing the real cases: five parameters define a colour, so
        // this pair must differ. Note the raster predicate (> 1 && < 5) cannot distinguish where the
        // count saturates -- five and six both fail it -- so only the colour cases above pin that.
        CHECK(build("#1;2;100;0;0~") != build("~"));
        CHECK(build("\"1;1;8;12#1;2;100;0;0~") != build("#1;2;100;0;0~"));
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
    SECTION("a huge run costs no more than the canvas width")
    {
        // The repeat count comes straight off the wire, and repetitions past the canvas edge
        // cannot change the image. Walking them anyway means four billion no-op calls -- a hang
        // from nothing but `cat`-ing a crafted file. This case would not terminate before the fix.
        CHECK(build("!4294967295~") == build("~~~~~~~~"));
    }

    SECTION("run across bands")
    {
        CHECK(build("!3~-!3~") == build("~~~-~~~"));
    }
}
