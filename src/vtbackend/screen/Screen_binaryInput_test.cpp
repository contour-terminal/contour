// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <random>
#include <ranges>
#include <string>
#include <string_view>

using namespace vtbackend;
using namespace std::string_view_literals;

// #1062: catting a binary file must neither crash nor lock the terminal up. The input is not text, so it
// is a mixture of invalid UTF-8, C0 controls, and C1 controls -- a lone 0x90 is DCS, and a DCS selecting
// ReGIS ('p') draws an image whatever follows.

namespace
{
constexpr auto PageLines = LineCount(24);
constexpr auto PageColumns = ColumnCount(80);

/// A terminal that has not been told its cell size in pixels, as one is before its display reports it.
[[nodiscard]] MockTerm<> makeTerminal()
{
    return MockTerm { PageSize { PageLines, PageColumns }, LineCount(100) };
}

/// Whether any cell of @p mc's page holds a piece of an image.
[[nodiscard]] bool pageHoldsAnImage(MockTerm<> const& mc)
{
    for (auto const line: std::views::iota(0, unbox(PageLines)))
        for (auto const column: std::views::iota(0, unbox(PageColumns)))
            if (mc.terminal.primaryScreen().at(LineOffset(line), ColumnOffset(column)).imageFragment())
                return true;
    return false;
}

/// Resets @p mc and prints a marker, so that a terminal left in some state by earlier input shows it is
/// still alive and parsing. The marker's first letter may be eaten by a UTF-8 sequence that the noise left
/// unfinished, so a test looks for the rest of it.
[[nodiscard]] std::string firstLineAfterReset(MockTerm<>& mc)
{
    mc.writeToScreen("\033\\\033c"sv); // leave any string in progress, then RIS
    mc.writeToScreen("alive"sv);
    return mc.terminal.primaryScreen().grid().lineText(LineOffset(0));
}

[[nodiscard]] bool cursorIsOnThePage(MockTerm<> const& mc)
{
    auto const position = mc.terminal.primaryScreen().cursor().position;
    return position.line >= LineOffset(0) && position.line < boxed_cast<LineOffset>(PageLines)
           && position.column >= ColumnOffset(0) && position.column < boxed_cast<ColumnOffset>(PageColumns);
}
} // namespace

TEST_CASE("Screen.binaryInput.regisImageWithoutCellPixelSize", "[screen][regression]")
{
    // The smallest byte sequence of the reporter's crash.txt that reproduced it: DCS (C1), 'p' selecting
    // ReGIS, two bytes of ReGIS, and an ESC that ends the string and commits the image. The image was laid
    // out against a cell height of zero: a division by zero, and an infinity cast to a line count. Only the
    // sanitizers (or a CPU that traps on it, as x86 does) see that; the test keeps the sequence honest.
    auto mc = makeTerminal();
    REQUIRE(unbox(mc.terminal.cellPixelSize().height) == 0);

    mc.writeToScreen("\x90pv6\x1b"sv);

    // A ReGIS image is a full-page plane, so it covers the page whatever the cell size is, and leaves the
    // cursor where it was. (The cursor offset that divided by zero is computed but not used for it, so on
    // arm64 nothing but the sanitizers can tell; this pins the outcome.)
    CHECK(pageHoldsAnImage(mc));
    CHECK(mc.terminal.primaryScreen().cursor().position == CellLocation {});
    CHECK(firstLineAfterReset(mc).contains("live"));
}

TEST_CASE("Screen.binaryInput.sixelWithoutCellPixelSize", "[screen][regression]")
{
    // The same layout serves Sixel, which divides by the cell size before it ever reaches renderImage().
    auto mc = makeTerminal();
    mc.writeToScreen("\033Pq#0;2;0;0;0#0!8~-\033\\"sv);

    // Dropped: no cell can hold a piece of it, and the cursor has not moved.
    CHECK_FALSE(pageHoldsAnImage(mc));
    CHECK(mc.terminal.primaryScreen().cursor().position == CellLocation {});
    CHECK(firstLineAfterReset(mc).contains("live"));
}

TEST_CASE("Screen.binaryInput.contourImageWithoutCellPixelSize", "[screen][regression]")
{
    // A Contour-protocol image that is sized in cells divides by the cell size, too. With no cell size it is
    // dropped; one that brings its own grid size does not need it, and is drawn.
    constexpr auto Pixels = "AAAAAAAAAAAAAAAAAAAAAA=="sv;
    static_assert(Pixels.size() % 4 == 0);

    auto mc = makeTerminal();
    mc.writeToScreen(std::format("\033P!go=s,f=3,w=2,h=2;!{}\033\\", Pixels));
    CHECK_FALSE(pageHoldsAnImage(mc));
    CHECK(mc.terminal.primaryScreen().cursor().position == CellLocation {});
    CHECK(firstLineAfterReset(mc).contains("live"));

    auto sized = makeTerminal();
    sized.writeToScreen(std::format("\033P!go=s,f=3,w=2,h=2,c=4,r=2;!{}\033\\", Pixels));
    CHECK(pageHoldsAnImage(sized));
}

TEST_CASE("Screen.binaryInput.randomBytesNeitherCrashNorHang", "[screen][regression]")
{
    // Deterministic noise, so a failure is reproducible: invalid UTF-8 lead and continuation bytes, C0 and
    // C1 controls, ESC, and printable ASCII, fed in chunks of every awkward size.
    constexpr auto Seeds = std::array<std::uint32_t, 4> { 1062, 0x90, 0xC0FFEE, 0xDEADBEEF };
    for (auto const seed: Seeds)
    {
        INFO("seed " << seed);
        // std::mt19937's output is fully specified, unlike the distributions', so every standard library
        // sees the same bytes.
        auto generator = std::mt19937 { seed };
        auto const next = [&](std::uint32_t bound) {
            return static_cast<std::uint32_t>(generator() % bound);
        };

        auto mc = makeTerminal();
        for ([[maybe_unused]] auto const round: std::views::iota(0, 60))
        {
            auto chunk = std::string(1 + next(600), '\0');
            for (auto& byte: chunk)
                switch (next(10))
                {
                    case 0:
                    case 1: byte = static_cast<char>(next(0x20)); break; // C0
                    case 2:
                    case 3: byte = static_cast<char>(0x80 + next(0x20)); break; // C1
                    case 4: byte = static_cast<char>(0xC0 + next(0x40)); break; // lead bytes
                    case 5: byte = static_cast<char>(0x20 + next(0x5F)); break; // printable
                    default: byte = static_cast<char>(next(256)); break;
                }
            mc.writeToScreen(chunk);
            REQUIRE(cursorIsOnThePage(mc));
        }
        CHECK(firstLineAfterReset(mc).contains("live"));
    }
}
