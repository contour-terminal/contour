// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty clipboard protocol (OSC 5522) and its paste-notification mode (5522).

#include <vtbackend/KittyClipboard.h>
#include <vtbackend/MockTerm.h>

#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

using namespace std::string_view_literals;
using namespace vtbackend;
using namespace vtbackend::kitty_clipboard;

// {{{ packet parser

TEST_CASE("KittyClipboard.parse.read_request", "[kittyclipboard]")
{
    auto const packet = parsePacket("type=read:id=abc;dGV4dC9wbGFpbg=="sv);
    REQUIRE(packet.has_value());
    CHECK(packet->type == PacketType::Read);
    CHECK(packet->id == "abc");
    CHECK(packet->location == Location::Clipboard);
    CHECK(crispy::base64::decode(packet->payload) == "text/plain");
}

TEST_CASE("KittyClipboard.parse.metadata_is_colon_separated", "[kittyclipboard]")
{
    // As in OSC 66, colons separate the pairs and the semicolon separates metadata from payload --
    // unlike OSC 52, whose parameters are semicolon-separated.
    // `mime=` arrives base64-encoded (`mime=<base64 encoded mime type>`), so the parser decodes it
    // and callers compare against a real MIME name rather than an encoded one.
    auto const packet = parsePacket("type=wdata:mime=dGV4dC9wbGFpbg==:id=7;QUI="sv);
    REQUIRE(packet.has_value());
    CHECK(packet->type == PacketType::WriteData);
    CHECK(packet->mimeType == "text/plain");
    CHECK(packet->id == "7");
    CHECK(crispy::base64::decode(packet->payload) == "AB");
}

TEST_CASE("KittyClipboard.parse.primary_selection", "[kittyclipboard]")
{
    CHECK(parsePacket("type=read:loc=primary;"sv)->location == Location::PrimarySelection);
    CHECK(parsePacket("type=read;"sv)->location == Location::Clipboard);
}

TEST_CASE("KittyClipboard.parse.rejects_malformed", "[kittyclipboard]")
{
    CHECK(parsePacket("type=read:novalue;"sv).error() == Error::MalformedMetadata);
    CHECK(parsePacket("type=nonsense;"sv).error() == Error::UnknownType);
}

TEST_CASE("KittyClipboard.parse.ignores_unknown_keys", "[kittyclipboard]")
{
    // `name` and `pw` exist in the protocol but mean nothing here; a packet carrying them must still
    // do the part that is understood.
    auto const packet = parsePacket("type=write:name=Zm9v:pw=cHc=:id=1;"sv);
    REQUIRE(packet.has_value());
    CHECK(packet->type == PacketType::Write);
    CHECK(packet->id == "1");
}

TEST_CASE("KittyClipboard.supported_mime_types", "[kittyclipboard]")
{
    CHECK(isSupportedMimeType("text/plain"));
    CHECK(isSupportedMimeType("UTF8_STRING"));
    CHECK(isSupportedMimeType("")); // unspecified means text, for a text clipboard
    CHECK_FALSE(isSupportedMimeType("image/png"));
    CHECK_FALSE(isSupportedMimeType("application/octet-stream"));
}

// }}}
// {{{ terminal behaviour

TEST_CASE("KittyClipboard.mode_5522_is_a_recognised_mode", "[kittyclipboard]")
{
    // This is exactly the probe blessed uses to decide the protocol is supported: DECRQM for 5522.
    // A "not recognised" (Ps=0) answer means unsupported.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    auto const modeNum = toDECModeNum(DECMode::PasteMimeNotifications);
    mock.writeToScreen(std::format("\033[?{}$p", modeNum));
    CHECK(mock.terminal.peekInput() == std::format("\033[?{};2$y", modeNum));
}

TEST_CASE("KittyClipboard.write_transmission_reaches_the_clipboard", "[kittyclipboard]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]5522;type=write:id=1\033\\"sv);
    mock.writeToScreen(std::format("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==:id=1;{}\033\\",
                                   crispy::base64::encode("hello"sv)));
    // An empty chunk ends the transmission.
    mock.writeToScreen("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==:id=1;\033\\"sv);

    CHECK(mock.clipboardData == "hello");
    CHECK(mock.terminal.peekInput() == "\033]5522;type=write:status=DONE\033\\");
}

TEST_CASE("KittyClipboard.chunks_are_reassembled_in_order", "[kittyclipboard]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]5522;type=write:id=2\033\\"sv);
    for (auto const& part: { "one "sv, "two "sv, "three"sv })
        mock.writeToScreen(std::format("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==:id=2;{}\033\\",
                                       crispy::base64::encode(part)));
    mock.writeToScreen("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==:id=2;\033\\"sv);

    // The point of the test: three chunks arrive separately and must land concatenated, in order.
    CHECK(mock.clipboardData == "one two three");
    CHECK(mock.terminal.peekInput() == "\033]5522;type=write:status=DONE\033\\");
}

TEST_CASE("KittyClipboard.an_endless_write_stream_is_abandoned_not_accumulated", "[kittyclipboard]")
{
    // The wdata buffer is only ever flushed by the empty end-of-transmission chunk, so a stream that
    // never sends one grows it without bound -- the same remote memory-exhaustion the kitty graphics
    // chunk path is bounded against. Past the cap the transmission is abandoned whole: a truncated
    // clipboard is not the data the application asked to store.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]5522;type=write:id=9\033\\"sv);

    // Chunks stay under Sequence::MaxOscLength; anything larger would be truncated by the parser
    // rather than reaching the accumulator.
    auto const chunk = crispy::base64::encode(std::string(static_cast<std::size_t>(32 * 1024), 'x'));
    auto const enough = (kitty_clipboard::MaxClipboardWriteSize / (static_cast<size_t>(32 * 1024))) + 2;
    for ([[maybe_unused]] auto const i: std::views::iota(size_t { 0 }, enough))
        mock.writeToScreen(std::format("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==:id=9;{}\033\\", chunk));

    CHECK(mock.terminal.peekInput() == "\033]5522;type=write:status=EIO\033\\");

    // The transmission is closed, so the terminating chunk delivers nothing to the clipboard.
    mock.writeToScreen("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==:id=9;\033\\"sv);
    CHECK(mock.clipboardData.empty());
}

TEST_CASE("KittyClipboard.data_without_an_open_write_is_refused", "[kittyclipboard]")
{
    // A wdata packet with no preceding write is not a transmission; accepting it would let a stray
    // packet overwrite the clipboard.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen(
        std::format("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;{}\033\\", crispy::base64::encode("x"sv)));
    CHECK(mock.terminal.peekInput().empty());
    CHECK(mock.clipboardData.empty());
}

TEST_CASE("KittyClipboard.an_unsupported_mime_type_is_refused_not_dropped", "[kittyclipboard]")
{
    // Contour's clipboard is text. Accepting an image and storing nothing would leave the
    // application believing its data is on the clipboard.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033]5522;type=write:id=3\033\\"sv);
    mock.writeToScreen(std::format("\033]5522;type=wdata:mime=aW1hZ2UvcG5n:id=3;{}\033\\",
                                   crispy::base64::encode("\x89PNG"sv)));
    CHECK(mock.terminal.peekInput() == "\033]5522;type=write:status=ENOSYS\033\\");
    CHECK(mock.clipboardData.empty());
}

TEST_CASE("KittyClipboard.read_is_refused_when_not_permitted", "[kittyclipboard]")
{
    // Reading the clipboard lets an application exfiltrate whatever the user last copied, so it is
    // gated by the same setting OSC 52 reads are -- and that setting defaults to off.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.terminal.settings().allowClipboardRead = false;

    mock.writeToScreen(
        std::format("\033]5522;type=read:id=4;{}\033\\", crispy::base64::encode("text/plain"sv)));
    CHECK(mock.terminal.peekInput() == "\033]5522;type=read:status=EPERM\033\\");
}

TEST_CASE("KittyClipboard.a_write_to_the_primary_selection_is_refused", "[kittyclipboard]")
{
    // `loc=primary` targets the X11 primary selection, which Contour does not implement. The spec's
    // answer for that is ENOSYS. Ignoring the key instead destroys whatever the user last copied with
    // Ctrl+Shift+C and then reports DONE, so the application believes it wrote where it asked.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.clipboardData = "user selection";

    mock.writeToScreen("\033]5522;type=write:loc=primary\033\\"sv);
    mock.writeToScreen(std::format("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;{}\033\\",
                                   crispy::base64::encode("clobber"sv)));
    mock.writeToScreen("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;\033\\"sv);

    CHECK(mock.terminal.peekInput().find("status=ENOSYS") != std::string_view::npos);
    CHECK(mock.clipboardData == "user selection");
}

TEST_CASE("KittyClipboard.the_targets_probe_lists_the_available_types", "[kittyclipboard]")
{
    // A payload of a single base64-encoded period asks which MIME types are on the clipboard. The
    // spec serves it without a permission prompt, precisely so a client is not prompted twice (once
    // to list, once to read). Running '.' through the supported-type check refuses it instead.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.terminal.settings().allowClipboardRead = true;
    mock.clipboardData = "hello";

    mock.writeToScreen(std::format("\033]5522;type=read;{}\033\\", crispy::base64::encode("."sv)));

    auto const reply = std::string(mock.terminal.peekInput());
    CHECK(reply.find("status=ENOSYS") == std::string::npos);
    // The answer names the type rather than carrying the data.
    CHECK(reply.find(std::string(crispy::base64::encode("text/plain"sv))) != std::string::npos);
    CHECK(reply.find("status=DONE") != std::string::npos);
}

TEST_CASE("KittyClipboard.a_large_read_is_chunked", "[kittyclipboard]")
{
    // "The terminal emulator should chunk up the data for an individual type, into chunks of size no
    // more than 4096 bytes (4096 is the size of a chunk BEFORE base64 encoding)". A client with a
    // bounded OSC buffer truncates or drops a single oversized packet.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.terminal.settings().allowClipboardRead = true;
    mock.clipboardData = std::string(10000, 'x');

    mock.writeToScreen(std::format("\033]5522;type=read;{}\033\\", crispy::base64::encode("text/plain"sv)));

    auto const reply = std::string(mock.terminal.peekInput());
    auto dataPackets = size_t { 0 };
    for (auto pos = reply.find("status=DATA"); pos != std::string::npos;
         pos = reply.find("status=DATA", pos + 1))
        ++dataPackets;
    // 10000 bytes at 4096 per chunk is three packets, not one.
    CHECK(dataPackets == 3);
}

TEST_CASE("KittyClipboard.a_write_survives_a_status_line_switch", "[kittyclipboard]")
{
    // The transmission belongs to the TERMINAL, not to whichever screen the sequence happened to be
    // dispatched to. A status-line-updating program switches with DECSASD between chunks; if the
    // buffer lives on the screen, those chunks land on the status screen (which has no open
    // transmission), their bytes are dropped, and the final chunk still answers DONE.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]5522;type=write\033\\"sv);
    mock.writeToScreen(
        std::format("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;{}\033\\", crispy::base64::encode("one "sv)));
    mock.writeToScreen("\033[1$}"sv); // DECSASD: to the status line
    mock.writeToScreen(
        std::format("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;{}\033\\", crispy::base64::encode("two"sv)));
    mock.writeToScreen("\033[0$}"sv); // back to the main display
    mock.writeToScreen("\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;\033\\"sv);

    CHECK(mock.clipboardData == "one two");
}

TEST_CASE("KittyClipboard.a_read_is_answered_in_the_5522_protocol", "[kittyclipboard]")
{
    // The spec answers a read with its own packets -- an OK, then one DATA packet per MIME type
    // carrying a base64 mime and base64 data, then DONE. Answering with OSC 52 instead leaves a
    // client that is parsing for `\033]5522;` waiting until its read timeout, while a legacy OSC 52
    // handler in the same client may swallow the reply as an unsolicited paste.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.terminal.settings().allowClipboardRead = true;
    mock.clipboardData = "hello";

    mock.writeToScreen(std::format("\033]5522;type=read;{}\033\\", crispy::base64::encode("text/plain"sv)));

    auto const reply = std::string(mock.terminal.peekInput());
    CHECK(reply.find("\033]52;") == std::string::npos); // never the OSC 52 shape
    CHECK(reply
          == std::format("\033]5522;type=read:status=OK\033\\"
                         "\033]5522;type=read:status=DATA:mime={};{}\033\\"
                         "\033]5522;type=read:status=DONE\033\\",
                         crispy::base64::encode("text/plain"sv),
                         crispy::base64::encode("hello"sv)));
}

TEST_CASE("KittyClipboard.a_read_for_only_unsupported_types_is_refused", "[kittyclipboard]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.terminal.settings().allowClipboardRead = true;

    mock.writeToScreen(
        std::format("\033]5522;type=read:id=5;{}\033\\", crispy::base64::encode("image/png"sv)));
    CHECK(mock.terminal.peekInput() == "\033]5522;type=read:status=ENOSYS\033\\");
}
