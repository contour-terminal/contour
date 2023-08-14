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
#include <vtbackend/MockTerm.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <vtpty/PageSize.h>

#include <catch2/catch.hpp>

// Checklist
// =========
//
// - [x] [count] |
// - [ ] [count] h
// - [ ] [count] j
// - [ ] [count] k
// - [ ] [count] l
// - [ ] [count] J
// - [ ] [count] K
// - [ ] [count] w
// - [ ] [count] b
// - [ ] [count] e
// - [ ] 0
// - [ ] ^
// - [ ] $
// - [ ] G
// - [ ] gg
// - [ ] %
// - [ ] i{TextObject}
// - [ ] a{TextObject}

using namespace terminal::test;

// {{{ helpers
namespace
{

// Construct a MockTerm instance with given dimension and fill it with some text.
//
// The text cursor is ensured to be in home position (top left), and
// input mode is set to normal mode.
auto setupMockTerminal(std::string_view text,
                       terminal::PageSize pageSize = terminal::PageSize { terminal::LineCount(6),
                                                                          terminal::ColumnCount(40) },
                       terminal::LineCount history = terminal::LineCount(0))
{
    return terminal::mock_term<terminal::MockPty> {
        terminal::PageSize { // increment line count by one for indicator statusline.
                             pageSize.lines + 1,
                             pageSize.columns },
        history,
        1024, // ptyReadBufferSize
        [text](auto& mock) {
            mock.terminal.setStatusDisplay(terminal::status_display_type::Indicator);
            mock.writeToScreen(text);
            mock.writeToScreen("\033[H");
            mock.terminal.inputHandler().setMode(terminal::vi_mode::Normal);
            // logScreenTextAlways(mock);
            REQUIRE(mock.terminal.state().viCommands.cursorPosition.line.value == 0);
            REQUIRE(mock.terminal.state().viCommands.cursorPosition.column.value == 0);
        }
    };
}

} // namespace
// }}}

TEST_CASE("vi.motions: |", "[vi]")
{
    // The meaning of this code shall not be questioned. It's purely for testing.
    auto mock = setupMockTerminal("auto pi_times(unsigned factor) noexcept;",
                                  terminal::PageSize { terminal::LineCount(2), terminal::ColumnCount(40) });

    // middle
    mock.sendCharPressSequence("15|");
    CHECK(mock.terminal.state().viCommands.cursorPosition.line.value == 0);
    CHECK(mock.terminal.state().viCommands.cursorPosition.column.value == 14);

    // at right margin
    mock.sendCharPressSequence("40|");
    CHECK(mock.terminal.state().viCommands.cursorPosition.line.value == 0);
    CHECK(mock.terminal.state().viCommands.cursorPosition.column.value == 39);

    // at left margin
    mock.sendCharPressSequence("1|");
    CHECK(mock.terminal.state().viCommands.cursorPosition.line.value == 0);
    CHECK(mock.terminal.state().viCommands.cursorPosition.column.value == 0);

    // one off right margin
    mock.sendCharPressSequence("41|");
    CHECK(mock.terminal.state().viCommands.cursorPosition.line.value == 0);
    CHECK(mock.terminal.state().viCommands.cursorPosition.column.value == 39);

    // without [count] leading to left margin
    mock.sendCharPressSequence("|");
    CHECK(mock.terminal.state().viCommands.cursorPosition.line.value == 0);
    CHECK(mock.terminal.state().viCommands.cursorPosition.column.value == 0);
}

TEST_CASE("vi.motions: text objects", "[vi]")
{
    // The meaning of this code shall not be questioned. It's purely for testing.
    auto mock = setupMockTerminal("auto pi_times(unsigned factor) noexcept\r\n"
                                  "{\r\n"
                                  "    auto constexpr pi = 3.1415;\r\n"
                                  "    return pi + ((factor - 1) * //\r\n"
                                  "                                pi);\r\n"
                                  "}",
                                  terminal::PageSize { terminal::LineCount(6), terminal::ColumnCount(40) });

    SECTION("vi( across multiple lines, nested")
    {
        mock.sendCharPressSequence("3j31|"); // position cursor onto the * symbol, line 4.
        REQUIRE(mock.terminal.state().viCommands.cursorPosition == 3_lineOffset + 30_columnOffset);

        mock.sendCharPressSequence("vi("); // cursor is now placed at the end of the selection
        CHECK(mock.terminal.state().viCommands.cursorPosition == 4_lineOffset + 33_columnOffset);
        REQUIRE(mock.terminal.selector() != nullptr);
        terminal::selection const& selection = *mock.terminal.selector();
        CHECK(selection.from() == 3_lineOffset + 17_columnOffset);
        CHECK(selection.to() == 4_lineOffset + 33_columnOffset);
    }

    SECTION("vi) across multiple lines, nested")
    {
        mock.sendCharPressSequence("3j31|"); // position cursor onto the * symbol, line 4.
        REQUIRE(mock.terminal.state().viCommands.cursorPosition == 3_lineOffset + 30_columnOffset);

        mock.sendCharPressSequence("vi)"); // cursor is now placed at the end of the selection
        CHECK(mock.terminal.state().viCommands.cursorPosition == 4_lineOffset + 33_columnOffset);
        REQUIRE(mock.terminal.selector() != nullptr);
        terminal::selection const& selection = *mock.terminal.selector();
        CHECK(selection.from() == 3_lineOffset + 17_columnOffset);
        CHECK(selection.to() == 4_lineOffset + 33_columnOffset);
    }
}

TEST_CASE("vi.motions: M", "[vi]")
{
    auto mock = setupMockTerminal("Hello\r\n",
                                  terminal::PageSize { terminal::LineCount(10), terminal::ColumnCount(40) });

    // first move cursor by one right, to also ensure that column is preserved
    mock.sendCharPressSequence("lM");
    CHECK(mock.terminal.state().viCommands.cursorPosition == 4_lineOffset + 1_columnOffset);

    // running M again won't change anything
    mock.sendCharPressSequence("M");
    CHECK(mock.terminal.state().viCommands.cursorPosition == 4_lineOffset + 1_columnOffset);
}

TEST_CASE("vi.motion: t{char}", "[vi]")
{
    auto mock = setupMockTerminal("One.Two..Three and more\r\n"
                                  "   On the next line.",
                                  terminal::PageSize { terminal::LineCount(10), terminal::ColumnCount(40) });
    mock.sendCharPressSequence("te"); // jump to the char before first `e`, which is `n`.
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 1_columnOffset);

    mock.sendCharPressSequence("t "); // jump to the char before first space character, which is `e`.
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 13_columnOffset);
}

TEST_CASE("vi.motion: b", "[vi]")
{
    auto mock = setupMockTerminal("One.Two..Three and more\r\n"
                                  "   On the next line.",
                                  terminal::PageSize { terminal::LineCount(10), terminal::ColumnCount(40) });
    mock.sendCharPressSequence("j$"); // jump to line 2, at the right-most non-space character.
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 1_lineOffset + 19_columnOffset);

    mock.sendCharPressSequence("b"); // l[ine.]
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 1_lineOffset + 15_columnOffset);
    mock.sendCharPressSequence("2b"); // t[he]
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 1_lineOffset + 6_columnOffset);
    mock.sendCharPressSequence("3b"); // a[nd] -- on line 1
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 15_columnOffset);
    mock.sendCharPressSequence("b"); // T[hree]
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 9_columnOffset);
    mock.sendCharPressSequence("b"); // .[.]
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 7_columnOffset);
    mock.sendCharPressSequence("b"); // T[wo]
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 4_columnOffset);
    mock.sendCharPressSequence("b"); // .
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 3_columnOffset);
    mock.sendCharPressSequence("b"); // O[ne]
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 0_columnOffset);

    mock.sendCharPressSequence("b");
    REQUIRE(mock.terminal.state().viCommands.cursorPosition == 0_lineOffset + 0_columnOffset);
}

TEST_CASE("ViCommands:modeChanged", "[vi]")
{
    auto mock = setupMockTerminal("Hello\r\n",
                                  terminal::PageSize { terminal::LineCount(10), terminal::ColumnCount(40) });

    SECTION("clearSearch() must be invoked when switch to ViMode::Insert")
    {
        mock.terminal.setNewSearchTerm(U"search_term", true);
        mock.terminal.state().inputHandler.setMode(terminal::vi_mode::Insert);
        REQUIRE(mock.terminal.state().searchMode.pattern.empty());
    }
}
