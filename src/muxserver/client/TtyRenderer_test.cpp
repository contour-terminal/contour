// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <string>

#include <muxserver/client/TtyRenderer.h>

using muxserver::client::RemoteScreen;
using muxserver::client::renderViewport;
using muxserver::client::sgrFor;
namespace proto = muxserver::proto;

namespace
{

[[nodiscard]] uint32_t raw(vtbackend::Color color)
{
    return std::bit_cast<uint32_t>(color);
}

} // namespace

TEST_CASE("sgrFor selects renditions and colors from a reset", "[muxserver][attach]")
{
    auto cell = proto::WireCell {};
    CHECK(sgrFor(cell) == "\033[0m"); // default everything

    cell.flags = static_cast<uint32_t>(vtbackend::CellFlag::Bold)
                 | static_cast<uint32_t>(vtbackend::CellFlag::Underline);
    cell.foreground = raw(vtbackend::Color { vtbackend::IndexedColor(1) });
    cell.background = raw(vtbackend::Color { vtbackend::RGBColor { 16, 32, 48 } });
    CHECK(sgrFor(cell) == "\033[0;1;4;38;5;1;48;2;16;32;48m");
}

TEST_CASE("renderViewport repaints rows, skips wide continuations, places the cursor", "[muxserver][attach]")
{
    auto screen = RemoteScreen {};
    screen.columns = 4;
    screen.lines = 2;

    auto delta = proto::Delta {};
    delta.stableViewportBase = 0;
    delta.cursorLine = 1;
    delta.cursorColumn = 2;
    auto line = proto::WireLine {};
    line.stableId = 0;
    line.columns = 4;
    auto wide = proto::WireCell {};
    wide.codepoint = U'\U0001F600'; // an emoji: width 2
    wide.width = 2;
    auto continuation = proto::WireCell {}; // codepoint 0 behind the wide glyph
    auto narrow = proto::WireCell {};
    narrow.codepoint = U'x';
    line.cells = { wide, continuation, narrow, proto::WireCell {} };
    delta.lines.push_back(line);
    screen.apply(delta);

    auto const bytes = renderViewport(screen);

    CHECK(bytes.starts_with("\033[?25l"));            // cursor hidden while painting
    CHECK(bytes.contains("\033[1;1H\033[0m\033[2K")); // row 1 repainted in place
    // The wide glyph is followed directly by 'x': the continuation cell was
    // skipped, and the unchanged SGR was not re-emitted in between.
    CHECK(bytes.contains("\xF0\x9F\x98\x80x"));
    CHECK(bytes.ends_with("\033[0m\033[2;3H\033[?25h")); // cursor restored 1-based
}
