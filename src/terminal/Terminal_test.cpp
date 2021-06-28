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
#include <catch2/catch.hpp>

#include <unicode/convert.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include <iostream>

using namespace std;

namespace // {{{ helpers
{
    /// Takes a textual screenshot using the terminals render buffer.
    vector<string> textScreenshot(terminal::Terminal const& _terminal)
    {
        terminal::RenderBufferRef renderBuffer = _terminal.renderBuffer();

        vector<string> lines;
        lines.resize(_terminal.screenSize().height);

        terminal::Coordinate lastPos = {};
        size_t lastCount = 0;
        for (terminal::RenderCell const& cell: renderBuffer.buffer.screen)
        {
            auto const gap = (cell.position.column + static_cast<int>(lastCount) - 1) - lastPos.column;
            auto& currentLine = lines.at(cell.position.row - 1);
            if (gap > 0) // Did we jump?
                currentLine.insert(currentLine.end(), gap - 1, ' ');

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
        explicit MockTerm(crispy::Size _size):
            pty_{_size},
            terminal_{
                pty_,
                1024,
                *this,
                1024, // max history line count
                chrono::milliseconds(500), // cursor blink interval
                chrono::steady_clock::time_point(), // initial time point
            }
        {
        }

        terminal::MockPty& pty() noexcept { return pty_; }
        terminal::Terminal& terminal() noexcept { return terminal_; }
        terminal::Terminal const& terminal() const noexcept { return terminal_; }

        void writeToStdin(std::string_view _text)
        {
            pty_.stdinBuffer() += _text;
        }

        void writeToStdout(std::string_view _text)
        {
            pty_.stdoutBuffer() += _text;
            terminal_.processInputOnce();
        }

        void logScreenText(std::string const& headline = "")
        {
            if (headline.empty())
                UNSCOPED_INFO("dump:");
            else
                UNSCOPED_INFO(headline + ":");

            for (int row = 1; row <= terminal().screen().size().height; ++row)
                UNSCOPED_INFO(fmt::format("[{}] \"{}\"", row, terminal().screen().renderTextLine(row)));
        }

    private:
        terminal::MockPty pty_;
        terminal::Terminal terminal_;
    };

    std::string trimmedTextScreenshot(MockTerm const& _mt)
    {
        return trimRight(join(textScreenshot(_mt.terminal())));
    }
} // }}}

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

TEST_CASE("Terminal.BlinkingCursor", "[terminal]")
{
    auto mc = MockTerm{{6, 4}};
    auto& terminal = mc.terminal();
    terminal.setCursorDisplay(terminal::CursorDisplay::Blink);
    auto constexpr BlinkInterval = chrono::milliseconds(500);
    terminal.setCursorBlinkingInterval(BlinkInterval);

    auto const clockBase = chrono::steady_clock::time_point();

    SECTION("over time")
    {
        auto const clockBeforeTurn = clockBase + BlinkInterval - chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer(clockBeforeTurn);
        CHECK(terminal.cursorCurrentlyVisible());

        auto const clockAfterTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockAfterTurn);
        terminal.ensureFreshRenderBuffer(clockAfterTurn);
        CHECK(!terminal.cursorCurrentlyVisible());
    }

    SECTION("force show on keyboard input")
    {
        // get a state where the blinking cursor is not visible
        auto const clockBeforeTurn = clockBase + BlinkInterval + chrono::milliseconds(1);
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer(clockBeforeTurn);
        CHECK(!terminal.cursorCurrentlyVisible());

        // type something into the terminal
        auto const clockAtInputEvent = clockBase + BlinkInterval + chrono::milliseconds(10);
        terminal.sendCharPressEvent(terminal::CharInputEvent{L'x', terminal::Modifier{}}, clockAtInputEvent);

        // now the cursor is visible before the interval has passed
        terminal.tick(clockBeforeTurn);
        terminal.ensureFreshRenderBuffer(clockBeforeTurn);
        CHECK(terminal.cursorCurrentlyVisible());
    }
}

TEST_CASE("Terminal.SynchronizedOutput", "[terminal]")
{
    constexpr auto BatchOn = "\033[?2026h"sv;
    constexpr auto BatchOff = "\033[?2026l"sv;

    auto const now = chrono::steady_clock::now();
    auto mc = MockTerm{{20, 1}};

    mc.writeToStdout(BatchOn);
    mc.writeToStdout("Hello ");
    mc.terminal().ensureFreshRenderBuffer(now);
    CHECK("" == trimmedTextScreenshot(mc));

    mc.writeToStdout(" World");
    mc.terminal().ensureFreshRenderBuffer(now);
    CHECK("" == trimmedTextScreenshot(mc));

    mc.writeToStdout(BatchOff);
    mc.terminal().ensureFreshRenderBuffer(now);
    CHECK("Hello  World" == trimmedTextScreenshot(mc));
}
