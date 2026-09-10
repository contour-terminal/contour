// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty graphics protocol's control-data parser, and for the grid effect of the
// commands that place an image.

#include <vtbackend/graphics/KittyGraphics.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <crispy/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

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

    SECTION("compressed payload")
    {
        mock.writeToScreen("\033_Gi=32,s=1,v=1,a=q,t=d,o=z,f=24;AAAA\033\\"sv);
        CHECK(mock.terminal.peekInput().contains("ENOTSUP"));
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
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1;{}\033\\", crispy::base64::encode(pixels)));
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
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1;{}\033\\", crispy::base64::encode(pixels)));

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
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=2;{}\033\\", crispy::base64::encode(pixels)));
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
    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=2;{}\033\\", crispy::base64::encode(pixels)));

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

    auto const reply = std::string(mock.terminal.peekInput());
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
    auto const encoded = crispy::base64::encode(pixels);

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
    auto const encoded = crispy::base64::encode(pixels);

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
    auto const encoded = crispy::base64::encode(pixels);

    mock.writeToScreen(std::format("\033_Ga=T,f=32,s=2,v=2,i=1,c=99,r=99;{}\033\\", encoded));

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
    auto const encoded = crispy::base64::encode(pixels);
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
    auto const encoded = crispy::base64::encode(pixels);

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

TEST_CASE("KittyGraphics.animation.self_running_playback_is_declined", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);
    mock.terminal.flushInput();

    // Nothing advances frames on a timer, so answering OK would leave the client believing an
    // animation is running while the screen holds one frame forever.
    mock.writeToScreen("\033_Ga=a,i=7,s=3\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOTSUP"));
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

TEST_CASE("KittyGraphics.animation.a_compressed_frame_is_refused_as_unsupported", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.terminal.flushInput();

    // Not EINVAL for whatever size the compressed bytes happen to have: a client cannot tell "wrong
    // byte count" from "I do not implement this", and a payload that happened to match would be
    // composited as raw pixels.
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,o=z,x=0,y=0,s=1,v=1;eJw=\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("ENOTSUP"));
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

    // A positional target Contour does not implement must do nothing, not clear the terminal.
    mock.writeToScreen("\033_Ga=d,d=Z\033\\"sv);
    mock.terminal.flushInput();
    mock.writeToScreen("\033_Ga=p,i=9\033\\"sv);
    CHECK(mock.terminal.peekInput().contains("OK"));
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

TEST_CASE("KittyGraphics.animation.selecting_a_frame_survives_a_playback_refusal", "[kitty]")
{
    auto mock = MockTerm<vtpty::MockPty> { PageSize { LineCount(4), ColumnCount(8) } };
    mock.writeToScreen("\033_Ga=T,f=24,i=7,s=2,v=1;/wAAAP8A\033\\"sv);
    mock.writeToScreen("\033_Ga=f,i=7,r=2,c=1,X=1,x=1,y=0,s=1,v=1;AAD/\033\\"sv);

    // The keys are independent and nothing makes a client split them. Refusing the half we cannot do
    // must not discard the half we can.
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
// }}}
