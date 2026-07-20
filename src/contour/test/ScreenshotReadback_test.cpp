// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure, GPU-free screenshot-readback helpers (ScreenshotReadback.h): the buffer-size
// arithmetic and the normalization of a raw RHI readback buffer into a deterministic, correctly-sized,
// correctly-oriented RGBA8 image buffer (including the row reversal a Y-up framebuffer capture needs).
//
// These govern how RhiRenderer's deferred offscreen screenshot readback is turned into a QImage without a
// GPU/RHI context, so they are pinned here. Which of the two orientations a real capture actually needs is
// a property of the live RHI backend, so it is pinned end-to-end in ScreenshotRhiReadback_test instead.

#include <contour/display/ScreenshotReadback.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace contour::display;

// {{{ screenshotBufferSize
TEST_CASE("screenshotBufferSize: RGBA8 byte count is width*height*4", "[screenshot]")
{
    CHECK(screenshotBufferSize(1, 1) == 4);
    CHECK(screenshotBufferSize(10, 20) == 800);
    CHECK(screenshotBufferSize(1920, 1080) == static_cast<std::size_t>(1920u * 1080u * 4u));
    CHECK(ScreenshotBytesPerPixel == 4);
}

TEST_CASE("screenshotBufferSize: non-positive dimensions yield zero", "[screenshot]")
{
    CHECK(screenshotBufferSize(0, 100) == 0);
    CHECK(screenshotBufferSize(100, 0) == 0);
    CHECK(screenshotBufferSize(-1, 10) == 0);
    CHECK(screenshotBufferSize(10, -1) == 0);
}
// }}}

// {{{ normalizeScreenshotBuffer
namespace
{
// Builds a width*height*4 RGBA buffer whose every pixel's R channel encodes its row index (so a flip is
// observable) and G channel its column index.
std::vector<uint8_t> makeRowTaggedBuffer(int width, int height)
{
    std::vector<uint8_t> buf(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            auto const i =
                ((static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)) * 4;
            buf[i + 0] = static_cast<uint8_t>(y);
            buf[i + 1] = static_cast<uint8_t>(x);
            buf[i + 2] = 0;
            buf[i + 3] = 255;
        }
    return buf;
}
} // namespace

TEST_CASE("normalizeScreenshotBuffer: exact-size input copied through unflipped", "[screenshot]")
{
    constexpr int W = 4;
    constexpr int H = 3;
    auto const src = makeRowTaggedBuffer(W, H);
    auto const out = normalizeScreenshotBuffer(src, W, H, /*flip*/ false);

    REQUIRE(out.size() == screenshotBufferSize(W, H));
    CHECK(out == src); // identical when not flipping
}

TEST_CASE("normalizeScreenshotBuffer: flip reverses row order", "[screenshot]")
{
    constexpr int W = 4;
    constexpr int H = 3;
    auto const src = makeRowTaggedBuffer(W, H);
    auto const out = normalizeScreenshotBuffer(src, W, H, /*flip*/ true);

    REQUIRE(out.size() == screenshotBufferSize(W, H));
    auto const rowBytes = static_cast<size_t>(W) * 4;
    // Row 0 of the output must equal the last source row (R channel == H-1), and vice versa.
    CHECK(out[0] == static_cast<uint8_t>(H - 1));             // out row 0 <- src row H-1
    CHECK(out[(static_cast<size_t>(H - 1)) * rowBytes] == 0); // out row H-1 <- src row 0
    // Column tagging (G channel) is preserved within a row (flip is vertical only).
    CHECK(out[1] == 0);     // out(0,0).G == src(H-1,0).G == 0
    CHECK(out[1 + 4] == 1); // out(0,1).G == 1
}

TEST_CASE("normalizeScreenshotBuffer: shorter input zero-pads the missing rows", "[screenshot]")
{
    constexpr int W = 4;
    constexpr int H = 4;
    // Provide only the first 2 rows worth of data.
    auto full = makeRowTaggedBuffer(W, H);
    auto const rowBytes = static_cast<size_t>(W) * 4;
    full.resize(rowBytes * 2);

    auto const out = normalizeScreenshotBuffer(full, W, H, /*flip*/ false);
    REQUIRE(out.size() == screenshotBufferSize(W, H));
    // The two supplied rows are present; the remaining two are zero-filled.
    CHECK(out[0] == 0);                // row 0 present
    CHECK(out[rowBytes] == 1);         // row 1 present
    CHECK(out[rowBytes * 2] == 0);     // row 2 zero-filled
    CHECK(out[rowBytes * 3] == 0);     // row 3 zero-filled
    CHECK(out[rowBytes * 3 + 3] == 0); // and its alpha is zero (defensive pad, not opaque)
}

TEST_CASE("normalizeScreenshotBuffer: longer input is truncated to the expected rows", "[screenshot]")
{
    constexpr int W = 2;
    constexpr int H = 2;
    // Over-supply (as a padded-row backend might): 3 rows of data for a 2-row image.
    auto over = makeRowTaggedBuffer(W, H + 1);
    auto const out = normalizeScreenshotBuffer(over, W, H, /*flip*/ false);
    REQUIRE(out.size() == screenshotBufferSize(W, H)); // exactly 2 rows, never reads the 3rd
}

TEST_CASE("normalizeScreenshotBuffer: empty input yields a zero buffer of the expected size", "[screenshot]")
{
    constexpr int W = 5;
    constexpr int H = 5;
    auto const out = normalizeScreenshotBuffer({}, W, H, /*flip*/ false);
    REQUIRE(out.size() == screenshotBufferSize(W, H));
    CHECK(std::ranges::all_of(out, [](uint8_t b) { return b == 0; }));
}

TEST_CASE("normalizeScreenshotBuffer: zero-size target yields an empty buffer", "[screenshot]")
{
    auto const src = makeRowTaggedBuffer(4, 4);
    CHECK(normalizeScreenshotBuffer(src, 0, 10, false).empty());
    CHECK(normalizeScreenshotBuffer(src, 10, 0, false).empty());
}
// }}}
