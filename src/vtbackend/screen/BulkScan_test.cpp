// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/MockTerm.hpp>

#include <vtparser/BulkScan.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
