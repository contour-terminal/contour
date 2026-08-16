// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QColor>
#include <QtGui/QImage>

namespace contour::platform
{

/// What a popup's drop shadow looks like.
struct PopupShadowParams
{
    int blur = 0;         ///< Gaussian blur radius, in logical pixels. 0 draws a hard-edged shadow.
    int offsetY = 0;      ///< Downward displacement, so the shadow reads as lying over the window.
    int cornerRadius = 0; ///< Corner radius of the popup the shadow is cast by.
    QColor color;         ///< The shadow's colour, alpha included.

    [[nodiscard]] bool operator==(PopupShadowParams const&) const noexcept = default;
};

/// A drop shadow rendered as a nine-patch, ready for QML's BorderImage.
struct PopupShadowImage
{
    QImage image;   ///< ARGB32 premultiplied. Its centre is transparent: the popup covers it.
    int margin = 0; ///< How far the shadow reaches beyond the popup, on every side.
    int corner = 0; ///< BorderImage's border width -- the unstretched corner, in image pixels.

    [[nodiscard]] bool isEmpty() const noexcept { return image.isNull(); }
};

/// Renders @p params into one nine-patch image.
///
/// A REAL Gaussian, not an approximation assembled in QML. Stacked translucent rectangles were tried
/// and are what this exists to replace: with eight layers over a twenty-four pixel blur the steps
/// are invisible against a dark terminal background and plainly banded against a light one, and
/// closing that gap by stacking more rectangles costs an item and a set of bindings each while still
/// only approaching what one blurred image gives exactly.
///
/// The image is a nine-patch on purpose. A popup is any size, and re-rendering a blur per popup per
/// resize is not worth it when the shadow is translation-invariant along each edge: the corners are
/// drawn once and the edges stretched, which is the same reason the window's own shadow is
/// published to the compositor as eight tiles.
[[nodiscard]] PopupShadowImage renderPopupShadow(PopupShadowParams const& params);

} // namespace contour::platform
