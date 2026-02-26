// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>
#include <vtbackend/MockTerm.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <crispy/escape.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using crispy::escape;

// NOLINTBEGIN(misc-const-correctness)

TEST_CASE("HUD.EnableDisable", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    CHECK_FALSE(terminal.isHudActive());

    // Enable HUD
    mock.writeToScreen("\033[?2035h");
    CHECK(terminal.isHudActive());

    // Disable HUD
    mock.writeToScreen("\033[?2035l");
    CHECK_FALSE(terminal.isHudActive());
}

TEST_CASE("HUD.WritesGoToHudScreen", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Write to primary screen
    mock.writeToScreen("PRIMARY");

    // Enable HUD and write to it
    mock.writeToScreen("\033[?2035h");
    mock.writeToScreen("\033[1;1H");
    mock.writeToScreen("HUD TEXT");

    // Verify primary screen is untouched
    CHECK(terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 7) == "PRIMARY");

    // Verify HUD screen has the HUD content
    CHECK(terminal.hudScreen().grid().lineText(LineOffset(0)).substr(0, 8) == "HUD TEXT");
}

TEST_CASE("HUD.CompositedRendering", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Fill primary with dots
    mock.writeToScreen("\033[1;1H");
    mock.writeToScreen("..........");
    mock.writeToScreen("..........");
    mock.writeToScreen("..........");

    // Enable HUD and write "HUD" at line 2, column 4
    mock.writeToScreen("\033[?2035h");
    mock.writeToScreen("\033[2;4HHUD");

    // Verify composited rendering: HUD text overlays dots
    terminal.breakLoopAndRefreshRenderBuffer();
    terminal.ensureFreshRenderBuffer();
    auto const screenshot = trimmedTextScreenshot(mock);

    CHECK(screenshot.find("HUD") != std::string::npos);
    // First line should be all dots (no HUD content there)
    auto const lines = textScreenshot(terminal);
    CHECK(lines[0] == "..........");
    // Second line: dots then HUD then dots
    CHECK(lines[1].find("HUD") != std::string::npos);
}

TEST_CASE("HUD.TransparentCells", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Write background
    mock.writeToScreen("\033[1;1H");
    mock.writeToScreen("ABCDEFGHIJ");

    // Enable HUD, write only at specific position
    mock.writeToScreen("\033[?2035h");
    mock.writeToScreen("\033[1;5HX");

    // Verify: primary content shows through except at HUD position
    terminal.breakLoopAndRefreshRenderBuffer();
    terminal.ensureFreshRenderBuffer();
    auto const lines = textScreenshot(terminal);

    // Line 0 should show ABCDXFGHIJ (X from HUD replaces E)
    CHECK(lines[0].find("ABCD") != std::string::npos);
    CHECK(lines[0].find("X") != std::string::npos);
}

TEST_CASE("HUD.CleanOnReEnable", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable HUD, write content
    mock.writeToScreen("\033[?2035h");
    mock.writeToScreen("\033[1;1HFIRST");

    // Disable HUD
    mock.writeToScreen("\033[?2035l");

    // Re-enable HUD — should be clean
    mock.writeToScreen("\033[?2035h");

    // HUD grid should be empty (clean state)
    auto const text = mock.terminal.hudScreen().grid().lineText(LineOffset(0));
    // All spaces means the line is clean
    auto const isClean = std::ranges::all_of(text, [](char c) { return c == ' '; });
    CHECK(isClean);
}

TEST_CASE("HUD.PrimaryScreenRestoredAfterDisable", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Write to primary
    mock.writeToScreen("PRIMARY");

    // Enable HUD, write some HUD content
    mock.writeToScreen("\033[?2035h");
    mock.writeToScreen("\033[1;1HHUD ONLY");

    // Disable HUD
    mock.writeToScreen("\033[?2035l");

    // Primary screen should be active and intact
    CHECK_FALSE(terminal.isHudActive());
    CHECK(terminal.isPrimaryScreen());
    CHECK(terminal.primaryScreen().grid().lineText(LineOffset(0)).substr(0, 7) == "PRIMARY");

    // Verify render buffer shows primary content without HUD overlay
    terminal.breakLoopAndRefreshRenderBuffer();
    terminal.ensureFreshRenderBuffer();
    auto const lines = textScreenshot(terminal);
    CHECK(lines[0].find("HUD ONLY") == std::string::npos);
    CHECK(lines[0].find("PRIMARY") != std::string::npos);
}

TEST_CASE("HUD.NotEnabledOnAlternateScreen", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Switch to alternate screen
    mock.writeToScreen("\033[?1049h");
    CHECK(terminal.isAlternateScreen());

    // Try enabling HUD — should be a no-op
    mock.writeToScreen("\033[?2035h");
    CHECK_FALSE(terminal.isHudActive());
}

TEST_CASE("HUD.DisabledOnAlternateScreenSwitch", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Enable HUD on primary
    mock.writeToScreen("\033[?2035h");
    CHECK(terminal.isHudActive());

    // Switch to alternate screen — should auto-disable HUD
    mock.writeToScreen("\033[?1049h");
    CHECK_FALSE(terminal.isHudActive());
    CHECK(terminal.isAlternateScreen());
}

TEST_CASE("HUD.CursorFromHud", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Move primary cursor to known position
    mock.writeToScreen("\033[3;5H");

    // Enable HUD and move HUD cursor
    mock.writeToScreen("\033[?2035h");
    mock.writeToScreen("\033[1;1H");

    // The current screen cursor should be at the HUD position
    auto const cursorPos = terminal.currentScreen().cursor().position;
    CHECK(cursorPos.line == LineOffset(0));
    CHECK(cursorPos.column == ColumnOffset(0));
}

TEST_CASE("HUD.HardResetClearsHud", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Enable HUD
    mock.writeToScreen("\033[?2035h");
    CHECK(terminal.isHudActive());

    // Issue hard reset (RIS)
    mock.writeToScreen("\033c");

    // HUD should be disabled
    CHECK_FALSE(terminal.isHudActive());
    CHECK(terminal.isPrimaryScreen());
}

TEST_CASE("HUD.Resize", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(5), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    // Enable HUD
    mock.writeToScreen("\033[?2035h");
    CHECK(terminal.isHudActive());

    // Resize terminal
    auto const newSize = PageSize { LineCount(8), ColumnCount(15) };
    terminal.resizeScreen(newSize);

    // HUD should still be active with correct size
    CHECK(terminal.isHudActive());
    CHECK(terminal.hudScreen().pageSize() == newSize);
}

TEST_CASE("HUD.DECRQM", "[hud]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };
    auto& terminal = mock.terminal;

    SECTION("reports reset when disabled")
    {
        mock.writeToScreen("\033[?2035$p");
        auto const reply = std::string(terminal.peekInput());
        CHECK(reply == "\033[?2035;2$y");
    }

    SECTION("reports set when enabled")
    {
        mock.writeToScreen("\033[?2035h");
        mock.writeToScreen("\033[?2035$p");
        auto const reply = std::string(terminal.peekInput());
        CHECK(reply == "\033[?2035;1$y");
    }
}

// NOLINTEND(misc-const-correctness)
