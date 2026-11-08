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
