// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty clipboard protocol (OSC 5522) and its paste-notification mode (5522).

#include <vtbackend/KittyClipboard.h>
#include <vtbackend/MockTerm.h>

#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

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
    auto const packet = parsePacket("type=wdata:mime=text/plain:id=7;QUI="sv);
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
    mock.writeToScreen(
        std::format("\033]5522;type=wdata:mime=text/plain:id=1;{}\033\\", crispy::base64::encode("hello"sv)));
    // An empty chunk ends the transmission.
    mock.writeToScreen("\033]5522;type=wdata:mime=text/plain:id=1;\033\\"sv);

    CHECK(mock.clipboardData == "hello");
    CHECK(mock.terminal.peekInput().find("DONE") != std::string_view::npos);
}

TEST_CASE("KittyClipboard.chunks_are_reassembled_in_order", "[kittyclipboard]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };

    mock.writeToScreen("\033]5522;type=write:id=2\033\\"sv);
    for (auto const& part: { "one "sv, "two "sv, "three"sv })
        mock.writeToScreen(
            std::format("\033]5522;type=wdata:mime=text/plain:id=2;{}\033\\", crispy::base64::encode(part)));
    mock.writeToScreen("\033]5522;type=wdata:mime=text/plain:id=2;\033\\"sv);

    // The point of the test: three chunks arrive separately and must land concatenated, in order.
    CHECK(mock.clipboardData == "one two three");
    CHECK(mock.terminal.peekInput().find("DONE") != std::string_view::npos);
}

TEST_CASE("KittyClipboard.data_without_an_open_write_is_refused", "[kittyclipboard]")
{
    // A wdata packet with no preceding write is not a transmission; accepting it would let a stray
    // packet overwrite the clipboard.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen(
        std::format("\033]5522;type=wdata:mime=text/plain;{}\033\\", crispy::base64::encode("x"sv)));
    CHECK(mock.terminal.peekInput().empty());
    CHECK(mock.clipboardData.empty());
}

TEST_CASE("KittyClipboard.an_unsupported_mime_type_is_refused_not_dropped", "[kittyclipboard]")
{
    // Contour's clipboard is text. Accepting an image and storing nothing would leave the
    // application believing its data is on the clipboard.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.writeToScreen("\033]5522;type=write:id=3\033\\"sv);
    mock.writeToScreen(std::format("\033]5522;type=wdata:mime=image/png:id=3;{}\033\\",
                                   crispy::base64::encode("\x89PNG"sv)));
    CHECK(mock.terminal.peekInput().find("ENOSYS") != std::string_view::npos);
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
    CHECK(mock.terminal.peekInput().find("EPERM") != std::string_view::npos);
}

TEST_CASE("KittyClipboard.a_read_for_only_unsupported_types_is_refused", "[kittyclipboard]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(3), ColumnCount(10) } };
    mock.terminal.settings().allowClipboardRead = true;

    mock.writeToScreen(
        std::format("\033]5522;type=read:id=5;{}\033\\", crispy::base64::encode("image/png"sv)));
    CHECK(mock.terminal.peekInput().find("ENOSYS") != std::string_view::npos);
}
