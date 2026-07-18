// SPDX-License-Identifier: Apache-2.0

/// @file text-sizing-demo.cpp
/// Demonstrates the kitty text sizing protocol (OSC 66) — text laid out at a size the application
/// chooses, rather than at whatever the terminal measures the characters to be.
///
/// The sequence is:
///
///     OSC 66 ; <key>=<value>:<key>=<value>... ; <text> ST
///
/// Note the metadata pairs are separated by COLONS while the semicolon separates metadata from
/// text -- unusual for an OSC, and the easiest part of the protocol to get wrong.
///
/// | key | range | meaning                                                              |
/// |-----|-------|----------------------------------------------------------------------|
/// | `s` | 1..7  | scale: the block is `s` cells tall                                   |
/// | `w` | 0..7  | width in cells; the block is `s * w` wide. 0 = derive from the text   |
/// | `n` | 0..15 | fractional scale numerator -- changes the DRAWN size, never the cells |
/// | `d` | 0..15 | fractional scale denominator; must exceed `n`                         |
/// | `v` | 0..2  | vertical alignment of a fractionally-scaled glyph: top/bottom/center  |
/// | `h` | 0..2  | horizontal alignment: left/right/center                               |
///
/// Each section below prints a caption and then the thing it demonstrates, so that what you should
/// be looking for is on screen next to what you are looking at. The earlier sections take one key
/// at a time; the later ones put whole runs of text through it, which is how an application would
/// actually use the protocol -- longer sentences at several scales, sizes mixed within one line,
/// mixed scripts in one payload, and a run long enough to meet the right margin.
///
/// Two settings change what you see, and it is worth running this under both:
///
///     text_scaling_method: stretch      (default) magnifies the ordinary-size glyph -- fast, and
///                                       visibly softer as the scale grows
///     text_scaling_method: rerasterize  asks the font for the glyph at the larger size -- crisp at
///                                       any scale, at the cost of memory and rasterization
///
/// Usage: text-sizing-demo

#include <string>
#include <string_view>

#include <unistd.h>

using namespace std::string_view_literals;

namespace
{

void writeToTTY(std::string_view s) noexcept
{
    ::write(STDOUT_FILENO, s.data(), s.size());
}

/// Emits one OSC 66 request.
/// @param metadata  the colon-separated key=value pairs, without the surrounding OSC.
/// @param text      the text to lay out at that size.
void sized(std::string_view metadata, std::string_view text)
{
    writeToTTY("\033]66;");
    writeToTTY(metadata);
    writeToTTY(";");
    writeToTTY(text);
    writeToTTY("\033\\");
}

/// A section heading, so each demonstration says what it is.
void heading(std::string_view title)
{
    writeToTTY("\r\n\033[1;4m");
    writeToTTY(title);
    writeToTTY("\033[m\r\n\r\n");
}

void note(std::string_view text)
{
    writeToTTY("\033[2m");
    writeToTTY(text);
    writeToTTY("\033[m\r\n");
}

/// Leaves enough blank lines for a block `rows` cells tall, so the next section does not overwrite
/// the rows it reaches down into.
void advancePast(int rows)
{
    for (auto i = 0; i < rows; ++i)
        writeToTTY("\r\n");
}

void demoWidth()
{
    heading("w = width in cells");
    note("Each block is exactly as wide as it asks for, whatever the text measures.");

    for (auto width = 1; width <= 5; ++width)
    {
        writeToTTY("  ");
        sized("w=" + std::to_string(width), "W");
        writeToTTY("|"); // marks where the block ended, so the width is countable
        writeToTTY("  w=" + std::to_string(width) + "\r\n");
    }
}

void demoScale()
{
    heading("s = scale");
    note("A scaled block is s cells WIDE and s cells TALL. Text below it is pushed clear.");

    for (auto scale = 1; scale <= 4; ++scale)
    {
        sized("s=" + std::to_string(scale), "A");
        writeToTTY("  s=" + std::to_string(scale) + "\r\n");
        advancePast(scale - 1);
    }
}

void demoScaleTimesWidth()
{
    heading("s and w together");
    note("The block is s*w columns wide and s rows tall -- the two multiply.");

    sized("s=2:w=3", "B");
    writeToTTY("  s=2 w=3 -> 6 columns, 2 rows\r\n");
    advancePast(1);

    sized("s=3:w=2", "C");
    writeToTTY("  s=3 w=2 -> 6 columns, 3 rows\r\n");
    advancePast(2);
}

void demoWithoutWidth()
{
    heading("w = 0 (the default)");
    note("Without an explicit width each character becomes its own s-wide block,");
    note("so this is three separate blocks rather than one wide one.");

    sized("s=2", "abc");
    writeToTTY("  s=2 with three characters\r\n");
    advancePast(1);
}

void demoFractional()
{
    heading("n / d = fractional scale");
    note("A fraction changes how large the glyph is DRAWN without changing how many");
    note("cells it occupies -- every block below is the same size in cells.");

    for (auto const& [fraction, label]: { std::pair { "n=1:d=2"sv, "1/2 size"sv },
                                          std::pair { "n=2:d=3"sv, "2/3 size"sv },
                                          std::pair { "n=3:d=4"sv, "3/4 size"sv } })
    {
        sized(std::string("s=3:w=1:") + std::string(fraction), "F");
        writeToTTY("  ");
        writeToTTY(label);
        writeToTTY("\r\n");
        advancePast(2);
    }
}

void demoAlignment()
{
    heading("v / h = alignment within the block");
    note("Where a fractionally-scaled glyph sits in the cells it was given.");
    note("v: 0=top 1=bottom 2=center   h: 0=left 1=right 2=center");

    for (auto v = 0; v <= 2; ++v)
    {
        for (auto h = 0; h <= 2; ++h)
        {
            sized("s=3:w=1:n=1:d=3:v=" + std::to_string(v) + ":h=" + std::to_string(h), "x");
            writeToTTY(" ");
        }
        writeToTTY("  v=" + std::to_string(v) + ", h=0/1/2\r\n");
        advancePast(2);
    }
}

void demoEdges()
{
    heading("Edges");
    note("A block never splits across a line: it moves whole, or is dropped if it");
    note("could never fit. Watch the right margin.");

    // Fill most of the line, then ask for a block that cannot fit in what is left.
    writeToTTY("................................................................\r\n");
    writeToTTY("..............................................................");
    sized("w=6", "E");
    writeToTTY("  <- moved to its own line rather than being cut in half\r\n");
}

void demoOverwrite()
{
    heading("Overwriting");
    note("Touching any part of a block destroys all of it -- half a glyph is not a");
    note("thing that can be drawn. The block below is overwritten from its middle.");

    sized("s=2:w=3", "Z");
    writeToTTY("  a 6x2 block\r\n");
    advancePast(1);

    writeToTTY("\033[3A");   // up to the block's first row
    writeToTTY("\033[3C");   // into the middle of it
    writeToTTY("\033[31m#"); // and write over it
    writeToTTY("\033[m");
    writeToTTY("\033[3B\r\n");
    note("(the whole block should be gone, leaving only the red #)");
}

void demoUnicode()
{
    heading("Scaled text is still text");
    note("Wide characters, emoji and combining marks keep their own widths inside a");
    note("scaled block -- the scale multiplies whatever the text already measured.");

    sized("s=2", "中"); // already two columns, so this becomes four
    writeToTTY("  CJK: 2 columns x s=2 -> 4\r\n");
    advancePast(1);

    sized("s=2", "😀");
    writeToTTY("  emoji\r\n");
    advancePast(1);

    sized("s=2", "क्नि");
    writeToTTY("  Devanagari conjunct\r\n");
    advancePast(1);
}

void demoLongerRuns()
{
    heading("Longer runs at each scale");
    note("The same sentence three times. Without an explicit w each character becomes its");
    note("own s-wide block, so a whole run scales without the application measuring it.");

    for (auto const scale: { 1, 2, 3 })
    {
        sized("s=" + std::to_string(scale), "Sized text scales");
        writeToTTY("  s=" + std::to_string(scale) + "\r\n");
        advancePast(scale - 1);
    }
}

void demoMixedSizesInOneLine()
{
    heading("Different sizes on one line");
    note("Sized and unsized text share a line; each block keeps the size it asked for.");

    writeToTTY("plain ");
    sized("s=2", "twice");
    writeToTTY(" plain ");
    sized("s=3", "thrice");
    writeToTTY(" plain\r\n");
    advancePast(2);
}

void demoLongerMixedScript()
{
    heading("A longer run of mixed scripts");
    note("One OSC 66 payload holding Latin, CJK and emoji. Each cluster keeps its own");
    note("width inside the run -- the CJK and the emoji stay twice as wide as the Latin.");

    sized("s=2", "Hello 世界 😀 ok");
    writeToTTY("  s=2\r\n");
    advancePast(1);
}

void demoWrappingRun()
{
    heading("A run that reaches the right margin");
    note("A block never splits: one that does not fit moves to the next line whole.");
    note("Count the blocks -- none is cut in half at the margin.");

    // Thirty 3-wide blocks: past 80 columns one of them meets the margin and moves down whole.
    //
    // Each is one row tall on purpose. A taller block claims the rows beneath it, so a wrapping
    // run of those would land on cells its own earlier blocks own -- and touching a block destroys
    // it. That interaction deserves showing on its own rather than tangled into wrapping.
    for (auto i = 0; i < 30; ++i)
        sized("w=3", i % 2 == 0 ? "#" : "=");
    writeToTTY("\r\n");
}

} // namespace

int main()
{
    writeToTTY("\033[2J\033[H"); // clear, home
    // Split so that the SGR introducer does not run into the word that follows it, which a spell
    // checker reading this file would otherwise take for a single misspelled token.
    writeToTTY("\033[1m"
               "kitty text sizing protocol (OSC 66)\033[m\r\n");
    note("If every block below looks like ordinary one-cell text, the terminal does not");
    note("implement OSC 66 and is showing you the payloads with the escapes stripped.");

    demoWidth();
    demoScale();
    demoScaleTimesWidth();
    demoWithoutWidth();
    demoFractional();
    demoAlignment();
    demoEdges();
    demoOverwrite();
    demoUnicode();
    demoLongerRuns();
    demoMixedSizesInOneLine();
    demoLongerMixedScript();
    demoWrappingRun();

    writeToTTY("\r\n");
    note("Re-run with text_scaling_method: rerasterize to compare glyph sharpness.");
    return 0;
}
