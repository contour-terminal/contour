// SPDX-License-Identifier: Apache-2.0
#include <contour/display/QImageBridge.hpp>
#include <contour/display/ScreenshotEncoder.hpp>

#include <vtbackend/graphics/SixelEncoder.hpp>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>

#include <cstddef>
#include <string>

using vtbackend::screenshot::Capture;
using vtbackend::screenshot::CaptureResult;
using vtbackend::screenshot::Format;
using vtbackend::screenshot::Status;

namespace contour::display
{

namespace
{
    [[nodiscard]] CaptureResult encodePng(QImage const& image)
    {
        auto bytes = QByteArray {};
        auto buffer = QBuffer { &bytes };
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return std::unexpected { Status::Unavailable };

        return Capture { .content = std::string { bytes.constData(), static_cast<size_t>(bytes.size()) },
                         .pixelSize = extentOf(image) };
    }
} // namespace

QRect pixelRectOf(vtbackend::Rect area, vtrasterizer::GridMetrics const& metrics) noexcept
{
    auto const topLeft = metrics.mapTopLeft(vtbackend::LineOffset::cast_from(area.top),
                                            vtbackend::ColumnOffset::cast_from(area.left));

    // Both corners are inclusive, so the bottom-right cell is part of the region: the exclusive edge is
    // one cell further on, which is what mapBottomLeft() names for the line.
    auto const bottomRight = metrics.mapBottomLeft(vtbackend::LineOffset::cast_from(area.bottom),
                                                   vtbackend::ColumnOffset::cast_from(unbox(area.right) + 1));

    return QRect { QPoint { topLeft.x, topLeft.y }, QPoint { bottomRight.x - 1, bottomRight.y - 1 } };
}

CaptureResult encodeScreenshot(QImage const& frame,
                               vtbackend::Rect area,
                               vtrasterizer::GridMetrics const& metrics,
                               Format format)
{
    // The region is already clamped to the PAGE, but the page is not the frame: a window whose height
    // is not a whole number of cells leaves the last row partly outside what was rendered, and a frame
    // captured mid-resize may be smaller still. Intersecting keeps the crop inside the pixels that
    // exist instead of reading past them.
    auto const crop = pixelRectOf(area, metrics).intersected(frame.rect());

    // Tested before cropping rather than after: QImage::copy() reads a NULL rectangle as "the whole
    // image", so a region that misses the frame entirely would otherwise come back as a screenshot of
    // everything -- the one wrong answer worse than no answer at all.
    if (crop.isEmpty())
        return std::unexpected { Status::Unavailable };

    auto const cropped = frame.copy(crop);
    if (cropped.isNull())
        return std::unexpected { Status::Unavailable };

    auto const image = toWireFormat(cropped);
    if (image.isNull())
        return std::unexpected { Status::Unavailable };

    auto const extent = extentOf(image);

    switch (format)
    {
        case Format::Png: return encodePng(image);
        case Format::Sixel:
            return Capture { .content = vtbackend::encodeSixel(tightlyPackedRgba(image), extent),
                             .pixelSize = extent };
        case Format::PlainText:
        case Format::VTSequences:
            // Grid formats never reach a renderer: Terminal::answerScreenshot() serves those off the
            // cells and only hands the renderer formats on.
        case Format::Rgba:
            // Reserved, and refused by screenshot::parseRequest() long before this.
            break;
    }

    return std::unexpected { Status::UnsupportedFormat };
}

} // namespace contour::display
