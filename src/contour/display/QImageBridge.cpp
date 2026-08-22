// SPDX-License-Identifier: Apache-2.0
#include <contour/display/QImageBridge.hpp>

#include <cstddef>
#include <ranges>

namespace contour::display
{

namespace
{
    constexpr auto WireFormat = QImage::Format_RGBA8888;
}

QImage toWireFormat(QImage const& image)
{
    return image.convertToFormat(WireFormat);
}

vtbackend::ImageSize extentOf(QImage const& image) noexcept
{
    return vtbackend::ImageSize { vtbackend::Width::cast_from(image.width()),
                                  vtbackend::Height::cast_from(image.height()) };
}

bool isTightlyPacked(QImage const& image) noexcept
{
    return image.bytesPerLine() == static_cast<qsizetype>(image.width()) * 4;
}

std::vector<uint8_t> tightlyPackedRgba(QImage const& image)
{
    auto const rowSize = static_cast<size_t>(image.width()) * 4;
    auto out = std::vector<uint8_t> {};
    out.reserve(rowSize * static_cast<size_t>(image.height()));

    for (auto const row: std::views::iota(0, image.height()))
    {
        auto const* const scanLine = image.constScanLine(row);
        out.insert(out.end(), scanLine, scanLine + rowSize);
    }

    return out;
}

} // namespace contour::display
