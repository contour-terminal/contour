// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/state/StateCodec.hpp>
#include <vtbackend/testing/StateHandover.hpp>

#include <format>
#include <ranges>
#include <string>

namespace vtbackend::test
{

/// The terminals of the golden session: small, with scrollback, and the cell size the images are placed by.
inline HandoverSetup goldenSetup()
{
    return HandoverSetup { .pageSize = PageSize { LineCount(6), ColumnCount(40) },
                           .history = LineCount(30),
                           .cellPixelSize = ImageSize { Width(10), Height(20) },
                           .inFlightTailLimit = DefaultInFlightTailLimit,
                           .checkpointStride = 64 };
}

/// The bytes the golden session's terminal parses: a title, coloured text enough to scroll twelve lines
/// into the history, a Sixel image, a kitty image, a hyperlink, a DECDLD soft font with cells drawn in it,
/// and a sequence left half-way.
/// @return The same bytes on every call.
inline std::string goldenSession()
{
    auto session = std::string { "\033]2;golden session\033\\" };
    for (auto const line: std::views::iota(1, 13))
        session += std::format("\033[3{}mline {:02} of the golden session\033[0m\r\n", (line % 7) + 1, line);
    session += "\033Pq\"1;1;12;6#1;2;100;0;0#1~~~~~~~~~~~~\033\\\r\n";
    session += "\033_Ga=T,f=24,s=2,v=1,i=7;AAAA/wAA\033\\\r\n";
    session += "\033]8;;https://example.com/golden\033\\a link\033]8;;\033\\ and text\r\n";
    // Format v2: a soft font, and two cells of the page drawn with it.
    session += "\033P1;1;1;10;0;0;20;0{ A~~~~~/~~~~~;@@@@@/@@@@@\033\\\033) A\x0E!\"\x0F soft\r\n";
    session += "last line, then a CSI left open\033[3";
    return session;
}

/// The bytes of the kitty golden: Claude Code showing a picture through Unicode placeholders. A virtual
/// placement with its resolved placeholder rows, an image stored under a number, and a placeholder row whose
/// image has not arrived -- what the virtual-placement and image-number tables and the placeholder origin
/// are frozen by. Kept apart from goldenSession(), whose text the older v2 stream is still compared against.
/// @return The same bytes on every call.
inline std::string goldenKittySession()
{
    auto const row = [](std::string_view sgr, std::string_view rowMark) {
        auto text = std::format("\033[{}m", sgr);
        for (auto const column: { "\xCC\x85", "\xCC\x8D", "\xCC\x8E", "\xCC\x90" })
            text += std::format("\xF4\x8E\xBB\xAE{}{}", rowMark, column);
        return text + "\033[m\r\n";
    };
    auto session = std::string { "$ claude\r\n" };
    session += "\033_Ga=T,U=1,i=300,c=4,r=2,f=24,s=2,v=2,q=2;/wAAAP8AAAD/////\033\\";
    session += "\033_Ga=t,I=5,f=24,s=1,v=1,q=2;AAAA\033\\";
    session += row("38;2;0;1;44", "\xCC\x85");
    session += row("38;2;0;1;44", "\xCC\x8D");
    session += row("38;5;9", "\xCC\x85");
    session += "done";
    return session;
}

/// @param mock A terminal; probed, so changed.
/// @return Its scrollback, a separator line, its main page, another separator and the transcript of its probe
///         replies (probeTranscript()): what the golden test compares, text and behaviour alike.
inline std::string describeTerminal(MockTerm<>& mock)
{
    auto const& terminal = mock.terminal;
    auto const& screen = terminal.primaryScreen();
    auto text = std::string {};
    auto const history = unbox<int>(screen.grid().historyLineCount());
    for (auto const offset: std::views::iota(-history, 0))
        text += std::format("{}\n", screen.grid().lineText(LineOffset(offset)));
    text += "----\n";
    text += screen.renderMainPageText();
    text += "---- probes\n";
    text += probeTranscript(mock);
    return text;
}

} // namespace vtbackend::test
