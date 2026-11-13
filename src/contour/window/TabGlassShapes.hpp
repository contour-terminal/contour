// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The tabs the operating system draws as glass (config::TabSurface::Glass), as the tab strip publishes them
/// from QML: read into the plain shapes a platform material lays out.

#include <contour/platform/TitleBandMaterial.hpp>

#include <QtCore/QVariant>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtGui/QColor>

#include <vector>

namespace contour::window
{

/// Reads the tab strip's glass shapes.
///
/// Each entry is a map of `x`, `y`, `width`, `height` and `radius` (window-relative logical pixels) and
/// `tint` (a colour; transparent tints nothing). An entry that is not a map, or has no positive size, is
/// skipped: it would draw a shape of nothing.
/// @param shapes What the strip published.
/// @return The shapes, in the strip's order.
[[nodiscard]] inline std::vector<platform::TabGlassShape> tabGlassShapesOf(QVariantList const& shapes)
{
    auto result = std::vector<platform::TabGlassShape> {};
    result.reserve(static_cast<size_t>(shapes.size()));
    for (auto const& entry: shapes)
    {
        if (entry.typeId() != QMetaType::QVariantMap)
            continue;
        auto const map = entry.toMap();
        auto const shape = platform::TabGlassShape {
            .x = map.value(QStringLiteral("x")).toDouble(),
            .y = map.value(QStringLiteral("y")).toDouble(),
            .width = map.value(QStringLiteral("width")).toDouble(),
            .height = map.value(QStringLiteral("height")).toDouble(),
            .radius = map.value(QStringLiteral("radius")).toDouble(),
            .tint = map.value(QStringLiteral("tint")).value<QColor>().rgba(),
        };
        if (shape.width <= 0.0 || shape.height <= 0.0)
            continue;
        // A tint that is wholly transparent is no tint at all.
        result.push_back(qAlpha(shape.tint) == 0 ? platform::TabGlassShape { .x = shape.x,
                                                                             .y = shape.y,
                                                                             .width = shape.width,
                                                                             .height = shape.height,
                                                                             .radius = shape.radius,
                                                                             .tint = 0 }
                                                 : shape);
    }
    return result;
}

} // namespace contour::window
