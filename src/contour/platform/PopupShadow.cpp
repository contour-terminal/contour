// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/AlphaBlur.hpp>
#include <contour/platform/PopupShadow.hpp>

#include <QtGui/QPainter>

#include <algorithm>
#include <ranges>

namespace contour::platform
{

namespace
{
    /// How far a blur of @p blur reaches, in logical pixels.
    ///
    /// Three box passes of half-width `boxRadiusFor(blur)` reach three times that. Rounded up, so
    /// the rendered region always contains the whole falloff rather than clipping its tail into a
    /// visible edge.
    [[nodiscard]] int reachOf(int blur) noexcept
    {
        return blur > 0 ? 3 * boxRadiusFor(blur) : 0;
    }
} // namespace

PopupShadowImage renderPopupShadow(PopupShadowParams const& params)
{
    auto const reach = reachOf(params.blur);
    auto const margin = reach + std::abs(params.offsetY);
    if (margin <= 0 || !params.color.isValid() || params.color.alpha() == 0)
        return {};

    // The corner the nine-patch keeps unstretched: the popup's own rounding plus the distance over
    // which a corner still bends the falloff. Beyond it the profile is one-dimensional, which is
    // exactly the condition that makes stretching the edges correct.
    auto const corner = margin + params.cornerRadius;

    // A synthetic popup just large enough that the middle of each edge has settled -- two corners
    // plus the single row and column the edges are stretched from.
    auto const extent = (2 * corner) + 1;
    auto const size = extent + (2 * margin);

    // The silhouette, displaced downward by the offset, drawn into the alpha plane the blur works on.
    auto plane = AlphaPlane { size, size };
    {
        auto mask = QImage { size, size, QImage::Format_Alpha8 };
        mask.fill(0);
        {
            auto painter = QPainter { &mask };
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::white);
            painter.drawRoundedRect(QRectF(margin, margin + params.offsetY, extent, extent),
                                    params.cornerRadius,
                                    params.cornerRadius);
        }
        for (auto const y: std::views::iota(0, size))
        {
            auto const* line = mask.constScanLine(y);
            for (auto const x: std::views::iota(0, size))
                plane.at(x, y) = line[x];
        }
    }

    blurAlphaPlane(plane, boxRadiusFor(params.blur));

    auto image = QImage { size, size, QImage::Format_ARGB32_Premultiplied };
    image.fill(Qt::transparent);

    auto const red = params.color.red();
    auto const green = params.color.green();
    auto const blue = params.color.blue();
    auto const tint = params.color.alpha();

    for (auto const y: std::views::iota(0, size))
    {
        auto* scanline = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (auto const x: std::views::iota(0, size))
        {
            auto const alpha = (plane.at(x, y) * tint) / 255;
            if (alpha == 0)
                continue;
            // Premultiplied, as the format demands.
            scanline[x] = qRgba((red * alpha) / 255, (green * alpha) / 255, (blue * alpha) / 255, alpha);
        }
    }

    // The popup itself is opaque and covers the middle, so nothing is cleared here: leaving the
    // shadow under it costs one blend of a region the popup then paints over, and clearing it would
    // put a hard edge exactly where the popup's antialiased border needs something behind it.
    return PopupShadowImage { .image = image, .margin = margin, .corner = corner };
}

} // namespace contour::platform
