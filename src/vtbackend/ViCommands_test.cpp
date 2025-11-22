// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <vtpty/PageSize.h>

#include <catch2/catch_test_macros.hpp>

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

using namespace vtbackend::test;

// {{{ helpers
namespace
{

// Construct a MockTerm instance with given dimension and fill it with some text.
//
// The text cursor is ensured to be in home position (top left), and
// input mode is set to normal mode.
auto setupMockTerminal(std::string_view text,
                       vtbackend::PageSize pageSize = vtbackend::PageSize { vtbackend::LineCount(6),
                                                                            vtbackend::ColumnCount(40) },
                       vtbackend::LineCount history = vtbackend::LineCount(0))
{
    return vtbackend::MockTerm<vtpty::MockPty> {
        vtpty::PageSize { // increment line count by one for indicator statusline.
                          .lines = pageSize.lines + 1,
                          .columns = pageSize.columns },
        history,
        1024, // ptyReadBufferSize
        [text](auto& mock) {
            mock.terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
            mock.writeToScreen(text);
            mock.writeToScreen("\033[H");
            mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
            // logScreenTextAlways(mock);
            REQUIRE(mock.terminal.normalModeCursorPosition().line.value == 0);
            REQUIRE(mock.terminal.normalModeCursorPosition().column.value == 0);
        }
    };
}

} // namespace
// }}}

// NOLINTBEGIN(misc-const-correctness)
TEST_CASE("vi.motions: |", "[vi]")
{
    // The meaning of this code shall not be questioned. It's purely for testing.
    auto mock =
        setupMockTerminal("auto pi_times(unsigned factor) noexcept;",
                          vtbackend::PageSize { vtbackend::LineCount(2), vtbackend::ColumnCount(40) });

    // middle
    mock.sendCharSequence("15|");
    CHECK(mock.terminal.normalModeCursorPosition().line.value == 0);
    CHECK(mock.terminal.normalModeCursorPosition().column.value == 14);

    // at right margin
    mock.sendCharSequence("40|");
    CHECK(mock.terminal.normalModeCursorPosition().line.value == 0);
    CHECK(mock.terminal.normalModeCursorPosition().column.value == 39);

    // at left margin
    mock.sendCharSequence("1|");
    CHECK(mock.terminal.normalModeCursorPosition().line.value == 0);
    CHECK(mock.terminal.normalModeCursorPosition().column.value == 0);

    // one off right margin
    mock.sendCharSequence("41|");
    CHECK(mock.terminal.normalModeCursorPosition().line.value == 0);
    CHECK(mock.terminal.normalModeCursorPosition().column.value == 39);

    // without [count] leading to left margin
    mock.sendCharSequence("|");
    CHECK(mock.terminal.normalModeCursorPosition().line.value == 0);
    CHECK(mock.terminal.normalModeCursorPosition().column.value == 0);
}

TEST_CASE("vi.motions: text objects", "[vi]")
{
    // The meaning of this code shall not be questioned. It's purely for testing.
    auto mock =
        setupMockTerminal("auto pi_times(unsigned factor) noexcept\r\n"
                          "{\r\n"
                          "    auto constexpr pi = 3.1415;\r\n"
                          "    return pi + ((factor - 1) * //\r\n"
                          "                                pi);\r\n"
                          "}",
                          vtbackend::PageSize { vtbackend::LineCount(6), vtbackend::ColumnCount(40) });

    SECTION("vi( across multiple lines, nested")
    {
        mock.sendCharSequence("3j31|"); // position cursor onto the * symbol, line 4.
        REQUIRE(mock.terminal.normalModeCursorPosition() == 3_lineOffset + 30_columnOffset);

        mock.sendCharSequence("vi("); // cursor is now placed at the end of the selection
        CHECK(mock.terminal.normalModeCursorPosition() == 4_lineOffset + 33_columnOffset);
        REQUIRE(mock.terminal.selector() != nullptr);
        vtbackend::Selection const& selection = *mock.terminal.selector();
        CHECK(selection.from() == 3_lineOffset + 17_columnOffset);
        CHECK(selection.to() == 4_lineOffset + 33_columnOffset);
    }

    SECTION("vi) across multiple lines, nested")
    {
        mock.sendCharSequence("3j31|"); // position cursor onto the * symbol, line 4.
        REQUIRE(mock.terminal.normalModeCursorPosition() == 3_lineOffset + 30_columnOffset);

        mock.sendCharSequence("vi)"); // cursor is now placed at the end of the selection
        CHECK(mock.terminal.normalModeCursorPosition() == 4_lineOffset + 33_columnOffset);
        REQUIRE(mock.terminal.selector() != nullptr);
        vtbackend::Selection const& selection = *mock.terminal.selector();
        CHECK(selection.from() == 3_lineOffset + 17_columnOffset);
        CHECK(selection.to() == 4_lineOffset + 33_columnOffset);
    }
}

TEST_CASE("vi.motions: M", "[vi]")
{
    auto mock = setupMockTerminal(
        "Hello\r\n", vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });

    // first move cursor by one right, to also ensure that column is preserved
    mock.sendCharSequence("lM");
    CHECK(mock.terminal.normalModeCursorPosition() == 4_lineOffset + 1_columnOffset);

    // running M again won't change anything
    mock.sendCharSequence("M");
    CHECK(mock.terminal.normalModeCursorPosition() == 4_lineOffset + 1_columnOffset);
}

TEST_CASE("vi.motion: t{char}", "[vi]")
{
    auto mock =
        setupMockTerminal("One.Two..Three and more\r\n"
                          "   On the next line.",
                          vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });
    mock.sendCharSequence("te"); // jump to the char before first `e`, which is `n`.
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 1_columnOffset);

    mock.sendCharSequence("t "); // jump to the char before first space character, which is `e`.
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 13_columnOffset);
}

TEST_CASE("vi.motion: b", "[vi]")
{
    auto mock =
        setupMockTerminal("One.Two..Three and more\r\n"
                          "   On the next line.",
                          vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });
    mock.sendCharSequence("j$"); // jump to line 2, at the right-most non-space character.
    REQUIRE(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 19_columnOffset);

    mock.sendCharSequence("b"); // l[ine.]
    REQUIRE(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 15_columnOffset);
    mock.sendCharSequence("2b"); // t[he]
    REQUIRE(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 6_columnOffset);
    mock.sendCharSequence("3b"); // a[nd] -- on line 1
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 15_columnOffset);
    mock.sendCharSequence("b"); // T[hree]
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 9_columnOffset);
    mock.sendCharSequence("b"); // .[.]
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 7_columnOffset);
    mock.sendCharSequence("b"); // T[wo]
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 4_columnOffset);
    mock.sendCharSequence("b"); // .
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 3_columnOffset);
    mock.sendCharSequence("b"); // O[ne]
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);

    mock.sendCharSequence("b");
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);
}

TEST_CASE("ViCommands:modeChanged", "[vi]")
{
    auto mock = setupMockTerminal(
        "Hello\r\n", vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });

    SECTION("clearSearch() must be invoked when switch to ViMode::Insert")
    {
        mock.terminal.setNewSearchTerm(U"search_term", true);
        mock.terminal.inputHandler().setMode(vtbackend::ViMode::Insert);
        REQUIRE(mock.terminal.search().pattern.empty());
    }
}
// NOLINTEND(misc-const-correctness)

TEST_CASE("yank", "[vi]")
{
    auto mock = setupMockTerminal(
        "Hello World", vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });

    mock.sendCharSequence("3l"); // Move cursor to second 'l'
    REQUIRE(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 3_columnOffset);
    mock.sendCharSequence("y0");

    CHECK(mock.clipboardData == "Hell");
}
