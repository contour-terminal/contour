// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pixel half of the OSC 533 screenshot extension: mapping a cell region onto the
// pixels it occupies in a rendered frame, and encoding those pixels as PNG or raw RGBA.
//
// No renderer, no window and no GPU are involved -- the frame is a QImage a test paints itself, which
// is exactly what makes the crop arithmetic checkable at all. Getting it wrong shifts every pixel
// screenshot by a margin without failing anything visible.

#include <contour/display/ScreenshotEncoder.hpp>

#include <QtGui/QColor>
#include <QtGui/QImage>

#include <catch2/catch_test_macros.hpp>

#include <ranges>

using contour::display::encodeScreenshot;
using contour::display::pixelRectOf;

using vtbackend::Bottom;
using vtbackend::ColumnCount;
using vtbackend::Height;
using vtbackend::Left;
using vtbackend::LineCount;
using vtbackend::Rect;
using vtbackend::Right;
using vtbackend::Top;
using vtbackend::Width;

using vtbackend::screenshot::Format;
using vtbackend::screenshot::Status;

namespace
{

/// A grid of 8x16 cells with a 5px left and 3px top margin, deliberately asymmetric so a test that
/// confuses the two axes fails.
[[nodiscard]] vtrasterizer::GridMetrics testMetrics()
{
    auto metrics = vtrasterizer::GridMetrics {};
    metrics.pageSize = vtbackend::PageSize { .lines = LineCount(4), .columns = ColumnCount(6) };
    metrics.cellSize = vtbackend::ImageSize { Width(8), Height(16) };
    metrics.pageMargin = vtrasterizer::PageMargin { .left = 5, .top = 3, .bottom = 0 };
    return metrics;
}

/// A frame large enough to hold @ref testMetrics' whole page, painted so every cell is a different
/// color: cell (line, column) is rgb(line * 40, column * 40, 0).
[[nodiscard]] QImage testFrame()
{
    auto const metrics = testMetrics();
    auto frame = QImage { 5 + (6 * 8), 3 + (4 * 16), QImage::Format_RGBA8888 };
    frame.fill(QColor(0, 0, 255));

    for (auto const line: std::views::iota(0, 4))
        for (auto const column: std::views::iota(0, 6))
        {
            auto const at = metrics.mapTopLeft(vtbackend::LineOffset(line), vtbackend::ColumnOffset(column));
            for (auto const y: std::views::iota(0, 16))
                for (auto const x: std::views::iota(0, 8))
                    frame.setPixelColor(at.x + x, at.y + y, QColor(line * 40, column * 40, 0));
        }

    return frame;
}

/// @return The four bytes of the pixel at (@p x, @p y) in a tightly-packed RGBA8 run @p rowWidth wide.
[[nodiscard]] QColor pixelAt(std::string const& rgba, int rowWidth, int x, int y)
{
    auto const at = ((static_cast<size_t>(y) * static_cast<size_t>(rowWidth)) + static_cast<size_t>(x)) * 4;
    REQUIRE(at + 4 <= rgba.size());
    return { static_cast<uint8_t>(rgba[at]),
             static_cast<uint8_t>(rgba[at + 1]),
             static_cast<uint8_t>(rgba[at + 2]),
             static_cast<uint8_t>(rgba[at + 3]) };
}

} // namespace

TEST_CASE("ScreenshotEncoder.pixelRectOf maps a cell region through the page margins", "[screenshot]")
{
    auto const metrics = testMetrics();

    SECTION("a single cell is one cell wide and one cell tall")
    {
        auto const rect = pixelRectOf(
            Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(0), .right = Right(0) }, metrics);
        CHECK(rect.x() == 5);
        CHECK(rect.y() == 3);
        CHECK(rect.width() == 8);
        CHECK(rect.height() == 16);
    }

    SECTION("both corners are inclusive, so the bottom-right cell is inside the rectangle")
    {
        auto const rect = pixelRectOf(
            Rect { .top = Top(1), .left = Left(2), .bottom = Bottom(2), .right = Right(4) }, metrics);
        CHECK(rect.x() == 5 + (2 * 8));
        CHECK(rect.y() == 3 + (1 * 16));
        // Columns 2..4 are three cells, lines 1..2 are two.
        CHECK(rect.width() == 3 * 8);
        CHECK(rect.height() == 2 * 16);
    }
}

TEST_CASE("ScreenshotEncoder.rgba is tightly packed and carries its own extent", "[screenshot]")
{
    // Lines 1..2, columns 2..3: a 16x32 rectangle out of the middle of the page.
    auto const area = Rect { .top = Top(1), .left = Left(2), .bottom = Bottom(2), .right = Right(3) };
    auto const capture = encodeScreenshot(testFrame(), area, testMetrics(), Format::Rgba);

    REQUIRE(capture.has_value());
    CHECK(unbox(capture->pixelSize.width) == 16);
    CHECK(unbox(capture->pixelSize.height) == 32);
    // Raw pixels say nothing about their own shape, so the reply's Pw/Ph must -- and the run must be
    // exactly width*height*4 bytes for those to describe it.
    CHECK(capture->content.size() == size_t { 16 } * 32 * 4);

    // The top-left pixel is cell (1,2)'s color, and the bottom-right one is cell (2,3)'s.
    CHECK(pixelAt(capture->content, 16, 0, 0) == QColor(40, 80, 0));
    CHECK(pixelAt(capture->content, 16, 15, 31) == QColor(80, 120, 0));
}

TEST_CASE("ScreenshotEncoder.png round-trips through Qt's decoder", "[screenshot]")
{
    auto const area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(1), .right = Right(2) };
    auto const capture = encodeScreenshot(testFrame(), area, testMetrics(), Format::Png);

    REQUIRE(capture.has_value());
    CHECK(unbox(capture->pixelSize.width) == 24);
    CHECK(unbox(capture->pixelSize.height) == 32);
    CHECK(capture->content.starts_with("\x89PNG\r\n\x1a\n"));

    auto decoded = QImage {};
    REQUIRE(decoded.loadFromData(reinterpret_cast<uchar const*>(capture->content.data()),
                                 static_cast<int>(capture->content.size()),
                                 "PNG"));
    CHECK(decoded.width() == 24);
    CHECK(decoded.height() == 32);
    CHECK(decoded.pixelColor(0, 0) == QColor(0, 0, 0));
    CHECK(decoded.pixelColor(23, 31) == QColor(40, 80, 0));
}

TEST_CASE("ScreenshotEncoder.a region reaching past the frame is clipped to it", "[screenshot]")
{
    // A window whose height is not a whole number of cells leaves the last row partly outside what was
    // rendered; a frame captured mid-resize may be smaller still. Either way the crop must stay inside
    // the pixels that exist rather than read past them.
    auto const frame = testFrame().copy(QRect { 0, 0, 5 + (6 * 8), 3 + (3 * 16) + 4 });
    auto const area = Rect { .top = Top(3), .left = Left(0), .bottom = Bottom(3), .right = Right(5) };
    auto const capture = encodeScreenshot(frame, area, testMetrics(), Format::Rgba);

    REQUIRE(capture.has_value());
    CHECK(unbox(capture->pixelSize.width) == 6 * 8);
    // Only the four rows of pixels that were actually rendered, not the sixteen the cell asks for.
    CHECK(unbox(capture->pixelSize.height) == 4);
    CHECK(capture->content.size() == size_t { 48 } * 4 * 4);
}

TEST_CASE("ScreenshotEncoder.a region wholly outside the frame is unavailable", "[screenshot]")
{
    auto const frame = QImage { 8, 16, QImage::Format_RGBA8888 };
    auto const area = Rect { .top = Top(3), .left = Left(4), .bottom = Bottom(3), .right = Right(5) };
    auto const capture = encodeScreenshot(frame, area, testMetrics(), Format::Rgba);

    REQUIRE(!capture.has_value());
    CHECK(capture.error() == Status::Unavailable);
}

TEST_CASE("ScreenshotEncoder.a grid format never reaches the renderer", "[screenshot]")
{
    // Terminal::answerScreenshot() serves the grid formats off the cells and hands only the renderer
    // formats here. Asking anyway is refused rather than answered with something plausible.
    auto const area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(0), .right = Right(0) };
    for (auto const format: { Format::PlainText, Format::VTSequences, Format::Sixel })
    {
        auto const capture = encodeScreenshot(testFrame(), area, testMetrics(), format);
        REQUIRE(!capture.has_value());
        CHECK(capture.error() == Status::UnsupportedFormat);
    }
}
