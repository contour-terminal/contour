// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/ScreenTestFixtures.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtparser/BulkScan.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

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
    // U+FFFD the state machine prints for it. Only mid-run: libunicode's text scanner (0.9.3) consumes
    // an invalid byte that ends a run without returning it.
    auto const setup = GENERATE(values<std::string_view>({ ""sv, "\033[?1049h"sv }));
    INFO(std::format("setup \"{}\"", core::escape(setup)));
    checkSameScreenAtEverySplit(PageSize { LineCount(3), ColumnCount(10) }, setup, "\xFF\xE2\x94\x82X"sv);
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
        "\xD8"
        "A1234567890 after a decoded prepend \xC3\xA4"sv, // U+0601, then '1' joins its cluster
        "ascii first \xE2\x94"
        "abcdefghijklmnop end"sv, // a truncated U+2502 between two stretches
    }));
    auto const setup = GENERATE(values<std::string_view>({ ""sv, "\033[?1049h"sv }));
    INFO(std::format("setup \"{}\" text \"{}\"", core::escape(setup), core::escape(text)));

    // In one read: libunicode's text scanner (0.9.3) hands a read that ends inside such a character on
    // as a complete run, so cuts between reads wait for its fix (scan_text stopping at ill-formed UTF-8).
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
    // belongs to its cluster. The stretch after it is long enough to be written in bulk, and its
    // first byte must still go through the segmenter. Written in one read, and in two cut after U+0600.
    auto const text = "\xD8\x80"
                      "1234567890 ascii after a prepend \xC3\xA4"sv;
    auto const pageSize = PageSize { LineCount(4), ColumnCount(80) };
    auto bulk = makeTerminal(pageSize, vtparser::BulkScan::Enabled);
    auto reference = makeTerminal(pageSize, vtparser::BulkScan::Disabled);
    for (auto* const mock: { &bulk, &reference })
        mock->writeToScreen(text);
    CHECK(dumpScreen(bulk.terminal.currentScreen()) == dumpScreen(reference.terminal.currentScreen()));
    CHECK(bulk.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).codepointCount() == 2);

    for (auto* const mock: { &bulk, &reference })
    {
        mock->writeToScreen("\033c"sv);
        mock->writeToScreen(text.substr(0, 2));
        mock->writeToScreen(text.substr(2));
    }
    CHECK(dumpScreen(bulk.terminal.currentScreen()) == dumpScreen(reference.terminal.currentScreen()));
}
