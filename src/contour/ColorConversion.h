// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <QtGui/QColor>

#include <cstdint>

namespace contour
{

/// Converts a vtbackend RGB color to a Qt QColor.
///
/// Single source of truth for the RGBColor -> QColor channel mapping, shared by every GUI site that
/// surfaces a model/terminal color to QML (tab accent color, the color palette, backgrounds), so an
/// alpha/gamma/channel-order change is made in exactly one place.
/// @param color The source RGB color.
/// @param alpha The alpha channel for the result (default fully opaque).
/// @return The equivalent QColor.
[[nodiscard]] inline QColor toQColor(vtbackend::RGBColor color, std::uint8_t alpha = 255) noexcept
{
    return QColor(color.red, color.green, color.blue, alpha);
}

/// Converts a Qt QColor to a vtbackend RGB color (dropping any alpha).
///
/// The inverse of toQColor for the RGB channels; used when a QML-chosen color (e.g. a tab color
/// picked from the palette) is written back into the model.
/// @param color The source QColor.
/// @return The equivalent RGBColor.
[[nodiscard]] inline vtbackend::RGBColor toRGBColor(QColor const& color) noexcept
{
    return vtbackend::RGBColor { static_cast<std::uint8_t>(color.red()),
                                 static_cast<std::uint8_t>(color.green()),
                                 static_cast<std::uint8_t>(color.blue()) };
}

} // namespace contour
