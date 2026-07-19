// SPDX-License-Identifier: Apache-2.0

/// @file bidi-demo.cpp
/// Demonstrates bidirectional text -- Hebrew and Arabic mixed with Latin -- and the escape
/// sequences that control how a terminal lays it out.
///
/// BiDi is *display-only*. The grid stores characters in LOGICAL order, the order they were
/// written in and the order the application addresses with escape sequences. Reordering happens on
/// the way to the screen. That is why copying a selection out of mixed text yields the logical
/// string even though the screen shows it reordered, and why a mouse click has to be mapped back
/// the other way.
///
/// A paragraph, not a row, is the unit the algorithm works on: it runs from one hard newline to the
/// next, so a wrapped line continues the paragraph it started. Section 5 is the case that catches
/// an implementation which reorders row by row.
///
/// The escape sequences, from the terminal-wg recommendation
/// (<https://terminal-wg.pages.freedesktop.org/bidi/>), which VTE also implements:
///
/// | Sequence            | Meaning                                              | Default |
/// |---------------------|------------------------------------------------------|---------|
/// | `CSI 8 h` / `8 l`   | BDSM: implicit (terminal reorders) / explicit (app does) | implicit |
/// | `CSI Ps SP k`       | SCP: character path. 1 = LTR, 2 = RTL, 0 = restore     | 0       |
/// | `CSI ? 2500 h/l`    | mirror box-drawing glyphs in an RTL context           | reset   |
/// | `CSI ? 2501 h/l`    | autodetect paragraph direction (first strong char)    | reset   |
/// | `CSI ? 1243 h/l`    | swap Left/Right arrow keys inside an RTL paragraph    | set     |
///
/// Those numbers are provisional: the recommendation is a draft and says so.
///
/// HOW TO READ THE OUTPUT
///
/// Each case prints what the terminal actually draws, then the reference rendering underneath.
/// The reference is wrapped in U+202D LEFT-TO-RIGHT OVERRIDE ... U+202C POP DIRECTIONAL FORMAT so
/// that it is shown verbatim -- without that, a terminal which HAS implemented BiDi would reorder
/// the already-reordered reference and display it wrong, and the demo would accuse a correct
/// terminal of being broken.
///
/// The reference renderings below were computed with libunicode's UAX#9 implementation, which
/// passes BidiTest.txt and BidiCharacterTest.txt in full.
///
/// FONTS
///
/// Use a font covering both scripts, or the Arabic joining test measures fallback rather than
/// shaping. On Fedora, FreeMono (gnu-freefont) is the only monospace face covering Hebrew AND
/// Arabic with real `arab` + `hebr` GSUB tables:
///
///     contour display.font.regular="FreeMono"
///
/// Usage: bidi-demo

#include <string_view>

#include <unistd.h>

using namespace std::string_view_literals;

namespace
{

// U+202D LRO ... U+202C PDF: render exactly as stored, whatever the terminal's BiDi state.
constexpr auto Verbatim = "‭"sv;
constexpr auto VerbatimEnd = "‬"sv;

void write(std::string_view text)
{
    ::write(STDOUT_FILENO, text.data(), text.size());
}

void writeLine(std::string_view text = ""sv)
{
    write(text);
    write("\r\n"sv);
}

void heading(std::string_view title)
{
    writeLine();
    write("\033[1;4m"sv); // bold + underline
    write(title);
    writeLine("\033[m"sv);
}

void note(std::string_view text)
{
    write("\033[2m"sv); // dim
    write(text);
    writeLine("\033[m"sv);
}

/// Prints one case: a caption, the text itself, and the rendering a conforming terminal produces.
void showCase(std::string_view caption, std::string_view logical, std::string_view expectedVisual)
{
    writeLine();
    note(caption);
    write("   drawn    : "sv);
    writeLine(logical);
    write("   should be: "sv);
    write(Verbatim);
    write(expectedVisual);
    write(VerbatimEnd);
    writeLine();
}

void basicRuns()
{
    heading("1. Pure right-to-left runs"sv);
    note("The letters run right to left, so the first character typed sits at the RIGHT edge.");

    showCase("Hebrew: shalom olam"sv, "שלום עולם"sv, "םלוע םולש"sv);

    showCase("Arabic: al-arabiyya -- letters must JOIN into a connected word, not stand apart. "
             "This is the shaping test, and it is the one a missing font silently fails."sv,
             "العربية"sv,
             "ةيبرعلا"sv);
}

void mixedDirection()
{
    heading("2. Mixed direction, and numbers inside it"sv);
    note("The paragraph direction comes from the FIRST strong character (rules P2/P3), so this");
    note("paragraph is left-to-right and the Hebrew is an RTL island inside it.");

    // Levels: abc=0, sp=0, Hebrew=1, sp=1, 123=2, sp=0, def=0.
    // European digits inside an RTL run resolve to an even level, so "123" reads left-to-right and
    // ends up visually LEFT of the Hebrew despite following it logically. Rules W2/W7 then I1.
    showCase("Digits keep their own direction and move: \"123\" follows the Hebrew logically "
             "but appears before it on screen."sv,
             "abc שלום 123 def"sv,
             "abc 123 םולש def"sv);

    showCase("Latin island inside a Hebrew paragraph -- here the paragraph is RTL, so the "
             "outermost order is reversed and \"abc\" stays left-to-right within it."sv,
             "שלום abc עולם"sv,
             "םלוע abc םולש"sv);

    showCase("Arabic with digits."sv, "العدد 42 هنا"sv, "انه 42 ددعلا"sv);
}

void mirroredBrackets()
{
    heading("3. Mirrored brackets"sv);
    note("A bracket in a right-to-left run is drawn as its MIRROR, so that it still opens and");
    note("closes around the text it encloses. Get the reordering right but skip the mirroring and");
    note("the parentheses come out backwards -- the text below would read )shalom(.");

    showCase("Parentheses around Hebrew: the pair must still enclose the word."sv, "(שלום)"sv, "(םולש)"sv);

    showCase("Brackets around a mixed run."sv, "[שלום abc]"sv, "[abc םולש]"sv);
}

void wrappedParagraph()
{
    heading("4. A paragraph wider than the terminal"sv);
    note("The algorithm runs over the whole PARAGRAPH, not one row at a time. If the terminal");
    note("reorders each row independently, this renders differently after a resize -- which is the");
    note("bug this case exists to catch. Resize the window and watch: the reordering must stay");
    note("consistent with the paragraph, not with wherever the wrap happened to land.");
    writeLine();

    write("   "sv);
    writeLine("שלום עולם this is a long line mixing Hebrew שלום and Latin text that will wrap "
              "when the terminal is narrower than it is, continuing עולם past the edge."sv);
}

void escapeSequences()
{
    heading("5. The control sequences"sv);

    note("BDSM -- who does the reordering. CSI 8 h leaves it to the terminal (the default);");
    note("CSI 8 l says the application has already reordered and the terminal must not touch it.");
    writeLine();
    write("   implicit (CSI 8 h) : "sv);
    write("\033[8h"sv);
    write("שלום abc"sv);
    writeLine();
    write("   explicit (CSI 8 l) : "sv);
    write("\033[8l"sv);
    write("שלום abc"sv);
    write("\033[8h"sv); // restore the default before moving on
    writeLine();
    note("   Under 'explicit' the text is drawn in logical order, unreordered.");

    writeLine();
    note("SCP -- forcing the paragraph direction regardless of the first strong character.");
    writeLine();
    write("   LTR  (CSI 1 SP k)  : "sv);
    write("\033[1 k"sv);
    writeLine("abc שלום 123"sv);
    write("   RTL  (CSI 2 SP k)  : "sv);
    write("\033[2 k"sv);
    writeLine("abc שלום 123"sv);
    write("\033[0 k"sv); // restore the terminal default
    note("   Same characters, different base direction -- so a different visual order.");

    writeLine();
    note("Autodetect (CSI ? 2501 h): pick each paragraph's direction from its first strong");
    note("character. With it reset, the paragraph direction is whatever SCP last selected.");
    writeLine();
    write("\033[?2501h"sv);
    write("   autodetect on      : "sv);
    writeLine("שלום abc"sv);
    write("\033[?2501l"sv);

    writeLine();
    note("Box drawing (CSI ? 2500 h): mirror box-drawing glyphs in a right-to-left context, so");
    note("that a box's corners still point the way the box is going.");
    writeLine();
    write("   mirroring off      : "sv);
    write("\033[?2500l"sv);
    writeLine("┌─┐ שלום"sv);
    write("   mirroring on       : "sv);
    write("\033[?2500h"sv);
    writeLine("┌─┐ שלום"sv);
    write("\033[?2500l"sv);
}

void interactiveChecks()
{
    heading("6. Checks that need a keyboard and a mouse"sv);
    note("These cannot be shown by printing; they are what to try by hand.");
    writeLine();
    writeLine("   Text to try it on:  abc שלום 123 def"sv);
    writeLine();
    note("   Arrow keys (CSI ? 1243, set by default): inside an RTL run, Left and Right swap, so");
    note("   that the cursor keeps moving the way the key points rather than the way memory runs.");
    note("   Walk the cursor across the whole line and check it never jumps unexpectedly.");
    writeLine();
    note("   Click-to-position: clicking a glyph must land the cursor on THAT character. This is");
    note("   the inverse mapping, visual back to logical, and it is easy to get right in one");
    note("   direction only.");
    writeLine();
    note("   Selection and copy: select across the Hebrew/Latin boundary. The highlight may look");
    note("   DISCONTIGUOUS -- that is correct, because a logically contiguous range need not be");
    note("   visually contiguous. What is pasted must be the LOGICAL string: abc שלום 123 def");
}

} // namespace

int main()
{
    write("\033[1mBiDi demo\033[m -- bidirectional text and the sequences that control it.\r\n"sv);
    note("Each case shows what your terminal drew, then what a conforming terminal draws.");
    note("If the two lines differ, the feature under test is not implemented (or is wrong).");

    basicRuns();
    mixedDirection();
    mirroredBrackets();
    wrappedParagraph();
    escapeSequences();
    interactiveChecks();

    writeLine();
    return 0;
}
