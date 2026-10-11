// SPDX-License-Identifier: Apache-2.0
#include <contour/session/ScreenshotFile.hpp>

#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <ranges>

using namespace vtbackend;

TEST_CASE("ScreenshotFile.replaysOnItsOwnWhateverTheTerminalIsDoing", "[session]")
{
    // A screenshot file is replayed by whatever terminal it is printed on, in whatever state that is in:
    // larger than the page captured and full of text, a coloured pen, line graphics in G0, origin mode
    // inside a scroll region, the cursor anywhere. It must draw the captured page all the same, at the
    // top-left, and nothing else.
    auto const pageSize = PageSize { LineCount(4), ColumnCount(6) };
    auto mock = MockTerm { pageSize };
    mock.writeToScreen("ab\r\ncdef\r\n\r\nxyz");
    auto const& screen = mock.terminal.primaryScreen();

    auto const replaySize = PageSize { LineCount(5), ColumnCount(8) };
    auto replay = MockTerm { replaySize };
    replay.writeToScreen("12345678\r\n12345678\r\n12345678\r\n12345678\r\n12345678" // text everywhere
                         "\033[41m"                                                 // a red background
                         "\033(0"                                                   // DEC line graphics in G0
                         "\033[2;3r\033[?6h" // origin mode in a region of rows 2-3
                         "\033[2;4H");       // the cursor somewhere
    replay.writeToScreen(contour::session::standaloneScreenshot(screen));
    auto const& replayed = replay.terminal.primaryScreen();
    // Row 2 of the captured page is blank, on the default background.
    auto const defaultBackground =
        screen.at(CellLocation { LineOffset(2), ColumnOffset(0) }).backgroundColor();

    for (auto const line: std::views::iota(0, unbox(replaySize.lines)))
    {
        INFO(std::format("line {}", line));
        auto const onPage = line < unbox(pageSize.lines);
        auto const text = replayed.grid().lineAt(LineOffset(line)).toUtf8ColumnAligned();
        if (onPage)
            CHECK(text.starts_with(screen.grid().lineAt(LineOffset(line)).toUtf8ColumnAligned()));
        CHECK(text.find_first_not_of(' ', onPage ? unbox<size_t>(pageSize.columns) : 0) == std::string::npos);
        for (auto const column: std::views::iota(0, unbox(replaySize.columns)))
        {
            INFO(std::format("column {}", column));
            auto const at = CellLocation { LineOffset(line), ColumnOffset(column) };
            CHECK(replayed.at(at).backgroundColor() == defaultBackground);
        }
    }
}

TEST_CASE("ScreenshotFile.leavesTheScreenshotItselfAlone", "[session]")
{
    // The state dump prints Screen::screenshot() among its own lines, so the prefix belongs to the file
    // alone.
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(4) } };
    mock.writeToScreen("ab");
    auto const& screen = mock.terminal.primaryScreen();
    CHECK(contour::session::standaloneScreenshot(screen)
          == std::string(contour::session::StandaloneScreenshotPrefix) + screen.screenshot());
    CHECK_FALSE(screen.screenshot().contains("\033[2J"));
}
