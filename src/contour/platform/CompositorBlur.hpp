// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a window asks of a compositor's blur behind it (KWin's, on Wayland and X11), and the region that adds
/// up to: the decision setBlurBehind() and setTitleBandBlur() share, apart from the protocols that carry it.

#include <contour/platform/SystemBackdrop.hpp>

#include <cstdint>

namespace contour::platform
{

/// What one window asks of the compositor's blur: `background.blur`'s wish, behind the whole window, and the
/// title band's (setTitleBandBlur()), behind its top alone. One blur object per surface carries both, so each
/// request recomputes the region from the two rather than overwriting the other's.
struct CompositorBlurWishes
{
    BlurBehindWish window = BlurBehindWish::Off; ///< The profile's wish, for the whole window.
    double bandHeight = 0.0;                     ///< The title band's height; 0 asks for none.

    bool operator==(CompositorBlurWishes const&) const = default;
};

/// The region a window's wishes add up to.
enum class CompositorBlurRegion : uint8_t
{
    None = 0,    ///< Nothing is blurred.
    WholeWindow, ///< The whole surface: the profile asked for it, which covers the band too.
    Band,        ///< The top @c bandHeight logical pixels.
};

/// @param wishes A window's wishes.
/// @return The region they add up to.
[[nodiscard]] constexpr CompositorBlurRegion regionOf(CompositorBlurWishes const& wishes) noexcept
{
    if (wishes.window == BlurBehindWish::On)
        return CompositorBlurRegion::WholeWindow;
    return wishes.bandHeight > 0.0 ? CompositorBlurRegion::Band : CompositorBlurRegion::None;
}

/// How wide a band region is: wider than any window, so it follows a resize without being sent again -- the
/// compositor clips a blur region to its surface.
inline constexpr int BandBlurWidth = 32767;

} // namespace contour::platform
