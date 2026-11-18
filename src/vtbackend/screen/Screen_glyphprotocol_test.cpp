// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/render/RenderBuffer.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/testing/GlyfRecordBuilder.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::glyf_test;

namespace
{

constexpr auto TestPage = PageSize { LineCount(2), ColumnCount(10) };

/// @return The message, as an application writes it, that registers the square at @p codepoint.
[[nodiscard]] std::string registerSquare(std::string_view codepoint)
{
    return std::format("\033_25a1;r;cp={};{}\033\\", codepoint, base64(squareRecord()));
}

} // namespace

TEST_CASE("GlyphProtocol.Screen: support is answered before DA1", "[screen][glyphprotocol]")
{
    // How an application detects the protocol: `s`, then DA1; an `s` reply ahead of the DA1 one means yes.
    auto mock = MockTerm { TestPage };
    mock.writeToScreen("\033_25a1;s\033\\\033[c");
    CHECK(mock.terminal.peekInput().starts_with("\033_25a1;s;fmt=glyf\033\\\033[?"));
}

TEST_CASE("GlyphProtocol.Screen: another APC is not taken for it", "[screen][glyphprotocol]")
{
    auto mock = MockTerm { TestPage };
    mock.writeToScreen("\033_25a1\033\\\033_25a2;s\033\\\033_hello\033\\");
    CHECK(mock.terminal.peekInput().empty());
}

TEST_CASE("GlyphProtocol.Screen: a registered glyph reaches the frame", "[screen][glyphprotocol]")
{
    auto mock = MockTerm { TestPage };
    mock.writeToScreen(registerSquare("100000"));
    CHECK(mock.terminal.peekInput() == "\033_25a1;r;cp=100000;status=0\033\\");
    // The cursor moves on to the next line: a line it sits on is rendered cell by cell whatever it holds.
    mock.writeToScreen("i:\U00100000\r\n");

    (void) mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();
    auto const& frame = buffer.get();
    REQUIRE(frame.registeredGlyphs != nullptr);
    CHECK(
        std::ranges::none_of(frame.lines, [](auto const& line) { return line.lineOffset == LineOffset(0); }));

    // The line is rendered cell by cell, so the glyph's cell is there for the renderer to draw into ...
    auto const cell = std::ranges::find(frame.cells,
                                        CellLocation { .line = LineOffset(0), .column = ColumnOffset(2) },
                                        &RenderCell::position);
    REQUIRE(cell != frame.cells.end());
    // ... and it still holds the codepoint the application wrote: the cell buffer is what is copied.
    CHECK(cell->codepoints == U"\U00100000");
    CHECK(cell->width == 1);
    CHECK(registeredGlyphOf(frame.registeredGlyphs.get(), cell->codepoints) != nullptr);
    CHECK(mock.terminal.primaryScreen().grid().lineText(LineOffset(0)).starts_with("i:\U00100000"));
}

TEST_CASE("GlyphProtocol.Screen: a session that registers nothing pays nothing", "[screen][glyphprotocol]")
{
    auto mock = MockTerm { TestPage };
    mock.writeToScreen("i:\U00100000\r\n");
    (void) mock.terminal.refreshRenderBuffer();
    auto const buffer = mock.terminal.renderBuffer();
    CHECK(buffer.get().registeredGlyphs == nullptr);
    // The plain line stays on the batched path.
    CHECK(std::ranges::any_of(buffer.get().lines,
                              [](auto const& line) { return line.lineOffset == LineOffset(0); }));
}

TEST_CASE("GlyphProtocol.Screen: the next frame follows the glossary", "[screen][glyphprotocol]")
{
    auto mock = MockTerm { TestPage };
    mock.writeToScreen("\U00100000");
    (void) mock.terminal.refreshRenderBuffer();
    REQUIRE(mock.terminal.renderBuffer().get().registeredGlyphs == nullptr);

    // The cell is already on screen; the frame after its glyph is registered carries the glyph.
    mock.writeToScreen(registerSquare("100000"));
    (void) mock.terminal.refreshRenderBuffer();
    CHECK(mock.terminal.renderBuffer().get().registeredGlyphs != nullptr);

    mock.writeToScreen("\033_25a1;c\033\\");
    (void) mock.terminal.refreshRenderBuffer();
    CHECK(mock.terminal.renderBuffer().get().registeredGlyphs == nullptr);
}

TEST_CASE("GlyphProtocol.Screen: RIS clears the glossary", "[screen][glyphprotocol]")
{
    auto mock = MockTerm { TestPage };
    mock.writeToScreen(registerSquare("100000"));
    REQUIRE_FALSE(mock.terminal.glyphGlossary().empty());
    mock.writeToScreen("\033c");
    CHECK(mock.terminal.glyphGlossary().empty());
}

TEST_CASE("GlyphProtocol.Screen: a payload of the full 64 KiB gets through", "[screen][glyphprotocol]")
{
    // 87,384 bytes of base64, more than an OSC may carry. A record of zeroes is a glyph without contours,
    // which is a valid (blank) one, so the reply tells the message arrived whole.
    auto mock = MockTerm { TestPage };
    auto const blank = std::vector<uint8_t>(MaxGlyphPayloadBytes, uint8_t { 0 });
    mock.writeToScreen(std::format("\033_25a1;r;cp=100000;{}\033\\", base64(blank)));
    CHECK(mock.terminal.peekInput() == "\033_25a1;r;cp=100000;status=0\033\\");
}

TEST_CASE("GlyphProtocol.Screen: a message past its cap is refused, not ignored", "[screen][glyphprotocol]")
{
    // The sender waits for an answer, and has no chunking to fall back on.
    auto mock = MockTerm { TestPage };
    auto const huge = std::string(MaxGlyphMessageLength + 1024, 'A');

    mock.writeToScreen(std::format("\033_25a1;r;cp=100000;{}\033\\", huge));
    CHECK(mock.terminal.peekInput() == "\033_25a1;r;cp=100000;status=1;reason=payload_too_large\033\\");
    CHECK(mock.terminal.glyphGlossary().empty());

    // Byte by byte, as a slow sender delivers it, the cap and the answer are the same.
    auto slow = MockTerm { TestPage };
    for (auto const ch: std::format("\033_25a1;r;cp=100001;{}\033\\", huge))
        slow.writeToScreen(std::string_view { &ch, 1 });
    CHECK(slow.terminal.peekInput() == "\033_25a1;r;cp=100001;status=1;reason=payload_too_large\033\\");
}

TEST_CASE("GlyphProtocol.Screen: the larger cap is the Glyph Protocol's alone", "[screen][glyphprotocol]")
{
    // Any other APC keeps the cap an OSC has, and is dropped past it without an answer.
    auto mock = MockTerm { TestPage };
    mock.writeToScreen(std::format("\033_Xfoo;{}\033\\", std::string(Sequence::MaxOscLength + 1, 'A')));
    CHECK(mock.terminal.peekInput().empty());
}
