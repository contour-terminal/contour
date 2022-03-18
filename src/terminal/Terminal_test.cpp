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
#include <terminal/Terminal.h>
#include <terminal/pty/MockPty.h>

#include <crispy/times.h>

#include <unicode/convert.h>

#include <catch2/catch.hpp>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace std;
using terminal::ColumnCount;
using terminal::ColumnOffset;
using terminal::LineCount;
using terminal::LineOffset;
using terminal::PageSize;

namespace
{
/// Takes a textual screenshot using the terminals render buffer.
vector<string> textScreenshot(terminal::Terminal const& _terminal)
{
    terminal::RenderBufferRef renderBuffer = _terminal.renderBuffer();

    vector<string> lines;
    lines.resize(_terminal.pageSize().lines.as<size_t>());

    terminal::CellLocation lastPos = {};
    size_t lastCount = 0;
    for (terminal::RenderCell const& cell: renderBuffer.buffer.screen)
    {
        auto const gap = (cell.position.column + static_cast<int>(lastCount) - 1) - lastPos.column;
        auto& currentLine = lines.at(unbox<size_t>(cell.position.line));
        if (*gap > 0) // Did we jump?
            currentLine.insert(currentLine.end(), unbox<size_t>(gap) - 1, ' ');

        currentLine += unicode::convert_to<char>(u32string_view(cell.codepoints));
        lastPos = cell.position;
        lastCount = 1;
    }

    return lines;
}

string join(vector<string> const& _lines)
{
    string output;
    for (string const& line: _lines)
    {
        output += line;
        output += '\n';
    }
    return output;
}

string trimRight(string _text)
{
    constexpr auto Whitespaces = "\x20\t\r\n"sv;
    while (!_text.empty() && Whitespaces.find(_text.back()) != Whitespaces.npos)
        _text.resize(_text.size() - 1);
    return _text;
}

class MockTerm: public terminal::Terminal::Events
{
  public:
    MockTerm(ColumnCount _columns, LineCount _lines): MockTerm { PageSize { _lines, _columns } } {}

    explicit MockTerm(PageSize _size):
        terminal_ {
            make_unique<terminal::MockPty>(_size),
            1024,
            *this,
            LineCount(1024), // max history line count
            LineOffset(0),
            chrono::milliseconds(500),          // cursor blink interval
            chrono::steady_clock::time_point(), // initial time point
        },
        pty_ { static_cast<terminal::MockPty&>(terminal_.device()) }
    {
    }

    terminal::MockPty& pty() noexcept { return pty_; }
    terminal::Terminal& terminal() noexcept { return terminal_; }
    terminal::Terminal const& terminal() const noexcept { return terminal_; }

    void writeToStdin(std::string_view _text) { pty_.stdinBuffer() += _text; }

    void writeToStdout(std::string_view _text)
    {
        pty_.appendStdOutBuffer(_text);
        terminal_.processInputOnce();
    }

    void logScreenText(std::string const& headline = "")
    {
        if (headline.empty())
            UNSCOPED_INFO("dump:");
        else
            UNSCOPED_INFO(headline + ":");

        for (int line = 1; line <= unbox<int>(terminal().screen().pageSize().lines); ++line)
            UNSCOPED_INFO(fmt::format(
                "[{}] \"{}\"", line, terminal().screen().grid().lineText(terminal::LineOffset(line))));
    }

  private:
    terminal::Terminal terminal_;
    terminal::MockPty& pty_;
};

std::string trimmedTextScreenshot(MockTerm const& _mt)
{
    return trimRight(join(textScreenshot(_mt.terminal())));
}
} // namespace

// TODO: Test case posibilities:
//
// - [x] Synchronized output (?2026)
// - [x] Blinking cursor visiblity over time and on input events
// - [ ] double click word selection
// - [ ] tripple click line selection
// - [ ] rectangular block selection
// - [ ] text selection with bypassing enabled application mouse protocol
// - [ ] extractLastMarkRange
// - [ ] scroll mark up
// - [ ] scroll mark down

// TODO: Writing text, leading to page-scroll properly updates viewport.
// TODO: Writing text, leading to page-scroll properly updates active selection.

TEST_CASE("Terminal.BlinkingCursor", "[terminal]")
{
    auto mc = MockTerm { ColumnCount { 6 }, LineCount { 4 } };
    auto& terminal = mc.terminal();
    terminal.setCursorDisplay(terminal::CursorDisplay::Blink);
    auto constexpr BlinkInterval = chrono::milliseconds(500);
    terminal.setCursorBlinkingInterval(BlinkInterval);

    auto const clockBase = chrono::steady_clock::time_point();

    SECTION("over time")
    {
        auto const clockBeforeTurn = clockBase + BlinkInterval - chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(terminal.cursorCurrentlyVisible());

        auto const clockAfterTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockAfterTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(!terminal.cursorCurrentlyVisible());
    }

    SECTION("force show on keyboard input")
    {
        // get a state where the blinking cursor is not visible
        auto const clockBeforeTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(!terminal.cursorCurrentlyVisible());

        // type something into the terminal
        auto const clockAtInputEvent = clockBase + BlinkInterval + chrono::milliseconds(10);
        terminal.sendCharPressEvent('x', terminal::Modifier {}, clockAtInputEvent);

        // now the cursor is visible before the interval has passed
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer();
        CHECK(terminal.cursorCurrentlyVisible());
    }
}

TEST_CASE("Terminal.DECCARA", "[terminal]")
{
    auto mock = MockTerm { ColumnCount(5), LineCount(5) };
    auto constexpr ClockBase = chrono::steady_clock::time_point();
    mock.terminal().tick(ClockBase);
    mock.terminal().ensureFreshRenderBuffer();
    CHECK("" == trimmedTextScreenshot(mock));

    mock.writeToStdout("12345\r\n"
                       "67890\r\n"
                       "ABCDE\r\n"
                       "abcde\r\n"
                       "fghij");

    mock.terminal().tick(ClockBase + chrono::seconds(1));
    mock.terminal().ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    auto const top = 2;
    auto const left = 3;
    auto const bottom = 4;
    auto const right = 5;
    mock.writeToStdout(fmt::format("\033[{top};{left};{bottom};{right};{sgr}$r",
                                   fmt::arg("top", top),
                                   fmt::arg("left", left),
                                   fmt::arg("bottom", bottom),
                                   fmt::arg("right", right),
                                   fmt::arg("sgr", 4)));

    mock.terminal().tick(ClockBase + chrono::seconds(2));
    mock.terminal().ensureFreshRenderBuffer();
    CHECK("12345\n67890\nABCDE\nabcde\nfghij" == trimmedTextScreenshot(mock));

    // Just peak into it and test. That's not really 100% precise, tbh., but
    // i'd like to keep on going and have fun doing the project and not die
    // early due to a overdose of TDD. :-)
    for (auto line = top; line <= bottom; ++line)
        for (auto column = left; column <= right; ++column)
        {
            auto const& someCell =
                mock.terminal().screen().at(LineOffset(line - 1), ColumnOffset(column - 1));
            CHECK(someCell.styles() & terminal::CellFlags::Underline);
        }
}

TEST_CASE("Terminal.SynchronizedOutput", "[terminal]")
{
    constexpr auto BatchOn = "\033[?2026h"sv;
    constexpr auto BatchOff = "\033[?2026l"sv;

    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm { ColumnCount(20), LineCount(1) };

    mc.writeToStdout(BatchOn);
    mc.writeToStdout("Hello ");
    mc.terminal().tick(now);
    mc.terminal().ensureFreshRenderBuffer();
    CHECK("" == trimmedTextScreenshot(mc));

    mc.writeToStdout(" World");
    mc.terminal().tick(now);
    mc.terminal().ensureFreshRenderBuffer();
    CHECK("" == trimmedTextScreenshot(mc));

    mc.writeToStdout(BatchOff);
    mc.terminal().tick(now);
    mc.terminal().ensureFreshRenderBuffer();
    CHECK("Hello  World" == trimmedTextScreenshot(mc));
}
