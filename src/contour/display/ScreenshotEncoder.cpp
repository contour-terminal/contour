// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ScreenshotEncoder.hpp>

#include <vtbackend/graphics/SixelEncoder.hpp>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using vtbackend::screenshot::Capture;
using vtbackend::screenshot::CaptureResult;
using vtbackend::screenshot::Format;
using vtbackend::screenshot::Status;

namespace contour::display
{

namespace
{
    /// The image the wire formats are defined in terms of: 8 bits per channel, straight (not
    /// premultiplied) alpha, rows top to bottom. The readback hands over premultiplied pixels, so this
    /// is a real conversion and not a relabelling -- a PNG written from premultiplied data has every
    /// translucent pixel too dark.
    constexpr auto WireFormat = QImage::Format_RGBA8888;

    /// Copies @p image out row by row rather than in one block: QImage pads rows to its own alignment,
    /// so bytesPerLine() is not width*4 in general, while encodeSixel() wants them tightly packed.
    [[nodiscard]] std::vector<uint8_t> tightlyPackedRgba(QImage const& image)
    {
        auto const rowSize = static_cast<size_t>(image.width()) * 4;
        auto out = std::vector<uint8_t> {};
        out.reserve(rowSize * static_cast<size_t>(image.height()));

        for (auto y = 0; y < image.height(); ++y)
        {
            auto const* const row = image.constScanLine(y);
            out.insert(out.end(), row, row + rowSize);
        }

        return out;
    }

    /// The image's own extent, which is what the reply's Pw/Ph carry -- and not the region's extent in
    /// cells times the cell size, because the crop may have been clipped to the frame.
    [[nodiscard]] vtbackend::ImageSize extentOf(QImage const& image) noexcept
    {
        return vtbackend::ImageSize { vtbackend::Width::cast_from(image.width()),
                                      vtbackend::Height::cast_from(image.height()) };
    }

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
    // one cell further on.
    auto const bottomRight = metrics.mapTopLeft(vtbackend::LineOffset::cast_from(unbox(area.bottom) + 1),
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

    auto const image = cropped.convertToFormat(WireFormat);
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
