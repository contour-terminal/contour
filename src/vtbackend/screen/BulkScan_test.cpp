// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/AsciiText.hpp>
#include <vtbackend/screen/ScreenTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/Workloads.hpp>

#include <vtparser/BulkScan.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
/// A terminal of @p pageSize whose parser runs with @p bulkScan.
MockTerm<> makeTerminal(PageSize pageSize, vtparser::BulkScan bulkScan)
{
    auto settings = MockTerm<>::createSettings(pageSize, HistoryLimits::plain(LineCount(20)), 1024);
    settings.parserBulkScan = bulkScan;
    return MockTerm<> { std::move(settings) };
}

/// Everything a bulk path could get wrong, one line of text per grid line: each cell's codepoints,
/// width, flags, colours and hyperlink; each line's flags; and the cursor with its pending wrap.
std::string dumpScreen(Screen const& screen)
{
    auto out = std::string {};
    for (auto const line: std::views::iota(-screen.historyLineCount().value, screen.pageSize().lines.value))
    {
        out += std::format("{:>3} [{}]", line, screen.grid().lineAt(LineOffset(line)).flags());
        for (auto const column: std::views::iota(0, screen.pageSize().columns.value))
        {
            auto const cell = screen.at(LineOffset(line), ColumnOffset(column));
            out += " |";
            for (auto const i: std::views::iota(size_t { 0 }, cell.codepointCount()))
                out += std::format(" {:04X}", static_cast<uint32_t>(cell.codepoint(i)));
            out += std::format(" w{} {} {} {} {} h{}",
                               cell.width(),
                               cell.flags(),
                               cell.foregroundColor(),
                               cell.backgroundColor(),
                               cell.underlineColor(),
                               cell.hyperlink().value);
        }
        out += '\n';
    }
    out += std::format("cursor {} wrapPending {}\n", screen.cursor().position, screen.cursor().wrapPending);
    return out;
}

/// Writes @p setup and then @p text, cut into two reads at every byte, to a terminal with bulk
/// scanning and to one without. Requires the two screens to agree after every cut.
///
/// Both terminals are reused across cuts and reset with RIS in between. They receive identical
/// input, so whatever RIS leaves behind it leaves behind in both.
void checkSameScreenAtEverySplit(PageSize pageSize, std::string_view setup, std::string_view text)
{
    auto bulk = makeTerminal(pageSize, vtparser::BulkScan::Enabled);
    auto reference = makeTerminal(pageSize, vtparser::BulkScan::Disabled);
    for (auto const split: std::views::iota(size_t { 0 }, text.size() + 1))
    {
        for (auto* const mock: { &bulk, &reference })
        {
            mock->writeToScreen("\033c"sv);
            mock->writeToScreen(setup);
            mock->writeToScreen(text.substr(0, split));
            mock->writeToScreen(text.substr(split));
        }
        INFO(std::format("split at byte {}", split));
        CHECK(dumpScreen(bulk.terminal.currentScreen()) == dumpScreen(reference.terminal.currentScreen()));
    }
}
} // namespace

TEST_CASE("BulkScan.TerminalTakesItFromSettings", "[BulkScan]")
{
    auto const pageSize = PageSize { LineCount(4), ColumnCount(12) };
    CHECK(makeTerminal(pageSize, vtparser::BulkScan::Disabled).terminal.parser().bulkScan()
          == vtparser::BulkScan::Disabled);
    CHECK(MockTerm { pageSize }.terminal.parser().bulkScan() == vtparser::BulkScan::Enabled);
}

TEST_CASE("BulkScan.PlainStreamGivesTheSameScreen", "[BulkScan]")
{
    checkSameScreenAtEverySplit(PageSize { LineCount(4), ColumnCount(12) },
                                ""sv,
                                "Hello \033[1;31mWorld\033[m \xC3\xA4\xC3\xB6 e\xCC\x81!"sv);
}

TEST_CASE("BulkScan.AnInvalidByteInsideARunGivesTheSameScreen", "[BulkScan]")
{
    // The bulk text path hands a byte that is not UTF-8 on inside its run; the screen must show the
    // U+FFFD the state machine prints for it. Also where the byte ends the run: libunicode's text
    // scanner stops in front of it (0.9.4), where 0.9.3 consumed it without returning it.
    auto const text = GENERATE(values<std::string_view>({
        "\xFF\xE2\x94\x82X"sv, // inside a run
        "\xE2\x94\x82X\xFF"sv, // ending one
    }));
    auto const setup = GENERATE(values<std::string_view>({ ""sv, "\033[?1049h"sv }));
    INFO(std::format("setup \"{}\" text \"{}\"", core::escape(setup), core::escape(text)));
    checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(10) }, setup, text);
}

TEST_CASE("BulkScan.RegionalIndicatorPairsGiveTheSameScreen", "[BulkScan]")
{
    // GB12/GB13: regional indicators pair up counting from the first one after a break, whatever
    // precedes it -- ASCII, other text, or the end of a read.
    auto const texts = std::array {
        "flags \xF0\x9F\x87\xA9\xF0\x9F\x87\xAA\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 after"sv,     // DE US
        "odd \xF0\x9F\x87\xA9\xF0\x9F\x87\xAA\xF0\x9F\x87\xBA after"sv,                       // DE, a lone U
        "\xC3\xA4\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA\xC3\xB6\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"sv, // after letters
    };
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
        for (auto const text: texts)
        {
            INFO(std::format(
                "{}, text \"{}\"", screenSelect.empty() ? "primary" : "alternate", core::escape(text)));
            checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(40) }, screenSelect, text);
        }

    // And what both must show: "flags ", then DE in one cell and US in the next.
    auto mock = makeTerminal(PageSize { LineCount(3), ColumnCount(40) }, vtparser::BulkScan::Enabled);
    mock.writeToScreen("flags \xF0\x9F\x87\xA9\xF0\x9F\x87\xAA\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"sv);
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(6)).codepoints()
          == U"\U0001F1E9\U0001F1EA");
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(8)).codepoints()
          == U"\U0001F1FA\U0001F1F8");
}

TEST_CASE("BulkScan.EditsOfShapedLinesGiveTheSameScreen", "[BulkScan]")
{
    // The surface F1's gate opened, on both screens: every edit that leaves a line Mixed or Spanning,
    // then ASCII written into the edited line again.
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
        for (auto const& edit: test::EditsOfShapedLines)
        {
            INFO(std::format("{}, {}", screenSelect.empty() ? "primary" : "alternate", edit.name));
            checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(10) },
                                        std::string(screenSelect) + std::string(edit.bytes),
                                        "\033[1;1Hxyz"sv);
        }
}

TEST_CASE("BulkScan.TextOnTheOpenedSurfaceGivesTheSameScreen", "[BulkScan]")
{
    struct Writing
    {
        std::string_view name;  ///< What the case covers, for the failure message.
        std::string_view setup; ///< Written in one read.
        std::string_view text;  ///< Cut into two reads at every byte.
    };
    auto const writings = std::array {
        Writing { .name = "ASCII after a cluster",
                  .setup = ""sv,
                  .text = "e\xCC\x81"
                          "abc\033[1;1Hxy"sv },
        Writing { .name = "non-ASCII text after a coloured run",
                  .setup = "\033[44mab\033[m"sv,
                  .text = "\xC3\xA4\xE4\xB8\xAD e\xCC\x81"sv },
        Writing { .name = "a wrap pending left of a widened margin, onto a wide character",
                  .setup = "\033[2;1Hab\xE4\xB8\xAD\033[?69h\033[1;5sabcde\033[?69l"sv,
                  .text = "xyz"sv },
        Writing { .name = "a single shift into US-ASCII over DEC graphics",
                  .setup = "\033(0\033*B"sv,
                  .text = "\033Nqqq"sv },
    };
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
        for (auto const& writing: writings)
        {
            INFO(std::format("{}, {}", screenSelect.empty() ? "primary" : "alternate", writing.name));
            checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(10) },
                                        std::string(screenSelect) + std::string(writing.setup),
                                        writing.text);
        }
}

TEST_CASE("BulkScan.AWrapRestoredAfterAWideningResizeGivesTheSameScreen", "[BulkScan]")
{
    // DECSC with a wrap pending on the last column, then DECCOLM widens the page in-band (DECNCSM keeps
    // its contents) and DECRC restores the wrap, now pending left of the right margin. The next line
    // holds a wide character for the wrapped text to land on.
    auto const setup = std::format("\033[?40h\033[?95h\033[2;1Hab\xE4\xB8\xAD\033[1;1H{}\0337\033[?3h\0338",
                                   std::string(80, '.'));
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
    {
        INFO((screenSelect.empty() ? "primary" : "alternate"));
        auto const fullSetup = std::string(screenSelect) + setup;

        auto probe = makeTerminal(PageSize { LineCount(3), ColumnCount(80) }, vtparser::BulkScan::Disabled);
        probe.writeToScreen(fullSetup);
        auto const& cursor = probe.terminal.currentScreen().cursor();
        REQUIRE(cursor.wrapPending);
        REQUIRE(cursor.position.column < probe.terminal.currentScreen().margin().horizontal.to);

        checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(80) }, fullSetup, "xyz"sv);
    }
}

TEST_CASE("BulkScan.ModesTheOldGateShieldedGiveTheSameScreen", "[BulkScan]")
{
    // Review Focus 2 against a true per-codepoint reference. Task 2.4 opened the bulk path to the
    // alternate screen and compared the two screens; here each mode is written with bulk scanning
    // and without it, on both screens, cut into two reads at every byte.
    auto const setup = GENERATE(values<std::string_view>({
        "\033[4h"sv,                                      // insert mode
        "\033[?7l"sv,                                     // autowrap off
        "\033[?69h\033[3;6s\033[1;3H"sv,                  // left/right margins
        "\033[2;3r\033[?69h\033[3;6s\033[?6h\033[1;1H"sv, // origin mode inside margins
        "\033(0"sv,                                       // DEC special graphics
        "\033[?1049h\033[4h"sv,                           // the alternate screen, insert mode
        "\033[?1049h\033[?7l"sv,                          // the alternate screen, autowrap off
        "\033[?1049h\033[?69h\033[3;6s\033[1;3H"sv,       // the alternate screen, margins
    }));
    INFO(std::format("setup \"{}\"", core::escape(setup)));
    checkSameScreenAtEverySplit(
        PageSize { LineCount(3), ColumnCount(10) }, setup, "XYZ\rab cdefghijkl \033[31mred\033[m mn"sv);
}

TEST_CASE("BulkScan.MixedTextGivesTheSameScreen", "[BulkScan]")
{
    auto const text = GENERATE(values<std::string_view>({
        "The quick brown fox w\xC3\xB6rld jumps over the lazy dog \xC3\xA4\xC3\xB6\xC3\xBC end of line"sv,
        // combining marks
        "plain words then e\xCC\x81 and more plain words a\xCC\x81\xCC\x82 tail"sv,
        // a ZWJ sequence
        "family emoji \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 after it"sv,
        // regional-indicator pairs
        "flags \xF0\x9F\x87\xA9\xF0\x9F\x87\xAA\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 after the flags"sv,
        // VS16
        "a red heart \xE2\x9D\xA4\xEF\xB8\x8F and keycap one 1\xEF\xB8\x8F\xE2\x83\xA3 done"sv,
        // two columns each
        "wide text \xE4\xB8\xAD\xE6\x96\x87 between ascii words here"sv,
        // box drawing
        "\xE2\x94\x82 column one      \xE2\x94\x82 column two      \xE2\x94\x82"sv,
        // starts and ends non-ASCII
        "\xC3\xA4\xC3\xB6\xC3\xBC some ascii in the middle \xC3\x9F"sv,
        // a mark at the margin
        "abcdefghijkl\xCC\x81 then more text after the mark"sv,
        // a ZWJ and a VS16 on ASCII bases, in text sparse enough for its stretches to be walked
        "plain words then a joiner\xE2\x80\x8D and a keycap 1\xEF\xB8\x8F in more plain words"sv,
    }));
    auto const setup = GENERATE(values<std::string_view>({
        // primary screen, from the left edge
        ""sv,
        // from mid-line, so runs end at the margin with a wrap pending
        "\033[1;8H"sv,
        // autowrap off: the last column is overwritten instead
        "\033[?7l\033[1;8H"sv,
        // the alternate screen
        "\033[?1049h\033[1;8H"sv,
        // a Mixed line: a coloured cell ahead of the text
        "\033[1;6H\033[41mX\033[m\033[1;1H"sv,
        // a Mixed line on the alternate screen
        "\033[?1049h\033[1;6H\033[41mX\033[m\033[1;1H"sv,
    }));
    auto const columns = GENERATE(12, 80);
    INFO(
        std::format("columns {} setup \"{}\" text \"{}\"", columns, core::escape(setup), core::escape(text)));
    checkSameScreenAtEverySplit(PageSize { LineCount(4), ColumnCount(columns) }, setup, text);
}

TEST_CASE("BulkScan.MixedTextOnShapedLinesGivesTheSameScreen", "[BulkScan]")
{
    // Mixed text written over every edit that leaves a line Mixed or Spanning, on both screens: a long
    // ASCII stretch, a combining mark on its last letter, another stretch and a ZWJ after it. The page
    // is wide enough for each stretch's interior to be written in bulk wherever the line allows it.
    auto const text = "\033[1;1H\xC3\xA4"
                      "bcdefghijklm\xCC\x81 nopqrstuvwx\xE2\x80\x8Dyz"sv;
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
        for (auto const& edit: test::EditsOfShapedLines)
        {
            INFO(std::format("{}, {}", screenSelect.empty() ? "primary" : "alternate", edit.name));
            checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(40) },
                                        std::string(screenSelect) + std::string(edit.bytes),
                                        text);
        }
}

TEST_CASE("BulkScan.AStretchPastANarrowedEmojiGivesTheSameScreen", "[BulkScan]")
{
    // The screen can take more columns than the parser's scan counted for a run. The scan measures a
    // wide emoji followed by VS15 as one column; the screen keeps the two columns the emoji took
    // before VS15 arrived, because it never narrows a cluster already on screen. Two of them before 77
    // letters: the scan counts 79 columns, which the 80-column page allows, but the screen needs 81.
    // Only asking again before the stretch is written in bulk keeps the letters past the margin from
    // being lost. Also with a letter first, so the emoji sit inside the run.
    struct Writing
    {
        std::string text; ///< Cut into two reads at every byte.
        size_t cellCount; ///< The columns the parser's scan counts for it in one read.
    };
    auto const watchVs15 = "\xE2\x8C\x9A\xEF\xB8\x8E"sv; // U+231A WATCH, U+FE0E VS15
    auto const writings = std::array {
        Writing { .text = std::format("{0}{0}{1}", watchVs15, std::string(77, 'a')), .cellCount = 79 },
        Writing { .text = std::format("x{0}{0}{1}", watchVs15, std::string(76, 'b')), .cellCount = 79 },
    };
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
        for (auto const& writing: writings)
        {
            INFO(std::format("{}, text \"{}\"",
                             screenSelect.empty() ? "primary" : "alternate",
                             core::escape(writing.text)));
            // Dense text skips the search for stretches and never reaches the check this is about.
            REQUIRE_FALSE(isTooDenseForBulkAscii(writing.text.size(), writing.cellCount));
            checkSameScreenAtEverySplit(
                PageSize { LineCount(4), ColumnCount(80) }, screenSelect, writing.text);
        }
}

TEST_CASE("BulkScan.AJoinerAfterAWrapThatScrollsGivesTheSameScreen", "[BulkScan]")
{
    // The scan counts a wide emoji narrowed by VS15 as one column, the screen keeps the two it took, so
    // the last letter lands on column 9 with a wrap pending that the scan did not expect. The combining
    // mark after it is in the same run (the letters behind it keep the scan from holding it back as the
    // end of the read), and the cursor is on the bottom row: the wrap scrolls, and the mark must be seen
    // against the blank line that scrolled in, as the state machine sees it, not joined to a cell there.
    auto const text = "\xE2\x8C\x9A\xEF\xB8\x8E"
                      "\xE2\x8C\x9A\xEF\xB8\x8E"
                      "abcdef"
                      "\xCC\x81"
                      "ij"sv;
    checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(10) }, "\033[3;1H"sv, text);
}

TEST_CASE("BulkScan.ASoftFontGlyphInsideAZwjSequenceGivesTheSameScreen", "[BulkScan]")
{
    // A soft font glyph is a space cell with an image, so what follows it starts a cluster of its own.
    // The run's segmentation state must say so: left at the ZWJ before the glyph, it would still be
    // waiting for the emoji that completes the sequence, and join the next one to the glyph's cell.
    auto const setup = "\033P1;0;0;10;10;0;20;0{ A@\033\\" // soft font 1 with a glyph for '!'
                       "\033) A\x0E"sv;                    // designated into G1, shifted out
    auto const text = "\xF0\x9F\x91\xA8\xE2\x80\x8D"       // MAN, ZWJ
                      "!"                                  // the glyph
                      "\xF0\x9F\x91\xA9"sv;                // WOMAN
    checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(10) }, setup, text);
}

TEST_CASE("BulkScan.InvalidUtf8BesideALongAsciiStretchGivesTheSameScreen", "[BulkScan]")
{
    // The text decoder takes the bytes after an incomplete lead byte as its continuation bytes,
    // whatever they are, ASCII included. So an ASCII stretch after one can lose its first bytes to a
    // codepoint that is not ASCII -- "\xD8" "A" decodes to U+0601 ARABIC SIGN SANAH, a Prepend
    // character the next letter joins -- and a stretch is only ASCII text where the decoder is idle.
    auto const text = GENERATE(values<std::string_view>({
        "\xE2"
        "abcdefghijklmnop \xC3\xA4 tail"sv, // a three-byte lead, two ASCII bytes short
        "\xF0"
        "abcdefghijklmnop \xC3\xA4 tail"sv, // a four-byte lead, three ASCII bytes short
        "ascii first \xE2\x94"
        "abcdefghijklmnop end"sv, // a truncated U+2502 between two stretches
    }));
    auto const setup = GENERATE(values<std::string_view>({ ""sv, "\033[?1049h"sv }));
    INFO(std::format("setup \"{}\" text \"{}\"", core::escape(setup), core::escape(text)));

    // Cut into two reads at every byte, so also where a read ends inside the incomplete character:
    // libunicode's text scanner stops in front of ill-formed UTF-8 (0.9.4), where 0.9.3 handed such a
    // read on as a run that ended inside the character, which the screen's run-local decoder dropped.
    checkSameScreenAtEverySplit(PageSize { LineCount(4), ColumnCount(80) }, setup, text);
}

TEST_CASE("BulkScan.InvalidUtf8DecodedToAPrependCharacterGivesTheSameScreen", "[BulkScan]")
{
    // "\xD8" "A" decodes to U+0601 ARABIC SIGN SANAH, a Prepend character the '1' after it joins. In one
    // read the stretch after it is part of mixed text, whose first byte goes through the segmenter. Cut
    // anywhere after the '1', the stretch is a run of nothing but ASCII, which is written in bulk whole
    // and does not join a Prepend character before it: the accepted divergence of the test below, so
    // this one does not cut the read.
    auto const text = "\xD8"
                      "A1234567890 after a decoded prepend \xC3\xA4"sv;
    auto const setup = GENERATE(values<std::string_view>({ ""sv, "\033[?1049h"sv }));
    INFO(std::format("setup \"{}\"", core::escape(setup)));
    auto const pageSize = PageSize { LineCount(4), ColumnCount(80) };
    auto bulk = makeTerminal(pageSize, vtparser::BulkScan::Enabled);
    auto reference = makeTerminal(pageSize, vtparser::BulkScan::Disabled);
    for (auto* const mock: { &bulk, &reference })
    {
        mock->writeToScreen(setup);
        mock->writeToScreen(text);
    }
    CHECK(dumpScreen(bulk.terminal.currentScreen()) == dumpScreen(reference.terminal.currentScreen()));
}

TEST_CASE("BulkScan.AsciiAfterAPrependCharacterJoinsIt", "[BulkScan]")
{
    // GB9b: nothing breaks after a Prepend character, so the '1' after U+0600 ARABIC NUMBER SIGN
    // belongs to its cluster. This covers mixed text: the text ends in a letter that is not ASCII, so
    // wherever the reads are cut, the read holding the '1' goes through the walk over mixed text, which
    // writes the first byte of every ASCII stretch through the segmenter, and only the rest in bulk.
    //
    // Accepted divergence: a run of nothing but ASCII is written in bulk whole, so it does NOT join a
    // Prepend character before it, where the per-codepoint path does. Two reads, "\xD8\x80" and then
    // "1234567", give 0600 | 1 | 2 ... with bulk scanning and 0600+1 | 2 ... without; so does one read
    // whose run before U+0600 fills the line, as the state machine then writes U+0600 and the letters
    // after it are a run of their own (35 columns, mode 2027 reset, 35 letters, U+0600, "mhfrjdp").
    // Asking the cell the last write ended in before each such run costs more than it is worth: it is
    // a read of another line per run, measured at +1.15% instructions on the ascii workload, +1.38% on
    // tui and +2.04% on sgr, and worse when kept out of line. Prepend characters are rare (Arabic
    // number signs, a few Indic letters), and the random differential below excludes the case by name.
    auto const text = "\xD8\x80"
                      "1234567890 ascii after a prepend \xC3\xA4"sv;
    for (auto const screenSelect: { ""sv, "\033[?1049h"sv })
    {
        INFO((screenSelect.empty() ? "primary" : "alternate"));
        checkSameScreenAtEverySplit(PageSize { LineCount(4), ColumnCount(80) }, screenSelect, text);
    }

    // And what both must show: U+0600 and the '1' in one cell, also with the reads cut after U+0600.
    auto mock = makeTerminal(PageSize { LineCount(4), ColumnCount(80) }, vtparser::BulkScan::Enabled);
    mock.writeToScreen(text.substr(0, 2));
    mock.writeToScreen(text.substr(2));
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).codepoints()
          == U"\U00000600\U00000031");
}

TEST_CASE("BulkScan.AClusterGrowingFromZeroColumnsKeepsItsHead", "[BulkScan]")
{
    // A combining mark on a blank line opens a cluster of zero columns, and the cursor stays on it. A
    // second mark widens the cluster to the one column every cluster takes; a skin tone, which joins
    // as an extension and is two columns wide, widens it to two. The head must stay a head that holds
    // the whole cluster, with the cursor after it, and the columns it grew into must continue it --
    // with bulk scanning and without.
    struct Growth
    {
        std::string_view text;       ///< Written to a blank line.
        std::u32string_view cluster; ///< What the head at column 0 must hold.
        unsigned width;              ///< How many columns the head must take.
    };
    auto const growths = std::array {
        Growth { .text = "\xCC\x81\xCC\x81"sv, .cluster = U"\U00000301\U00000301", .width = 1u },
        Growth { .text = "\xCC\x81\xF0\x9F\x8F\xBD"sv, .cluster = U"\U00000301\U0001F3FD", .width = 2u },
    };
    for (auto const& growth: growths)
        for (auto const bulkScan: { vtparser::BulkScan::Enabled, vtparser::BulkScan::Disabled })
        {
            INFO(std::format("text \"{}\", bulk scanning {}",
                             core::escape(growth.text),
                             bulkScan == vtparser::BulkScan::Enabled ? "on" : "off"));
            auto mock = makeTerminal(PageSize { LineCount(2), ColumnCount(10) }, bulkScan);
            mock.writeToScreen(growth.text);
            auto const& screen = mock.terminal.primaryScreen();
            auto const head = screen.at(LineOffset(0), ColumnOffset(0));
            CHECK(!head.flags().contains(CellFlag::WideCharContinuation));
            CHECK(head.codepoints() == growth.cluster);
            CHECK(head.width() == growth.width);
            for (auto const column: std::views::iota(1u, growth.width))
                CHECK(screen.at(LineOffset(0), ColumnOffset::cast_from(column))
                          .flags()
                          .contains(CellFlag::WideCharContinuation));
            CHECK(screen.cursor().position
                  == CellLocation { .line = LineOffset(0), .column = ColumnOffset::cast_from(growth.width) });
        }
}

namespace
{
/// One piece of text that is not ASCII, from each class the bulk paths treat differently: one, two and
/// three columns' worth of bytes, every way a codepoint joins the cluster before it (marks, joiners,
/// variation selectors, modifiers, regional indicators, Prepend, Hangul jamo, Indic conjuncts), and the
/// zero-width characters that join nothing.
constexpr auto NonAsciiPieces = std::array {
    "\xC3\xA4"sv,                             // U+00E4, two bytes
    "\xE2\x94\x82"sv,                         // U+2502, three bytes
    "\xE4\xB8\xAD"sv,                         // U+4E2D, two columns
    "\xF0\x9F\x98\x80"sv,                     // U+1F600, four bytes, two columns
    "\xCC\x81"sv,                             // U+0301 COMBINING ACUTE ACCENT
    "\xE2\x80\x8D"sv,                         // U+200D ZERO WIDTH JOINER
    "\xEF\xB8\x8F"sv,                         // U+FE0F VS16
    "\xEF\xB8\x8E"sv,                         // U+FE0E VS15
    "\xE2\x8C\x9A"sv,                         // U+231A WATCH, a wide emoji
    "\xE2\x8C\x9A\xEF\xB8\x8E"sv,             // U+231A narrowed by VS15
    "\xE2\x9D\xA4"sv,                         // U+2764 HEAVY BLACK HEART, narrow until VS16
    "\xF0\x9F\x87\xA9"sv,                     // U+1F1E9, regional indicator D
    "\xF0\x9F\x87\xAA"sv,                     // U+1F1EA, regional indicator E
    "\xD8\x80"sv,                             // U+0600 ARABIC NUMBER SIGN, Prepend
    "\xE2\x80\x8B"sv,                         // U+200B ZERO WIDTH SPACE
    "\xC2\xAD"sv,                             // U+00AD SOFT HYPHEN
    "\xE1\x84\x80"sv,                         // U+1100, a Hangul leading jamo
    "\xE1\x85\xA1"sv,                         // U+1161, a Hangul vowel jamo
    "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xA8"sv, // KA, VIRAMA, NA: a Devanagari conjunct
    "\xE0\xA4\x95\xE0\xA4\xBE"sv,             // KA with a spacing vowel sign
    "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"sv,     // thumbs up with a skin tone
    "\xF0\x9F\x8F\xBD"sv,                     // a skin tone on its own
};

/// One piece of ill-formed UTF-8, from each way the decoders can disagree about where it ends: stray
/// continuation bytes, lead bytes cut short by ASCII or by another lead, the bytes no encoding uses, and
/// overlong, surrogate and out-of-range encodings. A lead byte cut short takes the ASCII after it for its
/// continuation bytes, so a stretch of ASCII can lose its first bytes to a character that is not ASCII.
constexpr auto IllFormedPieces = std::array {
    "\x80"sv,     // a stray continuation byte
    "\xBF"sv,     // the last one
    "\xC3"sv,     // a two-byte lead, one byte short
    "\xE2\x94"sv, // a three-byte lead, one byte short
    "\xF0\x9F"sv, // a four-byte lead, two bytes short
    "\xE2"
    "a"sv, // a three-byte lead cut short by ASCII
    "\xF0\x9F"
    "xyz"sv,                  // a four-byte lead cut short by ASCII
    "\xC3\xE2\x94\x82"sv,     // a lead cut short by another lead
    "\xE2\xF0\x9F\x98\x80"sv, // the same with a four-byte character behind it
    "\xC0\xAF"sv,             // 0xC0 and 0xC1 are overlong leads
    "\xC1\xBF"sv,
    "\xF5\x80\x80\x80"sv, // 0xF5..0xFF lead nothing
    "\xF8"sv,
    "\xFE"sv,
    "\xFF"sv,
    "\xE0\x80\xAF"sv,     // overlong: '/' in three bytes
    "\xF0\x80\x80\xAF"sv, // overlong: '/' in four bytes
    "\xED\xA0\x80"sv,     // a surrogate, U+D800
    "\xED\xBF\xBF"sv,     // a surrogate, U+DFFF
    "\xF4\x90\x80\x80"sv, // above U+10FFFF
};

/// What the page holds and which modes are set before the random text arrives.
constexpr auto RandomTextSetups = std::array {
    ""sv,                                  // the primary screen, from the left edge
    "\033[1;5H"sv,                         // from mid-line
    "\033[?7l"sv,                          // autowrap off
    "\033[?7l\033[1;9H"sv,                 // autowrap off, from mid-line
    "\033[?1049h"sv,                       // the alternate screen
    "\033[?1049h\033[1;7H"sv,              // the alternate screen, from mid-line
    "\033[1;4H\033[41mX\033[m\033[1;1H"sv, // a Mixed line: a coloured cell ahead of the text
    "\033[?2027l"sv,                       // a cluster keeps its first codepoint's width
    "\033[?2027l\033[1;6H"sv,              // the same, from mid-line
    "\033[?1049h\033[?7l\033[1;3H"sv,      // the alternate screen, autowrap off
    "\033[31m"sv,                          // a coloured pen
    "Q\xCC\x81"sv,                         // the line already ends in a cluster
    "\033[4;1H"sv,                         // the bottom row: a wrap inside the run scrolls
    "\033[4;6H"sv,                         // the bottom row, from mid-line
};

/// A choice in [0, @p n), drawn from the engine directly so that a seed reproduces on every platform.
size_t below(std::mt19937& rng, size_t n)
{
    return static_cast<size_t>(test::detail::drawBetween(rng, 0, static_cast<int>(n) - 1));
}

/// Random text: up to ten pieces, half of them ASCII words of up to 30 letters (some followed by a
/// space), nine in twenty NonAsciiPieces, and one in twenty IllFormedPieces.
std::string randomMixedText(std::mt19937& rng)
{
    auto out = std::string {};
    for ([[maybe_unused]] auto const piece: std::views::iota(size_t { 0 }, 1 + below(rng, 10)))
    {
        if (auto const kind = below(rng, 20); kind < 10)
        {
            auto const letters = 1 + below(rng, kind < 4 ? 4 : 30);
            for ([[maybe_unused]] auto const letter: std::views::iota(size_t { 0 }, letters))
                out += static_cast<char>('a' + below(rng, 26));
            if (below(rng, 4) == 0)
                out += ' ';
        }
        else if (kind < 19)
            out += NonAsciiPieces[below(rng, NonAsciiPieces.size())];
        else
            out += IllFormedPieces[below(rng, IllFormedPieces.size())];
    }
    return out;
}

/// One random case: a page width, a setup, the text and where PTY reads cut it.
struct RandomTextCase
{
    ColumnCount columns;      ///< The page width; the page is four lines high.
    std::string_view setup;   ///< Written in one read, before the text.
    std::string text;         ///< Random text, now and then ill-formed UTF-8.
    std::vector<size_t> cuts; ///< Sorted offsets into text; text is written in cuts.size() + 1 reads.
};

/// @return The case @p seed stands for.
RandomTextCase randomTextCase(uint32_t seed)
{
    auto rng = std::mt19937 { seed };
    auto const columns = ColumnCount::cast_from(8 + below(rng, 60));
    auto const setup = RandomTextSetups[below(rng, RandomTextSetups.size())];
    auto text = randomMixedText(rng);
    auto cuts = std::vector<size_t> {};
    for ([[maybe_unused]] auto const cut: std::views::iota(size_t { 0 }, below(rng, 4)))
        cuts.push_back(below(rng, text.size() + 1));
    std::ranges::sort(cuts);
    return { .columns = columns, .setup = setup, .text = std::move(text), .cuts = std::move(cuts) };
}

/// An accepted divergence, older than the bulk path for mixed text: a run of nothing but ASCII never
/// joins the Prepend character written before it (GB9b), where the per-codepoint path does. Why it is
/// accepted is said at BulkScan.AsciiAfterAPrependCharacterJoinsIt.
/// @return Whether @p text holds an ASCII byte right after U+0600, the one Prepend character in
///         NonAsciiPieces.
bool asciiFollowsAPrependCharacter(std::string_view text)
{
    auto const prepend = "\xD8\x80"sv;
    return std::ranges::any_of(
        std::views::iota(prepend.size(), std::max(text.size(), prepend.size())), [&](size_t next) {
            return text.substr(0, next).ends_with(prepend) && static_cast<unsigned char>(text[next]) < 0x80;
        });
}

/// The state the renderer and the selection cannot resolve: a continuation cell with no head. A
/// cluster that grew from zero columns used to leave one, by overwriting its own head.
/// @return Whether a cell of @p screen's page continues a character that does not reach it.
bool holdsAnOrphanContinuation(Screen const& screen)
{
    for (auto const line: std::views::iota(0, screen.pageSize().lines.value))
    {
        auto head = std::optional<int> {}; // the column of the last cell that is no continuation
        for (auto const column: std::views::iota(0, screen.pageSize().columns.value))
        {
            auto const cell = screen.at(LineOffset(line), ColumnOffset(column));
            if (!cell.flags().contains(CellFlag::WideCharContinuation))
                head = column;
            else if (!head || screen.at(LineOffset(line), ColumnOffset(*head)).width() <= column - *head)
                return true;
        }
    }
    return false;
}

/// Writes each seed's case to a terminal with bulk scanning and to one without, and requires the two
/// screens to agree, every line's shape to cover its cells, and no continuation to be an orphan.
///
/// The screens are not compared for a case in the accepted divergence class
/// (asciiFollowsAPrependCharacter); its other checks still run.
void checkRandomMixedText(uint32_t firstSeed, uint32_t seedCount)
{
    for (auto const seed: std::views::iota(firstSeed, firstSeed + seedCount))
    {
        auto const testCase = randomTextCase(seed);
        auto const pageSize = PageSize { LineCount(4), testCase.columns };
        auto bulk = makeTerminal(pageSize, vtparser::BulkScan::Enabled);
        auto reference = makeTerminal(pageSize, vtparser::BulkScan::Disabled);
        for (auto* const mock: { &bulk, &reference })
        {
            mock->writeToScreen(testCase.setup);
            auto begin = size_t { 0 };
            for (auto const cut: testCase.cuts)
            {
                mock->writeToScreen(std::string_view(testCase.text).substr(begin, cut - begin));
                begin = cut;
            }
            mock->writeToScreen(std::string_view(testCase.text).substr(begin));
        }
        auto cuts = std::string {};
        for (auto const cut: testCase.cuts)
            cuts += std::format(" {}", cut);
        INFO(std::format("seed {}: {} columns, setup \"{}\", text \"{}\", cut at [{} ]",
                         seed,
                         testCase.columns,
                         core::escape(testCase.setup),
                         core::escape(testCase.text),
                         cuts));
        if (!asciiFollowsAPrependCharacter(testCase.text))
            CHECK(dumpScreen(bulk.terminal.currentScreen())
                  == dumpScreen(reference.terminal.currentScreen()));
        for (auto const* const mock: { &bulk, &reference })
        {
            test::checkShapeCoversCells(mock->terminal.currentScreen());
            CHECK_FALSE(holdsAnOrphanContinuation(mock->terminal.currentScreen()));
        }
    }
}
} // namespace

TEST_CASE("BulkScan.RandomMixedTextGivesTheSameScreen", "[BulkScan]")
{
    checkRandomMixedText(1, 300);
}

TEST_CASE("BulkScan.RandomMixedTextGivesTheSameScreen.ManySeeds", "[.][BulkScanManySeeds]")
{
    // Opt-in, by its tag: the same check over many more seeds, for a change to the text paths.
    checkRandomMixedText(1, 10000);
}
