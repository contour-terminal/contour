// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty graphics protocol's control-data parser, and for the grid effect of the
// commands that place an image.

#include <vtbackend/KittyGraphics.h>
#include <vtbackend/MockTerm.h>

#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace std::string_view_literals;
using namespace vtbackend;
using namespace vtbackend::kitty_graphics;

TEST_CASE("KittyGraphics.parse.minimal_query", "[kitty]")
{
    // The exact probe blessed/ucs-detect sends to decide whether the protocol is supported.
    auto const command = parseCommand("i=31,s=1,v=1,a=q,t=d,f=24;AAAA"sv);
    REQUIRE(command.has_value());
    CHECK(command->imageId == 31);
    CHECK(command->pixelWidth == 1);
    CHECK(command->pixelHeight == 1);
    CHECK(command->action == Action::Query);
    CHECK(command->medium == Medium::Direct);
    CHECK(command->format == Format::Rgb);
    CHECK(command->payload == "AAAA");
}

TEST_CASE("KittyGraphics.parse.defaults", "[kitty]")
{
    auto const command = parseCommand("i=1,s=1,v=1"sv);
    REQUIRE(command.has_value());
    // Transmit, direct, RGBA are the protocol's defaults, not ours to invent.
    CHECK(command->action == Action::Transmit);
    CHECK(command->medium == Medium::Direct);
    CHECK(command->format == Format::Rgba);
    CHECK(command->compression == Compression::None);
    CHECK(command->zIndex == 0);
    CHECK_FALSE(command->moreChunksFollow);
}

TEST_CASE("KittyGraphics.parse.payload_may_contain_semicolons", "[kitty]")
{
    // Only the FIRST semicolon separates control data from payload; base64 never contains one, but a
    // splitting parser would still corrupt anything that did.
    auto const command = parseCommand("a=T,s=1,v=1;AA;BB"sv);
    REQUIRE(command.has_value());
    CHECK(command->payload == "AA;BB");
}

TEST_CASE("KittyGraphics.parse.rejects_malformed", "[kitty]")
{
    CHECK_FALSE(parseCommand("i"sv).has_value());
    CHECK_FALSE(parseCommand("i=1,zz"sv).has_value());
    CHECK(parseCommand("a=X"sv).error() == Error::InvalidAction);
    CHECK(parseCommand("f=7"sv).error() == Error::InvalidFormat);
    CHECK(parseCommand("t=x"sv).error() == Error::InvalidMedium);

    // Missing dimensions are NOT a parse error: a continuation chunk legitimately carries none, and
    // only the terminal knows whether one is in flight. @see KittyGraphics.missing_dimensions_refused.
    CHECK(parseCommand("a=T,f=24;AAAA"sv).has_value());
}

TEST_CASE("KittyGraphics.missing_dimensions_refused", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=5;AAAA\033\\"sv);
    CHECK(mock.terminal.peekInput().find("EINVAL") != std::string_view::npos);
}

TEST_CASE("KittyGraphics.parse.ignores_unknown_keys", "[kitty]")
{
    // The protocol grows. Honouring the keys we know beats rejecting the whole command.
    auto const command = parseCommand("i=7,s=1,v=1,Q=9"sv);
    REQUIRE(command.has_value());
    CHECK(command->imageId == 7);
}

TEST_CASE("KittyGraphics.parse.quietness", "[kitty]")
{
    CHECK_FALSE(parseCommand("i=1,s=1,v=1")->quietOnSuccess);
    CHECK(parseCommand("i=1,s=1,v=1,q=1")->quietOnSuccess);
    CHECK_FALSE(parseCommand("i=1,s=1,v=1,q=1")->quietAlways);
    CHECK(parseCommand("i=1,s=1,v=1,q=2")->quietAlways);
}

TEST_CASE("KittyGraphics.query_is_answered", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\"sv);
    // blessed matches on exactly this shape: ESC _ G i=31 ; ... ESC \ containing "OK".
    CHECK(mock.terminal.peekInput() == "\033_Gi=31;OK\033\\");
}

TEST_CASE("KittyGraphics.transmit_and_display_puts_an_image_in_the_grid", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    // A 2x2 RGBA image: exactly one cell at this cell size.
    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\xFF\x00\x00\xFF"sv; // opaque red
    auto const encoded = crispy::base64::encode(pixels);

    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1;{}\033\\", encoded));

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment);
    CHECK(fragment->rasterizedImage().image().width() == Width(2));
    CHECK(fragment->rasterizedImage().image().height() == Height(2));
}

TEST_CASE("KittyGraphics.mismatched_payload_size_is_refused", "[kitty]")
{
    // The renderer reads width*height*bytesPerPixel from the buffer, so a short payload would be a
    // read past the end. It must be refused rather than stored.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    mock.writeToScreen(
        std::format("\033_Ga=T,f=32,s=64,v=64,i=2;{}\033\\", crispy::base64::encode("AAAA"sv)));

    CHECK(mock.terminal.peekInput().find("EINVAL") != std::string_view::npos);
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.unsupported_media_are_refused_not_ignored", "[kitty]")
{
    // Reading a path the application names would let it point the terminal at any file the user can
    // read; saying ENOTSUP is both safer and more useful than silence.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=32,s=1,v=1,t=f,i=3;L3RtcC94\033\\"sv);
    CHECK(mock.terminal.peekInput().find("ENOTSUP") != std::string_view::npos);
}

TEST_CASE("KittyGraphics.chunked_transmission_is_reassembled", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\x00\xFF\x00\xFF"sv;
    auto const encoded = crispy::base64::encode(pixels);
    auto const half = encoded.size() / 2;

    // Only the first chunk carries control data; the rest is payload.
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=4,m=1;{}\033\\", encoded.substr(0, half)));
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());

    mock.writeToScreen(std::format("\033_Gm=0;{}\033\\", encoded.substr(half)));
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.transmit_then_put_displays_the_stored_image", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\x00\x00\xFF\xFF"sv;

    // a=t stores without displaying.
    mock.writeToScreen(std::format("\033_Ga=t,f=32,s=2,v=2,i=9;{}\033\\", crispy::base64::encode(pixels)));
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());

    // a=p displays what was stored.
    mock.writeToScreen("\033_Ga=p,i=9\033\\"sv);
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.put_of_an_unknown_image_reports_ENOENT", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=p,i=1234\033\\"sv);
    CHECK(mock.terminal.peekInput().find("ENOENT") != std::string_view::npos);
}

TEST_CASE("KittyGraphics.non_kitty_APC_is_ignored", "[kitty]")
{
    // APC carries several application-defined protocols. One that is not ours must not be answered.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // Introducer 'X' rather than 'G'; the body is separated from it so that the two do not read as
    // one token to a spell checker.
    mock.writeToScreen("\033_X payload\033\\"sv);
    CHECK(mock.terminal.peekInput().empty());
}

// -- iTerm2 (OSC 1337) --------------------------------------------------------------------------

TEST_CASE("ITerm2.capabilities_are_reported", "[iterm2]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033]1337;Capabilities\a"sv);

    auto const reply = std::string(mock.terminal.peekInput());
    REQUIRE(reply.starts_with("\033]1337;Capabilities="));

    // Every advertised capability must be one Contour actually has -- an application that believes
    // a false advertisement takes a path that then fails.
    CHECK(reply.find("Sx") != std::string::npos);  // sixel
    CHECK(reply.find("Sy") != std::string::npos);  // synchronized output, DEC mode 2026
    CHECK(reply.find("H") != std::string::npos);   // hyperlinks, OSC 8
    CHECK(reply.find("Cw") != std::string::npos);  // clipboard writable, OSC 52
    CHECK(reply.find("Lr") != std::string::npos);  // DECSLRM
    CHECK(reply.find("No") != std::string::npos);  // notifications, OSC 99
}

TEST_CASE("ITerm2.a_download_is_not_drawn", "[iterm2]")
{
    // Without inline=1 the payload is a file transfer, not an image to display. Contour does not
    // save files an application sends, so nothing should reach the grid.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });
    mock.writeToScreen("\033]1337;File=name=eA==;size=4:AAAA\a"sv);
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("ITerm2.unknown_OSC_1337_verbs_are_ignored", "[iterm2]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033]1337;SetBadgeFormat=eA==\a"sv);
    CHECK(mock.terminal.peekInput().empty());
}
