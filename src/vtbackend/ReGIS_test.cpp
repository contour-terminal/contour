// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/Screen.h>
#include <vtbackend/test_helpers.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

// End-to-end tests for ReGIS (Remote Graphics Instruction Set) driven through the real
// parser -> Screen path via MockTerm. This file grows with each ReGIS phase; the cases below cover
// the Phase 0 wiring: capability advertisement (DA1 feature 3) and XTSMGRAPHICS ReGIS geometry.

TEST_CASE("ReGIS.DeviceAttributes.advertisesRegisGraphics", "[regis]")
{
    // Primary DA (CSI c) must list ReGIS graphics as feature "3", the same way Sixel is feature "4".
    // The delimiters make the match unambiguous: no other feature number contains a bare ";3;".
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[c");
    auto const reply = mock.terminal.peekInput();
    INFO("DA1 reply: " << reply);
    CHECK(reply.find(";3;") != std::string::npos);
}

TEST_CASE("ReGIS.XtSmGraphics.geometry", "[regis][xtsmgraphics]")
{
    // XTSMGRAPHICS item 3 (ReGIS geometry), action 1 (read) must report the fixed VT340 addressing
    // space of 800x480 with a success status (0) -- not the old Failure(3) stub.
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(20) } };
    mock.writeToScreen("\033[?3;1S");
    CHECK(mock.terminal.peekInput() == "\033[?3;0;800;480S"sv);
}

TEST_CASE("ReGIS.EndToEnd.drawCommitsImage", "[regis]")
{
    // A full DCS ReGIS string that draws a line must place an overlay image, so grid cells carry an
    // image fragment. ReGIS is a full-screen plane, so the home cell is covered.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    mock.writeToScreen("\033Pp P[10,10]V[400,300] \033\\");

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.imageFragment() != nullptr);
}

TEST_CASE("ReGIS.EndToEnd.reportOnlyDoesNotCommit", "[regis]")
{
    // A ReGIS string that only reports (draws nothing) sends its reply but places no image.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    mock.writeToScreen("\033PpR(P)\033\\");

    CHECK(mock.terminal.peekInput() == "[0,0]\r"sv);
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.imageFragment() == nullptr);
}

TEST_CASE("ReGIS.EndToEnd.statePersistsAcrossStrings", "[regis]")
{
    // ReGIS state carries across DCS strings: the first sets position, the second draws from it.
    auto mock = MockTerm { PageSize { LineCount(10), ColumnCount(20) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(10), Height(20) });
    mock.writeToScreen("\033PpP[10,10]\033\\");
    mock.writeToScreen("\033PpV[400,300]\033\\");

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    CHECK(cell.imageFragment() != nullptr);
}
