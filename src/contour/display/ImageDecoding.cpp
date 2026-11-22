// SPDX-License-Identifier: Apache-2.0
#include <contour/display/ImageDecoding.hpp>

#include <vtbackend/graphics/PngDecoding.hpp>

#include <QtGui/QImage>

#include <cstring>
#include <ranges>

namespace contour::display
{

std::optional<vtbackend::Image::Data> decodeImage(vtbackend::ImageFormat format,
                                                  std::span<uint8_t const> data,
                                                  vtbackend::ImageSize& size)
{
    if (format != vtbackend::ImageFormat::PNG)
        return std::nullopt;

    auto image = QImage {};
    // Recognised, not named: iTerm2 inline images and GIP hand any image file over as ImageFormat::PNG.
    image.loadFromData(static_cast<uchar const*>(data.data()), static_cast<int>(data.size()));
    if (image.isNull())
        return std::nullopt;

    image = image.convertToFormat(QImage::Format_RGBA8888);

    size = vtbackend::ImageSize { vtbackend::Width::cast_from(image.width()),
                                  vtbackend::Height::cast_from(image.height()) };

    auto const rowBytes = static_cast<size_t>(image.width()) * 4;
    auto pixels = vtbackend::Image::Data(static_cast<size_t>(image.height()) * rowBytes);
    auto* p = pixels.data();
    for (auto const row: std::views::iota(0, image.height()))
    {
        std::memcpy(p, image.constScanLine(row), rowBytes);
        p += rowBytes;
    }
    return pixels;
}

vtbackend::ImageDecoder makeImageDecoder()
{
    return vtbackend::PngDecoder { vtbackend::PngDecodingLimits {}, decodeImage };
}

} // namespace contour::display
