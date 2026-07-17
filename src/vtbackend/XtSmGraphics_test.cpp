// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/Screen.h>
#include <vtbackend/test_helpers.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

// XTSMGRAPHICS: CSI ? Pi ; Pa ; Pv S
//
//   Pi = 1 number of colour registers, 2 sixel geometry, 3 ReGIS geometry
//   Pa = 1 read, 2 reset to default, 3 set to value, 4 read maximum
//
// Every action must answer: an application that issues one and waits for the reply hangs otherwise,
// which is what makes the silent cases below defects rather than omissions.

namespace
{

/// Gives the terminal 10x10 pixel cells, so pixel geometry stays legible in the replies, and the
/// canvas ceiling the case under test needs. MockTerm is neither copyable nor movable, so this
/// configures in place rather than returning one.
template <typename Mock>
void configure(Mock& mock, ImageSize ceiling)
{
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(10) });
    mock.terminal.setImageCanvasCeiling(ceiling);
}

} // namespace

TEST_CASE("XtSmGraphics.SixelGeometry.Read", "[screen][xtsmgraphics]")
{
    SECTION("excludes the status line")
    {
        // The reply must describe the area images are actually painted into. Deriving it from
        // totalPageSize() -- which spans the indicator status line -- advertises one row that does
        // not exist, so an application sizing a canvas from the reply overshoots the grid.
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
        configure(mock, ImageSize { Width(1920), Height(1080) });
        mock.terminal.setStatusDisplay(StatusDisplayType::Indicator);

        mock.writeToScreen("\033[?2;1S");

        // 11 columns x 10px; 5 total lines minus the status line = 4 paintable rows x 10px.
        CHECK(mock.terminal.peekInput() == "\033[?2;0;110;40S"sv);
    }

    SECTION("spans the whole page when no status line is shown")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
        configure(mock, ImageSize { Width(1920), Height(1080) });

        mock.writeToScreen("\033[?2;1S");

        CHECK(mock.terminal.peekInput() == "\033[?2;0;110;50S"sv);
    }

    SECTION("is clamped by the ceiling on both axes")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
        configure(mock, ImageSize { Width(64), Height(16) });

        mock.writeToScreen("\033[?2;1S");

        CHECK(mock.terminal.peekInput() == "\033[?2;0;64;16S"sv);
    }
}

TEST_CASE("XtSmGraphics.SixelGeometry.ReadLimit reports the ceiling", "[screen][xtsmgraphics]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
    configure(mock, ImageSize { Width(1920), Height(1080) });

    mock.writeToScreen("\033[?2;4S");

    CHECK(mock.terminal.peekInput() == "\033[?2;0;1920;1080S"sv);
}

TEST_CASE("XtSmGraphics.SixelGeometry.SetToValue", "[screen][xtsmgraphics]")
{
    SECTION("clamps each axis independently")
    {
        // A lexicographic ordering compares the width first and, finding it smaller, returns the
        // requested size whole -- never clamping the height at all.
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
        configure(mock, ImageSize { Width(1920), Height(1080) });

        mock.writeToScreen("\033[?2;3;100;999999S");

        CHECK(mock.terminal.peekInput() == "\033[?2;0;100;1080S"sv);
        CHECK(mock.terminal.maxImageSize() == ImageSize { Width(100), Height(1080) });
    }

    SECTION("honours a value below the ceiling")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
        configure(mock, ImageSize { Width(1920), Height(1080) });

        mock.writeToScreen("\033[?2;3;640;480S");

        CHECK(mock.terminal.peekInput() == "\033[?2;0;640;480S"sv);
        CHECK(mock.terminal.maxImageSize() == ImageSize { Width(640), Height(480) });
    }

    SECTION("never raises the effective size above the ceiling")
    {
        auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
        configure(mock, ImageSize { Width(320), Height(200) });

        mock.writeToScreen("\033[?2;3;4096;4096S");

        CHECK(mock.terminal.maxImageSize() == ImageSize { Width(320), Height(200) });
    }
}

TEST_CASE("XtSmGraphics.SixelGeometry.ResetToDefault replies and restores the ceiling",
          "[screen][xtsmgraphics]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
    configure(mock, ImageSize { Width(1920), Height(1080) });

    mock.writeToScreen("\033[?2;3;640;480S");
    REQUIRE(mock.terminal.maxImageSize() == ImageSize { Width(640), Height(480) });

    mock.writeToScreen("\033[?2;2S");

    CHECK(mock.terminal.maxImageSize() == ImageSize { Width(1920), Height(1080) });
    // Replies accumulate; the reset must have produced one at all -- staying silent hangs the caller.
    CHECK(mock.terminal.peekInput() == "\033[?2;0;640;480S\033[?2;0;1920;1080S"sv);
}

TEST_CASE("XtSmGraphics.SixelGeometry.the ceiling drives the canvas until an application negotiates",
          "[screen][xtsmgraphics]")
{
    // The frontend sets the ceiling after construction, so the canvas must follow it rather than sit
    // at whatever the settings were born with.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
    configure(mock, ImageSize { Width(1920), Height(1080) });
    CHECK(mock.terminal.maxImageSize() == ImageSize { Width(1920), Height(1080) });

    mock.terminal.setImageCanvasCeiling(ImageSize { Width(800), Height(600) });
    CHECK(mock.terminal.maxImageSize() == ImageSize { Width(800), Height(600) });
}

TEST_CASE("XtSmGraphics.SixelGeometry.a ceiling change keeps what an application negotiated",
          "[screen][xtsmgraphics]")
{
    // setImageCanvasCeiling() fires whenever the display changes -- a window dragged to another
    // monitor, a session re-attached to a display on a tab switch or split rebuild. Resetting the
    // effective size there raised the canvas back to the full monitor behind the application's back,
    // so images were accepted at a size it never asked for and a later read replied with a value
    // contradicting the one it had cached.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
    configure(mock, ImageSize { Width(1920), Height(1080) });

    mock.writeToScreen("\033[?2;3;640;480S");
    REQUIRE(mock.terminal.maxImageSize() == ImageSize { Width(640), Height(480) });

    SECTION("a larger ceiling leaves it alone")
    {
        mock.terminal.setImageCanvasCeiling(ImageSize { Width(3840), Height(2160) });
        CHECK(mock.terminal.maxImageSize() == ImageSize { Width(640), Height(480) });
    }

    SECTION("a smaller ceiling still caps it")
    {
        mock.terminal.setImageCanvasCeiling(ImageSize { Width(320), Height(200) });
        CHECK(mock.terminal.maxImageSize() == ImageSize { Width(320), Height(200) });
    }

    SECTION("a ceiling that shrinks and grows again restores what was asked for")
    {
        // The request is the application's standing wish and the ceiling only ever caps it. Keeping
        // the clamped result instead would make a ceiling that happened to be small at the moment
        // the request arrived permanent.
        mock.terminal.setImageCanvasCeiling(ImageSize { Width(320), Height(200) });
        mock.terminal.setImageCanvasCeiling(ImageSize { Width(1920), Height(1080) });
        CHECK(mock.terminal.maxImageSize() == ImageSize { Width(640), Height(480) });
    }

    SECTION("a reset returns to following the ceiling, not to today's ceiling")
    {
        mock.writeToScreen("\033[?2;2S");
        mock.terminal.setImageCanvasCeiling(ImageSize { Width(3840), Height(2160) });
        CHECK(mock.terminal.maxImageSize() == ImageSize { Width(3840), Height(2160) });
    }
}

TEST_CASE("XtSmGraphics.ReGISGeometry always answers failure", "[screen][xtsmgraphics]")
{
    // Contour implements no ReGIS. Failure is the honest answer; silence hangs the caller.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(11) } };
    configure(mock, ImageSize { Width(1920), Height(1080) });

    SECTION("read")
    {
        mock.writeToScreen("\033[?3;1S");
    }
    SECTION("reset to default")
    {
        mock.writeToScreen("\033[?3;2S");
    }
    SECTION("set to value")
    {
        mock.writeToScreen("\033[?3;3;10;10S");
    }
    SECTION("read limit")
    {
        mock.writeToScreen("\033[?3;4S");
    }

    CHECK(mock.terminal.peekInput() == "\033[?3;3;0S"sv);
}
