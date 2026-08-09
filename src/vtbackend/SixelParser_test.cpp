// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/SixelParser.hpp>

#include <crispy/Times.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <format>
#include <ranges>
#include <string_view>
#include <tuple>

using namespace vtbackend;

namespace
{

constexpr auto OpaqueBlack = RGBAColor { 0, 0, 0, 0xFF };

SixelImageBuilder sixelImageBuilder(ImageSize size, RGBAColor defaultColor)
{
    auto ib = SixelImageBuilder(size, {}, defaultColor, std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(1, 1, size);
    return ib;
}

/// Runs @p input on a fresh 1:1-aspect builder of @p canvas size and finalizes it.
///
/// Deliberately not sixelImageBuilder(), which declares an explicit raster of its own: what these
/// cases pin is what a stream that declares its own raster -- or none -- leaves behind.
/// @param canvas the maximum image size the builder is constructed with.
/// @param input the sixel fragment to parse, without the DCS introducer or ST.
/// @return the finalized builder.
SixelImageBuilder buildOn(ImageSize canvas, std::string_view input)
{
    auto ib = SixelImageBuilder(canvas, {}, OpaqueBlack, std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };
    sp.parseFragment(input);
    sp.done();
    return ib;
}

/// Selects a colour, paints a single pixel with it, and reads that pixel back.
///
/// The colour registers are not observable directly, so what a definition actually meant can only be
/// seen in a pixel it painted.
/// @param colorDefinition a '#' colour-definition fragment, carrying no pixel data of its own.
/// @return the colour the painted pixel carries.
RGBColor paintOnePixelWith(std::string_view colorDefinition)
{
    auto ib = sixelImageBuilder(ImageSize { Width(4), Height(4) }, RGBAColor { 0, 0, 0, 0xFF });
    auto sp = SixelParser { ib };
    sp.parseFragment(colorDefinition);
    sp.parseFragment("@"); // bit 0: paints pixel (0, 0) with the register just selected
    sp.done();
    return ib.at(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }).rgb();
}

// {{{ State table properties
//
// The table is constexpr, so these cost nothing at runtime and fail the build rather than a test
// run. vtparser's equivalent table carries a standing "TODO: verify the above is correct
// (programmatically as much as possible)" and never got these; the point of writing them here is that
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
    // A zero Pan or Pad reads as one, which is what STD 070's guideline for omitted parameters says
    // and what xterm does; `"5;0` is therefore 5:1, not the previous ratio left untouched.
    sp.parseFragment("\"5;0");
    sp.done();
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(0) });
    CHECK(ib.aspectRatio() == 5);
    sp.parseFragment("\"0;5");
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

TEST_CASE("SixelParser.saturates out-of-range HLS color parameters", "[sixel]")
{
    // These parameters come straight off the wire with no bound on their magnitude, and hsl2rgb() of an
    // out-of-range value converts a double far outside 0..255 to uint8_t -- which is undefined, and
    // aborts a sanitizer build on nothing worse than a garbled sixel. Each saturates at the top of the
    // range the VT340 defines for it, exactly as the RGB parameters do.
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto ib = sixelImageBuilder(ImageSize { Width(1), Height(6) }, DefaultColor);
    auto sp = SixelParser { ib };

    // Hue, lightness and saturation each far beyond their range -- the digits alone overflow the
    // parameter accumulator's 32 bits.
    sp.parseFragment("#1;1;4000000000;4000000000;4000000000");
    sp.parseFragment("#1~");
    sp.done();

    // Lightness saturates at 100%, which is white whatever the hue and saturation resolve to.
    CHECK(ib.at(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) })
          == RGBAColor { 255, 255, 255, 255 });
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
        { Width(5), Height(30) }, {}, DefaultColor, std::make_shared<SixelColorPalette>(16, 256));
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

    auto ib = SixelImageBuilder({ Width(3), Height(7) },
                                { .vertical = 2, .horizontal = 1 },
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
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
                                {},
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(1, 1, ImageSize { Width(10), Height(1) });
    ib.setColor(0, PinColor);

    REQUIRE(ib.size() == ImageSize { Width(10), Height(1) });
    REQUIRE(ib.data().size() == static_cast<std::size_t>(10u * 1u * 4u));

    auto sp = SixelParser { ib };
    sp.parseFragment("~");

    // Storage must not have grown, and only the one row inside the raster may be painted.
    CHECK(ib.data().size() == static_cast<std::size_t>(10u * 1u * 4u));
    CHECK(ib.at(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }).rgb() == PinColor);
    CHECK(ib.sixelCursor() == CellLocation { LineOffset(0), ColumnOffset(1) });
}

TEST_CASE("SixelParser.aspect_ratio_bound_is_total", "[sixel]")
{
    // The bound is a pure function of the canvas, so every input it must survive can be stated here
    // rather than smuggled through a builder. Checking at compile time is deliberate: constant
    // evaluation diagnoses undefined behaviour, so a Pan of UINT_MAX failing to compile is exactly
    // the report we want -- the predecessor computed ceil() in float and converted afterwards.
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 4294967295u, .horizontal = 1u }, Height(600)) == 100u);
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 2147483904u, .horizontal = 1u }, Height(600)) == 100u);

    // Zero on either side reads as one, per STD 070's guideline for omitted parameters.
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 0u, .horizontal = 0u }, Height(600)) == 1u);
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 5u, .horizontal = 0u }, Height(600)) == 5u);
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 0u, .horizontal = 5u }, Height(600)) == 1u);

    // Rounding up is exact, where the float division could only approximate it.
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 15u, .horizontal = 2u }, Height(600)) == 8u);
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 16u, .horizontal = 2u }, Height(600)) == 8u);
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 17u, .horizontal = 2u }, Height(600)) == 9u);
    STATIC_CHECK(sixelAspectRatioFrom({ .vertical = 1u, .horizontal = 4u }, Height(600)) == 1u);

    // One sixel row is six pixel rows tall at scale 1, so the bound is the canvas over six: a taller
    // row cannot be displayed on any device. It follows the canvas rather than a constant, which is
    // what lets a 4K display use the whole range it can actually show.
    STATIC_CHECK(maxSixelAspectRatio(Height(600)) == 100u);
    STATIC_CHECK(maxSixelAspectRatio(Height(2160)) == 360u);
    STATIC_CHECK(maxSixelAspectRatio(Height(4320)) == 720u);

    // Never zero, however small the canvas -- a scale of zero would collapse every row computation.
    STATIC_CHECK(maxSixelAspectRatio(Height(4)) == 1u);
    STATIC_CHECK(maxSixelAspectRatio(Height(0)) == 1u);
}

TEST_CASE("SixelParser.raster_aspect_ratio_wraparound_is_clamped", "[sixel]")
{
    // A raster Pan close to UINT_MAX with Pad=1 used to set _aspectRatio to ~2^31 unbounded. The
    // render()/bandRows() bounds check (y + _aspectRatio > canvasHeight) is unsigned arithmetic,
    // so that huge aspect ratio made the check wrap around and pass while the write offset itself
    // (y, still ~2^31) stayed enormous -- a heap write far past the pixel buffer.
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x40 };

    auto ib = SixelImageBuilder(
        { Width(4), Height(600) }, {}, OpaqueBlack, std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(2147483904u, 1, std::nullopt);

    // A CHECK, not a REQUIRE: the write path below is the half of this test that reproduces the
    // heap write, and aborting the case here would skip it whenever the bound is what regressed.
    CHECK(ib.aspectRatio() == maxSixelAspectRatio(Height(600)));

    auto sp = SixelParser { ib };
    ib.setColor(0, PinColor);

    // Bit 1 set ('A' = 63+2): with an unbounded aspect ratio this alone was enough to compute a
    // pointer offset gigabytes past the buffer -- the wrapped bounds check above passed, y stayed
    // ~2^31, and paintBit() wrote there. At the bound it paints an ordinary, in-canvas band: bit 1
    // starts at row 1*100 and covers the 100 rows the scaling asks for.
    sp.parseFragment("A");
    sp.done();

    CHECK(ib.size() == ImageSize { Width(1), Height(200) });
    CHECK(ib.at({ .line = LineOffset(99), .column = ColumnOffset(0) }) == OpaqueBlack);
    CHECK(ib.at({ .line = LineOffset(100), .column = ColumnOffset(0) }).rgb() == PinColor);
    CHECK(ib.at({ .line = LineOffset(199), .column = ColumnOffset(0) }).rgb() == PinColor);
}

TEST_CASE("SixelParser.aspect_ratio_follows_the_canvas", "[sixel]")
{
    // The bound used to be the constant 255, which silently squashed a legal image on any display
    // taller than 255*6 = 1530 pixels. The image canvas is derived from the monitor, so that is
    // every 4K panel: a ratio of 300 needs 1800 pixel rows and fits a 2160-row canvas comfortably.
    CHECK(buildOn({ Width(3840), Height(2160) }, "\"300;1").aspectRatio() == 300u);
    CHECK(buildOn({ Width(3840), Height(2160) }, "\"360;1").aspectRatio() == 360u);

    // And a canvas that cannot show it bounds it, rather than the other way round.
    CHECK(buildOn({ Width(3840), Height(2160) }, "\"361;1").aspectRatio() == 360u);
    CHECK(buildOn({ Width(800), Height(600) }, "\"300;1").aspectRatio() == 100u);
}

TEST_CASE("SixelParser.constructed_aspect_ratio_is_bounded", "[sixel]")
{
    // The constructor is the builder's other writer of the aspect ratio, and it is public API: what
    // setRaster() refuses to accept must not arrive through it either.
    auto const build = [](SixelAspectRatio aspect) {
        return SixelImageBuilder({ Width(64), Height(600) },
                                 aspect,
                                 OpaqueBlack,
                                 std::make_shared<SixelColorPalette>(16, 256))
            .aspectRatio();
    };

    CHECK(build({ .vertical = 2, .horizontal = 1 }) == 2u);
    CHECK(build({ .vertical = 1000000, .horizontal = 1 }) == maxSixelAspectRatio(Height(600)));

    // A zero horizontal used to divide by zero and convert the resulting infinity, which is
    // undefined; it reads as one, matching what setRaster() does with a zero Pad.
    CHECK(build({ .vertical = 5, .horizontal = 0 }) == 5u);
    CHECK(build({ .vertical = 0, .horizontal = 0 }) == 1u);
}

TEST_CASE("SixelParser.finalize_is_idempotent", "[sixel]")
{
    // finalize() must be safe to run twice. On the implicit-raster path it compacts rows from the
    // max-canvas stride down to _size.width; re-running it would compact the already-compacted
    // buffer, reading at the wider stride past its end. SixelParser::done() calls finalize()
    // unconditionally, so a second done() reaches this.
    auto ib = SixelImageBuilder(ImageSize { Width(64), Height(64) },
                                {},
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

    auto ib = SixelImageBuilder(
        ImageSize { Width(64), Height(64) }, {}, DefaultColor, std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };
    sp.parseFragment("#1;2;100;100;0");
    sp.parseFragment("#2;2;0;100;100");
    sp.parseFragment("#3;2;100;0;100");
    sp.parseFragment("#1~#2~#3~"); // band 0, rows 0..5 : columns 0,1,2 -> C1,C2,C3
    sp.parseFragment("-");
    sp.parseFragment("#3~#2~#1~"); // band 1, rows 6..11: columns 0,1,2 -> C3,C2,C1
    sp.done();

    REQUIRE(ib.size() == ImageSize { Width(3), Height(12) });
    REQUIRE(ib.data().size() == static_cast<std::size_t>(3u * 12u * 4u)); // compacted to exactly the image

    for (auto const y: std::views::iota(0, 12))
        for (auto const x: std::views::iota(0, 3))
        {
            INFO(std::format("x={} y={}", x, y));
            auto const expected =
                y < 6 ? ByIndex.at(static_cast<size_t>(x)) : ByIndex.at(static_cast<size_t>(2 - x));
            CHECK(ib.at(CellLocation { .line = LineOffset(y), .column = ColumnOffset(x) }) == expected);
        }
}

TEST_CASE("SixelParser.at_of_an_image_that_never_painted", "[sixel]")
{
    // Storage is allocated lazily, on the first paint. A stream may define colours and never paint,
    // or paint only blank sixels -- the buffer is then still empty while _size holds its constructed
    // 1x1 sentinel, so at() may not index it and may not use _size as a modulus either.
    auto constexpr DefaultColor = RGBAColor { 0x11, 0x22, 0x33, 0xFF };

    auto const* const payload = GENERATE("#0;2;0;0;0", // defines a colour register, paints nothing
                                         "???",        // blank sixels: cursor movement only
                                         "!100?",      // a blank run: likewise
                                         "");          // nothing at all
    INFO(std::format("payload='{}'", payload));

    auto ib = SixelImageBuilder(ImageSize { Width(640), Height(480) },
                                {},
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };
    sp.parseFragment(std::string_view { payload });

    REQUIRE(ib.data().empty());
    CHECK(ib.at(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }) == DefaultColor);
}

TEST_CASE("SixelParser.finalize_keeps_an_explicitly_declared_single_row", "[sixel]")
{
    // `"1;1;100;1` declares a 1-pixel-tall image. finalize() compacted anything whose height read 1
    // down to the sixel cursor's line -- still 0 within the first band -- freeing every pixel and
    // leaving a zero-height image for the renderer to choke on.
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x40 };

    auto ib = SixelImageBuilder(ImageSize { Width(640), Height(480) },
                                {},
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
    ib.setRaster(1, 1, ImageSize { Width(100), Height(1) });
    ib.setColor(0, PinColor);

    auto sp = SixelParser { ib };
    sp.parseFragment("@");
    sp.done();

    CHECK(ib.size() == ImageSize { Width(100), Height(1) });
    CHECK(ib.data().size() == static_cast<std::size_t>(100u * 1u * 4u));
    CHECK(ib.at(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }).rgb() == PinColor);
}

TEST_CASE("SixelParser.finalize_keeps_a_single_painted_row", "[sixel]")
{
    // Without a raster the image grows as it paints, and one painted pixel row is a height of 1 --
    // which is also the constructed sentinel. Compacting on that predicate threw the row away.
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x40 };

    auto ib = SixelImageBuilder(ImageSize { Width(640), Height(480) },
                                {},
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
    ib.setColor(0, PinColor);

    auto sp = SixelParser { ib };
    sp.parseFragment("@"); // bit 0 only: paints pixel row 0 and nothing else
    sp.done();

    CHECK(ib.size() == ImageSize { Width(1), Height(1) });
    CHECK(ib.at(CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) }).rgb() == PinColor);
}

TEST_CASE("SixelParser.finalize_sizes_an_unpainted_image_by_the_cursor", "[sixel]")
{
    // The compaction branch exists for this: nothing painted, so the image is only what the cursor
    // walked over. Asking the storage rather than the height is what still tells this apart from the
    // two cases above.
    auto constexpr DefaultColor = RGBAColor { 0, 0, 0, 0xFF };

    auto ib = SixelImageBuilder(ImageSize { Width(640), Height(480) },
                                {},
                                DefaultColor,
                                std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };
    sp.parseFragment("--"); // two newlines, no pixel data
    sp.done();

    CHECK(ib.size() == ImageSize { Width(1), Height(12) }); // two bands of six rows
}

TEST_CASE("SixelParser.unpainted_image_height_stays_within_the_canvas", "[sixel]")
{
    // Nothing paints here, so finalize() sizes the image from the cursor alone -- the one path that
    // reaches reshape() without going through reserve(), and so the one place nothing else bounds
    // the height to the canvas. It also used to scale the cursor line by the aspect ratio, which
    // counts the ratio twice: newline() advances the line by six rows *times* the ratio already.
    // Together those reported a 58806-row image on a 600-row canvas.
    auto const heightAfter = [&](ImageSize canvas, std::string_view input) {
        return buildOn(canvas, input).size().height;
    };

    // One band of 6*99 rows walked over, and that is the height -- not that number times 99 again.
    CHECK(heightAfter({ Width(800), Height(600) }, "\"99;1-") == Height(594));
    CHECK(heightAfter({ Width(3840), Height(2160) }, "\"99;1-") == Height(594));

    // A ratio the canvas cannot fit a second band of leaves the cursor where it started.
    CHECK(heightAfter({ Width(800), Height(600) }, "\"100;1-") == Height(0));

    // Whatever the stream asks for, the image never claims more rows than the canvas has.
    CHECK(heightAfter({ Width(800), Height(600) }, "\"99;1----------") <= Height(600));
}

TEST_CASE("SixelParser.explicit_raster_height_saturates", "[sixel]")
{
    // Pv and the aspect ratio are both wire-supplied and their product was a 32-bit multiply done
    // before the clamp to the ceiling: Pv = 2^31 at ratio 2 wrapped to exactly zero, and a
    // zero-height canvas made every following sixel fail the row bounds check, so the image
    // vanished entirely instead of being clamped to what the canvas can hold.
    auto constexpr PinColor = RGBColor { 0x10, 0x20, 0x40 };

    auto ib = SixelImageBuilder(
        { Width(800), Height(600) }, {}, OpaqueBlack, std::make_shared<SixelColorPalette>(16, 256));
    auto sp = SixelParser { ib };
    ib.setColor(0, PinColor);
    sp.parseFragment("\"2;1;100;2147483648#0@");
    sp.done();

    CHECK(ib.size() == ImageSize { Width(100), Height(600) });
    CHECK(ib.at({ .line = LineOffset(0), .column = ColumnOffset(0) }).rgb() == PinColor);
}

TEST_CASE("SixelParser.hls_saturation_is_its_own_parameter", "[sixel]")
{
    // `#Pc;1;Ph;Pl;Ps` is hue, lightness, saturation -- three distinct slots. Reading saturation
    // from the lightness slot made Ps unobservable: both streams below painted the same washed-out
    // red regardless of what saturation they asked for.
    //
    // Hue 120 is the red axis (the parser shifts hue by 120 degrees), lightness 50%.
    CHECK(paintOnePixelWith("#0;1;120;50;100") == RGBColor { 0xFF, 0x00, 0x00 }); // saturated -> red
    CHECK(paintOnePixelWith("#0;1;120;50;0") == RGBColor { 0x7F, 0x7F, 0x7F });   // none -> mid grey
}

TEST_CASE("SixelParser.rgb_parameters_saturate", "[sixel]")
{
    // Colour parameters are unclamped on the wire and fold with wraparound, so a stream can hand the
    // 0..100 conversion a value near 2^32. Scaling that through a float landed outside int's range,
    // which is undefined before the conversion's own '% 256' could tame it.
    CHECK(paintOnePixelWith("#0;2;100;0;0") == RGBColor { 0xFF, 0x00, 0x00 });
    CHECK(paintOnePixelWith("#0;2;200;0;0") == RGBColor { 0xFF, 0x00, 0x00 });
    CHECK(paintOnePixelWith("#0;2;99999999999999999999;0;0") == RGBColor { 0xFF, 0x00, 0x00 });
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
                                {},
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
        auto ib = SixelImageBuilder(MaxSize, {}, DefaultColor, palette());
        CHECK(ib.data().empty());
        CHECK(ib.canvasSize() == MaxSize); // bounds still span the full canvas
    }

    SECTION("an explicit raster allocates exactly")
    {
        auto ib = SixelImageBuilder(MaxSize, {}, DefaultColor, palette());
        ib.setRaster(1, 1, ImageSize { Width(20), Height(20) });
        CHECK(ib.data().size() == static_cast<std::size_t>(20u * 20u * 4u));
        CHECK(ib.canvasSize() == ImageSize { Width(20), Height(20) });
    }

    SECTION("an implicit raster grows on demand")
    {
        auto ib = SixelImageBuilder(MaxSize, {}, DefaultColor, palette());
        auto sp = SixelParser { ib };
        sp.parseFragment("#1;2;100;100;0");
        sp.parseFragment("!20~-!20~-!20~");                              // 20 x 18 pixels
        CHECK(ib.data().size() < static_cast<std::size_t>(64u * 1024u)); // peak tracks the image, not MaxSize
        sp.done();
        CHECK(ib.size() == ImageSize { Width(20), Height(18) });
        // The contract Screen::sixelImage() relies on when it moves the buffer into an Image.
        CHECK(ib.data().size() == static_cast<std::size_t>(20u * 18u * 4u));
    }
}

TEST_CASE("SixelParser.run_matches_per_byte", "[sixel]")
{
    // A run of pixel data reaches the sink as one renderRun() rather than N render()s, so the sink
    // can establish once what every column of the run shares. That is only sound if the two agree
    // byte for byte, so this drives parse() per byte as the reference and pass() as the batched
    // path, and compares the whole buffer -- not hand-computed pixels, which would stop pinning the
    // equivalence the moment either side changed.
    auto const build = [](std::string_view input, bool batched, ImageSize canvas, bool explicitRaster) {
        auto ib = SixelImageBuilder(
            canvas, {}, RGBAColor { 0, 0, 0, 0xFF }, std::make_shared<SixelColorPalette>(16, 256));
        if (explicitRaster)
            ib.setRaster(1, 1, canvas);
        auto sp = SixelParser { ib };
        if (batched)
            sp.parseFragment(input); // routes through pass() -> renderRun()
        else
            for (auto const ch: input) // the per-byte reference
                sp.parse(ch);
        sp.done();
        return std::tuple { ib.size(), ib.sixelCursor(), ib.data() };
    };

    auto const agree = [&](std::string_view input, ImageSize canvas, bool explicitRaster) {
        return build(input, true, canvas, explicitRaster) == build(input, false, canvas, explicitRaster);
    };

    auto const canvas = ImageSize { Width(8), Height(12) };

    // Both rasters: the explicit path is what every real encoder emits and takes the batched code,
    // while the implicit path still has to grow the image a column at a time.
    auto const explicitRaster = GENERATE(true, false);
    CAPTURE(explicitRaster);

    SECTION("a run shorter than a band")
    {
        CHECK(agree("#1;2;100;100;0@", canvas, explicitRaster));
        CHECK(agree("#1;2;100;100;0~~~", canvas, explicitRaster));
    }
    SECTION("a run of every bit pattern")
    {
        // 63..126 is the whole data alphabet; this walks all 64 patterns through both paths.
        auto input = std::string { "#1;2;100;100;0" };
        for (auto const ch: std::views::iota(63, 127))
            input += static_cast<char>(ch);
        CHECK(agree(input, ImageSize { Width(80), Height(12) }, explicitRaster));
    }
    SECTION("a run reaching and passing the canvas edge")
    {
        CHECK(agree("#1;2;100;100;0~~~~~~~~", canvas, explicitRaster));     // exactly the width
        CHECK(agree("#1;2;100;100;0~~~~~~~~~~~~", canvas, explicitRaster)); // past it
    }
    SECTION("a run spanning a colour change")
    {
        CHECK(agree("#1;2;100;100;0~~#2;2;0;100;100~~", canvas, explicitRaster));
    }
    SECTION("runs spanning bands and rewinds")
    {
        CHECK(agree("#1;2;100;100;0~~~-~~~", canvas, explicitRaster));
        CHECK(agree("#1;2;100;100;0~~~$#2;2;0;100;100@@", canvas, explicitRaster));
    }
    SECTION("runs interleaved with repeats")
    {
        CHECK(agree("#1;2;100;100;0~!3@~", canvas, explicitRaster));
        CHECK(agree("#1;2;100;100;0!3?~~", canvas, explicitRaster));
    }
    SECTION("a run overhanging the canvas bottom")
    {
        // A canvas height that is not a multiple of the six-row band is what makes bits overhang at
        // all: newline() only advances while line + bandHeight < canvasHeight, so on a 12-high
        // canvas every bit of every reachable band lands inside and nothing is ever clipped.
        auto const shortCanvas = ImageSize { Width(8), Height(9) };

        // Band 0 deliberately paints nothing. A clipped bit that wrongly painted row 0 would
        // otherwise land on pixels band 0 had already set to the very same colour, and the bug
        // would be invisible -- which is exactly what the first case below used to hide.
        CHECK(agree("#1;2;100;100;0---~~~", shortCanvas, explicitRaster));
        // And with band 0 painted, in a different colour and different columns.
        CHECK(agree("#1;2;100;100;0~~~---#2;2;0;100;100!3?~~~", shortCanvas, explicitRaster));
        CHECK(agree("#1;2;100;100;0~~~---~~~", shortCanvas, explicitRaster));
    }
}

TEST_CASE("SixelParser.digit_runs_match_per_byte", "[sixel]")
{
    // pass() folds a run of digits into the current parameter on one dispatch rather than one per
    // digit -- digits are 30% of a sixel stream, and each otherwise paid a full table dispatch to do
    // n = n*10 + d. Pinned as an equivalence against parse() byte by byte, which still folds them one
    // at a time: comparing against hand-computed values would stop pinning it the moment the fold
    // changed. aspectRatio() is in the tuple because a raster's parameters are only visible there.
    auto const build = [](std::string_view input, bool batched) {
        auto const canvas = ImageSize { Width(8), Height(12) };
        auto ib = SixelImageBuilder(
            canvas, {}, RGBAColor { 0, 0, 0, 0xFF }, std::make_shared<SixelColorPalette>(16, 256));
        auto sp = SixelParser { ib };
        if (batched)
            sp.parseFragment(input); // routes through pass() -> foldDigits()
        else
            for (auto const ch: input) // the per-byte reference
                sp.parse(ch);
        sp.done();
        return std::tuple { ib.size(), ib.sixelCursor(), ib.data(), ib.aspectRatio() };
    };
    auto const agree = [&](std::string_view s) {
        return build(s, true) == build(s, false);
    };

    SECTION("multi-digit colour parameters")
    {
        CHECK(agree("#1;2;100;50;25~"));
        CHECK(agree("#255;2;100;100;0~"));
    }
    SECTION("multi-digit repeat count")
    {
        CHECK(agree("#1;2;100;100;0!12~"));
    }
    SECTION("multi-digit raster")
    {
        CHECK(agree("\"12;34;8;12#1;2;100;100;0~"));
    }
    SECTION("leading zeros")
    {
        CHECK(agree("#1;2;007;050;000~"));
    }
    SECTION("a run long enough to overflow wraps the same way")
    {
        // The fold keeps the parameter in a register across the run; per byte it is read back each
        // time. Both are unsigned, so both wrap -- but they must wrap identically.
        CHECK(agree("#1;2;99999999999999999999;100;0~"));
        CHECK(agree("!99999999999999999999~"));
    }
    SECTION("a digit that is not part of a run still parses")
    {
        // ColorIntroducer's first digit transitions into ColorParam, so it is not foldable and must
        // go through the ordinary dispatch. A one-digit parameter is the case that proves it.
        CHECK(agree("#1~"));
        CHECK(agree("#1;2;1;1;1~"));
    }

    SECTION("a parameter split across two calls continues rather than restarts")
    {
        // This is the production path, and the only one that can tell "continue the parameter" from
        // "start it at zero": every fold within a single call begins on a parameter that
        // ResetParams or ParamSeparator has just zeroed, so both read the same. A DCS payload
        // arrives from the PTY in whatever chunks read() returned -- a megabytes-long sixel comes in
        // tens of thousands of them -- so a parameter's digits straddling a call is routine.
        auto const buildSplit = [](std::string_view first, std::string_view second) {
            auto const canvas = ImageSize { Width(8), Height(12) };
            auto ib = SixelImageBuilder(
                canvas, {}, RGBAColor { 0, 0, 0, 0xFF }, std::make_shared<SixelColorPalette>(16, 256));
            auto sp = SixelParser { ib };
            sp.parseFragment(first);
            sp.parseFragment(second);
            sp.done();
            return std::tuple { ib.size(), ib.sixelCursor(), ib.data(), ib.aspectRatio() };
        };

        // Split inside a colour parameter, a repeat count and a raster parameter in turn.
        CHECK(buildSplit("#1;2;10", "0;50;25~") == build("#1;2;100;50;25~", true));
        CHECK(buildSplit("#1;2;100;100;0!1", "2~") == build("#1;2;100;100;0!12~", true));
        CHECK(buildSplit("\"12;3", "4;8;12#1;2;100;100;0~") == build("\"12;34;8;12#1;2;100;100;0~", true));
        // And split at every position of a multi-digit parameter, so no offset is special.
        auto const whole = std::string_view { "#1;2;12345;50;25~" };
        for (auto const at: std::views::iota(size_t { 1 }, whole.size()))
        {
            CAPTURE(at);
            CHECK(buildSplit(whole.substr(0, at), whole.substr(at)) == build(whole, true));
        }
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
                                    {},
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
    //
    // The repeat path *is* batched now: with an explicit raster a repeat paints each set bit as one
    // horizontal fill, which shares no code at all with the per-column render() the unrolled side
    // drives. So the two sides are deliberately driven differently -- the repeat through
    // parseFragment(), the unrolled reference through parse() byte by byte, which is what still
    // reaches render(). Comparing whole buffers rather than hand-computed pixels is what keeps this
    // pinning the equivalence rather than one side's idea of the answer.
    auto const build = [](std::string_view input, bool batched, bool explicitRaster, ImageSize canvas) {
        auto ib = SixelImageBuilder(
            canvas, {}, RGBAColor { 0, 0, 0, 0xFF }, std::make_shared<SixelColorPalette>(16, 256));
        if (explicitRaster)
            ib.setRaster(1, 1, canvas);
        auto sp = SixelParser { ib };
        sp.parseFragment("#1;2;100;100;0");
        if (batched)
            sp.parseFragment(input);
        else
            for (auto const ch: input)
                sp.parse(ch);
        sp.done();
        return std::tuple { ib.size(), ib.sixelCursor(), ib.data() };
    };

    // Both rasters: only the explicit one takes the batched fill, and it is what every real encoder
    // emits -- so without this the fill would be the one path nothing covers.
    auto const explicitRaster = GENERATE(true, false);
    CAPTURE(explicitRaster);

    // Both a canvas height that is a multiple of the six-row band and one that is not. This is not
    // decoration: newline() only advances while line + bandHeight < canvasHeight, so on a 12-high
    // canvas the sixel cursor only ever reaches y0 = 0 or 6, and from either every bit of the band
    // lands inside. The clipping that the batched paths' fitting-bit mask exists for cannot happen
    // there at all -- a 9-high canvas is what makes bits 3..5 of the second band overhang.
    auto const canvasHeight = GENERATE(12, 9);
    CAPTURE(canvasHeight);
    auto const canvas = ImageSize { Width(8), Height(canvasHeight) };

    auto const rep = [&](std::string_view s) {
        return build(s, true, explicitRaster, canvas);
    };
    auto const unrolled = [&](std::string_view s) {
        return build(s, false, explicitRaster, canvas);
    };

    SECTION("exact run")
    {
        CHECK(rep("!5~") == unrolled("~~~~~"));
    }
    SECTION("saturating run")
    {
        CHECK(rep("!99~") == unrolled("~~~~~~~~"));
    } // cursor stops at width 8
    SECTION("zero run")
    {
        CHECK(rep("!0~") == unrolled(""));
    }
    SECTION("bare introducer")
    {
        CHECK(rep("!~") == unrolled(""));
    }
    SECTION("blank run")
    {
        CHECK(rep("!5?") == unrolled("?????"));
    } // advances but must not widen
    SECTION("run then more")
    {
        CHECK(rep("!3~@@") == unrolled("~~~@@"));
    }
    SECTION("a huge run costs no more than the canvas width")
    {
        // The repeat count comes straight off the wire, and repetitions past the canvas edge
        // cannot change the image. Walking them anyway means four billion no-op calls -- a hang
        // from nothing but `cat`-ing a crafted file. This case would not terminate before the fix.
        CHECK(rep("!4294967295~") == unrolled("~~~~~~~~"));
    }

    SECTION("run across bands")
    {
        CHECK(rep("!3~-!3~") == unrolled("~~~-~~~"));
    }

    SECTION("every bit pattern repeats the same way it unrolls")
    {
        // Walks all 64 patterns through the fill, including the ones whose high bits overhang the
        // bottom band and must paint nothing.
        for (auto const ch: std::views::iota(63, 127))
        {
            auto const sixel = static_cast<char>(ch);
            CAPTURE(ch);
            CHECK(rep(std::string { "!4" } + sixel) == unrolled(std::string(4, sixel)));
            CHECK(rep(std::string { "--!4" } + sixel)
                  == unrolled(std::string { "--" } + std::string(4, sixel)));
        }
    }
}
