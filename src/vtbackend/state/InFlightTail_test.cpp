// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/InFlightTail.hpp>
#include <vtbackend/state/TerminalState.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/StateHandover.hpp>

#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{
/// Hands a terminal parsing @p corpus over at every byte offset and requires each handover to be exact.
void requireExactAtEveryOffset(std::string_view corpus, test::HandoverSetup const& setup = {})
{
    auto const mismatch = test::findHandoverMismatch(corpus, setup);
    if (mismatch)
        FAIL(std::format("handed over after {} of {} bytes (\"{}\" next), compared after {}: {}",
                         mismatch->offset,
                         corpus.size(),
                         core::escape(corpus.substr(mismatch->offset, 8)),
                         mismatch->checkpoint,
                         mismatch->what));
    SUCCEED();
}

/// @p text in DECDMAC's hex encoding (Pen 1).
std::string hexEncoded(std::string_view text)
{
    auto hex = std::string {};
    for (auto const ch: text)
        hex += std::format("{:02X}", static_cast<unsigned char>(ch));
    return hex;
}
} // namespace

TEST_CASE("round-trip at every byte offset of a Sixel image", "[InFlightTail][StateRoundTrip]")
{
    // Shared registers (DECSET 1070 reset): the second image uses register 1 before redefining it, so a
    // replay that started from the registers as they are once it redefined them would draw it blue.
    auto const image = "\033Pq\"1;1;12;6#1~~~#2;2;0;100;0#2~~$-#3!6~#1;2;0;0;100#1~~\033\\"sv;
    requireExactAtEveryOffset(
        std::format("before\r\n\033[?1070l{}mid\033Pq#1;2;100;0;0#1~~~~\033\\{}after\r\n"
                    "\033[?1070h{}end",
                    image,
                    image,
                    image));
}

TEST_CASE("round-trip at every byte offset of a ReGIS drawing", "[InFlightTail][StateRoundTrip]")
{
    auto const drawing = "text\r\n\033P1pS(E)W(I3)P[100,100]V[200,200][300,100]T'Hi'\033\\more\r\n"
                         "\033P0pP[50,50]V[60,60]W(I5)C[+30]R(P)\033\\done"sv;
    requireExactAtEveryOffset(drawing);

    // And the canvas itself, handed over halfway through the first string's payload.
    auto const setup = test::HandoverSetup {};
    auto const half = drawing.find("V[200");
    auto source = setup.makeTerminal();
    source->writeToScreen(drawing.substr(0, half));
    auto target = test::handedOver(*source, setup);
    REQUIRE(target.has_value());
    source->writeToScreen(drawing.substr(half));
    (*target)->writeToScreen(drawing.substr(half));
    auto const sourceCanvas = source->terminal.captureState().pages.front().regis;
    auto const targetCanvas = (*target)->terminal.captureState().pages.front().regis;
    REQUIRE(sourceCanvas.has_value());
    REQUIRE(targetCanvas.has_value());
    CHECK(std::ranges::count_if(sourceCanvas->pixels, [](uint8_t byte) { return byte != 0; }) > 0);
    CHECK(targetCanvas->pixels == sourceCanvas->pixels);
}

TEST_CASE("round-trip at every byte offset of a UTF-8 text with 4-byte sequences",
          "[InFlightTail][StateRoundTrip]")
{
    // Clusters that join across the offset (a skin tone, a ZWJ sequence, a combining mark), and REP after
    // a 4-byte character.
    requireExactAtEveryOffset("a\xF0\x9F\x98\x80"
                              "b\xF0\x9D\x84\x9E"
                              "c\r\n"
                              "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD e\xCC\x81 \xC3\xBC \xE6\x97\xA5\xE6\x9C\xAC "
                              "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\r\n"
                              "\xF0\x9F\x98\x80\033[3b\xE2\x9C\x85\033[2b end"sv);
}

TEST_CASE("round-trip at every byte offset of OSC 8 hyperlinked text", "[InFlightTail][StateRoundTrip]")
{
    requireExactAtEveryOffset(
        "\033]8;id=x;https://example.com/a\033\\link\033]8;;\033\\ plain "
        "\033]8;;http://example.com/b\007other\033]8;;\007\r\n"
        "\033]2;a title\033\\\033]8;id=x;https://example.com/a\033\\again\033]8;;\033\\"sv);
}

TEST_CASE("round-trip at every byte offset of a DECDMAC macro definition", "[InFlightTail][StateRoundTrip]")
{
    // Macro 3's body opens a definition of macro 4 -- which, begun in a macro body, is ignored whole, the
    // payload the host goes on to send included. A replay must hook it as ignored too, though no macro runs
    // any more by then.
    requireExactAtEveryOffset(std::format("\033P1;0;0!zHello \033[1mBold\033[m\033\\"
                                          "\033P2;0;1!z{}\033\\"
                                          "\033P3;0;1!z{}\033\\"
                                          "\033[1*z \033[2*z \033[3*zXYZ\033\\ \033[4*z end",
                                          hexEncoded("hex\r\n"),
                                          hexEncoded("\033P4;0;0!z")));
}

TEST_CASE("round-trip at every byte offset of a DECDLD soft font", "[InFlightTail][StateRoundTrip]")
{
    requireExactAtEveryOffset("\033P1;0;0;10;10;0;20;0{ A@\033\\"
                              "\033P1;1;1;10;10;0;20;0{ A~~~~~/~~~~~;@@@@@/@@@@@\033\\"
                              "\033) A\x0E!\"#\x0F text"sv);
}

TEST_CASE("round-trip at every byte offset of C0 controls inside sequences", "[InFlightTail][StateRoundTrip]")
{
    // VT500 parsers execute a C0 control met inside ESC, CSI and a DCS's parameters, and carry on with the
    // sequence. A handover between such a control and the sequence's end must not execute it twice: not the
    // LF or CR inside a CUP, the BS inside a CUF, the HT inside an ESC intermediate, the LF inside DECUDK's
    // parameters -- nor the ENQ whose answerback a replay would send again.
    // Each is followed by text, which lands where the cursor went: one executed twice moves it again.
    requireExactAtEveryOffset("ab\r\ncd\033[\n2Cx\033[\b3Cy\033[1\r;2Dz\033 \tFw\033[1\005;2H"
                              "\033P1;\n1|\033\\v\033[?1\n;2\n5hu\033[\0335;5H\033[K\033[0\t\x7F"
                              "m tail"sv);
}

TEST_CASE("round-trip at every byte offset of VT52 commands and REP", "[InFlightTail][StateRoundTrip]")
{
    requireExactAtEveryOffset("x\033[3b\033[?2l\033Y%%a\033A\033Y\n\"b\033<\033[1;1Hz\033[2b"sv);
}

TEST_CASE("round-trip at every byte offset of conformance-level and margin-mode changes",
          "[InFlightTail][StateRoundTrip]")
{
    // What CSI s means depends on DECLRMM (DECSLRM or SCOSC) and on the conformance level DECSCL set.
    requireExactAtEveryOffset(
        "\033[?69h\033[3;12s\033[1;1Hx\033[?69l\033[s\033[5;5H\033[u y"
        "\033[61\"p\033[?1$p\033[s\033[2;2H\033[u\033[?69h\033[2;8s z\033[65;1\"p\033[s q"sv);
}

TEST_CASE("a tail beyond the limit reports Overflowed", "[InFlightTail]")
{
    auto const setup = test::HandoverSetup { .inFlightTailLimit = 16 };
    auto source = setup.makeTerminal();
    source->writeToScreen("\033]2;0123456789"sv);
    CHECK(source->terminal.inFlightTailCompleteness() == TailCompleteness::Complete);
    CHECK(source->terminal.captureState().inFlightTail.size() == 14);

    source->writeToScreen("abcdef"sv);
    CHECK(source->terminal.inFlightTailCompleteness() == TailCompleteness::Overflowed);
    auto const overflowed = source->terminal.captureState();
    CHECK(overflowed.inFlightTailOverflowed == TailCompleteness::Overflowed);
    CHECK(overflowed.inFlightTail.empty());
    CHECK_FALSE(test::handedOver(*source, setup).has_value());

    // Once the sequence ends the terminal can be handed over again, and the next one is kept from its start.
    source->writeToScreen("\033\\x\033[3"sv);
    CHECK(source->terminal.inFlightTailCompleteness() == TailCompleteness::Complete);
    CHECK(source->terminal.captureState().inFlightTail == std::vector<uint8_t> { 0x1B, '[', '3' });
    CHECK(source->terminal.windowTitle() == "0123456789abcdef");
    CHECK(test::handedOver(*source, setup).has_value());
}

TEST_CASE("a half-decoded character a sequence cuts short is not carried", "[InFlightTail]")
{
    // The ESC cannot continue the lead byte's character, so it ends it -- printed as U+FFFD -- before it
    // begins the sequence: only the sequence is in flight.
    auto const setup = test::HandoverSetup {};
    auto source = setup.makeTerminal();
    source->writeToScreen("\xC3\033[1"sv);
    auto const captured = source->terminal.captureState();
    CHECK(captured.inFlightTail == std::vector<uint8_t> { 0x1B, '[', '1' });
    auto target = test::handedOver(*source, setup);
    REQUIRE(target.has_value());
    source->writeToScreen("mA"sv);
    (*target)->writeToScreen("mA"sv);
    CHECK((*target)->terminal.primaryScreen().renderMainPageText()
          == source->terminal.primaryScreen().renderMainPageText());
    CHECK(test::encodedState((*target)->terminal.captureState())
          == test::encodedState(source->terminal.captureState()));
}
