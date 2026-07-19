// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Bidi.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <string_view>
#include <vector>

using namespace vtbackend;
using unicode::Bidi_Direction;

namespace
{

[[nodiscard]] BidiPageLayout layOut(std::vector<BidiLineInput> const& lines,
                                    std::optional<Bidi_Direction> direction = std::nullopt)
{
    return computeBidiPageLayout(lines, direction);
}

/// The logical columns in the order they are drawn, left to right.
[[nodiscard]] std::vector<int> visualOrder(BidiLineLayout const& layout, size_t columns)
{
    auto result = std::vector<int> {};
    for (size_t i = 0; i < columns; ++i)
        result.push_back(unbox<int>(layout.logicalColumnAt(ColumnOffset::cast_from(i))));
    return result;
}

} // namespace

TEST_CASE("Bidi.mayContainBidi", "[bidi]")
{
    // The fast-path scan. Everything below U+0590 is left-to-right in an LTR paragraph.
    CHECK_FALSE(mayContainBidi(U""));
    CHECK_FALSE(mayContainBidi(U"hello world 123"));
    CHECK_FALSE(mayContainBidi(U"։")); // one below the threshold
    CHECK(mayContainBidi(U"֐"));      // the threshold itself
    CHECK(mayContainBidi(U"abc שלום"));
    CHECK(mayContainBidi(U"ا")); // Arabic
}

TEST_CASE("Bidi.pure_ltr_takes_the_identity_fast_path", "[bidi]")
{
    auto const layout = layOut({ { U"hello world", false } });

    REQUIRE(layout.lines.size() == 1);
    CHECK(layout.lines[0].identity);
    CHECK(layout.lines[0].paragraphDirection == Bidi_Direction::Left_To_Right);

    // The fast path must not even allocate the tables.
    CHECK(layout.lines[0].levels.empty());
    CHECK(layout.lines[0].visualToLogical.empty());
    CHECK(layout.lines[0].logicalToVisual.empty());

    // ... and must still answer queries, as the identity.
    CHECK(layout.lines[0].logicalColumnAt(ColumnOffset(3)) == ColumnOffset(3));
    CHECK(layout.lines[0].visualColumnAt(ColumnOffset(3)) == ColumnOffset(3));
    CHECK(layout.lines[0].levelAt(ColumnOffset(3)) == 0);
}

TEST_CASE("Bidi.hebrew_reverses", "[bidi]")
{
    auto const layout = layOut({ { U"שלום", false } }); // shalom

    REQUIRE(layout.lines.size() == 1);
    CHECK_FALSE(layout.lines[0].identity);
    CHECK(layout.lines[0].paragraphDirection == Bidi_Direction::Right_To_Left);
    CHECK(visualOrder(layout.lines[0], 4) == std::vector { 3, 2, 1, 0 });

    // The two tables must be inverses of one another.
    for (size_t i = 0; i < 4; ++i)
    {
        auto const column = ColumnOffset::cast_from(i);
        CHECK(layout.lines[0].logicalColumnAt(layout.lines[0].visualColumnAt(column)) == column);
    }
}

TEST_CASE("Bidi.digits_move_within_an_rtl_run", "[bidi]")
{
    // "abc <shalom> 123 def" -- the digits follow the Hebrew logically but are drawn before it,
    // because European digits inside a right-to-left run resolve to an even level.
    auto const layout = layOut({ { U"abc שלום 123 def", false } });

    REQUIRE(layout.lines.size() == 1);
    CHECK_FALSE(layout.lines[0].identity);
    CHECK(layout.lines[0].paragraphDirection == Bidi_Direction::Left_To_Right);

    // Logical: a b c _ ש ל ו ם _ 1 2 3 _ d e f
    // Visual : a b c _ 1 2 3 _ ם ו ל ש _ d e f
    CHECK(visualOrder(layout.lines[0], 16)
          == std::vector { 0, 1, 2, 3, 9, 10, 11, 8, 7, 6, 5, 4, 12, 13, 14, 15 });
}

// The point of the paragraph model: a soft-wrapped row is NOT resolved on its own. Resolving the
// same two rows independently gives a different answer, and that difference is the bug this guards.
TEST_CASE("Bidi.paragraph_spans_wrapped_lines", "[bidi]")
{
    auto const wrapped = layOut({
        { U"שלום", false }, // Hebrew, starts the paragraph
        { U"abc", true },   // continues it
    });

    auto const separate = layOut({
        { U"שלום", false },
        { U"abc", false }, // a paragraph of its own
    });

    REQUIRE(wrapped.lines.size() == 2);
    REQUIRE(separate.lines.size() == 2);

    // The Hebrew row is the same either way.
    CHECK(wrapped.lines[0].paragraphDirection == Bidi_Direction::Right_To_Left);
    CHECK(separate.lines[0].paragraphDirection == Bidi_Direction::Right_To_Left);

    // The continuation inherits the paragraph's right-to-left base ...
    CHECK(wrapped.lines[1].paragraphDirection == Bidi_Direction::Right_To_Left);
    // ... whereas on its own it would autodetect left-to-right from its own first strong character.
    CHECK(separate.lines[1].paragraphDirection == Bidi_Direction::Left_To_Right);
}

TEST_CASE("Bidi.multiple_paragraphs_resolve_independently", "[bidi]")
{
    auto const layout = layOut({
        { U"שלום", false }, // RTL paragraph
        { U"abc", false },  // LTR paragraph
        { U"الع", false },  // Arabic: RTL paragraph
    });

    REQUIRE(layout.lines.size() == 3);
    CHECK(layout.lines[0].paragraphDirection == Bidi_Direction::Right_To_Left);
    CHECK(layout.lines[1].paragraphDirection == Bidi_Direction::Left_To_Right);
    CHECK(layout.lines[2].paragraphDirection == Bidi_Direction::Right_To_Left);

    CHECK(layout.lines[1].identity);
}

TEST_CASE("Bidi.forced_direction_overrides_autodetection", "[bidi]")
{
    SECTION("forcing right-to-left on Latin text")
    {
        auto const layout = layOut({ { U"abc", false } }, Bidi_Direction::Right_To_Left);
        REQUIRE(layout.lines.size() == 1);
        CHECK(layout.lines[0].paragraphDirection == Bidi_Direction::Right_To_Left);
        // The letters keep their order -- a Latin run inside an RTL paragraph is still read left to
        // right. What changes is the level, and with it where the run sits on screen.
        CHECK(visualOrder(layout.lines[0], 3) == std::vector { 0, 1, 2 });
    }

    SECTION("forcing left-to-right on Hebrew text")
    {
        auto const layout = layOut({ { U"שלום", false } }, Bidi_Direction::Left_To_Right);
        REQUIRE(layout.lines.size() == 1);
        CHECK(layout.lines[0].paragraphDirection == Bidi_Direction::Left_To_Right);
        // Still reversed: the Hebrew is an RTL run, only the paragraph around it is not.
        CHECK(visualOrder(layout.lines[0], 4) == std::vector { 3, 2, 1, 0 });
    }

    SECTION("a forced right-to-left paragraph does not take the fast path")
    {
        // Pure ASCII would be identity under autodetection, but not once the base direction is
        // imposed -- so the >= U+0590 scan alone must not be allowed to decide.
        auto const layout = layOut({ { U"abc 123", false } }, Bidi_Direction::Right_To_Left);
        REQUIRE(layout.lines.size() == 1);
        CHECK(layout.lines[0].paragraphDirection == Bidi_Direction::Right_To_Left);
    }
}

TEST_CASE("Bidi.empty_input", "[bidi]")
{
    CHECK(layOut({}).lines.empty());

    auto const blank = layOut({ { U"", false } });
    REQUIRE(blank.lines.size() == 1);
    CHECK(blank.lines[0].identity);
}

TEST_CASE("Bidi.out_of_range_queries_are_the_identity", "[bidi]")
{
    auto const layout = layOut({ { U"של", false } });
    REQUIRE(layout.lines.size() == 1);

    // A column past the end of the line still has to map somewhere; a terminal row is wider than
    // the text on it.
    CHECK(layout.lines[0].logicalColumnAt(ColumnOffset(40)) == ColumnOffset(40));
    CHECK(layout.lines[0].visualColumnAt(ColumnOffset(40)) == ColumnOffset(40));

    // And a row that does not exist is a neutral left-to-right layout rather than a crash.
    CHECK(layout.lineAt(99).identity);
    CHECK(layout.lineAt(99).paragraphDirection == Bidi_Direction::Left_To_Right);
}

// ---- The control sequences ----

#include <vtbackend/MockTerm.h>
#include <vtbackend/RenderBuffer.h>
#include <vtbackend/Selector.h>

#include <crispy/escape.h>

TEST_CASE("Bidi.BDSM", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Set is the default: the terminal reorders bidirectional text itself.
    CHECK(mock.terminal.isModeEnabled(AnsiMode::BiDirectionalSupport));
    CHECK(mock.terminal.bidiReorderingEnabled());

    mock.writeToScreen("\033[8l"); // explicit: the application already reordered
    CHECK_FALSE(mock.terminal.isModeEnabled(AnsiMode::BiDirectionalSupport));
    CHECK_FALSE(mock.terminal.bidiReorderingEnabled());

    mock.writeToScreen("\033[8h");
    CHECK(mock.terminal.bidiReorderingEnabled());
}

TEST_CASE("Bidi.BDSM is reported by DECRQM", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // 1 = set. Answering 0 here would be claiming never to have heard of BDSM.
    mock.writeToScreen("\033[8$p");
    CHECK(crispy::escape(mock.terminal.peekInput()) == crispy::escape("\033[8;1$y"));
    mock.discardPendingReplies();

    mock.writeToScreen("\033[8l");
    mock.writeToScreen("\033[8$p");
    CHECK(crispy::escape(mock.terminal.peekInput()) == crispy::escape("\033[8;2$y")); // 2 = reset
}

TEST_CASE("Bidi.SCP selects the character path", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // No SCP yet: the terminal's own default, which is left-to-right.
    CHECK_FALSE(mock.terminal.characterPath().has_value());
    CHECK(mock.terminal.bidiParagraphDirection() == Bidi_Direction::Left_To_Right);

    mock.writeToScreen("\033[2 k"); // RTL
    CHECK(mock.terminal.characterPath() == Bidi_Direction::Right_To_Left);
    CHECK(mock.terminal.bidiParagraphDirection() == Bidi_Direction::Right_To_Left);

    mock.writeToScreen("\033[1 k"); // LTR
    CHECK(mock.terminal.characterPath() == Bidi_Direction::Left_To_Right);

    mock.writeToScreen("\033[0 k"); // restore the terminal default
    CHECK_FALSE(mock.terminal.characterPath().has_value());

    // An omitted parameter defaults to 0, so a bare SCP also restores the default.
    mock.writeToScreen("\033[2 k");
    REQUIRE(mock.terminal.characterPath().has_value());
    mock.writeToScreen("\033[ k");
    CHECK_FALSE(mock.terminal.characterPath().has_value());
}

TEST_CASE("Bidi.DECRLM selects a right-to-left page", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    CHECK(mock.terminal.bidiParagraphDirection() == Bidi_Direction::Left_To_Right);

    mock.writeToScreen("\033[?34h"); // DECRLM
    CHECK(mock.terminal.isModeEnabled(DECMode::RightToLeftMode));
    CHECK(mock.terminal.bidiParagraphDirection() == Bidi_Direction::Right_To_Left);

    mock.writeToScreen("\033[?34l");
    CHECK(mock.terminal.bidiParagraphDirection() == Bidi_Direction::Left_To_Right);
}

// Autodetection is the more specific request, so it wins over a base direction that SCP or DECRLM
// merely established as the default.
TEST_CASE("Bidi.autodetect overrides a selected character path", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033[2 k"); // force RTL
    REQUIRE(mock.terminal.bidiParagraphDirection() == Bidi_Direction::Right_To_Left);

    mock.writeToScreen("\033[?2501h"); // autodetect
    CHECK(mock.terminal.isModeEnabled(DECMode::BidiAutodetectParagraph));
    CHECK_FALSE(mock.terminal.bidiParagraphDirection().has_value());

    mock.writeToScreen("\033[?2501l");
    CHECK(mock.terminal.bidiParagraphDirection() == Bidi_Direction::Right_To_Left);
}

TEST_CASE("Bidi.private modes toggle and report", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    SECTION("box mirroring is reset by default")
    {
        CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BidiBoxMirroring));
        mock.writeToScreen("\033[?2500h");
        CHECK(mock.terminal.isModeEnabled(DECMode::BidiBoxMirroring));
    }

    SECTION("arrow-key swapping is SET by default")
    {
        // The recommendation makes this one default to set, unlike the other two.
        CHECK(mock.terminal.isModeEnabled(DECMode::BidiSwapArrowKeys));
        mock.writeToScreen("\033[?1243l");
        CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BidiSwapArrowKeys));
    }

    SECTION("DECRQM reports them rather than disclaiming knowledge")
    {
        mock.writeToScreen("\033[?2500$p");
        CHECK(crispy::escape(mock.terminal.peekInput()) == crispy::escape("\033[?2500;2$y")); // reset
        mock.discardPendingReplies();

        mock.writeToScreen("\033[?1243$p");
        CHECK(crispy::escape(mock.terminal.peekInput()) == crispy::escape("\033[?1243;1$y")); // set
    }
}

// ---- End to end: the render buffer really comes out in visual order ----

namespace
{
/// The line's codepoints as the render buffer holds them, left to right.
[[nodiscard]] std::u32string renderedLine(RenderBuffer const& buffer, int line)
{
    // A uniform-SGR line takes the trivial fast path and appears as a RenderLine ...
    for (auto const& renderLine: buffer.lines)
        if (renderLine.lineOffset.value == line)
            return renderLine.text;

    // ... otherwise as individual cells.
    auto result = std::u32string {};
    for (auto const& cell: buffer.cells)
        if (cell.position.line.value == line)
            result += cell.codepoints.empty() ? U' ' : cell.codepoints[0];
    return result;
}

[[nodiscard]] std::u32string trimmed(std::u32string text)
{
    while (!text.empty() && text.back() == U' ')
        text.pop_back();
    return text;
}
} // namespace

TEST_CASE("Bidi.render buffer is reordered", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(12) } };

    mock.writeToScreen("\u05E9\u05DC\u05D5\u05DD"); // shalom, logical order
    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);
    mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    // Stored logically, drawn reversed.
    // Reversed: final-mem, vav, lamed, shin.
    CHECK(trimmed(renderedLine(buffer.get(), 0)) == U"\u05DD\u05D5\u05DC\u05E9");
}

TEST_CASE("Bidi.BDSM reset leaves the text alone", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(12) } };

    mock.writeToScreen("\033[8l"); // explicit: the application already reordered
    mock.writeToScreen("\u05E9\u05DC\u05D5\u05DD");
    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);
    mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    // Drawn exactly as received -- reordering it here would undo the application's own work.
    CHECK(trimmed(renderedLine(buffer.get(), 0)) == U"\u05E9\u05DC\u05D5\u05DD");
}

TEST_CASE("Bidi.pure latin is untouched", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(12) } };

    mock.writeToScreen("hello");
    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);
    mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();

    CHECK(trimmed(renderedLine(buffer.get(), 0)) == U"hello");
}

// ---- Selection stays logical ----

// The whole point of the display-only model: reordering lives in the render buffer, so Selection's
// anchors and extractSelectionText() never see it. What is copied is reading order, even though the
// screen shows the characters in a different one -- and a logically contiguous selection rendering
// as visually DISCONTIGUOUS is correct, not a bug to be fixed.
TEST_CASE("Bidi.selection yields logical order", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(12) } };

    // Hebrew shalom, logical order: shin lamed vav final-mem.
    mock.writeToScreen("שלום");

    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);
    mock.terminal.refreshRenderBuffer();

    // Sanity: the screen really is reordered, so this test is not vacuous.
    REQUIRE(trimmed(renderedLine(mock.terminal.renderBuffer().get(), 0)) == U"םולש");

    mock.terminal.setSelector(std::make_unique<LinearSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(3) });
    mock.terminal.selector()->complete();

    // Logical order, i.e. what a person would paste and read -- NOT the visual order above.
    CHECK(mock.terminal.extractSelectionText() == "שלום");
}

TEST_CASE("Bidi.selection across a direction boundary is logical", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(20) } };

    mock.writeToScreen("abc שלום def");

    mock.terminal.setSelector(std::make_unique<LinearSelection>(
        mock.terminal.selectionHelper(),
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(0) },
        []() {}));
    (void) mock.terminal.selector()->extend(
        CellLocation { .line = LineOffset(0), .column = ColumnOffset(11) });
    mock.terminal.selector()->complete();

    CHECK(mock.terminal.extractSelectionText() == "abc שלום def");
}

// Hidden by default (leading '.'): run with `vtbackend_test "[.perf]"`.
//
// The claim under test is that the >= U+0590 scan keeps ordinary left-to-right output at parity.
// bench-headless cannot answer it -- that harness drives the parser and never reaches
// fillRenderBufferInternal, where the bidi layout is computed -- so the render path is timed here.
TEST_CASE("Bidi.perf refreshRenderBuffer over ASCII", "[.perf]")
{
    auto mock = MockTerm { PageSize { LineCount(40), ColumnCount(120) } };

    for (auto line = 0; line < 40; ++line)
        mock.writeToScreen("The quick brown fox jumps over the lazy dog 0123456789 "
                           "The quick brown fox jumps over the lazy dog 0123456\r\n");

    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);

    auto constexpr Iterations = 2000;
    auto const start = std::chrono::steady_clock::now();
    for (auto i = 0; i < Iterations; ++i)
    {
        mock.terminal.markScreenDirty();
        mock.terminal.refreshRenderBuffer();
    }
    auto const elapsed = std::chrono::steady_clock::now() - start;
    auto const us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    UNSCOPED_INFO("refreshRenderBuffer x" << Iterations << " over 40x120 ASCII: " << us << " us ("
                                          << (double) us / Iterations << " us/frame)");
    CHECK(us > 0);
}

// The bidi hint is a cached per-line flag, so every path that moves cells between lines has to carry
// it. Reflow is that path: without propagation the text lands on a line whose flag still says it
// holds nothing right-to-left, and reordering silently stops.
TEST_CASE("Bidi.reflow carries the bidi hint", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(8) } };
    mock.terminal.setMode(DECMode::TextReflow, true);

    // Long enough to wrap at 8 columns, so resizing has to move cells between lines.
    mock.writeToScreen("abcdשלום");

    mock.terminal.resizeScreen(PageSize { LineCount(3), ColumnCount(20) });

    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);
    mock.terminal.refreshRenderBuffer();

    // The Hebrew must still be reversed after the reflow.
    auto const line = trimmed(renderedLine(mock.terminal.renderBuffer().get(), 0));
    INFO("rendered: " << std::string(line.begin(), line.end()).size() << " codepoints");
    CHECK(line == U"abcdםולש");
}

// The plan wanted TextClusterGrouper's space flush relaxed, on the grounds that word-at-a-time
// grouping cannot express a run of right-to-left words reordering relative to one another. That
// reasoning assumed the grouper does the reordering. It does not: RenderBufferBuilder permutes the
// cells before the grouper ever sees them, so by then the words are already in visual order and a
// space may go on ending a shaping group. This pins that.
TEST_CASE("Bidi.multiple RTL words reorder relative to each other", "[bidi]")
{
    auto mock = MockTerm { PageSize { LineCount(2), ColumnCount(16) } };

    // shalom olam -- two words. Logical: shin lamed vav final-mem, space, ayin vav lamed final-mem.
    mock.writeToScreen("שלום עולם");

    auto constexpr ClockBase = std::chrono::steady_clock::time_point {};
    mock.terminal.tick(ClockBase);
    mock.terminal.refreshRenderBuffer();

    // The whole line reverses, so the SECOND word is drawn first and each word is itself reversed.
    CHECK(trimmed(renderedLine(mock.terminal.renderBuffer().get(), 0))
          == U"םלוע םולש");
}
