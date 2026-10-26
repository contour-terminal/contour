// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/testing/DetectingTerm.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/PageSize.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

// Checklist
// =========
//
// - [x] [count] |
// - [ ] [count] h
// - [x] [count] j
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

// "line 0" to "line <count - 1>", the last without a line feed, so that no blank row is left at the bottom.
std::string numberedLines(int count)
{
    auto text = std::string {};
    for (auto const i: std::views::iota(0, count))
        text += std::format("{}line {}", i == 0 ? "" : "\r\n", i);
    return text;
}

// The screen row the Vi cursor is drawn on.
template <typename Mock>
vtbackend::LineOffset viCursorScreenRow(Mock const& mock)
{
    return mock.terminal.viewport().translateGridToScreenCoordinate(
        mock.terminal.normalModeCursorPosition().line);
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

TEST_CASE("vi.motions: [count] j", "[vi]")
{
    // vim's `j`: down [count] lines, no further than the last one, keeping the column. On a page that has
    // scrolled by one, so that the history is not empty and the motion cannot lean on the two agreeing.
    auto mock =
        setupMockTerminal("line 0\r\nline 1\r\nline 2\r\nline 3\r\nline 4\r\n"
                          "line 5\r\nline 6\r\nline 7\r\nline 8\r\nline 9\r\nbottom",
                          vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) },
                          vtbackend::LineCount(10));
    REQUIRE(mock.terminal.primaryScreen().historyLineCount() == vtbackend::LineCount(1));

    SECTION("with a count")
    {
        mock.sendCharSequence("9j");
        CHECK(mock.terminal.normalModeCursorPosition() == 9_lineOffset + 0_columnOffset);
    }

    SECTION("as single steps")
    {
        mock.sendCharSequence("jjjjjjjjj");
        CHECK(mock.terminal.normalModeCursorPosition() == 9_lineOffset + 0_columnOffset);
    }

    SECTION("keeping the column")
    {
        mock.sendCharSequence("3l9j");
        CHECK(mock.terminal.normalModeCursorPosition() == 9_lineOffset + 3_columnOffset);
    }

    SECTION("no further than the last line")
    {
        mock.sendCharSequence("20j");
        CHECK(mock.terminal.normalModeCursorPosition() == 9_lineOffset + 0_columnOffset);
    }

    CHECK(mock.terminal.primaryScreen()
              .grid()
              .lineText(mock.terminal.normalModeCursorPosition().line)
              .starts_with("bottom"));
}

TEST_CASE("vi.motions: [count] j stops at the last row the status line leaves", "[vi]")
{
    // Entering Normal mode shows the indicator status line, which takes a row from the page. With the cursor
    // above the bottom row, the page gives up that bottom row rather than push its top one into the history:
    // a shrinking grid cuts below the cursor first, as a shrinking window does. The page is one row shorter
    // then, so `9j` from its top stops at its new last row, one short of nine.
    auto mock =
        vtbackend::MockTerm { vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) },
                              vtbackend::LineCount(10) };
    printLines(mock, "line", 10);
    mock.writeToScreen("bottom\033[H");
    mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    REQUIRE(mock.terminal.pageSize().lines == vtbackend::LineCount(9));

    mock.sendCharSequence("9j");
    CHECK(mock.terminal.normalModeCursorPosition() == 8_lineOffset + 0_columnOffset);
    CHECK(mock.terminal.primaryScreen().grid().lineText(8_lineOffset).starts_with("line 9"));

    // The row is only out of sight: leaving Normal mode takes the status line away, and the row comes back.
    mock.sendCharSequence("i");
    CHECK(mock.terminal.pageSize().lines == vtbackend::LineCount(10));
    CHECK(mock.terminal.primaryScreen().grid().lineText(9_lineOffset).starts_with("bottom"));
}

TEST_CASE("Viewport.safeArea caps the scrolloff as vim does", "[vi][viewport]")
{
    // A padding the page cannot hold on both sides leaves the middle row, the upper one of two: measured
    // in vim 9.1 with 'scrolloff' 8, whose cursor stays on row 4 of a 9- or a 10-row window.
    struct Row
    {
        int lines;
        int padding;
        int top;
        int bottom;
    };
    auto constexpr Rows = std::array {
        Row { .lines = 1, .padding = 8, .top = 0, .bottom = 0 },
        Row { .lines = 3, .padding = 8, .top = 1, .bottom = 1 },
        Row { .lines = 9, .padding = 8, .top = 4, .bottom = 4 },
        Row { .lines = 10, .padding = 8, .top = 4, .bottom = 4 },
        Row { .lines = 16, .padding = 8, .top = 7, .bottom = 7 },
        Row { .lines = 17, .padding = 8, .top = 8, .bottom = 8 },
        Row { .lines = 20, .padding = 8, .top = 8, .bottom = 11 },
        Row { .lines = 10, .padding = 2, .top = 2, .bottom = 7 },
        Row { .lines = 10, .padding = 0, .top = 0, .bottom = 9 },
    };

    for (auto const& row: Rows)
    {
        auto mock = vtbackend::MockTerm { vtbackend::PageSize { vtbackend::LineCount(row.lines),
                                                                vtbackend::ColumnCount(10) } };
        auto const area = mock.terminal.viewport().safeArea(vtbackend::LineCount(row.padding));
        INFO(std::format("{} rows, padding {}", row.lines, row.padding));
        CHECK(area.top == vtbackend::LineOffset(row.top));
        CHECK(area.bottom == vtbackend::LineOffset(row.bottom));
    }
}

TEST_CASE("vi.motions: a page shorter than its scrolloff keeps the cursor where it moved", "[vi]")
{
    // vim caps 'scrolloff' at half the window, and so does the viewport: three rows leave one row of
    // context on either side of the middle one, whatever the default eight asks for. Uncapped, a step
    // scrolled the page by up to eight rows, further than it is tall, which pushed the line the cursor
    // stepped onto off the page -- and the clamp to the page dragged the cursor after it.
    auto mock = setupMockTerminal(numberedLines(7),
                                  vtbackend::PageSize { vtbackend::LineCount(3), vtbackend::ColumnCount(40) },
                                  vtbackend::LineCount(10));
    REQUIRE(mock.terminal.primaryScreen().historyLineCount() == vtbackend::LineCount(4));
    REQUIRE(mock.terminal.viewport().scrollOff() == vtbackend::LineCount(8));

    SECTION("down, at the bottom of the scrollback")
    {
        mock.sendCharSequence("j");
        CHECK(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 0_columnOffset);
        CHECK(mock.terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));
    }

    SECTION("up into the history, onto the middle row")
    {
        mock.sendCharSequence("k");
        CHECK(mock.terminal.normalModeCursorPosition() == vtbackend::LineOffset(-1) + 0_columnOffset);
        CHECK(viCursorScreenRow(mock) == 1_lineOffset);
    }
}

TEST_CASE("vi.motions: j on a page shorter than twice its scrolloff keeps the cursor on the middle row",
          "[vi]")
{
    // Nine rows, as a ten-row page has beside the indicator status line, hold four rows of context on either
    // side of the middle one. Stepping down from the top row with history above, the cursor settles on the
    // middle row and the page follows it down a row at a time, until it is back at the bottom. Uncapped,
    // every other step scrolled the page up by several rows and the next one scrolled it back.
    auto mock = setupMockTerminal(numberedLines(30),
                                  vtbackend::PageSize { vtbackend::LineCount(9), vtbackend::ColumnCount(40) },
                                  vtbackend::LineCount(30));
    REQUIRE(mock.terminal.primaryScreen().historyLineCount() == vtbackend::LineCount(21));

    for (auto const offset: { 3, 2, 1, 0 })
    {
        mock.sendCharSequence("j");
        INFO(std::format("scrolled by {} rows", offset));
        CHECK(mock.terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(offset));
        CHECK(viCursorScreenRow(mock) == 4_lineOffset);
    }
}

TEST_CASE("vi.motions: j on a page taller than twice its scrolloff keeps all of it", "[vi]")
{
    // Twenty rows hold the whole default scrolloff of eight on either side: the first step from the top row
    // scrolls the history in until eight rows stand above the cursor, and the next one scrolls nothing.
    auto mock =
        setupMockTerminal(numberedLines(40),
                          vtbackend::PageSize { vtbackend::LineCount(20), vtbackend::ColumnCount(40) },
                          vtbackend::LineCount(40));

    mock.sendCharSequence("j");
    CHECK(mock.terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(7));
    CHECK(viCursorScreenRow(mock) == 8_lineOffset);

    mock.sendCharSequence("j");
    CHECK(mock.terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(7));
    CHECK(viCursorScreenRow(mock) == 9_lineOffset);
}

TEST_CASE("vi.motions: H and L on a page shorter than twice its scrolloff", "[vi]")
{
    // H and L keep the scrolloff too, so capped they meet on the middle row, as vim's do -- the upper one of
    // two. Uncapped, H went to the bottom row and L to the top one.
    auto const rows = GENERATE(9, 10);
    INFO(std::format("{} rows", rows));
    auto mock =
        setupMockTerminal(numberedLines(40),
                          vtbackend::PageSize { vtbackend::LineCount(rows), vtbackend::ColumnCount(40) },
                          vtbackend::LineCount(40));

    mock.sendCharSequence("H");
    CHECK(viCursorScreenRow(mock) == 4_lineOffset);
    CHECK(mock.terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));

    mock.sendCharSequence("L");
    CHECK(viCursorScreenRow(mock) == 4_lineOffset);
    CHECK(mock.terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));
}

TEST_CASE("vi.motions: zz centers on the middle row, the upper one of two", "[vi]")
{
    // Where vim's zz puts the line on a window of 9 or 10 rows. zz centers the line and then moves the cursor
    // onto it, keeping the scrolloff: uncapped, that second step scrolled the line down to the bottom row.
    auto const rows = GENERATE(9, 10);
    INFO(std::format("{} rows", rows));
    auto mock =
        setupMockTerminal(numberedLines(40),
                          vtbackend::PageSize { vtbackend::LineCount(rows), vtbackend::ColumnCount(40) },
                          vtbackend::LineCount(40));

    mock.sendCharSequence("zz");
    CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);
    CHECK(viCursorScreenRow(mock) == 4_lineOffset);
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

TEST_CASE("vi.motions: M goes to the middle row, the upper one of two", "[vi]")
{
    // Measured in vim 9.1, in a pty: M puts the cursor on row 4 of a 9- and of a 10-row window, row 1 of a
    // 3-row one, row 9 of a 19-row one and row 0 of a 2-row one -- (rows - 1) / 2 -- and scrolls nothing.
    auto constexpr Middles = std::array {
        std::pair { 2, 0 }, std::pair { 3, 1 }, std::pair { 9, 4 }, std::pair { 10, 4 }, std::pair { 19, 9 },
    };
    for (auto const& [rows, middle]: Middles)
    {
        INFO(std::format("{} rows", rows));
        auto mock =
            setupMockTerminal(numberedLines(40),
                              vtbackend::PageSize { vtbackend::LineCount(rows), vtbackend::ColumnCount(40) },
                              vtbackend::LineCount(40));

        mock.sendCharSequence("M");
        CHECK(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(middle));
        CHECK(mock.terminal.viewport().scrollOffset() == vtbackend::ScrollOffset(0));
    }
}

TEST_CASE("vi.motions: M counts only the rows drawn when collapsed folds leave the page short",
          "[vi][folding]")
{
    // With no history to fill in for the rows a collapsed block hides, the rows drawn end above the page's
    // bottom, and blank rows follow, as vim's window shows rows past the end of its buffer. vim's M counts
    // only the lines drawn: measured in vim 9.1, a 10-row window showing 5 lines puts it on line 3, one
    // showing 4 on line 2. Here the page draws 4 rows: the block's command, the prompt below it, and two
    // blank lines.
    auto mock =
        setupMockTerminal(commandRun("ls", { "a", "b", "c", "d", "e", "f" }) + std::string(PromptStart),
                          vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });
    collapseOnlyFold(mock);
    REQUIRE(mock.terminal.foldProjection().size() == 4uz);

    mock.sendCharSequence("M");
    CHECK(mock.terminal.normalModeCursorPosition().line == 7_lineOffset);
    CHECK(viCursorScreenRow(mock) == 1_lineOffset);
    CHECK(mock.terminal.primaryScreen().grid().lineText(7_lineOffset).starts_with("$ "));
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

TEST_CASE("vi.motion: % matches the bracket on the screen the cursor walks", "[vi]")
{
    // The bracket under the cursor and the one it is matched with are read from the same screen: the primary
    // one, and the alternate one while a full-screen application has it in use.
    auto const screenSwitch = GENERATE(std::string_view {}, std::string_view { "\033[?1049h" });
    INFO(std::format("screen switch: {}", screenSwitch.empty() ? "none" : "?1049h"));
    auto mock =
        setupMockTerminal(std::format("{}(x) [y]", screenSwitch),
                          vtbackend::PageSize { vtbackend::LineCount(4), vtbackend::ColumnCount(20) });

    mock.sendCharSequence("%");
    CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 2_columnOffset);
    mock.sendCharSequence("%");
    CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);
    mock.sendCharSequence("4l%");
    CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 6_columnOffset);
}

TEST_CASE("vi.search: * searches the word under the cursor literally while the regex toggle is on", "[vi]")
{
    // `*` takes a word from the screen, which is text: under the regex toggle "a.b" would also find "axb".
    auto mock = setupMockTerminal(
        "a.b axb a.b", vtbackend::PageSize { vtbackend::LineCount(4), vtbackend::ColumnCount(20) });
    REQUIRE(mock.terminal.setSearchOptions({ .syntax = vtbackend::SearchSyntax::Regex }).has_value());

    mock.sendCharSequence("*");
    CHECK(mock.terminal.search().pattern == U"a.b");
    CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 8_columnOffset);
    CHECK(mock.terminal.search().options.syntax == vtbackend::SearchSyntax::Regex);
}

TEST_CASE("ViCommands:modeChanged", "[vi]")
{
    auto mock = setupMockTerminal(
        "Hello\r\n", vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });

    SECTION("clearSearch() must be invoked when switch to ViMode::Insert")
    {
        (void) mock.terminal.setNewSearchTerm(U"search_term", vtbackend::SearchOrigin::DoubleClick);
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

// {{{ #1954: lock modifiers must never reach the Vi input handler

namespace
{

using vtbackend::Modifier;
using vtbackend::Modifiers;

/// Eight three-character lines, so that vertical motions and a [count] of 5 stay on-screen.
auto setupEightLineTerminal()
{
    return setupMockTerminal("abc\r\n"
                             "abc\r\n"
                             "abc\r\n"
                             "abc\r\n"
                             "abc\r\n"
                             "abc\r\n"
                             "abc\r\n"
                             "abc",
                             vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });
}

} // namespace

TEST_CASE("vi.locks: navigation keys move the cursor while lock keys are latched", "[vi][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = setupEightLineTerminal();

        mock.sendKeyEvent(vtbackend::Key::DownArrow, locks);
        CHECK(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 0_columnOffset);

        mock.sendKeyEvent(vtbackend::Key::RightArrow, locks);
        CHECK(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 1_columnOffset);

        mock.sendKeyEvent(vtbackend::Key::End, locks);
        CHECK(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 2_columnOffset);

        mock.sendKeyEvent(vtbackend::Key::Home, locks);
        CHECK(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 0_columnOffset);

        mock.sendKeyEvent(vtbackend::Key::UpArrow, locks);
        CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);

        mock.sendKeyEvent(vtbackend::Key::PageDown, locks);
        CHECK(mock.terminal.normalModeCursorPosition().line.value > 0);

        mock.sendKeyEvent(vtbackend::Key::PageUp, locks);
        CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);
    }
}

TEST_CASE("vi.locks: Escape leaves visual mode while lock keys are latched", "[vi][locks]")
{
    SECTION("via Key::Escape")
    {
        for (auto const locks: LockCombinations)
        {
            INFO(std::format("lock modifiers {}", locks));
            auto mock = setupEightLineTerminal();
            mock.sendCharSequence("v");
            REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Visual);

            mock.sendKeyEvent(vtbackend::Key::Escape, locks);
            CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
        }
    }

    SECTION("via the ESC character")
    {
        for (auto const locks: LockCombinations)
        {
            INFO(std::format("lock modifiers {}", locks));
            auto mock = setupEightLineTerminal();
            mock.sendCharSequence("v");
            REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Visual);

            mock.sendCharEvent(U'\033', locks);
            CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
        }
    }
}

TEST_CASE("vi.locks: `/` asks for the search prompt while lock keys are latched", "[vi][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = setupEightLineTerminal();

        mock.sendCharEvent(U'/', locks);
        CHECK(mock.searchPromptRequests == 1);
    }
}

TEST_CASE("vi.locks: [count] prefix is parsed while lock keys are latched", "[vi][locks]")
{
    for (auto const locks: LockCombinations)
    {
        INFO(std::format("lock modifiers {}", locks));
        auto mock = setupEightLineTerminal();

        mock.sendCharSequence("5j", locks);
        CHECK(mock.terminal.normalModeCursorPosition() == 5_lineOffset + 0_columnOffset);
    }
}

// The Qt frontend infers LockKey::NumLock for every keypad digit (Qt only reports Key_0..9 with
// KeypadModifier while NumLock is on), so numpad digits could never serve as a [count] prefix.
TEST_CASE("vi.locks: numpad digits serve as a [count] prefix", "[vi][locks]")
{
    auto mock = setupEightLineTerminal();

    mock.sendKeyEvent(vtbackend::Key::Numpad_5, NumLockOnly);
    mock.sendCharSequence("j");

    CHECK(mock.terminal.normalModeCursorPosition() == 5_lineOffset + 0_columnOffset);
}

// The negative half of the contract: only the lock bits are stripped, chord modifiers still gate.
TEST_CASE("vi.locks: chord modifiers still gate the Vi input handler", "[vi][locks]")
{
    SECTION("Control+DownArrow is not a motion")
    {
        auto mock = setupEightLineTerminal();
        mock.sendKeyEvent(vtbackend::Key::DownArrow, Modifiers { Modifier::Control });
        CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);
    }

    SECTION("Control+/ does not ask for the search prompt")
    {
        auto mock = setupEightLineTerminal();
        mock.sendCharEvent(U'/', Modifiers { Modifier::Control });
        CHECK(mock.searchPromptRequests == 0);
    }

    SECTION("Control+5 is not a [count] prefix")
    {
        auto mock = setupEightLineTerminal();
        mock.sendCharSequence("5j", Modifiers { Modifier::Control });
        CHECK(mock.terminal.normalModeCursorPosition() == 0_lineOffset + 0_columnOffset);
    }
}

// }}}

// Leaving visual mode is a terminal-side UI action; the Escape must be consumed, not forwarded to
// the application. The ESC-character path has always done so, the Key::Escape path had not.
TEST_CASE("vi.visual: Escape leaves visual mode without sending ESC to the application", "[vi]")
{
    SECTION("via Key::Escape")
    {
        auto mock = setupEightLineTerminal();
        mock.sendCharSequence("v");
        REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Visual);

        mock.resetReplyData();
        mock.sendKeyEvent(vtbackend::Key::Escape);

        CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
        CHECK(mock.replyData().empty());
    }

    SECTION("via the ESC character")
    {
        auto mock = setupEightLineTerminal();
        mock.sendCharSequence("v");
        REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Visual);

        mock.resetReplyData();
        mock.sendCharEvent(U'\033');

        CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
        CHECK(mock.replyData().empty());
    }
}

// `/` used to open a line editor inside the terminal, which then owned every subsequent keystroke.
// It now only asks the frontend for a prompt, so the terminal must be left exactly as it was.
TEST_CASE("vi.search: `/` asks for a prompt without capturing input or switching mode", "[vi]")
{
    auto mock = setupEightLineTerminal();
    REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Normal);

    mock.sendCharSequence("/");
    CHECK(mock.searchPromptRequests == 1);

    // No mode switch: the old prompt forced Normal mode only to make the status line carrying it
    // visible, and a floating find bar needs nothing of the sort.
    CHECK(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Normal);

    // No capture either. "j" is a motion again, rather than the first letter of a search term.
    mock.sendCharSequence("j");
    CHECK(mock.terminal.normalModeCursorPosition() == 1_lineOffset + 0_columnOffset);
    CHECK(mock.terminal.search().pattern.empty());
}

// {{{ Select All under Vi mode
// Select All reaches the terminal from the context menu, a key binding and the command palette — all of
// which are consulted BEFORE the Vi handler. So it installs a selection while the Vi layer owns the input,
// and it must honour the two invariants that layer only ever asserts.
TEST_CASE("vi.selectAll: honours the Vi layer's selection invariants", "[vi]")
{
    SECTION("Visual mode: the selection stays InProgress, because every motion extends it")
    {
        auto mock = setupMockTerminal("one\r\n"
                                      "two\r\n"
                                      "three");
        mock.sendCharEvent(U'v');
        REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Visual);

        mock.terminal.selectAll();

        REQUIRE(mock.terminal.selectionAvailable());
        // A Complete selection aborts on the very next keystroke: ViCommands::moveCursorTo() extends the
        // selector, and Selection::extend() opens with `assert(_state != State::Complete)`.
        CHECK_FALSE(mock.terminal.isSelectionComplete());

        mock.sendCharEvent(U'j'); // the motion that used to abort
        CHECK(mock.terminal.selectionAvailable());
    }

    SECTION("Normal mode: a yank replaces the standing selection rather than tripping over it")
    {
        auto mock = setupMockTerminal("one\r\n"
                                      "two\r\n"
                                      "three");
        REQUIRE(mock.terminal.inputHandler().mode() == vtbackend::ViMode::Normal);

        mock.terminal.selectAll();
        REQUIRE(mock.terminal.selectionAvailable());

        // `yy` used to abort on extractTextAndHighlightRange()'s `assert(!_terminal->selector())`.
        mock.sendCharEvent(U'y');
        mock.sendCharEvent(U'y');
        CHECK_FALSE(mock.terminal.selectionAvailable());
    }
}

TEST_CASE("vi.selectAll: the next motion extends from where the selection ends", "[vi]")
{
    // A selection an action makes in a Visual mode leaves the vi cursor at its end, which the next motion
    // then moves: the selection keeps its start and grows or shrinks at its end, as one the motions made.
    using namespace vtbackend;
    auto mock =
        setupMockTerminal("x\r\nx\r\nx\r\n" + commandRun("ls", { "a", "b" }) + std::string(PromptStart),
                          PageSize { LineCount(10), ColumnCount(20) });
    mock.sendCharEvent(U'V');
    REQUIRE(mock.terminal.inputHandler().mode() == ViMode::VisualLine);

    SECTION("a block")
    {
        REQUIRE(mock.terminal.selectCommandBlock(CommandBlockTarget::Last, CommandBlockPart::PromptAndOutput)
                    .has_value());
        mock.sendCharEvent(U'j');
        CHECK(mock.terminal.selector()->from().line == LineOffset(3));
        CHECK(mock.terminal.selector()->to().line == LineOffset(6));
    }

    SECTION("everything")
    {
        mock.terminal.selectAll();
        mock.sendCharEvent(U'k');
        CHECK(mock.terminal.selector()->from().line == LineOffset(0));
        CHECK(mock.terminal.selector()->to().line == LineOffset(8));
    }
}
// }}}

// {{{ Line marks belong to the logical line
TEST_CASE("vi.mark: `mm` marks the LOGICAL line, not the wrapped piece the cursor sits in", "[vi]")
{
    // The Vi cursor moves by PHYSICAL line, so `j` walks it into the continuations of a wrapped line. A
    // mark left on a continuation is one that a widening resize erases (growColumns rebuilds a joined
    // logical line from its head alone) and one the command-block scan cannot see (it reads the head). It
    // would not even toggle: reading a continuation never finds the mark sitting on the head.
    auto mock =
        setupMockTerminal(std::string(25, 'x'), // 25 columns on a 10-column page: two wraps
                          vtbackend::PageSize { vtbackend::LineCount(6), vtbackend::ColumnCount(10) });

    auto const& screen = mock.terminal.currentScreen();
    REQUIRE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(1), vtbackend::LineFlag::Wrapped));

    mock.sendCharEvent(U'j'); // onto the first continuation
    mock.sendCharSequence("mm");

    CHECK(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::UserMark));
    CHECK_FALSE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(1), vtbackend::LineFlag::UserMark));
    // A bookmark, not a prompt: block-atomic eviction and folding cut at Marked, and must not cut here.
    CHECK_FALSE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::Marked));

    SECTION("and toggles it off again from that same continuation")
    {
        mock.sendCharSequence("mm");
        CHECK_FALSE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::UserMark));
    }
}

TEST_CASE("vi.mark: `[m` and `]m` visit user marks and prompt marks alike", "[vi][semanticblocks]")
{
    auto mock = setupMockTerminal("0\r\n1\r\n2\r\n3\r\n4\r\n5");
    auto& screen = mock.terminal.currentScreen();
    screen.setLogicalLineFlags(vtbackend::LineOffset(1), vtbackend::LineFlag::Marked, true);   // a prompt
    screen.setLogicalLineFlags(vtbackend::LineOffset(3), vtbackend::LineFlag::UserMark, true); // `mm`

    mock.sendCharSequence("5j");
    REQUIRE(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(5));

    mock.sendCharSequence("[m");
    CHECK(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(3));
    mock.sendCharSequence("[m");
    CHECK(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(1));
    mock.sendCharSequence("]m");
    CHECK(mock.terminal.normalModeCursorPosition().line == vtbackend::LineOffset(3));
}

TEST_CASE("vi.mark: `vim` spans between a prompt mark and a user mark", "[vi][semanticblocks]")
{
    // The text object walks up from the cursor and down from it; each walk stops at either kind of mark.
    auto mock = setupMockTerminal("0\r\n1\r\n2\r\n3\r\n4\r\n5");
    auto& screen = mock.terminal.currentScreen();

    SECTION("the prompt mark above, the user mark below")
    {
        screen.setLogicalLineFlags(vtbackend::LineOffset(1), vtbackend::LineFlag::Marked, true);
        screen.setLogicalLineFlags(vtbackend::LineOffset(3), vtbackend::LineFlag::UserMark, true);
    }

    SECTION("the user mark above, the prompt mark below")
    {
        screen.setLogicalLineFlags(vtbackend::LineOffset(1), vtbackend::LineFlag::UserMark, true);
        screen.setLogicalLineFlags(vtbackend::LineOffset(3), vtbackend::LineFlag::Marked, true);
    }

    mock.sendCharSequence("2j");
    mock.sendCharSequence("vim");

    REQUIRE(mock.terminal.selector() != nullptr);
    CHECK(mock.terminal.selector()->from().line == vtbackend::LineOffset(2));
    CHECK(mock.terminal.selector()->to().line == vtbackend::LineOffset(2));
}

TEST_CASE("vi.mark: `mm` on a prompt row toggles the user mark and leaves the prompt mark",
          "[vi][semanticblocks]")
{
    // A row can carry both: the shell's prompt mark and the user's bookmark. `mm` is the user's, so it must
    // never clear the shell's -- that would take the block it heads out of folding and eviction.
    auto mock = setupMockTerminal("0\r\n1\r\n2");
    auto& screen = mock.terminal.currentScreen();
    screen.setLogicalLineFlags(vtbackend::LineOffset(0), vtbackend::LineFlag::Marked, true);

    mock.sendCharSequence("mm");
    CHECK(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::UserMark));
    CHECK(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::Marked));

    mock.sendCharSequence("mm");
    CHECK_FALSE(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::UserMark));
    CHECK(screen.isLineFlagEnabledAt(vtbackend::LineOffset(0), vtbackend::LineFlag::Marked));
}
TEST_CASE("vi.hint: gh scans with the profile's pattern table", "[vi][hintmode][links]")
{
    auto settings = vtbackend::Settings {};
    settings.pageSize = vtbackend::PageSize { vtbackend::LineCount(4), vtbackend::ColumnCount(40) };
    auto const entries = std::array { vtbackend::LinkPatternSpec {
        .name = "jira", .regex = R"(\b[A-Z]+-\d+\b)", .activation = {} } };
    settings.linkPatterns =
        std::make_shared<vtbackend::LinkPatterns const>(vtbackend::LinkPatterns::compile(entries));
    auto mock = vtbackend::MockTerm<vtpty::MockPty> { std::move(settings) };
    mock.writeToScreen("see ABC-123 now");
    mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    mock.sendCharSequence("gh");
    REQUIRE_FALSE(mock.terminal.hintMatches().empty());
    CHECK(std::ranges::any_of(mock.terminal.hintMatches(),
                              [](auto const& m) { return m.matchedText == "ABC-123"; }));
}

// {{{ links
namespace
{

/// The directory the tests' files are in. Absolute here: on Windows a root directory without a drive is
/// relative to the current drive.
#ifdef _WIN32
constexpr auto Work = std::string_view { "C:/w" };
#else
constexpr auto Work = std::string_view { "/w" };
#endif

/// Shows @p text on @p term in vi normal mode, the vi cursor at the top left, as setupMockTerminal() does.
/// No detection cycle runs: the index is empty.
void showInNormalMode(DetectingTerm& term, std::string_view text)
{
    term.mock.terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    term.mock.writeToScreen(text);
    term.mock.writeToScreen("\033[H");
    term.mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    REQUIRE(term.mock.terminal.normalModeCursorPosition() == vtbackend::CellLocation {});
}

/// The vi cursor of @p term.
[[nodiscard]] vtbackend::CellLocation viCursor(DetectingTerm const& term)
{
    return term.mock.terminal.normalModeCursorPosition();
}

[[nodiscard]] constexpr vtbackend::CellLocation at(int line, int column)
{
    return vtbackend::CellLocation { .line = vtbackend::LineOffset(line),
                                     .column = vtbackend::ColumnOffset(column) };
}

/// A page of 6 rows and the indicator status line, as setupMockTerminal() makes it.
constexpr auto LinkPage = vtbackend::PageSize { vtbackend::LineCount(7), vtbackend::ColumnCount(40) };

} // namespace

TEST_CASE("vi.link: ]x and [x visit link starts, scanning what is not indexed yet", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    showInNormalMode(term, "a https://x.y/ b\r\nc https://z.w/");

    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(0, 2)); // found without a detection cycle: scanned on the keypress
    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(1, 2));
    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(1, 2)); // none further: the cursor stays
    term.mock.sendCharSequence("[x");
    CHECK(viCursor(term) == at(0, 2));
    term.mock.sendCharSequence("[x");
    CHECK(viCursor(term) == at(0, 2)); // none before it either
    term.mock.sendCharSequence("gg02]x");
    CHECK(viCursor(term) == at(1, 2));
}

TEST_CASE("vi.link: [x and ]x scan page by page beyond the index, into the scrollback", "[vi][links]")
{
    // Three pages of text, a link on the first row and on the last: neither is within a page of the other.
    auto term = DetectingTerm { LinkPage, {}, vtbackend::HistoryLimits::plain(vtbackend::LineCount(30)) };
    auto text = std::string { "top https://first.example/" };
    for ([[maybe_unused]] auto const _: std::views::iota(0, 16))
        text += "\r\nplain";
    text += "\r\nend https://last.example/";
    term.mock.writeToScreen(text);
    term.mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal); // the vi cursor ends the last row

    term.mock.sendCharSequence("[x");
    auto const last = viCursor(term);
    CHECK(last.column == vtbackend::ColumnOffset(4));
    term.mock.sendCharSequence("[x"); // two pages back, in the scrollback
    auto const first = viCursor(term);
    CHECK(first.column == vtbackend::ColumnOffset(4));
    CHECK(first.line < vtbackend::LineOffset(0));
    CHECK(last.line - first.line > vtbackend::LineOffset(11)); // two pages away
    term.mock.sendCharSequence("gg0");
    CHECK(viCursor(term).line == first.line); // the top row: nothing was skipped

    term.mock.sendCharSequence("]x"); // from the index now
    CHECK(viCursor(term) == first);
    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == last);
}

TEST_CASE("vi.link: a path counts once its probe has confirmed it", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    auto const path = std::format("{}/notes.md", Work);
    term.files.addFile(path, "");
    showInNormalMode(term, std::format("see {}", path));

    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(0, 0)); // the probe has only started
    term.probe.drain();
    term.mock.terminal.refreshRenderBuffer(); // the frame adopts its answer
    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(0, 4));
}

TEST_CASE("vi.link: ]x combines with y as a motion", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    showInNormalMode(term, "ab https://x.y/");
    term.mock.sendCharSequence("y]x");
    CHECK(term.mock.clipboardData == "ab h"); // up to the link's first cell, as `y0` and `yw` take theirs
}

namespace
{

/// @p count plain lines, each ending with a line feed but the last.
[[nodiscard]] std::string plainLines(int count)
{
    auto text = std::string {};
    for (auto const line: std::views::iota(0, count))
        text += line + 1 < count ? "plain\r\n" : "plain";
    return text;
}

/// Shows @p text on @p term with the indicator status line set first, as setupMockTerminal() does, and runs a
/// detection cycle over what the viewport shows: the viewport and a page above it are scanned, nothing else.
void showAndDetect(DetectingTerm& term, std::string_view text)
{
    term.mock.terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    term.mock.writeToScreen(text);
    term.cycle();
}

/// The number of link spans the index holds on @p line of @p term's page on display.
[[nodiscard]] std::size_t spansOn(DetectingTerm& term, int line)
{
    auto const _ = std::scoped_lock { term.mock.terminal };
    auto const* index = term.mock.terminal.detectedLinks();
    if (!index)
        return 0;
    return index->onRow(term.mock.terminal.displayedPage().grid().stableLineIdOf(vtbackend::LineOffset(line)))
        .size();
}

/// 26 lines, rows -20 to 5 of a 6-row page: links at rows -16 and -12, beyond what a detection cycle at the
/// bottom scans, and one at row 5, within it.
[[nodiscard]] std::string linksAroundABand()
{
    return "top\r\n" + plainLines(3) + "\r\nband https://band.example/\r\n" + plainLines(3)
           + "\r\nmore https://more.example/\r\n" + plainLines(16) + "\r\nend https://end.example/";
}

} // namespace

TEST_CASE("vi.link: ]x finds a link in rows never scanned before one the index holds", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage, {}, vtbackend::HistoryLimits::plain(vtbackend::LineCount(30)) };
    showAndDetect(term, linksAroundABand());
    REQUIRE(term.linkAt(5, 4) != nullptr);
    REQUIRE(term.linkAt(-16, 5) == nullptr); // the band: never scanned
    term.mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    term.mock.sendCharSequence("gg0");
    REQUIRE(viCursor(term) == at(-20, 0));

    SECTION("one step")
    {
        term.mock.sendCharSequence("]x");
        CHECK(viCursor(term) == at(-16, 5));
        term.mock.sendCharSequence("]x");
        CHECK(viCursor(term) == at(-12, 5));
        term.mock.sendCharSequence("]x");
        CHECK(viCursor(term) == at(5, 4));
    }

    SECTION("a count crossing the band")
    {
        term.mock.sendCharSequence("2]x");
        CHECK(viCursor(term) == at(-12, 5));
        term.mock.sendCharSequence("[x");
        CHECK(viCursor(term) == at(-16, 5));
    }
}

TEST_CASE("vi.link: ]x finds a link on a row rewritten since its scan", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    showAndDetect(term, "a\r\nx https://a.example/\r\nb\r\ny https://b.example/");
    REQUIRE(term.linkAt(1, 2) != nullptr);
    REQUIRE(term.linkAt(3, 2) != nullptr);
    term.mock.writeToScreen("\033[2;1Hzz https://c.example/\033[K"); // row 1 now holds its link at column 3
    term.mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    term.mock.sendCharSequence("gg0");

    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(1, 3));
}

TEST_CASE("vi.link: repeated [x reach a link beyond one keypress's scan", "[vi][links]")
{
    // Two pages a keypress: the link 40 rows up is four keypresses away, each scanning further.
    auto term = DetectingTerm { LinkPage,
                                vtbackend::LinkDetectorTuning { .motionSearchPages = 2 },
                                vtbackend::HistoryLimits::plain(vtbackend::LineCount(60)) };
    term.mock.terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    term.mock.writeToScreen("first https://first.example/\r\n" + plainLines(40));
    term.mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto const start = viCursor(term);
    REQUIRE(start.line == vtbackend::LineOffset(5));

    for ([[maybe_unused]] auto const press: std::views::iota(0, 3))
    {
        term.mock.sendCharSequence("[x");
        CHECK(viCursor(term) == start);
    }
    term.mock.sendCharSequence("[x");
    CHECK(viCursor(term) == at(-35, 6));
}

TEST_CASE("vi.link: [x lands on a wrapped link's first row", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    auto const url = "https://example.com/" + std::string(40, 'a');

    SECTION("from its continuation row")
    {
        showInNormalMode(term, "word " + url);
        term.mock.sendCharSequence("j$");
        REQUIRE(viCursor(term).line == vtbackend::LineOffset(1));
        term.mock.sendCharSequence("[x");
        CHECK(viCursor(term) == at(0, 5));
    }

    SECTION("whose first row is above the page scanned")
    {
        // Rows -4 to 5: the link's first row is -1 and its second 0; from row 5 the first page scanned is
        // rows 0 to 5, which the logical line widens to row -1.
        term.mock.terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
        term.mock.writeToScreen("x\r\nx\r\nx\r\nword " + url + "\r\n" + plainLines(5));
        term.mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
        REQUIRE(viCursor(term).line == vtbackend::LineOffset(5));
        term.mock.sendCharSequence("[x");
        CHECK(viCursor(term) == at(-1, 5));
    }
}

TEST_CASE("vi.link: a scan refreshes the link under a pointer that has not moved", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    showInNormalMode(term, "a https://x.y/");
    term.mock.terminal.sendMouseMoveEvent(
        vtbackend::Modifier::None, at(0, 4), vtbackend::PixelCoordinate {}, false);
    REQUIRE(term.mock.terminal.hoveringLinkId() == vtbackend::LinkId {});

    term.mock.sendCharSequence("]x");
    REQUIRE(term.linkAt(0, 4) != nullptr);
    CHECK(term.mock.terminal.hoveringLinkId() == term.linkAt(0, 4)->id);
}

TEST_CASE("vi.link: ]x takes in a probe's answer without a frame", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    auto const path = std::format("{}/notes.md", Work);
    term.files.addFile(path, "");
    showInNormalMode(term, std::format("see {}", path));

    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(0, 0)); // the probe has only started
    term.probe.drain();                // and answered; no frame adopts it
    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(0, 4));
}

TEST_CASE("vi.link: a scan beside the background match indexes a path once", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    auto const path = std::format("{}/notes.md", Work);
    term.files.addFile(path, "");
    term.mock.terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    term.mock.writeToScreen(std::format("see {}", path));
    // A collect: its match job waits on the executor while `]x` scans the same row.
    term.frame();
    term.mock.terminal.tick(term.mock.terminal.currentTime() + term.tuning.debounce);
    term.frame();
    term.mock.terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    term.mock.sendCharSequence("gg0]x");

    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
    {
        term.match.drain();
        term.probe.drain();
        term.frame();
    }
    CHECK(spansOn(term, 0) == 1);
}

TEST_CASE("vi.link: ]x and [x stay put without link detection", "[vi][links]")
{
    auto mock = setupMockTerminal("a https://x.y/ b https://z.w/");
    mock.sendCharSequence("]x");
    CHECK(mock.terminal.normalModeCursorPosition() == at(0, 0));
    mock.sendCharSequence("$");
    auto const end = mock.terminal.normalModeCursorPosition();
    mock.sendCharSequence("[x");
    CHECK(mock.terminal.normalModeCursorPosition() == end);
}

TEST_CASE("vi.link: ]x skips paths on the alternate screen unless enabled", "[vi][links]")
{
    auto term = DetectingTerm { LinkPage };
    term.files.addFile(std::format("{}/notes.txt", Work), "");
    term.mock.terminal.setCurrentWorkingDirectory(vtbackend::test::fileUrlOf(std::filesystem::path { Work }));
    // The path at columns 2-10, the URL from column 14 on.
    showInNormalMode(term, "\033[?1049h\033[Ha notes.txt b https://x.y/");

    term.mock.sendCharSequence("]x");
    CHECK(viCursor(term) == at(0, 14)); // the path is not even probed: no probe answer would change this
    CHECK(term.probe.pending() == 0);

    auto policy = vtbackend::LinkDetectionPolicy {};
    policy.otherScreens.filePaths = vtbackend::DetectLinks::Yes;
    {
        auto const _ = std::scoped_lock { term.mock.terminal };
        term.mock.terminal.setLinkDetectionPolicy(policy);
    }
    term.mock.sendCharSequence("gg0]x");
    REQUIRE(term.probe.pending() == 1); // a path counts once its probe has confirmed it
    term.probe.drain();
    term.mock.sendCharSequence("gg0]x");
    CHECK(viCursor(term) == at(0, 2));
}
// }}}
