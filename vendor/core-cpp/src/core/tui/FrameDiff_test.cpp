// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Buffer.hpp>
#include <core/tui/FrameDiff.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

using core::tui::Buffer;
using core::tui::DiffAddressing;
using core::tui::diffFrames;
using core::tui::DiffHyperlinks;
using core::tui::DiffImages;
using core::tui::DiffOptions;
using core::tui::DiffReflow;
using core::tui::DiffRepaint;
using core::tui::DiffRowMarks;
using core::tui::Rect;
using core::tui::Style;

namespace
{

/// Everything out of band switched on, so "nothing changed" is checked against all of it.
auto everything() -> DiffOptions
{
    return DiffOptions {
        .hyperlinks = DiffHyperlinks::Emit,
        .images = DiffImages::Emit,
        .rowMarks = DiffRowMarks::Emit,
    };
}

/// A style with only bold set.
auto bold() -> Style
{
    auto style = Style {};
    style.bold = true;
    return style;
}

/// A style with only the foreground set, to 256-colour index @p index.
auto foreground(std::uint8_t index) -> Style
{
    auto style = Style {};
    style.fg = index;
    return style;
}

/// A style with only inverse video set.
auto inverse() -> Style
{
    auto style = Style {};
    style.inverse = true;
    return style;
}

/// A frame of @p rows x @p cols with @p text at the top-left.
auto frame(int rows, int cols, std::string_view text = {}, Style const& style = {}) -> Buffer
{
    auto buffer = Buffer { rows, cols };
    if (!text.empty())
        buffer.putString(0, 0, text, style);
    return buffer;
}

} // namespace

TEST_CASE("diffFrames: identical frames diff to nothing")
{
    auto previous = frame(3, 10, "same", bold());
    previous.addHyperlink(Rect { .x = 0, .y = 0, .width = 4, .height = 1 }, "https://example.org/");
    previous.addMark(1, 2, "\033]133;A\033\\");
    previous.addImage(Rect { .x = 6, .y = 1, .width = 2, .height = 2 }, "#0;2;0;0;0~-");
    auto const next = previous;

    CHECK(diffFrames(previous, next).empty());
    CHECK(diffFrames(previous, next, everything()).empty());
}

TEST_CASE("diffFrames: one changed cell is one cursor move and that cell")
{
    auto const previous = frame(3, 10);
    auto next = previous;
    next.putString(1, 4, "x", {});

    // CUP to row 2, column 5 (1-based), the grapheme, and the SGR reset every styled write ends with.
    CHECK(diffFrames(previous, next) == "\033[2;5Hx\033[m");
}

TEST_CASE("diffFrames: a wide character is written once, its continuation cell not at all")
{
    auto const previous = frame(1, 6);
    auto const next = frame(1, 6, "\xE4\xB8\xAD"); // U+4E2D, two columns

    CHECK(diffFrames(previous, next) == "\033[1;1H\xE4\xB8\xAD\033[m");
}

TEST_CASE("diffFrames: two narrow characters replacing a wide one are both written")
{
    auto const previous = frame(1, 6, "\xE4\xB8\xAD");
    auto const next = frame(1, 6, "ab");

    CHECK(diffFrames(previous, next) == "\033[1;1Ha\033[mb\033[m");
}

TEST_CASE("diffFrames: a style change alone repaints the cell with its SGR")
{
    auto const previous = frame(1, 4, "ab");
    auto const next = frame(1, 4, "ab", bold());

    CHECK(diffFrames(previous, next) == "\033[1;1H\033[1ma\033[m\033[1mb\033[m");
}

TEST_CASE("diffFrames: a full-screen change matches the recorded golden")
{
    auto previous = Buffer { 2, 3 };
    previous.putString(0, 0, "abc", {});
    previous.putString(1, 0, "def", {});
    auto next = Buffer { 2, 3 };
    next.putString(0, 0, "XYZ", foreground(1));
    next.putString(1, 0, "\xE4\xB8\xADQ", inverse());

    // Recorded from this implementation and read cell by cell: a run of ASCII cells is reached by one
    // move, a new row and the cell after a wide character each by their own, the wide character's
    // continuation is skipped, and each write carries its own SGR and reset.
    auto const golden = std::string { "\033[1;1H\033[38;5;1mX\033[m"
                                      "\033[38;5;1mY\033[m"
                                      "\033[38;5;1mZ\033[m"
                                      "\033[2;1H\033[7m\xE4\xB8\xAD\033[m"
                                      "\033[2;3H\033[7mQ\033[m" };
    CHECK(diffFrames(previous, next) == golden);
}

TEST_CASE("diffFrames: an unchanged cell between two changed ones is moved over")
{
    auto const previous = frame(1, 4, "abc");
    auto const next = frame(1, 4, "XbZ");

    CHECK(diffFrames(previous, next) == "\033[1;1HX\033[m\033[1;3HZ\033[m");
}

TEST_CASE("diffFrames: a cell after a non-ASCII narrow grapheme is reached by an absolute move")
{
    // A terminal may disagree about a non-ASCII grapheme's width, so the cursor is not trusted after one.
    auto const previous = frame(1, 4);
    auto const next = frame(1, 4, "\xC3\xA9x");

    CHECK(diffFrames(previous, next) == "\033[1;1H\xC3\xA9\033[m\033[1;2Hx\033[m");
}

TEST_CASE("diffFrames: the last column's write is not trusted to leave the cursor on the row")
{
    // A terminal holds the cursor on the last column with a wrap pending; the next row is moved to.
    auto const previous = Buffer { 2, 2 };
    auto next = Buffer { 2, 2 };
    next.putString(0, 0, "ab", {});
    next.putString(1, 0, "cd", {});

    CHECK(diffFrames(previous, next) == "\033[1;1Ha\033[mb\033[m\033[2;1Hc\033[md\033[m");
}

TEST_CASE("diffFrames: DiffRepaint::All writes every cell of identical frames")
{
    auto const previous = frame(1, 2, "ok");

    CHECK(diffFrames(previous, previous, DiffOptions { .repaint = DiffRepaint::All })
          == "\033[1;1Ho\033[mk\033[m");
}

TEST_CASE("diffFrames: the area bounds what is written")
{
    auto const previous = frame(2, 4);
    auto next = previous;
    next.putString(0, 0, "a", {});
    next.putString(1, 3, "b", {});

    CHECK(diffFrames(previous, next, DiffOptions { .area = Rect { .x = 2, .y = 1, .width = 2, .height = 1 } })
          == "\033[2;4Hb\033[m");
}

TEST_CASE("diffFrames: an area wholly outside the frame writes nothing")
{
    auto const previous = frame(2, 4);
    auto const next = frame(2, 4, "abcd");

    // Below, to the right, and above-left: each clips to nothing rather than to a reversed range.
    for (auto const area: { Rect { .x = 0, .y = 5, .width = 4, .height = 2 },
                            Rect { .x = 9, .y = 0, .width = 2, .height = 2 },
                            Rect { .x = -5, .y = -5, .width = 3, .height = 3 } })
        CHECK(diffFrames(previous, next, DiffOptions { .repaint = DiffRepaint::All, .area = area }).empty());
}

TEST_CASE("diffFrames: a cell beyond the previous frame counts as changed")
{
    auto const previous = frame(1, 1, "a");
    auto const next = frame(1, 2, "ab");

    CHECK(diffFrames(previous, next) == "\033[1;2Hb\033[m");
}

TEST_CASE("diffFrames: a retargeted link repaints only when hyperlinks are emitted")
{
    auto previous = frame(1, 3, "doc");
    previous.addHyperlink(Rect { .x = 0, .y = 0, .width = 3, .height = 1 }, "https://a.example/");
    auto next = frame(1, 3, "doc");
    next.addHyperlink(Rect { .x = 0, .y = 0, .width = 3, .height = 1 }, "https://b.example/");

    // Cells alone are identical: without the option the link is not this diff's to see.
    CHECK(diffFrames(previous, next).empty());

    auto const bytes = diffFrames(previous, next, DiffOptions { .hyperlinks = DiffHyperlinks::Emit });
    CHECK(bytes.starts_with("\033[1;1H\033]8;id="));
    CHECK(bytes.contains(";https://b.example/\033\\d\033[m"));
    CHECK(bytes.ends_with("\033]8;;\033\\")); // Closed at the end of the row.
}

TEST_CASE("diffFrames: a new image is written where it starts, over cells it then leaves alone")
{
    auto const previous = frame(2, 4);
    auto next = previous;
    next.putString(0, 0, "abcd", {});
    next.addImage(Rect { .x = 1, .y = 0, .width = 2, .height = 2 }, "PIX");

    CHECK(diffFrames(previous, next, DiffOptions { .images = DiffImages::Emit })
          == "\033[1;1Ha\033[m\033[1;4Hd\033[m\033[1;2H\033P0;1qPIX\033\\");
}

TEST_CASE("diffFrames: an image that moves repaints the cells it left and is written again")
{
    auto previous = frame(1, 4);
    previous.addImage(Rect { .x = 0, .y = 0, .width = 1, .height = 1 }, "PIX");
    auto next = frame(1, 4);
    next.addImage(Rect { .x = 3, .y = 0, .width = 1, .height = 1 }, "PIX");

    CHECK(diffFrames(previous, next, DiffOptions { .images = DiffImages::Emit })
          == "\033[1;1H \033[m\033[1;4H\033P0;1qPIX\033\\");
}

TEST_CASE("diffFrames: a changed cell is preceded by the row mark governing it")
{
    auto const previous = frame(1, 6);
    auto next = previous;
    next.putString(0, 4, "z", {});
    next.addMark(0, 2, "<M>");

    // The marks changed, so the row repaints whole; the mark goes out at its own column, once, before
    // the first cell of its segment.
    auto const bytes = diffFrames(previous, next, DiffOptions { .rowMarks = DiffRowMarks::Emit });
    CHECK(bytes.starts_with("\033[1;1H \033[m \033[m<M> \033[m"));
    CHECK(bytes.ends_with("z\033[m \033[m"));
    CHECK_FALSE(diffFrames(previous, next).contains("<M>"));
}

TEST_CASE("diffFrames: relative addressing walks the rows from the cursor")
{
    auto const previous = frame(2, 3, "abc");
    auto next = previous;
    next.putString(0, 1, "X", {});

    // Carriage return, a move over the unchanged cell, the changed one, a move over the next, an
    // erase to the end of the row, and a line feed to the row below, which is unchanged too.
    auto const options =
        DiffOptions { .addressing = DiffAddressing::Relative, .reflow = DiffReflow::Inhibit };
    CHECK(diffFrames(previous, next, options)
          == "\r\033[?2028l\033[1CX\033[m\033[1C\033[K\n"
             "\r\033[?2028l\033[1C\033[1C\033[1C\033[K");
}
