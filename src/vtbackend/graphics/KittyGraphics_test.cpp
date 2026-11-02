// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty graphics protocol's control-data parser, and for the grid effect of the
// commands that place an image.

#include <vtbackend/graphics/KittyGraphics.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <zlib.h>

using namespace std::string_literals;
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

TEST_CASE("KittyGraphics.parse.compose_keys_are_frames_and_origins", "[kitty]")
{
    auto const command = parseCommand("a=c,i=1,r=7,c=9,w=23,h=27,X=4,Y=8,x=1,y=3"sv);
    REQUIRE(command.has_value());
    CHECK(command->action == Action::Compose);
    CHECK(command->composeSourceFrame == 7);
    CHECK(command->composeDestinationFrame == 9);
    CHECK(command->sourceWidth == 23);
    CHECK(command->sourceHeight == 27);
    CHECK(command->composeSourceX == 4);
    CHECK(command->composeSourceY == 8);
    CHECK(command->composeDestinationX == 1);
    CHECK(command->composeDestinationY == 3);
    CHECK(command->compositionMode == CompositionMode::AlphaBlend);
    CHECK_FALSE(command->doNotMoveCursor);
    CHECK(command->columns == 0);
    CHECK(command->rows == 0);
}

TEST_CASE("KittyGraphics.parse.compose_C_selects_overwrite", "[kitty]")
{
    auto const command = parseCommand("a=c,i=1,r=7,c=9,w=23,h=27,X=4,Y=8,x=1,y=3,C=1"sv);
    REQUIRE(command.has_value());
    CHECK(command->compositionMode == CompositionMode::Replace);
    CHECK_FALSE(command->doNotMoveCursor);
}

TEST_CASE("KittyGraphics.parse.frame_X_and_Y_keep_their_meaning", "[kitty]")
{
    auto const command = parseCommand("a=f,i=1,X=1,Y=5,x=2,y=3"sv);
    REQUIRE(command.has_value());
    CHECK(command->compositionMode == CompositionMode::Replace);
    CHECK(command->frameBackground == 5);
    CHECK(command->sourceX == 2);
    CHECK(command->sourceY == 3);
}

TEST_CASE("KittyGraphics.parse.defaults", "[kitty]")
{
    auto const command = parseCommand("i=1,s=1,v=1"sv);
    REQUIRE(command.has_value());
    // Transmit, direct, RGBA are the protocol's defaults, not ours to invent.
    CHECK(command->action == Action::Transmit);
    CHECK(command->medium == Medium::Direct);
    CHECK(command->transmissionFormat() == Format::Rgba);
    CHECK_FALSE(command->format.has_value());
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
    CHECK(mock.terminal.peekInput().contains("EINVAL"));
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

TEST_CASE("KittyGraphics.query_validates_what_a_transmission_would_reject", "[kitty]")
{
    // `a=q` exists so an application can discover what a transmission would do WITHOUT performing
    // one. Answering OK and then failing the real transmission leaves it with nothing to fall back
    // to: its feature probe already said yes.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };

    SECTION("unsupported medium")
    {
        mock.writeToScreen("\033_Gi=31,s=1,v=1,a=q,t=t,f=24;AAAA\033\\"sv);
        CHECK(mock.terminal.peekInput().contains("ENOTSUP"));
    }

    SECTION("compressed payload is supported")
    {
        mock.writeToScreen("\033_Gi=32,s=1,v=1,a=q,t=d,o=z,f=24;AAAA\033\\"sv);
        CHECK(mock.terminal.peekInput() == "\033_Gi=32;OK\033\\");
    }

    SECTION("missing dimensions on a raw pixel format")
    {
        mock.writeToScreen("\033_Gi=33,a=q,t=d,f=24;AAAA\033\\"sv);
        CHECK(mock.terminal.peekInput().contains("EINVAL"));
    }
}

TEST_CASE("KittyGraphics.lower_case_delete_keeps_the_image_data", "[kitty]")
{
    // In the protocol the CASE of `d=` decides how far the delete reaches: lower case removes
    // placements and leaves the data resident. The standard redraw idiom is `a=d,d=a` followed by
    // `a=p,i=1` -- destroying the data on the first step makes the second answer ENOENT.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\xFF\x00\x00\xFF"sv;
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1;{}\033\\", core::base64::encode(pixels)));
    REQUIRE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());

    mock.writeToScreen("\033_Ga=d,d=a,i=1\033\\"sv);
    // The placement is gone ...
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());

    // ... but the image can still be placed again without retransmitting its pixels. The cursor is
    // homed first: `a=T` moved it past the image it displayed, and `a=p` places at the cursor.
    mock.terminal.flushInput();
    mock.writeToScreen("\033[H\033_Ga=p,i=1\033\\"sv);
    CHECK(!mock.terminal.peekInput().contains("ENOENT"));
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.upper_case_delete_frees_the_image_data", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\xFF\x00\x00\xFF"sv;
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1;{}\033\\", core::base64::encode(pixels)));

    mock.writeToScreen("\033_Ga=d,d=A,i=1\033\\"sv);
    mock.terminal.flushInput();
    mock.writeToScreen("\033_Ga=p,i=1\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
}

TEST_CASE("KittyGraphics.an_endless_chunk_stream_is_abandoned_not_accumulated", "[kitty]")
{
    // Every byte a remote host writes ends up in the reassembly buffer, and a stream that never
    // sends its terminating m=0 would otherwise grow until the process is killed -- taking every
    // pane in the window with it. The transmission is abandoned whole rather than truncated: a
    // partial payload decoded against its declared dimensions is not an image.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };

    auto const chunk = std::string(4096, 'A');
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=4096,v=4096,i=1,m=1;{}\033\\", chunk));

    auto const chunksToOverrun = (kitty_graphics::MaxChunkedPayloadSize / chunk.size()) + 2;
    for ([[maybe_unused]] auto const i: std::views::iota(size_t { 0 }, chunksToOverrun))
        mock.writeToScreen(std::format("\033_Gm=1;{}\033\\", chunk));

    CHECK(mock.terminal.peekInput().contains("EINVAL"));

    // Abandoning drops the opener, so the chunks still arriving open a fresh transmission of their
    // own -- bounded in turn. Close it, and the next complete transmission is read on its own terms.
    mock.writeToScreen("\033_Gm=0;\033\\"sv);
    mock.terminal.flushInput();
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });
    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\xFF\x00\x00\xFF"sv;
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=2;{}\033\\", core::base64::encode(pixels)));
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.hard_reset_drops_a_half_open_transmission", "[kitty]")
{
    // A program that dies mid-transmission leaves the opener engaged. Surviving RIS, it would
    // swallow the NEXT program's first graphics command as a continuation chunk -- taking the dead
    // command's format and dimensions instead of its own.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    mock.writeToScreen("\033_Ga=T,f=32,s=64,v=64,i=1,m=1;AAAA\033\\"sv); // no terminating m=0
    mock.writeToScreen("\033c"sv);                                       // RIS

    // A complete, well-formed transmission now succeeds on its own terms.
    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\xFF\x00\x00\xFF"sv;
    mock.terminal.flushInput();
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=2;{}\033\\", core::base64::encode(pixels)));

    CHECK(!mock.terminal.peekInput().contains("EINVAL"));
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.transmit_and_display_puts_an_image_in_the_grid", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    // A 2x2 RGBA image: exactly one cell at this cell size.
    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\xFF\x00\x00\xFF"sv; // opaque red
    auto const encoded = core::base64::encode(pixels);

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

    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=64,v=64,i=2;{}\033\\", core::base64::encode("AAAA"sv)));

    CHECK(mock.terminal.peekInput().contains("EINVAL"));
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.unsupported_media_are_refused_not_ignored", "[kitty]")
{
    // Reading a path the application names would let it point the terminal at any file the user can
    // read; saying ENOTSUP is both safer and more useful than silence.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=32,s=1,v=1,t=f,i=3;L3RtcC94\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOTSUP"));
}

TEST_CASE("KittyGraphics.chunked_transmission_is_reassembled", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\x00\xFF\x00\xFF"sv;
    auto const encoded = core::base64::encode(pixels);
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
    mock.writeToScreen(std::format("\033_Ga=t,f=32,s=2,v=2,i=9;{}\033\\", core::base64::encode(pixels)));
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());

    // a=p displays what was stored.
    mock.writeToScreen("\033_Ga=p,i=9\033\\"sv);
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.put_of_an_unknown_image_reports_ENOENT", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=p,i=1234\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
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

    auto const reply = mock.terminal.peekInput();
    REQUIRE(reply.starts_with("\033]1337;Capabilities="));

    // Every advertised capability must be one Contour actually has -- an application that believes
    // a false advertisement takes a path that then fails.
    CHECK(reply.contains("Sx")); // sixel
    CHECK(reply.contains("Sy")); // synchronized output, DEC mode 2026
    CHECK(reply.contains("H"));  // hyperlinks, OSC 8
    CHECK(reply.contains("Cw")); // clipboard writable, OSC 52
    CHECK(reply.contains("Lr")); // DECSLRM
    CHECK(reply.contains("No")); // notifications, OSC 99
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

TEST_CASE("ITerm2.an_absurd_height_does_not_scroll_the_screen_away", "[iterm2]")
{
    // `height=` is an application-supplied cell count that reaches renderImage with autoScroll set,
    // and that function's remainder loop performs one linefeed() per row it could not draw. Unclamped,
    // `height=1000` scrolls a thousand rows: at the scale an application can actually ask for
    // (`height=90000000`) the terminal wedges for minutes and the scrollback is shredded. The image
    // can never occupy more than the page, so the request is clamped to it.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) }, LineCount(20) };
    auto const& screen = mock.terminal.primaryScreen();
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    // The PNG decoder is an injected dependency and MockTerm has none, so without this the image
    // never decodes, renderImage returns before the scroll loop, and the test proves nothing.
    mock.terminal.setImageDecoder(
        [](ImageFormat, std::span<uint8_t const>, ImageSize& size) -> std::optional<Image::Data> {
            size = ImageSize { Width(2), Height(2) };
            return Image::Data(2uz * 2uz * 4uz, uint8_t { 0xFF });
        });

    mock.writeToScreen("marker"sv);
    mock.writeToScreen("\033]1337;File=inline=1;height=1000:AAAA\a"sv);

    // Reaching the placement at all is what makes the assertion below meaningful.
    auto placed = false;
    for (auto const line: std::views::iota(-20, 4))
        for (auto const column: std::views::iota(0, 8))
            if (screen.at(LineOffset(line), ColumnOffset(column)).imageFragment())
                placed = true;
    REQUIRE(placed);

    // A thousand linefeeds would push `marker` clean out of a twenty-line history; a page's worth
    // does not.
    auto found = false;
    for (auto const line: std::views::iota(-20, 4))
        if (screen.grid().lineAt(LineOffset(line)).toUtf8Trimmed().contains("marker"))
            found = true;
    CHECK(found);
}

TEST_CASE("ITerm2.unknown_OSC_1337_verbs_are_ignored", "[iterm2]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033]1337;SetBadgeFormat=eA==\a"sv);
    CHECK(mock.terminal.peekInput().empty());
}

TEST_CASE("KittyGraphics.a_row_count_above_INT_MAX_does_not_wedge_the_terminal", "[kitty]")
{
    // `r=`/`c=` are uint32_t on the wire but the counts they feed are signed, so anything above
    // INT_MAX narrows to a negative extent. A negative extent puts GridSize::end() before begin(),
    // and the placement loop counts upwards: it never terminates, and every iteration writes further
    // past the grid. One escape sequence from any process holding the tty is enough. Reaching the
    // assertions below at all is the substance of this test -- before the clamp it never returned.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\xFF\x00\x00\xFF"sv;
    auto const encoded = core::base64::encode(pixels);

    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1,c=1,r=3000000000;{}\033\\", encoded));

    // Clamped to the page, so the image is placed rather than dropped -- and only within the page.
    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).imageFragment());
    CHECK(!mock.terminal.peekInput().contains("EINVAL"));
}

TEST_CASE("KittyGraphics.a_column_count_above_INT_MAX_does_not_wedge_the_terminal", "[kitty]")
{
    // The column axis narrows the same way, and additionally drives GridSize::iterator's makeOffset(),
    // which divides by the column count.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\x00\x00\xFF\xFF"sv;
    auto const encoded = core::base64::encode(pixels);

    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1,c=4000000000,r=1;{}\033\\", encoded));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

TEST_CASE("KittyGraphics.an_oversized_but_positive_cell_count_is_clamped_to_the_page", "[kitty]")
{
    // The ordinary in-range case of the same clamp: a request larger than the page places over the
    // page and stops there, rather than being refused outright.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string {};
    for (int i = 0; i < 4; ++i)
        pixels += "\x00\xFF\x00\xFF"sv;
    auto const encoded = core::base64::encode(pixels);

    // `C=1`: a page-filling placement otherwise advances the cursor past the bottom-right corner, which
    // wraps and scrolls the page by a line, as kitty does -- the clamp is what is under test here.
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1,c=99,r=99,C=1;{}\033\\", encoded));

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).imageFragment());
    CHECK(screen.at(LineOffset(3), ColumnOffset(7)).imageFragment());
}

TEST_CASE("KittyGraphics.an_APC_body_past_the_cap_is_dropped_not_dispatched_truncated", "[kitty]")
{
    // An APC body is bounded, as OSC is, because it is attacker-controlled. What the bound must NOT
    // do is hand the front of an over-long body on as though it were whole: the kitty parser then
    // sees a well-formed command carrying base64 cut off mid-stream, so the image decodes to garbage
    // or not at all -- and because the truncation was silent, the terminal answered as if the
    // transmission had succeeded, leaving the client no reason to retry with chunking.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    // 100x100 RGBA is 40000 bytes, whose base64 is 53336 -- past the 50 KiB cap.
    auto pixels = std::string(100uz * 100uz * 4uz, '\xFF');
    auto const encoded = core::base64::encode(pixels);
    REQUIRE(encoded.size() > static_cast<std::size_t>(1024 * 50));

    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=100,v=100,i=7;{}\033\\", encoded));

    // Nothing was placed, and nothing claimed success.
    CHECK_FALSE(screen.at(LineOffset(0), ColumnOffset(0)).imageFragment());
    CHECK(!mock.terminal.peekInput().contains("OK"));
}

TEST_CASE("KittyGraphics.a_chunked_transmission_of_the_same_size_still_works", "[kitty]")
{
    // The counterpart to the cap: chunking (m=1) is the supported way to send a payload this large,
    // and each chunk stays well under the bound. Without this, the cap above would read as "images
    // over 50 KiB are unsupported" rather than "send them the way the protocol says to".
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    auto const& screen = mock.terminal.primaryScreen();
    mock.terminal.setCellPixelSize(ImageSize { Width(2), Height(2) });

    auto pixels = std::string(100uz * 100uz * 4uz, '\xFF');
    auto const encoded = core::base64::encode(pixels);

    auto constexpr ChunkSize = size_t { 4096 };
    auto first = true;
    for (size_t offset = 0; offset < encoded.size(); offset += ChunkSize)
    {
        auto const chunk = encoded.substr(offset, ChunkSize);
        auto const more = offset + ChunkSize < encoded.size() ? 1 : 0;
        if (first)
        {
            mock.writeToScreen(std::format("\033_Ga=T,f=32,s=100,v=100,i=8,m={};{}\033\\", more, chunk));
            first = false;
        }
        else
            mock.writeToScreen(std::format("\033_Gm={};{}\033\\", more, chunk));
    }

    CHECK(screen.at(LineOffset(0), ColumnOffset(0)).imageFragment());
}

// {{{ cursor movement
// The spec: "After placing an image on the screen the cursor must be moved to the right by the number of
// cols in the image placement rectangle and down by the number of rows" -- kitty's own graphics.c moves
// it down by rows - 1, so it ends on the image's last row, immediately right of it. `C=1` opts out.

TEST_CASE("KittyGraphics.cursor.a_placement_advances_past_the_image", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("\033[1;3H\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=1;/wAAAP8A\033\\"sv);
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(3)).imageFragment());
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(0), .column = ColumnOffset(4) });
}

TEST_CASE("KittyGraphics.cursor.a_tall_placement_leaves_the_cursor_on_its_last_row", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("\033[2;3H\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=3;/wAAAP8A\033\\"sv);
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(3), .column = ColumnOffset(4) });
}

TEST_CASE("KittyGraphics.cursor.a_put_advances_like_a_transmit_and_display", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("\033_Ga=t,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033[3;2H\033_Ga=p,i=7,c=3,r=2\033\\"sv);
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(3), .column = ColumnOffset(4) });
}

TEST_CASE("KittyGraphics.cursor.C_1_keeps_the_cursor_where_it_was", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("\033[2;3H\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=3,C=1;/wAAAP8A\033\\"sv);
    CHECK(mock.terminal.primaryScreen().at(LineOffset(1), ColumnOffset(2)).imageFragment());
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(1), .column = ColumnOffset(2) });
}

TEST_CASE("KittyGraphics.cursor.text_after_a_placement_continues_beside_it", "[kitty]")
{
    // What the advance is for: `icat`-style output writes the caption right after the image.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("AB\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=1;/wAAAP8A\033\\CD"sv);
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(2)).imageFragment());
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(4)).toUtf8() == "C");
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(5)).toUtf8() == "D");
}

TEST_CASE("KittyGraphics.cursor.reaching_the_right_edge_wraps_once", "[kitty]")
{
    // Leaving the screen is implementation-defined; Contour does what kitty's screen.c and ghostty do:
    // a cursor pushed to or past the right edge moves to the first column of the next line.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("\033[2;9H\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=2;/wAAAP8A\033\\"sv);
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) });
}

TEST_CASE("KittyGraphics.cursor.wrapping_off_the_bottom_margin_scrolls", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033[4;7H\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=1;/wAAAP8A\033\\"sv);
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(3), .column = ColumnOffset(0) });
    // The image scrolled up with the page, as kitty scrolls for the wrapped cursor.
    CHECK(mock.terminal.primaryScreen().at(LineOffset(2), ColumnOffset(6)).imageFragment());
    CHECK_FALSE(mock.terminal.primaryScreen().at(LineOffset(3), ColumnOffset(6)).imageFragment());
}

TEST_CASE("KittyGraphics.cursor.without_autowrap_the_right_edge_clamps", "[kitty]")
{
    // DECAWM reset: a cursor never wraps, so it stops at the last column -- no wrap row, no scroll.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033[?7l\033[4;1H\033_Ga=T,f=24,i=7,s=2,v=1,c=8,r=1;/wAAAP8A\033\\"sv);
    auto const& screen = mock.terminal.primaryScreen();
    CHECK(screen.cursor().position == CellLocation { .line = LineOffset(3), .column = ColumnOffset(7) });
    CHECK_FALSE(screen.cursor().wrapPending);
    CHECK(screen.at(LineOffset(3), ColumnOffset(0)).imageFragment());
    CHECK(screen.at(LineOffset(3), ColumnOffset(7)).imageFragment());
}

TEST_CASE("KittyGraphics.cursor.the_right_margin_is_the_edge_inside_DECLRMM", "[kitty]")
{
    // Left/right margins 3..6 (1-based): the image reaching the right margin wraps to the LEFT margin.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("\033[?69h\033[3;6s\033[2;5H\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=1;/wAAAP8A\033\\"sv);
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(2), .column = ColumnOffset(2) });
}

TEST_CASE("KittyGraphics.cursor.without_autowrap_DECLRMM_clamps_to_the_right_margin", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen(
        "\033[?7l\033[?69h\033[3;6s\033[2;5H\033_Ga=T,f=24,i=7,s=2,v=1,c=3,r=1;/wAAAP8A\033\\"sv);
    CHECK(mock.terminal.primaryScreen().cursor().position
          == CellLocation { .line = LineOffset(1), .column = ColumnOffset(5) });
}

TEST_CASE("KittyGraphics.cursor.a_pending_wrap_start_wraps_once", "[kitty]")
{
    // Text filled the line, leaving the cursor on the last column with a wrap pending. The image is
    // placed there, and pushing past the edge wraps once -- the pending wrap is consumed, not doubled.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("ABCDEFGH"sv);
    REQUIRE(mock.terminal.primaryScreen().cursor().wrapPending);
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=1;/wAAAP8A\033\\"sv);
    auto const& screen = mock.terminal.primaryScreen();
    CHECK(screen.at(LineOffset(0), ColumnOffset(7)).imageFragment());
    CHECK(screen.cursor().position == CellLocation { .line = LineOffset(1), .column = ColumnOffset(0) });
    CHECK_FALSE(screen.cursor().wrapPending);
}

TEST_CASE("KittyGraphics.cursor.moving_down_scrolls_inside_a_DECSTBM_region", "[kitty]")
{
    // Region rows 1..3 (1-based), cursor on its second row, a three-row image: the cursor walks down by
    // index, so reaching the bottom margin scrolls the region -- as kitty and ghostty do -- and the
    // row below the region is left alone.
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    mock.writeToScreen("\033[1;3r\033[2;1H\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=3;/wAAAP8A\033\\"sv);
    auto const& screen = mock.terminal.primaryScreen();
    CHECK(screen.cursor().position == CellLocation { .line = LineOffset(2), .column = ColumnOffset(2) });
    auto const rowAt = [&](int line) -> int {
        auto const fragment = screen.at(LineOffset(line), ColumnOffset(0)).imageFragment();
        return fragment ? unbox(fragment->offset().line) : -1;
    };
    CHECK(rowAt(0) == 0);  // scrolled up from line 1
    CHECK(rowAt(1) == 1);  // scrolled up from line 2
    CHECK(rowAt(2) == -1); // the region's freshly scrolled-in row
    CHECK(rowAt(3) == 2);  // outside the region: never scrolled
}
// }}}

// {{{ animation
TEST_CASE("KittyGraphics.parse.frame_transmit_reinterprets_cell_keys", "[kitty]")
{
    // In an animation command the protocol reuses three keys that mean something else everywhere
    // else: `c` is the frame to copy from, `r` the frame to edit, and `z` the gap in milliseconds --
    // not columns, rows and z-index. Decoding them as cell geometry would silently place a frame at
    // the wrong index and give it no delay.
    auto const command = parseCommand("a=f,i=7,c=2,r=3,z=75,x=4,y=5,s=6,v=8;QUJD"sv);
    REQUIRE(command.has_value());
    CHECK(command->action == Action::Frame);
    CHECK(command->imageId == 7);
    CHECK(command->baseFrame == 2);
    CHECK(command->targetFrame == 3);
    CHECK(command->frameGapMilliseconds == 75);
    // The rectangle this frame's pixels cover, which keeps its ordinary meaning.
    CHECK(command->sourceX == 4);
    CHECK(command->sourceY == 5);
    CHECK(command->pixelWidth == 6);
    CHECK(command->pixelHeight == 8);
    // The cell-geometry fields must be left alone, so nothing downstream reads a frame number as a
    // column count.
    CHECK(command->columns == 0);
    CHECK(command->rows == 0);
    CHECK(command->zIndex == 0);
}

TEST_CASE("KittyGraphics.parse.animate_reinterprets_size_keys", "[kitty]")
{
    // Animation control overloads two more keys than frame transmission does: `s` is the play state
    // and `v` the loop count, where everywhere else they are the pixel width and height. Reading
    // them as dimensions would treat "loop three times" as a 3-pixel-tall image.
    auto const command = parseCommand("a=a,i=7,s=3,v=4,c=2,r=5,z=60"sv);
    REQUIRE(command.has_value());
    CHECK(command->action == Action::Animate);
    CHECK(command->imageId == 7);
    CHECK(command->animationState == AnimationState::Loop);
    CHECK(command->loopCount == 4);
    CHECK(command->currentFrame == 2);
    CHECK(command->targetFrame == 5);
    CHECK(command->frameGapMilliseconds == 60);
    // Nothing may leak into the geometry fields.
    CHECK(command->pixelWidth == 0);
    CHECK(command->pixelHeight == 0);
    CHECK(command->columns == 0);
    CHECK(command->rows == 0);
    CHECK(command->zIndex == 0);
}

TEST_CASE("KittyGraphics.parse.animate_states", "[kitty]")
{
    // s=1 stop, s=2 run but wait for more frames at the end, s=3 run and loop. Absent means unset,
    // which must not be confused with "stop".
    CHECK(parseCommand("a=a,i=1,s=1"sv)->animationState == AnimationState::Stop);
    CHECK(parseCommand("a=a,i=1,s=2"sv)->animationState == AnimationState::RunAwaitingFrames);
    CHECK(parseCommand("a=a,i=1,s=3"sv)->animationState == AnimationState::Loop);
    CHECK(parseCommand("a=a,i=1"sv)->animationState == AnimationState::Unset);
}

namespace
{
/// The pixels a placed image is actually showing, read back out of the grid.
[[nodiscard]] std::vector<uint8_t> placedPixels(MockTerm<vtpty::MockPty>& mock)
{
    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    if (!fragment)
        return {};
    auto const& data = fragment->rasterizedImage().image().data();
    return { data.begin(), data.end() };
}

/// Sends @p sequence and returns the reply it alone produced.
[[nodiscard]] std::string replyTo(MockTerm<vtpty::MockPty>& mock, std::string_view sequence)
{
    mock.discardPendingReplies();
    mock.writeToScreen(sequence);
    return mock.terminal.peekInput();
}
} // namespace

TEST_CASE("KittyGraphics.animation.frame_transmit_is_accepted", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // A 2x1 RGB image: red, green.
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    (void) mock.terminal.peekInput();
    mock.terminal.flushInput();

    // Frame 2, copied from frame 1, overwriting the second pixel with blue. Asserting on OK rather
    // than merely "not ENOTSUP", which would also pass for EINVAL, ENOENT and ENOSPC.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("OK"));
}

TEST_CASE("KittyGraphics.animation.a_frame_is_composited_over_its_base", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    REQUIRE(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 255, 0 });

    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    // Building a frame must not disturb what is on screen; only a=a changes that.
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 255, 0 });

    // Frame 2 is frame 1 with the second pixel replaced: red, blue.
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 0, 255 });
}

TEST_CASE("KittyGraphics.animation.alpha_blends_over_the_base_frame", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // RGBA this time, because the blend path only runs for four-byte pixels.
    mock.writeToScreen("\033_Ga=T,f=32,i=7,s=2,v=1;/wAA/wD/AP8=\033\\"sv);

    // Half-transparent blue over the opaque green pixel, with no X=1, so it must be composited
    // rather than copied. Read through a signed char an opaque alpha becomes 4294967295 and the
    // arithmetic collapses, so this is the case that catches it.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,x=1,y=0,s=1,v=1;AAD/gA==\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 255, 0, 127, 128, 255 });
}

TEST_CASE("KittyGraphics.animation.self_running_playback_is_accepted", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);

    // Frames now advance on the terminal's own clock (@see KittyGraphics.playback.*), so both playing
    // states are honoured -- and answered as such, not with the ENOTSUP they used to get.
    CHECK(replyTo(mock, "\033_Ga=a,i=7,s=3\033\\"sv) == "\033_Gi=7;OK\033\\");
    CHECK(replyTo(mock, "\033_Ga=a,i=7,s=2\033\\"sv) == "\033_Gi=7;OK\033\\");
}

TEST_CASE("KittyGraphics.animation.control_is_legal_before_any_frame", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // Every transmitted image already has a root frame -- the protocol has no command to send frame 1
    // separately -- so animation control is legal before any a=f. The spec relies on exactly this to
    // set the root frame's gap. Answering ENOENT tells a client its image is gone and provokes the
    // full re-transmission this feature exists to avoid.
    mock.writeToScreen("\033_Ga=a,i=7,c=1\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("OK"));
    mock.terminal.flushInput();

    // An id that was never transmitted is the real ENOENT.
    mock.writeToScreen("\033_Ga=a,i=99,c=1\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
}

TEST_CASE("KittyGraphics.animation.retransmitting_an_id_drops_its_frames", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);

    // A new image under the same id: the frames composited from the old one describe nothing, and
    // their geometry need not even match.
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=1,v=1;AP8A\033\\"sv);
    mock.terminal.flushInput();

    // Only the new image's root frame remains, so frame 2 is gone rather than stale.
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
}

TEST_CASE("KittyGraphics.animation.a_frame_may_omit_its_dimensions", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // Absent s=/v= means the whole image, which is the natural spelling for a frame that replaces
    // everything -- refusing it would reject the common case.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1;AAD//wAA\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("OK"));
}

TEST_CASE("KittyGraphics.animation.a_faint_source_does_not_wrap_the_blend", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // A nearly-white, mostly-transparent base pixel.
    mock.writeToScreen("\033_Ga=T,f=32,i=7,s=1,v=1;////Sg==\033\\"sv);

    // A barely-there source over it. The two roundings in the blend disagree, so the quotient reaches
    // 256 and wraps a near-white result to black -- and a low source alpha is exactly what a fade-in
    // animation is made of, so this is the common case rather than a corner.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,x=0,y=0,s=1,v=1;2traAQ==\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    auto const pixels = placedPixels(mock);
    REQUIRE(pixels.size() == 4);
    CHECK(pixels[0] >= 250); // white, not black
}

TEST_CASE("KittyGraphics.animation.a_rejected_frame_creates_no_animation", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);

    // A rectangle far larger than the image: rejected. Seeding frame 1 before that check would leave
    // an animation entry behind, and "has frames" is exactly how a=a decides one exists.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,x=0,y=0,s=99,v=99;AAD/\033\\"sv);
    mock.terminal.flushInput();

    // The root frame is always there, but the rejected frame 2 must not be.
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
}

TEST_CASE("KittyGraphics.animation.naming_a_frame_that_does_not_exist_is_refused", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.terminal.flushInput();

    // Answering OK would leave the client believing frame 5 is on screen.
    mock.writeToScreen("\033_Ga=a,i=7,c=5\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
    mock.terminal.flushInput();

    // Likewise for a frame composited over a base that does not exist.
    mock.writeToScreen("\033_Ga=f,i=7,r=3,c=99,X=1,x=0,y=0,s=1,v=1;AAD/\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
}

TEST_CASE("KittyGraphics.animation.a_frame_without_r_appends", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);

    // The protocol's normal streaming spelling: no r=, meaning "create a new frame". It must APPEND,
    // not overwrite the base -- and it must behave the same whether the image was displayed (a=T,
    // which records a placement) or merely stored (a=t, which does not).
    mock.writeToScreen("\033_Ga=f,i=7,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.terminal.flushInput();

    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("OK"));
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 0, 255 });
}

TEST_CASE("KittyGraphics.animation.deleting_frames_keeps_other_images", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=t,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=t,f=24,i=9,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.terminal.flushInput();

    // d=F means "delete image 7's animation frames". Before frames existed it fell into the
    // catch-all that cleared every image in the terminal -- and still answered OK.
    mock.writeToScreen("\033_Ga=d,d=F,i=7\033\\"sv);
    mock.terminal.flushInput();
    mock.writeToScreen("\033_Ga=p,i=9\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("OK"));
}

TEST_CASE("KittyGraphics.animation.an_unimplemented_delete_target_destroys_nothing", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=t,f=24,i=9,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // A positional target Contour does not implement must do nothing, not clear the terminal -- and
    // say so, rather than answer OK for a delete that did not happen. But only to a command that
    // identified itself: one naming no image reads no reply, and an unsolicited one is typed keys.
    CHECK(replyTo(mock, "\033_Ga=d,d=Z\033\\"sv).empty());
    CHECK(replyTo(mock, "\033_Ga=d,d=Z,i=9\033\\"sv).contains("ENOTSUP"));
    CHECK(replyTo(mock, "\033_Ga=p,i=9\033\\"sv).contains("OK"));
}

TEST_CASE("KittyGraphics.animation.editing_the_live_frame_shows", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    REQUIRE(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 0, 255 });

    // The lowest-bandwidth loop the protocol offers: keep editing the frame already on screen, never
    // send a=a again. Without a redraw the animation freezes and every later delta is invisible.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,X=1,x=0,y=0,s=1,v=1;AP8A\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 0, 255, 0, 0, 0, 255 });
}

TEST_CASE("KittyGraphics.animation.a_deleted_placement_leaves_no_frame_on_screen", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    REQUIRE_FALSE(placedPixels(mock).empty());

    // The cells now hold a FRAME, not the base image, so a delete matching only the base would leave
    // the image visible -- and for d=I would strand the frame in the grid, outside every quota.
    mock.writeToScreen("\033_Ga=d,d=i,i=7\033\\"sv);
    CHECK(placedPixels(mock).empty());
}

TEST_CASE("KittyGraphics.animation.a_frame_number_beyond_the_next_is_refused", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=t,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // r=128 on a one-frame image would otherwise fabricate 126 aliases of frame 1 in a single escape,
    // exhausting MaxAnimationFrames while the byte quota sees nothing, and then report frames the
    // client never sent as displayable.
    mock.writeToScreen("\033_Ga=f,i=7,r=128,X=1,x=0,y=0,s=1,v=1;AAD/\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOENT"));
}

TEST_CASE("KittyGraphics.animation.editing_frame_one_is_seen_by_later_reads", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);

    // Two edits of the root frame, the spec's multi-block idiom. If a read of frame 1 short-circuits
    // to the stored image, the second edit re-bases on the untouched original and the first is lost.
    mock.writeToScreen("\033_Ga=f,i=7,r=1,X=1,x=0,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=1,X=1,x=1,y=0,s=1,v=1;AP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=1\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 0, 0, 255, 0, 255, 0 });
}

TEST_CASE("KittyGraphics.animation.repeating_c_does_not_discard_earlier_blocks", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);

    // A frame composed from two rectangles, with c= repeated on both because they come from one
    // template. `c` is the source only when CREATING a frame; once r= names an existing one, that
    // frame is the canvas -- preferring c would silently drop the first block.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=0,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 0, 0, 255, 0, 255, 0 });
}

TEST_CASE("KittyGraphics.animation.a_frame_in_another_format_is_refused", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=t,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // f= is a claim about the payload's layout. Four-byte pixels composited as three-byte ones, or a
    // PNG file's header blended in as pixels, is the reinterpretation the medium and compression
    // checks already refuse.
    mock.writeToScreen("\033_Ga=f,f=32,i=7,r=2,c=1,X=1,x=0,y=0,s=1,v=1;AAD/AA==\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("EINVAL"));
}

TEST_CASE("KittyGraphics.animation.selecting_a_frame_together_with_playback_shows_it", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);

    // The keys are independent and nothing makes a client split them: starting playback must not
    // discard the frame selection that came with it.
    mock.writeToScreen("\033_Ga=a,i=7,c=2,s=3\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 0, 255 });
}

TEST_CASE("KittyGraphics.animation.a_frame_advance_follows_the_cells_when_the_screen_scrolls", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);

    // Scroll the image down a row. A frame advance must find it where it now IS -- tracking a
    // coordinate alongside meant every path that moves cells had to remember to keep it in step.
    mock.writeToScreen("\033[H\033[1L"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);

    auto const& cell = mock.terminal.primaryScreen().at(LineOffset(1), ColumnOffset(0));
    auto const fragment = cell.imageFragment();
    REQUIRE(fragment != nullptr);
    auto const& data = fragment->rasterizedImage().image().data();
    CHECK(std::vector<uint8_t>(data.begin(), data.end()) == std::vector<uint8_t> { 255, 0, 0, 0, 0, 255 });
    // ... and nothing was stamped at the row it used to occupy.
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).imageFragment() == nullptr);
}

TEST_CASE("KittyGraphics.animation.an_erased_image_is_not_resurrected", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.writeToScreen("\033[2J"sv);
    REQUIRE(placedPixels(mock).empty());

    // The cells were cleared, so there is nothing to advance. A recorded coordinate would have
    // repainted the image over whatever occupies those cells now.
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(placedPixels(mock).empty());
}

TEST_CASE("KittyGraphics.animation.deleting_frames_keeps_the_picture_on_screen", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    REQUIRE(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 0, 255 });

    // d=f reclaims frame memory and keeps the image displayed -- clearing the cells would force the
    // re-transmission the verb exists to avoid. What was showing was a frame, so the base goes back.
    mock.writeToScreen("\033_Ga=d,d=f,i=7\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 255, 0 });
}

TEST_CASE("KittyGraphics.animation.a_delete_by_id_without_an_id_destroys_nothing", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    REQUIRE_FALSE(placedPixels(mock).empty());

    // `d=I` names id 0, which no image can have, so it must delete nothing -- not fall through to
    // the branch that clears every image in the terminal.
    mock.writeToScreen("\033_Ga=d,d=I\033\\"sv);
    CHECK_FALSE(placedPixels(mock).empty());
}

TEST_CASE("KittyGraphics.animation.a_frame_advance_keeps_the_cursor_out_of_it", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);

    // A hyperlink opened after the placement must not be stamped across the image's cells by a frame
    // advance: the redraw belongs to the placement, not to whatever the cursor carries now.
    mock.writeToScreen("\033]8;;http://example.com\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(mock.terminal.primaryScreen().at(LineOffset(0), ColumnOffset(0)).hyperlink() == HyperlinkId(0));
}

TEST_CASE("KittyGraphics.animation.each_placement_keeps_its_geometry_across_a_frame_advance", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(6), ColumnCount(10) } };
    // The same image placed twice at different sizes: a 2x1 cell placement and a 6x2 one below it.
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1,c=2,r=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033[4;1H\033_Ga=p,i=7,c=6,r=2\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);

    auto const spanAt = [&](int line) -> std::pair<int, int> {
        auto const fragment =
            mock.terminal.primaryScreen().at(LineOffset(line), ColumnOffset(0)).imageFragment();
        if (!fragment)
            return {};
        auto const span = fragment->rasterizedImage().cellSpan();
        return { unbox(span.lines), unbox(span.columns) };
    };
    REQUIRE(spanAt(0) == std::pair { 1, 2 });
    REQUIRE(spanAt(3) == std::pair { 2, 6 });

    // Building the new frame's raster once and sharing it would give the second placement the
    // first one's span, so it would be drawn at the wrong size.
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(spanAt(0) == std::pair { 1, 2 });
    CHECK(spanAt(3) == std::pair { 2, 6 });
}

TEST_CASE("KittyGraphics.storage.the_quota_is_a_terminal_setting", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.settings().kittyImageStorageQuota = 6; // Room for exactly one 2x1 RGB image.

    CHECK(replyTo(mock, "\033_Ga=t,f=24,i=1,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
    CHECK(replyTo(mock, "\033_Ga=t,f=24,i=2,s=2,v=1;/wAAAP8A\033\\"sv).contains("ENOSPC"));
    // Replacing the image under the same id is billed net of the bytes it frees.
    CHECK(replyTo(mock, "\033_Ga=t,f=24,i=1,s=2,v=1;AP8A/wAA\033\\"sv).contains("OK"));
}

TEST_CASE("KittyGraphics.storage.a_refused_retransmission_keeps_the_frames", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // Three 2x1 RGB images' worth: another image, image 1, and one frame of image 1.
    mock.terminal.settings().kittyImageStorageQuota = 18;
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=2,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=1,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
    REQUIRE(replyTo(mock, "\033_Ga=f,i=1,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv).contains("OK"));

    // 15 bytes do not fit beside image 2's 6 even with image 1 and its frame discounted, so this is
    // refused -- and a refusal must leave image 1 exactly as it was, frames included.
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=1,s=5,v=1;AAAAAAAAAAAAAAAAAAAA\033\\"sv).contains("ENOSPC"));
    CHECK(replyTo(mock, "\033_Ga=a,i=1,c=2\033\\"sv).contains("OK"));
}

TEST_CASE("KittyGraphics.storage.a_retransmission_is_not_billed_for_the_frames_it_replaces", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.settings().kittyImageStorageQuota = 18;
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=2,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=1,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
    REQUIRE(replyTo(mock, "\033_Ga=f,i=1,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv).contains("OK"));

    // 12 bytes fit beside image 2 once image 1 and its frame -- both about to be freed -- are not
    // counted against them.
    CHECK(replyTo(mock, "\033_Ga=t,f=24,i=1,s=4,v=1;AAAAAAAAAAAAAAAA\033\\"sv).contains("OK"));
}

TEST_CASE("KittyGraphics.storage.editing_the_root_frame_is_billed_once", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.settings().kittyImageStorageQuota = 6; // Exactly one 2x1 RGB image.
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=1,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));

    // Editing frame 1 replaces the stored image rather than adding to it, so it must fit.
    CHECK(replyTo(mock, "\033_Ga=f,i=1,r=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv).contains("OK"));
    // A second frame, by contrast, really is a second copy.
    CHECK(replyTo(mock, "\033_Ga=f,i=1,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv).contains("ENOSPC"));
}

TEST_CASE("KittyGraphics.animation.the_frame_count_is_capped", "[kitty]")
{
    using kitty_graphics::MaxAnimationFrames;
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=1,s=1,v=1;AAAA\033\\"sv).contains("OK"));

    // Frame 1 is the image itself, so MaxAnimationFrames - 1 frames can be appended to it.
    for ([[maybe_unused]] auto const frame: std::views::iota(1u, MaxAnimationFrames))
        REQUIRE(replyTo(mock, "\033_Ga=f,i=1,s=1,v=1;AAAA\033\\"sv).contains("OK"));
    CHECK(replyTo(mock, "\033_Ga=f,i=1,s=1,v=1;AAAA\033\\"sv).contains("ENOSPC"));
}

TEST_CASE("KittyGraphics.animation.a_new_frame_without_c_starts_from_the_background", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=32,i=7,s=2,v=1;/wAA/wD/AP8=\033\\"sv);

    // No `c=`, so the canvas is `Y=`, an RGBA literal: 65535 is opaque blue. Only the first pixel is
    // transmitted (opaque red); the second must be the background, not the base image's green.
    mock.writeToScreen("\033_Ga=f,i=7,Y=65535,X=1,x=0,y=0,s=1,v=1;/wAA/w==\033\\"sv);
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 255, 0, 0, 255, 255 });
}

TEST_CASE("KittyGraphics.storage.freeing_an_image_frees_its_frames", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.settings().kittyImageStorageQuota = 12; // Two 2x1 RGB images' worth.
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=1,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
    REQUIRE(replyTo(mock, "\033_Ga=f,i=1,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv).contains("OK"));
    REQUIRE(replyTo(mock, "\033_Ga=t,f=24,i=2,s=2,v=1;/wAAAP8A\033\\"sv).contains("ENOSPC"));

    // `d=I` frees the image's data, and its frames are copies of it: leaving them behind would keep
    // the quota full with memory no verb can reach any more.
    mock.writeToScreen("\033_Ga=d,d=I,i=1\033\\"sv);
    CHECK(replyTo(mock, "\033_Ga=t,f=24,i=2,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
    CHECK(replyTo(mock, "\033_Ga=t,f=24,i=3,s=2,v=1;/wAAAP8A\033\\"sv).contains("OK"));
}
// }}}

namespace
{
/// Deflates @p input into a zlib stream and base64-encodes it, as an application sending `o=z` would.
[[nodiscard]] std::string zlibBase64(std::string_view input)
{
    auto const source = std::vector<uint8_t>(input.begin(), input.end());
    auto out = std::vector<uint8_t>(compressBound(static_cast<uLong>(source.size())));
    auto outSize = static_cast<uLongf>(out.size());
    auto const status =
        compress2(out.data(), &outSize, source.data(), static_cast<uLong>(source.size()), Z_BEST_COMPRESSION);
    REQUIRE(status == Z_OK);
    out.resize(outSize);
    return core::base64::encode(out.begin(), out.end());
}
} // namespace

TEST_CASE("KittyGraphics.compression.transmit_with_o_z_displays_the_image", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // A 2x2 RGBA image.
    auto const pixels = "\xFF\x00\x00\xFF\x00\xFF\x00\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF"s;
    CHECK(replyTo(mock, std::format("\033_Ga=T,f=32,s=2,v=2,i=3,o=z;{}\033\\", zlibBase64(pixels)))
          == "\033_Gi=3;OK\033\\");
    CHECK(placedPixels(mock) == std::vector<uint8_t> { pixels.begin(), pixels.end() });
}

TEST_CASE("KittyGraphics.compression.frame_with_o_z_is_composited", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // Frame 2: the second pixel replaced by blue.
    auto const blue = "\x00\x00\xFF"s;
    CHECK(
        replyTo(mock, std::format("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1,o=z;{}\033\\", zlibBase64(blue)))
            .contains("OK"));
    mock.writeToScreen("\033_Ga=a,i=7,c=2\033\\"sv);
    CHECK(placedPixels(mock) == std::vector<uint8_t> { 255, 0, 0, 0, 0, 255 });
}

TEST_CASE("KittyGraphics.compression.raw_bomb_answers_EINVAL", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // Declares 1x1 RGBA (4 bytes) but inflates to 4 MiB: refused at the declared size, not inflated.
    auto const zeros = std::string(size_t { 4 } << 20, '\0');
    CHECK(replyTo(mock, std::format("\033_Ga=T,f=32,s=1,v=1,i=3,o=z;{}\033\\", zlibBase64(zeros)))
          == "\033_Gi=3;EINVAL:payload size does not match dimensions\033\\");
}

TEST_CASE("KittyGraphics.compression.png_bomb_answers_ENOSPC", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.terminal.settings().kittyImageStorageQuota = 4096;
    auto const zeros = std::string(size_t { 4 } << 20, '\0');
    CHECK(replyTo(mock, std::format("\033_Ga=T,f=100,i=3,o=z;{}\033\\", zlibBase64(zeros)))
          == "\033_Gi=3;ENOSPC:image storage quota exceeded\033\\");
}

TEST_CASE("KittyGraphics.compression.corrupt_answers_EINVAL", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // Valid base64, but not a zlib stream.
    CHECK(replyTo(mock, "\033_Ga=T,f=32,s=1,v=1,i=3,o=z;AAAAAAAAAA==\033\\"sv)
          == "\033_Gi=3;EINVAL:corrupt compressed payload\033\\");
}

TEST_CASE("KittyGraphics.compression.dimensions_that_overflow_are_refused_before_inflating", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // The payload is a valid stream for one pixel: it is the declared size that is the problem, and a
    // refusal that came from inflating (or from the size check after it) would not name that.
    auto const pixel = zlibBase64("\x01\x02\x03\x04"sv);

    // 2^32-1 squared times four wraps size_t.
    CHECK(replyTo(mock, std::format("\033_Ga=T,f=32,s=4294967295,v=4294967295,i=3,o=z;{}\033\\", pixel))
          == "\033_Gi=3;EINVAL:image dimensions too large\033\\");
    // 65536x65536 RGBA is exactly 16 GiB: representable, and far over the 128 MiB quota.
    CHECK(replyTo(mock, std::format("\033_Ga=T,f=32,s=65536,v=65536,i=3,o=z;{}\033\\", pixel))
          == "\033_Gi=3;EINVAL:image dimensions too large\033\\");
}

TEST_CASE("KittyGraphics.compression.frame_bomb_answers_EINVAL", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // One RGB pixel declared, 4 MiB inflated.
    auto const zeros = std::string(size_t { 4 } << 20, '\0');
    CHECK(replyTo(mock,
                  std::format("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1,o=z;{}\033\\", zlibBase64(zeros)))
          == "\033_Gi=7;EINVAL:payload size does not match dimensions\033\\");
}

TEST_CASE("KittyGraphics.compression.chunked_transmission_is_inflated_once_reassembled", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    auto const pixels = "\xFF\x00\x00\xFF\x00\xFF\x00\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF"s;
    auto const encoded = zlibBase64(pixels);
    // Split on a 4-character boundary so each chunk is valid base64 on its own.
    auto const split = (encoded.size() / 2) / 4 * 4;
    REQUIRE(split > 0);

    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=3,o=z,m=1;{}\033\\", encoded.substr(0, split)));
    mock.terminal.flushInput();
    CHECK(replyTo(mock, std::format("\033_Gm=0;{}\033\\", encoded.substr(split))).contains("OK"));
    CHECK(placedPixels(mock) == std::vector<uint8_t> { pixels.begin(), pixels.end() });
}

// {{{ playback
namespace
{
using namespace std::chrono_literals;

/// The single pixel of each frame placeThreeFrames() builds, as placedPixels() reads it back.
[[nodiscard]] std::vector<uint8_t> red()
{
    return { 255, 0, 0 };
}
[[nodiscard]] std::vector<uint8_t> green()
{
    return { 0, 255, 0 };
}
[[nodiscard]] std::vector<uint8_t> blue()
{
    return { 0, 0, 255 };
}
[[nodiscard]] std::vector<uint8_t> white()
{
    return { 255, 255, 255 };
}

/// Places a one-pixel image under id 7 at the top left with three frames -- red, green, blue -- each shown
/// for the matching gap in milliseconds (`z=`: zero leaves the default, negative makes the frame gapless).
void placeThreeFrames(MockTerm<vtpty::MockPty>& mock, int first, int second, int third)
{
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=1,v=1;/wAA\033\\"sv);
    mock.writeToScreen(std::format("\033_Ga=a,i=7,r=1,z={}\033\\", first));
    mock.writeToScreen(std::format("\033_Ga=f,i=7,X=1,s=1,v=1,z={};AP8A\033\\", second));
    mock.writeToScreen(std::format("\033_Ga=f,i=7,X=1,s=1,v=1,z={};AAD/\033\\", third));
    mock.discardPendingReplies();
}

/// Moves the test's clock on by @p delta and lets the display tick at the new time, as it does each frame.
void advance(MockTerm<vtpty::MockPty>& mock, std::chrono::milliseconds delta)
{
    mock.steadyClock.advance(delta);
    mock.terminal.tick(mock.steadyClock.now());
}

/// The playback deadline the terminal published, relative to the test's clock.
[[nodiscard]] std::optional<std::chrono::steady_clock::duration> dueIn(MockTerm<vtpty::MockPty>& mock)
{
    auto const due = mock.terminal.kittyAnimationDue();
    if (!due)
        return std::nullopt;
    return *due - mock.steadyClock.now();
}
} // namespace

TEST_CASE("KittyGraphics.playback.s3_advances_frames_by_their_gaps", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    CHECK(replyTo(mock, "\033_Ga=a,i=7,s=3\033\\"sv) == "\033_Gi=7;OK\033\\");
    REQUIRE(placedPixels(mock) == red());

    // Each frame is shown for its own gap, measured from when it came up -- not a moment less.
    advance(mock, 9ms);
    CHECK(placedPixels(mock) == red());
    advance(mock, 1ms);
    CHECK(placedPixels(mock) == green());
    advance(mock, 19ms);
    CHECK(placedPixels(mock) == green());
    advance(mock, 1ms);
    CHECK(placedPixels(mock) == blue());
}

TEST_CASE("KittyGraphics.playback.s3_wraps_to_frame_1", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);

    advance(mock, 10ms);
    advance(mock, 20ms);
    REQUIRE(placedPixels(mock) == blue());
    advance(mock, 30ms);
    CHECK(placedPixels(mock) == red());
    advance(mock, 10ms);
    CHECK(placedPixels(mock) == green());
    CHECK(dueIn(mock) == 20ms);
}

TEST_CASE("KittyGraphics.playback.v_n_stops_after_n_minus_1_loops", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    // v=3: two loops, then rest on the last frame.
    mock.writeToScreen("\033_Ga=a,i=7,s=3,v=3\033\\"sv);

    advance(mock, 10ms);
    advance(mock, 20ms);
    REQUIRE(placedPixels(mock) == blue());
    advance(mock, 30ms);
    REQUIRE(placedPixels(mock) == red());
    advance(mock, 10ms);
    advance(mock, 20ms);
    REQUIRE(placedPixels(mock) == blue());

    advance(mock, 30ms);
    CHECK(placedPixels(mock) == blue());
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());
    advance(mock, 1000ms);
    CHECK(placedPixels(mock) == blue());
}

TEST_CASE("KittyGraphics.playback.s2_holds_at_last_frame_then_continues_on_new_frame", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    CHECK(replyTo(mock, "\033_Ga=a,i=7,s=2\033\\"sv) == "\033_Gi=7;OK\033\\");

    advance(mock, 10ms);
    advance(mock, 20ms);
    advance(mock, 30ms);
    // Out of frames: it waits on the last one, and nothing need wake the display for it meanwhile.
    CHECK(placedPixels(mock) == blue());
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());

    // A new frame ends the wait at once: the last one has had its gap already.
    CHECK(replyTo(mock, "\033_Ga=f,i=7,X=1,s=1,v=1,z=10;////\033\\"sv) == "\033_Gi=7;OK\033\\");
    CHECK(dueIn(mock) == 0ms);
    advance(mock, 0ms);
    CHECK(placedPixels(mock) == white());
    advance(mock, 10ms);
    CHECK(placedPixels(mock) == white());
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());
}

TEST_CASE("KittyGraphics.playback.s1_stops_on_current_frame", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    advance(mock, 10ms);
    REQUIRE(placedPixels(mock) == green());

    CHECK(replyTo(mock, "\033_Ga=a,i=7,s=1\033\\"sv) == "\033_Gi=7;OK\033\\");
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());
    advance(mock, 1000ms);
    CHECK(placedPixels(mock) == green());
}

TEST_CASE("KittyGraphics.playback.gapless_frames_are_skipped", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, -1, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);

    advance(mock, 10ms);
    CHECK(placedPixels(mock) == blue());
    advance(mock, 30ms);
    CHECK(placedPixels(mock) == red());
}

TEST_CASE("KittyGraphics.playback.zero_gap_uses_the_default", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    // The root frame's gap is never set, so it has the protocol's default.
    placeThreeFrames(mock, 0, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);

    auto const defaultGap = std::chrono::milliseconds(DefaultFrameGapMilliseconds);
    advance(mock, defaultGap - 1ms);
    CHECK(placedPixels(mock) == red());
    advance(mock, 1ms);
    CHECK(placedPixels(mock) == green());
}

TEST_CASE("KittyGraphics.playback.all_gapless_holds_without_a_deadline", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, -1, -1, -1);
    CHECK(replyTo(mock, "\033_Ga=a,i=7,s=3\033\\"sv) == "\033_Gi=7;OK\033\\");

    // Nothing to show for any time at all: a deadline would spin the display for no frame.
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());
    advance(mock, 1000ms);
    CHECK(placedPixels(mock) == red());
}

TEST_CASE("KittyGraphics.playback.explicit_frame_selection_works_while_playing", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    advance(mock, 10ms);
    REQUIRE(placedPixels(mock) == green());

    mock.writeToScreen("\033_Ga=a,i=7,c=3\033\\"sv);
    CHECK(placedPixels(mock) == blue());
    mock.writeToScreen("\033_Ga=a,i=7,c=1\033\\"sv);
    CHECK(placedPixels(mock) == red());

    // ... and playback goes on from the frame selected, for that frame's whole gap.
    CHECK(dueIn(mock) == 10ms);
    advance(mock, 10ms);
    CHECK(placedPixels(mock) == green());
}

TEST_CASE("KittyGraphics.playback.deleted_image_drops_deadline", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    REQUIRE(mock.terminal.kittyAnimationDue().has_value());

    mock.writeToScreen("\033_Ga=d,d=I,i=7\033\\"sv);
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());
    CHECK_FALSE(mock.terminal.nextRender().has_value());
}

TEST_CASE("KittyGraphics.playback.scrolled_off_placement_holds_no_deadline", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);

    // Off the page and into the history: nothing on display to advance, so nothing to wake up for.
    mock.writeToScreen("\033[4;1H\n\n\n\n"sv);
    REQUIRE(placedPixels(mock).empty());
    advance(mock, 10ms);
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());

    // A placement on the page plays again.
    mock.writeToScreen("\033_Ga=p,i=7\033\\"sv);
    CHECK(mock.terminal.kittyAnimationDue().has_value());
}

TEST_CASE("KittyGraphics.playback.nextRender_reports_time_to_next_frame", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);

    CHECK(mock.terminal.nextRender() == 10ms);
    // Measured against the steady clock as well: in the daemon, nothing moves the terminal's own clock.
    mock.steadyClock.advance(10ms);
    CHECK(mock.terminal.nextRender() == 0ms);
}

TEST_CASE("KittyGraphics.playback.alternate_screen_switch_pauses_and_resumes", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);

    // The primary screen's animation is not on display behind the alternate screen.
    mock.writeToScreen("\033[?1049h"sv);
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());
    advance(mock, 10ms);
    CHECK(placedPixels(mock) == red());

    mock.writeToScreen("\033[?1049l"sv);
    CHECK(mock.terminal.kittyAnimationDue().has_value());
    advance(mock, 0ms);
    CHECK(placedPixels(mock) == green());
}

TEST_CASE("KittyGraphics.playback.RIS_drops_deadline", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    REQUIRE(mock.terminal.kittyAnimationDue().has_value());

    mock.writeToScreen("\033c"sv);
    CHECK_FALSE(mock.terminal.kittyAnimationDue().has_value());
}

namespace
{
/// Counts what the terminal tells its display, for the tests that assert on the notifications themselves.
struct CountingTerm final: MockTerm<vtpty::MockPty>
{
    using MockTerm<vtpty::MockPty>::MockTerm;
    int screenUpdates = 0;
    int renderBufferUpdates = 0;
    void screenUpdated() override { ++screenUpdates; }
    void renderBufferUpdated() override { ++renderBufferUpdates; }
};
} // namespace

TEST_CASE("KittyGraphics.playback.tick_raises_screenUpdated_when_a_frame_advanced", "[kitty]")
{
    // The daemon hears of a frame through screenUpdated() alone: it never asks nextRender().
    auto mock = CountingTerm { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    mock.screenUpdates = 0;

    advance(mock, 5ms);
    CHECK(mock.screenUpdates == 0);
    advance(mock, 5ms);
    CHECK(mock.screenUpdates == 1);
}

TEST_CASE("KittyGraphics.playback.a_deadline_is_announced_only_when_it_moves", "[kitty]")
{
    // An idle display re-reads nextRender() when told to. Not telling it would leave it asleep past the
    // first frame; telling it on every command would wake it for nothing.
    auto mock = CountingTerm { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 10, 20, 30);

    auto const before = mock.renderBufferUpdates;
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    auto const afterStart = mock.renderBufferUpdates;
    CHECK(afterStart > before);

    // The same deadline again: nothing for the display to re-read.
    mock.writeToScreen("\033_Ga=a,i=7,v=1\033\\"sv);
    CHECK(mock.renderBufferUpdates == afterStart);
}

TEST_CASE("KittyGraphics.playback.tick_and_parser_do_not_race", "[kitty][threading]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    placeThreeFrames(mock, 1, 1, 1);
    // Playing first: tick() takes the state lock only while a frame is due, so a stopped animation would
    // race nothing.
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    REQUIRE(mock.terminal.kittyAnimationDue().has_value());

    auto stop = std::atomic<bool> { false };
    auto ticker = std::thread { [&]() {
        while (!stop.load(std::memory_order_acquire))
            advance(mock, 1ms);
    } };

    for (auto const round: std::views::iota(0, 200))
    {
        mock.writeToScreen("\033_Ga=f,i=7,r=2,X=1,s=1,v=1;////\033\\"sv);
        mock.writeToScreen(std::format("\033_Ga=a,i=7,c={}\033\\", (round % 3) + 1));
        mock.discardPendingReplies();
    }

    stop.store(true, std::memory_order_release);
    ticker.join();
    CHECK(mock.terminal.kittyAnimationDue().has_value());
}
// }}}
