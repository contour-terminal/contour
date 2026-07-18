// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISContext.h>

#include <cmath>

using crispy::point;

namespace vtbackend::regis
{

void ReGISContext::reset()
{
    // Restore every field to its in-class (VT340 power-up) default rather than re-listing them here,
    // which would silently drift as fields are added. The supersample factor and canvas buffer size
    // are physical display properties, not ReGIS state, so they survive a Pmode 1/3 reset and the
    // context stays consistent with the actual canvas.
    auto const keepSupersample = supersample;
    auto const keepCanvasSize = canvasSize;
    *this = ReGISContext {};
    supersample = keepSupersample;
    canvasSize = keepCanvasSize;
}

point ReGISContext::userToPixel(double userX, double userY) const noexcept
{
    auto const canvasWidth = unbox<double>(canvasSize.width);
    auto const canvasHeight = unbox<double>(canvasSize.height);
    auto const spanX = window.x1 - window.x0;
    auto const spanY = window.y1 - window.y0;
    auto const px = spanX != 0.0 ? ((userX - window.x0) / spanX) * (canvasWidth - 1.0) : 0.0;
    auto const py = spanY != 0.0 ? ((userY - window.y0) / spanY) * (canvasHeight - 1.0) : 0.0;
    return point { .x = static_cast<int>(std::lround(px)), .y = static_cast<int>(std::lround(py)) };
}

std::pair<double, double> ReGISContext::pixelToUser(point p) const noexcept
{
    auto const canvasWidth = unbox<double>(canvasSize.width);
    auto const canvasHeight = unbox<double>(canvasSize.height);
    auto const spanX = window.x1 - window.x0;
    auto const spanY = window.y1 - window.y0;
    auto const userX = canvasWidth > 1.0
                           ? window.x0 + ((static_cast<double>(p.x) / (canvasWidth - 1.0)) * spanX)
                           : window.x0;
    auto const userY = canvasHeight > 1.0
                           ? window.y0 + ((static_cast<double>(p.y) / (canvasHeight - 1.0)) * spanY)
                           : window.y0;
    return { userX, userY };
}

point ReGISContext::userDeltaToPixel(double dx, double dy) const noexcept
{
    auto const canvasWidth = unbox<double>(canvasSize.width);
    auto const canvasHeight = unbox<double>(canvasSize.height);
    auto const spanX = window.x1 - window.x0;
    auto const spanY = window.y1 - window.y0;
    auto const px = spanX != 0.0 ? (dx / spanX) * (canvasWidth - 1.0) : 0.0;
    auto const py = spanY != 0.0 ? (dy / spanY) * (canvasHeight - 1.0) : 0.0;
    return point { .x = static_cast<int>(std::lround(px)), .y = static_cast<int>(std::lround(py)) };
}

Pen ReGISContext::currentPen() const noexcept
{
    // Line width and pattern multiplier are specified in logical pixels but drawn on the supersampled
    // canvas, so scale them by the supersample factor to preserve the on-screen appearance.
    return Pen {
        .color = registers.at(foregroundRegister),
        .mode = writingMode,
        .pattern = static_cast<uint8_t>(negativePattern ? ~pattern : pattern),
        .patternMultiplier = patternMultiplier * supersample,
        .lineWidth = lineWidth * static_cast<int>(supersample),
        .shade = shadingEnabled,
        .shadeVertical = shadingVertical,
        .shadeReference = shadingReference,
    };
}

RGBAColor ReGISContext::backgroundColor() const noexcept
{
    if (!backgroundOpaque)
        return RGBAColor { 0 };
    return RGBAColor { registers.at(backgroundRegister), 0xFF };
}

} // namespace vtbackend::regis
