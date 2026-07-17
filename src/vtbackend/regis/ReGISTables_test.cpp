// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISTables.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace vtbackend::regis;

// The ReGIS grammar tables are data, so their shape is asserted at compile time: a wrong entry is a
// build break, not a runtime surprise.

static_assert(commandOf('P') == Command::Position);
static_assert(commandOf('p') == Command::Position, "command letters are case-insensitive");
static_assert(commandOf('V') == Command::Vector);
static_assert(commandOf('C') == Command::Curve);
static_assert(commandOf('W') == Command::Write);
static_assert(commandOf('T') == Command::Text);
static_assert(commandOf('S') == Command::ScreenCmd);
static_assert(commandOf('F') == Command::Fill);
static_assert(commandOf('L') == Command::Load);
static_assert(commandOf('R') == Command::Report);
static_assert(commandOf('@') == Command::Macrograph);
static_assert(commandOf('X') == Command::None, "an unassigned letter is not a command");
static_assert(commandOf(' ') == Command::None);

static_assert(PixelVectorDelta[0].dx == +1 && PixelVectorDelta[0].dy == 0, "0 = right");
static_assert(PixelVectorDelta[2].dx == 0 && PixelVectorDelta[2].dy == -1, "2 = up (y grows down)");
static_assert(PixelVectorDelta[4].dx == -1 && PixelVectorDelta[4].dy == 0, "4 = left");
static_assert(PixelVectorDelta[6].dx == 0 && PixelVectorDelta[6].dy == +1, "6 = down");

static_assert(StandardPattern[0] == 0x00, "pattern 0 draws nothing");
static_assert(StandardPattern[1] == 0xff, "pattern 1 is solid");
static_assert(DefaultPattern == 0xff);

static_assert(StandardTextSize[0].width == 9 && StandardTextSize[0].height == 10, "size 0 = 9x10");
static_assert(StandardTextSize[16].width == 144 && StandardTextSize[16].height == 240, "size 16 = 144x240");

static_assert(NamedColors[0].letter == 'D' && NamedColors[0].rgb == RGBColor { 0, 0, 0 });
static_assert(NamedColors[7].letter == 'W' && NamedColors[7].rgb == RGBColor { 255, 255, 255 });

TEST_CASE("ReGISTables.commandDispatch", "[regis][tables]")
{
    // A runtime mirror of the static asserts, for a readable failure message if the table regresses.
    CHECK(commandOf('W') == Command::Write);
    CHECK(commandOf('s') == Command::ScreenCmd);
    CHECK(commandOf('?') == Command::None);
}
